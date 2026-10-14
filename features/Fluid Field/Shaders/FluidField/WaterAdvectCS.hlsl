// Fluid Field water, pass 2: semi-Lagrangian advection of the staggered velocity, so currents
// carry waves and wakes downstream.

#include "FluidField/WaterCommon.hlsli"

Texture2D<float2> VelocityIn : register(t0);
RWTexture2D<float2> VelocityOut : register(u0);

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);
	float2 here = VelocityIn[id];

	// u on the west face, at (x, y + 0.5): v there is the mean of the four surrounding south faces
	float2 uPos = float2(logical) + float2(0.0, 0.5);
	float vAtU = 0.25 * (LoadClamped(VelocityIn, logical).y + LoadClamped(VelocityIn, logical + int2(-1, 0)).y +
							LoadClamped(VelocityIn, logical + int2(0, 1)).y + LoadClamped(VelocityIn, logical + int2(-1, 1)).y);
	float u = SampleU(VelocityIn, uPos - DeltaTime * float2(here.x, vAtU) / CellSize);

	// v on the south face, at (x + 0.5, y): u there is the mean of the four surrounding west faces
	float2 vPos = float2(logical) + float2(0.5, 0.0);
	float uAtV = 0.25 * (LoadClamped(VelocityIn, logical).x + LoadClamped(VelocityIn, logical + int2(1, 0)).x +
							LoadClamped(VelocityIn, logical + int2(0, -1)).x + LoadClamped(VelocityIn, logical + int2(1, -1)).x);
	float v = SampleV(VelocityIn, vPos - DeltaTime * float2(uAtV, here.y) / CellSize);

	VelocityOut[id] = float2(u, v);
}
