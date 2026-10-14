// Fluid Field water, pass 5: what other shaders read. Surface is (slope x, slope y, height above
// the vanilla surface, coverage), and Flow is the cell-centred velocity. Coverage is 0 on dry cells
// and fades to 0 toward the grid edge, so consumers blend back to vanilla there.

#include "FluidField/WaterCommon.hlsli"

Texture2D<float> Depth : register(t0);
Texture2D<float2> Velocity : register(t1);
Texture2D<float2> CellMap : register(t2);

RWTexture2D<float4> SurfaceOut : register(u0);
RWTexture2D<float2> FlowOut : register(u1);

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

	float coverage = wet ? (1.0 - EdgeWeight(logical)) * shoreFade : 0.0;

	float2 here = Velocity[id];
	float uE = LoadClamped(Velocity, logical + int2(1, 0)).x;
	float vN = LoadClamped(Velocity, logical + int2(0, 1)).y;
	float2 flow = wet ? 0.5 * float2(here.x + uE, here.y + vN) : 0.0;

	SurfaceOut[id] = float4(slopeX, slopeY, heightOffset, coverage);
	FlowOut[id] = flow;
}
