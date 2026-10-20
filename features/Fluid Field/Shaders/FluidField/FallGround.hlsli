// Fluid Field waterfall flow (D28, D32, D59): vanilla's lit falling-water meshes, their textures carried
// by each mesh's own solved flow so they part round rock and close up behind it, foamed where the water
// strikes rock, and cut where they run under the pools (D36, D56). FluidField binds the constants, the
// pools' surface (FallSites::BuildPoolMap) and each mesh's flow bake only for the draws of grounded
// meshes; Enabled is 0 for every other draw.

#ifndef FLUID_FIELD_FALL_GROUND_HLSLI
#define FLUID_FIELD_FALL_GROUND_HLSLI

namespace FallGround
{
	cbuffer FallGroundCB : register(b13)
	{
		float2 Origin;         // World position of texel (0, 0)
		float InverseCell;     // Texels per world unit
		float Size;            // Texels along each side
		uint Enabled;          // This draw is a grounded mesh
		uint DebugMode;        // 0 off; 1: the mesh's bake; 2: the flow's direction; 3: vanilla's own texture
		float Time;            // Seconds
		float PoolFoamHeight;  // World units above the water below over which the falls foam into it
		float2 Scroll;         // D67: the falls' one scroll (texture coordinates, a repeat at most), and its rate per second
		float2 TexOffset;      // This mesh's texture scroll (vanilla's), taken back out of its coordinates
		float3 CameraOffset;   // FrameBuffer::CameraPosAdjust, for the vertex shaders that have none (D38)
		uint Whitening;        // The falls whiten with their foam and speed (D34); 0 for the debug comparison (D39)
		uint VertexBase;       // D56: this mesh's first value in VertexClip, or NO_VERTEX_CLIP
		uint HideUnderwater;   // Falling water under the pools' surface is not drawn (D36)
		float2 InverseScale;   // D57: undoes vanilla's animated texture scale (its slow zoom), with TexOffset its scroll
		uint BakeOn;           // D59: this mesh has a flow bake (FlowBake)
		float3 GroundPad;
		float4 BakeRect;  // D59: the bake's texture coordinates: its least u and v, and one over its extent
	};

	Texture2D<float2> Heights : register(t120);  // The captured rock (x, not read here), and the pools' surface (y, D36)
	// D56: per vertex of the falls' lit meshes, how far it stands out of the rock (negative inside); D67: and how
	// much of it shows where it overlaps a mesh leading it (its seam edge fades, dithered, in both passes)
	/** D69: per vertex of the falls' lit meshes: the rock cut, fade, height above the water below (a), and its move across a seam's gap, in its mesh's space (b). */
	struct FallVertex
	{
		float4 a;
		float4 b;
	};
	StructuredBuffer<FallVertex> VertexClip : register(t123);  // D68: and how far it stands above the water it falls into (z)
	// D59, D67: this mesh's flow, solved on its own texture grid and linked to the mesh above it (FallSites::LinkBakes):
	// the texture coordinates its water reads (across: its streak; along: its run time at the shared scroll), a
	// rock's wake, and foam (its rim, water piling against it)
	Texture2D<float4> FlowBake : register(t124);

	static const uint NO_VERTEX_CLIP = 0xFFFFFFFF;
	/** D58: the most a vertex counts as above or under the cut, world units. */
	static const float CLIP_MOST = 48.0;
	/** D59: seconds of running for the water to be most of the way aerated, and how white aerated water gets. */
	static const float AERATION_SECONDS = 1.2;
	static const float AERATION_WHITE = 0.3;
	static const float NONE = -1.0e6;  // Below this, no ground is captured
	/** D78: world units per second the flow direction view (DebugMode 2) shows at full brightness. */
	static const float DEBUG_FLOW_SPEED = 420.0;

