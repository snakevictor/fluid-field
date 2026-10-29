// The unified falls' lobe foam on the river's own foam (FluidField::FoamLookAt): FallShading's own functions and values, copied as
// they are (the falls' header needs their constant buffer), run at the unified sheet's scale on a flow coordinate the river sim's flow
// carries (not a bake). Kept apart from the falls: tune the river here.

namespace FoamLook
{

	/** World units one flow-uv unit spans on the unified sheet (its noises' cells are per such unit). */
	static const float UV_WORLD = 968.0;
	/** The river sim's foam (0 to 1) as the lobes' baked aeration: its least foam where theirs starts, its densest past their heavy cover. */
	static const float2 AERATION_FROM_FOAM = float2(0.2, 0.9);

	// FallShading's, as they are
	static const float FALLS_UV_WORLD = 512.0;
	static const float FOAM_TILE = 160.0;
	static const float FOAM_OPACITY = 0.8;
	static const float FOAM_BREAKUP_LEAST = 0.75;
	static const float MORPH_SECONDS = 2.5;
	static const float FOAM_CHURN_SECONDS = 1.6;
	static const float2 UNIFIED_AERATION_DRIVE = float2(0.2, 0.62);
	static const float2 AERATION_HEAVY = float2(0.52, 0.66);
	static const float AERATION_HEAVY_DRIVE = 1.5;
	static const float AERATION_LAYER_OPACITY = 1.06;
	static const float2 AERATION_CLUMP_CELLS = float2(40.0, 12.0);
	static const float AERATION_CLUMP = 0.6;
	static const float2 AERATION_GAP_CELLS = float2(22.0, 14.0);
	static const float AERATION_GAP_PERIOD = 6.0;
	static const float AERATION_GAP_LIFE = 0.35;
	static const float AERATION_GAP_CHANCE = 0.3;
	static const float2 AERATION_GAP_SIZE = float2(0.18, 0.38);
	static const float AERATION_GAP_STRETCH = 1.8;
	static const float AERATION_GAP_DRIFT = 0.8;
	static const float AERATION_GAP_RAGGED = 0.6;
	static const float2 AERATION_GAP_STRENGTH = float2(0.35, 1.0);
	static const float AERATION_GAP_MOTTLE = 0.5;
	static const float2 BILLOW_CELLS = float2(18.0, 23.0);
	static const float BILLOW_BOIL = 1.0;
	static const float2 LUMP_EDGE_CELLS = float2(60.0, 76.0);
	static const float LUMP_EDGE = 0.75;
	static const float LUMP_EDGE_RATE = 2.0;
	static const float2 LUMP_BIG_CELLS = float2(20.0, 25.0);
	static const float LUMP_BIG = 0.5;
	static const float2 LUMP_FROM = float2(0.35, 0.6);
	static const float FOAM_MOUND = 20.0;
	static const float HOLLOW_DARK = 0.45;
	static const float TWO_TONE = 0.55;
	static const float RIM_GLOW = 2.5;
	static const float FOAM_DIFFUSE = 0.55;
	static const float FOAM_WRAP = 1.0;
	static const float FOAM_REFLECT = 0.15;
	static const float FOAM_SSS_BACK = 0.35;
	static const float FOAM_SSS_POWER = 2.0;
	static const float FOAM_SSS_WRAP = 0.9;
	static const float FOAM_SSS_WRAP_LIGHT = 0.1;
	static const float FOAM_SSS_TINT = 0.6;
	static const float FOAM_SSS_THICK = 0.4;
	static const float FOAM_ALBEDO = 0.85;
	static const float FOAM_KNEE = 0.75;

	float2 FlowScale(float a_worldScale)
	{
		const float repeats = FALLS_UV_WORLD / max(a_worldScale, 1.0);
		return float2(repeats, max(round(repeats), 1.0));
	}

	float BubbleHash(float2 a_p)
	{
		a_p = frac(a_p * float2(0.1031, 0.1030));
		a_p += dot(a_p, a_p.yx + 33.33);
		return frac((a_p.x + a_p.y) * a_p.x);
	}

