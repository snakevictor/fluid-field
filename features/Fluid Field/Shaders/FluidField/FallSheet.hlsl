// Fluid Field waterfall sheets (D25, S7): falling water drawn in place of vanilla's waterfall
// bodies. Each stream traced off a lip (FallCapture::TraceStreams) is a ribbon of points: falling
// free, sliding on rock, ending in the water below. Neighbouring ribbons overlap into a sheet and
// part where their streams do. Drawn after the world with depth testing and alpha blending: clear
// water refracts the scene behind it (a copy taken just before), aerated water is white and lit.

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

struct StreamPoint
{
	float3 Position;   // World units
	float Along;       // 0 at the lip to 1 at the end
	float2 Side;       // Horizontal direction across the stream
	float HalfWidth;   // World units
	float Speed;       // World units per second
	float Across;      // -1 at one end of its lip to 1 at the other
	float OnRock;      // 1 where it slides on rock
	float TravelTime;  // Seconds from the lip
	float Pad;
};

cbuffer FallSheetCB : register(b7)
{
	uint Rows;
	float Time;     // Seconds
	float Opacity;  // Most opacity of the falling water
	float Pad0;
	float2 InverseSize;  // Of the target
	float2 Pad1;
};

StructuredBuffer<StreamPoint> Points : register(t100);
Texture2D<float4> Scene : register(t101);
SamplerState LinearClamp : register(s0);

struct VSOutput
{
	float4 Position: SV_POSITION;
	float3 WorldPosition: POSITION0;  // Camera-relative
	float Along: TEXCOORD0;
	float Across: TEXCOORD1;  // Along the lip
	float Speed: TEXCOORD2;
	float OnRock: TEXCOORD3;
	float TravelTime: TEXCOORD4;  // Seconds from the lip
	float Ribbon: TEXCOORD5;      // -1 to 1 across this stream
	float Fallen: TEXCOORD6;      // World units below the lip
};

VSOutput VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	// Each stream is a ribbon: two triangles per row, its two edges either side of the path
	static const uint2 CORNERS[6] = { uint2(0, 0), uint2(1, 0), uint2(0, 1), uint2(0, 1), uint2(1, 0), uint2(1, 1) };
	const uint2 corner = CORNERS[vertexID % 6];
	const uint row = vertexID / 6 + corner.y;
	const StreamPoint p = Points[instanceID * Rows + row];
	const StreamPoint lip = Points[instanceID * Rows];
	const float edge = corner.x == 0 ? -1.0 : 1.0;

	VSOutput output;
	output.WorldPosition = p.Position + float3(p.Side * p.HalfWidth * edge, 0.0) - FrameBuffer::CameraPosAdjust.xyz;
	output.Position = mul(FrameBuffer::CameraViewProj, float4(output.WorldPosition, 1.0));
	output.Along = p.Along;
	output.Across = p.Across;
	output.Speed = p.Speed;
	output.OnRock = p.OnRock;
	output.TravelTime = p.TravelTime;
	output.Ribbon = edge;
	output.Fallen = lip.Position.z - p.Position.z;
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

/** How brightly the aerated water is lit, as a fraction of the light around it (as the ground fog). */
static const float BRIGHTNESS = 0.2;
/** Seconds between repeats of the flow pattern down the fall. */
static const float PATTERN_PERIOD = 0.6;
/** World units of fall over which the water turns white. */
static const float AERATION_FALL = 400.0;
/** Most a clear sheet bends what is seen through it, as a fraction of the screen. */
static const float REFRACTION = 0.012;

float4 PSMain(VSOutput input) : SV_Target
{
	// The pattern is carried by the water. Each point knows how long the water takes to reach it
	// from the lip, so (travel time - now) labels the water passing there: the pattern always moves
	// down the fall at the water's own speed, and stretches where the water is fast.
	const float flowPhase = (input.TravelTime - Time) / PATTERN_PERIOD;
	// Across in world units (each stream is about 16 wide), so the streaks keep their size
	const float acrossUnits = input.Across * 8.0 + input.Ribbon * 0.5;
	const float coarse = ValueNoise(float2(acrossUnits * 0.6, flowPhase));
	const float fine = ValueNoise(float2(acrossUnits * 2.3, flowPhase * 3.0 + coarse * 2.0));
	const float streaks = fine * 0.6 + coarse * 0.4;

	// Clear and glassy over the lip, breaking up and turning white as it falls and speeds up, and
	// where it tumbles over rock
	const float aerationWanted = saturate(0.08 + 0.75 * saturate(input.Fallen / AERATION_FALL) + 0.3 * input.OnRock);
	const float aeration = smoothstep(1.0 - aerationWanted - 0.2, 1.0 - aerationWanted + 0.2, streaks);
	// Soft stream edges, so overlapping streams merge and split ones read as separate flows; it
	// starts from the surface at the lip and ends in the water below
	const float ribbonEdge = 1.0 - smoothstep(0.4, 1.0, abs(input.Ribbon));
	const float sheetEdge = 1.0 - smoothstep(0.85, 1.0, abs(input.Across));
	const float ends = smoothstep(0.0, 0.04, input.Along) * (1.0 - smoothstep(0.9, 1.0, input.Along));
	const float coverage = ribbonEdge * sheetEdge * ends;

	// The surface's tilt from the sheet's own shape, rippled by the streaks
	const float3 dx = ddx(input.WorldPosition);
	const float3 dy = ddy(input.WorldPosition);
	float3 normal = normalize(cross(dx, dy));
	const float3 view = normalize(input.WorldPosition);
	if (dot(normal, view) > 0.0)
		normal = -normal;
	const float ripple = (streaks - 0.5) * 2.0;
	const float fresnel = 0.02 + 0.98 * pow(1.0 - saturate(dot(-view, normal)), 5.0);
	const float3 ambient = max(float3(SharedData::AmbientSHR.x, SharedData::AmbientSHG.x, SharedData::AmbientSHB.x), 0.0) * 0.886;
	const float3 sunLight = SharedData::DirLightColor.xyz;
	const float glint = pow(saturate(dot(reflect(view, normal), SharedData::DirLightDirection.xyz)), 60.0);

	// Clear water: the scene behind, bent by the moving surface and tinted, with the sky's reflection
	const float2 screen = input.Position.xy * InverseSize;
	const float2 bend = float2(ripple, ValueNoise(float2(acrossUnits * 1.7 + 13.0, flowPhase * 2.0)) * 2.0 - 1.0) * REFRACTION * (1.0 - aeration);
	const float3 behind = Scene.SampleLevel(LinearClamp, screen + bend, 0).rgb;
	const float3 clearWater = behind * float3(0.82, 0.9, 0.9) + (ambient * fresnel * 0.6 + sunLight * glint * 0.5) * BRIGHTNESS;
	// Aerated water: white, lit by the sun and the sky, brighter where the sun shines through it
	const float backlit = pow(saturate(dot(view, SharedData::DirLightDirection.xyz)), 4.0);
	const float3 whiteWater = float3(0.9, 0.93, 0.95) * (ambient + sunLight * (0.35 + 0.4 * backlit)) * BRIGHTNESS;
	const float3 color = lerp(clearWater, whiteWater, aeration);

	// Clear water is see-through but still refracts, so it covers what is behind it fully where it is
	const float alpha = saturate(lerp(0.85, 0.95, aeration)) * coverage * Opacity;
	if (alpha <= 0.002)
		discard;
	return float4(color * alpha, alpha);  // Premultiplied
}
