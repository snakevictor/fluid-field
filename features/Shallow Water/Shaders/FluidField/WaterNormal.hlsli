// Fluid Field's simulated water in the vanilla water shader (D8 step 2, D17).
// Near level (t66/t67) and coarser far level (t68/t69), stored toroidally: cell c in texel c & (GridRes - 1).
// Surface texel: slopes, height above vanilla, coverage (0 on dry cells, fading to 0 at the level's edge).
// Near fades into far, far into vanilla.

#include "FluidField/FoamLook.hlsli"

namespace FluidField
{
	Texture2D<float4> WaterSurfaceTex : register(t66);
	Texture2D<float4> WaterFlowTex : register(t67);
	Texture2D<float4> FarSurfaceTex : register(t68);
	Texture2D<float4> FarFlowTex : register(t69);

	/** World units one tile of the flow-carried detail normals covers. */
	static const float FLOW_DETAIL_TILE = 384.0;
	/** Seconds between restarts of each detail phase; short so the detail never stretches far. */
	static const float FLOW_DETAIL_PERIOD = 0.6;
	/** Fastest detail drift (world units/s). */
	static const float FLOW_DETAIL_MAX_DRIFT = 120.0;
	/**
	 * D170: fastest detail and foam drift at a fall's lip (flow's w), so the surface speeds up and stretches into the drop.
	 * FallStrands' CREST_SPEED (200) is where the falls' texture takes over.
	 */
	static const float LIP_MAX_DRIFT = 240.0;

	/** D170: the drift cap, raised from FLOW_DETAIL_MAX_DRIFT toward a fall's lip. */
	float MaxDrift(float a_lip)
	{
		return lerp(FLOW_DETAIL_MAX_DRIFT, LIP_MAX_DRIFT, saturate(a_lip));
	}

	/** Flow speed (world units/s) at which the detail reaches full strength. */
	static const float FLOW_DETAIL_FULL_SPEED = 160.0;
	/** Share of the near level's width over which it hands over to the far level (matches WaterOutputCS). */
	static const float NEAR_FADE = 0.15;

	/** One water level's placement on the world. */
	struct Level
	{
		int2 GridMin;
		int GridRes;
		float CellSize;
		bool Enabled;
	};

	Level NearLevel()
	{
		Level level;
		level.GridMin = SharedData::fluidFieldSettings.GridMin;
		level.GridRes = int(SharedData::fluidFieldSettings.GridRes);
		level.CellSize = SharedData::fluidFieldSettings.CellSize;
		level.Enabled = SharedData::fluidFieldSettings.Enabled != 0;
		return level;
	}

	Level FarLevel()
	{
		Level level;
		level.GridMin = SharedData::fluidFieldSettings.FarGridMin;
		level.GridRes = int(SharedData::fluidFieldSettings.FarGridRes);
		level.CellSize = SharedData::fluidFieldSettings.FarCellSize;
		level.Enabled = SharedData::fluidFieldSettings.Enabled != 0 && SharedData::fluidFieldSettings.FarEnabled != 0;
		return level;
	}

	/** A world position in a level's cell coordinates (texel centres at integers); false outside it. */
	bool ToCell(Level a_level, float2 a_worldXY, out float2 a_cellPosition)
	{
		a_cellPosition = a_worldXY / max(a_level.CellSize, 1e-3) - 0.5;
		const int2 logical = int2(floor(a_cellPosition)) - a_level.GridMin;
		return a_level.Enabled && all(logical >= 0) && all(logical < a_level.GridRes - 1);
	}

	float4 BilinearSurface(Texture2D<float4> a_tex, Level a_level, float2 a_cellPosition)
	{
		const int mask = a_level.GridRes - 1;
		const int2 c = int2(floor(a_cellPosition));
		const float2 t = a_cellPosition - float2(c);
		return lerp(lerp(a_tex.Load(int3(c & mask, 0)), a_tex.Load(int3((c + int2(1, 0)) & mask, 0)), t.x),
			lerp(a_tex.Load(int3((c + int2(0, 1)) & mask, 0)), a_tex.Load(int3((c + int2(1, 1)) & mask, 0)), t.x), t.y);
	}

