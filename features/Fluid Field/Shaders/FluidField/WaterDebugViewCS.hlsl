// Fluid Field debug overlay for the water layer: depth, flow and waves, north up, player-centred.

#include "FluidField/WaterCommon.hlsli"

cbuffer WaterDebugCB : register(b2)
{
	float DepthScale;  // 1 / depth shown at full brightness
	float FlowScale;   // 1 / speed shown at full brightness
	float WaveScale;   // 1 / height offset shown at full saturation
	float WaterDebugPad0;
};

Texture2D<float> Depth : register(t0);
Texture2D<float4> Surface : register(t1);
Texture2D<float2> Flow : register(t2);

RWTexture2D<float4> DepthView : register(u0);
RWTexture2D<float4> FlowView : register(u1);
RWTexture2D<float4> WaveView : register(u2);

float3 HueToRgb(float hue)
{
	float3 rgb = abs(frac(hue + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0;
	return saturate(rgb);
}

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = int2(id.x, int(GridRes) - 1 - int(id.y));
	uint2 storage = LogicalToStorage(logical);

	float depth = Depth[storage];
	float4 surface = Surface[storage];
	bool wet = depth > WetThreshold;

	// Depth: black when dry, light blue when shallow, deep blue when deep
	float t = saturate(depth * DepthScale);
	DepthView[id] = wet ? float4(lerp(float3(0.55, 0.85, 1.0), float3(0.05, 0.15, 0.55), t), 1.0) : float4(0.08, 0.08, 0.08, 1.0);

	// Flow: hue is direction (east red, north green, west cyan, south violet), brightness speed
	float2 flow = Flow[storage];
	float hue = atan2(flow.y, flow.x) / 6.28318530718;
	FlowView[id] = float4(HueToRgb(hue) * saturate(length(flow) * FlowScale), 1.0);

	// Waves: red above the vanilla surface, blue below, grey where dry
	float wave = surface.z * WaveScale;
	WaveView[id] = wet ? float4(0.15 + saturate(wave), 0.15, 0.15 + saturate(-wave), 1.0) : float4(0.08, 0.08, 0.08, 1.0);
}
