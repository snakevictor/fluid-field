// Fluid Field waterfall flow (D59): per-vertex and baked data for the falls sheets, textured along each mesh's solved flow
// (parting round rock, closing behind it) and cut under the pools (D36) and in rock. Enabled is 0 for every other draw.

#ifndef FLUID_FIELD_FALL_GROUND_HLSLI
#define FLUID_FIELD_FALL_GROUND_HLSLI

namespace FallGround
{
	cbuffer FallGroundCB : register(b13)
	{
		float2 Origin;         // World position of texel (0, 0)
		float InverseCell;     // Texels per world unit
		float Size;            // Texels per side
		uint Enabled;          // This draw is a grounded mesh
		uint DebugMode;        // Unused; kept for the layout
		float Time;            // Seconds
		float PoolFoamHeight;  // Unused; kept for the layout
		float2 Scroll;         // D67: the falls' shared scroll (texture coordinates, at most one repeat) and its rate per second
		float2 TexOffset;      // Vanilla's texture scroll, removed from the coordinates
		float3 CameraOffset;   // D38: FrameBuffer::CameraPosAdjust, for vertex shaders without it
		uint Whitening;        // Unused; kept for the layout
		uint VertexBase;       // D56: this mesh's first VertexClip entry, or NO_VERTEX_CLIP
		uint HideUnderwater;   // D36: don't draw falling water under the pools' surface
		float2 InverseScale;   // D57: undoes vanilla's animated texture scale (with TexOffset, its scroll)
		uint BakeOn;           // D59: this mesh has a FlowBake (not read here)
		float PreviousPhase;   // D85: last frame's Scroll.x, for the wobble's motion vectors
		float BlendAlpha;      // Unused; kept for the layout
		uint BakeVelocity;     // Unused; kept for the layout
		float4 BakeRect;       // D59: the bake's least u and v, and one over its extent
		float4 AerationLook;   // x height fallen before air or foam; y the bake's aeration to foam; z milk scale; w the aeration foam starts at
	};

	Texture2D<float2> Heights : register(t120);  // Captured rock (x, unused here) and the pools' surface (y, D36)
	/** D69: per falls vertex. a: rock clearance (D56, negative inside), seam fade (D67), height above the water below (D68), side flags (w). */
	struct FallVertex
	{
		float4 a;
		float4 b;  // D69: move across a seam's gap; D82: plus swell, mesh space; D85: w its streak
		float4 c;  // D85: wobble direction and size (its normal); w its run down the flow
		float4 d;  // x world units per unit of run (D188); y open water or air behind to the first rock (D106); z depth under its source water (D124, NO_WATER: starts mid-fall); w distance to the nearest other shown sheet (D128, NO_WATER: none within 64)
	};
	StructuredBuffer<FallVertex> VertexClip : register(t123);
	// D59: this mesh's flow bake, linked to the mesh above (FallSites::LinkBakes); five row bands (BakeRowsAt)
	Texture2D<float4> FlowBake : register(t124);

	static const uint NO_VERTEX_CLIP = 0xFFFFFFFF;
	/** D58: clamp on a vertex's distance above or below the cut, world units. */
	static const float CLIP_MOST = 48.0;
	/** D59: seconds of running to be mostly aerated. */
	static const float AERATION_SECONDS = 1.2;
	static const float NONE = -1.0e6;  // Below this, no ground is captured

	/** D36: the pools' surface height at a world position; false where there is none. */
	bool Pool(float2 a_position, out float a_height)
	{
		// D58: bilinear, or the cut steps at every texel
		a_height = 0.0;
		const float2 texel = (a_position - Origin) * InverseCell;
		const int2 base = int2(floor(texel));
		if (any(base < 0) || any(base + 1 >= int(Size)))
			return false;
		const float2 t = texel - float2(base);
		const float4 corner = float4((1.0 - t.x) * (1.0 - t.y), t.x * (1.0 - t.y), (1.0 - t.x) * t.y, t.x * t.y);
		const float4 h = float4(Heights.Load(int3(base, 0)).y, Heights.Load(int3(base + int2(1, 0), 0)).y, Heights.Load(int3(base + int2(0, 1), 0)).y,
			Heights.Load(int3(base + int2(1, 1), 0)).y);
		const float4 k = corner * (h > NONE);
		const float total = dot(k, 1.0);
		if (total < 0.2)
			return false;
		a_height = dot(k, h) / total;
		return true;
	}

