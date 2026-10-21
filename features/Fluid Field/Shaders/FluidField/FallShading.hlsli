// D159: the see-through falls' own pixel logic, shared by FallSurface.hlsl (our port of the river's look) and Water.hlsl's
// FLUID_FIELD_FALLS variant (the river's own shader drawing the falls): what of a sheet shows (the inner side over another
// sheet D100/D103, the front surface D125/D126, the seam, cut and lip fades, the contact with what lies behind), and the
// falls' white water (the cut, seam, meet and contact lines, D121-D146) over the bubble pattern of their own normal map.
// Needs Common/FrameBuffer.hlsli and Common/SharedData.hlsli included first.

#ifndef FLUID_FIELD_FALL_SHADING_HLSLI
#define FLUID_FIELD_FALL_SHADING_HLSLI

#include "FluidField/FallGround.hlsli"

namespace FallShading
{
	/** D125: world units behind the falls' front surface that still count as the front itself. */
	static const float FRONT_EPSILON = 2.0;
	/** D129: world units one repeat of the falls' texture (their flow coordinates) spans, for the water's normal maps' scale. */
	static const float FALLS_UV_WORLD = 512.0;
	/** D174: world units over the water below over which the falls foam where they enter it. */
	static const float WATER_FOAM_REACH = 64.0;
	/** D174: world units under the water below over which the falls' foam fades out. */
	static const float WATER_FOAM_SINK = 8.0;
	/** D141: world units below the water a fall pours out of over which the sheet fades in: the river runs on into the falling water, no seam. */
	static const float LIP_BLEND = 24.0;
	/** D143: world units below the lip's blend over which the other foam (seams, sheets meeting, rock contact) comes in. */
	static const float LIP_QUIET = 48.0;
	/** D145: seconds of flow on a sheet before its seams, meets, cuts and contacts may foam (twice this to full). */
	static const float CREST_QUIET_SECONDS = 0.3;
	/** D147: world units from what lies behind (rock, ground, any mesh) over which the contact foams, soft as the river's shore. */
	static const float CONTACT_FOAM_REACH = 64.0;
	/** D128, D174: world units from the next sheet behind over which the water foams where two meet (their overlap, D173). */
	static const float MEET_FOAM_REACH = 96.0;
	/** D174: the river's foam pattern (WaterNormal.hlsli, FluidField::GetFoamCover): world units a tile of it covers, the seconds
	 * between its two phases' restarts and the fastest it drifts (the river's) */
	static const float FOAM_TILE = 160.0;
	static const float FOAM_PERIOD = 0.6;
	static const float FOAM_MAX_DRIFT = 120.0;
	/** D174: the fastest the foam's pattern drifts on level water (along the bake's flow, whose texel-to-texel changes it must not open). */
	static const float FOAM_LEVEL_DRIFT = 40.0;
	/** D127: how far aerated water's colour goes toward the river's foam colour. */
	static const float AERATION_LIFT = 0.06;
	/** The brightest the falls add: the scene is HDR, but one runaway value is a square after bloom and the upscalers. */
	static const float MAX_OUTPUT = 64.0;
	/** D139: the "none" distances (FallGround::NO_WATER) are held to this before interpolation; above NONE_ABOVE reads as none. */
	static const float NONE_HELD = 512.0;
	static const float NONE_ABOVE = 500.0;

	/** a_vector normalised, or a_fallback where it has no length (a zero vector normalises to NaN, which bloom spreads into squares). */
	float3 SafeNormalize(float3 a_vector, float3 a_fallback)
	{
		const float lengthSquared = dot(a_vector, a_vector);
		return lengthSquared > 1e-12 ? a_vector * rsqrt(lengthSquared) : a_fallback;
	}

	/**
	 * D130: the repeats of a texture a_worldScale units across per repeat of the falls' flow coordinates (FALLS_UV_WORLD),
	 * a whole number of them along v: the flow's scroll phase wraps at 1, so a fractional count there jumped once a cycle
	 */
	float2 FlowScale(float a_worldScale)
	{
		const float repeats = FALLS_UV_WORLD / max(a_worldScale, 1.0);
		return float2(repeats, max(round(repeats), 1.0));
	}

	/** D127: how much of the falls' own normal map tilts the surface (it is drawn for an opaque, diffuse sheet: high contrast). */
	static const float NORMAL_STRENGTH = 0.8;

