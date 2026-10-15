// Fluid Field water, pass 1: refresh the per-cell map, carry state over from the previous step,
// start new and reset cells from vanilla water, apply emitters, the background current, the level
// relaxation and the edge blend back to vanilla.
//
// When a cell's bed changes under live water (its map arrived late, or fine detail landed), the
// water surface is kept where it was and the depth recomputed, so data arriving makes no waves.
// If the cell changes between wet and dry, it restarts as calm vanilla water instead.

#include "FluidField/WaterCommon.hlsli"

struct Emitter
{
	float2 Position;    // Logical texel coordinates
	float Radius;       // Texels, Gaussian falloff
	float DepthRate;    // World units per second added to depth at the centre
	float2 Velocity;    // Velocity the water near the emitter is pulled toward
	float RadialSpeed;  // Speed the water spreads out from the centre, on top of Velocity (waterfalls, D22)
	float FoamRate;     // Foam made per second at the centre (read by the foam pass)
};

Texture2D<float> DepthIn : register(t0);
Texture2D<float2> VelocityIn : register(t1);
Texture2D<float2> CellMapIn : register(t2);
StructuredBuffer<Emitter> Emitters : register(t3);

static const float BED_CHANGE_THRESHOLD = 2.0;
/** World units of depth per second that drain from water running off vanilla's planes in a fall area. */
static const float FALL_DRAIN = 1.5;

RWTexture2D<float> DepthOut : register(u0);
RWTexture2D<float2> VelocityOut : register(u1);
RWTexture2D<float2> CellMapOut : register(u2);

/** @brief Cell-centred velocity from the staggered faces (u on the west face, v on the south face). */
float2 CentredVelocity(int2 logical)
{
	float2 here = LoadClamped(VelocityIn, logical);
	return 0.5 * float2(here.x + LoadClamped(VelocityIn, logical + int2(1, 0)).x, here.y + LoadClamped(VelocityIn, logical + int2(0, 1)).y);
}

/** @brief Vorticity (curl of the velocity) at a cell centre, per second. */
float Curl(int2 logical)
{
	float dvdx = CentredVelocity(logical + int2(1, 0)).y - CentredVelocity(logical + int2(-1, 0)).y;
	float dudy = CentredVelocity(logical + int2(0, 1)).x - CentredVelocity(logical + int2(0, -1)).x;
	return (dvdx - dudy) / (2.0 * CellSize);
}

/**
 * @brief Vorticity confinement: a push along N x omega, where N points up the gradient of |omega|.
 * It puts back the swirl that advection smears out, so eddies behind obstacles stay alive.
 */
float2 ConfinementForce(int2 logical)
{
	float omega = Curl(logical);
	float2 gradient = float2(abs(Curl(logical + int2(1, 0))) - abs(Curl(logical + int2(-1, 0))),
		abs(Curl(logical + int2(0, 1))) - abs(Curl(logical + int2(0, -1))));
	float len = length(gradient);
	if (len < 1.0e-5)
		return 0.0;
	float2 n = gradient / len;
	return Vorticity * CellSize * float2(n.y * omega, -n.x * omega);
}

/** @brief Hash of a cell and a time step to [-1, 1]. */
float CellNoise(int2 cell, float step)
{
	uint h = uint(cell.x) * 73856093u ^ uint(cell.y) * 19349663u ^ uint(step) * 83492791u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return float(h & 0xFFFFu) / 32767.5 - 1.0;
}

/** @brief Smooth value noise in 3D (space and time), in [-1, 1]. */
float Hash3(int3 p)
{
	uint h = uint(p.x) * 73856093u ^ uint(p.y) * 19349663u ^ uint(p.z) * 83492791u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return float(h & 0xFFFFu) / 32767.5 - 1.0;
}

float ValueNoise(float3 p)
{
	int3 i = int3(floor(p));
	float3 f = p - float3(i);
	float3 u = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(lerp(Hash3(i), Hash3(i + int3(1, 0, 0)), u.x), lerp(Hash3(i + int3(0, 1, 0)), Hash3(i + int3(1, 1, 0)), u.x), u.y),
		lerp(lerp(Hash3(i + int3(0, 0, 1)), Hash3(i + int3(1, 0, 1)), u.x), lerp(Hash3(i + int3(0, 1, 1)), Hash3(i + int3(1, 1, 1)), u.x), u.y), u.z);
}

/** @brief World units across one stirring swirl, and how fast the stirring pattern changes. */
static const float STIR_SCALE = 160.0;
static const float STIR_RATE = 0.35;
/** @brief Stirring acceleration per unit of river speed (1/s), before Turbulence scales it. */
static const float STIR_STRENGTH = 0.9;

/**
 * @brief Curl noise: the curl of a smooth, slowly changing stream function, so it swirls water
 * without piling it up anywhere. Real rivers never settle into one flow; this keeps ours moving.
 */
