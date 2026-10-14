// Fluid Field wind, pass 1: carry velocity over from the previous step and add emitter impulses.
// Texels that scrolled into the grid since the previous step hold another cell's data, so they
// start from rest, as does every texel after a reset.

#include "FluidField/Common.hlsli"

struct Emitter
{
	float2 Position;  // Logical texel coordinates
	float Radius;     // Texels, Gaussian falloff
	float Pad0;
	float2 Velocity;  // World units per second, added once
	float2 Pad1;
};

Texture2D<float2> VelocityIn : register(t0);
StructuredBuffer<Emitter> Emitters : register(t1);
RWTexture2D<float2> VelocityOut : register(u0);

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);
	int2 cell = GridMin + logical;
	bool keptFromPrevStep = HistoryValid != 0 && all(cell >= PrevGridMin) && all(cell < PrevGridMin + int(GridRes));

	float2 velocity = keptFromPrevStep ? VelocityIn[id] : 0.0;

	float2 position = float2(logical) + 0.5;
	for (uint i = 0; i < EmitterCount; i++) {
		Emitter e = Emitters[i];
		float2 d = position - e.Position;
		velocity += e.Velocity * exp(-dot(d, d) / (e.Radius * e.Radius));
	}

	VelocityOut[id] = velocity;
}
