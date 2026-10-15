// Fluid Field water, foam pass (D19): foam is a quantity carried by the simulated flow. Each step it
// is advected along the cell-centred velocity, fades with a lifetime of a few seconds, and is made
// where real water turns white:
//   - shear: the flow changes sharply across a cell, as beside pillars and rocks;
//   - impact: water running at a dry cell (an obstacle, a bank), and fast water rushing along one;
//   - breaking: fast, shallow flow (Froude number near or above 1), as over rapids;
//   - actors: wakes and splashes from the emitters.
// Foam streams downstream of every obstacle and gathers in eddies, which vanilla's placed foam
// effects cannot do.

#include "FluidField/WaterCommon.hlsli"

struct Emitter
{
	float2 Position;  // Logical texel coordinates
	float Radius;     // Texels, Gaussian falloff
	float DepthRate;  // World units per second added to depth at the centre (negative: a splash)
	float2 Velocity;  // Velocity the water near the emitter is pulled toward
	float2 Pad1;
};

Texture2D<float> Depth : register(t0);
Texture2D<float2> Velocity : register(t1);
Texture2D<float2> CellMap : register(t2);
StructuredBuffer<Emitter> Emitters : register(t3);
Texture2D<float> FoamIn : register(t7);

RWTexture2D<float> FoamOut : register(u0);

/** Seconds for foam to fade to about a third. */
static const float FOAM_LIFETIME = 4.0;
/** Most foam a cell holds (1 is fully white once drawn). */
static const float FOAM_MAX = 1.5;
/** Foam made per second at full strength by each source. */
static const float SHEAR_RATE = 2.0;
static const float IMPACT_RATE = 5.0;
static const float WALL_RATE = 1.5;
static const float BREAKING_RATE = 1.5;
static const float WAKE_RATE = 3.0;
static const float SPLASH_RATE = 8.0;

/** The eight neighbour directions, for finding obstacles around a cell. */
static const int2 NEIGHBOURS[8] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1), int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1) };

bool IsWet(int2 logical)
{
	return LoadDepth(Depth, logical) > WetThreshold;
}

float2 CentredVelocity(int2 logical)
{
	float2 here = LoadClamped(Velocity, logical);
	return 0.5 * float2(here.x + LoadClamped(Velocity, logical + int2(1, 0)).x, here.y + LoadClamped(Velocity, logical + int2(0, 1)).y);
}

/** Last step's foam at a logical cell; 0 for cells that were not in the grid then (or after a reset). */
float LoadFoam(int2 logical)
{
	int2 cell = GridMin + logical;
	bool kept = HistoryValid != 0 && all(cell >= PrevGridMin) && all(cell < PrevGridMin + int(GridRes));
	return kept ? FoamIn[LogicalToStorage(clamp(logical, 0, int(GridRes) - 1))] : 0.0;
}

/** Last step's foam at a logical position (texel centres at +0.5), bilinear. */
float SampleFoam(float2 position)
{
	float2 p = position - 0.5;
	int2 i = int2(floor(p));
	float2 t = p - float2(i);
	return lerp(lerp(LoadFoam(i), LoadFoam(i + int2(1, 0)), t.x), lerp(LoadFoam(i + int2(0, 1)), LoadFoam(i + int2(1, 1)), t.x), t.y);
}

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);
	float depth = Depth[id];
	if (depth <= WetThreshold || FoamStrength <= 0.0) {
		FoamOut[id] = 0.0;
		return;
	}

	// Carried along the flow, and fading
	float2 v = CentredVelocity(logical);
	float2 position = float2(logical) + 0.5;
	float foam = SampleFoam(position - v * DeltaTime / CellSize) * exp(-DeltaTime / FOAM_LIFETIME);

	// Shear: how sharply the flow changes across this cell (1/s), between wet neighbours only
	float2 vE = IsWet(logical + int2(1, 0)) ? CentredVelocity(logical + int2(1, 0)) : v;
	float2 vW = IsWet(logical + int2(-1, 0)) ? CentredVelocity(logical + int2(-1, 0)) : v;
	float2 vN = IsWet(logical + int2(0, 1)) ? CentredVelocity(logical + int2(0, 1)) : v;
	float2 vS = IsWet(logical + int2(0, -1)) ? CentredVelocity(logical + int2(0, -1)) : v;
	float shear = (length(vE - vW) + length(vN - vS)) / (2.0 * CellSize);

	// Impact: water running at an obstacle or a bank within two cells. The solver stops the flow
	// at the wall itself, so the speed that counts is the one arriving from a cell upstream.
	float speed = length(v);
	int2 upstream = speed > 1.0 ? int2(round(-v / speed)) : int2(0, 0);
	float2 incoming = CentredVelocity(logical + upstream);
	float impact = 0.0;
	float walls = 0.0;
	[unroll] for (int n = 0; n < 8; n++)
	{
		float2 direction = normalize(float2(NEIGHBOURS[n]));
		[unroll] for (int r = 1; r <= 2; r++)
		{
			if (!IsWet(logical + NEIGHBOURS[n] * r)) {
				impact = max(impact, dot(incoming, direction) * (r == 1 ? 1.0 : 0.6));
				walls += r == 1 ? 1.0 : 0.5;
			}
		}
	}
	// Fast water rushing along a rock or bank churns white as well, but only well past a calm river's speed
	float wallRush = saturate(walls) * saturate((speed - 100.0) / 200.0);

	// Breaking: fast, shallow water; Froude number = speed / sqrt(g * depth)
	float froude = speed / sqrt(Gravity * max(depth, 1.0));

	float made = SHEAR_RATE * saturate((shear - 0.25) * 0.8) + IMPACT_RATE * saturate((impact - 10.0) / 80.0) +
	             WALL_RATE * wallRush + BREAKING_RATE * saturate((froude - 0.5) * 2.0);

	// Actors: a wake whitens the water behind the actor, a splash much more
	for (uint i = 0; i < EmitterCount; i++) {
		Emitter e = Emitters[i];
		float2 d = position - e.Position;
		float w = exp(-dot(d, d) / (e.Radius * e.Radius));
		made += w * (WAKE_RATE * saturate(length(e.Velocity) / 300.0) + SPLASH_RATE * saturate(-e.DepthRate / 400.0));
	}

	foam = min(foam + made * FoamStrength * DeltaTime, FOAM_MAX);
	// No foam at the grid's border, so nothing piles up where the water is eased back to vanilla
	foam *= 1.0 - EdgeWeight(logical);
	FoamOut[id] = foam;
}
