#ifndef FLUIDFIELD_FOGPATTERN_HLSLI
#define FLUIDFIELD_FOGPATTERN_HLSLI

// Fluid Field fog (D21): the ground fog's resting pattern of banks and clearings. Shared by the fog simulation (which
// settles toward it) and the ground fog pass (which draws it beyond the simulated area) so they meet without a seam.
// a_offset: its drift with the weather's wind, world units.

namespace FogPattern
{
	/** World units across one patch of banks and clearings. */
	static const float PATCH_SIZE = 900.0;
	/** Fog in the clearings, as a fraction of the banks'. */
	static const float PATCH_FLOOR = 0.3;

	float Hash(float2 p)
	{
		return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
	}

	/** Smooth value noise, 0 to 1. */
	float ValueNoise(float2 p)
	{
		const float2 i = floor(p);
		const float2 f = p - i;
		const float2 u = f * f * (3.0 - 2.0 * f);
		return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), u.x), lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), u.x), u.y);
	}

	/** How much fog rests at a world position when nothing disturbs it (0 to 1). */
	float Resting(float2 a_worldXY, float2 a_offset)
	{
		const float2 p = (a_worldXY - a_offset) / PATCH_SIZE;
		const float n = ValueNoise(p) * 0.65 + ValueNoise(p * 2.7 + 17.3) * 0.35;
		return lerp(PATCH_FLOOR, 1.0, smoothstep(0.3, 0.7, n));
	}
}

#endif
