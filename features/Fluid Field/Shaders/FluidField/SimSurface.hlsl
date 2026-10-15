// Fluid Field's own water surface (D24): the simulated water wherever there is no vanilla water
// plane under it, such as water pouring down a waterfall's rock. A heightfield mesh over each fall
// area, a vertex per cell of the near water level (height = bed + depth), drawn after the world
// with depth testing. Shaded as water: the scene behind it refracted and absorbed with depth, the
// sky reflected, the sun's glint, and white where the simulation foams or the water is steep and fast.

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

cbuffer SimSurfaceCB : register(b7)
{
	int2 CellMin;    // First world cell of this area's grid of vertices
	int2 CellCount;  // Vertices across and down
	uint GridRes;    // The water level's texels per side (power of two)
	float CellSize;  // World units per cell
	float Time;      // Seconds
	float Brightness;
	float2 ScreenRcp;  // 1 / render target size
	float2 SurfacePad;
};

Texture2D<float> Depth : register(t100);
Texture2D<float2> CellMap : register(t101);  // (bed, vanilla depth)
Texture2D<float4> Flow : register(t102);     // (velocity xy, foam, -)
Texture2D<float> FallBed : register(t103);
Texture2D<float4> SceneColor : register(t104);
SamplerState LinearClamp : register(s0);

/** Water thinner than this is not drawn. */
static const float MIN_DEPTH = 1.5;
static const float VANILLA_WET = 1.0;
static const float NO_FALL_BED = -1.0e6;

struct VSOutput
{
	float4 Position: SV_POSITION;
	float3 WorldPosition: POSITION0;  // Camera-relative
	float3 Normal: NORMAL0;
	float2 Velocity: TEXCOORD0;
	float Foam: TEXCOORD1;
	float WaterDepth: TEXCOORD2;
	float Visible: TEXCOORD3;
	float2 WorldXY: TEXCOORD4;
};

int2 Storage(int2 cell)
{
	return cell & int(GridRes - 1);
}

/** Water surface height at a cell, and whether simulated water is drawn there. */
float SurfaceAt(int2 cell, out bool visible)
{
	const int2 s = Storage(cell);
	const float depth = Depth[s];
	const float2 map = CellMap[s];
	visible = depth > MIN_DEPTH && map.y <= VANILLA_WET && FallBed[s] > NO_FALL_BED;
	return map.x + depth;
}

VSOutput VSMain(uint vertexID : SV_VertexID)
{
	static const int2 CORNERS[6] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(0, 1), int2(1, 0), int2(1, 1) };
	const uint quad = vertexID / 6;
	const int2 cell = CellMin + int2(quad % uint(CellCount.x - 1), quad / uint(CellCount.x - 1)) + CORNERS[vertexID % 6];

	bool visible;
	const float height = SurfaceAt(cell, visible);
	bool vE, vW, vN, vS;
	float hE = SurfaceAt(cell + int2(1, 0), vE);
	float hW = SurfaceAt(cell - int2(1, 0), vW);
	float hN = SurfaceAt(cell + int2(0, 1), vN);
	float hS = SurfaceAt(cell - int2(0, 1), vS);
	hE = vE ? hE : height;
	hW = vW ? hW : height;
	hN = vN ? hN : height;
	hS = vS ? hS : height;
	const float3 normal = normalize(float3((hW - hE) / (2.0 * CellSize), (hS - hN) / (2.0 * CellSize), 1.0));

	const int2 s = Storage(cell);
	const float4 flow = Flow[s];

	VSOutput output;
	const float2 worldXY = (float2(cell) + 0.5) * CellSize;
	output.WorldPosition = float3(worldXY, height) - FrameBuffer::CameraPosAdjust.xyz;
	output.Position = mul(FrameBuffer::CameraViewProj, float4(output.WorldPosition, 1.0));
	output.Normal = normal;
	output.Velocity = flow.xy;
	output.Foam = flow.z;
	output.WaterDepth = Depth[s];
	output.Visible = visible ? 1.0 : 0.0;
	output.WorldXY = worldXY;
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

/** Bubbly breakup carried by the flow (two phases, restarted half a period apart, cross-faded). */
float Breakup(float2 worldXY, float2 velocity)
{
	const float period = 0.6;
	const float phase0 = frac(Time / period);
	const float phase1 = frac(Time / period + 0.5);
	const float2 uv = worldXY / 48.0;
	const float2 drift = velocity * period / 48.0;
	const float n0 = ValueNoise(uv - drift * phase0) * 0.6 + ValueNoise(uv * 2.7 - drift * phase0 * 2.7) * 0.4;
	const float n1 = ValueNoise(uv - drift * phase1 + 5.3) * 0.6 + ValueNoise(uv * 2.7 - drift * phase1 * 2.7 + 2.1) * 0.4;
	return lerp(n0, n1, abs(phase0 * 2.0 - 1.0));
}

float4 PSMain(VSOutput input) : SV_Target
{
	// Soft edge where the simulated water meets vanilla's or the dry rock
	const float edge = smoothstep(0.4, 0.9, input.Visible);
	if (edge <= 0.002)
		discard;

	const float3 view = normalize(input.WorldPosition);
	float3 n = normalize(input.Normal);
	const float speed = length(input.Velocity);
	const float steep = 1.0 - n.z;

	// The scene behind, bent through the water and absorbed with its depth
	const float2 screenUV = input.Position.xy * ScreenRcp;
	const float2 refractedUV = screenUV + n.xy * 0.04 * saturate(input.WaterDepth / 24.0);
	const float3 behind = SceneColor.SampleLevel(LinearClamp, refractedUV, 0).rgb;
	const float3 absorption = float3(0.045, 0.02, 0.016);
	const float3 refracted = behind * exp(-absorption * input.WaterDepth);

	// The sky reflected (ambient), and the sun's glint
	const float3 facing = dot(n, view) > 0.0 ? -n : n;
	const float fresnel = 0.02 + 0.98 * pow(1.0 - saturate(dot(-view, facing)), 5.0);
	const float3 ambient = max(float3(SharedData::AmbientSHR.x, SharedData::AmbientSHG.x, SharedData::AmbientSHB.x), 0.0) * 0.886;
	const float3 sun = SharedData::DirLightColor.xyz;
	const float glint = pow(saturate(dot(reflect(view, facing), SharedData::DirLightDirection.xyz)), 80.0);
	const float3 water = refracted * (1.0 - fresnel) + (ambient * fresnel + sun * glint * 0.8) * Brightness;

	// White water: the simulation's foam, and air beaten into steep, fast water, broken up by the flow
	const float aerated = saturate(input.Foam * 0.7 + steep * 1.5 * saturate((speed - 120.0) / 250.0));
	const float breakup = Breakup(input.WorldXY, input.Velocity);
	const float white = smoothstep(1.0 - aerated - 0.1, 1.0 - aerated + 0.25, breakup) * saturate(aerated * 2.0);
	const float3 whiteWater = float3(0.85, 0.88, 0.9) * (ambient + sun * 0.4) * Brightness;

	const float3 color = lerp(water, whiteWater, white);
	return float4(color * edge, edge);  // Premultiplied
}
