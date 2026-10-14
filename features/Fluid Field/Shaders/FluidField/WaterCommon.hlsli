#ifndef FLUIDFIELD_WATERCOMMON_HLSLI
#define FLUIDFIELD_WATERCOMMON_HLSLI

// Fluid Field shallow water: shared constants, map sampling and staggered-grid helpers.
//
// State lives on the same toroidal grid as the wind (Common.hlsli):
//   Depth    R32F:  water depth above the bed at the cell centre
//   Velocity RG32F: staggered; R is u on the cell's west face, G is v on its south face
//   CellMap  RG32F: per-cell bed height and vanilla depth, refreshed every step from WaterMap
// WaterMap is a coarse, world-aligned, toroidal map built on the CPU: R is the bed height,
// G the vanilla water surface height (NO_WATER where there is none), at MapSpacing intervals.
// ObstacleMap is finer detail around structures (pillars, piers, rocks), from refined bed rays:
// R the bed height, GB the texel's own fine lattice coordinates (so stale wrapped texels are
// rejected), A the kind (0 none, 1 landscape, 2 mesh), at ObstacleSpacing intervals.

#include "FluidField/Common.hlsli"

cbuffer WaterCB : register(b1)
{
	float Gravity;          // World units per second squared
	float MaxDepth;         // Depth used for transport is capped here, keeping waves shallow and the step stable
	float MaxSpeed;         // Face velocities are clamped to this, world units per second
	float WetThreshold;     // Depth below which a cell counts as dry
	float2 Current;         // Background current, world units per second
	float CurrentRelax;     // Per-second rate at which velocity relaxes toward the current
	float LevelRelax;       // Per-second rate at which depth relaxes toward the vanilla depth
	uint MapRes;            // WaterMap texels per side, a power of two
	float MapSpacing;       // World units between WaterMap texels
	float EdgeWidth;        // Texels over which the grid edge blends back to vanilla
	float EdgeRelax;        // Per-step blend toward vanilla at the very edge
	uint ObstacleRes;       // ObstacleMap texels per side, a power of two (0: no detail)
	float ObstacleSpacing;  // World units between ObstacleMap texels
	float2 WaterPad0;
};

static const float NO_WATER = -1.0e8;

Texture2D<float2> WaterMap : register(t4);
Texture2D<float4> ObstacleMap : register(t5);

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
	return WaterMap[uint2(lattice & int(MapRes - 1))];
}

bool HasWater(float surface)
{
	return surface > NO_WATER * 0.5;
}

/** @brief Bed and vanilla surface at a world position. A surface is only interpolated between lattice points that all have water. */
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

/** @brief Texels from the nearest grid edge, as a 0..1 weight that is 1 at the edge and 0 past EdgeWidth. */
float EdgeWeight(int2 logical)
{
	int2 fromEdge = min(logical, int(GridRes) - 1 - logical);
	return saturate(1.0 - float(min(fromEdge.x, fromEdge.y)) / max(EdgeWidth, 1.0));
}

float LoadDepth(Texture2D<float> tex, int2 logical)
{
	return tex[LogicalToStorage(clamp(logical, 0, int(GridRes) - 1))];
}

/** @brief Staggered u (west faces) sampled at a logical position in texel units. u of texel i sits at (i, j + 0.5). */
float SampleU(Texture2D<float2> velocity, float2 position)
{
	float2 f = position - float2(0.0, 0.5);
	float2 b = floor(f);
	float2 t = f - b;
	int2 i = int2(b);
	float u00 = LoadClamped(velocity, i).x;
	float u10 = LoadClamped(velocity, i + int2(1, 0)).x;
	float u01 = LoadClamped(velocity, i + int2(0, 1)).x;
	float u11 = LoadClamped(velocity, i + int2(1, 1)).x;
	return lerp(lerp(u00, u10, t.x), lerp(u01, u11, t.x), t.y);
}

/** @brief Staggered v (south faces) sampled at a logical position in texel units. v of texel j sits at (i + 0.5, j). */
float SampleV(Texture2D<float2> velocity, float2 position)
{
	float2 f = position - float2(0.5, 0.0);
	float2 b = floor(f);
	float2 t = f - b;
	int2 i = int2(b);
	float v00 = LoadClamped(velocity, i).y;
	float v10 = LoadClamped(velocity, i + int2(1, 0)).y;
	float v01 = LoadClamped(velocity, i + int2(0, 1)).y;
	float v11 = LoadClamped(velocity, i + int2(1, 1)).y;
	return lerp(lerp(v00, v10, t.x), lerp(v01, v11, t.x), t.y);
}

#endif  // FLUIDFIELD_WATERCOMMON_HLSLI
