// Fluid Field water, pass 5: what other shaders read. Surface is (slope x, slope y, height above
// the vanilla surface, coverage), and Flow is the cell-centred velocity, foam and depth. Coverage is 0 on dry cells
// and fades to 0 toward the grid edge, so consumers blend back to vanilla there.

#include "FluidField/WaterCommon.hlsli"

Texture2D<float> Depth : register(t0);
Texture2D<float2> Velocity : register(t1);
Texture2D<float2> CellMap : register(t2);
Texture2D<float> Foam : register(t3);

RWTexture2D<float4> SurfaceOut : register(u0);

/** @brief Surface dip, world units, per unit of swirl (1/s) at an eddy's centre. */
static const float EDDY_DIP_PER_SWIRL = 6.0;
/** @brief Steepest surface slope passed to the water shader (rise over run). */
static const float MAX_SLOPE = 0.35;
RWTexture2D<float4> FlowOut : register(u1);

float Eta(int2 logical, float fallback)
{
	uint2 storage = LogicalToStorage(clamp(logical, 0, int(GridRes) - 1));
	float depth = Depth[storage];
	// A dry neighbour has no surface; use this cell's so the shoreline does not read as a cliff
	return depth > WetThreshold ? CellMap[storage].x + depth : fallback;
}

bool IsWet(int2 logical)
{
	return Depth[LogicalToStorage(clamp(logical, 0, int(GridRes) - 1))] > WetThreshold;
}

float2 SlopeAt(int2 logical)
{
	uint2 storage = LogicalToStorage(clamp(logical, 0, int(GridRes) - 1));
	float eta = CellMap[storage].x + Depth[storage];
	return float2(Eta(logical + int2(1, 0), eta) - Eta(logical + int2(-1, 0), eta),
			   Eta(logical + int2(0, 1), eta) - Eta(logical + int2(0, -1), eta)) /
	       (2.0 * CellSize);
}

float2 CentredVelocity(int2 logical)
{
	float2 here = LoadClamped(Velocity, logical);
	return 0.5 * float2(here.x + LoadClamped(Velocity, logical + int2(1, 0)).x, here.y + LoadClamped(Velocity, logical + int2(0, 1)).y);
}