	/** D47: depth of the cut under the pools' surface, so the pool's water overlaps it. */
	static const float POOL_CUT = 48.0;  // D176: deep enough that only foam shows at the waterline

	/**
	 * D38: a falls vertex's height above where it is buried (under the pools, or in rock, D56), for SV_ClipDistance.
	 * Every falls pass must cut at the same line, or hidden water stays in depth. CLIP_MOST on every other draw (D99).
	 * @param a_position Camera-relative world position.
	 * @param a_vertex SV_VertexID, for VertexClip.
	 */
	float ClipDistance(float3 a_position, uint a_vertex)
	{
		// D99: CLIP_MOST, not 1, so the pixel shaders' CutFade sees it wholly shown
		if (!Enabled)
			return CLIP_MOST;
		if (!HideUnderwater)
			return CLIP_MOST;
		const float3 position = a_position + CameraOffset;
		// D58: clamped so a far value never drags the cut across a triangle
		float distance = CLIP_MOST;
		// D71: pools cut a mesh only at its foot (a.w odd): near a lip the captured pool reaches over the drop
		float pool;
		const bool foot = VertexBase == NO_VERTEX_CLIP || fmod(VertexClip[VertexBase + a_vertex].a.w, 2.0) > 0.5;  // D100: +2 inward normal; D103: +4 covered inner side
		if (foot && Pool(position.xy, pool))
			distance = min(distance, position.z - (pool - POOL_CUT));
		// D56: rock per vertex from the game's collision (FallSites::ClipStep); a top-down map buried overhangs
		if (VertexBase != NO_VERTEX_CLIP)
			distance = min(distance, VertexClip[VertexBase + a_vertex].a.x);
		return clamp(distance, -CLIP_MOST, CLIP_MOST);
	}

	/**
	 * D99: world units over which falling water fades out toward its cut. Matches FallSites::INSIDE_MARGIN, so a vertex
	 * just in front of a leading mesh's face is wholly shown.
	 */
	static const float CUT_SOFT = 6.0;

	/** D99: a falls pixel's visibility from its interpolated ClipDistance: 0 at the cut, 1 past CUT_SOFT. */
	float CutFade(float a_clip)
	{
		return Enabled ? saturate(a_clip / CUT_SOFT) : 1.0;
	}

	/** D68: height above the water below, where there is none. */
	static const float NO_WATER = 1.0e4;

	/** D67: a falls vertex's seam fade and (D68) height above the water below; 1 and NO_WATER on every other draw. */
	float2 Fade(uint a_vertex)
	{
		if (!Enabled || VertexBase == NO_VERTEX_CLIP)
			return float2(1.0, NO_WATER);
		return VertexClip[VertexBase + a_vertex].a.yz;
	}

	/** D85: hash to [-1, 1). */
	float WobbleHash(float2 a_p)
	{
		a_p = frac(a_p * float2(0.1031, 0.1030));
		a_p += dot(a_p, a_p.yx + 33.33);
		return frac((a_p.x + a_p.y) * a_p.x) * 2.0 - 1.0;
	}

	/** D85: smooth noise, a_cells per texture repeat, wrapping each repeat (the scroll's phase wraps at 1). */
	float WobbleNoise(float2 a_p, float2 a_cells)
	{
		const float2 p = a_p * a_cells;
		const float2 i = floor(p);
		const float2 f = p - i;
		const float2 u = f * f * (3.0 - 2.0 * f);
		const float2 i0 = i - a_cells * floor(i / a_cells), i1 = (i + 1.0) - a_cells * floor((i + 1.0) / a_cells);
		return lerp(lerp(WobbleHash(i0), WobbleHash(float2(i1.x, i0.y)), u.x), lerp(WobbleHash(float2(i0.x, i1.y)), WobbleHash(i1), u.x), u.y);
	}

