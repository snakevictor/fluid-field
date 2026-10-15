// Fluid Field waterfalls (D23): curtains of falling water drawn in place of vanilla's waterfall
// bodies. Each curtain is a grid of points (columns across the lip, rows down the fall) built on
// the CPU: ballistic columns that lie on the rock where they meet it. Drawn after the world with
// depth testing and alpha blending, before the ground fog.

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

struct CurtainPoint
{
	float3 Position;  // World units
	float Along;      // 0 at the lip to 1 at the end of the fall
	float Speed;      // World units per second
	float Across;     // -1 at one edge to 1 at the other
	float OnRock;     // 1 where the water lies on rock
	float Pad;
};

cbuffer WaterfallCB : register(b7)
{
	uint Columns;
	uint Rows;
	float Time;     // Seconds
	float Opacity;  // Most opacity of the falling water
};

StructuredBuffer<CurtainPoint> Points : register(t100);

struct VSOutput
{
	float4 Position: SV_POSITION;
	float3 WorldPosition: POSITION0;  // Camera-relative
	float Along: TEXCOORD0;
	float Across: TEXCOORD1;
	float Speed: TEXCOORD2;
	float OnRock: TEXCOORD3;
	float Distance: TEXCOORD4;  // World units travelled down the fall
};

VSOutput VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	// Two triangles per grid cell
	static const uint2 CORNERS[6] = { uint2(0, 0), uint2(1, 0), uint2(0, 1), uint2(0, 1), uint2(1, 0), uint2(1, 1) };
	const uint quad = vertexID / 6;
	const uint2 cell = uint2(quad % (Columns - 1), quad / (Columns - 1)) + CORNERS[vertexID % 6];
	const CurtainPoint p = Points[instanceID * Columns * Rows + cell.y * Columns + cell.x];

	VSOutput output;
	output.WorldPosition = p.Position - FrameBuffer::CameraPosAdjust.xyz;
	output.Position = mul(FrameBuffer::CameraViewProj, float4(output.WorldPosition, 1.0));
	output.Along = p.Along;
	output.Across = p.Across;
	output.Speed = p.Speed;
	output.OnRock = p.OnRock;
	// Distance down the fall, for texture flow: the sum of speed over time, approximated per row
	output.Distance = p.Along * p.Speed;
	return output;
}

float Hash(float2 p)
{
	return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float ValueNoise(float2 p)
{
	const float2 i = floor(p);
	const float2 f = p - i;
	const float2 u = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), u.x), lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), u.x), u.y);
}

/** How brightly the falling water is lit, as a fraction of the light around it (as the ground fog). */
static const float BRIGHTNESS = 0.2;

float4 PSMain(VSOutput input) : SV_Target
{
	// Streaks: narrow across the curtain, stretched down it, and scrolling down at the water's speed
	const float2 uv = float2(input.Across * 6.0, input.Along * 2.0 - Time * 0.9);
	const float streak = ValueNoise(float2(uv.x * 3.0, uv.y * 0.6)) * 0.6 + ValueNoise(float2(uv.x * 9.0, uv.y * 1.8)) * 0.4;

	// The curtain's edges thin out; the water is solid near the lip and breaks up as it falls
	const float edges = (1.0 - smoothstep(0.7, 1.0, abs(input.Across))) * (1.0 - smoothstep(0.85, 1.0, input.Along));
	const float breakup = lerp(0.95, 0.55, input.Along);
	const float coverage = saturate(edges * smoothstep(1.0 - breakup - 0.2, 1.0 - breakup + 0.2, streak));
	const float alpha = coverage * Opacity;
	if (alpha <= 0.002)
		discard;

	// Lit like a white, scattering surface by the sky and the sun; whiter toward the bottom
	const float3 ambient = max(float3(SharedData::AmbientSHR.x, SharedData::AmbientSHG.x, SharedData::AmbientSHB.x), 0.0) * 0.886;
	const float3 sun = SharedData::DirLightColor.xyz * 0.35;
	const float3 water = lerp(float3(0.55, 0.65, 0.7), float3(0.9, 0.93, 0.95), saturate(input.Along * 0.8 + streak * 0.3));
	// Scaled like the ground fog: the scene's light values are far brighter than a surface's albedo
	const float3 color = water * (ambient + sun) * BRIGHTNESS;
	return float4(color * alpha, alpha);  // Premultiplied
}