/** @brief Swirl strength |curl v| at a cell centre, per second; zero on dry cells. */
float Swirl(int2 logical)
{
	if (!IsWet(logical))
		return 0.0;
	float dvdx = CentredVelocity(logical + int2(1, 0)).y - CentredVelocity(logical + int2(-1, 0)).y;
	float dudy = CentredVelocity(logical + int2(0, 1)).x - CentredVelocity(logical + int2(0, -1)).x;
	return abs(dvdx - dudy) / (2.0 * CellSize);
}

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);
	float depth = Depth[id];
	float2 cellMap = CellMap[id];
	bool wet = depth > WetThreshold;

	float eta = cellMap.x + depth;

	// Slopes smoothed over the wet cells of a 3 x 3 neighbourhood (tent weights), so water piled
	// against a wall or bank does not show as a one-cell spike
	float2 slope = 0.0;
	float slopeWeight = 0.0;
	[unroll] for (int dy = -1; dy <= 1; dy++)
	{
		[unroll] for (int dx = -1; dx <= 1; dx++)
		{
			int2 n = logical + int2(dx, dy);
			float w = (2.0 - abs(dx)) * (2.0 - abs(dy));
			if (IsWet(n)) {
				slope += SlopeAt(n) * w;
				slopeWeight += w;
			}
		}
	}
	slope = slopeWeight > 0.0 ? slope / slopeWeight : 0.0;

	// Only waves, wakes and eddies should tilt the surface, not the river's overall gradient: subtract
	// the mean slope of the wet water around (a sparse 9 x 9 window, every other cell)
	float2 meanSlope = 0.0;
	float meanWeight = 0.0;
	[unroll] for (int my = -4; my <= 4; my += 2)
	{
		[unroll] for (int mx = -4; mx <= 4; mx += 2)
		{
			int2 n = logical + int2(mx, my);
			if (IsWet(n)) {
				meanSlope += SlopeAt(n);
				meanWeight += 1.0;
			}
		}
	}
	if (meanWeight > 0.0)
		slope -= meanSlope / meanWeight;

	// Eddies dent the surface: a dip where the water swirls, sloping out to its rim
	float dipEast = -EDDY_DIP_PER_SWIRL * Swirl(logical + int2(1, 0));
	float dipWest = -EDDY_DIP_PER_SWIRL * Swirl(logical + int2(-1, 0));
	float dipNorth = -EDDY_DIP_PER_SWIRL * Swirl(logical + int2(0, 1));
	float dipSouth = -EDDY_DIP_PER_SWIRL * Swirl(logical + int2(0, -1));
	slope += float2(dipEast - dipWest, dipNorth - dipSouth) / (2.0 * CellSize);

	// Cap the steepness, so one-step spikes at jagged edges cannot flash. (The output is not read
	// back for temporal smoothing: typed loads of RGBA16F UAVs are optional in D3D11.)
	float slopeLength = length(slope);
	if (slopeLength > MAX_SLOPE)
		slope *= MAX_SLOPE / slopeLength;
	float slopeX = slope.x;
	float slopeY = slope.y;

	// Fade out over about two cells approaching any shore or obstacle
	float wetCount = 0.0;
	[unroll] for (int fy = -2; fy <= 2; fy++)
	{
		[unroll] for (int fx = -2; fx <= 2; fx++)
			wetCount += IsWet(logical + int2(fx, fy)) ? 1.0 : 0.0;
	}
	float shoreFade = smoothstep(0.5, 1.0, wetCount / 25.0);

	// Height relative to vanilla water: vanilla surface is bed + vanilla depth where it is wet
	float vanillaSurface = cellMap.x + cellMap.y;
	float heightOffset = wet && cellMap.y > WetThreshold ? eta - vanillaSurface : 0.0;

	// A wide, smooth fade toward the grid edge (15% of the grid), so the hand-over to vanilla has no line
	int2 fromEdge = min(logical, int(GridRes) - 1 - logical);
	float edgeFade = smoothstep(0.0, 1.0, float(min(fromEdge.x, fromEdge.y)) / (0.15 * float(GridRes)));
	float coverage = wet ? edgeFade * shoreFade : 0.0;

	float2 here = Velocity[id];
	float uE = LoadClamped(Velocity, logical + int2(1, 0)).x;
	float vN = LoadClamped(Velocity, logical + int2(0, 1)).y;
	float2 flow = wet ? 0.5 * float2(here.x + uE, here.y + vN) : 0.0;

	SurfaceOut[id] = float4(slopeX, slopeY, heightOffset, coverage);
	// Foam, softened over its four neighbours so single cells do not sparkle; none on dry cells
	float foam = 0.0;
	if (wet) {
		foam = 0.5 * Foam[id];
		foam += 0.125 * Foam[LogicalToStorage(clamp(logical + int2(1, 0), 0, int(GridRes) - 1))];
		foam += 0.125 * Foam[LogicalToStorage(clamp(logical + int2(-1, 0), 0, int(GridRes) - 1))];
		foam += 0.125 * Foam[LogicalToStorage(clamp(logical + int2(0, 1), 0, int(GridRes) - 1))];
		foam += 0.125 * Foam[LogicalToStorage(clamp(logical + int2(0, -1), 0, int(GridRes) - 1))];
	}
	// The far level behind this one takes over its foam in the water shader's cross-fade, so the
	// foam is not faded here as well (that made a dip, a visible seam)
	// w: the water's depth, for the waterfalls (D27), which thin it where the water is shallower than around it
	FlowOut[id] = float4(flow, SeedRes != 0 ? foam : foam * edgeFade, wet ? Depth[id] : 0.0);
}