	float RibbonNoise(float2 a_p, float a_period)
	{
		const float2 i = floor(a_p);
		const float2 f = a_p - i;
		const float2 u = f * f * (3.0 - 2.0 * f);
		const float y0 = i.y - a_period * floor(i.y / a_period), y1 = (i.y + 1.0) - a_period * floor((i.y + 1.0) / a_period);
		return lerp(lerp(BubbleHash(float2(i.x, y0)), BubbleHash(float2(i.x + 1.0, y0)), u.x), lerp(BubbleHash(float2(i.x, y1)), BubbleHash(float2(i.x + 1.0, y1)), u.x), u.y);
	}

	float RibbonOctaves(float2 a_flowUv, float2 a_cells, float2 a_offset)
	{
		const float2 p = a_flowUv * a_cells + a_offset;
		return 0.65 * RibbonNoise(p, a_cells.y) + 0.35 * RibbonNoise(p * float2(2.3, 2.0) + float2(5.1, 0.0), a_cells.y * 2.0);
	}

	float Morphing(float2 a_flowUv, float2 a_cells, float a_time)
	{
		const float cycle = a_time / MORPH_SECONDS;
		const float phase0 = frac(cycle);
		const float2 jump0 = float2(BubbleHash(float2(floor(cycle), 3.1)), BubbleHash(float2(floor(cycle), 7.7))) * 97.0;
		const float2 jump1 = float2(BubbleHash(float2(floor(cycle + 0.5), 5.3)), BubbleHash(float2(floor(cycle + 0.5), 1.9))) * 97.0;
		const float a = RibbonOctaves(a_flowUv, a_cells, float2(jump0.x, floor(jump0.y)));
		const float b = RibbonOctaves(a_flowUv, a_cells, float2(jump1.x, floor(jump1.y)));
		const float w = abs(phase0 * 2.0 - 1.0);
		const float blend = lerp(b, a, 1.0 - w);
		return 0.5 + (blend - 0.5) * rsqrt(max(w * w + (1.0 - w) * (1.0 - w), 0.5));
	}

	float AerationGaps(float2 a_flowUv, float a_time)
	{
		float gap = 0.0;
		const float2 p = a_flowUv * AERATION_GAP_CELLS;
		const float2 cell = floor(p);
		[unroll] for (int j = -1; j <= 0; j++)
		{
			const float2 c = cell + float2(0.0, j);
			const float cycle = a_time / AERATION_GAP_PERIOD + BubbleHash(c + 5.3);
			const float t = frac(cycle);
			if (t > AERATION_GAP_LIFE || BubbleHash(c + floor(cycle) * 1.913) > AERATION_GAP_CHANCE)
				continue;
			const float life = t / AERATION_GAP_LIFE;
			const float open = smoothstep(0.0, 0.25, life) * (1.0 - smoothstep(0.55, 0.9, life));
			const float2 centre = c + float2(lerp(0.25, 0.75, BubbleHash(c)), lerp(0.2, 0.5, BubbleHash(c + 7.3)) + life * AERATION_GAP_DRIFT * BubbleHash(c + 2.9));
			const float radius = lerp(AERATION_GAP_SIZE.x, AERATION_GAP_SIZE.y, BubbleHash(c + 3.1)) * open;
			const float2 d = (p - centre) / max(float2(radius, radius * AERATION_GAP_STRETCH), 1e-4);
			const float2 q = (p - centre) + BubbleHash(c + 9.7) * 37.0;
			const float ragged = ((RibbonNoise(q * 5.0, 1e4) - 0.5) * 1.4 + (RibbonNoise(q * 13.0 + 3.3, 1e4) - 0.5) * 0.6) * AERATION_GAP_RAGGED;
			const float shape = open > 0.01 ? 1.0 - smoothstep(0.25, 1.0, length(d) + ragged) : 0.0;
			const float mottle = lerp(1.0 - AERATION_GAP_MOTTLE, 1.0, RibbonNoise(q * 9.0 + 7.1, 1e4));
			gap = max(gap, shape * mottle * lerp(AERATION_GAP_STRENGTH.x, AERATION_GAP_STRENGTH.y, BubbleHash(c + 4.4)));
		}
		return gap;
	}

	float FoamChurn(float a_time, out float2 a_jumpA, out float2 a_jumpB)
	{
		const float cycle = a_time / FOAM_CHURN_SECONDS;
		a_jumpA = float2(BubbleHash(float2(floor(cycle), 4.3)) * 7.0, 0.0);
		a_jumpB = float2(BubbleHash(float2(floor(cycle + 0.5), 9.1)) * 7.0, 0.0);
		return 1.0 - abs(frac(cycle) * 2.0 - 1.0);
	}