float2 StirVelocity(float2 worldPos)
{
	float3 p = float3(worldPos / STIR_SCALE, SimTime * STIR_RATE);
	const float e = 0.5;
	float dPsiDx = ValueNoise(p + float3(e, 0, 0)) - ValueNoise(p - float3(e, 0, 0));
	float dPsiDy = ValueNoise(p + float3(0, e, 0)) - ValueNoise(p - float3(0, e, 0));
	// A second, finer octave
	float3 q = p * float3(2.3, 2.3, 1.7) + 17.0;
	dPsiDx += 0.5 * (ValueNoise(q + float3(e, 0, 0)) - ValueNoise(q - float3(e, 0, 0)));
	dPsiDy += 0.5 * (ValueNoise(q + float3(0, e, 0)) - ValueNoise(q - float3(0, e, 0)));
	return float2(dPsiDy, -dPsiDx) / (2.0 * e);
}

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);
	int2 cell = GridMin + logical;

	float2 worldPos = CellWorldPos(logical);
	float2 map = SampleWaterMap(worldPos);
	float bed = map.x;
	float detailBed;
	if (ObstacleRes > 0 && LoadObstacle(worldPos, detailBed))
		bed = detailBed;
	// Under waterfalls the rock is mapped by rays (D24): the water runs over its surface
	const float fallBed = FallBedEnabled != 0 ? FallBed[id] : NO_FALL_BED * 2.0;
	const bool inFall = fallBed > NO_FALL_BED;
	if (inFall)
		bed = max(bed, fallBed);
	float vanillaDepth = HasWater(map.y) ? max(map.y - bed, 0.0) : 0.0;
	CellMapOut[id] = float2(bed, vanillaDepth);

	bool vanillaWet = vanillaDepth > WetThreshold;
	// Rivers follow the flowmap; lakes and the sea (water types without the flowmap) stay still
	float2 restVelocity = vanillaWet ? (FlowmapEnabled != 0 ? SampleRiverFlow(worldPos) * SampleRiverFlowScale(worldPos) : Current) : 0.0;

	// Cells that scrolled in, and every cell after a reset, start as calm vanilla water
	bool kept = HistoryValid != 0 && all(cell >= PrevGridMin) && all(cell < PrevGridMin + int(GridRes));
	float depth = kept ? DepthIn[id] : vanillaDepth;
	float2 velocity = kept ? VelocityIn[id] : restVelocity;

	if (kept) {
		float oldBed = CellMapIn[id].x;
		if (abs(bed - oldBed) > BED_CHANGE_THRESHOLD) {
			bool wasWet = depth > WetThreshold;
			float keptDepth = oldBed + depth - bed;
			if (wasWet && keptDepth > WetThreshold && vanillaWet) {
				depth = keptDepth;
			} else {
				depth = vanillaDepth;
				velocity = restVelocity;
			}
		}
	}

	if (depth > WetThreshold) {
		float2 position = float2(logical) + 0.5;
		for (uint i = 0; i < EmitterCount; i++) {
			Emitter e = Emitters[i];
			float2 d = position - e.Position;
			float w = exp(-dot(d, d) / (e.Radius * e.Radius));
			// Outward from the centre, strongest around its edge (a plunge spreading out)
			float distance = length(d);
			float2 radial = e.RadialSpeed > 0.0 ? (d / max(distance, 1e-3)) * e.RadialSpeed * saturate(distance / max(e.Radius * 0.5, 1e-3)) : 0.0;
			velocity = lerp(velocity, e.Velocity + radial, w * saturate(DeltaTime * 8.0));
			depth += e.DepthRate * w * DeltaTime;
		}
	}

	if (Vorticity > 0.0 && depth > WetThreshold && kept) {
		// Symmetric flow past an obstacle does not shed vortices on its own; real rivers always carry
		// small disturbances. Nudge fast water beside dry cells sideways, varying in time and space.
		float speed = length(velocity);
		bool besideObstacle = LoadDepth(DepthIn, logical + int2(1, 0)) <= WetThreshold || LoadDepth(DepthIn, logical + int2(-1, 0)) <= WetThreshold ||
		                      LoadDepth(DepthIn, logical + int2(0, 1)) <= WetThreshold || LoadDepth(DepthIn, logical + int2(0, -1)) <= WetThreshold;
		// Swirl reinforcement only in open water: beside jagged edges it made the flow flicker
		if (!besideObstacle)
			velocity += ConfinementForce(logical) * DeltaTime;
		// Stir moving water, in proportion to the river's speed here
		float riverSpeed = length(restVelocity);
		if (riverSpeed > 1.0)
			velocity += StirVelocity(worldPos) * riverSpeed * STIR_STRENGTH * Vorticity * DeltaTime;
		// (No per-cell shedding nudge: random per cell, it rang shores at the grid's own frequency.
		// The smooth stirring above keeps the flow shedding behind obstacles.)
	}

	velocity = lerp(velocity, restVelocity, saturate(CurrentRelax * DeltaTime));
	if (inFall && !vanillaWet) {
		// Falling water off vanilla's planes is the simulation's own: it runs on by gravity and only
		// thin films drain away, instead of being pulled back to dry
		depth = max(depth - FALL_DRAIN * DeltaTime, 0.0);
	} else {
		depth = lerp(depth, vanillaDepth, saturate(LevelRelax * DeltaTime));
	}

	// Absorbing border: the pull toward calm vanilla water grows quadratically toward the edge and is
	// a per-second rate, so waves fade into it instead of bouncing off an abrupt change
	float edgeWeight = EdgeWeight(logical);
	float edge = saturate(edgeWeight * edgeWeight * EdgeRelax * DeltaTime);
	velocity = lerp(velocity, restVelocity, edge);
	depth = lerp(depth, vanillaDepth, edge);

	DepthOut[id] = max(depth, 0.0);
	// Damping calms waves and stray motion, not the river itself: only the deviation decays
	VelocityOut[id] = restVelocity + (velocity - restVelocity) * VelocityDecay;
}
