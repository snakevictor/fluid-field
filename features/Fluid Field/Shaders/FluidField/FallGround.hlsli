// Fluid Field waterfall grounding (D26): vanilla's falling-water meshes bent onto the rock they run
// over, and softened where they meet it. FluidField binds the constants and the captured ground
// (FallSites::BuildGroundMap) only for the draws of grounded meshes; Enabled is 0 for every other
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
		float ContactWidth;  // World units over which the water softens and foams where it meets rock
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