	float FlowFoamOnce(Texture2D<float4> a_tex, SamplerState a_sampler, float2 a_uv, float2 a_dx, float2 a_dy)
	{
		const float p0 = length(a_tex.SampleGrad(a_sampler, a_uv, a_dx, a_dy).xy * 2.0 - 1.0);
		const float p1 = length(a_tex.SampleGrad(a_sampler, a_uv * 0.61 + 0.37, a_dx * 0.61, a_dy * 0.61).xy * 2.0 - 1.0);
		return saturate((p0 + p1) * 0.5 * 1.4);
	}

	float FlowFoamPatternGrad(Texture2D<float4> a_tex, SamplerState a_sampler, float2 a_flowUv, float2 a_tilesDx, float2 a_tilesDy, float a_mipBias)
	{
		const float2 tiles = a_flowUv * FlowScale(FOAM_TILE);
		const float phase0 = frac(tiles.y);
		const float phase1 = frac(tiles.y + 0.5);
		const float2 uv0 = float2(tiles.x, phase0 - 0.5);
		const float2 uv1 = float2(tiles.x + 0.43, phase1 - 0.5 + 0.29);
		const float bias = exp2(a_mipBias);
		const float2 dx = a_tilesDx * bias, dy = a_tilesDy * bias;
		const float w = abs(phase0 * 2.0 - 1.0);
		return lerp(FlowFoamOnce(a_tex, a_sampler, uv0, dx, dy), FlowFoamOnce(a_tex, a_sampler, uv1, dx, dy), w);
	}

	float FoamBreakup(Texture2D<float4> a_tex, SamplerState a_sampler, float2 a_flowUv, float a_mipBias)
	{
		const float2 uv = a_flowUv * FlowScale(FOAM_TILE * 4.0) + 0.13;
		const float n = length(a_tex.SampleBias(a_sampler, uv, a_mipBias + 4.0).xy * 2.0 - 1.0) * 1.4;
		return lerp(FOAM_BREAKUP_LEAST, 1.0, smoothstep(0.1, 0.9, n));
	}

	float AerationCover(float a_aeration, float a_pattern)
	{
		const float threshold = a_pattern * 0.6;
		return smoothstep(threshold + 0.15, threshold + 0.75, a_aeration) * FOAM_OPACITY;
	}

	struct Sample
	{
		float cover;   // 0 to 1
		float billow;  // 0 hollow to 1 mound
		float2 slope;  // the billows' slope as the lobes' tilt reads it (per flow-uv unit over FALLS_UV_WORLD), world x and y
		float whole;   // how whole the foam is (its mounds' height share)
	};

