// Fluid Field water, pass 1: refresh the per-cell map, carry state over from the previous step,
// start new and reset cells from vanilla water, apply emitters, the background current, the level
// relaxation and the edge blend back to vanilla.

#include "FluidField/WaterCommon.hlsli"

struct Emitter
{
	float2 Position;  // Logical texel coordinates
	float Radius;     // Texels, Gaussian falloff
	float DepthRate;  // World units per second added to depth at the centre
	float2 Velocity;  // Velocity the water near the emitter is pulled toward
	float2 Pad1;
};

Texture2D<float> DepthIn : register(t0);
Texture2D<float2> VelocityIn : register(t1);
StructuredBuffer<Emitter> Emitters : register(t3);

RWTexture2D<float> DepthOut : register(u0);
RWTexture2D<float2> VelocityOut : register(u1);
RWTexture2D<float2> CellMapOut : register(u2);

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);
	int2 cell = GridMin + logical;

	float2 map = SampleWaterMap(CellWorldPos(logical));
	float bed = map.x;
	float vanillaDepth = HasWater(map.y) ? max(map.y - bed, 0.0) : 0.0;
	CellMapOut[id] = float2(bed, vanillaDepth);

	bool vanillaWet = vanillaDepth > WetThreshold;
	float2 restVelocity = vanillaWet ? Current : 0.0;

	// Cells that scrolled in, and every cell after a reset, start as calm vanilla water
	bool kept = HistoryValid != 0 && all(cell >= PrevGridMin) && all(cell < PrevGridMin + int(GridRes));
	float depth = kept ? DepthIn[id] : vanillaDepth;
	float2 velocity = kept ? VelocityIn[id] : restVelocity;

	if (depth > WetThreshold) {
		float2 position = float2(logical) + 0.5;
		for (uint i = 0; i < EmitterCount; i++) {
			Emitter e = Emitters[i];
			float2 d = position - e.Position;
			float w = exp(-dot(d, d) / (e.Radius * e.Radius));
			velocity = lerp(velocity, e.Velocity, w * saturate(DeltaTime * 8.0));
			depth += e.DepthRate * w * DeltaTime;
		}
	}

	velocity = lerp(velocity, restVelocity, saturate(CurrentRelax * DeltaTime));
	depth = lerp(depth, vanillaDepth, saturate(LevelRelax * DeltaTime));

	float edge = EdgeWeight(logical) * EdgeRelax;
	velocity = lerp(velocity, restVelocity, edge);
	depth = lerp(depth, vanillaDepth, edge);

	DepthOut[id] = max(depth, 0.0);
	VelocityOut[id] = velocity * VelocityDecay;
}
