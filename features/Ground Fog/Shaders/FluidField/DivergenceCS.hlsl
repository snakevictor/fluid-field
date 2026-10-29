// Fluid Field wind, pass 3: velocity divergence, per second.

#include "FluidField/Common.hlsli"

Texture2D<float2> Velocity : register(t0);
RWTexture2D<float> DivergenceOut : register(u0);

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);

	float left = LoadClamped(Velocity, logical + int2(-1, 0)).x;
	float right = LoadClamped(Velocity, logical + int2(1, 0)).x;
	float down = LoadClamped(Velocity, logical + int2(0, -1)).y;
	float up = LoadClamped(Velocity, logical + int2(0, 1)).y;

	DivergenceOut[id] = (right - left + up - down) / (2.0 * CellSize);
}
