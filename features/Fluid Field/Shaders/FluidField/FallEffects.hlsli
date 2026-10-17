// Fluid Field waterfall effects' constants (D31, D38): shared by the spray (FallSpray.hlsl) and the
// waterfall fog (FallFog.hlsl), which FallSurfaceRenderer draws after the world. Matches
// FallSurfaceRenderer::Constants.

#ifndef FLUID_FIELD_FALL_EFFECTS_HLSLI
#define FLUID_FIELD_FALL_EFFECTS_HLSLI

cbuffer FallEffectsCB : register(b7)
{
	float Time;         // Seconds
	float BaseSpeed;    // World units per second of free-flowing water
	float Brightness;   // Scales how brightly spray and fog are lit
	uint DebugMode;     // Waterfall Shading: 3 shows what makes the fog
	uint EmitterCount;  // Spray emitters bound
	uint SprayPerEmitter;
	uint MistPerEmitter;
	float SprayLife;     // Seconds
	float SpraySpeed;    // World units per second thrown at full strength
	float SpraySize;     // World units across
	float SprayStretch;  // Seconds of motion a droplet is drawn smeared over
	float SprayOpacity;
	float MistLife;
	float MistSize;
	float MistRise;  // World units per second
	float MistOpacity;
	float Gravity;     // World units per second squared
	float FogDensity;  // Extinction per world unit at full strength (0: no fog)
	uint FogSteps;     // Samples along each ray
	float FogBottom;   // The lowest and highest world heights the fog reaches: rays are marched only between them
	float FogTop;
	float3 EffectsPad;
};

#endif  // FLUID_FIELD_FALL_EFFECTS_HLSLI
