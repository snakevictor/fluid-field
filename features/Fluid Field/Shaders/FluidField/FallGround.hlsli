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
		float GroundPad;
	};

	Texture2D<float2> Heights : register(t120);  // The captured rock, and the pools' surface (D36)
	Texture2D<float> SceneDepth : register(t121);
	Texture2D<float4> Stream : register(t122);  // Velocity (x, y; 1 free), foam plus 2 on water (0 elsewhere), head (world height, D33)
	// D56, D57: per vertex of the falls' lit meshes: how far it stands out of the rock (negative inside), and
	// how far it is moved in its own model space to reach the rock where vanilla's open edge falls short
	StructuredBuffer<float4> VertexData : register(t123);

	static const uint NO_VERTEX_CLIP = 0xFFFFFFFF;
	/** D56: vanilla's scroll when this mesh's has not been measured yet (texture coordinates per second; the bodies run 0.12 to 0.17). */
	static const float DEFAULT_SCROLL = 0.15;
	/** D56: the solve slows or quickens vanilla's flow within these, as a fraction of it: stalled against rock, squeezed past it. */
	static const float SLOWEST_FLOW = 0.5;
	static const float FASTEST_FLOW = 1.6;
	/**
	 * D57: the most the solve turns the flow aside, as across-to-down: little on steep water (the solve,
	 * from above, knows nothing of a curtain's face), much on flat runs, where it follows the channel
	 * round rock (3.7: 75 degrees). It never turns it back.
	 */
	static const float STEEP_TURN = 0.0;
	static const float FLAT_TURN = 3.7;
	/** D57: the surface's upward normal from which the solve leads (flat water) and below which it has no say (a curtain). */
	static const float SOLVE_FROM = 0.45;
	static const float SOLVE_TO = 0.8;

	/** D57: how far a vertex of the falls' lit meshes is moved in model space (its open edge reaching the rock), or 0. */
	float3 VertexShift(uint a_vertex)
	{
		if (!Enabled || VertexBase == NO_VERTEX_CLIP)
			return 0.0;
		return VertexData[VertexBase + a_vertex].yzw;
	}

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
		const int2 texel = int2(floor((a_position - Origin) * InverseCell + 0.5));
		a_height = 0.0;
		if (any(texel < 0) || any(texel >= int(Size)))
			return false;
		a_height = Heights.Load(int3(texel, 0)).y;
		return a_height > NONE;
	}

	/** D37: falling water this far under the captured rock and ground is buried in it. */
	static const float BURIED = 120.0;  // D49: only deeply buried water: the ground map sees overhangs from above, and the rock's depth hides the rest
	/** D47: how far round a point the ground is looked at for its lowest (the map is a 16-unit top-down grid). */
	static const float BURIED_REACH = 14.0;
	/** D47: the falls are cut this far under the pools' surface, so the pool's water overlaps the cut. */
	static const float POOL_CUT = 20.0;  // D49: under the pools the falls are hidden, the dense foam at the waterline covering the line; D57: deep enough that the shallow water's ripples never open a gap under the cut

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
		float distance = 1.0e6;
		float pool;
		if (Pool(position.xy, pool))
			distance = min(distance, position.z - (pool - POOL_CUT));
		// D56: the rock per vertex, tested against the game's collision (FallSites::ClipStep): the top-down
		// ground map called everything under an overhang buried, and holed the falls there
		if (VertexBase != NO_VERTEX_CLIP)
			distance = min(distance, VertexData[VertexBase + a_vertex].x);
		return distance;
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
	};

	/**
	 * A pixel of falling water carried by the falls' flow (D32), as a flowmap: its texture moves in two
	 * phases half a cycle apart that cross-fade, each reset once a cycle before it stretches too far.
	 *
	 * D56: forward is the mesh's own: every falls mesh's v increases downstream and vanilla scrolls its
	 * texture toward +v, faster in world terms where the artist stretched the texture down the drops. That
	 * scroll is the base, always forward however the mesh arches or folds. The solved flow, lifted onto the
	 * surface, only turns it aside (round rock, at most MOST_TURN) and slows or quickens it (stalled
	 * against rock, squeezed past it); it never turns it back.
	 */
	FlowSample Flow(float2 a_uv, float3 a_position, float3 a_normal, float3 a_tangent, float3 a_bitangent)
	{
		FlowSample result;
		result.stream = StreamAt(a_position.xy);
		const float3 dpx = ddx(a_position);
		const float3 dpy = ddy(a_position);
		const float2 dux = ddx(a_uv);
		const float2 duy = ddy(a_uv);
		const float2 uvStill = (a_uv - TexOffset) * InverseScale;
		result.uvA = uvStill;
		result.uvB = uvStill + float2(0.31, 0.17);
		result.blendB = 0.0;
		result.strength = 0.0;
		result.velocity = 0.0;
		const float det = dux.x * duy.y - dux.y * duy.x;
		if (abs(det) <= 1e-12)
			return result;
		// The surface's world motion per texture coordinate along u and along v (v runs downstream)
		const float3 tu = (dpx * duy.y - dpy * dux.y) / det;
		const float3 tv = (dpy * dux.x - dpx * duy.x) / det;

		// The solved flow lifted onto the surface, measured across (along u) and down (along v) it, in world
		// units: through the mesh's smooth frame where the draw has it (D35), else through this triangle's
		const bool smoothFrame = dot(a_normal, a_normal) > 0.25 && TexScale.x > 0.0 && TexScale.y > 0.0;
		float3 normal = smoothFrame ? normalize(a_normal) : normalize(cross(dpx, dpy));
		normal *= normal.z < 0.0 ? -1.0 : 1.0;
		const float2 solved = result.stream.xy;
		const float water = smoothstep(0.3, 0.8, result.stream.w);
		const float3 lifted = float3(solved, -dot(normal.xy, solved) / max(normal.z, 0.15));
		float3 alongU = tu / max(length(tu), 1e-6);
		float3 alongV = tv / max(length(tv), 1e-6);
		float aspect = length(tv) / max(length(tu), 1e-6);  // World units per v over per u
		if (smoothFrame) {
			const float3 a = normalize(a_tangent);
			const float3 b = normalize(a_bitangent);
			const bool swapped = abs(dot(tu, b)) > abs(dot(tu, a));
			alongU = (swapped ? b : a) * (dot(swapped ? b : a, tu) < 0.0 ? -1.0 : 1.0);
			alongV = (swapped ? a : b) * (dot(swapped ? a : b, tv) < 0.0 ? -1.0 : 1.0);
			aspect = TexScale.y / TexScale.x;
		}
		const float across = dot(lifted, alongU);
		const float down = dot(lifted, alongV);

		// Vanilla's forward, slowed or quickened by the solve and turned aside by it, never back
		// D57: the solve leads on flat water only: from above it sees a curtain's face edge-on, and its turn
		// there sheared the texture into marble
		const float say = water * smoothstep(SOLVE_FROM, SOLVE_TO, normal.z);
		const float mostTurn = lerp(STEEP_TURN, FLAT_TURN, say);
		const float forward = (Scroll.y > 1e-3 ? Scroll.y : DEFAULT_SCROLL) * lerp(1.0, clamp(length(solved), SLOWEST_FLOW, FASTEST_FLOW), say);
		const float turn = say * clamp(across / max(down, 0.25 * length(float2(across, down)) + 1e-4), -mostTurn, mostTurn);
		const float2 uvVelocity = float2(turn * forward * aspect, forward);
		result.velocity = tu * uvVelocity.x + tv * uvVelocity.y;
		result.strength = 1.0;

		float2 travel = uvVelocity * FlowPeriod;  // Texture moved per cycle
		const float most = length(travel);
		travel *= most > FlowMost ? FlowMost / most : 1.0;

		// Two phases, staggered over the surface by a smooth offset so the resets do not all land at once
		const float stagger = 0.25 * (sin(dot(a_position, float3(0.0021, 0.0027, 0.0013))) + sin(dot(a_position, float3(-0.0017, 0.0012, 0.0024))));
		const float cycle = Time / FlowPeriod + stagger;
		const float phaseA = frac(cycle);
		const float phaseB = frac(cycle + 0.5);
		result.uvA = uvStill - travel * (phaseA - 0.5);
		result.uvB = uvStill - travel * (phaseB - 0.5) + float2(0.31, 0.17);
		result.blendB = abs(1.0 - 2.0 * phaseA);
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
