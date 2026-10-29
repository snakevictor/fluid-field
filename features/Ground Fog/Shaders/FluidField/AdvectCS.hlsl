// Fluid Field wind, pass 2: semi-Lagrangian self-advection of velocity, then decay.

#include "FluidField/Common.hlsli"

Texture2D<float2> VelocityIn : register(t0);
RWTexture2D<float2> VelocityOut : register(u0);

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);
	float2 velocity = VelocityIn[id];

	// Trace back along the velocity, in texel units
	float2 source = float2(logical) + 0.5 - DeltaTime * velocity / CellSize;

	VelocityOut[id] = SampleBilinear(VelocityIn, source) * VelocityDecay;
}
