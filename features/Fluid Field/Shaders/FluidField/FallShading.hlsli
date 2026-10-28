// D159: the see-through falls' pixel logic, shared by FallSurface.hlsl and Water.hlsl's FLUID_FIELD_FALLS variant: which parts
// of a sheet show (inner side D100/D103, front surface D125/D126, seam, cut, lip and contact fades) and the white water (D121-D146).
// Needs Common/FrameBuffer.hlsli and Common/SharedData.hlsli included first.

#ifndef FLUID_FIELD_FALL_SHADING_HLSLI
#define FLUID_FIELD_FALL_SHADING_HLSLI

#include "FluidField/FallGround.hlsli"

namespace FallShading
{
	/** D125: world units behind the front surface that still count as the front. */
	static const float FRONT_EPSILON = 2.0;
	/** D174: world units behind the front a surface must lie to be the next sheet behind (PSBehindDepth). */
	static const float BEHIND_LAYER_EPSILON = 0.05;
	/** D174: pixel offset of the neighbours SheetGap also reads. */
	static const int SHEET_GAP_SPREAD = 3;
	/** D176: per-vertex sheet gap (world units) under which the per-pixel gap is fully trusted; not trusted past twice it. */
	static const float SHEET_GAP_TRUST = 96.0;
	/** D176: world units within which a pool under a pixel is the falls' source water (AboveWater). */
	static const float SOURCE_POOL_MATCH = 24.0;

	/**
	 * D174: world units from a falls pixel (a_depth, linear) to the next sheet behind (a_behind, second depth layer), nearest of
	 * the pixel and its neighbours: where two sheets cross they share a depth and the second layer holds neither (a dark line).
	 */
	float SheetGap(Texture2D<float4> a_behind, int2 a_pixel, float a_depth, float a_vertexGap)
	{
		// D176: only where the vertices (D128: other sheets only) see another sheet near, else a falls' own wobbling layers foam in slivers
		const float trust = 1.0 - saturate((a_vertexGap - SHEET_GAP_TRUST) / SHEET_GAP_TRUST);
		if (trust <= 0.0)
			return a_vertexGap;
		const int2 offsets[5] = { int2(0, 0), int2(SHEET_GAP_SPREAD, 0), int2(-SHEET_GAP_SPREAD, 0), int2(0, SHEET_GAP_SPREAD), int2(0, -SHEET_GAP_SPREAD) };
		float gap = 1e9;
		[unroll] for (int k = 0; k < 5; k++)
			gap = min(gap, abs(SharedData::GetScreenDepth(a_behind.Load(int3(a_pixel + offsets[k], 0)).x) - a_depth));
		return lerp(a_vertexGap, min(gap, a_vertexGap), trust);
	}
	/** D129: world units per repeat of the falls' flow coordinates, for the normal maps' scale. */
	static const float FALLS_UV_WORLD = 512.0;
	/** D174: world units above the water below over which the falls foam where they enter it. */
	static const float WATER_FOAM_REACH = 64.0;
	/** D174: world units from the sheet's cut over which it foams. */
	static const float CUT_FOAM_REACH = 24.0;
	/** D174: world units under the water below over which the falls' foam fades out. */
	static const float WATER_FOAM_SINK = 32.0;  // D176: the wobble can lift underwater foam into view; gone before the cuts (WATER_KEEP, POOL_CUT 48)
	/** D141: world units below the source water over which the sheet fades in, so the river runs on into the fall with no seam. */
	static const float LIP_BLEND = 24.0;
	/** D143: world units below the lip's blend over which the other foam (seams, meets, rock contact) comes in. */
	static const float LIP_QUIET = 48.0;
	/** D145: seconds of flow before a sheet's seams, meets, cuts and contacts may foam (full at twice this). */
	static const float CREST_QUIET_SECONDS = 0.3;
	/** D147: world units from what lies behind over which the contact foams. */
	static const float CONTACT_FOAM_REACH = 64.0;
	/** D128, D174: world units from the next sheet behind over which the water foams where two meet (D173). */
	static const float MEET_FOAM_REACH = 96.0;
	/** D174: the river's foam pattern (FluidField::GetFoamCover): tile size (world units), phase restart period (s), top drift (units/s). */
	static const float FOAM_TILE = 160.0;
	static const float FOAM_PERIOD = 0.6;
	static const float FOAM_MAX_DRIFT = 120.0;
	/** D174: top foam drift on level water (units/s); faster, the bake's texel-to-texel changes open the pattern. */
	static const float FOAM_LEVEL_DRIFT = 40.0;
	/** D176: bake speed along the flow lines (units/s) under which the foam's drift fades to none. */
	static const float FOAM_STILL_SPEED = 30.0;
	/** D127: how far aerated water's colour goes toward the river's foam colour. */
	static const float AERATION_LIFT = 0.06;
	/** Output cap: one runaway HDR value becomes a square after bloom and the upscalers. */
	static const float MAX_OUTPUT = 64.0;
	/** D139: "none" distances (FallGround::NO_WATER) are held to NONE_HELD before interpolation; above NONE_ABOVE reads as none. */
	static const float NONE_HELD = 512.0;
	static const float NONE_ABOVE = 500.0;

	/** a_vector normalised, or a_fallback if zero (a NaN would spread into squares through bloom). */
	float3 SafeNormalize(float3 a_vector, float3 a_fallback)
	{
		const float lengthSquared = dot(a_vector, a_vector);
		return lengthSquared > 1e-12 ? a_vector * rsqrt(lengthSquared) : a_fallback;
	}

	/**
	 * D130: repeats of a texture a_worldScale units across per flow-coordinate repeat; a whole number along v, since the
	 * scroll phase wraps at 1 and a fractional count jumps once a cycle.
	 */
	float2 FlowScale(float a_worldScale)
	{
		const float repeats = FALLS_UV_WORLD / max(a_worldScale, 1.0);
		return float2(repeats, max(round(repeats), 1.0));
	}

	/** D174: mips down the sky reflection is read (rough water). */
	static const float REFLECTION_BLUR_MIPS = 4.0;

	/** D127: strength of the falls' own normal map (drawn for an opaque sheet, so high contrast). */
	static const float NORMAL_STRENGTH = 0.8;

