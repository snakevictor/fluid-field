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
	/** D174: world units behind the front a surface must lie to be the next sheet behind (PSBehindDepth). */
	static const float BEHIND_LAYER_EPSILON = 0.05;
	/** D174: pixels out the next sheet behind is also read at (SheetGap). */
	static const int SHEET_GAP_SPREAD = 3;
	/** D176: world units from another sheet (per vertex) within which the pixels' next sheet is trusted fully, none past twice it. */
	static const float SHEET_GAP_TRUST = 96.0;
	/** D176: world units within which a pool under a pixel is the water the falls pour out of (AboveWater). */
	static const float SOURCE_POOL_MATCH = 24.0;

	/**
	 * D174: world units from a pixel of the falls (a_depth, linear) to the next sheet behind it (a_behind, the second depth
	 * layer), the nearest of the pixel's own and its neighbours' a few pixels out: right on the line where two sheets cross, they
	 * lie at the same depth and the second layer holds neither (a dark line through the meeting foam, SS 1228, 1229)
	 */
	float SheetGap(Texture2D<float4> a_behind, int2 a_pixel, float a_depth, float a_vertexGap)
	{
		// D176: the pixels' answer only where the vertices (D128: other sheets only, not the mesh's own) say another sheet is
		// near: a falls' own stacked layers, brought within a hair of each other by the wobble, foamed in slivers (SS 1251-1253)
		const float trust = 1.0 - saturate((a_vertexGap - SHEET_GAP_TRUST) / SHEET_GAP_TRUST);
		if (trust <= 0.0)
			return a_vertexGap;
		const int2 offsets[5] = { int2(0, 0), int2(SHEET_GAP_SPREAD, 0), int2(-SHEET_GAP_SPREAD, 0), int2(0, SHEET_GAP_SPREAD), int2(0, -SHEET_GAP_SPREAD) };
		float gap = 1e9;
		[unroll] for (int k = 0; k < 5; k++)
			gap = min(gap, abs(SharedData::GetScreenDepth(a_behind.Load(int3(a_pixel + offsets[k], 0)).x) - a_depth));
		return lerp(a_vertexGap, min(gap, a_vertexGap), trust);
	}
	/** D129: world units one repeat of the falls' texture (their flow coordinates) spans, for the water's normal maps' scale. */
	static const float FALLS_UV_WORLD = 512.0;
	/** D174: world units over the water below over which the falls foam where they enter it. */
	static const float WATER_FOAM_REACH = 64.0;
	/** D174: world units from the sheet's cut over which it foams. */
	static const float CUT_FOAM_REACH = 24.0;
	/** D174: world units under the water below over which the falls' foam fades out. */
	static const float WATER_FOAM_SINK = 32.0;  // D176: the falls' foam carried on under the water (the user: the sheets' wobble lifted a waterline cut into view), gone before its cuts (WATER_KEEP, POOL_CUT 48)
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
	/** D176: the bake's speed along the mesh's flow lines under which the foam's drift fades to none (world units a second). */
	static const float FOAM_STILL_SPEED = 30.0;
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

	/** D174: mips down the falls read the sky's reflection (rough water: the horizon soft over a curved crest). */
	static const float REFLECTION_BLUR_MIPS = 4.0;

	/** D127: how much of the falls' own normal map tilts the surface (it is drawn for an opaque, diffuse sheet: high contrast). */
	static const float NORMAL_STRENGTH = 0.8;

	/**
	 * D174: a tangent-space normal (x along the texture's u, y along its v, z along a_normal) in world axes, the frame from the
	 * mesh's own tangent (a_tangent, the direction v runs, interpolated per vertex as the normal). PerturbNormal's frame, from the
	 * screen derivatives of the flow coordinates, is constant over each triangle and turned at every edge (hard-edged facets in
	 * the reflection, SS 1203); along the bake's flow, its direction changed from texel to texel; down the sheet, it flipped
	 * across every crest's ridge, where downhill turns from upstream to downstream (patches with hard edges, SS 1214-1221).
	 */
	float3 SheetNormal(float3 a_normal, float3 a_tangent, float3 a_tangentNormal)
	{
		const float3 along = SafeNormalize(a_tangent - a_normal * dot(a_tangent, a_normal), normalize(cross(a_normal, abs(a_normal.z) < 0.9 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0))));
		const float3 across = cross(along, a_normal);
		return SafeNormalize(across * a_tangentNormal.x + along * a_tangentNormal.y + a_normal * a_tangentNormal.z, a_normal);
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

	/** D179: world units the bake's swell of 1 raises the surface (its shape between the vertices, per pixel). */
	static const float SWELL_HEIGHT = 0.0;  // D180: off until its slope comes from the bake's own field (screen derivatives drew the triangles)
	/** D179: world units across a bubble cell (two layers, this and 0.45 of it), and how much the bubbles whiten and glint. */
	static const float BUBBLE_CELL = 6.0;
	static const float BUBBLE_MILK = 0.0;
	static const float BUBBLE_GLINT = 0.9;
	/** D180: how silvery a bubble's rim and how bright its highlight. */
	static const float BUBBLE_RIM = 0.85;
	static const float BUBBLE_HIGHLIGHT = 2.0;
	/** D180: world units of water a falling sheet holds (straight through it): its depth tint, as shallows. */
	static const float SHEET_DEPTH = 96.0;  // D181: 24 read as glass beside the river (its view paths run 100-300 units through the water)
	/** D181: the least depth of water a sheet is tinted as (the river's shallows' look: its tint ramps over 256). */
	static const float SHEET_TINT_DEPTH = 160.0;
	/** D179: how much aeration makes the water opaque (the bubbles scatter what lies behind). */
	static const float BUBBLE_OPACITY = 0.0;
	/** D179: how much aeration foams (the river's foam pattern, as the contact's), as well as bubbling (the user). */
	static const float AERATION_FOAM = 0.22;  // D186: 0.22, was 0.3 (the user: aerated water read thick)
	/** D186: how milky aeration and speed make the falls (FluidField::ApplyAeration's measure). */
	static const float MILK_AERATION = 0.35;
	static const float MILK_SPEED = 0.2;
	/** D189: the most the falls' water goes toward its foam's light (white with air): the river's cap (FluidField::MAX_AERATION, 0.15) left
	 * them the water type's body colour, near black out of the sun (SS 1494, 1495; 16:30) */
	static const float MILK_MOST = 0.3;
	/** D189: the least the falls' body colour is of the scene's ambient light, and its share of that at the crest (the river's look). */
	static const float BODY_AMBIENT = 0.06;  // D189: 0.3 drew the falls solid white (the ambient light runs well over 1)
	static const float BODY_AMBIENT_CREST = 0.5;
	/** D189: the milk in streaks down the fall (the user: flat, a grey plastic sheet, CAM 4): the foam pattern drawn out along the flow
	 * (these times finer across, coarser along), and how far it moves the milk either way */
	static const float MILK_STREAK_ACROSS = 3.0;
	static const float MILK_STREAK_ALONG = 0.3;
	static const float MILK_STREAK = 0.5;  // D191: the white is the ribbons'; 0.85  // D190: 0.6, and sharpened (the aerated phase: white streaks in clear water, not an even grey, the user)
	/** D189: the foam where the sheet meets rock behind it: its share on the rock's sides, and what the bake's pile-up (swell: the
	 * upstream face) and wake (downstream) add (an even white ring round every boulder, an outline round every sheet: CAM 4, 5) */
	static const float ROCK_FOAM_SIDE = 0.0;  // D189: 0.35 traced every sheet's sides against the rock (CAM 4)
	static const float ROCK_FOAM_PILE = 1.2;
	static const float ROCK_FOAM_WAKE = 0.8;
	/** D189: the share of the foam at a sheet's cut edges (a traced white outline round each sheet: CAM 4). */
	static const float CUT_FOAM_SHARE = 0.4;
	/** D186: the aeration's look eased inward: from this aeration it starts to show, and is whole by this (the user: subtler). */
	static const float AERATION_EASE_FROM = 0.15;
	static const float AERATION_EASE_FULL = 0.85;
	/** D179: the bake's disturbance (stirred water: a boulder's wake, piling against rock) over which the water churns with air, and how much. */
	static const float STIRRED_FROM = 0.15;
	static const float STIRRED_FULL = 1.0;
	static const float STIRRED_AERATION = 0.85;

	/**
	 * D179: a normal raised by a height over the surface (a_height, world units, smooth over the screen), by its screen
	 * derivatives (Mikkelsen's surface gradient): the bake's swell, so the sheet bulges and piles up between its vertices
	 */
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
	 * D179: one layer of bubbles (0 to 1 cover) on a lattice of cells, a share of the cells holding one (a_density); a_stretch
	 * the cells' length along the flow over their width in world units (the flow coordinates run long where the water is
	 * fast): the bubbles drawn round in the world, not drawn out with them
	 */
	float3 BubbleLayer(float2 a_p, float a_density, float2x2 a_toScreen, float a_cellPixels)
	{
		const float2 cell = floor(a_p);
		const float2 f = a_p - cell;
		float3 best = 0.0;  // cover, and the offset from the bubble's centre over its radius (its sphere's x, y)
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
				// D180: round on the screen, facing the camera at any slant of the sheet (on the sheet, they foreshortened into
				// flat ellipses): the offset from its centre taken to screen pixels, over its radius in pixels (its cell's size
				// on screen, the layer's world size: not its foreshortened extent)
				const float2 offset = mul(a_toScreen, f - centre) / max(radius * a_cellPixels, 1e-3);
				const float cover = 1.0 - smoothstep(0.85, 1.0, length(offset));
				if (cover > best.x)
					best = float3(cover, offset);
			}
		}
		return best;
	}

	/** D179: bubble layers inside the water: their depth under the surface (world units), the deepest; layers get smaller and dimmer down. */
	static const int BUBBLE_LAYERS = 3;
	static const float BUBBLE_DEPTH = 9.0;
	/** D179: how many times denser the bubbles are in aerated water than in the plain flow. */
	static const float BUBBLE_AERATED_GAIN = 7.0 / 3.0;  // D186: 7 to 3, was 4 to 1 (the user)
	/** D180: the share of the lattice's cells that hold a bubble, overall (a third of D179's: the user). */
	static const float BUBBLE_AMOUNT = 0.9 / 6.0;  // D181: halved again (the user); D186: 10% fewer
	/** D181: the most the falls' foam covers: slightly see-through, as the river's (WaterNormal.hlsli FOAM_OPACITY). */
	static const float FOAM_OPACITY = 0.8;
	/** D181: the least the coarse break-up mask leaves of the foam's strength (its thinned patches). */
	static const float FOAM_BREAKUP_LEAST = 0.75;

	/**
	 * D179: the bubbles carried in aerated water (the user: aeration seen as bubbling, "3d, parallaxed"), on the falls' flow
	 * coordinates (a_flowUv: they ride the water), in BUBBLE_LAYERS layers at depths under the surface: each read where the
	 * view ray reaches its depth (a_viewShift: flow coordinates per world unit of depth, from the view across the sheet),
	 * smaller and dimmer the deeper. 0 to 1 cover; a layer fades to its average where its cells are a few pixels across
	 */
	float4 Bubbles(float2 a_flowUv, float2 a_viewShift, float a_aeration, float a_cellPixels)
	{
		// x the cover of the nearest bubble (a nearer layer in front of a deeper one), yz the pixel's place on its sphere in screen
		// axes (-1 to 1, x right, y down), w how deep its layer lies (0 to 1). a_cellPixels: screen pixels a world unit spans here
		if (a_aeration <= 0.02)
			return 0.0;
		// Four times as dense in aerated water as in the plain flow (the user)
		const float density = saturate(a_aeration * 0.35 * BUBBLE_AMOUNT * lerp(1.0, BUBBLE_AERATED_GAIN, smoothstep(0.3, 0.7, a_aeration)));
		float4 result = 0.0;
		[unroll] for (int k = BUBBLE_LAYERS - 1; k >= 0; k--)
		{
			const float depthShare = (k + 0.5) / BUBBLE_LAYERS;
			const float cellSize = BUBBLE_CELL * lerp(1.0, 0.4, depthShare);
			const float2 uv = a_flowUv + a_viewShift * (BUBBLE_DEPTH * depthShare);
			const float2 p = uv * FlowScale(cellSize) + float2(0.37, 0.61) * k;
			// Screen pixels per unit of the lattice, both ways (the lattice to the screen)
			const float2 dpx = ddx(p), dpy = ddy(p);
			const float det = dpx.x * dpy.y - dpy.x * dpx.y;
			const float2x2 toScreen = abs(det) > 1e-12 ? float2x2(dpy.y, -dpy.x, -dpx.y, dpx.x) / det : float2x2(0, 0, 0, 0);
			const float cellPixels = cellSize * a_cellPixels;
			const float sharp = smoothstep(1.5, 4.0, cellPixels);
			const float3 layer = BubbleLayer(p, density, toScreen, cellPixels);
			const float cover = lerp(density * 0.1, layer.x, sharp) * lerp(1.0, 0.45, depthShare);
			// D190: unresolved, a place on the sphere whose rim matches its average over the disc (its centre drew far bubbles as
			// dark lensing: the white came on only near)
			result = lerp(result, float4(cover, layer.yz * sharp + float2(0.55, 0.55) * (1.0 - sharp), depthShare), saturate(cover * 2.0));
		}
		return result;
	}

	/**
	 * D180: a bubble's look, an air sphere in water seen face on: its rim silvery (light reflected inside the water at the edge
	 * of the air), a highlight off the sun on its curve, and what lies behind lensed through it (refracted toward its centre,
	 * dimmer). a_sphere its place on the bubble in screen axes (-1 to 1); a_right, a_up, a_toEye the camera's axes in world.
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
	 * D189: the height fallen (the bake's, down steep faces, vertical drops counted thrice: FallStrands) over which a fall turns from
	 * the river's look to its own (milky, less see-through, bubbly): the user, plane-identical at the crest, muddier and bubblier
	 * as it falls, gradually (by the height below its source water or its flow's time it jumped from mesh to mesh)
	 */
	static const float DROP_FULL = 600.0;  // D190: 400 (the height fallen recently now, FallStrands' CALM_RUN: no run time)
	/** D189: or by the seconds it has run from the lip (the bake's run, carried on from mesh to mesh): cascades slide, falling little
	 * by the height counted down steep faces, so they read as all crest (milkless, near black: SS 1494, 1495) */
	static const float DROP_SECONDS = 1.5;
	/** D189: at the crest, the share left of the falls' own milk, bubbles and normal map (the river's look there). */
	// D190: none of the falls' own at the crest (the user: it deviated a lot from the river's shader there); the cascades' black
	// (D189's 0.6, SS 1494, 1495) is the body's floor's (BODY_AMBIENT by the sheet's steepness)
	static const float DROP_CREST_MILK = 0.0;
	static const float DROP_CREST_BUBBLES = 0.0;
	static const float DROP_CREST_NORMAL = 0.25;
	/** D189: fully dropped, how much milkier and how many more bubbles (D191: its opacity the body's, BODY_OPACITY). */
	static const float DROP_MILK = 0.0;  // D191: 0.2 (an even grey over the dropped water: the references keep the body colour)
	static const float DROP_BUBBLES = 1.3;

	/**
	 * D189: how far the water has dropped from its crest (0 to 1): by the bake's height fallen (FlowSample::plunge.z). D190: fallen
	 * recently (it clears over calm runs), and not by its run time: a cascade below a shelf read dropped (white streaks) from its
	 * crest (the user: they should start only after the fall, after even the crest's splashes, SS 1617)
	 */
	float Dropped(float a_fallen)
	{
		return smoothstep(0.0, 1.0, saturate(a_fallen / DROP_FULL));
	}

	/**
	 * D190: on a slide (a mesh never steeper than the tangent's: FlowSample::plunge.w -1), by the height fallen or the seconds it has
	 * run from the lip (DROP_SECONDS): sliding, it falls little, and by the height alone the cascades stayed the river's from top to
	 * bottom, clear over dark rock: black (CAM 1, 2)
	 */
	float DroppedSliding(float a_fallen, float a_runTime)
	{
		return smoothstep(0.0, 1.0, saturate(max(a_fallen / DROP_FULL, (a_runTime - CREST_QUIET_SECONDS) / DROP_SECONDS)));
	}

	/**
	 * D190: the aerated phase (0 to 1) just below a fall's tangent (the bake's progress, FlowSample::plunge.w: world height down the
	 * whole fall, 0 at its tangent, 1 at its foot, times how tall it is; negative above the tangent, -1 where it never turns steep)
	 */
	static const float PHASE_AERATED_FULL = 0.3;  // D191: 0.15 (with the slides gone above the tangent, a hard line across the curtain)

	float PhaseAerated(float a_progress)
	{
		return smoothstep(0.0, PHASE_AERATED_FULL, a_progress);
	}

	/**
	 * D190: the share of a slide's own drop (DroppedSliding) a pixel takes: all of it where the mesh never turns steep (its progress -1)
	 * and well above a fall's tangent, none at the brink, clear over the last SLIDE_CLEAR_BAND of the fall's height above it (a long
	 * slide ending in a drop read clear, black, from top to bottom: CAM 1's right cascade)
	 */
	static const float SLIDE_CLEAR_BAND = 0.06;  // 0.15 left a dark strip over a long slide's brink (CAM 1, 16:30)

	float SlideShare(float a_progress)
	{
		return saturate(-a_progress / SLIDE_CLEAR_BAND);
	}

	/**
	 * D191: a fall's white water in ribbons down the flow (the user's references, Niagara and Victoria: the body's own colour between
	 * white bands that widen and close up until they merge near the foot; no grey phase, no even layer): the ribbons' field is the
	 * streak pattern (fine, drawn out along the flow) over a coarse field of columns (RIBBON_COLUMNS of it: each column turns white at
	 * its own rate), covered over a threshold that falls with the bake's progress, merged by RIBBON_MERGE. A slide (no tangent) by
	 * its own drop, to RIBBON_SLIDE at most (the rapids are their own pass)
	 */
	static const float RIBBON_MERGE = 0.65;
	static const float RIBBON_SLIDE = 0.45;
	/**
	 * D191: the ribbons' field: smooth noise in columns over the flow coordinates (across, along: long, riding the flow), two octaves,
	 * with the fine streaks' share for their ragged edges. The foam pattern's own columns ran near 1 down a fast fall: thresholded,
	 * the white came in all at once, a comb of dark spikes across the fall (r6); noise spreads evenly, so few, thin ribbons start first
	 */
	// (cells along a whole number per flow unit: the scroll's phase wraps at 1, and the noise repeats with it, else the white skipped
	// once a cycle: the user, r7)
	static const float2 RIBBON_FREQUENCY = float2(12.0, 1.0);
	static const float RIBBON_STREAK_DETAIL = 0.25;
	/**
	 * D191: the ribbons shift and distort over time (the user: fixed white streaks are "a big no"): their columns bent sideways by a
	 * slow noise down the flow and in time (cells across of the bend, its cells along per flow unit, seconds per cell of time), and
	 * the field itself blended between two patterns drifting apart over time
	 */
	static const float RIBBON_BEND = 0.9;
	static const float RIBBON_BEND_ALONG = 3.0;
	static const float RIBBON_BEND_SECONDS = 2.5;
	static const float RIBBON_MORPH_SECONDS = 4.0;
	/** D191: the field's threshold where the ribbons start and where they have merged, and their edge (field units either side). */
	static const float RIBBON_THRESHOLD_TOP = 0.85;
	static const float RIBBON_THRESHOLD_MERGED = 0.15;
	static const float RIBBON_EDGE = 0.12;  // r7: 0.06 (hard edges, the user)
	/** D191: how far out of a ribbon its haze reaches (field units: the water beside it whitened, air carried out of it), how milky and how opaque. */
	static const float RIBBON_HAZE_REACH = 0.3;
	static const float RIBBON_HAZE_MILK = 0.3;
	static const float RIBBON_HAZE_OPACITY = 0.5;
	/** D191: the share of the dropped water's own milk, aeration and bubbles kept in the gaps between ribbons (the body colour shows). */
	static const float RIBBON_GAP = 0.25;

	/**
	 * D191: smooth value noise (0 to 1) on a unit lattice, repeating every a_period cells along y (a whole number: the flow's run
	 * coordinate, which jumps by 1 where the scroll's phase wraps, so the pattern must repeat with it)
	 */
	float RibbonNoise(float2 a_p, float a_period)
	{
		const float2 i = floor(a_p);
		const float2 f = a_p - i;
		const float2 u = f * f * (3.0 - 2.0 * f);
		const float y0 = i.y - a_period * floor(i.y / a_period), y1 = (i.y + 1.0) - a_period * floor((i.y + 1.0) / a_period);
		return lerp(lerp(BubbleHash(float2(i.x, y0)), BubbleHash(float2(i.x + 1.0, y0)), u.x), lerp(BubbleHash(float2(i.x, y1)), BubbleHash(float2(i.x + 1.0, y1)), u.x), u.y);
	}

	/** D191: two octaves of RibbonNoise over a_cells per flow unit (y a whole number), repeating every flow unit along. */
	float RibbonOctaves(float2 a_flowUv, float2 a_cells, float2 a_offset)
	{
		const float2 p = a_flowUv * a_cells + a_offset;
		return 0.65 * RibbonNoise(p, a_cells.y) + 0.35 * RibbonNoise(p * float2(2.3, 2.0) + float2(5.1, 0.0), a_cells.y * 2.0);
	}

	/**
	 * D191: the ribbons' field (0 to 1) at a point of the flow at a_time (seconds), a_streaks the fine streaks there (FlowFoamPattern
	 * drawn out along the flow): columns bent sideways over time, the pattern morphing between two
	 */
	float RibbonField(float2 a_flowUv, float a_streaks, float a_time)
	{
		// The bend: across, by a noise down the flow (RIBBON_BEND_ALONG cells a flow unit, repeating) and in time (its x)
		const float bend = (RibbonNoise(float2(a_time / RIBBON_BEND_SECONDS, a_flowUv.y * RIBBON_BEND_ALONG), RIBBON_BEND_ALONG) - 0.5) * 2.0 * RIBBON_BEND;
		const float2 uv = a_flowUv + float2(bend / RIBBON_FREQUENCY.x, 0.0);
		// Two patterns, each drifting its own way across, crossfaded over a cycle: each fades out before its drift could show as a slide
		const float cycle = a_time / RIBBON_MORPH_SECONDS;
		const float phase0 = frac(cycle), phase1 = frac(cycle + 0.5);
		const float a = RibbonOctaves(uv, RIBBON_FREQUENCY, float2(phase0 * 1.7, 0.0));
		const float b = RibbonOctaves(uv, RIBBON_FREQUENCY, float2(-phase1 * 1.7 + 31.0, 0.0));
		const float w = abs(phase0 * 2.0 - 1.0);
		// (crossfaded noises lose contrast in the middle: given back about the mean)
		const float columns = 0.5 + (lerp(a, b, w) - 0.5) * rsqrt(max(w * w + (1.0 - w) * (1.0 - w), 0.5));
		return lerp(columns, a_streaks, RIBBON_STREAK_DETAIL);
	}

	/** D191: how far a pixel's ribbons have spread (0 none, 1 merged): by the bake's progress, a slide by its own drop. */
	float RibbonReach(float a_progress, float a_sliding)
	{
		return max(smoothstep(0.0, RIBBON_MERGE, a_progress), a_sliding * RIBBON_SLIDE);
	}

	/** D191: x the ribbons' cover (0 to 1), y their haze (wider and soft); a_field the streaks over the columns, a_reach RibbonReach's. */
	float2 Ribbons(float a_field, float a_reach)
	{
		if (a_reach <= 0.0)
			return 0.0;
		const float threshold = lerp(RIBBON_THRESHOLD_TOP, RIBBON_THRESHOLD_MERGED - RIBBON_EDGE, a_reach);
		const float start = smoothstep(0.0, 0.08, a_reach);
		return float2(smoothstep(threshold - RIBBON_EDGE, threshold + RIBBON_EDGE, a_field), smoothstep(threshold - RIBBON_HAZE_REACH, threshold, a_field)) * start;
	}

	/**
	 * D191: the crest's glow (the references: the sheet thins over the lip and lights up in its own colour, never darker): how much
	 * brighter, over the band of the fall's progress above its tangent, gone just below it
	 */
	static const float CREST_GLOW = 0.0;  // r7: 0.5 drew a grey band across every curtain (the tangent lies well below the crest seen: the user)
	static const float CREST_GLOW_BAND = 0.15;

	/**
	 * D191: the falling body in its own colour (the references: Niagara's light green between its ribbons, never the grey of the rock
	 * seen blurred through the sheet, r6): the water's shallow hue at BODY_LIGHT of the foam's light, over BODY_OPACITY of what lies
	 * behind, by how far it has dropped; and BODY_REFLECTION_LOSS of its mirror (a falling curtain is rough: the horizon's blurred
	 * band across every plunge, r6)
	 */
	static const float BODY_LIGHT = 0.35;
	static const float BODY_TINT = 0.0;             // r7: 0.8 painted the falls lime (the water type's raw shallow colour, not the river's rendered look: the user)
	static const float BODY_OPACITY = 0.1;          // r7: 0.75 with the tint (the old DROP_OPACITY again)
	static const float BODY_REFLECTION_LOSS = 0.0;  // r7: off with the tint (the source's look)

	/** D191: a_color's hue at a_luminance (its chroma kept, the brightest channel held to 1.5 times the luminance). */
	float3 BodyTint(float3 a_color, float a_luminance)
	{
		const float luminance = max(dot(a_color, float3(0.2126, 0.7152, 0.0722)), 1e-4);
		float3 hue = a_color / luminance;
		const float top = max(hue.r, max(hue.g, hue.b));
		hue = top > 1.5 ? lerp(1.0, hue, 0.5 / (top - 1.0)) : hue;
		return hue * a_luminance;
	}

	/**
	 * D191: the white water in tufts (the Victoria reference: lit crowns, shadowed gaps between clumps, riding the flow): a coarse noise
	 * over the flow coordinates, its light from TUFT_DARK to TUFT_BRIGHT of the foam's
	 */
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

	/** D190: a plunge's white water (D191: its ribbons): how aerated and how bubbly. */
	static const float PLUNGE_WHITE_AERATION = 0.9;
	static const float PLUNGE_WHITE_BUBBLES = 0.6;
	/** D190: its surface foam (white and foamy, the user), and the share of the mirror reflection it takes away (foam is matte). */
	static const float PLUNGE_WHITE_FOAM = 1.0;  // D191: the ribbons solid, broken into the pattern only at their edges
	static const float WHITE_REFLECTION_LOSS = 0.75;
	/** D190: how much of the sun's shadow the falls take (the cascades' detail changing with distance showed as the falls' white changing). */
	static const float SUN_SHADOW = 0.3;

	/** D189: the bake's own aeration's share at the crest (it saturates within a short drop: flat over whole sheets), whole fully dropped. D190: none. */
	static const float DROP_CREST_AERATION = 0.0;
	/** D189: how much more the aerated volume shows fully dropped, and how much a landing drives it. */
	static const float DROP_AERATION_FOAM = 1.5;
	static const float PLUNGE_AERATION_FOAM = 0.9;

	/**
	 * D192: how much undropped water reads as rapids (0 to 1): running fast down a steep face (the bake's speed, fields.x x 300 world units a
	 * second, by the face's steepness, 1 - |normal.z|), or stirred (a_fields.z), quiet just above a fall's tangent (a_progress: its crest
	 * stays glass, RAPIDS_CREST_BAND of the fall's height). Set from the baked data (bakestats.py, cache 26): speed median 300 units a
	 * second, p90 390; the CAM 15 cascade 267 / 356 at steepness 0.19 (the old ramp, speed 1.2 to 2.5 alone, never reached it)
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
		// (D191: smooth now that every vertex of a mesh with a source has its depth under it, FallSites' D124 fill: a vertex past
		// SOURCE_DEPTH_MOST took none, and the quiet band ended along the rows of triangles, a saw-tooth across the crest, CAM 11)
		const bool hasSource = a_belowSource < NONE_ABOVE;
		return (hasSource ? saturate((a_belowSource - LIP_BLEND) / LIP_QUIET) : 1.0) * saturate((a_runTime - CREST_QUIET_SECONDS) / CREST_QUIET_SECONDS);
	}

	/**
	 * D174: how strongly the falls foam here (0 to 1), wherever they touch something: the next sheet just behind
	 * (a_sheetGap, world units to the falls' second depth layer), rock, ground or any mesh just behind (a_sceneGap) and the
	 * water they pour into (a_aboveWater, FallGround::Fade's y). Per pixel: the per-vertex cut, seam and meeting distances
	 * drew it in the triangles' shape (white wedges, SS 1150-1155). Not on the lip (a_lip: the crest stays glassy, D145).
	 */
	float ContactStrength(float a_sheetGap, float a_sceneGap, float a_aboveWater, float a_lip, float a_clip, float a_rockShare = 1.0, float a_cutShare = 1.0)
	{
		if (!FallGround::Whitening)
			return 0.0;
		float contact = 1.0 - saturate(a_sheetGap / MEET_FOAM_REACH);
		contact = max(contact, (1.0 - saturate(a_sceneGap / CONTACT_FOAM_REACH)) * a_rockShare);  // D189: by the flow there (ROCK_FOAM_*)
		// The sheet's cut (FallGround::ClipDistance: where it was cut at another sheet, the ground or the water): the foam where two
		// sheets meet ran short of their cut edges, a strip of bare water there faded out, a dark line (SS 1209, 1228)
		contact = max(contact, (1.0 - saturate(a_clip / CUT_FOAM_REACH)) * a_cutShare);
		contact = max(contact, 1.0 - saturate(max(a_aboveWater, 0.0) / WATER_FOAM_REACH));
		// Not on the lip (the crest stays glassy, D145), the water below's foam too: on a short drop the crest lies within its
		// reach of the pool below (SS 1259-1261). D176: a foot no longer takes its pool for its source (FallSites: only water
		// over the mesh's middle), which had cut the waterline foam (SS 1236)
		contact *= a_lip;
		// Falling off as the river's simulated foam does, a narrow solid core and a wide tail broken into the pattern's
		// bubbles: linear over a short reach, it drew a solid white outline
		// Stops at the water below (a_aboveWater is negative under its surface): seen through the water, foam under it read
		// as floating behind the surface
		return contact * contact * saturate(1.0 + a_aboveWater / WATER_FOAM_SINK);
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
	float FoamPattern(Texture2D<float4> a_tex, SamplerState a_sampler, float3 a_world, float3 a_normal, float3 a_tangent, float3 a_velocity, float a_time, float a_mipBias)
	{
		float3 w = pow(abs(a_normal), 4.0);
		w /= max(w.x + w.y + w.z, 1e-4);
		// D176: drifting along the mesh's own flow lines (a_tangent, the direction its texture runs, smooth over the mesh) at a
		// constant speed, its sense from the bake's flow (faded where the water is still): the foam follows the water round
		// crests and rocks without the bake's steps from texel to texel (its speed per pixel opened a lattice, SS 1191-1193)
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
			// No tangent (FallSurface.hlsl's pass): down steep water, along the bake's flow on level water, slower
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
	 * D176: the river's foam pattern (its normal map read as bubbles) on the falls' flow coordinates (FallGround::Flow's uvA:
	 * the bake's flow, integrated and scrolled), FOAM_TILE world units a repeat, a whole number along the flow (D130): the foam
	 * moves, stretches and bends with the baked water. Not dragged by FlowSample::velocity, the direction v runs from the
	 * screen derivatives, constant per triangle (a lattice, SS 1191-1193)
	 */
	float FlowFoamOnce(Texture2D<float4> a_tex, SamplerState a_sampler, float2 a_uv, float2 a_dx, float2 a_dy)
	{
		const float p0 = length(a_tex.SampleGrad(a_sampler, a_uv, a_dx, a_dy).xy * 2.0 - 1.0);
		const float p1 = length(a_tex.SampleGrad(a_sampler, a_uv * 0.61 + 0.37, a_dx * 0.61, a_dy * 0.61).xy * 2.0 - 1.0);
		return saturate((p0 + p1) * 0.5 * 1.4);
	}

	/** D188: how much of the wobble's slope tilts the light, and the most it may tilt it. */
	static const float WOBBLE_LIGHT = 1.0;
	/** D188: how much foam the bake's landings leave (its impact, and the foam carried on down the flow). */
	static const float PLUNGE_FOAM_IMPACT = 1.2;
	static const float PLUNGE_FOAM_CARRIED = 0.6;
	static const float WOBBLE_MOST_TILT = 0.6;

	/**
	 * D188: a_normal tilted by the slope of the wobble that moves the sheet's vertices (FallGround::Wobble, along a_up), from its
	 * gradient over its streak and run (central differences) and the world units each spans (a_wobble.w the run's; the streak's
	 * by the coordinates' stretch); a_frame the way the run goes. a_wobble: FallGround::WobbleAt, z in world units.
	 */
	float3 WobbleNormal(float3 a_normal, float3 a_up, float3 a_frame, float4 a_wobble, float a_stretch)
	{
		if (a_wobble.z <= 0.01 || a_wobble.w <= 1e-3)
			return a_normal;
		static const float STEP = 0.01;
		const float dRun = (FallGround::Wobble(a_wobble.x, a_wobble.y + STEP) - FallGround::Wobble(a_wobble.x, a_wobble.y - STEP)) / (2.0 * STEP);
		const float dAcross = (FallGround::Wobble(a_wobble.x + STEP, a_wobble.y) - FallGround::Wobble(a_wobble.x - STEP, a_wobble.y)) / (2.0 * STEP);
		const float runScale = a_wobble.w, acrossScale = a_wobble.w / max(a_stretch, 0.25);
		float2 slope = float2(dAcross / acrossScale, dRun / runScale) * a_wobble.z * WOBBLE_LIGHT;
		const float tilt = length(slope);
		slope *= tilt > WOBBLE_MOST_TILT ? WOBBLE_MOST_TILT / tilt : 1.0;
		const float3 along = SafeNormalize(a_frame - a_up * dot(a_frame, a_up), cross(a_up, float3(1.0, 0.0, 0.0)));
		const float3 across = cross(along, a_up);
		return SafeNormalize(a_normal - across * slope.x - along * slope.y, a_normal);
	}

	/** D188: how much finer the falls' detail normal layer is, how strong, and the distances it fades out between (near to far). */
	static const float FLOW_DETAIL_SCALE = 3.0;
	static const float FLOW_DETAIL_STRENGTH = 0.5;
	static const float FLOW_DETAIL_NEAR = 400.0;
	static const float FLOW_DETAIL_FAR = 1800.0;
	/** D188: a sharper mip for the falls' normal maps (magnified up close they read as blocks), and the stretch evened out. */
	static const float FLOW_NORMAL_MIP_BIAS = -0.75;
	static const float2 FLOW_STRETCH_RANGE = float2(0.4, 3.0);

	/**
	 * D188: a normal map (tangent space, xy) on the flow's coordinates with their stretch evened out (the user, SS 1466: fast water
	 * draws the coordinates out along the flow and the texture with them, magnified into long blocky shards): as
	 * FlowFoamPattern, along the flow by a_stretch on the run coordinate wrapped into tiles, two phases half a tile apart
	 * crossfaded where each wraps (no shear from a stretch varying across the sheet). a_scale: repeats per flow unit.
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
		// (crossfaded as slopes: averaged normals lose contrast where the two phases meet)
		const float3 n = lerp(n0, n1, w);
		return float3(n.xy * rsqrt(max(w * w + (1.0 - w) * (1.0 - w), 0.5)), n.z);
	}

	/** D189: FlowFoamPattern with its tiles' screen gradients given (a_tilesDx, a_tilesDy: ddx and ddy of a_flowUv * FlowScale(FOAM_TILE)), for branches. */
	float FlowFoamPatternGrad(Texture2D<float4> a_tex, SamplerState a_sampler, float2 a_flowUv, float2 a_tilesDx, float2 a_tilesDy, float a_mipBias, float a_stretch)
	{
		const float2 tiles = a_flowUv * FlowScale(FOAM_TILE);
		const float phase0 = frac(tiles.y);
		const float phase1 = frac(tiles.y + 0.5);
		const float2 uv0 = float2(tiles.x, (phase0 - 0.5) * a_stretch);
		const float2 uv1 = float2(tiles.x + 0.43, (phase1 - 0.5) * a_stretch + 0.29);
		// The gradients from the unwrapped coordinates (the wrap's jump would pick the lowest mip along its seam), with the bias
		const float bias = exp2(a_mipBias);
		const float2 dx = a_tilesDx * float2(1.0, a_stretch) * bias, dy = a_tilesDy * float2(1.0, a_stretch) * bias;
		const float w = abs(phase0 * 2.0 - 1.0);  // phase0's own weight falls to 0 where it wraps, phase1's where it does
		return lerp(FlowFoamOnce(a_tex, a_sampler, uv0, dx, dy), FlowFoamOnce(a_tex, a_sampler, uv1, dx, dy), w);
	}

	float FlowFoamPattern(Texture2D<float4> a_tex, SamplerState a_sampler, float2 a_flowUv, float a_mipBias, float a_stretch = 1.0)
	{
		// D182: along the flow by the coordinates' stretch (a_stretch: the bake's, world units a unit of v spans over a unit of u),
		// so the bubbles keep round where fast water draws the coordinates out. Not on the run coordinate itself (large far down
		// a sheet, scaled by a stretch varying across it, it sheared into swirls, SS 1315): on it wrapped into a tile, in two
		// phases half a tile apart, each faded out where it wraps (the shear stays within a tile)
		const float2 tiles = a_flowUv * FlowScale(FOAM_TILE);
		return FlowFoamPatternGrad(a_tex, a_sampler, a_flowUv, ddx(tiles), ddy(tiles), a_mipBias, a_stretch);
	}

	/**
	 * D176: world units over the water the falls pour into, per pixel: over the pools' captured surface (FallGround::Pool) at a
	 * mesh's foot (a_foot, FallGround::Fade's z: as the pool cut, D71: near a lip the pool above reaches over the drop), else the
	 * vertices' own (a_vertexAbove). Each vertex took its own plane: one just under the pool, its neighbour a plane far below, the
	 * height between them leapt over one triangle (no waterline foam, the line in the triangles' steps, SS 1235, 1236)
	 */
	float AboveWater(float3 a_world, float a_foot, float a_vertexAbove, float a_belowSource)
	{
		// Also wherever the pixel is over the pool: over its surface, it can only be the water below; but not the pool the
		// falls pour out of (a_belowSource: how far under it they are): a crest bulging over it foamed (SS 1259)
		// (D191: the depth under the source is whole over a mesh with one now, FallSites' D124 fill: the test held on some vertices
		// and failed on their neighbours, a false waterline foaming in a wedge along the triangles across the crest, CAM 12)
		float pool;
		if (FallGround::Pool(a_world.xy, pool) && (a_foot > 0.01 || a_world.z >= pool)) {
			const bool source = a_belowSource < NONE_ABOVE && pool >= a_world.z + a_belowSource - SOURCE_POOL_MATCH;
			return source ? a_vertexAbove : a_world.z - pool;
		}
		return a_vertexAbove;
	}

	/**
	 * D181: the foam broken into clumps: the same pattern at a quarter of its frequency (a slow, coarse mask), thinning it in
	 * patches so dense foam does not read as an even sheet. 0 to 1, a multiplier on the foam's strength.
	 */
	float FoamBreakup(Texture2D<float4> a_tex, SamplerState a_sampler, float2 a_flowUv, float a_mipBias, float a_stretch)
	{
		const float2 uv = float2(a_flowUv.x, a_flowUv.y * a_stretch) * FlowScale(FOAM_TILE * 4.0) + 0.13;
		// Read four mips down (a soft, broad mask: the sharp one flickered in shards)
		const float n = length(a_tex.SampleBias(a_sampler, uv, a_mipBias + 4.0).xy * 2.0 - 1.0) * 1.4;
		return lerp(FOAM_BREAKUP_LEAST, 1.0, smoothstep(0.1, 0.9, n));
	}

	/** D174: how much of the pixel the foam covers, as the river's (dense foam solid, thin foam only in the pattern's bubbles). */
	/**
	 * D183: how much of the pixel aerated water's foam covers: a long, soft ramp (from flecks in the pattern's bubbles at low aeration
	 * to dense foam), not FoamCover's narrow one (it switched aerated water on and off, the user)
	 */
	float AerationCover(float a_aeration, float a_pattern)
	{
		const float threshold = a_pattern * 0.6;
		// D183: starting late, so it pulls inward from its edges into smaller patches (the user), still a soft ramp
		return smoothstep(threshold + 0.15, threshold + 0.75, a_aeration) * FOAM_OPACITY;
	}

	/**
	 * D189: the aerated water as a volume (the user: parallaxed, 3D): AERATION_LAYERS layers of its foam from the first depth to
	 * the last under the surface, each read where the view ray reaches it (a_viewShift, as Bubbles), laid front to back (a
	 * nearer layer hides what is behind it), the deeper ones thinner and dimmer (less light reaches them through the water)
	 */
	static const int AERATION_LAYERS = 4;
	static const float AERATION_DEPTH_FIRST = 4.0;
	static const float AERATION_DEPTH_LAST = 40.0;
	/** D189: the light left on the deepest layer, and the share of its cover it keeps. */
	static const float AERATION_DEEP_LIGHT = 0.5;
	static const float AERATION_DEEP_COVER = 0.8;
	/**
	 * D190: the volume read as inside the water, not painted on (the user): its front layer thinner than those behind it, the deeper
	 * ones softer (AERATION_DEEP_BLUR mips: light scattered on its way out) and toward the water's colour (AERATION_DEEP_TINT)
	 */
	static const float AERATION_FRONT_COVER = 0.35;
	static const float AERATION_DEEP_BLUR = 2.0;
	static const float AERATION_DEEP_TINT = 0.6;

	/**
	 * D189: x the volume's cover (0 to 1), y its light over the surface's (the deeper layers dimmer). a_drive: AerationCover's.
	 * D190: z how deep its cover lies on average (0 the first layer, 1 the last)
	 */
	float3 AerationVolume(Texture2D<float4> a_tex, SamplerState a_sampler, float2 a_flowUv, float2 a_viewShift, float a_drive, float a_mipBias, float a_stretch)
	{
		// The gradients outside the branch (the layers lie over the same tiles, shifted)
		const float2 tiles = a_flowUv * FlowScale(FOAM_TILE);
		const float2 tilesDx = ddx(tiles), tilesDy = ddy(tiles);
		float cover = 0.0, light = 0.0, deep = 0.0, through = 1.0;
		[branch] if (a_drive > 0.01)
		{
			[unroll] for (int k = 0; k < AERATION_LAYERS; k++)
			{
				const float t = k / max(AERATION_LAYERS - 1.0, 1.0);
				const float depth = lerp(AERATION_DEPTH_FIRST, AERATION_DEPTH_LAST, t * t);
				const float pattern = FlowFoamPatternGrad(a_tex, a_sampler, a_flowUv + a_viewShift * depth + float2(0.37, 0.19) * k, tilesDx, tilesDy,
					a_mipBias + t * AERATION_DEEP_BLUR, a_stretch);
				const float share = lerp(AERATION_FRONT_COVER, 1.0, smoothstep(0.0, 0.4, t)) * lerp(1.0, AERATION_DEEP_COVER, t);
				const float layer = AerationCover(a_drive * lerp(1.0, 0.8, t), pattern) * 0.7 * share;
				cover += through * layer;
				light += through * layer * lerp(1.0, AERATION_DEEP_LIGHT, t);
				deep += through * layer * t;
				through *= 1.0 - layer;
			}
		}
		return cover > 1e-4 ? float3(cover, light / cover, deep / cover) : float3(0.0, 1.0, 0.0);
	}

	/** D189: how much the bake's own foam (the water striking rock, squeezing past it, rejoining behind it) and its landings whiten and aerate. */
	static const float BAKE_FOAM = 0.5;
	static const float BAKE_FOAM_AERATION = 0.6;
	static const float PLUNGE_AERATION_IMPACT = 1.0;
	static const float PLUNGE_AERATION_CARRIED = 0.8;

	/**
	 * D186: the contact's foam, solid where the contact is strong (water striking rock or the ground head on: thick and steady, the
	 * user, SS 1409), broken into the pattern only in its tail
	 */
	float ContactCover(float a_contact, float a_pattern)
	{
		const float threshold = a_pattern * 0.5 * (1.0 - smoothstep(0.45, 0.85, a_contact));
		return a_contact > 0.02 ? smoothstep(threshold, threshold + 0.25, a_contact) * FOAM_OPACITY : 0.0;
	}

	/**
	 * D189: the foam's relief (the user: lumpy white water, not a flat texture): world units its lumps rise, how much the
	 * relief shades it, the light left in its hollows, and the world units toward the sun over which a lump shadows the next
	 */
	static const float FOAM_RELIEF_HEIGHT = 4.0;  // D190: 3, and read with parallax (FoamParallax): it looked shaded, not raised (the user)
	static const float FOAM_RELIEF = 0.8;
	static const float FOAM_CAVITY = 0.45;  // D191: 0.55 (the white read painted, r6)
	static const float FOAM_SHADOW_REACH = 6.0;
	static const float FOAM_SHADOW = 0.75;  // D191: 0.6
	static const float FOAM_SHADOW_MEAN = 0.2;

	/**
	 * D190: the flow coordinates where the view ray meets the foam's raised surface (the user: shaded, not raised): its lumps stand
	 * up to FOAM_RELIEF_HEIGHT over the water, so the pattern is read nearer the eye by their height (a_viewShift as Bubbles': flow
	 * coordinates per world unit under the surface), two steps toward the height met
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

	/** D189: a_normal raised by the foam's height (a_height, 0 to 1, from a texture: smooth over the screen), per pixel (fine derivatives). */
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
	 * D189: the foam's light over its flat light (a multiplier): its lumps lit by the sun on their slopes (wrapped: foam scatters),
	 * its hollows dimmer, shadowed where the foam toward the sun (a_sunHeight) stands higher; a_sunShare and a_ambientShare
	 * the light's two parts, so the hollows dim only the ambient and a shadowed lump only the sun
	 */
	float FoamRelief(float3 a_up, float3 a_relief, float a_height, float a_sunHeight, float3 a_sun, float a_sunShare, float a_ambientShare)
	{
		const float plain = a_ambientShare + a_sunShare * saturate(dot(a_up, a_sun) * 0.5 + 0.5);
		const float lit = saturate(dot(a_relief, a_sun) * 0.5 + 0.5);
		// D190: about 1 on average (dimmer hollows, brighter crowns; the shadow's usual share given back): up close, where the relief
		// resolves, the foam went greyer than far off, where it is flat (the white faded nearing the plunges, the user)
		const float shadow = 1.0 - FOAM_SHADOW * (saturate((a_sunHeight - a_height) * 4.0) - FOAM_SHADOW_MEAN);
		const float shaded = a_ambientShare * lerp(FOAM_CAVITY, 2.0 - FOAM_CAVITY, a_height) + a_sunShare * lit * shadow;
		return lerp(1.0, clamp(shaded / max(plain, 1e-4), 0.25, 1.6), FOAM_RELIEF);
	}

	float FoamCover(float a_contact, float a_pattern)
	{
		const float threshold = a_pattern * 0.5;
		return a_contact > 0.02 ? smoothstep(threshold, threshold + 0.25, a_contact) * FOAM_OPACITY : 0.0;
	}

	/**
	 * D67, D99: how much of the pixel shows: faded at seams over a mesh leading it, where it meets what lies behind
	 * (a_behind; foam keeps that contact soft, only shorter) and toward its cut. The same factors PSDepth discards by (the
	 * front-surface pre-pass): anything that fades a pixel here must be discarded there too. D141's fade-in under the lip
	 * is gone (D158): the pre-pass still claimed those pixels, which punched holes along the crests.
	 */
	float Alpha(float a_seamFade, float a_behind, float a_foam, float a_clip)
	{
		// D174: foam opaque to the sheet's edge (faded with the water at its cut and seams, the meeting foam stopped short of it)
		const float contact = saturate(a_behind / 4.0);
		return lerp(saturate(a_seamFade) * contact * FallGround::CutFade(a_clip), 1.0, a_foam);
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
	/** D189: halvings that home in on where a ray went behind the scene (its step overshot by up to its length: the hit's edge stepped). */
	static const int SSR_REFINE = 4;
	/** D189: world units in front of the scene within which a ray that never went behind it still takes some of it (the silhouette soft). */
	static const float SSR_NEAR_MISS = 32.0;
	/** D189: the steps after which a near miss counts (the first ones run just in front of what lies behind the sheet itself). */
	static const int SSR_NEAR_FROM = 4;
	/** D189: how far fast and aerated water spreads its reflected rays (rough water: a blurred reflection, no hard cut). */
	static const float SSR_ROUGH_SPEED = 0.05;
	static const float SSR_ROUGH_AERATION = 0.12;

	/** D189: where a_p (camera-relative) lies on the screen (a_uv), its pixel there, and how far behind the scene's depth it is (world units; < 0 in front). */
	float ScreenBehind(Texture2D<float4> a_depth, float3 a_p, float2 a_region, out float2 a_uv, out float2 a_pixel, out bool a_on)
	{
		const float4 clip = mul(FrameBuffer::CameraViewProj, float4(a_p, 1.0));
		a_uv = clip.xy / max(clip.w, 1e-3) * float2(0.5, -0.5) + 0.5;
		a_pixel = a_uv * a_region;
		a_on = clip.w > 1.0 && all(a_uv > 0.0) && all(a_uv < 1.0);
		return a_on ? clip.w - SharedData::GetScreenDepth(a_depth.Load(int3(a_pixel, 0)).x) : -1e9;
	}

	/**
	 * D155: the scene's colour (a_color, sampled with a_sampler) where a reflected ray from a_position (camera-relative) along
	 * a_direction first passes behind the scene's depth (a_depth, the device depth in x), and how sure that is (a: 0 where it
	 * leaves the screen, goes too far or meets nothing). a_dimensions: the textures' size; a_region: the rendered part.
	 * D189: a_jitter (0 to 1, per pixel and frame) shifts its first step (the steps' bands broken up, resolved over frames); the
	 * hit homed in on (SSR_REFINE); and a ray that only passed just in front of the scene takes some of it, by how near (the user:
	 * a hard cut where neighbouring rays split into rock and sky)
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
				// D189: homed in on the crossing between the last step (in front) and this one (behind)
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
				// D174: a ray gone behind the scene took what it went behind, less the further behind it landed: only within the
				// thickness (and none past it), neighbouring rays split into rock and sky with a hard edge (SS 1210-1215)
				const float within = 1.0 - saturate((backBehind - SSR_THICKNESS) / (SSR_THICKNESS * 3.0));
				return float4(a_color.SampleLevel(a_sampler, backPixel / a_dimensions, 0).rgb, reach * within);
			}
			if (i >= SSR_NEAR_FROM && -behind < nearGap) {
				nearGap = -behind;
				nearPixel = pixel;
				nearFade = reach;
			}
		}
		// D189: no hit, but it passed this near in front of the scene: some of what it passed over (a soft silhouette)
		const float near = 1.0 - saturate(nearGap / SSR_NEAR_MISS);
		return nearFade > 0.0 ? float4(a_color.SampleLevel(a_sampler, nearPixel / a_dimensions, 0).rgb, nearFade * near * near * 0.75) : 0.0;
	}
}

#endif  // FLUID_FIELD_FALL_SHADING_HLSLI
