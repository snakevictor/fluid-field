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

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);
	float depth = Depth[id];
	float2 cellMap = CellMap[id];
	bool wet = depth > WetThreshold;

	float eta = cellMap.x + depth;
	float slopeX = (Eta(logical + int2(1, 0), eta) - Eta(logical + int2(-1, 0), eta)) / (2.0 * CellSize);
	float slopeY = (Eta(logical + int2(0, 1), eta) - Eta(logical + int2(0, -1), eta)) / (2.0 * CellSize);

	// Height relative to vanilla water: vanilla surface is bed + vanilla depth where it is wet
	float vanillaSurface = cellMap.x + cellMap.y;
	float heightOffset = wet && cellMap.y > WetThreshold ? eta - vanillaSurface : 0.0;

	float coverage = wet ? 1.0 - EdgeWeight(logical) : 0.0;

	float2 here = Velocity[id];
	float uE = LoadClamped(Velocity, logical + int2(1, 0)).x;
	float vN = LoadClamped(Velocity, logical + int2(0, 1)).y;
	float2 flow = wet ? 0.5 * float2(here.x + uE, here.y + vN) : 0.0;

	SurfaceOut[id] = float4(slopeX, slopeY, heightOffset, coverage);
	FlowOut[id] = flow;
}
