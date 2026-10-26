// Fluid Field wind, pass 1: carry velocity over and add emitter impulses. Texels that scrolled in hold another
// cell's data, so they start from rest, as does every texel after a reset.

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

/** Vorticity confinement strength: restores swirls advection smooths away, so stirred air (and its fog) keeps curling. */
static const float VORTICITY = 0.6;

/** Curl of the velocity at a logical cell, 1/s (central differences). */
float Curl(int2 logical)
{
	float2 east = LoadClamped(VelocityIn, logical + int2(1, 0));
	float2 west = LoadClamped(VelocityIn, logical - int2(1, 0));
	float2 north = LoadClamped(VelocityIn, logical + int2(0, 1));
	float2 south = LoadClamped(VelocityIn, logical - int2(0, 1));
	return ((east.y - west.y) - (north.x - south.x)) / (2.0 * CellSize);
}
StructuredBuffer<Emitter> Emitters : register(t1);
RWTexture2D<float2> VelocityOut : register(u0);

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);
	int2 cell = GridMin + logical;
	bool keptFromPrevStep = HistoryValid != 0 && all(cell >= PrevGridMin) && all(cell < PrevGridMin + int(GridRes));

	float2 velocity = keptFromPrevStep ? VelocityIn[id] : 0.0;

	// Vorticity confinement: push each swirl's flow around its centre
	if (keptFromPrevStep) {
		float curl = Curl(logical);
		float2 gradient = float2(abs(Curl(logical + int2(1, 0))) - abs(Curl(logical - int2(1, 0))),
			abs(Curl(logical + int2(0, 1))) - abs(Curl(logical - int2(0, 1))));
		float magnitude = length(gradient);
		if (magnitude > 1e-6) {
			float2 n = gradient / magnitude;
			velocity += VORTICITY * CellSize * curl * float2(n.y, -n.x) * DeltaTime;
		}
	}

	float2 position = float2(logical) + 0.5;
	for (uint i = 0; i < EmitterCount; i++) {
		Emitter e = Emitters[i];
		float2 d = position - e.Position;
		velocity += e.Velocity * exp(-dot(d, d) / (e.Radius * e.Radius));
	}

	VelocityOut[id] = velocity;
}