	/**
	 * D176: BilinearFlow with foam (z) carried from wet corners into dry ones; blending toward a dry corner's zero left a
	 * bare strip a cell wide against every wall.
	 */
	float4 BilinearFlowFoamToWalls(Texture2D<float4> a_flow, Texture2D<float4> a_surface, Level a_level, float2 a_cellPosition)
	{
		const int mask = a_level.GridRes - 1;
		const int2 c = int2(floor(a_cellPosition));
		const float2 t = a_cellPosition - float2(c);
		float4 f[4] = { a_flow.Load(int3(c & mask, 0)), a_flow.Load(int3((c + int2(1, 0)) & mask, 0)), a_flow.Load(int3((c + int2(0, 1)) & mask, 0)),
			a_flow.Load(int3((c + int2(1, 1)) & mask, 0)) };
		const float4 wet = float4(a_surface.Load(int3(c & mask, 0)).w, a_surface.Load(int3((c + int2(1, 0)) & mask, 0)).w, a_surface.Load(int3((c + int2(0, 1)) & mask, 0)).w,
			a_surface.Load(int3((c + int2(1, 1)) & mask, 0)).w);
		float wetFoam = 0.0;
		[unroll] for (int k = 0; k < 4; k++)
			wetFoam = max(wetFoam, wet[k] > 0.05 ? f[k].z : 0.0);
		[unroll] for (int k = 0; k < 4; k++)
			f[k]
				.z = lerp(wetFoam, f[k].z, saturate(wet[k] * 4.0));
		return lerp(lerp(f[0], f[1], t.x), lerp(f[2], f[3], t.x), t.y);
	}

	/** How much the near level (rather than the far one) decides the water here: 1 inside, 0 past its edge band. */
	float NearWeight(Level a_near, float2 a_worldXY)
	{
		const float2 logical = a_worldXY / a_near.CellSize - float2(a_near.GridMin);
		const float fromEdge = min(min(logical.x, logical.y), min(a_near.GridRes - logical.x, a_near.GridRes - logical.y));
		return smoothstep(0.0, 1.0, fromEdge / (NEAR_FADE * a_near.GridRes));
	}

	/** The simulated surface at a world position, near level cross-faded into far; false outside both. */
	bool SampleSurface(float3 a_worldPosition, out float4 a_surface)
	{
		a_surface = 0;
		const Level nearLevel = NearLevel();
		const Level farLevel = FarLevel();
		float2 nearCell, farCell;
		const bool inNear = ToCell(nearLevel, a_worldPosition.xy, nearCell);
		const bool inFar = ToCell(farLevel, a_worldPosition.xy, farCell);
		if (!inNear && !inFar)
			return false;
		const float4 nearSurface = inNear ? BilinearSurface(WaterSurfaceTex, nearLevel, nearCell) : 0;
		const float4 farSurface = inFar ? BilinearSurface(FarSurfaceTex, farLevel, farCell) : 0;
		const float weight = inNear ? (inFar ? NearWeight(nearLevel, a_worldPosition.xy) : 1.0) : 0.0;
		// Eased in after a (re)start, growing from vanilla's flat water
		a_surface = lerp(farSurface, nearSurface, weight) * SharedData::fluidFieldSettings.Fade;
		return true;
	}

	/** Flow (xy, world units/s) and foam (z) at a world position, blended between levels like the surface. */
	float4 FlowFoamAtWorld(float2 a_worldXY)
	{
		const Level nearLevel = NearLevel();
		const Level farLevel = FarLevel();
		float2 nearCell, farCell;
		const bool inNear = ToCell(nearLevel, a_worldXY, nearCell);
		const bool inFar = ToCell(farLevel, a_worldXY, farCell);
		const float4 nearFlow = inNear ? BilinearFlowFoamToWalls(WaterFlowTex, WaterSurfaceTex, nearLevel, nearCell) : 0;
		const float4 farFlow = inFar ? BilinearFlowFoamToWalls(FarFlowTex, FarSurfaceTex, farLevel, farCell) : 0;
		const float weight = inNear ? (inFar ? NearWeight(nearLevel, a_worldXY) : 1.0) : 0.0;
		return lerp(farFlow, nearFlow, weight) * SharedData::fluidFieldSettings.Fade;
	}