	/**
	 * D174: a tangent-space normal (x across the sheet, y down it, z along a_normal) in world axes, the frame from the sheet's
	 * own normal alone. PerturbNormal's frame, from the screen derivatives of the flow coordinates, is constant over each
	 * triangle (the coordinates are interpolated linearly) and turned at every edge: the detail normal with it, hard-edged facets
	 * in the reflection (SS 1203). Along the bake's flow instead, its direction changing from texel to texel did the same.
	 */
	float3 SheetNormal(float3 a_normal, float3 a_tangentNormal)
	{
		float3 down = float3(0.0, 0.0, -1.0) - a_normal * dot(float3(0.0, 0.0, -1.0), a_normal);
		down = length(down) > 0.1 ? normalize(down) : float3(0.0, 1.0, 0.0);
		const float3 across = normalize(cross(a_normal, down));
		return SafeNormalize(across * a_tangentNormal.x + down * a_tangentNormal.y + a_normal * a_tangentNormal.z, a_normal);
	}

	/**
	 * A tangent-space normal (x, y along a_uv's axes, z along a_normal) in world axes, the frame from the screen derivatives
	 * of a_position and a_uv (no tangents needed). Pixel shaders only.
	 */
	float3 PerturbNormal(float3 a_normal, float3 a_position, float2 a_uv, float3 a_tangentNormal)
	{
		const float3 dp1 = ddx(a_position);
		const float3 dp2 = ddy(a_position);
		const float2 duv1 = ddx(a_uv);
		const float2 duv2 = ddy(a_uv);
		const float3 dp2perp = cross(dp2, a_normal);
		const float3 dp1perp = cross(a_normal, dp1);
		const float3 t = dp2perp * duv1.x + dp1perp * duv2.x;
		const float3 b = dp2perp * duv1.y + dp1perp * duv2.y;
		const float scale = rsqrt(max(max(dot(t, t), dot(b, b)), 1e-20));
		return SafeNormalize(t * scale * a_tangentNormal.x + b * scale * a_tangentNormal.y + a_normal * a_tangentNormal.z, a_normal);
	}

	/** D100, D103: seen from inside its curl where another sheet's outer face lies behind that inner side: not drawn. */
	bool HiddenInnerSide(float4 a_outward, float3 a_toCamera)
	{
		return a_outward.w > 0.5 && dot(SafeNormalize(a_outward.xyz, a_toCamera), a_toCamera) < -0.05;
	}

	/** D125, D126: a fragment behind the falls' front surface (linear depths, world units): the same body of water, hidden. */
	bool BehindFront(float a_depth, float a_frontDepth)
	{
		return a_depth - a_frontDepth > FRONT_EPSILON;
	}

	/** D59: how aerated the water is after running for a_runTime seconds (0 to 1). */
	float Aerated(float a_runTime)
	{
		return 1.0 - exp(-a_runTime / FallGround::AERATION_SECONDS);
	}

	/**
	 * D141, D143, D145: how much the lines may foam: nothing within the lip's blend and quiet below the water a fall pours out
	 * of (a_belowSource, held; NONE_ABOVE and over: none), and for a sheet's first seconds of flow (its crest).
	 */
	float Lip(float a_belowSource, float a_runTime)
	{
		const bool hasSource = a_belowSource < NONE_ABOVE;
		return (hasSource ? saturate((a_belowSource - LIP_BLEND) / LIP_QUIET) : 1.0) * saturate((a_runTime - CREST_QUIET_SECONDS) / CREST_QUIET_SECONDS);
	}

	/**
	 * D174: how strongly the falls foam here (0 to 1), wherever they touch something: the next sheet just behind
	 * (a_sheetGap, world units to the falls' second depth layer), rock, ground or any mesh just behind (a_sceneGap) and the
	 * water they pour into (a_aboveWater, FallGround::Fade's y). Per pixel: the per-vertex cut, seam and meeting distances
	 * drew it in the triangles' shape (white wedges, SS 1150-1155). Not on the lip (a_lip: the crest stays glassy, D145).
	 */
	float ContactStrength(float a_sheetGap, float a_sceneGap, float a_aboveWater, float a_lip)
	{
		if (!FallGround::Whitening)
			return 0.0;
		float contact = 1.0 - saturate(a_sheetGap / MEET_FOAM_REACH);
		contact = max(contact, 1.0 - saturate(a_sceneGap / CONTACT_FOAM_REACH));
		contact = max(contact, 1.0 - saturate(max(a_aboveWater, 0.0) / WATER_FOAM_REACH));
		// Falling off as the river's simulated foam does, a narrow solid core and a wide tail broken into the pattern's
		// bubbles: linear over a short reach, it drew a solid white outline
		// Stops at the water below (a_aboveWater is negative under its surface): seen through the water, foam under it read
		// as floating behind the surface
		return contact * contact * a_lip * saturate(1.0 + a_aboveWater / WATER_FOAM_SINK);
	}

