// Fluid Field's simulated water in the vanilla water shader (D8 step 2, D17).
//
// Two water levels: the near level (full detail around the player) at t66/t67 and the far level
// (coarser, wider) at t68/t69. Each surface texture is the solver's output in toroidal storage:
// world cell c lives in texel c & (GridRes - 1). A texel holds the surface slopes (world units per
// unit), the height above the vanilla surface, and a coverage that is 0 on dry cells and fades to
// 0 toward the level's edge. The near level fades into the far one, and the far one into vanilla.

namespace FluidField
{
	Texture2D<float4> WaterSurfaceTex : register(t66);
	Texture2D<float4> WaterFlowTex : register(t67);
	Texture2D<float4> FarSurfaceTex : register(t68);
	Texture2D<float4> FarFlowTex : register(t69);

	/** World units one tile of the flow-carried detail normals covers. */
	static const float FLOW_DETAIL_TILE = 384.0;
	/** Seconds between restarts of each of the two detail phases: short, so the detail never stretches far. */
	static const float FLOW_DETAIL_PERIOD = 0.6;
	/** Fastest the flow-carried detail drifts, world units per second. */
	static const float FLOW_DETAIL_MAX_DRIFT = 120.0;
	/** Flow speed (world units per second) at which the detail reaches full strength. */
	static const float FLOW_DETAIL_FULL_SPEED = 160.0;
	/** Fraction of the near level's width over which it hands over to the far level (as WaterOutputCS fades). */
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

	/** A world position in a level's cell coordinates (texel centres at whole numbers); false outside it. */
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

	float4 BilinearFlow(Texture2D<float4> a_tex, Level a_level, float2 a_cellPosition)
	{
		const int mask = a_level.GridRes - 1;
		const int2 c = int2(floor(a_cellPosition));
		const float2 t = a_cellPosition - float2(c);
		return lerp(lerp(a_tex.Load(int3(c & mask, 0)), a_tex.Load(int3((c + int2(1, 0)) & mask, 0)), t.x),
			lerp(a_tex.Load(int3((c + int2(0, 1)) & mask, 0)), a_tex.Load(int3((c + int2(1, 1)) & mask, 0)), t.x), t.y);
	}

	/** How much the near level (rather than the far one) decides the water here: 1 inside, 0 past its edge band. */
	float NearWeight(Level a_near, float2 a_worldXY)
	{
		const float2 logical = a_worldXY / a_near.CellSize - float2(a_near.GridMin);
		const float fromEdge = min(min(logical.x, logical.y), min(a_near.GridRes - logical.x, a_near.GridRes - logical.y));
		return smoothstep(0.0, 1.0, fromEdge / (NEAR_FADE * a_near.GridRes));
	}

