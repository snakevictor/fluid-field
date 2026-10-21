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
	/** D127: how much of the white water reaches the foam cover: below 1, even the densest bands stay patchy where the pattern is high, as the river's foam (solid bands read as paint). */
	static const float FOAM_DENSITY = 0.7;
	/** D121: world units from a cut line (FallGround::ClipDistance) over which the water foams. */
	static const float CUT_FOAM_REACH = 24.0;
	/** D141: world units below the water a fall pours out of over which the sheet fades in: the river runs on into the falling water, no seam. */
	static const float LIP_BLEND = 24.0;
	/** D143: world units below the lip's blend over which the other foam (seams, sheets meeting, rock contact) comes in. */
	static const float LIP_QUIET = 48.0;
	/** D145: seconds of flow on a sheet before its seams, meets, cuts and contacts may foam (twice this to full). */
	static const float CREST_QUIET_SECONDS = 0.3;
	/** D147: world units from what lies behind over which the contact foams, soft as the river's shore. */
	static const float CONTACT_FOAM_REACH = 32.0;
	/** D121: how white a seam's overlap and a mesh's contact foam at most. */
	static const float SEAM_FOAM = 0.7;
	static const float CONTACT_FOAM = 0.45;
	/** D128: world units from another sheet over which the water foams where two meet, and how white at most. */
	static const float MEET_FOAM_REACH = 64.0;
	static const float MEET_FOAM = 1.0;
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
	 * D121, D128, D146: the white water along where the falls break: their cut lines (a_clip), seams over a leading mesh
	 * (a_seamFade, FallGround::Fade's x) and where two sheets meet (a_toOtherSheet), only where the water already falls
	 * (steep, from the flow's velocity) and past the lip (a_lip).
	 */
	float LineWhite(float3 a_velocity, float a_clip, float a_seamFade, float a_toOtherSheet, float a_lip)
	{
		if (!FallGround::Whitening)
			return 0.0;
		const float steep = saturate((abs(a_velocity.z) / max(length(a_velocity), 1e-3) - 0.4) / 0.4);
		const float quiet = a_lip * steep;
		float white = (1.0 - saturate(a_clip / CUT_FOAM_REACH)) * quiet;
		white = max(white, (1.0 - saturate(a_seamFade)) * SEAM_FOAM * quiet);
		return max(white, (1.0 - saturate(a_toOtherSheet / MEET_FOAM_REACH)) * MEET_FOAM * quiet);
	}

	/** D121, D147: the white water where the sheet meets rock or any mesh (a_behind: world units to the scene behind it). */
	float ContactWhite(float a_behind, float a_lip)
	{
		return FallGround::Whitening ? (1.0 - saturate(a_behind / CONTACT_FOAM_REACH)) * CONTACT_FOAM * a_lip : 0.0;
	}

	/**
	 * D122, D127, D137: the foam's bubble pattern: two blurred samples of the falls' own normal map running on the flow, each
	 * a whole number of repeats along the scroll; FallGround's lace where there is no normal map.
	 */
	float FoamPattern(Texture2D<float4> a_normalTex, SamplerState a_sampler, bool a_hasNormal, FallGround::FlowSample a_flow)
	{
		float pattern = FallGround::Lace(a_flow);
		[branch] if (a_hasNormal)
		{
			const float p0 = length(a_normalTex.SampleBias(a_sampler, a_flow.uvA, 1.5).xy * 2.0 - 1.0);
			const float p1 = length(a_normalTex.SampleBias(a_sampler, a_flow.uvA * float2(0.6, 2.0) + 0.37, 1.5).xy * 2.0 - 1.0);
			pattern = saturate((p0 + p1) * 0.5 * 1.4);
		}
		return pattern;
	}

	/** D122, D142: how much of the pixel the foam covers: dense foam solid, thin foam only where the pattern lets it through. */
	float FoamCover(float a_white, float a_pattern)
	{
		const float threshold = a_pattern * 0.5 * (1.0 - saturate(a_white) * 0.7);
		return FallGround::Whitening && a_white > 0.02 ? smoothstep(threshold, threshold + 0.35, a_white * FOAM_DENSITY) * 0.95 : 0.0;
	}

	/**
	 * D67, D99: how much of the pixel shows: faded at seams over a mesh leading it, where it meets what lies behind
	 * (a_behind; foam keeps that contact soft, only shorter) and toward its cut. The same factors PSDepth discards by (the
	 * front-surface pre-pass): anything that fades a pixel here must be discarded there too. D141's fade-in under the lip
	 * is gone (a0e81760d): the pre-pass still claimed those pixels, which punched holes along the crests.
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
