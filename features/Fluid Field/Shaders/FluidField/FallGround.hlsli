// Fluid Field waterfall grounding (D26) and flow (D28, D29): vanilla's falling-water meshes bent onto
// the rock they run over (effect-shaded layers only: the lit surface is drawn after a depth prepass
// its position must match exactly), their streaks laid along the falls' steady streamlines so they part round rock
// and close up behind it, foamed where the flow piles against rock or squeezes past it, and softened
// where they cut into rock. FluidField binds the constants, the captured ground
// (FallSites::BuildGroundMap) and the flow (FallSites::BuildStreamMap) only for the draws of grounded
// meshes; Enabled is 0 for every other effect draw.

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
		float ContactWidth;  // World units over which the water fades where it cuts into rock
		float LiftMost;      // Water deeper than this inside rock is left there (a boulder hides it); shallower is lifted onto it
		float FoamStrength;  // How white the foam is
		uint DebugMode;      // 0 off; 1: speed as red, foam as green, water as blue; 2: the streamlines as bands; 3: grounding without the streamline texture
		float UScale;        // World units across the flow per repeat of vanilla's texture
		float SteepFrom;     // The surface's upward normal below which the water is too steep for the flow (vanilla's streaks)
		float SteepTo;       // ...and above which the flow's streaks are used in full
		float SlowFrom;      // Flow speed (1 free) below which vanilla's texture stays
		float SlowTo;        // ...and above which the streamline texture is used in full
		float Pad;
	};

	Texture2D<float> Heights : register(t120);
	Texture2D<float> SceneDepth : register(t121);
	Texture2D<float4> Stream : register(t122);  // Stream function (world units across the flow), speed (1 free), foam, water

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
	 * The falls' flow at a world position, bilinear over its water texels only: stream function,
	 * speed, foam, and how much water is around (0 to 1).
	 */
	float4 StreamAt(float2 a_position)
	{
		const float2 texel = (a_position - Origin) * InverseCell;
		const int2 base = int2(floor(texel));
		if (any(base < 0) || any(base + 1 >= int(Size)))
			return 0.0;
		const float2 t = texel - float2(base);
		const float4 s00 = Stream.Load(int3(base, 0));
		const float4 s10 = Stream.Load(int3(base + int2(1, 0), 0));
		const float4 s01 = Stream.Load(int3(base + int2(0, 1), 0));
		const float4 s11 = Stream.Load(int3(base + int2(1, 1), 0));
		// The stream function is weighted by water, so rock beside the flow does not pull it toward 0
		const float4 w = float4((1.0 - t.x) * (1.0 - t.y) * s00.w, t.x * (1.0 - t.y) * s10.w, (1.0 - t.x) * t.y * s01.w, t.x * t.y * s11.w);
		const float total = w.x + w.y + w.z + w.w;
		if (total <= 1e-4)
			return 0.0;
		const float3 value = (s00.xyz * w.x + s10.xyz * w.y + s01.xyz * w.z + s11.xyz * w.w) / total;
		return float4(value, lerp(lerp(s00.w, s10.w, t.x), lerp(s01.w, s11.w, t.x), t.y));
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

	/**
	 * A pixel of falling water laid along the falls' streamlines. Vanilla's streaks are lines of
	 * constant u, scrolling along v; here u becomes the stream function (scaled to the texture), so
	 * every streak follows a streamline: it parts round a boulder, cannot cross it, and closes up
	 * behind it, and streaks crowd together where the flow squeezes through a gap.
	 * @param a_uvStream Texture coordinates along the streamlines.
	 * @param a_weight How much of the streamline texture to use (0 off the flow, where vanilla's stays).
	 * @param a_stream The flow here (StreamAt), for foam and the debug views.
	 */
	void Flow(float2 a_uv, float3 a_position, out float2 a_uvStream, out float a_weight, out float4 a_stream)
	{
		a_stream = StreamAt(a_position.xy);
		// The flow is solved on the falls seen from above: where the water drops steeply its streamlines
		// are squeezed to nothing, so there vanilla's own streaks stay
		const float3 normal = normalize(cross(ddx(a_position), ddy(a_position)));
		// Where the flow is slow (pools, eddies) its streamlines loop: vanilla's texture stays there
		a_weight = smoothstep(0.3, 0.8, a_stream.w) * smoothstep(SteepFrom, SteepTo, abs(normal.z)) * smoothstep(SlowFrom, SlowTo, a_stream.y);
		a_uvStream = float2(a_stream.x / UScale, a_uv.y);
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