	/** The simulated flow (world units/s). */
	float2 FlowAtWorld(float2 a_worldXY)
	{
		return FlowFoamAtWorld(a_worldXY).xy;
	}

	/** How much vanilla's flow normals show (1) rather than its still-water ripples (0): none on still simulated water. */
	float FlowmapWeight(float3 a_worldPosition)
	{
		float4 surface;
		if (!SampleSurface(a_worldPosition, surface))
			return 1.0;
		return lerp(1.0, smoothstep(5.0, 40.0, length(FlowAtWorld(a_worldPosition.xy))), saturate(surface.w));
	}

	/** Flow averaged over nine samples within four near cells: dragged by raw flow, detail twists into an oil-film look. */
	float2 SampleFlow(float3 a_worldPosition)
	{
		const float radius = 4.0 * SharedData::fluidFieldSettings.CellSize;
		float2 sum = FlowAtWorld(a_worldPosition.xy);
		[unroll] for (int i = 0; i < 8; i++)
		{
			const float angle = i * 0.785398;
			sum += FlowAtWorld(a_worldPosition.xy + radius * float2(cos(angle), sin(angle)));
		}
		return sum / 9.0;
	}

	/** Extra vanilla ripple amplitude per unit of shear (1/s) in the simulated flow. */
	static const float ROUGHNESS_PER_SHEAR = 0.6;

	/** How fast the flow changes across a near cell (1/s): high around rocks, where real water breaks up. */
	float FlowShear(float3 a_worldPosition)
	{
		const float d = SharedData::fluidFieldSettings.CellSize;
		const float2 dx = FlowAtWorld(a_worldPosition.xy + float2(d, 0)) - FlowAtWorld(a_worldPosition.xy - float2(d, 0));
		const float2 dy = FlowAtWorld(a_worldPosition.xy + float2(0, d)) - FlowAtWorld(a_worldPosition.xy - float2(0, d));
		return (length(dx) + length(dy)) / (2.0 * d);
	}

	/** Detail normals dragged by the flow: classic two-phase flow map, phases half a period apart so no restart shows. */
	float3 FlowDetailNormal(float3 a_worldPosition, float2 a_flow, float a_maxDrift = FLOW_DETAIL_MAX_DRIFT)
	{
		const float time = SharedData::Timer / FLOW_DETAIL_PERIOD;
		const float phase0 = frac(time);
		const float phase1 = frac(time + 0.5);
		const float2 uv = a_worldPosition.xy / FLOW_DETAIL_TILE;
		const float2 flow = a_flow * min(1.0, a_maxDrift / max(length(a_flow), 1e-3));
		const float2 drift = flow * FLOW_DETAIL_PERIOD / FLOW_DETAIL_TILE;
		const float3 n0 = Normals01Tex.SampleBias(Normals01Sampler, uv - drift * phase0, SharedData::MipBias).xyz * 2.0 - 1.0;
		const float3 n1 = Normals01Tex.SampleBias(Normals01Sampler, uv - drift * phase1 + 0.5, SharedData::MipBias).xyz * 2.0 - 1.0;
		const float w = abs(phase0 * 2.0 - 1.0);  // 1 when phase 0 restarts, hiding it
		// Keeps ripple strength mid-fade; a plain lerp flattens them twice a period
		const float2 slope = (n0.xy * (1.0 - w) + n1.xy * w) / sqrt(w * w + (1.0 - w) * (1.0 - w));
		return normalize(float3(slope, max(lerp(n0.z, n1.z, w), 0.2)));
	}

	/** Simulated coverage: 0 outside both levels, on dry cells and at the outer edge. */
	float GetCoverage(float3 a_worldPosition)
	{
		float4 surface;
		return SampleSurface(a_worldPosition, surface) ? saturate(surface.w) : 0.0;
	}

	/** World units vanilla's normal lookups shift per unit of simulated surface slope. */
	static const float WARP_PER_SLOPE = 48.0;
	/** Extra vanilla ripple amplitude per unit of simulated slope. */
	static const float ROUGHNESS_PER_SLOPE = 3.0;

