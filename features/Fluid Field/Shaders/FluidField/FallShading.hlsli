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
	static const float AERATION_FOAM = 0.3;
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
	static const float BUBBLE_AERATED_GAIN = 4.0;
	/** D180: the share of the lattice's cells that hold a bubble, overall (a third of D179's: the user). */
	static const float BUBBLE_AMOUNT = 1.0 / 6.0;  // D181: halved again (the user)
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
			result = lerp(result, float4(cover, layer.yz * sharp, depthShare), saturate(cover * 2.0));
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
	float ContactStrength(float a_sheetGap, float a_sceneGap, float a_aboveWater, float a_lip, float a_clip)
	{
		if (!FallGround::Whitening)
			return 0.0;
		float contact = 1.0 - saturate(a_sheetGap / MEET_FOAM_REACH);
		contact = max(contact, 1.0 - saturate(a_sceneGap / CONTACT_FOAM_REACH));
		// The sheet's cut (FallGround::ClipDistance: where it was cut at another sheet, the ground or the water): the foam where two
		// sheets meet ran short of their cut edges, a strip of bare water there faded out, a dark line (SS 1209, 1228)
		contact = max(contact, 1.0 - saturate(a_clip / CUT_FOAM_REACH));
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
	float FlowFoamPattern(Texture2D<float4> a_tex, SamplerState a_sampler, float2 a_flowUv, float a_mipBias, float a_stretch = 1.0)
	{
		// D181: along the flow by the coordinates' stretch (a_stretch: world units a unit of v spans over a unit of u), so the
		// bubbles keep round where fast water draws the coordinates out (they were drawn into streaks)
		const float2 uv = float2(a_flowUv.x, a_flowUv.y * a_stretch) * FlowScale(FOAM_TILE);
		const float p0 = length(a_tex.SampleBias(a_sampler, uv, a_mipBias).xy * 2.0 - 1.0);
		const float p1 = length(a_tex.SampleBias(a_sampler, uv * 0.61 + 0.37, a_mipBias).xy * 2.0 - 1.0);
		return saturate((p0 + p1) * 0.5 * 1.4);
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
			if (behind > 0.0) {
				// D174: a ray gone behind the scene took what it went behind, less the further behind it landed: only within the
				// thickness (and none past it), neighbouring rays split into rock and sky with a hard edge (SS 1210-1215)
				const float2 edge = min(uv, 1.0 - uv);
				const float within = 1.0 - saturate((behind - SSR_THICKNESS - step) / (SSR_THICKNESS * 3.0 + step));
				const float fade = saturate(min(edge.x, edge.y) * 10.0) * (1.0 - float(i) / float(SSR_STEPS)) * within;
				return float4(a_color.SampleLevel(a_sampler, pixel / a_dimensions, 0).rgb, fade);
			}
		}
		return 0.0;
	}
}

#endif  // FLUID_FIELD_FALL_SHADING_HLSLI