	/**
	 * D85: a wobble at (streak, run less phase): a_waves crests per texture repeat across the flow, broken by noise at
	 * a_bendCells and a_noiseCells. All whole, or it jumps at the phase wrap (D91). -0.15 to 0.85 (D88).
	 */
	float WobbleWave(float a_across, float a_run, float a_waves, float2 a_bendCells, float2 a_noiseCells)
	{
		const float wave = sin(6.2831853 * a_waves * a_run + 1.7 * WobbleNoise(float2(a_across, a_run), a_bendCells));
		const float wobble = 0.65 * wave + 0.35 * WobbleNoise(float2(a_across, a_run), a_noiseCells);
		// D88: mostly outward; dipping far sank the water into its banks
		return 0.5 * wobble + 0.35;
	}

	float Wobble(float a_across, float a_run)
	{
		return WobbleWave(a_across, a_run, 2.0, float2(2.0, 2.0), float2(3.0, 4.0));  // D90: shorter waves folded triangles
	}

	/** Shares of a sheet's vertical motion: a slow swell (one crest per repeat) and a finer, quicker bubbling over it. */
	static const float Z_SLOW_SHARE = 0.55;
	static const float Z_FINE_SHARE = 0.3;
	static const float Z_MICRO_SHARE = 0.18;  ///< and a smaller, quicker one still
	static const float Z_MOST = 25.0;         ///< world units its vertical motion reaches at most

	/** D124: a vertex's depth under the water its fall pours from; NO_WATER where none. */
	float BelowSource(uint a_vertex)
	{
		if (!Enabled || VertexBase == NO_VERTEX_CLIP)
			return NO_WATER;
		return VertexClip[VertexBase + a_vertex].d.z;
	}

	/** D128: distance to the nearest other shown falls sheet; NO_WATER where none is near. */
	float ToOtherSheet(uint a_vertex)
	{
		if (!Enabled || VertexBase == NO_VERTEX_CLIP)
			return NO_WATER;
		return VertexClip[VertexBase + a_vertex].d.w;
	}

	/** D104: world units of open water or air behind a falls vertex to the first rock, baked; large where there is none. */
	float Behind(uint a_vertex)
	{
		if (!Enabled || VertexBase == NO_VERTEX_CLIP)
			return 600.0;
		// D106: behind the visible face (against the authored normal)
		return VertexClip[VertexBase + a_vertex].d.y;
	}

	/**
	 * D100: 1 where the vertex's normal faces its mesh's outer side, -1 where it faces in. D103: the side is carried over
	 * the sheet (FallSites::ComputeSides), not taken per vertex.
	 */
	float OutwardSign(uint a_vertex)
	{
		if (!Enabled || VertexBase == NO_VERTEX_CLIP)
			return 1.0;
		return fmod(VertexClip[VertexBase + a_vertex].a.w, 4.0) >= 1.5 ? -1.0 : 1.0;
	}

	/** D103: 1 where another shown sheet's outer face lies behind the vertex's inner side, else 0. */
	float InnerCovered(uint a_vertex)
	{
		if (!Enabled || VertexBase == NO_VERTEX_CLIP)
			return 0.0;
		return VertexClip[VertexBase + a_vertex].a.w >= 3.5 ? 1.0 : 0.0;
	}

	/**
	 * D188: a falls vertex's wobble for lighting: x streak, y run less phase, z size (model units), w world units per
	 * unit of run (0: none)
	 */
	float4 WobbleAt(uint a_vertex, float a_phase)
	{
		if (!Enabled || VertexBase == NO_VERTEX_CLIP)
			return 0.0;
		const FallVertex fallVertex = VertexClip[VertexBase + a_vertex];
		return float4(fallVertex.b.w, fallVertex.c.w - a_phase, length(fallVertex.c.xyz), fallVertex.d.x);
	}

	/** The vertical bob at (a_across, a_run): the slow swell, the finer bubbling and the smaller one over it. */
	float BobLayers(float a_across, float a_run)
	{
		return Z_SLOW_SHARE * WobbleWave(a_across, a_run, 1.0, float2(2.0, 1.0), float2(3.0, 2.0)) +
		       Z_FINE_SHARE * WobbleWave(a_across * 3.0, a_run, 5.0, float2(5.0, 5.0), float2(7.0, 10.0)) +
		       Z_MICRO_SHARE * WobbleWave(a_across * 7.0, a_run, 11.0, float2(11.0, 11.0), float2(13.0, 18.0));
	}