	/**
	 * D17: how the simulated surface reshapes vanilla's normals: shifts its lookups downslope (world units) so ripples bend
	 * around wakes and standing waves, and adds roughness where the surface is steep or the flow shears. Both fade with coverage.
	 */
	void GetVanillaWarp(float3 a_worldPosition, out float2 a_warp, out float a_roughness)
	{
		a_warp = 0;
		a_roughness = 0;
		float4 surface;
		if (!SampleSurface(a_worldPosition, surface))
			return;
		const float coverage = saturate(surface.w);
		a_warp = -surface.xy * WARP_PER_SLOPE * coverage;
		// Rougher where the surface is steep or the flow shears (capped at 1.5)
		a_roughness = min(saturate(length(surface.xy) * ROUGHNESS_PER_SLOPE) + saturate(FlowShear(a_worldPosition) * ROUGHNESS_PER_SHEAR), 1.5) * coverage;
	}

	/** Roughness of the pixel being shaded (0 to 1.5), set while building its normal; used by the sun glint. */
	static float PixelRoughness = 0.0;

	/** Most extra ripple amplitude (fraction) on the roughest water seen from above. */
	static const float MAX_RIPPLE_BOOST = 0.6;

	/**
	 * Multiplier on vanilla's ripple amplitude on rough water. Fades toward grazing views, where steeper ripples reflect
	 * below the horizon and turn the water black. a_viewDirection points from the camera to the surface.
	 */
	float RippleBoost(float a_roughness, float3 a_viewDirection)
	{
		const float lookingDown = saturate(-a_viewDirection.z * 3.0);
		return 1.0 + saturate(a_roughness / 1.5) * MAX_RIPPLE_BOOST * lookingDown;
	}

	/**
	 * Tilts a normal toward up so its reflection stays at least half as high as flat water's; with simulated waves on
	 * vanilla's normals, grazing views otherwise reflect the ground and go black.
	 */
	float3 KeepReflectionAboveHorizon(float3 a_normal, float3 a_viewDirection, float3 a_worldPosition)
	{
		const float coverage = GetCoverage(a_worldPosition);
		if (coverage <= 0.0)
			return a_normal;
		const float flatHeight = -a_viewDirection.z;  // Reflection height off flat water
		const float height = reflect(a_viewDirection, a_normal).z;
		const float wanted = 0.5 * flatHeight;
		const float t = saturate((wanted - height) / max(flatHeight - height, 1e-4));
		return normalize(lerp(a_normal, float3(0, 0, 1), t * coverage));
	}

	/** Widens the sun glint on rough water: exponent down to a third, rescaled to keep energy (normalised Phong). */
	float SunGlint(float a_cosine, float a_exponent)
	{
		const float exponent = a_exponent * lerp(1.0, 0.33, saturate(PixelRoughness / 1.5));
		const float energy = (exponent + 2.0) / max(a_exponent + 2.0, 1e-3);
		return exp2(exponent * log2(a_cosine)) * energy;
	}

	/** Strongest whitening of the water body by bubbles (fraction of the foam colour). */
	static const float MAX_AERATION = 0.15;
	/** How far foam's colour goes from the lit water colour toward the scene light. */
	static const float FOAM_WHITENESS = 0.1;

	/**
	 * Foam's colour: the lit water colour (a_waterLit) lifted toward the scene light (a_light), keeping the water's hue;
	 * pure lit white blows out the exposure.
	 */
	float3 FoamColor(float3 a_waterLit, float3 a_light)
	{
		return lerp(a_waterLit, a_light, FOAM_WHITENESS);
	}

	/** Bubbles make rapids milkier than pools: blends toward the foam colour by foam and flow speed. */
	void ApplyAeration(inout float3 a_color, float3 a_worldPosition, float3 a_foamColor, float a_amount)
	{
		const float4 flowFoam = FlowFoamAtWorld(a_worldPosition.xy);
		const float aeration = saturate(flowFoam.z * 0.6 + length(flowFoam.xy) / 600.0) * MAX_AERATION * a_amount;
		a_color = lerp(a_color, a_foamColor, aeration);
	}

	/** How much foam dulls the glint and reflection (0 to 1): foam is a rough, matte layer. */
	float FoamMatte(float3 a_worldPosition)
	{
		return saturate(FlowFoamAtWorld(a_worldPosition.xy).z * 1.5);
	}

