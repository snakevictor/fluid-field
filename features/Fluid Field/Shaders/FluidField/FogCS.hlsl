// Fluid Field fog (D21): how much ground fog lies on each cell of the wind grid, drawn by the
// ground fog pass (GroundFogCS). 1 is full fog, 0 clear air.
//
// Each step the fog is carried along by the wind and the weather's drift and settles back over a
// few seconds toward a resting pattern of banks and clearings. Fog is thick and slow: an actor's
// body shoves it aside rather than clearing it, so it piles into a raised rim around them (up to
// MaxFog, twice the resting amount) and leaves a clear patch at their feet; walking drags the
// displaced fog along and the air they push curls it into swirls behind them. Strong stirring
// thins it a little, mixing in clear air.
// Cells that scrolled into the grid since the last step, and every cell after a reset, start at rest.

#include "FluidField/Common.hlsli"

cbuffer FogCB : register(b1)
{
	float2 Drift;         // Weather wind carrying the fog, world units per second
	float RefillRate;     // Per-second rate at which thinned fog fills back in toward 1
	float StirRate;       // Per-second rate at which fully stirred air clears the fog
	float StirSpeed;      // Wind speed (world units per second) that counts as fully stirred
	float DisplaceSpeed;  // Speed (world units per second) at which an actor's body shoves the fog outward
	float MaxFog;         // Most fog a cell can be packed to, as a multiple of the resting amount
	float FogPad;
};

struct Emitter
{
	float2 Position;  // Logical texel coordinates
	float Radius;     // Texels, Gaussian falloff
	float Pad0;
	float2 Velocity;
	float2 Pad1;
};

Texture2D<float2> Velocity : register(t0);
Texture2D<float> FogIn : register(t1);
StructuredBuffer<Emitter> Emitters : register(t2);
RWTexture2D<float> FogOut : register(u0);

/** World units across one patch of the resting fog: fog lies in banks and clearings, not as a sheet. */
static const float PATCH_SIZE = 900.0;
/** Fog left in the clearings between banks, as a fraction of the banks'. */
static const float PATCH_FLOOR = 0.3;

float Hash(float2 p)
{
	return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

/** Smooth value noise, 0 to 1. */
float ValueNoise(float2 p)
{
	const float2 i = floor(p);
	const float2 f = p - i;
	const float2 u = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), u.x), lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), u.x), u.y);
}

/** How much fog rests at a world position when nothing disturbs it. */
float RestingFog(float2 worldXY)
{
	const float2 p = worldXY / PATCH_SIZE;
	const float n = ValueNoise(p) * 0.65 + ValueNoise(p * 2.7 + 17.3) * 0.35;
	return lerp(PATCH_FLOOR, 1.0, smoothstep(0.3, 0.7, n));
}

/** Last step's fog at a logical cell; 1 for cells that were not in the grid then (or after a reset). */
float LoadFog(int2 logical)
{
	int2 cell = GridMin + logical;
	bool kept = HistoryValid != 0 && all(cell >= PrevGridMin) && all(cell < PrevGridMin + int(GridRes));
	return kept ? FogIn[LogicalToStorage(clamp(logical, 0, int(GridRes) - 1))] : RestingFog((float2(cell) + 0.5) * CellSize);
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
 * How actors' bodies shove the fog at a logical position, world units per second: outward from
 * each actor, zero at its centre, strongest around its edge and gone a little beyond, so the fog
 * clears at the feet and packs into a rim.
 */
float2 Displacement(float2 position)
{
	float2 shove = 0.0;
	for (uint i = 0; i < EmitterCount; i++) {
		Emitter e = Emitters[i];
		float2 d = position - e.Position;
		float distance = length(d);
		float r = max(e.Radius, 0.5);
		// d/r * exp(-(d/r)^2) peaks at 1/sqrt(2) of the radius; scaled so the peak is DisplaceSpeed
		shove += (d / max(distance, 1e-3)) * DisplaceSpeed * 2.33 * (distance / r) * exp(-dot(d, d) / (r * r));
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
	const float resting = RestingFog((float2(GridMin + logical) + 0.5) * CellSize);
	fog = resting + (fog - resting) * exp(-RefillRate * DeltaTime);

	FogOut[id] = clamp(fog, 0.0, MaxFog);
}
