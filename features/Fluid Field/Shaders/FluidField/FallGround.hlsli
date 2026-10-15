// Fluid Field waterfall grounding (D26): vanilla's falling-water meshes bent onto the rock they run
// over and pushed round rock standing in their way, their streaks curved round that rock with foam
// piled against it and trailing behind it, and softened where they cut into rock.
// FluidField binds the constants, the captured ground (FallSites::BuildGroundMap) and the rock field
// (FallSites::BuildRockField) only for the draws of grounded meshes; Enabled is 0 for every other
// effect draw.

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
		float Cling;           // Water hovering less than this above the rock is drawn down onto it
		float ContactWidth;    // World units over which the water fades where it cuts into rock
		float Reach;           // How far above rock its push still bends the water
		float FoamStrength;    // How white the foam is
		float LiftMost;        // Water deeper than this inside rock is left there (a boulder hides it); shallower is lifted onto it
		uint DebugMode;        // 0 off, 1: the fall flow's speed as red, foam as green, presence as blue
		float VertexPush;      // How much of the push moves the mesh's vertices
		float VertexPushMost;  // World units
		int2 FlowGridMin;      // The fall flow's grid (D27): world cell of its logical texel (0, 0)
		uint FlowRes;          // Texels per side, a power of two, stored toroidally
		float FlowCell;        // World units per texel
		uint FlowEnabled;      // The fall flow is running
		float FlowPeriod;      // Seconds per flowmap cycle
		float Time;            // Seconds
		float LieHeight;       // Water within about this of the rock lies on it: the fall flow says where it is
		float FlowGain;        // How far the fall flow's sideways velocity carries the streaks, as a multiple of its speed
		float PresenceRadius;  // World units around each point its depth is compared with
	};

	Texture2D<float> Heights : register(t120);
	Texture2D<float> SceneDepth : register(t121);
	Texture2D<float4> Rock : register(t122);      // Push x, y (world units), presence, prominence
	Texture2D<float4> FallFlow : register(t123);  // The fall flow's output: velocity x, y, foam, depth

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
		const float h00 = Heights.Load(int3(base, 0));
		const float h10 = Heights.Load(int3(base + int2(1, 0), 0));
		const float h01 = Heights.Load(int3(base + int2(0, 1), 0));
		const float h11 = Heights.Load(int3(base + int2(1, 1), 0));
		if (min(min(h00, h10), min(h01, h11)) < NONE)
			return false;
		a_height = lerp(lerp(h00, h10, t.x), lerp(h01, h11, t.x), t.y);
		return true;
	}

	/** The rock field at a world position, bilinear; zero outside the map. */
	float4 RockAt(float2 a_position)
	{
		const float2 texel = (a_position - Origin) * InverseCell;
		const int2 base = int2(floor(texel));
		if (any(base < 0) || any(base + 1 >= int(Size)))
			return 0.0;
		const float2 t = texel - float2(base);
		return lerp(lerp(Rock.Load(int3(base, 0)), Rock.Load(int3(base + int2(1, 0), 0)), t.x),
			lerp(Rock.Load(int3(base + int2(0, 1), 0)), Rock.Load(int3(base + int2(1, 1), 0)), t.x), t.y);
	}

	/** How strongly rock at a world position bends water at a_z: fully at the rock's top, none Reach above it. */
	float Nearness(float2 a_position, float a_z)
	{
		float ground;
		if (!Height(a_position, ground))
			return 0.0;
		return saturate(1.0 - (a_z - ground) / Reach);
	}

	/**
	 * A vertex of falling water, at a world position, moved onto the rock: pushed aside round rock
	 * standing in its way, out of the bed where it clips just under it, and down onto rock it hovers
	 * just above. Water deep inside rock it is not pushed out of is a boulder's: it is left there,
	 * hidden, so the boulder stays dry.
	 */
	float3 Ground(float3 a_position)
	{
		// The mesh is coarse: moved far, its triangles tear. So vertices move only a little of the push
		// (the texture carries the rest), and only at or inside rock
		const float4 rock = RockAt(a_position.xy);
		float2 move = rock.xy * VertexPush * saturate(Nearness(a_position.xy, a_position.z) * 2.0 - 1.0);
		const float length2 = dot(move, move);
		if (length2 > VertexPushMost * VertexPushMost)
			move *= VertexPushMost * rsqrt(length2);
		a_position.xy += move;
		float ground;
		if (!Height(a_position.xy, ground))
			return a_position;
		const float surface = ground + Offset;
		if (a_position.z < surface)
			return surface - a_position.z <= LiftMost ? float3(a_position.xy, surface) : a_position;
		// Just above the rock: eased down onto it, leaving water well clear of it where it is
		const float gap = a_position.z - surface;
		if (gap < Cling)
			a_position.z = surface + gap * gap / Cling;
		return a_position;
	}

	/**
	 * The fall flow (D27) at a world position, bilinear: velocity x, y (world units per second), foam,
	 * and depth. False outside its grid or when it is not running.
	 */
	bool FallFlowAt(float2 a_position, out float4 a_flow)
	{
		a_flow = 0.0;
		if (!FlowEnabled)
			return false;
		const float2 cell = a_position / FlowCell - 0.5;
		const int2 base = int2(floor(cell));
		if (any(base < FlowGridMin) || any(base + 1 >= FlowGridMin + int(FlowRes)))
			return false;
		const float2 t = cell - float2(base);
		const int mask = int(FlowRes) - 1;
		const float4 f00 = FallFlow.Load(int3(base & mask, 0));
		const float4 f10 = FallFlow.Load(int3((base + int2(1, 0)) & mask, 0));
		const float4 f01 = FallFlow.Load(int3((base + int2(0, 1)) & mask, 0));
		const float4 f11 = FallFlow.Load(int3((base + int2(1, 1)) & mask, 0));
		a_flow = lerp(lerp(f00, f10, t.x), lerp(f01, f11, t.x), t.y);
		return true;
	}

	/**
	 * How much of the fall flow's water is at a world position, 0 to 1: its depth against the depth
	 * around it, so a sheet that is thin everywhere (fast water on steep rock) stays whole, and the
	 * shallow shadow behind a boulder, or a dry patch, thins away.
	 */
	float Presence(float2 a_position, float a_depth)
	{
		float4 around;
		float sum = 0.0, count = 0.0;
		[unroll] for (int i = 0; i < 4; i++)
		{
			const float2 offset = float2(i == 0 ? 1 : i == 1 ? -1 :
															   0,
									  i == 2 ? 1 : i == 3 ? -1 :
															0) *
			                      PresenceRadius;
			if (FallFlowAt(a_position + offset, around)) {
				sum += around.w;
				count += 1.0;
			}
		}
		const float mean = count > 0.0 ? sum / count : a_depth;
		return smoothstep(0.15, 0.6, a_depth / max(mean, 1.0));
	}

	/**
	 * A pixel of falling water flowed round rock in its way. Vanilla's texture scrolls along v, so its
	 * streaks are lines of constant u. Two things move them across: the rock field's push, a fixed
	 * shift that bends each streak round the rock and back behind it; and the fall flow's own sideways
	 * velocity, carried as a two-phase flowmap, so the streaks travel round the rock as the solved water
	 * does. Foam comes from the fall flow (shear and impacts), with a little hugging the rock. Where
	 * the water lies on the rock, the fall flow also says where it is (Presence): in the shallow
	 * shadow behind a boulder the mesh thins away, so the flow visibly splits round the rock.
	 * @param a_uv0, a_uv1 Texture coordinates to sample, blended by a_blend.
	 * @param a_rock The rock field here, for the debug view.
	 * @param a_foam How much foam is here, 0 to 1.
	 * @param a_speed The fall flow's speed here, for the debug view.
	 * @param a_presence How much of the water is there, 0 to 1: the fall flow's coverage where it lies on rock.
	 */
	void Flow(float2 a_uv, float3 a_position, out float2 a_uv0, out float2 a_uv1, out float a_blend, out float4 a_rock, out float a_foam, out float a_speed, out float a_presence)
	{
		a_presence = 1.0;
		a_uv0 = a_uv;
		a_uv1 = a_uv;
		a_blend = 0.0;
		a_rock = 0.0;
		a_foam = 0.0;
		a_speed = 0.0;

		// The world's change across the texture: d(position)/du and d(position)/dv
		const float3 dPdx = ddx(a_position);
		const float3 dPdy = ddy(a_position);
		const float2 dUVdx = ddx(a_uv);
		const float2 dUVdy = ddy(a_uv);
		const float det = dUVdx.x * dUVdy.y - dUVdx.y * dUVdy.x;
		if (abs(det) < 1e-12)
			return;
		const float3 dPdu = (dPdx * dUVdy.y - dPdy * dUVdx.y) / det;
		const float3 dPdv = (dPdy * dUVdx.x - dPdx * dUVdy.x) / det;
		// Down the fall: along v, whichever way descends
		const float3 down = normalize(dPdv.z <= 0.0 ? dPdv : -dPdv);
		const float uScale = 1.0 / max(dot(dPdu, dPdu), 1e-6);

		// The rock field's fixed push, across the flow only
		a_rock = RockAt(a_position.xy);
		const float near = Nearness(a_position.xy, a_position.z);
		float3 push = float3(a_rock.xy * near, 0.0);
		push -= down * dot(push, down);
		const float2 shift = float2(dot(push, dPdu) * uScale, 0.0);
		a_uv0 -= shift;
		a_uv1 -= shift;
		a_foam = smoothstep(0.12, 0.45, a_rock.z) * near * 0.5;

		// The fall flow's sideways velocity, carried as a flowmap
		float4 flow;
		if (!FallFlowAt(a_position.xy, flow))
			return;
		// The fall flow speaks for water lying on the rock; free-falling water well above it is left be
		float ground;
		const float lying = Height(a_position.xy, ground) ? saturate(1.0 - (a_position.z - ground - LieHeight) / LieHeight) : 0.0;
		a_presence = lerp(1.0, Presence(a_position.xy, flow.w), lying);
		a_speed = length(flow.xy);
		float3 velocity = float3(flow.xy, 0.0);
		velocity -= down * dot(velocity, down);
		const float uRate = dot(velocity, dPdu) * uScale * FlowGain;  // Texture u per second
		const float phase0 = frac(Time / FlowPeriod);
		const float phase1 = frac(Time / FlowPeriod + 0.5);
		a_uv0.x -= uRate * FlowPeriod * (phase0 - 0.5);
		a_uv1.x -= uRate * FlowPeriod * (phase1 - 0.5);
		a_blend = abs(2.0 * phase0 - 1.0);
		a_foam = max(a_foam, saturate(flow.z) * lying);
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
