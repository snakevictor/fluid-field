// Fluid Field waterfall grounding (D26) and flow (D28, D32): vanilla's falling-water meshes bent onto
// the rock they run over (effect-shaded layers only: the lit surface is drawn after a depth prepass
// its position must match exactly), their textures carried by the falls' steady flow as a flowmap so
// they part round rock and close up behind it, foamed where the flow piles against rock or squeezes
// past it, and softened where they cut into rock. FluidField binds the constants, the captured ground
// (FallSites::BuildGroundMap) and the flow (FallSites::BuildStreamMap) only for the draws of grounded
// meshes; Enabled is 0 for every other effect draw.

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
		float Offset;          // How far above the rock the water lies
		float ContactWidth;    // World units over which the water fades where it cuts into rock
		float LiftMost;        // Water deeper than this inside rock is left there (a boulder hides it); shallower is lifted onto it
		float FoamStrength;    // How white the foam is
		uint DebugMode;        // 0 off; 1: speed as red, foam as green, water as blue; 2: the flow's direction; 3: grounding with vanilla's own texture
		float Time;            // Seconds
		float FlowSpeed;       // World units per second of free-flowing water (speed 1)
		float FlowPeriod;      // Seconds per cycle of the flowmap's two phases
		float FlowMost;        // Most texture repeats the texture moves per cycle
		float2 Scroll;         // D56: texture coordinates per second vanilla's own scroll moves this mesh's texture (toward +v: downstream)
		float2 TexOffset;      // This mesh's texture scroll (vanilla's), taken back out of its coordinates
		uint VertexBase;       // D56: this mesh's first value in VertexClip, or NO_VERTEX_CLIP
		float Translucency;    // D53: how see-through the falls' thin water is (0: opaque; only on blended draws)
		float2 TexScale;       // This mesh's world units per texture repeat along u and v (D35); 0 unknown
		uint HideUnderwater;   // Falling water under the pools' surface is not drawn (D36)
		float PoolFoamHeight;  // World units above the water below over which the falls foam into it
		float3 CameraOffset;   // FrameBuffer::CameraPosAdjust, for the vertex shaders that have none (D38)
		uint Whitening;        // The falls whiten with their foam and speed (D34); 0 for the debug comparison (D39)
		uint HideDraw;         // D56: this draw shows nothing of the falls (a blended mesh's depth pre-pass)
		float2 InverseScale;   // D57: undoes vanilla's animated texture scale (its slow zoom), with TexOffset its scroll
		uint BakeOn;           // D59: this mesh has a flow bake (FlowBake)
		float4 BakeRect;       // D59: the bake's texture coordinates: its least u and v, and one over its extent
	};

	Texture2D<float2> Heights : register(t120);  // The captured rock, and the pools' surface (D36)
	Texture2D<float> SceneDepth : register(t121);
	Texture2D<float4> Stream : register(t122);  // Velocity (x, y; 1 free), foam plus 2 on water (0 elsewhere), head (world height, D33)
	StructuredBuffer<float> VertexClip : register(t123);
	// D59: this mesh's flow, solved on its own texture grid (FallSites::SolveBake): across and down in texture
	// coordinates per unit of vanilla's scroll, foam (rock, edges), and seconds the water has run
	Texture2D<float4> FlowBake : register(t124);  // D56: per vertex of the falls' lit meshes, how far it stands out of the rock (negative inside)

	static const uint NO_VERTEX_CLIP = 0xFFFFFFFF;
	/** D58: the most a vertex counts as above or under the cut, world units. */
	static const float CLIP_MOST = 48.0;
	/** D56: vanilla's scroll when this mesh's has not been measured yet (texture coordinates per second; the bodies run 0.12 to 0.17). */
	static const float DEFAULT_SCROLL = 0.15;
	/** D59: the most a deviation moves the texture per cycle (texture repeats): more blurs the cross-fade. */
	static const float DEVIATION_MOST = 0.3;
	/** D59: seconds of running for the water to be most of the way aerated, and how white aerated water gets. */
	static const float AERATION_SECONDS = 1.2;
	static const float AERATION_WHITE = 0.3;
	static const float NONE = -1.0e6;  // Below this, no ground is captured

	/** The captured ground under a world position, bilinear; false where any texel around it is unknown. */
	bool Height(float2 a_position, out float a_height)
	{
		a_height = 0.0;
		const float2 texel = (a_position - Origin) * InverseCell;
		const int2 base = int2(floor(texel));
		if (any(base < 0) || any(base + 1 >= int(Size)))
			return false;
		const float2 t = texel - float2(base);
		const float h00 = Heights.Load(int3(base, 0)).x;
		const float h10 = Heights.Load(int3(base + int2(1, 0), 0)).x;
		const float h01 = Heights.Load(int3(base + int2(0, 1), 0)).x;
		const float h11 = Heights.Load(int3(base + int2(1, 1), 0)).x;
		if (min(min(h00, h10), min(h01, h11)) < NONE)
			return false;
		a_height = lerp(lerp(h00, h10, t.x), lerp(h01, h11, t.x), t.y);
		return true;
	}

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

	/** D37: falling water this far under the captured rock and ground is buried in it. */
	static const float BURIED = 120.0;  // D49: only deeply buried water: the ground map sees overhangs from above, and the rock's depth hides the rest
	/** D47: how far round a point the ground is looked at for its lowest (the map is a 16-unit top-down grid). */
	static const float BURIED_REACH = 14.0;
	/** D47: the falls are cut this far under the pools' surface, so the pool's water overlaps the cut. */
	static const float POOL_CUT = 8.0;  // D49: under the pools the falls are hidden, the foam at the waterline covering the line; D58: with the pool read bilinear, shallow again

	/** D47: the lowest captured ground round a world position: at a cliff edge, the cliff foot's, not its top's. */
	bool LowestGround(float2 a_position, out float a_height)
	{
		float h;
		if (!Height(a_position, a_height))
			return false;
		[unroll] for (int i = 0; i < 4; i++)
		{
			const float2 offset = float2(i == 0 ? BURIED_REACH : i == 1 ? -BURIED_REACH :
																		  0.0,
				i == 2 ? BURIED_REACH : i == 3 ? -BURIED_REACH :
												 0.0);
			a_height = Height(a_position + offset, h) ? min(a_height, h) : a_height;
		}
		return true;
	}

	/** Whether falling water at a world position runs on under the pools or into the rock and ground, where it is not drawn (D36, D37). */
	bool Buried(float3 a_position)
	{
		if (!HideUnderwater)
			return false;
		float pool, ground;
		if (Pool(a_position.xy, pool) && a_position.z < pool - POOL_CUT)
			return true;
		return LowestGround(a_position.xy, ground) && a_position.z < ground - BURIED;
	}

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
		if (HideDraw)
			return -1.0;
		if (!HideUnderwater)
			return 1.0;
		const float3 position = a_position + CameraOffset;
		// D58: held within CLIP_MOST, so a far value never drags the cut across a triangle onto its other corner
		float distance = CLIP_MOST;
		float pool;
		if (Pool(position.xy, pool))
			distance = min(distance, position.z - (pool - POOL_CUT));
		// D56: the rock per vertex, tested against the game's collision (FallSites::ClipStep): the top-down
		// ground map called everything under an overhang buried, and holed the falls there
		if (VertexBase != NO_VERTEX_CLIP)
			distance = min(distance, VertexClip[VertexBase + a_vertex]);
		return clamp(distance, -CLIP_MOST, CLIP_MOST);
	}

	/**
	 * The falls' flow at a world position, bilinear over its water texels only: velocity (x, y),
	 * foam, and how much water is around (0 to 1); and the head the water there has fallen from.
	 */
	float4 StreamAt(float2 a_position, out float a_head)
	{
		a_head = 0.0;
		const float2 texel = (a_position - Origin) * InverseCell;
		const int2 base = int2(floor(texel));
		if (any(base < 0) || any(base + 1 >= int(Size)))
			return 0.0;
		const float2 t = texel - float2(base);
		float4 s[4];
		s[0] = Stream.Load(int3(base, 0));
		s[1] = Stream.Load(int3(base + int2(1, 0), 0));
		s[2] = Stream.Load(int3(base + int2(0, 1), 0));
		s[3] = Stream.Load(int3(base + int2(1, 1), 0));
		const float4 corner = float4((1.0 - t.x) * (1.0 - t.y), t.x * (1.0 - t.y), (1.0 - t.x) * t.y, t.x * t.y);
		// Weighted by water, so rock beside the flow does not slow it toward 0
		float3 value = 0.0;
		float total = 0.0, water = 0.0;
		[unroll] for (int i = 0; i < 4; i++)
		{
			const float wet = s[i].z >= 1.5 ? 1.0 : 0.0;
			const float k = corner[i] * wet;
			value += float3(s[i].xy, s[i].z - 2.0) * k;
			a_head += s[i].w * k;
			total += k;
			water += corner[i] * wet;
		}
		if (total <= 1e-4)
			return 0.0;
		a_head /= total;
		return float4(value / total, water);
	}

	float4 StreamAt(float2 a_position)
	{
		float head;
		return StreamAt(a_position, head);
	}

	/**
	 * A vertex of falling water, at a world position, lifted out of the bed where it clips just under
	 * it. It is never drawn down: vanilla's water covers the grass and clutter on the rock beneath it.
	 * Water deep inside rock is a boulder's: it is left there, hidden, so the boulder stays dry.
	 */
	float3 Ground(float3 a_position)
	{
		float ground;
		if (!Height(a_position.xy, ground))
			return a_position;
		const float surface = ground + Offset;
		if (a_position.z < surface && surface - a_position.z <= LiftMost)
			a_position.z = surface;
		return a_position;
	}

	/** A pixel of falling water's texture coordinates under the flowmap (Flow). */
	struct FlowSample
	{
		float2 uvA;       // The first phase
		float2 uvB;       // The second phase, half a cycle later
		float blendB;     // How much of the second phase shows
		float strength;   // 0: vanilla's own texture (coordinates unchanged), 1: the flowmap in full
		float3 velocity;  // World units per second, along the surface
		float4 stream;    // StreamAt
		float foam;       // D59: the bake's foam: where the water piles against rock, squeezes past it, and frays at its edges
		float runTime;    // D59: seconds the water has run since it came onto this mesh (0 without a bake)
		float pace;       // D59: the water's speed against vanilla's (1 without a bake)
	};

	/**
	 * A pixel of falling water carried by the falls' flow (D32).
	 *
	 * D56: forward is the mesh's own: every falls mesh's v increases downstream and vanilla scrolls its
	 * texture toward +v, faster in world terms where the artist stretched the texture down the drops.
	 * D58: that forward is one continuous scroll, a single sample: the two phases of a flowmap (D32) each
	 * show a different patch of the texture, and cross-fading them where the flow is the same everywhere
	 * only marbled the curtains. The phases carry only a deviation from it (a_deviation, texture
	 * coordinates per second on top of the scroll), and fade out where there is none.
	 */
	/** D59: this mesh's flow bake at a texture coordinate (vanilla's own, unscrolled), bilinear; vanilla's forward where there is none. */
	float4 BakeAt(float2 a_uv)
	{
		if (!BakeOn)
			return float4(0.0, 1.0, 0.0, 0.0);
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

	FlowSample Flow(float2 a_uv, float3 a_position)
	{
		FlowSample result;
		result.stream = StreamAt(a_position.xy);
		const float2 uvStill = (a_uv - TexOffset) * InverseScale;
		const float forward = Scroll.y > 1e-3 ? Scroll.y * InverseScale.y : DEFAULT_SCROLL;
		// D59: the mesh's own flow, solved on its texture grid, as a deviation from vanilla's forward
		const float4 baked = BakeAt(uvStill);
		const float2 a_deviation = float2(baked.x, baked.y - 1.0) * forward;
		result.foam = baked.z;
		result.runTime = baked.w;
		result.pace = length(baked.xy);
		const float2 uvBase = uvStill - float2(0.0, frac(forward * Time));
		result.uvA = uvBase;
		result.uvB = uvBase;
		result.blendB = 0.0;
		result.strength = 1.0;
		result.velocity = 0.0;

		// The surface's world motion per texture coordinate along u and along v (v runs downstream)
		const float3 dpx = ddx(a_position);
		const float3 dpy = ddy(a_position);
		const float2 dux = ddx(a_uv);
		const float2 duy = ddy(a_uv);
		const float det = dux.x * duy.y - dux.y * duy.x;
		if (abs(det) > 1e-12) {
			const float3 tu = (dpx * duy.y - dpy * dux.y) / det;
			const float3 tv = (dpy * dux.x - dpx * duy.x) / det;
			result.velocity = tu * a_deviation.x + tv * (forward + a_deviation.y);
		}

		// The deviation in two phases, staggered over the surface so the resets do not all land at once.
		// D59: always cross-faded in full (fading the second phase out where the deviation is small left the
		// first one's reset showing: the texture jumped back every cycle), and both phases are the same patch
		// of the texture, only a deviation apart: they never marble (D58), and are not contrast boosted
		float2 travel = a_deviation * FlowPeriod;
		const float most = length(travel);
		const float travelMost = min(FlowMost, DEVIATION_MOST);
		travel *= most > travelMost ? travelMost / most : 1.0;
		[branch] if (most > 1e-4)
		{
			const float stagger = 0.25 * (sin(dot(a_position, float3(0.0021, 0.0027, 0.0013))) + sin(dot(a_position, float3(-0.0017, 0.0012, 0.0024))));
			const float cycle = Time / FlowPeriod + stagger;
			const float phaseA = frac(cycle);
			const float phaseB = frac(cycle + 0.5);
			result.uvA = uvBase - travel * (phaseA - 0.5);
			result.uvB = uvBase - travel * (phaseB - 0.5);
			result.blendB = abs(1.0 - 2.0 * phaseA);
		}
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
	 * it in the flowmap's two phases (the same texture coordinates as the water's texture), so they
	 * stream with the water. 0 to 1, high on the filaments. The texture runs along u.
	 */
	float Lace(FlowSample a_flow)
	{
		const float2 stretch = float2(0.7, 3.2);
		const float a = LaceNoise(a_flow.uvA * stretch) * 0.6 + LaceNoise(a_flow.uvA * stretch * 2.7 + 3.1) * 0.4;
		const float b = LaceNoise(a_flow.uvB * stretch + 11.7) * 0.6 + LaceNoise(a_flow.uvB * stretch * 2.7 + 7.9) * 0.4;
		const float n = lerp(a, b, a_flow.blendB);
		// Ridges of the noise: thin lines where it crosses its middle
		return 1.0 - saturate(abs(n - 0.5) * 2.6);
	}

	/**
	 * D53: how thick the falling water is, 0 to 1, in patches: broad soft blobs carried by the flow in
	 * its two phases, so thin, see-through sheets and thick runs stream down the falls together.
	 */
	float Thickness(FlowSample a_flow)
	{
		const float2 stretch = float2(0.45, 0.9);
		const float a = LaceNoise(a_flow.uvA * stretch + 5.3) * 0.65 + LaceNoise(a_flow.uvA * stretch * 2.3 + 1.7) * 0.35;
		const float b = LaceNoise(a_flow.uvB * stretch + 17.1) * 0.65 + LaceNoise(a_flow.uvB * stretch * 2.3 + 9.4) * 0.35;
		return lerp(a, b, a_flow.blendB);
	}

	/**
	 * How much of the falling water shows where it cuts into rock, from the scene's depth at the pixel:
	 * it fades over the last ContactWidth before the rock, so intersections are soft. Water lying on
	 * its bed sits Offset above it and stays whole.
	 */
	float Visibility(float2 a_pixel, float a_depth)
	{
		const float sceneDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(a_pixel, 0)));
		const float depth = SharedData::GetScreenDepth(a_depth);
		return saturate((sceneDepth - depth) / ContactWidth);
	}
}

#endif  // FLUID_FIELD_FALL_GROUND_HLSLI