	/**
	 * The simulated surface at an absolute world position: the near level where it is in charge, the
	 * far one beyond, cross-faded over the near level's edge band. False outside both levels.
	 */
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
		// Eased in after a (re)start: slopes, height and coverage all grow from vanilla's flat water
		a_surface = lerp(farSurface, nearSurface, weight) * SharedData::fluidFieldSettings.Fade;
		return true;
	}

	/** Flow (xy, world units per second) and foam (z) at a world position, blended between levels like the surface. */
	float4 FlowFoamAtWorld(float2 a_worldXY)
	{
		const Level nearLevel = NearLevel();
		const Level farLevel = FarLevel();
		float2 nearCell, farCell;
		const bool inNear = ToCell(nearLevel, a_worldXY, nearCell);
		const bool inFar = ToCell(farLevel, a_worldXY, farCell);
		const float4 nearFlow = inNear ? BilinearFlow(WaterFlowTex, nearLevel, nearCell) : 0;
		const float4 farFlow = inFar ? BilinearFlow(FarFlowTex, farLevel, farCell) : 0;
		const float weight = inNear ? (inFar ? NearWeight(nearLevel, a_worldXY) : 1.0) : 0.0;
		return lerp(farFlow, nearFlow, weight) * SharedData::fluidFieldSettings.Fade;
	}

	/** The simulated flow at a world position, world units per second. */
	float2 FlowAtWorld(float2 a_worldXY)
	{
		return FlowFoamAtWorld(a_worldXY).xy;
	}

	/** World units one flowmap texel covers (64 texels per 4096-unit cell). */
	static const float FLOWMAP_TEXEL = 64.0;
	/** Simulated flow speed (world units per second) at which vanilla's flow normals scroll at full speed. */
	static const float FLOWMAP_FULL_SPEED = 150.0;

	/**
	 * Vanilla's flowmap sample with its direction (RG, world axes as * 2 - 1) and scroll speed (A)
	 * taken from the simulated flow where Fluid Field covers the water (D20). The flowmap is a
	 * fixed, generated field: it can point straight out of an island or along a lake that is still,
	 * while the simulated flow goes around obstacles and stops where the water spreads out.
	 */
	float4 SteerFlowmap(float4 a_color, float3 a_worldPosition, float2 a_uvShift)
	{
		float4 surface;
		if (!SampleSurface(a_worldPosition, surface))
			return a_color;
		const float coverage = saturate(surface.w);
		// One direction per flowmap texel (64 world units), taken at that texel's centre like the
		// flowmap itself: vanilla's four-tap blend hides the steps between texels, but a direction
		// changing within a texel twists its texture and the ripples stretch
		const float2 shift = float2(sign(a_uvShift.x), -sign(a_uvShift.y)) * (0.5 * FLOWMAP_TEXEL);
		const float2 texelCentre = (floor((a_worldPosition.xy + shift) / FLOWMAP_TEXEL) + 0.5) * FLOWMAP_TEXEL;
		const float2 flow = FlowAtWorld(texelCentre);
		const float speed = length(flow);
		const float2 direction = flow / max(speed, 1e-3);
		a_color.xy = lerp(a_color.xy, direction * 0.5 + 0.5, coverage * smoothstep(2.0, 15.0, speed));
		a_color.w = lerp(a_color.w, saturate(speed / FLOWMAP_FULL_SPEED), coverage);
		return a_color;
	}

	/** How much vanilla's flow normals show (1) rather than its still-water ripples (0): none on still simulated water. */
	float FlowmapWeight(float3 a_worldPosition)
	{
		float4 surface;
		if (!SampleSurface(a_worldPosition, surface))
			return 1.0;
		return lerp(1.0, smoothstep(5.0, 40.0, length(FlowAtWorld(a_worldPosition.xy))), saturate(surface.w));
	}

	/**
	 * The simulated flow smoothed over about nine near cells, for dragging detail along: the raw
	 * flow twists the texture in tight eddies, which reads as an oil film rather than water.
	 */
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

	/**
	 * How fast the simulated flow changes across a near cell (1/s): high where water is forced around
	 * a pillar or rock, which is where real water breaks up instead of flowing smoothly.
	 */
	float FlowShear(float3 a_worldPosition)
	{
		const float d = SharedData::fluidFieldSettings.CellSize;
		const float2 dx = FlowAtWorld(a_worldPosition.xy + float2(d, 0)) - FlowAtWorld(a_worldPosition.xy - float2(d, 0));
		const float2 dy = FlowAtWorld(a_worldPosition.xy + float2(0, d)) - FlowAtWorld(a_worldPosition.xy - float2(0, d));
		return (length(dx) + length(dy)) / (2.0 * d);
	}

	/**
	 * Detail normals carried along by the simulated flow (the classic two-phase flow map): two
	 * samples of the vanilla normal texture, each dragged along the flow and restarted half a period
	 * apart, cross-faded so neither restart shows.
	 */
	float3 FlowDetailNormal(float3 a_worldPosition, float2 a_flow)
	{
		const float time = SharedData::Timer / FLOW_DETAIL_PERIOD;
		const float phase0 = frac(time);
		const float phase1 = frac(time + 0.5);
		const float2 uv = a_worldPosition.xy / FLOW_DETAIL_TILE;
		const float2 flow = a_flow * min(1.0, FLOW_DETAIL_MAX_DRIFT / max(length(a_flow), 1e-3));
		const float2 drift = flow * FLOW_DETAIL_PERIOD / FLOW_DETAIL_TILE;
		const float3 n0 = Normals01Tex.SampleBias(Normals01Sampler, uv - drift * phase0, SharedData::MipBias).xyz * 2.0 - 1.0;
		const float3 n1 = Normals01Tex.SampleBias(Normals01Sampler, uv - drift * phase1 + 0.5, SharedData::MipBias).xyz * 2.0 - 1.0;
		const float w = abs(phase0 * 2.0 - 1.0);  // 1 when phase 0 restarts, so it is hidden then
		// Blended so the ripples keep their strength mid-fade; a plain lerp flattens them twice a period
		const float2 slope = (n0.xy * (1.0 - w) + n1.xy * w) / sqrt(w * w + (1.0 - w) * (1.0 - w));
		return normalize(float3(slope, max(lerp(n0.z, n1.z, w), 0.2)));
	}

	/** How much Fluid Field simulates the water here: 0 outside both levels, on dry cells and at the outer edge. */
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
	 * How the simulated surface reshapes vanilla's own normals at a world position (D17): a shift
	 * of vanilla's normal-texture lookups (world units) toward the downslope, so its ripple pattern
	 * bends and compresses around wakes, eddies and standing waves, and an extra roughness where
	 * the surface is steep or the flow shears. Both fade with coverage.
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
		// Rougher where the surface is steep, and where the flow shears around obstacles (up to x2.5)
		a_roughness = min(saturate(length(surface.xy) * ROUGHNESS_PER_SLOPE) + saturate(FlowShear(a_worldPosition) * ROUGHNESS_PER_SHEAR), 1.5) * coverage;
	}

	/** Surface roughness at the pixel being shaded (0 calm to 1.5), set while its normal is built, for the sun glint. */
	static float PixelRoughness = 0.0;

	/** Most extra vanilla ripple amplitude, as a fraction, on the roughest water seen from above. */
	static const float MAX_RIPPLE_BOOST = 0.6;

	/**
	 * How much vanilla's ripple amplitude is multiplied by on rough simulated water. Seen at a
	 * grazing angle, steeper ripples tip reflections below the horizon, where they pick up the dark
	 * lower half of the sky and the water turns black, so the boost fades out toward the horizon.
	 * a_viewDirection points from the camera to the surface.
	 */
	float RippleBoost(float a_roughness, float3 a_viewDirection)
	{
		const float lookingDown = saturate(-a_viewDirection.z * 3.0);
		return 1.0 + saturate(a_roughness / 1.5) * MAX_RIPPLE_BOOST * lookingDown;
	}

	/**
	 * Tilts a water normal back toward up just enough that its reflection stays above the horizon
	 * (at least half as high as flat water would reflect), where Fluid Field shapes the water. Vanilla's
	 * normals were tuned for its own gentle ripples; with the simulated waves on top, grazing views
	 * otherwise reflect the ground and the water goes black.
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

	/**
	 * Widens the sun's glint on rough water and narrows it on calm water: the specular exponent is
	 * lowered with roughness (to a third at most), and the lobe rescaled so its total energy stays
	 * about the same, as a normalised Phong lobe would.
	 */
	float SunGlint(float a_cosine, float a_exponent)
	{
		const float exponent = a_exponent * lerp(1.0, 0.33, saturate(PixelRoughness / 1.5));
		const float energy = (exponent + 2.0) / max(a_exponent + 2.0, 1e-3);
		return exp2(exponent * log2(a_cosine)) * energy;
	}

	/** Strongest whitening of the water body by bubbles, as a fraction of the foam colour. */
	static const float MAX_AERATION = 0.15;
	/** How far foam's colour goes from the lit water colour toward the scene light (white). */
	static const float FOAM_WHITENESS = 0.1;

	/**
	 * Foam's colour: the lit water colour lifted toward the scene light. Foam takes on the water's
	 * hue and stays within the scene's brightness range; pure lit white blows out the exposure.
	 * a_waterLit is the water colour under the surface light, a_light the scene light itself.
	 */
	float3 FoamColor(float3 a_waterLit, float3 a_light)
	{
		return lerp(a_waterLit, a_light, FOAM_WHITENESS);
	}

	/**
	 * Fast, foamy water is full of bubbles that scatter light, so rapids look lighter and milkier than
	 * calm pools: blends a water colour toward the foam colour by foam and flow speed.
	 */
	void ApplyAeration(inout float3 a_color, float3 a_worldPosition, float3 a_foamColor, float a_amount)
	{
		const float4 flowFoam = FlowFoamAtWorld(a_worldPosition.xy);
		const float aeration = saturate(flowFoam.z * 0.6 + length(flowFoam.xy) / 600.0) * MAX_AERATION * a_amount;
		a_color = lerp(a_color, a_foamColor, aeration);
	}

	/**
	 * How much foam dulls the surface's reflection at a world position (0 to 1): foam is a rough,
	 * matte layer, so the sun's glint and the mirror reflection fade where it is dense.
	 */
	float FoamMatte(float3 a_worldPosition)
	{
		return saturate(FlowFoamAtWorld(a_worldPosition.xy).z * 1.5);
	}

	/** World units one tile of the foam's bubbly breakup pattern covers. */
	static const float FOAM_TILE = 160.0;
	/** Seconds between restarts of each of the foam pattern's two phases: short, so it never stretches far. */
	static const float FOAM_PERIOD = 0.6;
	/** Fastest the foam pattern drifts, world units per second. */
	static const float FOAM_MAX_DRIFT = 120.0;

	/**
	 * How white the water is at a world position (0 to 1): the simulated foam, broken into bubbles and
	 * streaks by vanilla's normal texture dragged along the simulated flow (two-phase, like the flow
	 * detail), so foam streams and churns with the water instead of sitting on it. The pattern follows
	 * the smoothed flow, restarts often and drifts at a capped speed: dragged by the raw flow, tight
	 * swirls pull it into long smears.
	 */
	float GetFoamCover(float3 a_worldPosition)
	{
		const float4 flowFoam = FlowFoamAtWorld(a_worldPosition.xy);
		const float foam = flowFoam.z;
		if (foam <= 0.02)
			return 0.0;
		const float time = SharedData::Timer / FOAM_PERIOD;
		const float phase0 = frac(time);
		const float phase1 = frac(time + 0.5);
		const float2 uv = a_worldPosition.xy / FOAM_TILE;
		float2 flow = SampleFlow(a_worldPosition);
		flow *= min(1.0, FOAM_MAX_DRIFT / max(length(flow), 1e-3));
		const float2 drift = flow * FOAM_PERIOD / FOAM_TILE;
		const float p0 = length(Normals01Tex.SampleBias(Normals01Sampler, uv - drift * phase0, SharedData::MipBias).xy * 2.0 - 1.0);
		const float p1 = length(Normals01Tex.SampleBias(Normals01Sampler, uv - drift * phase1 + 0.37, SharedData::MipBias).xy * 2.0 - 1.0);
		// Blended so the pattern keeps its contrast mid-fade; a plain lerp evens it out, and the foam pulses
		const float w = abs(phase0 * 2.0 - 1.0);
		const float mean = 0.5;
		const float pattern = saturate((((p0 - mean) * (1.0 - w) + (p1 - mean) * w) / sqrt(w * w + (1.0 - w) * (1.0 - w)) + mean) * 1.4);
		// Dense foam covers solidly; thin foam only shows in the pattern's bubbles and streaks
		const float threshold = pattern * 0.5;
		return smoothstep(threshold, threshold + 0.25, foam) * 0.95;
	}

	/**
	 * Draws foam (a_foamColor, from FoamColor) over a water colour, covering the reflection and
	 * refraction where the foam is. Returns how much foam covers this pixel.
	 */
	float ApplyFoam(inout float3 a_color, float3 a_worldPosition, float3 a_foamColor)
	{
		const float cover = GetFoamCover(a_worldPosition);
		if (cover > 0.0)
			a_color = lerp(a_color, a_foamColor, cover);
		return cover;
	}

	/**
	 * Draws foam over water that is blended over the scene with a_opacity (the see-through case):
	 * the result must equal the water's blend with foam laid on top, so colour and opacity change
	 * together. Lerping the colour alone is wrong where vanilla has divided the colour by a small
	 * opacity, and shows as bright rims of water colour around rocks.
	 */
	void ApplyFoamBlended(inout float3 a_color, inout float a_opacity, float3 a_worldPosition, float3 a_foamColor)
	{
		const float cover = GetFoamCover(a_worldPosition);
		if (cover <= 0.0)
			return;
		const float opacity = lerp(a_opacity, 1.0, cover);
		a_color = (a_color * (a_opacity * (1.0 - cover)) + a_foamColor * cover) / max(opacity, 1e-3);
		a_opacity = opacity;
	}

	/** Water depth (world units) at which the tint reaches its full strength. */
	static const float TINT_FULL_DEPTH = 256.0;
	/** Most of the see-through view the tint replaces with the water's own colour, at Water Tint 1. */
	static const float MAX_TINT = 0.75;

	/**
	 * Tints all water with its own colour, as it looks from underwater: makes the water body more
	 * opaque (vanilla's refraction weight), more so the deeper it is, so the bed fades into the water
	 * type's colour instead of showing through clear. Shallows near the shore stay clear.
	 */
	float TintedRefractionWeight(float a_refractionMul, float a_waterDepth)
	{
		const float tint = SharedData::fluidFieldSettings.WaterTint * MAX_TINT * saturate(a_waterDepth / TINT_FULL_DEPTH);
		return 1.0 - (1.0 - a_refractionMul) * (1.0 - tint);
	}

	/**
	 * Adds Fluid Field's wave normal to a water normal at an absolute world position.
	 * Returns a_normal unchanged outside the simulated area or where it has no water.
	 */
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
			// Slow water keeps some detail, and the ramp to full is wide, so flow rates blend
			// None on still water (it would freeze a pattern onto lakes and the sea), a base level once
			// the water moves, and a wide ramp to full so flow rates blend
			const float speed = length(flow);
			const float amount = flowDetail * smoothstep(5.0, 40.0, speed) * lerp(0.4, 1.0, smoothstep(0.0, FLOW_DETAIL_FULL_SPEED * 1.5, speed));
			if (amount > 0.0) {
				const float3 detail = FlowDetailNormal(a_worldPosition, flow);
				const float3 detailNormal = normalize(float3(detail.xy * amount, 1.0));
				combined = normalize(ReorientNormal(detailNormal, combined));
			}
		}
		return normalize(lerp(a_normal, combined, coverage));
	}
}
