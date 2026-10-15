// Fluid Field waterfall grounding (D26): vanilla's falling-water meshes bent onto the rock they run
// over, their texture flowed around rocks in the way, and softened where they meet the rock.
// FluidField binds the constants and the captured ground (FallSites::BuildGroundMap) only for the
// draws of grounded meshes; Enabled is 0 for every other effect draw.

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
		float ContactWidth;  // World units over which the water softens and foams where it meets rock
		float FlowDistance;  // How far, world units, the texture is carried sideways round a rock per flow cycle
		float FlowPeriod;    // Seconds per flow cycle
		float ReachAbove;    // Rock this far below the water already turns it
		float ReachWidth;    // World units over which rock in the way turns the water
	};

	Texture2D<float> Heights : register(t120);
	Texture2D<float> SceneDepth : register(t121);

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

	/**
	 * A vertex of falling water, at a world position, moved onto the rock: out of rock it cuts into
	 * (sideways off a steep face where it can, else up onto it), and down onto rock it hovers just
	 * above, so the water runs on the rock rather than through or over it.
	 */
	float3 Ground(float3 a_position)
	{
		float ground;
		if (!Height(a_position.xy, ground))
			return a_position;
		const float surface = ground + Offset;
		if (a_position.z < surface) {
			// Inside the rock. Off a steep face the way out is sideways, down the slope
			float hx0, hx1, hy0, hy1;
			const float step = 16.0;
			if (Height(a_position.xy - float2(step, 0), hx0) && Height(a_position.xy + float2(step, 0), hx1) &&
				Height(a_position.xy - float2(0, step), hy0) && Height(a_position.xy + float2(0, step), hy1)) {
				const float2 gradient = float2(hx1 - hx0, hy1 - hy0) / (2.0 * step);
				if (dot(gradient, gradient) > 1.0) {
					const float2 downhill = -normalize(gradient);
					[loop] for (int i = 1; i <= 8; i++)
					{
						const float2 candidate = a_position.xy + downhill * (8.0 * i);
						float h;
						if (Height(candidate, h) && a_position.z >= h + Offset)
							return float3(candidate, a_position.z);
					}
				}
			}
			return float3(a_position.xy, surface);
		}
		// Just above the rock: eased down onto it, leaving water well clear of it where it is
		const float gap = a_position.z - surface;
		if (gap < Cling)
			a_position.z = surface + gap * gap / Cling;
		return a_position;
	}

	/**
	 * How much rock stands in the water's way at a world position: 0 where it is well below the water
	 * (at a_z), 1 where it reaches the surface or above.
	 */
	float Obstacle(float2 a_position, float a_z)
	{
		float ground;
		if (!Height(a_position, ground))
			return 0.0;
		return saturate((ground - a_z + ReachAbove) / ReachAbove);
	}

	/**
	 * The texture coordinates of a pixel of falling water, flowed around the rock in its way. The way
	 * the texture runs in the world comes from the mesh's own coordinates (down the fall along v); rock
	 * in the way turns it aside, down the slope of the rock around it. The sideways turn is carried by
	 * the flow as a two-phase flowmap, so the streaks bend round the rock, part ahead of it and close
	 * up behind it. a_turn is how hard the water is turned there, for foam in its wake.
	 */
	void Flow(float2 a_uv, float3 a_position, float a_time, out float2 a_uv0, out float2 a_uv1, out float a_blend, out float a_turn)
	{
		a_uv0 = a_uv;
		a_uv1 = a_uv;
		a_blend = 0.0;
		a_turn = 0.0;

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

		// Rock in the way, around this point: the water turns away from it
		const float step = 24.0;
		const float2 gradient = float2(Obstacle(a_position.xy + float2(step, 0), a_position.z) - Obstacle(a_position.xy - float2(step, 0), a_position.z),
									Obstacle(a_position.xy + float2(0, step), a_position.z) - Obstacle(a_position.xy - float2(0, step), a_position.z)) /
		                        (2.0 * step) * ReachWidth;
		float3 turn = float3(-gradient, 0.0);
		// Only across the flow: water is not pushed back up the fall
		const float3 down = normalize(dPdv.z <= 0.0 ? dPdv : -dPdv);
		turn -= down * dot(turn, down);
		a_turn = saturate(length(turn));
		if (a_turn < 0.01)
			return;

		// The turn in texture units per cycle
		const float3 carried = turn * FlowDistance;
		const float2 flowUV = float2(dot(carried, dPdu) / max(dot(dPdu, dPdu), 1e-6), dot(carried, dPdv) / max(dot(dPdv, dPdv), 1e-6));
		const float phase0 = frac(a_time / FlowPeriod);
		const float phase1 = frac(a_time / FlowPeriod + 0.5);
		a_uv0 = a_uv - flowUV * (phase0 - 0.5);
		a_uv1 = a_uv - flowUV * (phase1 - 0.5);
		a_blend = abs(2.0 * phase0 - 1.0);
	}

	/**
	 * How much of the falling water shows where it meets the rock in front of or behind it, and how
	 * much foam it has there, from the scene's depth at the pixel.
	 */
	void Contact(float2 a_pixel, float a_depth, out float a_visibility, out float a_foam)
	{
		const float sceneDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(a_pixel, 0)));
		const float depth = SharedData::GetScreenDepth(a_depth);
		const float gap = max(sceneDepth - depth, 0.0);
		a_visibility = saturate(gap / (ContactWidth * 0.35));
		a_foam = 1.0 - saturate(gap / ContactWidth);
	}
}

#endif  // FLUID_FIELD_FALL_GROUND_HLSLI
