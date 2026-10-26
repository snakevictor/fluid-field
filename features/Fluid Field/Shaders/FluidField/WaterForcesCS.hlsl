// Fluid Field water, pass 1: refresh the cell map, carry state over (new and reset cells start as vanilla water), apply
// emitters, current, level relaxation and the edge blend to vanilla.
// When a cell's bed changes under live water (late map data), the surface is kept and depth recomputed so it makes no
// waves; a cell that changes between wet and dry restarts as calm vanilla water.

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

static const float BED_CHANGE_THRESHOLD = 2.0;  // World units

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

/** @brief Vorticity confinement (push along N x omega, N up the gradient of |omega|): restores swirl advection smears out. */
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

/** @brief Hash of a lattice point to [-1, 1]. */
float Hash3(int3 p)
{
	uint h = uint(p.x) * 73856093u ^ uint(p.y) * 19349663u ^ uint(p.z) * 83492791u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return float(h & 0xFFFFu) / 32767.5 - 1.0;
}

/** @brief Smooth value noise in 3D (space and time), in [-1, 1]. */
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

/** @brief D170: per-second rate at which water at a lip (full weight) takes the lip's velocity. */
static const float LIP_PULL = 6.0;
/** @brief D170: share of the depth drawn down at a lip's brink, and its cap in world units. */
static const float LIP_DRAWDOWN = 0.3;
static const float LIP_DRAWDOWN_MOST = 16.0;

/** @brief Curl noise from a slowly changing stream function: swirls water without piling it up, so rivers never settle. */
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
		// Symmetric flow past an obstacle sheds no vortices on its own; real rivers always carry small disturbances
		float speed = length(velocity);
		bool besideObstacle = LoadDepth(DepthIn, logical + int2(1, 0)) <= WetThreshold || LoadDepth(DepthIn, logical + int2(-1, 0)) <= WetThreshold ||
		                      LoadDepth(DepthIn, logical + int2(0, 1)) <= WetThreshold || LoadDepth(DepthIn, logical + int2(0, -1)) <= WetThreshold;
		// Confinement only in open water: beside jagged edges it makes the flow flicker
		if (!besideObstacle)
			velocity += ConfinementForce(logical) * DeltaTime;
		// Stir moving water in proportion to the river's speed
		float riverSpeed = length(restVelocity);
		if (riverSpeed > 1.0)
			velocity += StirVelocity(worldPos) * riverSpeed * STIR_STRENGTH * Vorticity * DeltaTime;
		// No per-cell random nudge: it rings shores at the grid frequency; the smooth stirring sheds vortices instead
	}

	velocity = lerp(velocity, restVelocity, saturate(CurrentRelax * DeltaTime));
	// D170: drawdown: water is pulled toward a fall's lip, at least as fast as its texture leaves the crest, harder nearer
	// it. Continuity thins it as it speeds up, and its rest level drops toward the brink (a free overfall passes critical
	// depth, about two thirds of the upstream depth, just before the edge)
	float levelDepth = vanillaDepth;
	if (depth > WetThreshold) {
		const float4 lip = LipAt(worldPos);
		if (lip.z > 1e-3) {
			const float speed = length(lip.xy);
			const float2 way = lip.xy / max(speed, 1e-3);
			// D179: target speed rises from the river's own current to the lip's at the brink; pulling to the lip's speed
			// everywhere it reached sped up whole cascades, since every stretch lies near some lip
			const float own = max(dot(restVelocity, way), 0.0);
			const float2 target = way * max(lerp(own, speed, saturate(lip.z)), dot(velocity, way));
			velocity = lerp(velocity, target, sqrt(saturate(lip.z)) * saturate(LIP_PULL * DeltaTime));
			levelDepth = vanillaDepth - min(vanillaDepth * LIP_DRAWDOWN, LIP_DRAWDOWN_MOST) * saturate(lip.z);
		}
	}
	depth = lerp(depth, levelDepth, saturate(LevelRelax * DeltaTime));

	// Absorbing border: a per-second pull to vanilla growing quadratically toward the edge, so waves fade instead of bouncing
	float edgeWeight = EdgeWeight(logical);
	float edge = saturate(edgeWeight * edgeWeight * EdgeRelax * DeltaTime);
	velocity = lerp(velocity, restVelocity, edge);
	depth = lerp(depth, vanillaDepth, edge);

	DepthOut[id] = max(depth, 0.0);
	// Only the deviation from the river decays, so damping calms waves, not the current
	VelocityOut[id] = restVelocity + (velocity - restVelocity) * VelocityDecay;
}