	/** D174: the river's foam pattern on one fixed world plane (its axes a_u, a_v), two phases drifting (a_drift, world units a second along them) and restarting. */
	float FoamPlane(Texture2D<float4> a_tex, SamplerState a_sampler, float3 a_world, float3 a_u, float3 a_v, float2 a_drift, float a_time, float a_mipBias)
	{
		const float2 p = float2(dot(a_world, a_u), dot(a_world, a_v));
		// Each patch restarts at its own time (as the river's, D68: one clock pulsed the whole foam)
		const float2 patch = p / (FOAM_TILE * 2.5);
		const float2 cell = floor(patch);
		const float2 f = patch - cell;
		const float2 s = f * f * (3.0 - 2.0 * f);
		const float4 corner = frac(sin(float4(dot(cell, float2(127.1, 311.7)), dot(cell + float2(1, 0), float2(127.1, 311.7)), dot(cell + float2(0, 1), float2(127.1, 311.7)),
									   dot(cell + float2(1, 1), float2(127.1, 311.7)))) *
								   43758.5453);
		const float offset = lerp(lerp(corner.x, corner.y, s.x), lerp(corner.z, corner.w, s.x), s.y);
		const float time = a_time / FOAM_PERIOD + offset * 2.0;
		const float phase0 = frac(time);
		const float phase1 = frac(time + 0.5);
		const float2 drift = a_drift * FOAM_PERIOD / FOAM_TILE;
		const float2 uv = p / FOAM_TILE;
		const float p0 = length(a_tex.SampleBias(a_sampler, uv - drift * phase0, a_mipBias).xy * 2.0 - 1.0);
		const float p1 = length(a_tex.SampleBias(a_sampler, uv - drift * phase1 + 0.37, a_mipBias).xy * 2.0 - 1.0);
		return saturate(lerp(p0, p1, abs(phase0 * 2.0 - 1.0)) * 1.4);
	}

	/**
	 * D174: the river's foam pattern (FluidField::GetFoamCover's look: the water type's normal map read as bubbles and
	 * streaks, a tile in world units, two phases drifting on the flow and restarting often) on the sheet, from three fixed world
	 * planes blended by the sheet's normal (a_normal). Fixed axes: laid on the sheet's own axes (from its per-pixel normal) and
	 * world positions ~1e5 units out, a hundredth of a degree between two triangles' normals moved the pattern tens of units, a
	 * lattice of the triangles (SS 1172, 1182). The drift: down steep water at FOAM_MAX_DRIFT, along the bake's flow (a_velocity)
	 * on level water at FOAM_LEVEL_DRIFT at most. Dragged by the bake's velocity at full speed, its small changes from one texel to
	 * the next opened the pattern into a larger lattice on the bake's grid (SS 1191-1193).
	 */
	float FoamPattern(Texture2D<float4> a_tex, SamplerState a_sampler, float3 a_world, float3 a_normal, float3 a_velocity, float a_time, float a_mipBias)
	{
		float3 w = pow(abs(a_normal), 4.0);
		w /= max(w.x + w.y + w.z, 1e-4);
		float2 level = a_velocity.xy;
		level *= min(1.0, FOAM_LEVEL_DRIFT / max(length(level), 1e-3));
		const float2 fall = float2(0.0, FOAM_MAX_DRIFT);  // along v, down
		float pattern = 0.0;
		[branch] if (w.z > 0.01)
			pattern += w.z * FoamPlane(a_tex, a_sampler, a_world, float3(1, 0, 0), float3(0, 1, 0), level, a_time, a_mipBias);
		[branch] if (w.x > 0.01)
			pattern += w.x * FoamPlane(a_tex, a_sampler, a_world + 0.31 * FOAM_TILE, float3(0, 1, 0), float3(0, 0, -1), fall, a_time, a_mipBias);
		[branch] if (w.y > 0.01)
			pattern += w.y * FoamPlane(a_tex, a_sampler, a_world + 0.67 * FOAM_TILE, float3(1, 0, 0), float3(0, 0, -1), fall, a_time, a_mipBias);
		return pattern;
	}