	/** A wobble's vertical part (a_up: world up in model space), and the Z_MOST clamp's scale at a_run. */
	void SplitWobble(FallVertex a_vertex, float a_run, float3 a_up, out float3 o_vertical, out float o_clamp)
	{
		const float3 up = normalize(a_up);
		o_vertical = up * dot(a_vertex.c.xyz, up);
		const float reach = length(o_vertical) * abs(BobLayers(a_vertex.b.w, a_run));
		const float most = Z_MOST / length(a_up);  // (a_up's length is the model's scale)
		o_clamp = reach > most ? most / reach : 1.0;
	}

	/**
	 * D85: a falls vertex's move (seam gap, swell, wobble at phase a_phase), mesh space; 0 on other draws. a_up: world up in
	 * model space (the model-to-world matrix's third row); the wobble's vertical part bobs instead.
	 */
	float3 Shift(uint a_vertex, float a_phase, float3 a_up)
	{
		if (!Enabled || VertexBase == NO_VERTEX_CLIP)
			return 0.0;
		const FallVertex fallVertex = VertexClip[VertexBase + a_vertex];
		const float across = fallVertex.b.w, run = fallVertex.c.w - a_phase;
		if (dot(a_up, a_up) < 1e-8)
			return fallVertex.b.xyz + fallVertex.c.xyz * Wobble(across, run);
		float3 vertical;
		float clampScale;
		SplitWobble(fallVertex, run, a_up, vertical, clampScale);
		return fallVertex.b.xyz + (fallVertex.c.xyz - vertical) * Wobble(across, run) + vertical * BobLayers(across, run) * clampScale;
	}

	/**
	 * For FallShading::WobbleNormal: a vertex's wobble along its surface, and its vertical bob's share along its normal
	 * (a_normal: model space), model units; all in x when a_up is zero.
	 */
	float2 WobbleSizes(uint a_vertex, float a_phase, float3 a_up, float3 a_normal)
	{
		if (!Enabled || VertexBase == NO_VERTEX_CLIP)
			return 0.0;
		const FallVertex fallVertex = VertexClip[VertexBase + a_vertex];
		if (dot(a_up, a_up) < 1e-8)
			return float2(length(fallVertex.c.xyz), 0.0);
		float3 vertical;
		float clampScale;
		SplitWobble(fallVertex, fallVertex.c.w - a_phase, a_up, vertical, clampScale);
		const float facing = abs(dot(normalize(a_up), a_normal / max(length(a_normal), 1e-6)));
		return float2(length(fallVertex.c.xyz - vertical), length(vertical) * clampScale * facing);
	}

	/** A falls pixel's flow (Flow). */
	struct FlowSample
	{
		float2 uvA;       // Texture coordinates the water reads
		float3 velocity;  // World units per second, along the surface
		float foam;       // D59: baked foam: rock rims, water piling against and squeezing past rock
		float wake;       // Always 0: band 0's z holds the streak columns
		float runTime;    // D59: seconds run since entering this mesh
		float4 fields;    // D179: BakeFieldsAt: speed over vanilla's, swell, disturbance, aeration
		float3 frame;     // D186: world direction of the along coordinate (the normal maps' frame); zero where unknown
		float4 plunge;    // D188: x landing strength, y foam left after it; D189: z height fallen; D190: w progress of its own drop
		float stretch;    // D181: BakeStretchAt
		float streak;     // Streaks left (0 once the water has met rock)
		float3 contact;   // Distance to rock, land or the pool (x), that swept down the flow (y), world units; heave over hidden rock (z)
		float column;     // Streak columns, across
	};