	/** D181: most foam cover (river, falls and pools), slightly see-through. */
	static const float FOAM_OPACITY = 0.8;
	/** World units one tile of the foam's breakup pattern covers. */
	static const float FOAM_TILE = 160.0;
	/** Seconds between restarts of each foam pattern phase; short so it never stretches far. */
	static const float FOAM_PERIOD = 0.6;
	/** Fastest foam pattern drift (world units/s). */
	static const float FOAM_MAX_DRIFT = 120.0;

	/**
	 * Foam cover (0 to 1): simulated foam broken up by vanilla's normal texture dragged two-phase along the smoothed flow
	 * at a capped speed; raw flow's tight swirls smear it.
	 */
	float GetFoamCover(float3 a_worldPosition)
	{
		const float4 flowFoam = FlowFoamAtWorld(a_worldPosition.xy);
		const float foam = flowFoam.z;
		if (foam <= 0.02)
			return 0.0;
		// D68: per-patch restart offset; with one clock the cross-fade thinned all foam at once and it pulsed
		const float2 foamPatch = a_worldPosition.xy / (FOAM_TILE * 2.5);
		const float2 cell = floor(foamPatch);
		const float2 f = foamPatch - cell;
		const float2 s = f * f * (3.0 - 2.0 * f);
		const float4 corner = frac(sin(float4(dot(cell, float2(127.1, 311.7)), dot(cell + float2(1, 0), float2(127.1, 311.7)), dot(cell + float2(0, 1), float2(127.1, 311.7)),
									   dot(cell + float2(1, 1), float2(127.1, 311.7)))) *
								   43758.5453);
		const float offset = lerp(lerp(corner.x, corner.y, s.x), lerp(corner.z, corner.w, s.x), s.y);
		const float time = SharedData::Timer / FOAM_PERIOD + offset * 2.0;
		const float phase0 = frac(time);
		const float phase1 = frac(time + 0.5);
		const float2 uv = a_worldPosition.xy / FOAM_TILE;
		float2 flow = SampleFlow(a_worldPosition);
		flow *= min(1.0, max(FOAM_MAX_DRIFT, MaxDrift(flowFoam.w)) / max(length(flow), 1e-3));  // D170: faster toward a lip
		const float2 drift = flow * FOAM_PERIOD / FOAM_TILE;
		const float p0 = length(Normals01Tex.SampleBias(Normals01Sampler, uv - drift * phase0, SharedData::MipBias).xy * 2.0 - 1.0);
		const float p1 = length(Normals01Tex.SampleBias(Normals01Sampler, uv - drift * phase1 + 0.37, SharedData::MipBias).xy * 2.0 - 1.0);
		const float pattern = saturate(lerp(p0, p1, abs(phase0 * 2.0 - 1.0)) * 1.4);
		// Dense foam covers solidly; thin foam only shows in the pattern's bubbles and streaks
		const float threshold = pattern * 0.5;
		return smoothstep(threshold, threshold + 0.25, foam) * FOAM_OPACITY;
	}

