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
		float2 Origin;       // World position of texel (0, 0)
		float InverseCell;   // Texels per world unit
		float Size;          // Texels along each side
		uint Enabled;        // This draw is a grounded mesh
		float Offset;        // How far above the rock the water lies
		float Cling;         // Water hovering less than this above the rock is drawn down onto it
		float ContactWidth;  // World units over which the water fades where it cuts into rock
		float Reach;         // How far above rock its push still bends the water
		float WakeLength;    // World units the foam trails behind rock
		float FoamStrength;  // How white the foam is
		float LiftMost;      // Water deeper than this inside rock is left there (a boulder hides it); shallower is lifted onto it
		uint DebugMode;      // 0 off, 1 the rock field: push as red and green, presence as blue
		float3 Pad;
	};

	Texture2D<float> Heights : register(t120);
	Texture2D<float> SceneDepth : register(t121);
	Texture2D<float4> Rock : register(t122);  // Push x, y (world units), presence, prominence

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
		const float4 rock = RockAt(a_position.xy);
		a_position.xy += rock.xy * Nearness(a_position.xy, a_position.z);
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
	 * A pixel of falling water flowed round rock in its way. Vanilla's texture scrolls along v, so its
	 * streaks are lines of constant u: shifting u by the push across the flow curves each streak round
	 * the rock and back behind it. Foam piles up where the water runs into rock (the rock's presence
	 * rises just downstream) and trails behind it in a widening wake.
	 * @param a_uv The pixel's texture coordinates; returns them shifted.
	 * @param a_rock The rock field here, for the debug view.
	 * @param a_foam How much foam is here, 0 to 1.
	 */
	float2 Flow(float2 a_uv, float3 a_position, out float4 a_rock, out float a_foam)
	{
		a_rock = 0.0;
		a_foam = 0.0;

		// The world's change across the texture: d(position)/du and d(position)/dv
		const float3 dPdx = ddx(a_position);
		const float3 dPdy = ddy(a_position);
		const float2 dUVdx = ddx(a_uv);
		const float2 dUVdy = ddy(a_uv);
		const float det = dUVdx.x * dUVdy.y - dUVdx.y * dUVdy.x;
		if (abs(det) < 1e-12)
			return a_uv;
		const float3 dPdu = (dPdx * dUVdy.y - dPdy * dUVdx.y) / det;
		const float3 dPdv = (dPdy * dUVdx.x - dPdx * dUVdy.x) / det;
		// Down the fall: along v, whichever way descends
		const float3 down = normalize(dPdv.z <= 0.0 ? dPdv : -dPdv);
		const float2 downAcross = normalize(down.xy + float2(1e-5, 0.0));

		a_rock = RockAt(a_position.xy);
		const float near = Nearness(a_position.xy, a_position.z);

		// The push, across the flow only, moves the streak that passes here: sample where it came from
		float3 push = float3(a_rock.xy * near, 0.0);
		push -= down * dot(push, down);
		const float2 shift = float2(dot(push, dPdu) / max(dot(dPdu, dPdu), 1e-6), dot(push, dPdv) / max(dot(dPdv, dPdv), 1e-6));

		// Foam piled where the water runs into rock, and trailing behind it
		const float ahead = RockAt(a_position.xy + downAcross * 24.0).z;
		const float pillow = saturate((ahead - a_rock.z) * 4.0) * near;
		float wake = 0.0;
		[unroll] for (int i = 1; i <= 4; i++)
		{
			const float distance = WakeLength * i / 4.0;
			wake = max(wake, RockAt(a_position.xy - downAcross * distance).w * (1.0 - distance / (WakeLength * 1.25)));
		}
		wake *= 1.0 - a_rock.w;
		a_foam = saturate(pillow + wake * 0.8);
		return a_uv - shift;
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