	/**
	 * D174: a tangent-space normal in world axes, framed by the mesh's interpolated tangent (a_tangent, the direction v runs).
	 * PerturbNormal's derivative frame is constant per triangle and flips across crests, giving hard-edged facets.
	 */
	float3 SheetNormal(float3 a_normal, float3 a_tangent, float3 a_tangentNormal)
	{
		const float3 along = SafeNormalize(a_tangent - a_normal * dot(a_tangent, a_normal), normalize(cross(a_normal, abs(a_normal.z) < 0.9 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0))));
		const float3 across = cross(along, a_normal);
		return SafeNormalize(across * a_tangentNormal.x + along * a_tangentNormal.y + a_normal * a_tangentNormal.z, a_normal);
	}

	/** A tangent-space normal in world axes, framed from the screen derivatives of a_position and a_uv. Pixel shaders only. */
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

	/** D100, D103: an inner side seen from inside its curl, with another sheet's outer face behind it: not drawn. */
	bool HiddenInnerSide(float4 a_outward, float3 a_toCamera)
	{
		return a_outward.w > 0.5 && dot(SafeNormalize(a_outward.xyz, a_toCamera), a_toCamera) < -0.05;
	}

	/** D125, D126: a fragment behind the front surface (linear depths): the same water, hidden. */
	bool BehindFront(float a_depth, float a_frontDepth)
	{
		return a_depth - a_frontDepth > FRONT_EPSILON;
	}

	/** D179: world units a bake swell of 1 raises the surface. */
	static const float SWELL_HEIGHT = 0.0;  // D180: off until its slope comes from the bake's field (screen derivatives showed the triangles)
	/** D179: world units across a bubble cell (smaller in deeper layers), and how much the bubbles whiten and glint. */
	static const float BUBBLE_CELL = 6.0;
	static const float BUBBLE_MILK = 0.0;
	static const float BUBBLE_GLINT = 0.9;
	/** D180: how silvery a bubble's rim and how bright its highlight. */
	static const float BUBBLE_RIM = 0.85;
	static const float BUBBLE_HIGHLIGHT = 2.0;
	/** D180: world units of water through a falling sheet, for its depth tint. */
	static const float SHEET_DEPTH = 96.0;  // D181: less reads as glass beside the river (its view paths run 100-300 units)
	/** D181: the least depth a sheet is tinted as (the river's shallows tint ramps over 256). */
	static const float SHEET_TINT_DEPTH = 160.0;
	/** D179: how much aeration makes the water opaque. */
	static const float BUBBLE_OPACITY = 0.0;
	/** D179: how much aeration foams (the river's foam pattern), besides bubbling. */
	static const float AERATION_FOAM = 0.22;  // D186
	/** D186: how milky aeration and speed make the falls (FluidField::ApplyAeration's measure). */
	static const float MILK_AERATION = 0.35;
	static const float MILK_SPEED = 0.2;
	/** D189: the most the falls go toward the foam's light; the river's cap (FluidField::MAX_AERATION, 0.15) left them near black out of the sun. */
	static const float MILK_MOST = 0.3;
	/** D189: the falls' body colour floor as a share of ambient light, and its share at the crest. */
	static const float BODY_AMBIENT = 0.06;  // D189: kept low, ambient light runs well over 1
	static const float BODY_AMBIENT_CREST = 0.5;
	/** D189: milk streaked down the fall: foam pattern scale across and along the flow, and how far it moves the milk either way. */
	static const float MILK_STREAK_ACROSS = 3.0;
	static const float MILK_STREAK_ALONG = 0.3;
	static const float MILK_STREAK = 0.5;  // D190, D191: the white comes from the ribbons
	/** D189: rock contact foam on the rock's sides, and what the bake's pile-up (upstream face) and wake (downstream) add. */
	static const float ROCK_FOAM_SIDE = 0.0;  // D189: off, it outlined every sheet against the rock
	static const float ROCK_FOAM_PILE = 1.2;
	static const float ROCK_FOAM_WAKE = 0.8;
	/** D189: foam share at a sheet's cut edges (more outlines every sheet). */
	static const float CUT_FOAM_SHARE = 0.4;
	/** D186: aeration's look eased in: starts at AERATION_EASE_FROM, whole by AERATION_EASE_FULL. */
	static const float AERATION_EASE_FROM = 0.15;
	static const float AERATION_EASE_FULL = 0.85;
	/** D179: bake disturbance range over which stirred water churns with air, and how much. */
	static const float STIRRED_FROM = 0.15;
	static const float STIRRED_FULL = 1.0;
	static const float STIRRED_AERATION = 0.85;

	/** D179: a_normal raised by a_height (world units, screen-smooth) via its screen derivatives (Mikkelsen's surface gradient). */
	float3 RaiseNormal(float3 a_normal, float3 a_position, float a_height)
	{
		const float3 dpdx = ddx(a_position);
		const float3 dpdy = ddy(a_position);
		const float3 r1 = cross(dpdy, a_normal);
		const float3 r2 = cross(a_normal, dpdx);
		const float det = dot(dpdx, r1);
		if (abs(det) < 1e-8)
			return a_normal;
		const float3 gradient = (ddx(a_height) * r1 + ddy(a_height) * r2) / det;
		return SafeNormalize(a_normal - gradient, a_normal);
	}

	float BubbleHash(float2 a_p)
	{
		a_p = frac(a_p * float2(0.1031, 0.1030));
		a_p += dot(a_p, a_p.yx + 33.33);
		return frac((a_p.x + a_p.y) * a_p.x);
	}

	/**
	 * D179: one layer of bubbles on a cell lattice, a_density the share of cells holding one. Returns cover (0 to 1) and the
	 * pixel's offset from the bubble's centre over its radius.
	 */
	float3 BubbleLayer(float2 a_p, float a_density, float2x2 a_toScreen, float a_cellPixels)
	{
		const float2 cell = floor(a_p);
		const float2 f = a_p - cell;
		float3 best = 0.0;  // cover, offset over radius (the sphere's x, y)
		[unroll] for (int j = -1; j <= 1; j++)
		{
			[unroll] for (int i = -1; i <= 1; i++)
			{
				const float2 c = cell + float2(i, j);
				const float present = BubbleHash(c * 1.37 + 11.0);
				if (present > a_density)
					continue;
				const float2 centre = float2(i, j) + float2(BubbleHash(c), BubbleHash(c + 7.3)) * 0.8 + 0.1;
				const float radius = lerp(0.18, 0.42, BubbleHash(c + 3.1));
				// D180: round on screen at any slant (on the sheet they foreshortened into ellipses); radius from the cell's on-screen size
				const float2 offset = mul(a_toScreen, f - centre) / max(radius * a_cellPixels, 1e-3);
				const float cover = 1.0 - smoothstep(0.85, 1.0, length(offset));
				if (cover > best.x)
					best = float3(cover, offset);
			}
		}
		return best;
	}

	/** D179: bubble layers under the surface and the deepest one's depth (world units); deeper layers are smaller and dimmer. */
	static const int BUBBLE_LAYERS = 3;
	static const float BUBBLE_DEPTH = 9.0;
	/** D179: how many times denser bubbles are in aerated water than in the plain flow. */
	static const float BUBBLE_AERATED_GAIN = 7.0 / 3.0;  // D186
	/** D180: overall share of lattice cells holding a bubble. */
	static const float BUBBLE_AMOUNT = 0.9 / 6.0;  // D181, D186
	/** D181: the falls' foam's top cover, slightly see-through as the river's (WaterNormal.hlsli FOAM_OPACITY). */
	static const float FOAM_OPACITY = 0.8;
	/** D181: the least of the foam's strength the coarse break-up mask leaves. */
	static const float FOAM_BREAKUP_LEAST = 0.75;

	/**
	 * D179: bubbles riding the flow coordinates in BUBBLE_LAYERS layers, each read where the view ray reaches its depth
	 * (a_viewShift: flow coordinates per world unit of depth), smaller and dimmer deeper. A layer fades to its average where
	 * its cells are a few pixels across. a_cellPixels: screen pixels per world unit.
	 */
	float4 Bubbles(float2 a_flowUv, float2 a_viewShift, float a_aeration, float a_cellPixels)
	{
		// x nearest bubble's cover, yz the pixel's place on its sphere (screen axes, -1 to 1, y down), w its layer's depth (0 to 1)
		if (a_aeration <= 0.02)
			return 0.0;
		// BUBBLE_AERATED_GAIN times as dense in aerated water as in the plain flow
		const float density = saturate(a_aeration * 0.35 * BUBBLE_AMOUNT * lerp(1.0, BUBBLE_AERATED_GAIN, smoothstep(0.3, 0.7, a_aeration)));
		float4 result = 0.0;
		[unroll] for (int k = BUBBLE_LAYERS - 1; k >= 0; k--)
		{
			const float depthShare = (k + 0.5) / BUBBLE_LAYERS;
			const float cellSize = BUBBLE_CELL * lerp(1.0, 0.4, depthShare);
			const float2 uv = a_flowUv + a_viewShift * (BUBBLE_DEPTH * depthShare);
			const float2 p = uv * FlowScale(cellSize) + float2(0.37, 0.61) * k;
			// Lattice to screen pixels
			const float2 dpx = ddx(p), dpy = ddy(p);
			const float det = dpx.x * dpy.y - dpy.x * dpx.y;
			const float2x2 toScreen = abs(det) > 1e-12 ? float2x2(dpy.y, -dpy.x, -dpx.y, dpx.x) / det : float2x2(0, 0, 0, 0);
			const float cellPixels = cellSize * a_cellPixels;
			const float sharp = smoothstep(1.5, 4.0, cellPixels);
			const float3 layer = BubbleLayer(p, density, toScreen, cellPixels);
			const float cover = lerp(density * 0.1, layer.x, sharp) * lerp(1.0, 0.45, depthShare);
			// D190: unresolved, a sphere place matching the disc's average look (the centre drew far bubbles as dark lensing)
			result = lerp(result, float4(cover, layer.yz * sharp + float2(0.55, 0.55) * (1.0 - sharp), depthShare), saturate(cover * 2.0));
		}
		return result;
	}

	/**
	 * D180: a bubble's look, an air sphere in water seen face on: silvery rim, a sun highlight, the background lensed and dimmer.
	 * a_sphere: place on the bubble in screen axes (-1 to 1); a_right, a_up, a_toEye: camera axes in world.
	 */
	float3 BubbleColor(float2 a_sphere, float3 a_right, float3 a_up, float3 a_toEye, float3 a_sunDirection, float3 a_sunColor, float3 a_light, float3 a_behind)
	{
		const float r2 = saturate(dot(a_sphere, a_sphere));
		const float3 n = SafeNormalize(a_right * a_sphere.x - a_up * a_sphere.y + a_toEye * sqrt(1.0 - r2), a_toEye);
		const float rim = pow(r2, 2.5);
		const float highlight = pow(saturate(dot(reflect(-a_toEye, n), a_sunDirection)), 64.0);
		const float3 body = a_behind * 0.6;
		return lerp(body, a_light * 1.4, rim * BUBBLE_RIM) + a_sunColor * highlight * BUBBLE_HIGHLIGHT;
	}

	/**
	 * D189: height fallen (the bake's, steep faces only, vertical drops counted thrice: FallStrands) over which a fall turns
	 * gradually from the river's look at the crest to its own (milky, less see-through, bubbly).
	 */
	static const float DROP_FULL = 600.0;  // D190: recent height fallen (FallStrands' CALM_RUN)
	/** D189: or seconds run from the lip: cascades slide and fall little, so by height alone they read as all crest. */
	static const float DROP_SECONDS = 1.5;
	/** D189: share of the falls' own milk, bubbles and normal map kept at the crest. */
	// D190: none at the crest, to match the river's shader there; the cascades' dark floor is BODY_AMBIENT's
	static const float DROP_CREST_MILK = 0.0;
	static const float DROP_CREST_BUBBLES = 0.0;
	static const float DROP_CREST_NORMAL = 0.25;
	/** D189: fully dropped, how much milkier and how many more bubbles (D191: opacity is BODY_OPACITY). */
	static const float DROP_MILK = 0.0;  // D191: off, the references keep the body colour
	static const float DROP_BUBBLES = 1.3;

	/**
	 * D189: how far the water has dropped from its crest (0 to 1), by the bake's height fallen (FlowSample::plunge.z).
	 * D190: recent height only (it clears over calm runs), not run time, so white starts only after the fall.
	 */
	float Dropped(float a_fallen)
	{
		return smoothstep(0.0, 1.0, saturate(a_fallen / DROP_FULL));
	}

	/**
	 * D190: Dropped for a slide (never steeper than the tangent: FlowSample::plunge.w -1): by height fallen or run time
	 * (DROP_SECONDS), since by height alone cascades stayed clear and black top to bottom.
	 */
	float DroppedSliding(float a_fallen, float a_runTime)
	{
		return smoothstep(0.0, 1.0, saturate(max(a_fallen / DROP_FULL, (a_runTime - CREST_QUIET_SECONDS) / DROP_SECONDS)));
	}

	/**
	 * D190: the aerated phase (0 to 1) just below a fall's tangent. a_progress: FlowSample::plunge.w, 0 at the tangent, 1 at the
	 * foot, negative above the tangent, -1 where the mesh never turns steep.
	 */
	static const float PHASE_AERATED_FULL = 0.3;  // D191: narrower drew a hard line across the curtain

	float PhaseAerated(float a_progress)
	{
		return smoothstep(0.0, PHASE_AERATED_FULL, a_progress);
	}

	/**
	 * D190: share of a slide's own drop (DroppedSliding) a pixel takes: all where the mesh never turns steep or well above a
	 * tangent, none from SLIDE_CLEAR_BAND above it down (else a long slide ending in a drop read clear and black throughout).
	 */
	static const float SLIDE_CLEAR_BAND = 0.06;  // wider left a dark strip over a long slide's brink

	float SlideShare(float a_progress)
	{
		return saturate(-a_progress / SLIDE_CLEAR_BAND);
	}

	/**
	 * D191: a fall's white water as ribbons down the flow (refs: Niagara, Victoria), body colour between white bands that widen
	 * and merge near the foot. Field: streaks over coarse columns, covered over a threshold that falls with the bake's progress,
	 * merged by RIBBON_MERGE. A slide goes by its own drop, to RIBBON_SLIDE at most (rapids are separate).
	 */
	static const float RIBBON_MERGE = 0.65;
	static const float RIBBON_SLIDE = 0.45;
	/**
	 * D191: the ribbons' field: two octaves of column noise over the flow coordinates (cells across, along), plus the fine streaks'
	 * share for ragged edges. Noise spreads evenly, so a few thin ribbons start first rather than a comb all at once.
	 */
	// (whole cells along per flow unit: the scroll phase wraps at 1 and the noise must repeat with it, or the white skips once a cycle)
	static const float2 RIBBON_FREQUENCY = float2(12.0, 1.0);
	static const float RIBBON_STREAK_DETAIL = 0.25;
	/**
	 * D191: the ribbons move: columns bent sideways by a slow noise (bend in cells across, cells along per flow unit, seconds per
	 * cell of time), and the field crossfaded between two patterns drifting apart.
	 */
	static const float RIBBON_BEND = 0.9;
	static const float RIBBON_BEND_ALONG = 3.0;
	static const float RIBBON_BEND_SECONDS = 2.5;
	static const float RIBBON_MORPH_SECONDS = 4.0;
	/** D191: field threshold where ribbons start and where they have merged, and their edge (field units either side). */
	static const float RIBBON_THRESHOLD_TOP = 0.85;
	static const float RIBBON_THRESHOLD_MERGED = 0.15;
	static const float RIBBON_EDGE = 0.12;  // narrower reads as hard edges
	/** D191: reach of a ribbon's haze (field units), its milk and opacity. */
	static const float RIBBON_HAZE_REACH = 0.3;
	static const float RIBBON_HAZE_MILK = 0.3;
	static const float RIBBON_HAZE_OPACITY = 0.5;
	/** Baked falls: the clear sheet's opacity at most (its colour over what lies behind), and how far what lies behind takes the water's hue. */
	static const float BAKED_CLEAR_OPACITY = 0.8;
	static const float BAKED_CLEAR_TINT = 0.6;
	/** Baked falls: how far the ribbons merge at most (1: a solid white sheet), so the most aerated water keeps clear gaps. */
	static const float RIBBON_MOST_REACH = 0.6;  // at 0.85 the threshold let nearly all the field through: no gaps
	/** Baked falls: share of the bake's aeration foam kept in the ribbons' gaps (outside their haze), so the gaps stay clear. */
	static const float RIBBON_GAP_FOAM = 0.15;
	/** Baked falls: the bake's speed (over 300 units a second) over which the water leaves the river plane's look for the falls'. */
	static const float2 MOTION_SPEED = float2(0.8, 2.0);
	/** Baked falls: the depth of river (world units) the calm water's tint reads as, whatever lies just behind the sheet. */
	static const float RIVER_LOOK_DEPTH = 256.0;
	/** Baked falls: world units fallen over which the crest leaves the river plane's own shading for the falls'. */
	static const float CREST_RIVER_FALL = 48.0;
	/** Baked falls: world units fallen past Foam Start Height over which the water counts as wholly dropped. */
	static const float BAKED_DROP_FULL = 240.0;
	/**
	 * Baked falls: the churn breaking the ribbons up low down and where stirred: its cells (across, along per flow unit; along whole),
	 * drift (cells a second across), the fall over which it comes in (world units), the bake's stirring that brings it too, and
	 * its share of the field at most.
	 */
	static const float2 CHURN_FREQUENCY = float2(28.0, 3.0);
	static const float CHURN_DRIFT = 0.35;
	static const float CHURN_FROM = 250.0;
	static const float CHURN_FULL = 700.0;
	static const float CHURN_STIRRED = 0.15;
	/** Baked falls: the bake's stirring over which the ribbons hand over to the foam's pattern (its p75 1.2, p90 8). */
	static const float STIR_FROM = 1.5;
	static const float STIR_FULL = 6.0;
	/** Baked falls: how far stirred water turns to plain foam. */
	static const float STIR_WHITE = 0.8;
	/** Baked falls: seconds each layout of the foam's grain lives (it churns as it runs). */
	static const float FOAM_MORPH_SECONDS = 1.6;
	/** Baked falls: world units fallen (past Foam Start Height) over which the water whitens by e. */
	static const float WHITE_BREAKUP = 260.0;
	/** Baked falls: the ribbons' reach scaled from x to y by a noise riding the water, over its cells (across, along per flow unit; whole). */
	static const float2 REACH_RIDE = float2(0.35, 1.55);
	static const float2 REACH_RIDE_CELLS = float2(10.0, 3.0);
	static const float CHURN_SHARE = 0.85;
	/**
	 * Baked falls: the pockets between merged ribbons: the reach their curving starts at (whole at RIBBON_MOST_REACH), their warp across
	 * the flow (flow units at most), its noise's cells (across, along per flow unit; along whole) and drift (cells a second), and the
	 * pockets' own cells (rounder than the churn's; along whole).
	 */
	static const float POCKET_CURVE_FROM = 0.3;
	static const float POCKET_WARP = 0.06;
	static const float2 POCKET_WARP_CELLS = float2(5.0, 2.0);
	static const float POCKET_WARP_DRIFT = 0.15;
	static const float2 POCKET_FREQUENCY = float2(14.0, 5.0);
	/** Baked falls: world units above the pool below over which the pockets close (shut by x, open from y). */
	static const float2 POCKET_CLOSE = float2(40.0, 160.0);
	/**
	 * Baked falls: the streaks from the drop: their cells (across, along per flow unit; along whole), seconds each streak takes to fade
	 * in or out, and how far its field dims and lifts (faded streaks fall under the ribbons' threshold, lit ones over).
	 */
	static const float2 STREAK_FREQUENCY = float2(34.0, 4.0);  // along: 1 cell spanned the whole fall (static stripes, the texture sliding under)
	static const float STREAK_LIFE = 2.09;
	static const float STREAK_FADE_LOW = 0.3;
	static const float STREAK_FADE_HIGH = 1.35;
	/** Baked falls: the streaks' on/off window (life noise either side of half), their reach before the aeration has built (ribbon
	 * reach), the drop it comes in over past Foam Start Height (world units), and how much crisper a fully on streak's edge is. */
	static const float STREAK_SWITCH = 0.1;
	/** Baked falls: the share of time a streak is on (its life noise over 1 minus this). */
	static const float STREAK_ON_SHARE = 0.67;
	/** Baked falls (unified): the aeration from which the surface whitens (below it only milk and bubbles under the surface). */
	static const float SURFACE_WHITE_FROM = 0.95;
	/** Baked falls: the clear water's opacity over rock close behind and over a gap, and the depth behind (world units) between. */
	static const float CLEAR_OVER_ROCK = 0.2;
	static const float CLEAR_OVER_GAP = 0.95;
	static const float2 CLEAR_GAP = float2(15.0, 260.0);
	/** Baked falls: along CLEAR_GAP, where the film starts thickening and where it is full (shallow holds, then deepens fast). */
	static const float2 CLEAR_KNEE = float2(0.2, 0.6);
	/** Baked falls (unified): the aeration over which the streaks widen into one another (merged by y). */
	static const float2 STREAK_MERGE = float2(0.45, 0.8);
	static const float STREAK_TOP_REACH = 0.3;
	static const float STREAK_RISE = 300.0;
	/** Baked falls: how much further down (world units of drop) a streak may start than the first (each its own, by its column). */
	static const float STREAK_START_SPREAD = 180.0;
	/** Baked falls (unified): the depth behind (world units) over which the water comes to lie on rock or land, and how white its
	 * churn foams there. */
	static const float2 ON_ROCK = float2(60.0, 150.0);
	static const float ROCK_CHURN_FOAM = 0.9;
	/** Baked falls (unified): only the streaks foam (no contact, landing, baked, churn or aeration foam), while the streaks are tuned. */
	static const bool STREAKS_ONLY_FOAM = true;
	/** Baked falls (unified): the falls' own normal map alone, without the river's three (trying the look). */
	static const bool FALLS_NORMALS_ONLY = true;
	/** Baked falls (unified): contact foam at its edges (only where they run level: CONTACT_LEVEL, of the edge gradient's vertical share)
	 * and at its lower lip. */
	static const float CONTACT_EDGE = 0.0;  // off until it is baked (screen-depth bands changed with the view)
	static const int UNIFIED_AERATION_LAYERS = 1;
	/** Unified: its streaks' foam as the aeration's (the same soft cover, so they blend), driven this hard, clumped down to 1 - STREAK_CLUMP
	 * by the aeration's clumping noise, and at most STREAK_OPACITY (a little see-through). */
	/** Unified: its foam wet (FOAM_REFLECT of the reflection, by fresnel; a broad sun sheen) and lit through: backlit into the sun
	 * (FOAM_SSS_BACK, narrowed by FOAM_SSS_POWER), wrapped round its shaded side, toward the water's hue by FOAM_SSS_TINT, down to
	 * FOAM_SSS_THICK where it is dense. */
	static const float FOAM_REFLECT = 0.15;
	/** Unified: its foam diffuse (FOAM_DIFFUSE of its light by the sun's angle to it), wrapped FOAM_WRAP past the terminator (the light
	 * scattered inside it). */
	static const float FOAM_DIFFUSE = 0.55;
	static const float FOAM_WRAP = 1.0;
	static const float FOAM_SHEEN = 0.25;
	static const float FOAM_SHEEN_POWER = 12.0;
	static const float FOAM_SSS_BACK = 0.35;
	static const float FOAM_SSS_POWER = 2.0;
	static const float FOAM_SSS_WRAP = 0.9;
	static const float FOAM_SSS_WRAP_LIGHT = 0.1;
	static const float FOAM_SSS_TINT = 0.6;
	static const float FOAM_SSS_THICK = 0.4;
	static const float STREAK_FOAM_DRIVE = 1.4;
	static const float STREAK_CLUMP = 0.675;
	static const float STREAK_OPACITY = 0.85;
	static const float2 UNIFIED_AERATION_DRIVE = float2(0.3, 0.8);  ///< its baked aeration (tops out near 0.8) from none to whole aeration foam
	/** Unified: brief clear patches in the aeration (tall cells, across and along its flow, morphing open and shut in place): the share
	 * of the time open and the softness of their edges. */
	static const float2 AERATION_GAP_CELLS = float2(22.0, 14.0);    ///< per uv unit, across and along (the flow uv runs ~4 x longer along per world unit: half corrected, they stay long) (two lattices, the second 1.6 x finer)
	static const float AERATION_GAP_PERIOD = 6.0;                   ///< seconds between a cell's patches
	static const float AERATION_GAP_LIFE = 0.35;                    ///< of the period a patch is open
	static const float AERATION_GAP_CHANCE = 0.3;                   ///< of a cell's cycles that open one
	static const int AERATION_GAP_LATTICES = 1;                     ///< 2: a second, finer lattice too
	static const float2 AERATION_GAP_SIZE = float2(0.18, 0.38);     ///< radius across (cells), smallest to largest
	static const float AERATION_GAP_STRETCH = 1.8;                  ///< its length along over its width
	static const float AERATION_GAP_DRIFT = 0.8;                    ///< cells along it may drift on its own over its life (each its own speed)
	static const float AERATION_GAP_RAGGED = 0.6;                   ///< its rim broken by this share of its radius (two scales of noise)
	static const float2 AERATION_GAP_STRENGTH = float2(0.35, 1.0);  ///< how clear a patch gets at most (each its own)
	static const float AERATION_GAP_MOTTLE = 0.5;                   ///< its inside thinned unevenly by this much noise
	/** Unified: the aeration clumped by a slow noise (cells across and along), down to 1 - AERATION_CLUMP between clumps. */
	static const float2 AERATION_CLUMP_CELLS = float2(40.0, 12.0);
	/** Unified: from this baked aeration to the next its most aerated water is nearly covered (drive past whole, less clumping and gaps). */
	static const float2 AERATION_HEAVY = float2(0.65, 0.78);
	static const float AERATION_HEAVY_DRIVE = 1.5;     // (more drove every grain value past the cover: a flat sheet)
	static const float AERATION_LAYER_OPACITY = 1.06;  ///< the top layer's cover, at most ~85% (the volume's own is 0.7)
	static const float AERATION_CLUMP = 0.6;
	/** Unified: seconds per layout of the foam grain (two crossfaded, each jumping across while unseen: it churns, not slides). */
	static const float FOAM_CHURN_SECONDS = 1.6;

	/** Unified: two churn layouts' values blended at FoamChurn's weight, their contrast kept (a plain crossfade halved it mid-way: the
	 * cover thresholded on it pulsed twice a cycle). */
	float ChurnBlend(float a_b, float a_a, float a_weight)
	{
		const float w = a_weight;
		return 0.5 + (lerp(a_b, a_a, w) - 0.5) * rsqrt(max(w * w + (1.0 - w) * (1.0 - w), 0.5));
	}

	/** Unified: a_uv's churn offsets, the weight of the first, for FOAM_CHURN_SECONDS. */
	float FoamChurn(float a_time, out float2 a_jumpA, out float2 a_jumpB)
	{
		const float cycle = a_time / FOAM_CHURN_SECONDS;
		a_jumpA = float2(BubbleHash(float2(floor(cycle), 4.3)) * 7.0, 0.0);
		a_jumpB = float2(BubbleHash(float2(floor(cycle + 0.5), 9.1)) * 7.0, 0.0);
		return 1.0 - abs(frac(cycle) * 2.0 - 1.0);
	}  ///< of AERATION_LAYERS: only the top one, while the look is built up  ///< every foam on the one grain (the streaks'), moving and looking alike
	static const float CONTACT_LIP = 0.0;  // off with the edges until contact foam is baked
	static const float2 CONTACT_LEVEL = float2(0.5, 0.85);
	static const float STREAK_CRISP = 0.8;
	/** Baked falls: a streak's brightness (by its column), less while it fades; its edge and core light (by how far inside it), and
	 * its grain's swing (the flow-stretched foam pattern). */
	static const float2 STREAK_TONE = float2(0.61, 1.14);
	static const float STREAK_FADING_TONE = 0.8;
	static const float2 STREAK_CORE = float2(0.55, 0.95);
	static const float STREAK_EDGE_LIGHT = 0.75;
	static const float STREAK_CORE_LIGHT = 1.1;
	static const float STREAK_GRAIN = 0.25;
	/** Baked falls: a ribbon's frayed edge: solid from STREAK_SOLID (of its coverage) in, fibres (the flow-stretched foam pattern at
	 * STREAK_FIBRE_ACROSS tiles across, its STREAK_FIBRE span kept) toward the edge. */
	static const float2 STREAK_SOLID = float2(0.55, 0.95);
	static const float STREAK_FIBRE_ACROSS = 12.0;
	static const float2 STREAK_FIBRE = float2(0.35, 0.75);
	/** Baked falls: the ribbons' bubbly holes: the foam pattern at STREAK_BUBBLE_SCALE times its tiling, its STREAK_BUBBLES span kept;
	 * inside a ribbon only STREAK_BUBBLES_INSIDE of them. */
	static const float STREAK_BUBBLE_SCALE = 2.5;
	static const float2 STREAK_BUBBLES = float2(0.2, 0.6);
	static const float STREAK_BUBBLES_INSIDE = 0.35;
	/** D191: share of the dropped water's milk, aeration and bubbles kept between ribbons. */
	static const float RIBBON_GAP = 0.25;

	/**
	 * D191: smooth value noise (0 to 1) on a unit lattice, repeating every a_period cells along y (whole: the run coordinate
	 * jumps by 1 where the scroll phase wraps).
	 */
	float RibbonNoise(float2 a_p, float a_period)
	{
		const float2 i = floor(a_p);
		const float2 f = a_p - i;
		const float2 u = f * f * (3.0 - 2.0 * f);
		const float y0 = i.y - a_period * floor(i.y / a_period), y1 = (i.y + 1.0) - a_period * floor((i.y + 1.0) / a_period);
		return lerp(lerp(BubbleHash(float2(i.x, y0)), BubbleHash(float2(i.x + 1.0, y0)), u.x), lerp(BubbleHash(float2(i.x, y1)), BubbleHash(float2(i.x + 1.0, y1)), u.x), u.y);
	}

	/** D191: two octaves of RibbonNoise at a_cells per flow unit (y whole), repeating every flow unit along. */
	float RibbonOctaves(float2 a_flowUv, float2 a_cells, float2 a_offset)
	{
		const float2 p = a_flowUv * a_cells + a_offset;
		return 0.65 * RibbonNoise(p, a_cells.y) + 0.35 * RibbonNoise(p * float2(2.3, 2.0) + float2(5.1, 0.0), a_cells.y * 2.0);
	}

	/** Baked falls: seconds a churn or pocket pattern lives (it fades in and out over that, in place). */
	static const float MORPH_SECONDS = 2.5;

	/**
	 * Unified: brief clear patches in the aeration (1: clear). Ragged ellipses on two lattices riding the flow (a_flowUv), each cell's
	 * patch opening at its own time for AERATION_GAP_LIFE of the period and drifting on at its own speed; one per cell at a time.
	 */
	float AerationGaps(float2 a_flowUv, float a_time)
	{
		float gap = 0.0;
		[unroll] for (int k = 0; k < AERATION_GAP_LATTICES; k++)
		{
			const float2 cells = AERATION_GAP_CELLS * (k == 0 ? 1.0 : 1.6);
			const float2 p = a_flowUv * cells + float2(0.43, 0.71) * k;
			const float2 cell = floor(p);
			[unroll] for (int j = -1; j <= 0; j++)  // a patch drifts down into the next cell along
			{
				const float2 c = cell + float2(0.0, j);
				// its id wrapped along like the noises (the flow uv jumps a whole repeat as the scroll wraps: unwrapped, every patch swapped)
				const float2 id = float2(c.x, c.y - cells.y * floor(c.y / cells.y)) + 31.7 * k;
				const float cycle = a_time / AERATION_GAP_PERIOD + BubbleHash(id + 5.3);
				const float t = frac(cycle);
				// only some cycles open one (each cycle its own draw, so the spots do not repeat)
				if (t > AERATION_GAP_LIFE || BubbleHash(id + floor(cycle) * 1.913) > AERATION_GAP_CHANCE)
					continue;
				const float life = t / AERATION_GAP_LIFE;
				// it opens, holds, then closes up to nothing (its rim shrinking in, never fading out where it stands)
				const float open = smoothstep(0.0, 0.25, life) * (1.0 - smoothstep(0.55, 0.9, life));
				const float2 centre = c + float2(lerp(0.25, 0.75, BubbleHash(id)), lerp(0.2, 0.5, BubbleHash(id + 7.3)) + life * AERATION_GAP_DRIFT * BubbleHash(id + 2.9));
				const float radius = lerp(AERATION_GAP_SIZE.x, AERATION_GAP_SIZE.y, BubbleHash(id + 3.1)) * open;
				const float2 d = (p - centre) / max(float2(radius, radius * AERATION_GAP_STRETCH), 1e-4);
				const float2 q = (p - centre) + BubbleHash(id + 9.7) * 37.0;
				const float ragged = ((RibbonNoise(q * 5.0, 1e4) - 0.5) * 1.4 + (RibbonNoise(q * 13.0 + 3.3, 1e4) - 0.5) * 0.6) * AERATION_GAP_RAGGED;
				const float shape = open > 0.01 ? 1.0 - smoothstep(0.25, 1.0, length(d) + ragged) : 0.0;  // a long soft rim, not an edge
				const float mottle = lerp(1.0 - AERATION_GAP_MOTTLE, 1.0, RibbonNoise(q * 9.0 + 7.1, 1e4));
				gap = max(gap, shape * mottle * lerp(AERATION_GAP_STRENGTH.x, AERATION_GAP_STRENGTH.y, BubbleHash(id + 4.4)));
			}
		}
		return gap;
	}

	/**
	 * Baked falls: RibbonOctaves foaming in and out in place: two patterns crossfaded, each jumping to a new random layout while it is
	 * wholly faded out (no drift, so nothing slides across the surface).
	 */
	float Morphing(float2 a_flowUv, float2 a_cells, float a_time)
	{
		const float cycle = a_time / MORPH_SECONDS;
		const float phase0 = frac(cycle), phase1 = frac(cycle + 0.5);
		const float2 jump0 = float2(BubbleHash(float2(floor(cycle), 3.1)), BubbleHash(float2(floor(cycle), 7.7))) * 97.0;
		const float2 jump1 = float2(BubbleHash(float2(floor(cycle + 0.5), 5.3)), BubbleHash(float2(floor(cycle + 0.5), 1.9))) * 97.0;
		// whole cells along keep the noise repeating with the scroll
		const float a = RibbonOctaves(a_flowUv, a_cells, float2(jump0.x, floor(jump0.y)));
		const float b = RibbonOctaves(a_flowUv, a_cells, float2(jump1.x, floor(jump1.y)));
		const float w = abs(phase0 * 2.0 - 1.0);  // a's weight is 0 where it jumps (phase0 0), b's where it does (phase0 0.5)
		const float blend = lerp(b, a, 1.0 - w);
		return 0.5 + (blend - 0.5) * rsqrt(max(w * w + (1.0 - w) * (1.0 - w), 0.5));
	}

	/** Baked falls: Morphing for the streaks: one octave of column noise (the second, rounder octave drew blotches round them). */
	float MorphingColumns(float2 a_flowUv, float2 a_cells, float a_time)
	{
		const float cycle = a_time / MORPH_SECONDS;
		const float phase0 = frac(cycle);
		const float2 jump0 = float2(BubbleHash(float2(floor(cycle), 3.1)), BubbleHash(float2(floor(cycle), 7.7))) * 97.0;
		const float2 jump1 = float2(BubbleHash(float2(floor(cycle + 0.5), 5.3)), BubbleHash(float2(floor(cycle + 0.5), 1.9))) * 97.0;
		const float2 p = a_flowUv * a_cells;
		const float a = RibbonNoise(p + float2(jump0.x, floor(jump0.y)), a_cells.y);
		const float b = RibbonNoise(p + float2(jump1.x, floor(jump1.y)), a_cells.y);
		const float w = abs(phase0 * 2.0 - 1.0);
		const float blend = lerp(b, a, 1.0 - w);
		return 0.5 + (blend - 0.5) * rsqrt(max(w * w + (1.0 - w) * (1.0 - w), 0.5));
	}

	/** D191: the ribbons' field (0 to 1) at a_time (s); a_streaks: the fine streaks (FlowFoamPattern drawn out along the flow). */
	float RibbonField(float2 a_flowUv, float a_streaks, float a_time)
	{
		const float bend = (RibbonNoise(float2(a_time / RIBBON_BEND_SECONDS, a_flowUv.y * RIBBON_BEND_ALONG), RIBBON_BEND_ALONG) - 0.5) * 2.0 * RIBBON_BEND;
		const float2 uv = a_flowUv + float2(bend / RIBBON_FREQUENCY.x, 0.0);
		// Each pattern fades out before its drift could show as a slide
		const float cycle = a_time / RIBBON_MORPH_SECONDS;
		const float phase0 = frac(cycle), phase1 = frac(cycle + 0.5);
		const float a = RibbonOctaves(uv, RIBBON_FREQUENCY, float2(phase0 * 1.7, 0.0));
		const float b = RibbonOctaves(uv, RIBBON_FREQUENCY, float2(-phase1 * 1.7 + 31.0, 0.0));
		const float w = abs(phase0 * 2.0 - 1.0);
		// Restore the contrast crossfading loses mid-blend
		const float columns = 0.5 + (lerp(a, b, w) - 0.5) * rsqrt(max(w * w + (1.0 - w) * (1.0 - w), 0.5));
		return lerp(columns, a_streaks, RIBBON_STREAK_DETAIL);
	}

	/** D191: how far the ribbons have spread (0 none, 1 merged), by the bake's progress; a slide by its own drop. */
	float RibbonReach(float a_progress, float a_sliding)
	{
		return max(smoothstep(0.0, RIBBON_MERGE, a_progress), a_sliding * RIBBON_SLIDE);
	}

	/** D191: x ribbon cover (0 to 1), y their haze; a_field from RibbonField, a_reach from RibbonReach. */
	float2 Ribbons(float a_field, float a_reach)
	{
		if (a_reach <= 0.0)
			return 0.0;
		const float threshold = lerp(RIBBON_THRESHOLD_TOP, RIBBON_THRESHOLD_MERGED - RIBBON_EDGE, a_reach);
		const float start = smoothstep(0.0, 0.08, a_reach);
		return float2(smoothstep(threshold - RIBBON_EDGE, threshold + RIBBON_EDGE, a_field), smoothstep(threshold - RIBBON_HAZE_REACH, threshold, a_field)) * start;
	}

	/** Baked falls: the ribbons' edge (field units either side, softer than RIBBON_EDGE) and how far the fine streaks break it up. */
	static const float RIBBON_EDGE_SOFT = 0.22;
	static const float EDGE_BREAK = 0.45;

	/** Baked falls: Ribbons with RIBBON_EDGE_SOFT (feathered, ragged edges). */
	/** Baked falls: Ribbons over an edge of a_edge field units either side. */
	float2 RibbonsEdge(float a_field, float a_reach, float a_edge)
	{
		if (a_reach <= 0.0)
			return 0.0;
		const float threshold = lerp(RIBBON_THRESHOLD_TOP, RIBBON_THRESHOLD_MERGED - a_edge, a_reach);
		const float start = smoothstep(0.0, 0.08, a_reach);
		return float2(smoothstep(threshold - a_edge, threshold + a_edge, a_field), smoothstep(threshold - RIBBON_HAZE_REACH - a_edge, threshold, a_field)) * start;
	}

	float2 RibbonsSoft(float a_field, float a_reach)
	{
		if (a_reach <= 0.0)
			return 0.0;
		const float threshold = lerp(RIBBON_THRESHOLD_TOP, RIBBON_THRESHOLD_MERGED - RIBBON_EDGE_SOFT, a_reach);
		const float start = smoothstep(0.0, 0.08, a_reach);
		return float2(smoothstep(threshold - RIBBON_EDGE_SOFT, threshold + RIBBON_EDGE_SOFT, a_field), smoothstep(threshold - RIBBON_HAZE_REACH - RIBBON_EDGE_SOFT, threshold, a_field)) * start;
	}

	/**
	 * Baked falls: the white water's lumps: cells (across, along per flow unit; along whole), the step toward the sun they are read at
	 * (in the sun's flow-coordinate direction), and their shading: base light, the sun-facing contrast, and the body's own swell.
	 */
	static const float2 LUMP_FREQUENCY = float2(16.0, 4.0);
	static const float LUMP_SUN_STEP = 6.0;
	static const float LUMP_BASE = 0.85;
	static const float LUMP_CONTRAST = 2.6;
	static const float LUMP_BODY = 0.5;
	/** Baked falls: how far the ribbons' thin edges take the water's colour, and its brightness there. */
	static const float EDGE_WATER = 0.6;
	static const float EDGE_WATER_LIGHT = 1.3;

	/** D191: the crest's glow (the sheet thins over the lip and brightens): how much, over a progress band above the tangent. */
	static const float CREST_GLOW = 0.0;  // off: the tangent lies well below the visible crest, so it drew a grey band across curtains
	static const float CREST_GLOW_BAND = 0.15;

	/**
	 * D191: the falling body in its own colour: the water's shallow hue at BODY_LIGHT of the foam's light, over BODY_OPACITY of
	 * what lies behind, by how far it has dropped; BODY_REFLECTION_LOSS of its mirror (a falling curtain is rough).
	 */
	static const float BODY_LIGHT = 0.35;
	static const float BODY_TINT = 0.0;  // off: the water type's raw shallow colour painted the falls lime
	static const float BODY_OPACITY = 0.1;
	static const float BODY_REFLECTION_LOSS = 0.0;  // off with the tint

	/** D191: a_color's hue at a_luminance (chroma kept, brightest channel held to 1.5x the luminance). */
	float3 BodyTint(float3 a_color, float a_luminance)
	{
		const float luminance = max(dot(a_color, float3(0.2126, 0.7152, 0.0722)), 1e-4);
		float3 hue = a_color / luminance;
		const float top = max(hue.r, max(hue.g, hue.b));
		hue = top > 1.5 ? lerp(1.0, hue, 0.5 / (top - 1.0)) : hue;
		return hue * a_luminance;
	}

	/** D191: white water in tufts riding the flow (lit crowns, shadowed gaps): coarse noise scaling the foam's light by TUFT_DARK to TUFT_BRIGHT. */
	static const float2 TUFT_FREQUENCY = float2(7.0, 4.0);
	static const float TUFT_DARK = 0.6;
	static const float TUFT_BRIGHT = 1.12;

	float Tufts(float2 a_flowUv)
	{
		const float n = RibbonOctaves(a_flowUv, TUFT_FREQUENCY, float2(2.7, 0.0));
		return lerp(TUFT_DARK, TUFT_BRIGHT, smoothstep(0.25, 0.75, n));
	}

	float CrestGlow(float a_progress)
	{
		return a_progress > -0.99 ? smoothstep(-CREST_GLOW_BAND, 0.0, a_progress) * (1.0 - smoothstep(0.0, 0.1, a_progress)) : 0.0;
	}

	/** D190: a plunge's white water (D191: its ribbons): aeration and bubbles. */
	static const float PLUNGE_WHITE_AERATION = 0.9;
	static const float PLUNGE_WHITE_BUBBLES = 0.6;
	/** D190: its surface foam, and the share of mirror reflection it removes (foam is matte). */
	static const float PLUNGE_WHITE_FOAM = 1.0;  // D191: ribbons solid, broken into the pattern only at their edges
	static const float WHITE_REFLECTION_LOSS = 0.75;
	/** D190: how much sun shadow the falls take (the cascades' shadow detail changes with distance). */
	static const float SUN_SHADOW = 0.3;

	/** D189: the bake's aeration share at the crest (it saturates within a short drop); whole fully dropped. D190: none. */
	static const float DROP_CREST_AERATION = 0.0;
	/** D189: how much more the aerated volume shows fully dropped, and how much a landing drives it. */
	static const float DROP_AERATION_FOAM = 1.5;
	static const float PLUNGE_AERATION_FOAM = 0.9;

	/**
	 * D192: how much undropped water reads as rapids (0 to 1): fast (a_fields.x, x300 = units/s) down a steep face (1 - |normal.z|),
	 * or stirred (a_fields.z); quiet within RAPIDS_CREST_BAND above a tangent so the crest stays glassy. Ranges from bakestats.py
	 * (cache 26): speed median 300 units/s, p90 390.
	 */
	static const float2 RAPIDS_SPEED = float2(0.7, 1.2);
	static const float2 RAPIDS_STEEP = float2(0.1, 0.35);
	static const float2 RAPIDS_STIRRED = float2(0.15, 0.8);
	static const float RAPIDS_CREST_BAND = 0.1;

	float Rapids(float4 a_fields, float a_progress, float a_steepness)
	{
		const float running = smoothstep(RAPIDS_SPEED.x, RAPIDS_SPEED.y, a_fields.x) * smoothstep(RAPIDS_STEEP.x, RAPIDS_STEEP.y, a_steepness);
		const float churn = max(running, smoothstep(RAPIDS_STIRRED.x, RAPIDS_STIRRED.y, a_fields.z));
		const float crest = a_progress > -0.99 ? 1.0 - saturate(-a_progress / RAPIDS_CREST_BAND) : 0.0;
		return churn * (1.0 - crest);
	}

	/** D59: aeration (0 to 1) after a_runTime seconds of flow. */
	float Aerated(float a_runTime)
	{
		return 1.0 - exp(-a_runTime / FallGround::AERATION_SECONDS);
	}

	/**
	 * D141, D143, D145: how much the lines may foam: none within the lip's blend, quiet below the source water (a_belowSource;
	 * NONE_ABOVE and over: no source), and for a sheet's first seconds of flow.
	 */
	float Lip(float a_belowSource, float a_runTime)
	{
		// D191: smooth since every vertex of a mesh with a source has its depth under it (FallSites' D124 fill)
		const bool hasSource = a_belowSource < NONE_ABOVE;
		return (hasSource ? saturate((a_belowSource - LIP_BLEND) / LIP_QUIET) : 1.0) * saturate((a_runTime - CREST_QUIET_SECONDS) / CREST_QUIET_SECONDS);
	}

	/**
	 * D174: contact foam strength (0 to 1) where the falls touch the next sheet behind (a_sheetGap), anything behind (a_sceneGap),
	 * their cut (a_clip) or the water below (a_aboveWater, FallGround::Fade's y). Per pixel: per-vertex distances drew triangles.
	 * Scaled by a_lip so the crest stays glassy (D145).
	 */
	float ContactStrength(float a_sheetGap, float a_sceneGap, float a_aboveWater, float a_lip, float a_clip, float a_rockShare = 1.0, float a_cutShare = 1.0)
	{
		if (!FallGround::Whitening)
			return 0.0;
		float contact = 1.0 - saturate(a_sheetGap / MEET_FOAM_REACH);
		contact = max(contact, (1.0 - saturate(a_sceneGap / CONTACT_FOAM_REACH)) * a_rockShare);  // D189: by the flow there (ROCK_FOAM_*)
		// The cut (FallGround::ClipDistance): meeting foam alone stops short of the cut edge, leaving a dark line
		contact = max(contact, (1.0 - saturate(a_clip / CUT_FOAM_REACH)) * a_cutShare);
		contact = max(contact, 1.0 - saturate(max(a_aboveWater, 0.0) / WATER_FOAM_REACH));
		// The water below's foam too: on a short drop the crest lies within its reach. D176: a foot's pool is no longer taken as its source
		contact *= a_lip;
		// Squared like the river's foam: a narrow solid core and a wide tail (linear drew a solid outline).
		// Stops at the water below (a_aboveWater < 0 under it): foam seen through the water read as floating behind the surface
		return contact * contact * saturate(1.0 + a_aboveWater / WATER_FOAM_SINK);
	}

	/**
	 * A unified sheet's contact foam, by kind: x the edges (other sheets, rock or ground behind, the cut), y the lower lip (the water
	 * below). Unsquared; ContactStrength's terms.
	 */
	float2 ContactParts(float a_sheetGap, float a_sceneGap, float a_aboveWater, float a_lip, float a_clip, float a_rockShare, float a_cutShare)
	{
		if (!FallGround::Whitening)
			return 0.0;
		float edge = 1.0 - saturate(a_sheetGap / MEET_FOAM_REACH);
		edge = max(edge, (1.0 - saturate(a_sceneGap / CONTACT_FOAM_REACH)) * a_rockShare);
		edge = max(edge, (1.0 - saturate(a_clip / CUT_FOAM_REACH)) * a_cutShare);
		const float lip = 1.0 - saturate(max(a_aboveWater, 0.0) / WATER_FOAM_REACH);
		return float2(edge, lip) * a_lip * saturate(1.0 + a_aboveWater / WATER_FOAM_SINK);
	}

	/** How level an edge of a_value runs in the world (1: its gradient is vertical, the edge horizontal; 0: an upright edge). */
	float EdgeLevel(float a_value, float3 a_position)
	{
		const float3 dpx = ddx(a_position), dpy = ddy(a_position);
		const float dvx = ddx(a_value), dvy = ddy(a_value);
		// the gradient in the surface's plane: g = a dpx + b dpy with g.dpx = dvx, g.dpy = dvy
		const float xx = dot(dpx, dpx), xy = dot(dpx, dpy), yy = dot(dpy, dpy);
		const float det = xx * yy - xy * xy;
		if (abs(det) < 1e-12)
			return 0.0;
		const float a = (dvx * yy - dvy * xy) / det, b = (dvy * xx - dvx * xy) / det;
		const float3 g = a * dpx + b * dpy;
		const float len = length(g);
		return len > 1e-6 ? smoothstep(CONTACT_LEVEL.x, CONTACT_LEVEL.y, abs(g.z) / len) : 0.0;
	}

	/** D174: the river's foam pattern on a fixed world plane (axes a_u, a_v): two phases drifting at a_drift (units/s) and restarting. */
	float FoamPlane(Texture2D<float4> a_tex, SamplerState a_sampler, float3 a_world, float3 a_u, float3 a_v, float2 a_drift, float a_time, float a_mipBias)
	{
		const float2 p = float2(dot(a_world, a_u), dot(a_world, a_v));
		// Each patch restarts at its own time, as the river's (D68: one clock pulsed the whole foam)
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
	 * D174: the river's foam pattern (FluidField::GetFoamCover's look) on the sheet, triplanar over fixed world planes by a_normal.
	 * Fixed axes because axes from the per-pixel normal, ~1e5 units out, shift the pattern tens of units between triangles.
	 * Drift: FOAM_MAX_DRIFT down steep water, at most FOAM_LEVEL_DRIFT along the bake's flow (a_velocity) on level water.
	 */
	float FoamPattern(Texture2D<float4> a_tex, SamplerState a_sampler, float3 a_world, float3 a_normal, float3 a_tangent, float3 a_velocity, float a_time, float a_mipBias)
	{
		float3 w = pow(abs(a_normal), 4.0);
		w /= max(w.x + w.y + w.z, 1e-4);
		// D176: drift at constant speed along the mesh's smooth flow lines (a_tangent), its sense from the bake's flow (faded where
		// still): the bake's per-texel speed opened a lattice
		float3 drift;
		const float tangentLength = length(a_tangent);
		[branch] if (tangentLength > 1e-3)
		{
			float3 along = a_tangent - a_normal * dot(a_tangent, a_normal);
			along = SafeNormalize(along, a_tangent / tangentLength);
			const float sense = clamp(dot(a_velocity, along) / FOAM_STILL_SPEED, -1.0, 1.0);
			drift = along * (FOAM_MAX_DRIFT * sense);
		}
		else
		{
			// No tangent (FallSurface.hlsl's pass): down steep water, slower along the bake's flow on level water
			float3 level = float3(a_velocity.xy, 0.0);
			level *= min(1.0, FOAM_LEVEL_DRIFT / max(length(level), 1e-3));
			drift = lerp(float3(0.0, 0.0, -FOAM_MAX_DRIFT), level, w.z);
		}
		float pattern = 0.0;
		[branch] if (w.z > 0.01)
			pattern += w.z * FoamPlane(a_tex, a_sampler, a_world, float3(1, 0, 0), float3(0, 1, 0), drift.xy, a_time, a_mipBias);
		[branch] if (w.x > 0.01)
			pattern += w.x * FoamPlane(a_tex, a_sampler, a_world + 0.31 * FOAM_TILE, float3(0, 1, 0), float3(0, 0, -1), float2(drift.y, -drift.z), a_time, a_mipBias);
		[branch] if (w.y > 0.01)
			pattern += w.y * FoamPlane(a_tex, a_sampler, a_world + 0.67 * FOAM_TILE, float3(1, 0, 0), float3(0, 0, -1), float2(drift.x, -drift.z), a_time, a_mipBias);
		return pattern;
	}

	/**
	 * D176: one read of the river's foam pattern on the falls' flow coordinates (FallGround::Flow's uvA), so the foam moves and
	 * bends with the baked water; two scales averaged.
	 */
	float FlowFoamOnce(Texture2D<float4> a_tex, SamplerState a_sampler, float2 a_uv, float2 a_dx, float2 a_dy)
	{
		const float p0 = length(a_tex.SampleGrad(a_sampler, a_uv, a_dx, a_dy).xy * 2.0 - 1.0);
		const float p1 = length(a_tex.SampleGrad(a_sampler, a_uv * 0.61 + 0.37, a_dx * 0.61, a_dy * 0.61).xy * 2.0 - 1.0);
		return saturate((p0 + p1) * 0.5 * 1.4);
	}

	/** D188: how much of the wobble's slope tilts the light (WOBBLE_MOST_TILT caps it). */
	static const float WOBBLE_LIGHT = 1.0;
	/** D188: foam left by the bake's landings: at impact, and carried down the flow. */
	static const float PLUNGE_FOAM_IMPACT = 1.2;
	static const float PLUNGE_FOAM_CARRIED = 0.6;
	static const float WOBBLE_MOST_TILT = 0.6;

	/**
	 * D188: a_normal tilted by the slope of the vertex wobble (FallGround::Wobble, along a_up), by central differences over its
	 * streak and run. a_wobble: FallGround::WobbleAt (z in world units, w world units per run unit); a_frame: the run's direction.
	 */
	/** The surface's height by the wobble (a_sizes: along the surface, and a unified sheet's vertical bob along the normal; world units). */
	float WobbleHeight(float a_across, float a_run, float2 a_sizes)
	{
		return a_sizes.x * FallGround::Wobble(a_across, a_run) + (a_sizes.y > 0.0 ? a_sizes.y * FallGround::BobLayers(a_across, a_run) : 0.0);
	}

	float3 WobbleNormal(float3 a_normal, float3 a_up, float3 a_frame, float4 a_wobble, float2 a_sizes, float a_stretch)
	{
		if (a_sizes.x + a_sizes.y <= 0.01 || a_wobble.w <= 1e-3)
			return a_normal;
		static const float STEP = 0.01;
		const float dRun = (WobbleHeight(a_wobble.x, a_wobble.y + STEP, a_sizes) - WobbleHeight(a_wobble.x, a_wobble.y - STEP, a_sizes)) / (2.0 * STEP);
		const float dAcross = (WobbleHeight(a_wobble.x + STEP, a_wobble.y, a_sizes) - WobbleHeight(a_wobble.x - STEP, a_wobble.y, a_sizes)) / (2.0 * STEP);
		const float runScale = a_wobble.w, acrossScale = a_wobble.w / max(a_stretch, 0.25);
		float2 slope = float2(dAcross / acrossScale, dRun / runScale) * WOBBLE_LIGHT;
		const float tilt = length(slope);
		slope *= tilt > WOBBLE_MOST_TILT ? WOBBLE_MOST_TILT / tilt : 1.0;
		const float3 along = SafeNormalize(a_frame - a_up * dot(a_frame, a_up), cross(a_up, float3(1.0, 0.0, 0.0)));
		const float3 across = cross(along, a_up);
		return SafeNormalize(a_normal - across * slope.x - along * slope.y, a_normal);
	}

	/** D188: the detail normal layer's relative scale, strength, and fade-out distances (near to far). */
	static const float FLOW_DETAIL_SCALE = 3.0;
	static const float FLOW_DETAIL_STRENGTH = 0.5;
	static const float FLOW_DETAIL_NEAR = 400.0;
	static const float FLOW_DETAIL_FAR = 1800.0;
	/** D188: sharper mip for the falls' normal maps (magnified up close they look blocky), and the clamp on the flow's stretch. */
	static const float FLOW_NORMAL_MIP_BIAS = -0.75;
	static const float2 FLOW_STRETCH_RANGE = float2(0.4, 3.0);

	/**
	 * D188: a normal map (tangent space) on the flow coordinates with their stretch evened out, as FlowFoamPattern: fast water
	 * otherwise draws the texture into long blocky shards. a_scale: repeats per flow unit.
	 */
	float3 FlowNormalEven(Texture2D<float4> a_tex, SamplerState a_sampler, float2 a_flowUv, float a_stretch, float a_scale, float a_mipBias)
	{
		const float stretch = clamp(a_stretch, FLOW_STRETCH_RANGE.x, FLOW_STRETCH_RANGE.y);
		const float2 tiles = a_flowUv * a_scale;
		const float phase0 = frac(tiles.y);
		const float phase1 = frac(tiles.y + 0.5);
		const float2 uv0 = float2(tiles.x, (phase0 - 0.5) * stretch);
		const float2 uv1 = float2(tiles.x + 0.43, (phase1 - 0.5) * stretch + 0.29);
		const float bias = exp2(a_mipBias);
		const float2 dx = ddx(tiles) * float2(1.0, stretch) * bias, dy = ddy(tiles) * float2(1.0, stretch) * bias;
		const float w = abs(phase0 * 2.0 - 1.0);
		const float3 n0 = a_tex.SampleGrad(a_sampler, uv0, dx, dy).xyz * 2.0 - 1.0;
		const float3 n1 = a_tex.SampleGrad(a_sampler, uv1, dx, dy).xyz * 2.0 - 1.0;
		// Crossfaded as slopes, contrast restored (averaged normals flatten where the phases meet)
		const float3 n = lerp(n0, n1, w);
		return float3(n.xy * rsqrt(max(w * w + (1.0 - w) * (1.0 - w), 0.5)), n.z);
	}

	/** D189: FlowFoamPattern taking its tiles' screen gradients (ddx, ddy of a_flowUv * FlowScale(FOAM_TILE)), for use in branches. */
	float FlowFoamPatternGrad(Texture2D<float4> a_tex, SamplerState a_sampler, float2 a_flowUv, float2 a_tilesDx, float2 a_tilesDy, float a_mipBias, float a_stretch)
	{
		const float2 tiles = a_flowUv * FlowScale(FOAM_TILE);
		const float phase0 = frac(tiles.y);
		const float phase1 = frac(tiles.y + 0.5);
		const float2 uv0 = float2(tiles.x, (phase0 - 0.5) * a_stretch);
		const float2 uv1 = float2(tiles.x + 0.43, (phase1 - 0.5) * a_stretch + 0.29);
		// Gradients from the unwrapped coordinates (the wrap's jump would pick the lowest mip along its seam)
		const float bias = exp2(a_mipBias);
		const float2 dx = a_tilesDx * float2(1.0, a_stretch) * bias, dy = a_tilesDy * float2(1.0, a_stretch) * bias;
		const float w = abs(phase0 * 2.0 - 1.0);  // each phase's weight is 0 where it wraps
		return lerp(FlowFoamOnce(a_tex, a_sampler, uv0, dx, dy), FlowFoamOnce(a_tex, a_sampler, uv1, dx, dy), w);
	}

	float FlowFoamPattern(Texture2D<float4> a_tex, SamplerState a_sampler, float2 a_flowUv, float a_mipBias, float a_stretch = 1.0)
	{
		// D182: stretched along the flow by a_stretch (world units per v over per u) so bubbles stay round in fast water. Applied
		// within wrapped tiles, two phases half a tile apart: on the raw run coordinate a varying stretch shears into swirls
		const float2 tiles = a_flowUv * FlowScale(FOAM_TILE);
		return FlowFoamPatternGrad(a_tex, a_sampler, a_flowUv, ddx(tiles), ddy(tiles), a_mipBias, a_stretch);
	}

	/**
	 * D176: world units above the water the falls pour into, per pixel: over the captured pool (FallGround::Pool) at a mesh's foot
	 * (a_foot, FallGround::Fade's z; D71) or wherever the pixel is above it, else the per-vertex value (a_vertexAbove), whose
	 * per-vertex planes jump across triangles.
	 */
	float AboveWater(float3 a_world, float a_foot, float a_vertexAbove, float a_belowSource)
	{
		// Not the source pool (a_belowSource), or a crest bulging over it foams. D191: a_belowSource is whole over the mesh (D124 fill)
		float pool;
		if (FallGround::Pool(a_world.xy, pool) && (a_foot > 0.01 || a_world.z >= pool)) {
			const bool source = a_belowSource < NONE_ABOVE && pool >= a_world.z + a_belowSource - SOURCE_POOL_MATCH;
			return source ? a_vertexAbove : a_world.z - pool;
		}
		return a_vertexAbove;
	}

	/** D181: foam break-up mask (0 to 1, a strength multiplier): the pattern at a quarter frequency, so dense foam isn't an even sheet. */
	float FoamBreakup(Texture2D<float4> a_tex, SamplerState a_sampler, float2 a_flowUv, float a_mipBias, float a_stretch)
	{
		// widened across by the stretch, not drawn out along: a varying stretch along broke its tiling, so it jumped as the scroll wrapped
		const float2 uv = float2(a_flowUv.x / max(a_stretch, 0.25), a_flowUv.y) * FlowScale(FOAM_TILE * 4.0) + 0.13;
		// Four mips down: the sharp mask flickered in shards
		const float n = length(a_tex.SampleBias(a_sampler, uv, a_mipBias + 4.0).xy * 2.0 - 1.0) * 1.4;
		return lerp(FOAM_BREAKUP_LEAST, 1.0, smoothstep(0.1, 0.9, n));
	}

	/** D183: aerated water's foam cover: a long, soft ramp from flecks to dense foam (FoamCover's narrow one switched it on and off). */
	float AerationCover(float a_aeration, float a_pattern)
	{
		const float threshold = a_pattern * 0.6;
		// D183: starting late, so it pulls in from its edges into smaller patches
		return smoothstep(threshold + 0.15, threshold + 0.75, a_aeration) * FOAM_OPACITY;
	}

	/**
	 * D189: aerated water as a volume: AERATION_LAYERS foam layers between the first and last depth, each read where the view ray
	 * reaches it (a_viewShift, as Bubbles), composited front to back, deeper ones thinner and dimmer.
	 */
	static const int AERATION_LAYERS = 4;
	static const float AERATION_DEPTH_FIRST = 4.0;
	static const float AERATION_DEPTH_LAST = 40.0;
	/** D189: light left on the deepest layer, and the share of its cover it keeps. */
	static const float AERATION_DEEP_LIGHT = 0.5;
	static const float AERATION_DEEP_COVER = 0.8;
	/** D190: so it reads inside the water: front layer thinner, deeper ones blurred (mips) and tinted toward the water's colour. */
	static const float AERATION_FRONT_COVER = 0.35;
	static const float AERATION_DEEP_BLUR = 2.0;
	static const float AERATION_DEEP_TINT = 0.6;

	/** D189: x cover (0 to 1), y light over the surface's; D190: z mean depth of the cover (0 first layer, 1 last). a_drive: AerationCover's. */
	float3 AerationVolume(Texture2D<float4> a_tex, SamplerState a_sampler, float2 a_flowUv, float2 a_viewShift, float a_drive, float a_mipBias, float a_stretch, int a_layers = AERATION_LAYERS,
		float a_frontCover = AERATION_FRONT_COVER, float a_layerOpacity = 0.7)
	{
		// Gradients outside the branch (the layers share tiles, shifted)
		const float2 tiles = a_flowUv * FlowScale(FOAM_TILE);
		const float2 tilesDx = ddx(tiles), tilesDy = ddy(tiles);
		float cover = 0.0, light = 0.0, deep = 0.0, through = 1.0;
		[branch] if (a_drive > 0.01)
		{
			[unroll] for (int k = 0; k < AERATION_LAYERS; k++)
			{
				if (k >= a_layers)
					break;
				const float t = k / max(AERATION_LAYERS - 1.0, 1.0);
				const float depth = lerp(AERATION_DEPTH_FIRST, AERATION_DEPTH_LAST, t * t);
				const float pattern = FlowFoamPatternGrad(a_tex, a_sampler, a_flowUv + a_viewShift * depth + float2(0.37, 0.19) * k, tilesDx, tilesDy,
					a_mipBias + t * AERATION_DEEP_BLUR, a_stretch);
				const float share = lerp(a_frontCover, 1.0, smoothstep(0.0, 0.4, t)) * lerp(1.0, AERATION_DEEP_COVER, t);
				const float layer = saturate(AerationCover(a_drive * lerp(1.0, 0.8, t), pattern) * a_layerOpacity * share);
				cover += through * layer;
				light += through * layer * lerp(1.0, AERATION_DEEP_LIGHT, t);
				deep += through * layer * t;
				through *= 1.0 - layer;
			}
		}
		return cover > 1e-4 ? float3(cover, light / cover, deep / cover) : float3(0.0, 1.0, 0.0);
	}

	/** D189: how much the bake's own foam (striking, squeezing past, rejoining behind rock) and its landings whiten and aerate. */
	static const float BAKE_FOAM = 0.5;
	static const float BAKE_FOAM_AERATION = 0.6;
	static const float PLUNGE_AERATION_IMPACT = 1.0;
	static const float PLUNGE_AERATION_CARRIED = 0.8;

	/** D186: contact foam cover: solid where contact is strong (water striking rock head on), broken into the pattern only in its tail. */
	float ContactCover(float a_contact, float a_pattern)
	{
		const float threshold = a_pattern * 0.5 * (1.0 - smoothstep(0.45, 0.85, a_contact));
		return a_contact > 0.02 ? smoothstep(threshold, threshold + 0.25, a_contact) * FOAM_OPACITY : 0.0;
	}

	/**
	 * D189: foam relief (lumpy white water): lump height (world units), relief shading strength, light left in hollows, and the
	 * world units toward the sun over which a lump shadows the next.
	 */
	static const float FOAM_RELIEF_HEIGHT = 4.0;  // D190: with parallax (FoamParallax) so it reads raised, not just shaded
	static const float FOAM_RELIEF = 0.8;
	static const float FOAM_CAVITY = 0.45;  // D191: higher read as painted
	static const float FOAM_SHADOW_REACH = 6.0;
	static const float FOAM_SHADOW = 0.75;  // D191
	static const float FOAM_SHADOW_MEAN = 0.2;

	/**
	 * D190: flow coordinates where the view ray meets the foam's raised surface (lumps up to FOAM_RELIEF_HEIGHT; a_viewShift as
	 * Bubbles'), in two steps.
	 */
	float2 FoamParallax(Texture2D<float4> a_tex, SamplerState a_sampler, float2 a_flowUv, float2 a_viewShift, float a_mipBias, float a_stretch)
	{
		const float2 tiles = a_flowUv * FlowScale(FOAM_TILE);
		const float2 tilesDx = ddx(tiles), tilesDy = ddy(tiles);
		float2 uv = a_flowUv;
		[unroll] for (int k = 0; k < 2; k++)
			uv = a_flowUv - a_viewShift * (FOAM_RELIEF_HEIGHT * FlowFoamPatternGrad(a_tex, a_sampler, uv, tilesDx, tilesDy, a_mipBias, a_stretch));
		return uv;
	}

	/** D189: a_normal raised by the foam's height (a_height, 0 to 1, screen-smooth), with fine derivatives. */
	float3 FoamReliefNormal(float3 a_normal, float3 a_position, float a_height)
	{
		const float3 dpdx = ddx_fine(a_position);
		const float3 dpdy = ddy_fine(a_position);
		const float3 r1 = cross(dpdy, a_normal);
		const float3 r2 = cross(a_normal, dpdx);
		const float det = dot(dpdx, r1);
		if (abs(det) < 1e-8)
			return a_normal;
		const float height = a_height * FOAM_RELIEF_HEIGHT;
		const float3 gradient = (ddx_fine(height) * r1 + ddy_fine(height) * r2) / det;
		return SafeNormalize(a_normal - gradient, a_normal);
	}

	/**
	 * D189: the foam's light over its flat light (a multiplier): lumps lit on their slopes (wrapped: foam scatters), hollows dimmer,
	 * shadowed where foam toward the sun (a_sunHeight) stands higher. Hollows dim only the ambient share, shadows only the sun's.
	 */
	float FoamRelief(float3 a_up, float3 a_relief, float a_height, float a_sunHeight, float3 a_sun, float a_sunShare, float a_ambientShare)
	{
		const float plain = a_ambientShare + a_sunShare * saturate(dot(a_up, a_sun) * 0.5 + 0.5);
		const float lit = saturate(dot(a_relief, a_sun) * 0.5 + 0.5);
		// D190: about 1 on average (the shadow's mean given back), else close-up foam, where the relief resolves, reads greyer than far
		const float shadow = 1.0 - FOAM_SHADOW * (saturate((a_sunHeight - a_height) * 4.0) - FOAM_SHADOW_MEAN);
		const float shaded = a_ambientShare * lerp(FOAM_CAVITY, 2.0 - FOAM_CAVITY, a_height) + a_sunShare * lit * shadow;
		return lerp(1.0, clamp(shaded / max(plain, 1e-4), 0.25, 1.6), FOAM_RELIEF);
	}

	/** D174: foam cover, as the river's: dense foam solid, thin foam only in the pattern's bubbles. */
	float FoamCover(float a_contact, float a_pattern)
	{
		const float threshold = a_pattern * 0.5;
		return a_contact > 0.02 ? smoothstep(threshold, threshold + 0.25, a_contact) * FOAM_OPACITY : 0.0;
	}

	/**
	 * D67, D99: how much of the pixel shows: faded at seams, at contact with what lies behind (a_behind) and toward the cut.
	 * PSDepth (front-surface pre-pass) must discard by the same factors. D158: no fade-in under the lip (the pre-pass punched holes).
	 */
	float Alpha(float a_seamFade, float a_behind, float a_foam, float a_clip)
	{
		// D174: foam opaque to the sheet's edge
		const float contact = saturate(a_behind / 4.0);
		return lerp(saturate(a_seamFade) * contact * FallGround::CutFade(a_clip), 1.0, a_foam);
	}

	/**
	 * D108, D110: "Depth Behind" debug bands: red under 50 units, yellow to 150, green to 300, blue to 600, magenta at the reach.
	 */
	float3 DepthBehindBand(float a_behind)
	{
		return a_behind >= 599.0 ? float3(1.0, 0.0, 1.0) : a_behind >= 300.0 ? float3(0.0, 0.2, 1.0) :
		                                               a_behind >= 150.0     ? float3(0.0, 1.0, 0.0) :
		                                               a_behind >= 50.0      ? float3(1.0, 1.0, 0.0) :
		                                                                       float3(1.0, 0.0, 0.0);
	}

	/** D155: SSR march: steps, first step and its growth (world units), and surface thickness. */
	static const int SSR_STEPS = 24;
	static const float SSR_FIRST_STEP = 8.0;
	static const float SSR_GROWTH = 1.18;
	static const float SSR_THICKNESS = 48.0;
	/** D189: bisection steps refining a hit (a step overshoots by up to its length). */
	static const int SSR_REFINE = 4;
	/** D189: world units in front of the scene within which a missing ray still takes some of it (a soft silhouette). */
	static const float SSR_NEAR_MISS = 32.0;
	/** D189: first step at which near misses count (earlier ones skim what lies behind the sheet itself). */
	static const int SSR_NEAR_FROM = 4;
	/** D189: reflected ray spread for fast and aerated water (rough water). */
	static const float SSR_ROUGH_SPEED = 0.05;
	static const float SSR_ROUGH_AERATION = 0.12;

	/** D189: a_p (camera-relative) on screen (a_uv, a_pixel), and how far behind the scene's depth it lies (world units; < 0 in front). */
	float ScreenBehind(Texture2D<float4> a_depth, float3 a_p, float2 a_region, out float2 a_uv, out float2 a_pixel, out bool a_on)
	{
		const float4 clip = mul(FrameBuffer::CameraViewProj, float4(a_p, 1.0));
		a_uv = clip.xy / max(clip.w, 1e-3) * float2(0.5, -0.5) + 0.5;
		a_pixel = a_uv * a_region;
		a_on = clip.w > 1.0 && all(a_uv > 0.0) && all(a_uv < 1.0);
		return a_on ? clip.w - SharedData::GetScreenDepth(a_depth.Load(int3(a_pixel, 0)).x) : -1e9;
	}

	/**
	 * D155: screen-space reflection: the scene colour where a ray from a_position (camera-relative) along a_direction first passes
	 * behind the depth buffer, and its confidence in a (0 off screen, too far or no hit). a_dimensions: texture size; a_region:
	 * rendered part. D189: a_jitter (0 to 1) offsets the first step to break up banding; hits are refined (SSR_REFINE); near
	 * misses take some colour so neighbouring rays don't split hard into rock and sky.
	 */
	float4 ScreenReflection(Texture2D<float4> a_color, Texture2D<float4> a_depth, SamplerState a_sampler, float3 a_position, float3 a_direction, float2 a_dimensions, float2 a_region, float a_jitter)
	{
		float3 p = a_position;
		float step = SSR_FIRST_STEP;
		p += a_direction * (step * a_jitter);
		float nearGap = SSR_NEAR_MISS;
		float2 nearPixel = 0.0;
		float nearFade = 0.0;
		[loop] for (int i = 0; i < SSR_STEPS; i++)
		{
			const float3 last = p;
			p += a_direction * step;
			step *= SSR_GROWTH;
			float2 uv, pixel;
			bool on;
			const float behind = ScreenBehind(a_depth, p, a_region, uv, pixel, on);
			if (!on)
				break;
			const float2 edge = min(uv, 1.0 - uv);
			const float reach = saturate(min(edge.x, edge.y) * 10.0) * (1.0 - float(i) / float(SSR_STEPS));
			if (behind > 0.0) {
				// D189: bisect between the last step (in front) and this one (behind)
				float3 front = last, back = p;
				float backBehind = behind;
				float2 backPixel = pixel;
				[unroll] for (int k = 0; k < SSR_REFINE; k++)
				{
					const float3 middle = (front + back) * 0.5;
					float2 middleUv, middlePixel;
					bool middleOn;
					const float middleBehind = ScreenBehind(a_depth, middle, a_region, middleUv, middlePixel, middleOn);
					if (middleOn && middleBehind > 0.0) {
						back = middle;
						backBehind = middleBehind;
						backPixel = middlePixel;
					} else
						front = middle;
				}
				// D174: fade past the thickness rather than cut, else neighbouring rays split into rock and sky with a hard edge
				const float within = 1.0 - saturate((backBehind - SSR_THICKNESS) / (SSR_THICKNESS * 3.0));
				return float4(a_color.SampleLevel(a_sampler, backPixel / a_dimensions, 0).rgb, reach * within);
			}
			if (i >= SSR_NEAR_FROM && -behind < nearGap) {
				nearGap = -behind;
				nearPixel = pixel;
				nearFade = reach;
			}
		}
		// D189: no hit: take some of the nearest near miss (a soft silhouette)
		const float near = 1.0 - saturate(nearGap / SSR_NEAR_MISS);
		return nearFade > 0.0 ? float4(a_color.SampleLevel(a_sampler, nearPixel / a_dimensions, 0).rgb, nearFade * near * near * 0.75) : 0.0;
	}
}

#endif  // FLUID_FIELD_FALL_SHADING_HLSLI
