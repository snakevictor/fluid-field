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
	float2 Position;  // Logical texel coordinates
	float Radius;     // Texels, Gaussian falloff
	float DepthRate;  // World units per second added to depth at the centre
	float2 Velocity;  // Velocity the water near the emitter is pulled toward
	float2 Pad1;
};

Texture2D<float> DepthIn : register(t0);
Texture2D<float2> VelocityIn : register(t1);
Texture2D<float2> CellMapIn : register(t2);
StructuredBuffer<Emitter> Emitters : register(t3);

static const float BED_CHANGE_THRESHOLD = 2.0;

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

/** @brief Fast water next to a dry cell or obstacle starts to shed eddies from small sideways nudges. */
static const float SHEDDING_MIN_SPEED = 30.0;
static const float SHEDDING_STRENGTH = 0.6;
static const float SHEDDING_RATE = 3.0;  // Nudge changes per second

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
	float vanillaDepth = HasWater(map.y) ? max(map.y - bed, 0.0) : 0.0;
	CellMapOut[id] = float2(bed, vanillaDepth);

	bool vanillaWet = vanillaDepth > WetThreshold;
	float2 restVelocity = vanillaWet ? (FlowmapEnabled != 0 ? SampleRiverFlow(worldPos) : Current) : 0.0;

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
			velocity = lerp(velocity, e.Velocity, w * saturate(DeltaTime * 8.0));
			depth += e.DepthRate * w * DeltaTime;
		}
	}

	if (Vorticity > 0.0 && depth > WetThreshold && kept) {
		velocity += ConfinementForce(logical) * DeltaTime;

		// Symmetric flow past an obstacle does not shed vortices on its own; real rivers always carry
		// small disturbances. Nudge fast water beside dry cells sideways, varying in time and space.
		float speed = length(velocity);
		bool besideObstacle = LoadDepth(DepthIn, logical + int2(1, 0)) <= WetThreshold || LoadDepth(DepthIn, logical + int2(-1, 0)) <= WetThreshold ||
		                      LoadDepth(DepthIn, logical + int2(0, 1)) <= WetThreshold || LoadDepth(DepthIn, logical + int2(0, -1)) <= WetThreshold;
		if (besideObstacle && speed > SHEDDING_MIN_SPEED) {
			float2 side = float2(-velocity.y, velocity.x) / speed;
			velocity += side * CellNoise(cell, floor(SimTime * SHEDDING_RATE)) * speed * SHEDDING_STRENGTH * Vorticity * DeltaTime;
		}
	}

	velocity = lerp(velocity, restVelocity, saturate(CurrentRelax * DeltaTime));
	depth = lerp(depth, vanillaDepth, saturate(LevelRelax * DeltaTime));

	float edge = EdgeWeight(logical) * EdgeRelax;
	velocity = lerp(velocity, restVelocity, edge);
	depth = lerp(depth, vanillaDepth, edge);

	DepthOut[id] = max(depth, 0.0);
	// Damping calms waves and stray motion, not the river itself: only the deviation decays
	VelocityOut[id] = restVelocity + (velocity - restVelocity) * VelocityDecay;
}
