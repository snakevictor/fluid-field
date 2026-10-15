// Fluid Field fog (D21): how much ground fog lies on each cell of the wind grid, drawn by the
// ground fog pass (GroundFogCS). 1 is full fog, 0 clear air.
//
// Each step the fog is carried along by the wind and the weather's drift and settles back over a
// few seconds toward a resting pattern of banks and clearings. Fog is thick and slow, and only
// moving actors disturb it: their bodies plough it aside, mostly ahead and to the sides, packing it
// up (to MaxFog, twice the resting amount) along their path, and the air they push curls it into
// swirls behind them. An actor standing still pushes nothing, so the fog flows back over its feet.
// Strong stirring thins it a little, mixing in clear air.
// Cells that scrolled into the grid since the last step, and every cell after a reset, start at rest.

#include "FluidField/Common.hlsli"
#include "FluidField/FogPattern.hlsli"

cbuffer FogCB : register(b1)
{
	float2 Drift;         // Weather wind carrying the fog, world units per second
	float RefillRate;     // Per-second rate at which thinned fog fills back in toward 1
	float StirRate;       // Per-second rate at which fully stirred air clears the fog
	float StirSpeed;      // Wind speed (world units per second) that counts as fully stirred
	float DisplaceSpeed;  // Speed (world units per second) at which an actor's body shoves the fog outward
	float MaxFog;         // Most fog a cell can be packed to, as a multiple of the resting amount
	float FogPad;
	float2 PatternOffset;  // How far the resting pattern has drifted with the weather's wind, world units
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

/** Actor speed at which the shove reaches DisplaceSpeed (a run). */
static const float FULL_SHOVE_SPEED = 350.0;

Texture2D<float2> Velocity : register(t0);
Texture2D<float> FogIn : register(t1);
StructuredBuffer<Emitter> Emitters : register(t2);
RWTexture2D<float> FogOut : register(u0);

/** Last step's fog at a logical cell; 1 for cells that were not in the grid then (or after a reset). */
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

/**
 * How moving actors plough the fog at a logical position, world units per second: outward from
 * each actor, mostly ahead of it and to its sides, scaled by its speed. Zero at its centre,
 * strongest around its edge and gone a little beyond.
 */
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
		// Ahead and to the sides; little behind, where the actor has already been
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

	// Carried along by the wind, the weather's drift and the shove of actors' bodies; where that
	// shove spreads out the fog thins, where it converges (just outside the body) the fog packs up
	float2 position = float2(logical) + 0.5;
	float2 shove = Displacement(position);
	float2 source = position - DeltaTime * (wind + Drift + shove) / CellSize;
	float fog = SampleFog(source);
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