	/**
	 * The river's foam in the falls' lobe foam look (FoamLook): the lobes' foam on a flow coordinate the sim's flow carries (two phases
	 * dragged along it and restarted), its foam as their aeration.
	 */
	FoamLook::Sample FoamLookAt(float3 a_worldPosition)
	{
		FoamLook::Sample none = (FoamLook::Sample)0;
		const float4 flowFoam = FlowFoamAtWorld(a_worldPosition.xy);
		const float foam = flowFoam.z;
		if (foam <= 0.005)
			return none;
		const float2 foamPatch = a_worldPosition.xy / (FOAM_TILE * 2.5);
		const float2 cell = floor(foamPatch);
		const float2 f = foamPatch - cell;
		const float2 s = f * f * (3.0 - 2.0 * f);
		const float4 corner = frac(sin(float4(dot(cell, float2(127.1, 311.7)), dot(cell + float2(1, 0), float2(127.1, 311.7)), dot(cell + float2(0, 1), float2(127.1, 311.7)),
									   dot(cell + float2(1, 1), float2(127.1, 311.7)))) *
								   43758.5453);
		const float offset = lerp(lerp(corner.x, corner.y, s.x), lerp(corner.z, corner.w, s.x), s.y);
		const float time = SharedData::Timer / FOAM_PERIOD + offset * 2.0;
		const float phase0 = frac(time), phase1 = frac(time + 0.5);
		float2 flow = SampleFlow(a_worldPosition);
		flow *= min(1.0, max(FOAM_MAX_DRIFT, MaxDrift(flowFoam.w)) / max(length(flow), 1e-3));
		const float2 drift = flow * FOAM_PERIOD;
		const float aeration = lerp(FoamLook::AERATION_FROM_FOAM.x, FoamLook::AERATION_FROM_FOAM.y, saturate(foam));
		const FoamLook::Sample a = FoamLook::At(Normals01Tex, Normals01Sampler, (a_worldPosition.xy - drift * phase0) / FoamLook::UV_WORLD, aeration, SharedData::MipBias, SharedData::Timer);
		const FoamLook::Sample b = FoamLook::At(Normals01Tex, Normals01Sampler, (a_worldPosition.xy - drift * phase1) / FoamLook::UV_WORLD + 0.37, aeration, SharedData::MipBias, SharedData::Timer);
		const float w = abs(phase0 * 2.0 - 1.0);  // (each phase's weight 0 where it restarts)
		FoamLook::Sample r;
		r.cover = lerp(a.cover, b.cover, w);
		r.billow = lerp(a.billow, b.billow, w);
		r.slope = lerp(a.slope, b.slope, w);
		r.whole = lerp(a.whole, b.whole, w);
		return r;
	}

	/** Draws foam (a_foamColor, from FoamColor) over a water colour. Returns the foam cover. */
	float ApplyFoam(inout float3 a_color, float3 a_worldPosition, float3 a_foamColor)
	{
		const float cover = GetFoamCover(a_worldPosition);
		if (cover > 0.0)
			a_color = lerp(a_color, a_foamColor, cover);
		return cover;
	}

	/** Water depth (world units) at which the tint reaches its full strength. */
	static const float TINT_FULL_DEPTH = 256.0;
	/** Most of the see-through view replaced by the water's colour, at Water Tint 1. */
	static const float MAX_TINT = 0.75;

	/** Raises vanilla's refraction weight with depth so the bed fades into the water's colour; shallows stay clear. */
	float TintedRefractionWeight(float a_refractionMul, float a_waterDepth)
	{
		const float tint = SharedData::fluidFieldSettings.WaterTint * MAX_TINT * saturate(a_waterDepth / TINT_FULL_DEPTH);
		return 1.0 - (1.0 - a_refractionMul) * (1.0 - tint);
	}

	/** Adds the simulated wave normal at a world position; a_normal unchanged where there is no simulated water. */
	float3 ApplyWaterNormal(float3 a_worldPosition, float3 a_normal)
	{
		float4 surface;
		if (!SampleSurface(a_worldPosition, surface))
			return a_normal;
		const float coverage = saturate(surface.w);
		if (coverage <= 0.0)
			return a_normal;

		const float strength = SharedData::fluidFieldSettings.NormalStrength;
		const float3 waveNormal = normalize(float3(-surface.x * strength, -surface.y * strength, 1.0));
		float3 combined = normalize(ReorientNormal(waveNormal, a_normal));

		// Surface detail carried by the simulated flow
		const float flowDetail = SharedData::fluidFieldSettings.FlowDetail;
		if (flowDetail > 0.0) {
			const float2 flow = SampleFlow(a_worldPosition);
			// None on still water (it would freeze a pattern on lakes), a base once moving, a wide ramp so flow rates blend
			const float speed = length(flow);
			const float amount = flowDetail * smoothstep(5.0, 40.0, speed) * lerp(0.4, 1.0, smoothstep(0.0, FLOW_DETAIL_FULL_SPEED * 1.5, speed));
			if (amount > 0.0) {
				const float3 detail = FlowDetailNormal(a_worldPosition, flow, MaxDrift(FlowFoamAtWorld(a_worldPosition.xy).w));  // D170
				const float3 detailNormal = normalize(float3(detail.xy * amount, 1.0));
				combined = normalize(ReorientNormal(detailNormal, combined));
			}
		}
		return normalize(lerp(a_normal, combined, coverage));
	}
}
