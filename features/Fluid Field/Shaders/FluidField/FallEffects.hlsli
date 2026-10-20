// Fluid Field waterfall spray constants (D31), for FallSpray.hlsl, which FallSurfaceRenderer draws
// after the world. Matches
// FallSurfaceRenderer::Constants.

#ifndef FLUID_FIELD_FALL_EFFECTS_HLSLI
#define FLUID_FIELD_FALL_EFFECTS_HLSLI

cbuffer FallEffectsCB : register(b7)
{
	float Time;         // Seconds
	float Brightness;   // Scales how brightly spray and fog are lit
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
	float Gravity;  // World units per second squared
	float EffectsPad;
};

#endif  // FLUID_FIELD_FALL_EFFECTS_HLSLI
