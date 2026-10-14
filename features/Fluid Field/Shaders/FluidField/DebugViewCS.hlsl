// Fluid Field debug overlay: unwraps each layer into a north-up, player-centred RGBA image.

#include "FluidField/Common.hlsli"

cbuffer DebugViewCB : register(b1)
{
	float VelocityScale;    // 1 / speed shown at full brightness
	float DivergenceScale;  // 1 / divergence shown at full saturation
	float PressureScale;    // 1 / pressure shown at full saturation
	float DebugViewPad0;
};

Texture2D<float2> Velocity : register(t0);
Texture2D<float> Divergence : register(t1);
Texture2D<float> Pressure : register(t2);

RWTexture2D<float4> VelocityView : register(u0);
RWTexture2D<float4> DivergenceView : register(u1);
RWTexture2D<float4> PressureView : register(u2);

float3 HueToRgb(float hue)
{
	float3 rgb = abs(frac(hue + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0;
	return saturate(rgb);
}

// Red for positive, blue for negative, black at zero
float4 Diverging(float value)
{
	return float4(saturate(value), 0.0, saturate(-value), 1.0);
}

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	// Image rows run top to bottom, logical y runs south to north
	int2 logical = int2(id.x, int(GridRes) - 1 - int(id.y));
	uint2 storage = LogicalToStorage(logical);

	// Hue encodes direction (east red, north yellow-green, west cyan, south blue-violet), brightness speed
	float2 velocity = Velocity[storage];
	float hue = atan2(velocity.y, velocity.x) / 6.28318530718;
	VelocityView[id] = float4(HueToRgb(hue) * saturate(length(velocity) * VelocityScale), 1.0);

	DivergenceView[id] = Diverging(Divergence[storage] * DivergenceScale);
	PressureView[id] = Diverging(Pressure[storage] * PressureScale);
}