	/** The lobes' aeration foam at flow uv a_uv for the aeration a_aeration, as Water.hlsl draws it on a unified sheet. */
	Sample At(Texture2D<float4> a_tex, SamplerState a_sampler, float2 a_uv, float a_aeration, float a_mipBias, float a_time)
	{
		Sample s;
		const float2 tiles = a_uv * FlowScale(FOAM_TILE);
		const float2 tilesDx = ddx(tiles), tilesDy = ddy(tiles);
		// its lumps (the lobes' timed by their streaks: none here, so whole) and billows
		const float boil = a_time * BILLOW_BOIL;
		const float fine = lerp(1.0 - LUMP_EDGE, 1.0 + 0.5 * LUMP_EDGE, Morphing(a_uv, LUMP_EDGE_CELLS, boil * LUMP_EDGE_RATE + 3.1));
		const float big = lerp(1.0 - LUMP_BIG, 1.0 + 0.4 * LUMP_BIG, Morphing(a_uv, LUMP_BIG_CELLS, boil + 8.3));
		const float lumpEdge = lerp(1.0, fine * big, smoothstep(LUMP_FROM.x, LUMP_FROM.y, a_aeration));
		const float raw = Morphing(a_uv, BILLOW_CELLS, boil);
		const float2 step = 0.25 / BILLOW_CELLS;
		s.slope = float2(Morphing(a_uv + float2(step.x, 0.0), BILLOW_CELLS, boil) - raw, Morphing(a_uv + float2(0.0, step.y), BILLOW_CELLS, boil) - raw) / step / FALLS_UV_WORLD;
		s.billow = smoothstep(0.2, 0.8, raw);
		// its drive: heavy cover, clear patches, clumps, breakup
		const float heavy = smoothstep(AERATION_HEAVY.x, AERATION_HEAVY.y, a_aeration);
		const float gap = AerationGaps(a_uv, a_time) * (1.0 - 0.7 * heavy);
		const float clump = lerp(lerp(1.0 - AERATION_CLUMP, 1.0, Morphing(a_uv, AERATION_CLUMP_CELLS, a_time + 5.1)), 1.0, heavy);
		const float drive = smoothstep(UNIFIED_AERATION_DRIVE.x, UNIFIED_AERATION_DRIVE.y, a_aeration) * lerp(1.0, AERATION_HEAVY_DRIVE, heavy) * (1.0 - gap) * clump * lumpEdge *
		                    lerp(FoamBreakup(a_tex, a_sampler, a_uv, a_mipBias), 1.0, heavy);
		// its top layer, its grain churning in place
		float2 jumpA, jumpB;
		const float weight = FoamChurn(a_time, jumpA, jumpB);
		const float coverB = saturate(AerationCover(drive, FlowFoamPatternGrad(a_tex, a_sampler, a_uv + jumpB, tilesDx, tilesDy, a_mipBias)) * AERATION_LAYER_OPACITY);
		const float coverA = saturate(AerationCover(drive, FlowFoamPatternGrad(a_tex, a_sampler, a_uv + jumpA, tilesDx, tilesDy, a_mipBias)) * AERATION_LAYER_OPACITY);
		s.cover = drive > 0.01 ? lerp(coverB, coverA, weight) : 0.0;
		s.whole = saturate(s.cover / FOAM_OPACITY);
		return s;
	}

	/** As the lobes' foam is lit (Water.hlsl's unified block): diffuse round its mounds, hollows, two tone, reflection, light through, ceiling. */
	float3 Light(Sample a_s, float3 a_light, float3 a_normal, float3 a_viewDirection, float3 a_toSun, float3 a_sunLight, float3 a_ambient,
		float3 a_water, float3 a_reflection, float a_fresnel)
	{
		const float3 mound = normalize(a_normal - float3(a_s.slope * FOAM_MOUND * a_s.whole, 0.0));
		const float3 hue = a_water / max(dot(a_water, float3(0.2126, 0.7152, 0.0722)), 1e-3);
		const float sunLum = dot(a_sunLight, float3(0.2126, 0.7152, 0.0722)), ambientLum = dot(a_ambient, float3(0.2126, 0.7152, 0.0722));
		const float through = pow(saturate(dot(a_viewDirection, a_toSun)), FOAM_SSS_POWER) * FOAM_SSS_BACK +
		                      saturate((dot(mound, a_toSun) + FOAM_SSS_WRAP) / (1.0 + FOAM_SSS_WRAP)) * (1.0 - saturate(dot(mound, a_toSun))) * FOAM_SSS_WRAP_LIGHT;
		const float thin = lerp(1.0, FOAM_SSS_THICK, a_s.whole) * (1.0 + RIM_GLOW * (1.0 - a_s.billow));
		const float wrapped = saturate((dot(mound, a_toSun) + FOAM_WRAP) / (1.0 + FOAM_WRAP));
		float3 light = a_light * lerp(1.0, lerp(ambientLum / max(ambientLum + sunLum, 1e-4), 1.0, wrapped), FOAM_DIFFUSE);
		light *= lerp(1.0, HOLLOW_DARK, (1.0 - a_s.billow) * a_s.whole);
		light *= lerp(lerp(1.0, hue, TWO_TONE), 1.0, saturate(mound.z * 0.5 + 0.5));
		light = lerp(light, a_reflection, saturate(a_fresnel * FOAM_REFLECT)) + a_sunLight * lerp(1.0, hue, FOAM_SSS_TINT) * through * thin;
		const float ceiling = FOAM_ALBEDO * (sunLum + ambientLum);
		const float knee = ceiling * FOAM_KNEE;
		const float lum = dot(light, float3(0.2126, 0.7152, 0.0722));
		[flatten] if (lum > knee && ceiling > knee)
			light *= (knee + (ceiling - knee) * (1.0 - exp(-(lum - knee) / (ceiling - knee)))) / lum;
		return light;
	}
}
