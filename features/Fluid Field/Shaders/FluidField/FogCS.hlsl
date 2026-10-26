// Fluid Field fog (D21): ground fog amount per wind-grid cell (1 full, 0 clear), drawn by GroundFogCS.
// Carried by the wind and the weather's drift, it settles back toward a resting pattern over a few seconds.
// Only moving actors disturb it: their bodies plough it aside (mostly ahead and sideways), packing it up to
// MaxFog, and their wake curls it; a standing actor pushes nothing. Strong stirring thins it a little.
// Cells that scrolled in, and all cells after a reset, start at the resting pattern.

#include "FluidField/Common.hlsli"
#include "FluidField/FogPattern.hlsli"

cbuffer FogCB : register(b1)
{
	float2 Drift;         // Weather wind carrying the fog, world units per second
	float RefillRate;     // Per-second rate at which fog relaxes toward the resting pattern
	float StirRate;       // Per-second rate at which fully stirred air clears the fog
	float StirSpeed;      // Wind speed (world units per second) that counts as fully stirred
	float DisplaceSpeed;  // World units per second an actor's body shoves the fog outward
	float MaxFog;         // Most fog a cell can be packed to, as a multiple of the resting amount
	float FogPad;
	float2 PatternOffset;  // Resting pattern's drift with the weather's wind, world units
	float2 FogPad2;
};

struct Emitter
{
	float2 Position;  // Logical texel coordinates
	float Radius;     // Texels, Gaussian falloff
	float Speed;      // The actor's speed, world units per second
	float2 Velocity;  // Wind impulse, along the actor's motion
	float2 Pad1;
};

/** Heavy fog follows the air only partly, and smooths out as it moves. */
static const float FOG_DRAG = 0.6;
static const float FOG_DIFFUSION = 0.15;

/** Actor speed at which the shove reaches DisplaceSpeed (a run). */
static const float FULL_SHOVE_SPEED = 350.0;

Texture2D<float2> Velocity : register(t0);
Texture2D<float> FogIn : register(t1);
StructuredBuffer<Emitter> Emitters : register(t2);
RWTexture2D<float> FogOut : register(u0);

/** Last step's fog at a logical cell; the resting pattern for cells not in the grid then (or after a reset). */
float LoadFog(int2 logical)
{
	int2 cell = GridMin + logical;
	bool kept = HistoryValid != 0 && all(cell >= PrevGridMin) && all(cell < PrevGridMin + int(GridRes));
	return kept ? FogIn[LogicalToStorage(clamp(logical, 0, int(GridRes) - 1))] : FogPattern::Resting((float2(cell) + 0.5) * CellSize, PatternOffset);
}

/** Last step's fog at a logical position (texel centres at +0.5), bilinear. */
float SampleFog(float2 position)
{
	float2 p = position - 0.5;
	int2 i = int2(floor(p));
	float2 t = p - float2(i);
	return lerp(lerp(LoadFog(i), LoadFog(i + int2(1, 0)), t.x), lerp(LoadFog(i + int2(0, 1)), LoadFog(i + int2(1, 1)), t.x), t.y);
}

/** Actors' shove on the fog at a logical position (world units/s): outward, mostly ahead and sideways, peaking at their edge. */
float2 Displacement(float2 position)
{
	float2 shove = 0.0;
	for (uint i = 0; i < EmitterCount; i++) {
		Emitter e = Emitters[i];
		float push = DisplaceSpeed * saturate(e.Speed / FULL_SHOVE_SPEED);
		float impulse = length(e.Velocity);
		if (push <= 0.0 || impulse <= 0.0)
			continue;
		float2 heading = e.Velocity / impulse;
		float2 d = position - e.Position;
		float distance = length(d);
		float r = max(e.Radius, 0.5);
		float2 outward = d / max(distance, 1e-3);
		// Little behind, where the actor has already been
		float bow = 0.25 + 0.75 * saturate(dot(outward, heading) * 0.5 + 0.6);
		// d/r * exp(-(d/r)^2) peaks at 1/sqrt(2) of the radius; scaled so the peak is the push
		shove += outward * push * bow * 2.33 * (distance / r) * exp(-dot(d, d) / (r * r));
	}
	return shove;
}

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);
	float2 wind = Velocity[id];

	// Advected by wind, drift and shove; the fog thins where the shove diverges and packs up where it converges
	float2 position = float2(logical) + 0.5;
	float2 shove = Displacement(position);
	float2 source = position - DeltaTime * (wind * FOG_DRAG + Drift + shove) / CellSize;
	float fog = SampleFog(source);
	// Viscous: disturbances soften rather than staying sharp
	float neighbours = 0.25 * (SampleFog(source + float2(1, 0)) + SampleFog(source - float2(1, 0)) + SampleFog(source + float2(0, 1)) + SampleFog(source - float2(0, 1)));
	fog = lerp(fog, neighbours, saturate(FOG_DIFFUSION * DeltaTime * 60.0));
	float divergence = (Displacement(position + float2(1, 0)).x - Displacement(position - float2(1, 0)).x +
						   Displacement(position + float2(0, 1)).y - Displacement(position - float2(0, 1)).y) /
	                   (2.0 * CellSize);
	fog *= exp(-divergence * DeltaTime);

	// Thinned where the air is stirred, then filling back in
	float stirred = saturate(length(wind) / max(StirSpeed, 1.0));
	fog *= exp(-StirRate * stirred * DeltaTime);
	const float resting = FogPattern::Resting((float2(GridMin + logical) + 0.5) * CellSize, PatternOffset);
	fog = resting + (fog - resting) * exp(-RefillRate * DeltaTime);

	FogOut[id] = clamp(fog, 0.0, MaxFog);
}