	/** The pools' surface over a world position (the water the falls leave and land in), or false where there is none (D36). */
	bool Pool(float2 a_position, out float a_height)
	{
		// D58: bilinear over the pool's own texels: the nearest texel stepped the cut every 16 units, a saw-tooth
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

	/** D47: the falls are cut this far under the pools' surface, so the pool's water overlaps the cut. */
	static const float POOL_CUT = 8.0;  // D49: under the pools the falls are hidden, the foam at the waterline covering the line; D58: with the pool read bilinear, shallow again

	/**
	 * D38: how far a vertex of falling water stands above where it is buried (under the pools' surface,
	 * or in the rock, D56), for SV_ClipDistance. The depth pre-pass (Utility.hlsl) and
	 * the lit pass (Lighting.hlsl) cut the same triangles at the same line, so nothing is left behind in
	 * depth where the water is hidden. 1 on every other draw.
	 * @param a_position Camera-relative world position (add CameraOffset for the absolute one).
	 * @param a_vertex The vertex's index in its mesh (SV_VertexID), for VertexClip.
	 */
	float ClipDistance(float3 a_position, uint a_vertex)
	{
		if (!Enabled)
			return 1.0;
		if (!HideUnderwater)
			return 1.0;
		const float3 position = a_position + CameraOffset;
		// D58: held within CLIP_MOST, so a far value never drags the cut across a triangle onto its other corner
		float distance = CLIP_MOST;
		// D71: the pools cut a mesh only at its foot (VertexClip's a.w): near a lip the captured pool reaches over
		// the drop, and cut a hole in the curtain just under it where a rock stands behind
		float pool;
		const bool foot = VertexBase == NO_VERTEX_CLIP || VertexClip[VertexBase + a_vertex].a.w > 0.5;
		if (foot && Pool(position.xy, pool))
			distance = min(distance, position.z - (pool - POOL_CUT));
		// D56: the rock per vertex, tested against the game's collision (FallSites::ClipStep): the top-down
		// ground map called everything under an overhang buried, and holed the falls there
		if (VertexBase != NO_VERTEX_CLIP)
			distance = min(distance, VertexClip[VertexBase + a_vertex].a.x);
		return clamp(distance, -CLIP_MOST, CLIP_MOST);
	}

	/** D68: a vertex's height above the water it falls into, where there is none. */
	static const float NO_WATER = 1.0e4;

	/**
	 * D67: how much of a vertex of falling water shows (its seam edges over a mesh leading it fade), and (D68) how
	 * far it stands above the water it falls into; 1 and NO_WATER on every other draw.
	 */
	float2 Fade(uint a_vertex)
	{
		if (!Enabled || VertexBase == NO_VERTEX_CLIP)
			return float2(1.0, NO_WATER);
		return VertexClip[VertexBase + a_vertex].a.yz;
	}

	/** D69: how far a vertex of falling water moves, in its mesh's space, to close a seam's gap; 0 on every other draw. The same in both depth passes. */
	float3 Shift(uint a_vertex)
	{
		if (!Enabled || VertexBase == NO_VERTEX_CLIP)
			return 0.0;
		return VertexClip[VertexBase + a_vertex].b.xyz;
	}

	/**
	 * D67: whether a pixel of falling water is dithered out at its fade (the mesh leading it shows there instead).
	 * The same pattern in the depth pre-pass and the lit pass (the pixel and the frame, not the draw), which TAA
	 * resolves into a blend. Lit falls are opaque, so this is how two of them blend.
	 */
	bool Dithered(float2 a_pixel, float a_fade)
	{
		if (a_fade >= 0.999)
			return false;
		const float frame = float(uint(Time * 60.0) & 63u);
		const float2 p = a_pixel + 5.588238 * frame;
		const float noise = frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
		return noise >= smoothstep(0.0, 1.0, a_fade);
	}

	/** A pixel of falling water's texture coordinates under the flowmap (Flow). */
	struct FlowSample
	{
		float2 uvA;       // The texture coordinates the water reads
		float3 velocity;  // World units per second, along the surface
		float foam;       // D59: the bake's foam: the rock's rim, the water piling against it and squeezing past it
		float wake;       // D60: the wake of a rock above, fading down the streaks
		float runTime;    // D59: seconds the water has run since it came onto this mesh (0 without a bake)
	};

	/** D59: this mesh's flow bake at a texture coordinate (vanilla's own, unscrolled), bilinear; 0 where there is none. D67: across, along, wake, foam. */
	float4 BakeAt(float2 a_uv)
	{
		if (!BakeOn)
			return 0.0;
		uint width, height;
		FlowBake.GetDimensions(width, height);
		const float2 texel = (a_uv - BakeRect.xy) * BakeRect.zw * float2(width, height) - 0.5;
		const int2 base = clamp(int2(floor(texel)), 0, int2(width, height) - 2);
		const float2 t = saturate(texel - float2(base));
		const float4 a = FlowBake.Load(int3(base, 0));
		const float4 b = FlowBake.Load(int3(base + int2(1, 0), 0));
		const float4 c = FlowBake.Load(int3(base + int2(0, 1), 0));
		const float4 d = FlowBake.Load(int3(base + int2(1, 1), 0));
		return lerp(lerp(a, b, t.x), lerp(c, d, t.x), t.y);
	}

	/**
	 * D67: a pixel of falling water's texture coordinates: the bake's (its streak across, its run time along)
	 * less the falls' one scroll, integrated on the CPU. The streaks bend round the rock and close behind it,
	 * the texture runs faster where the water does, and runs on from one mesh into the next (both read the
	 * same coordinates where they meet); nothing but the scroll moves in time, so it cannot pulse or run back.
	 * Without a bake, vanilla's own coordinates under the same scroll.
	 */
	FlowSample Flow(float2 a_uv, float3 a_position)
	{
		FlowSample result;
		const float2 uvStill = (a_uv - TexOffset) * InverseScale;
		const float4 baked = BakeAt(uvStill);
		result.foam = baked.w;
		result.wake = baked.z;
		result.runTime = BakeOn ? max(baked.y, 0.0) / max(Scroll.y, 1e-3) : 0.0;
		const float2 uvBase = (BakeOn ? baked.xy : uvStill) - float2(0.0, Scroll.x);
		result.uvA = uvBase;
		result.velocity = 0.0;
		const float3 dpx = ddx(a_position);
		const float3 dpy = ddy(a_position);
		const float2 dux = ddx(a_uv);
		const float2 duy = ddy(a_uv);
		const float det = dux.x * duy.y - dux.y * duy.x;
		if (abs(det) > 1e-12)
			result.velocity = (dpy * dux.x - dpx * duy.x) / det * Scroll.y;
		return result;
	}

	float LaceHash(float2 a_p)
	{
		a_p = frac(a_p * float2(0.1031, 0.1030));
		a_p += dot(a_p, a_p.yx + 33.33);
		return frac((a_p.x + a_p.y) * a_p.x);
	}

	float LaceNoise(float2 a_p)
	{
		const float2 i = floor(a_p);
		const float2 f = a_p - i;
		const float2 u = f * f * (3.0 - 2.0 * f);
		return lerp(lerp(LaceHash(i), LaceHash(i + float2(1, 0)), u.x), lerp(LaceHash(i + float2(0, 1)), LaceHash(i + float2(1, 1)), u.x), u.y);
	}

	/**
	 * D42: foam as lace, the way white water looks: thin filaments, stretched along the flow, carried by
	 * it (the same texture coordinates as the water's texture), so they stream with the water. 0 to 1,
	 * high on the filaments. The texture runs along u.
	 */
	float Lace(FlowSample a_flow)
	{
		const float2 stretch = float2(0.7, 3.2);
		const float n = LaceNoise(a_flow.uvA * stretch) * 0.6 + LaceNoise(a_flow.uvA * stretch * 2.7 + 3.1) * 0.4;
		// Ridges of the noise: thin lines where it crosses its middle
		return 1.0 - saturate(abs(n - 0.5) * 2.6);
	}
}

#endif  // FLUID_FIELD_FALL_GROUND_HLSLI
