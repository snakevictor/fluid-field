// Fluid Field wind, pass 4: one Jacobi iteration of the pressure Poisson equation.
// Pressure outside the grid is 0, so wind leaves through the edge freely.

#include "FluidField/Common.hlsli"

Texture2D<float> PressureIn : register(t0);
Texture2D<float> Divergence : register(t1);
RWTexture2D<float> PressureOut : register(u0);

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);

	float neighbours = LoadOrZero(PressureIn, logical + int2(-1, 0)) +
	                   LoadOrZero(PressureIn, logical + int2(1, 0)) +
	                   LoadOrZero(PressureIn, logical + int2(0, -1)) +
	                   LoadOrZero(PressureIn, logical + int2(0, 1));

	PressureOut[id] = (neighbours - CellSize * CellSize * Divergence[id]) * 0.25;
}
