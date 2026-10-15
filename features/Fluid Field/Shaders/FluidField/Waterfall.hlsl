// Fluid Field waterfalls (D23): curtains of falling water drawn in place of vanilla's waterfall
// bodies. Each curtain is a grid of points (columns across the lip, rows down the fall) built on
// the CPU: ballistic columns that lie on the rock where they meet it. Drawn after the world with
// depth testing and alpha blending, before the ground fog.

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

struct CurtainPoint
{
	float3 Position;   // World units
	float Along;       // 0 at the lip to 1 at the end of the fall
	float Speed;       // World units per second
	float Across;      // -1 at one edge to 1 at the other
	float OnRock;      // 1 where the water lies on rock
	float TravelTime;  // Seconds the water takes to get here from the lip
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
	float TravelTime: TEXCOORD4;  // Seconds from the lip
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
	output.TravelTime = p.TravelTime;
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
/** Seconds between repeats of the flow pattern down the fall. */
static const float PATTERN_PERIOD = 0.8;

float4 PSMain(VSOutput input) : SV_Target
{
	// The pattern is carried by the water. Each point knows how long the water takes to reach it
	// from the lip, so (travel time - now) labels the water passing there: the pattern always moves
	// down the fall at the water's own speed, and stretches where the water is fast.
	const float flowPhase = (input.TravelTime - Time) / PATTERN_PERIOD;
	const float2 uv = float2(input.Across * 5.0, flowPhase);
	const float coarse = ValueNoise(float2(uv.x * 2.0, uv.y));
	const float fine = ValueNoise(float2(uv.x * 7.0, uv.y * 3.0 + coarse));

	// Aerated where the water is fast or tumbles over rock, clear where it is slow; thin at the edges
	const float speedFactor = saturate((input.Speed - 150.0) / 500.0);
	const float aerationWanted = saturate(0.15 + 0.6 * speedFactor + 0.25 * input.OnRock);
	const float aeration = smoothstep(1.0 - aerationWanted - 0.15, 1.0 - aerationWanted + 0.15, fine * 0.6 + coarse * 0.4);
	const float edges = (1.0 - smoothstep(0.6, 1.0, abs(input.Across))) * (1.0 - smoothstep(0.85, 1.0, input.Along));

	// The surface's tilt from the curtain's own shape, for reflection and the sun's glint
	const float3 normal = normalize(cross(ddx(input.WorldPosition), ddy(input.WorldPosition)));
	const float3 view = normalize(input.WorldPosition);
	const float3 n = dot(normal, view) > 0.0 ? -normal : normal;
	const float fresnel = 0.02 + 0.98 * pow(1.0 - saturate(dot(-view, n)), 5.0);
	const float3 ambient = max(float3(SharedData::AmbientSHR.x, SharedData::AmbientSHG.x, SharedData::AmbientSHB.x), 0.0) * 0.886;
	const float3 sunLight = SharedData::DirLightColor.xyz;
	const float glint = pow(saturate(dot(reflect(view, n), SharedData::DirLightDirection.xyz)), 60.0);

	// Clear water: mostly the sky's reflection and see-through; aerated water: white, lit, more opaque
	const float3 clearWater = (float3(0.2, 0.28, 0.3) * ambient + ambient * fresnel * 0.8 + sunLight * glint * 0.6) * BRIGHTNESS;
	const float3 whiteWater = float3(0.9, 0.93, 0.95) * (ambient + sunLight * 0.35) * BRIGHTNESS;
	const float3 color = lerp(clearWater, whiteWater, aeration);
	const float alpha = saturate(lerp(0.25 + fresnel * 0.5, 0.9, aeration)) * edges * Opacity;
	if (alpha <= 0.002)
		discard;
	return float4(color * alpha, alpha);  // Premultiplied
}
