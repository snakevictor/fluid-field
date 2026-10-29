#ifndef FLUIDFIELD_WATERCOMMON_HLSLI
#define FLUIDFIELD_WATERCOMMON_HLSLI

// Fluid Field shallow water: shared constants, map sampling and staggered-grid helpers.
// State lives on the wind's toroidal grid (Common.hlsli):
//   Depth    R32F:  water depth above the bed at the cell centre
//   Velocity RG32F: staggered; R is u on the cell's west face, G is v on its south face
//   CellMap  RG32F: per-cell bed height and vanilla depth, refreshed every step from WaterMap
// WaterMap: coarse, world-aligned, toroidal, built on the CPU at MapSpacing: R bed height, G vanilla surface height
// (NO_WATER where none), B river flow scale. ObstacleMap: finer bed around structures at ObstacleSpacing: R bed height,
// GB the texel's own lattice coordinates (to reject stale wrapped texels), A kind (0 none, 1 landscape, 2 mesh).

#include "FluidField/Common.hlsli"

cbuffer WaterCB : register(b1)
{
	float Gravity;            // World units per second squared
	float MaxDepth;           // Transport depth cap: keeps waves shallow and the step stable
	float MaxSpeed;           // Face velocity clamp, world units per second
	float WetThreshold;       // Depth below which a cell counts as dry
	float2 Current;           // Background current, world units per second
	float CurrentRelax;       // Per-second rate at which velocity relaxes toward the current
	float LevelRelax;         // Per-second rate at which depth relaxes toward the vanilla depth
	uint MapRes;              // WaterMap texels per side, a power of two
	float MapSpacing;         // World units between WaterMap texels
	float EdgeWidth;          // Texels over which the grid edge blends back to vanilla
	float EdgeRelax;          // Per-step blend toward vanilla at the very edge
	uint ObstacleRes;         // ObstacleMap texels per side, a power of two (0: no detail)
	float ObstacleSpacing;    // World units between ObstacleMap texels
	float RiverSpeed;         // World units per second at full flowmap strength
	uint FlowmapEnabled;      // RiverFlowmap is bound and replaces Current
	float2 FlowmapCells;      // Flowmap size in cells
	int2 FlowmapOffset;       // Added to a cell's coordinates to get its flowmap tile
	float Vorticity;          // Vorticity confinement strength: keeps eddies behind obstacles alive
	float SimTime;            // Seconds simulated since the last reset
	float FoamStrength;       // Scales how much foam the foam pass makes (0: none)
	float FoamMax;            // Most foam a cell holds
	int2 SeedGridMin;         // World cell of the foam-seeding level's texel (0, 0)
	uint SeedRes;             // Its texels per side (0: no seed)
	float SeedCellSize;       // Its world units per texel
	float2 PlungeOrigin;      // D44: world position of the plunge map's texel (0, 0)
	float PlungeInverseCell;  // Its texels per world unit
	uint PlungeSize;          // Its texels per side (0: none)
	uint LipMapOn;            // D170: LipMap is bound, on the plunge map's lattice
	float3 LipPad;
};

static const float NO_WATER = -1.0e8;

Texture2D<float4> WaterMap : register(t4);
Texture2D<float4> ObstacleMap : register(t5);
// Unified Water's world flowmap: 64 texels per cell, southern tile row last. RG * 2 - 1 is the flow in world axes
// (east, north), its length the strength; A an intensity mask; B unused.
Texture2D<float4> RiverFlowmap : register(t6);
// D170: the falls' lips (FallSites::BuildLipMap) on the plunge lattice: xy draw toward a lip (world units/s), z strength (0 to 1)
Texture2D<float4> LipMap : register(t10);

/** @brief D170: the lips' draw on the water at a world position, bilinear; 0 without a map. */
float4 LipAt(float2 worldXY)
{
	if (LipMapOn == 0 || PlungeSize == 0)
		return 0.0;
	const float2 texel = (worldXY - PlungeOrigin) * PlungeInverseCell;
	const int2 c = int2(floor(texel));
	if (any(c < 0) || any(c + 1 >= int(PlungeSize)))
		return 0.0;
	const float2 t = texel - float2(c);
	return lerp(lerp(LipMap.Load(int3(c, 0)), LipMap.Load(int3(c + int2(1, 0), 0)), t.x), lerp(LipMap.Load(int3(c + int2(0, 1), 0)), LipMap.Load(int3(c + int2(1, 1), 0)), t.x), t.y);
}

static const float FLOWMAP_TEXELS_PER_CELL = 64.0;
static const float WORLD_UNITS_PER_CELL = 4096.0;

float2 LoadRiverFlow(int2 texel)
{
	int2 size = int2(FlowmapCells * FLOWMAP_TEXELS_PER_CELL);
	float4 c = RiverFlowmap[uint2(clamp(texel, 0, size - 1))];
	return (c.xy * 2.0 - 1.0) * c.w;
}

/** @brief The river's flow at a world position, world units per second, bilinear between flowmap texels. */
float2 SampleRiverFlow(float2 worldPos)
{
	float2 cells = worldPos / WORLD_UNITS_PER_CELL + float2(FlowmapOffset);
	// Columns run east from the west edge; rows run south from the north edge
	float2 texel = float2(cells.x, FlowmapCells.y - cells.y) * FLOWMAP_TEXELS_PER_CELL - 0.5;
	int2 t0 = int2(floor(texel));
	float2 f = texel - float2(t0);
	float2 flow = lerp(lerp(LoadRiverFlow(t0), LoadRiverFlow(t0 + int2(1, 0)), f.x),
		lerp(LoadRiverFlow(t0 + int2(0, 1)), LoadRiverFlow(t0 + int2(1, 1)), f.x), f.y);
	return flow * RiverSpeed;
}