	/**
	 * D59: row band a_half (of five) of the flow bake at a vanilla, unscrolled texture coordinate; band 0 holds across,
	 * along, streak column, foam. D80: cubic B-spline over 4x4 texels, as bilinear stepped the gradients (and mips).
	 */
	float4 BakeRowsAt(float2 a_uv, uint a_half)
	{
		uint width, height;
		FlowBake.GetDimensions(width, height);
		height /= 5;  // Bands: flow, fields, streak and frame, plunges, contact
		const float2 texel = (a_uv - BakeRect.xy) * BakeRect.zw * float2(width, height) - 0.5;
		const float2 cell = floor(texel);
		const float2 t = texel - cell;
		const float2 s = 1.0 - t;
		const float2 weights[4] = { s * s * s / 6.0, (4.0 - 6.0 * t * t + 3.0 * t * t * t) / 6.0, (4.0 - 6.0 * s * s + 3.0 * s * s * s) / 6.0, t * t * t / 6.0 };
		const int2 last = int2(width, height) - 1;
		float4 sum = 0.0;
		[unroll] for (int j = 0; j < 4; j++)
		{
			float4 row = 0.0;
			[unroll] for (int i = 0; i < 4; i++)
				row += weights[i].x * FlowBake.Load(int3(clamp(int2(cell) + int2(i - 1, j - 1), 0, last) + int2(0, a_half * height), 0));
			sum += weights[j].y * row;
		}
		return sum;
	}

	float4 BakeAt(float2 a_uv) { return BakeRowsAt(a_uv, 0); }

	/** D179: the water's fields: x speed over vanilla's, y swell, z disturbance, w aeration. */
	float4 BakeFieldsAt(float2 a_uv) { return BakeRowsAt(a_uv, 1); }

	/** Fastest the textures run (world units/s): the bake's 600+ spun the foam. */
	static const float UNIFIED_SPEED_MOST = 250.0;
	/** Share of its gap to UNIFIED_SPEED_MOST that slower water keeps (1 keeps it all). */
	static const float UNIFIED_SPEED_LAG = 0.45;

	/** Texture speed (world units/s): the bake's, capped, slower water brought up toward the cap. */
	float UnifiedSpeed(float a_speedField)
	{
		return UNIFIED_SPEED_MOST - max(UNIFIED_SPEED_MOST - a_speedField * 300.0, 0.0) * UNIFIED_SPEED_LAG;
	}

	/**
	 * D67: a falls pixel's flow: the bake's coordinates (streak across, run time along) less the shared scroll. Streaks
	 * bend round rock, speed up with the water and run on into the next mesh; only the scroll moves in time, so the
	 * texture cannot pulse or run back.
	 */
	FlowSample Flow(float2 a_uv, float3 a_position)
	{
		FlowSample result;
		const float2 uvStill = (a_uv - TexOffset) * InverseScale;
		const float4 baked = BakeAt(uvStill);
		result.foam = baked.w;
		result.wake = 0.0;
		result.runTime = max(baked.y, 0.0) / max(Scroll.y, 1e-3);
		result.fields = BakeFieldsAt(uvStill);
		const float4 third = BakeRowsAt(uvStill, 2);
		result.streak = saturate(third.x);
		result.contact = BakeRowsAt(uvStill, 4).xyz;
		result.frame = third.yzw;
		result.plunge = BakeRowsAt(uvStill, 3);
		// u: the flow's streamlines (the bake's columns); v: travel time, so every texture follows the flow. The stretch: travel
		// speed over the scroll against the uv's own scale
		float2 uvBase = baked.xy - float2(0.0, Scroll.x);
		uvBase.x = baked.z;
		result.stretch = clamp(UnifiedSpeed(result.fields.x) / max(Scroll.y * 968.0, 1e-3), 0.25, 4.0);
		result.uvA = uvBase;
		result.column = baked.z;
		result.velocity = 0.0;
		const float3 dpx = ddx(a_position);
		const float3 dpy = ddy(a_position);
		const float2 dux = ddx(baked.xy);
		const float2 duy = ddy(baked.xy);
		const float det = dux.x * duy.y - dux.y * duy.x;
		if (abs(det) > 1e-12)
			result.velocity = (dpy * dux.x - dpx * duy.x) / det * Scroll.y;
		// the baked flow direction at the bake's speed (world units/s)
		if (dot(result.frame, result.frame) > 0.25)
			result.velocity = normalize(result.frame) * UnifiedSpeed(result.fields.x);
		return result;
	}
}

#endif  // FLUID_FIELD_FALL_GROUND_HLSLI
