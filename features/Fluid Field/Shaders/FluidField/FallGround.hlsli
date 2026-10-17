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
		float SteepFrom;       // The surface's upward normal below which the water falls straight down its slope
		float SteepTo;         // ...and above which it follows the solved flow
		float2 TexOffset;      // This mesh's texture scroll (vanilla's), taken back out of its coordinates
		float Gravity;         // World units per second squared the falling water gains speed by (D33)
		float Translucency;    // D53: how see-through the falls' thin water is (0: opaque; only on blended draws)
		float2 TexScale;       // This mesh's world units per texture repeat along u and v (D35); 0 unknown
		uint HideUnderwater;   // Falling water under the pools' surface is not drawn (D36)
		float PoolFoamHeight;  // World units above the water below over which the falls foam into it
		float3 CameraOffset;   // FrameBuffer::CameraPosAdjust, for the vertex shaders that have none (D38)
		uint Whitening;        // The falls whiten with their foam and speed (D34); 0 for the debug comparison (D39)
	};

	Texture2D<float2> Heights : register(t120);  // The captured rock, and the pools' surface (D36)
	Texture2D<float> SceneDepth : register(t121);
	Texture2D<float4> Stream : register(t122);  // Velocity (x, y; 1 free), foam plus 2 on water (0 elsewhere), head (world height, D33)

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
	/** D54: the most a buried vertex counts as buried, so a cut stays near its buried corner. */
	static const float BURIED_SOFT = 4.0;
	/** D54: the least speed falling water's texture moves at, as a fraction of free flow: where it leaves a pool it is already going. */
	static const float LEAST_FLOW = 0.6;
	/** D47: the falls are cut this far under the pools' surface, so the pool's water overlaps the cut. */
	static const float POOL_CUT = 2.0;  // D49: under the pools the falls are hidden; the dense foam at the waterline covers the line

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
	 * or in the captured rock and ground), for SV_ClipDistance. The depth pre-pass (Utility.hlsl) and
	 * the lit pass (Lighting.hlsl) cut the same triangles at the same line, so nothing is left behind in
	 * depth where the water is hidden. 1 on every other draw.
	 * @param a_position Camera-relative world position (add CameraOffset for the absolute one).
	 */
	float ClipDistance(float3 a_position)
	{
		if (!Enabled || !HideUnderwater)
			return 1.0;
		const float3 position = a_position + CameraOffset;
		float distance = 1.0e6;
		float pool, ground;
		if (Pool(position.xy, pool))
			distance = min(distance, position.z - (pool - POOL_CUT));
		// D54: the rock's side is held near the line: Bethesda's triangles are large, and a vertex deep in the
		// rock pulled the cut far across the triangle into water that stands out of it
		if (LowestGround(position.xy, ground))
			distance = min(distance, max(position.z - (ground - BURIED), -BURIED_SOFT));
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
	 * A pixel of falling water carried by the falls' flow (D32), as a flowmap: its texture moves with the
	 * water at the pixel, in two phases half a cycle apart that cross-fade, each reset once a cycle
	 * before it stretches too far. The flow is solved from above; on the surface it is the solved
	 * velocity lifted onto the slope, and where the surface is steep, straight down it. The water
	 * gains speed down the slope from the height it has dropped below its head (D33). The velocity is
	 * taken into the mesh's texture space through its screen-space derivatives, so every mesh and
	 * layer moves with the same water whatever its texture layout. Vanilla's own scroll is taken out.
	 */
	FlowSample Flow(float2 a_uv, float3 a_position, float3 a_normal, float3 a_tangent, float3 a_bitangent)
	{
		FlowSample result;
		float head;
		result.stream = StreamAt(a_position.xy, head);
		const float3 dpx = ddx(a_position);
		const float3 dpy = ddy(a_position);
		const float2 dux = ddx(a_uv);
		const float2 duy = ddy(a_uv);
		// D35: the mesh's own normal and tangents, interpolated over its vertices, where the draw has them:
		// the triangles' faces are coarse on Bethesda's meshes, and the flow jumped at every edge
		const bool smoothFrame = dot(a_normal, a_normal) > 0.25 && TexScale.x > 0.0 && TexScale.y > 0.0;
		float3 normal = smoothFrame ? normalize(a_normal) : normalize(cross(dpx, dpy));
		normal *= normal.z < 0.0 ? -1.0 : 1.0;

		// The solved flow lifted onto the surface; on steep water, straight down its slope, at least at free speed
		const float2 solved = result.stream.xy;
		const float solvedSpeed = length(solved);
		const float3 lifted = float3(solved, -dot(normal.xy, solved) / max(normal.z, 0.15));
		const float3 downhill = float3(0.0, 0.0, -1.0) + normal * normal.z;
		const float steep = 1.0 - smoothstep(SteepFrom, SteepTo, normal.z);
		const float water = smoothstep(0.3, 0.8, result.stream.w);
		// D53: steep water runs down its slope, always: on a fold of Bethesda's meshes too, it moves down
		// the screen, never back up the falls (D34 let the solve lead where the two disagreed)
		const float3 liftedDirection = lifted / max(length(lifted), 1e-4);
		const float3 downDirection = downhill / max(length(downhill), 1e-4);
		const bool solvedHere = water > 0.0 && solvedSpeed > 1e-3;
		const float agree = solvedHere ? saturate(dot(liftedDirection, downDirection) * 4.0) : 1.0;
		float3 direction = solvedHere ? lerp(liftedDirection, downDirection, steep) : downDirection;
		direction /= max(length(direction), 1e-4);
		// D44: water never runs uphill: where the solve points up the surface, it turns down it, the more
		// the steeper it pointed up
		if (direction.z > 0.0) {
			const float3 level = float3(direction.xy, 0.0);
			direction = lerp(level, downDirection, saturate(direction.z * 3.0));
			direction = length(direction) > 0.3 ? normalize(direction) : downDirection;
		}
		const float speed = max(lerp(solvedSpeed, max(solvedSpeed, 1.0), steep * agree), LEAST_FLOW) * FlowSpeed;
		// Gravity (D33): the speed gained from the drop below the head, along the course
		const float drop = water > 0.0 ? max(head - a_position.z, 0.0) : 0.0;
		const float gained = sqrt(speed * speed + 2.0 * Gravity * drop) - speed;
		result.velocity = direction * (speed + gained);
		result.strength = max(water, steep * step(0.05, length(downhill)));

		// Into texture space: the surface's tangents along u and v, then the velocity in u and v
		const float det = dux.x * duy.y - dux.y * duy.x;
		float2 uvVelocity = 0.0;
		if (abs(det) > 1e-12) {
			const float3 tu = (dpx * duy.y - dpy * dux.y) / det;
			const float3 tv = (dpy * dux.x - dpx * duy.x) / det;
			if (smoothFrame) {
				// The smooth tangents, and the mesh's texel density; the faces only say which tangent runs along u
				const float3 a = normalize(a_tangent);
				const float3 b = normalize(a_bitangent);
				const bool swapped = abs(dot(tu, b)) > abs(dot(tu, a));
				float3 alongU = swapped ? b : a;
				float3 alongV = swapped ? a : b;
				alongU *= dot(alongU, tu) < 0.0 ? -1.0 : 1.0;
				alongV *= dot(alongV, tv) < 0.0 ? -1.0 : 1.0;
				uvVelocity = float2(dot(result.velocity, alongU) / TexScale.x, dot(result.velocity, alongV) / TexScale.y);
			} else {
				const float uu = dot(tu, tu), uv = dot(tu, tv), vv = dot(tv, tv);
				const float gram = uu * vv - uv * uv;
				if (gram > 1e-12) {
					const float wu = dot(result.velocity, tu), wv = dot(result.velocity, tv);
					uvVelocity = float2(vv * wu - uv * wv, uu * wv - uv * wu) / gram;
				}
			}
		} else
			result.strength = 0.0;
		float2 travel = uvVelocity * FlowPeriod;  // Texture moved per cycle
		const float most = length(travel);
		travel *= most > FlowMost ? FlowMost / most : 1.0;

		// Two phases, staggered over the surface by a smooth offset so the resets do not all land at once
		const float stagger = 0.25 * (sin(dot(a_position, float3(0.0021, 0.0027, 0.0013))) + sin(dot(a_position, float3(-0.0017, 0.0012, 0.0024))));
		const float cycle = Time / FlowPeriod + stagger;
		const float phaseA = frac(cycle);
		const float phaseB = frac(cycle + 0.5);
		const float2 uvStill = a_uv - TexOffset;
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