/** @brief Fine bed detail at a world position, where the refined rays have it. */
bool LoadObstacle(float2 worldPos, out float bed)
{
	int2 f = int2(round(worldPos / ObstacleSpacing));
	float4 o = ObstacleMap[uint2(f & int(ObstacleRes - 1))];
	bed = o.x;
	return o.w > 0.5 && all(int2(o.yz) == f);
}

float2 LoadMapLattice(int2 lattice)
{
	return WaterMap[uint2(lattice & int(MapRes - 1))].xy;
}

/** @brief How much the water at a world position flows with the river current (from its water type), bilinear. */
float SampleRiverFlowScale(float2 worldPos)
{
	float2 f = worldPos / MapSpacing;
	int2 i = int2(floor(f));
	float2 t = f - float2(i);
	int mask = int(MapRes - 1);
	float s00 = WaterMap[uint2(i & mask)].z;
	float s10 = WaterMap[uint2((i + int2(1, 0)) & mask)].z;
	float s01 = WaterMap[uint2((i + int2(0, 1)) & mask)].z;
	float s11 = WaterMap[uint2((i + int2(1, 1)) & mask)].z;
	return lerp(lerp(s00, s10, t.x), lerp(s01, s11, t.x), t.y);
}

bool HasWater(float surface)
{
	return surface > NO_WATER * 0.5;
}

/** @brief Bed and vanilla surface at a world position. The surface is interpolated only where all four corners have water. */
float2 SampleWaterMap(float2 worldPos)
{
	float2 f = worldPos / MapSpacing;
	float2 b = floor(f);
	float2 t = f - b;
	int2 i = int2(b);

	float2 m00 = LoadMapLattice(i);
	float2 m10 = LoadMapLattice(i + int2(1, 0));
	float2 m01 = LoadMapLattice(i + int2(0, 1));
	float2 m11 = LoadMapLattice(i + int2(1, 1));

	float bed = lerp(lerp(m00.x, m10.x, t.x), lerp(m01.x, m11.x, t.x), t.y);

	float surface;
	if (HasWater(m00.y) && HasWater(m10.y) && HasWater(m01.y) && HasWater(m11.y))
		surface = lerp(lerp(m00.y, m10.y, t.x), lerp(m01.y, m11.y, t.x), t.y);
	else
		surface = max(max(m00.y, m10.y), max(m01.y, m11.y));

	return float2(bed, surface);
}

float2 CellWorldPos(int2 logical)
{
	return (float2(GridMin + logical) + 0.5) * CellSize;
}

/** @brief Grid edge weight: 1 at the edge, 0 from EdgeWidth texels in. */
float EdgeWeight(int2 logical)
{
	int2 fromEdge = min(logical, int(GridRes) - 1 - logical);
	return saturate(1.0 - float(min(fromEdge.x, fromEdge.y)) / max(EdgeWidth, 1.0));
}

float LoadDepth(Texture2D<float> tex, int2 logical)
{
	return tex[LogicalToStorage(clamp(logical, 0, int(GridRes) - 1))];
}

/** @brief Catmull-Rom weights for the four samples around fractional position t. */
float4 CatmullRomWeights(float t)
{
	float t2 = t * t;
	float t3 = t2 * t;
	return 0.5 * float4(-t3 + 2.0 * t2 - t, 3.0 * t3 - 5.0 * t2 + 2.0, -3.0 * t3 + 4.0 * t2 + t, t3 - t2);
}

/** @brief One velocity component, Catmull-Rom sampled and clamped to the four nearest (no overshoot); keeps eddies longer than bilinear. */
float SampleFace(Texture2D<float2> velocity, float2 f, uint component)
{
	float2 b = floor(f);
	float2 t = f - b;
	int2 i = int2(b);
	float4 wx = CatmullRomWeights(t.x);
	float4 wy = CatmullRomWeights(t.y);
	float result = 0.0;
	float lo = 1.0e30;
	float hi = -1.0e30;
	[unroll] for (int y = 0; y < 4; y++)
	{
		float row = 0.0;
		[unroll] for (int x = 0; x < 4; x++)
		{
			float value = LoadClamped(velocity, i + int2(x - 1, y - 1))[component];
			row += value * wx[x];
			if ((x == 1 || x == 2) && (y == 1 || y == 2)) {
				lo = min(lo, value);
				hi = max(hi, value);
			}
		}
		result += row * wy[y];
	}
	return clamp(result, lo, hi);
}

/** @brief Staggered u (west faces) sampled at a logical position in texel units. u of texel i sits at (i, j + 0.5). */
float SampleU(Texture2D<float2> velocity, float2 position)
{
	return SampleFace(velocity, position - float2(0.0, 0.5), 0);
}

/** @brief Staggered v (south faces) sampled at a logical position in texel units. v of texel j sits at (i + 0.5, j). */
float SampleV(Texture2D<float2> velocity, float2 position)
{
	return SampleFace(velocity, position - float2(0.5, 0.0), 1);
}

#endif  // FLUIDFIELD_WATERCOMMON_HLSLI
