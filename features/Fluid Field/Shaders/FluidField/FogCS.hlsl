// Fluid Field fog (D21): a ground-fog density multiplier on the wind grid, read by Exponential
// Height Fog's volumetric fog. 1 is the weather's fog unchanged, 0 is clear air.
//
// Each step the fog is carried along by the wind and the weather's drift, fills back in toward 1
// over a few seconds, and thins where the air is stirred: an actor walking or running through it
// drags clear air down into it, so it parts behind them and swirls in their wake before closing.
// Cells that scrolled into the grid since the last step, and every cell after a reset, start at 1.

#include "FluidField/Common.hlsli"

cbuffer FogCB : register(b1)
{
	float2 Drift;      // Weather wind carrying the fog, world units per second
	float RefillRate;  // Per-second rate at which thinned fog fills back in toward 1
	float StirRate;    // Per-second rate at which fully stirred air clears the fog
	float StirSpeed;   // Wind speed (world units per second) that counts as fully stirred
	float3 FogPad;
};

Texture2D<float2> Velocity : register(t0);
Texture2D<float> FogIn : register(t1);
RWTexture2D<float> FogOut : register(u0);

/** Last step's fog at a logical cell; 1 for cells that were not in the grid then (or after a reset). */
float LoadFog(int2 logical)
{
	int2 cell = GridMin + logical;
	bool kept = HistoryValid != 0 && all(cell >= PrevGridMin) && all(cell < PrevGridMin + int(GridRes));
	return kept ? FogIn[LogicalToStorage(clamp(logical, 0, int(GridRes) - 1))] : 1.0;
}

/** Last step's fog at a logical position (texel centres at +0.5), bilinear. */
float SampleFog(float2 position)
{
	float2 p = position - 0.5;
	int2 i = int2(floor(p));
	float2 t = p - float2(i);
	return lerp(lerp(LoadFog(i), LoadFog(i + int2(1, 0)), t.x), lerp(LoadFog(i + int2(0, 1)), LoadFog(i + int2(1, 1)), t.x), t.y);
}

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);
	float2 wind = Velocity[id];

	// Carried along by the wind and the weather's drift
	float2 source = float2(logical) + 0.5 - DeltaTime * (wind + Drift) / CellSize;
	float fog = SampleFog(source);

	// Thinned where the air is stirred, then filling back in
	float stirred = saturate(length(wind) / max(StirSpeed, 1.0));
	fog *= exp(-StirRate * stirred * stirred * DeltaTime);
	fog = 1.0 - (1.0 - fog) * exp(-RefillRate * DeltaTime);

	FogOut[id] = saturate(fog);
}