	/** D174: how much of the pixel the foam covers, as the river's (dense foam solid, thin foam only in the pattern's bubbles). */
	float FoamCover(float a_contact, float a_pattern)
	{
		const float threshold = a_pattern * 0.5;
		return a_contact > 0.02 ? smoothstep(threshold, threshold + 0.25, a_contact) * 0.95 : 0.0;
	}

	/**
	 * D67, D99: how much of the pixel shows: faded at seams over a mesh leading it, where it meets what lies behind
	 * (a_behind; foam keeps that contact soft, only shorter) and toward its cut. The same factors PSDepth discards by (the
	 * front-surface pre-pass): anything that fades a pixel here must be discarded there too. D141's fade-in under the lip
	 * is gone (D158): the pre-pass still claimed those pixels, which punched holes along the crests.
	 */
	float Alpha(float a_seamFade, float a_behind, float a_foam, float a_clip)
	{
		const float contact = saturate(a_behind / 4.0);
		return saturate(a_seamFade) * lerp(contact, sqrt(contact), a_foam) * FallGround::CutFade(a_clip);
	}

	/**
	 * D108, D110: Waterfall Shading debug "Depth Behind": the open water or air behind the sheet in bands of colour: red under
	 * 50 units, yellow to 150, green to 300, blue to 600, magenta at the reach (nothing hit).
	 */
	float3 DepthBehindBand(float a_behind)
	{
		return a_behind >= 599.0 ? float3(1.0, 0.0, 1.0) : a_behind >= 300.0 ? float3(0.0, 0.2, 1.0) :
		                                               a_behind >= 150.0     ? float3(0.0, 1.0, 0.0) :
		                                               a_behind >= 50.0      ? float3(1.0, 1.0, 0.0) :
		                                                                       float3(1.0, 0.0, 0.0);
	}

	/** D155: the screen-space reflection march: steps, the first step and its growth (world units), and how thick a surface is. */
	static const int SSR_STEPS = 24;
	static const float SSR_FIRST_STEP = 8.0;
	static const float SSR_GROWTH = 1.18;
	static const float SSR_THICKNESS = 48.0;

	/**
	 * D155: the scene's colour (a_color, sampled with a_sampler) where a reflected ray from a_position (camera-relative) along
	 * a_direction first passes behind the scene's depth (a_depth, the device depth in x), and how sure that is (a: 0 where it
	 * leaves the screen, goes too far or meets nothing). a_dimensions: the textures' size; a_region: the rendered part.
	 */
	float4 ScreenReflection(Texture2D<float4> a_color, Texture2D<float4> a_depth, SamplerState a_sampler, float3 a_position, float3 a_direction, float2 a_dimensions, float2 a_region)
	{
		float3 p = a_position;
		float step = SSR_FIRST_STEP;
		[loop] for (int i = 0; i < SSR_STEPS; i++)
		{
			p += a_direction * step;
			step *= SSR_GROWTH;
			const float4 clip = mul(FrameBuffer::CameraViewProj, float4(p, 1.0));
			if (clip.w <= 1.0)
				break;
			const float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
			if (any(uv <= 0.0) || any(uv >= 1.0))
				break;
			const float2 pixel = uv * a_region;
			const float sceneDepth = SharedData::GetScreenDepth(a_depth.Load(int3(pixel, 0)).x);
			const float behind = clip.w - sceneDepth;
			if (behind > 0.0 && behind < SSR_THICKNESS + step) {
				const float2 edge = min(uv, 1.0 - uv);
				const float fade = saturate(min(edge.x, edge.y) * 10.0) * (1.0 - float(i) / float(SSR_STEPS));
				return float4(a_color.SampleLevel(a_sampler, pixel / a_dimensions, 0).rgb, fade);
			}
		}
		return 0.0;
	}
}

#endif  // FLUID_FIELD_FALL_SHADING_HLSLI
