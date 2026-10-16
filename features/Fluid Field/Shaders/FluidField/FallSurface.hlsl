// Fluid Field fall surfaces (D30): the falls' own water, one dense mesh per site built from its
// steady flow (FallStream), drawn after the world in place of vanilla's falling-water meshes. The
// vertex shader displaces the surface by the flow: it mounds over rock just under it, stands in waves
// where fast water runs shallow, and carries small ripples downstream. The pixel shader lays vanilla's
// falling-water texture along the streamlines (across) and the travel time (along, scrolling), lights
// it with the sun and the sky, foams it, and softens it where it meets rock in front of it.

#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

struct SurfaceVertex
{
	float3 Position;  // World units
	float Across;     // Stream function: world units across the flow
	float Travel;     // Seconds from where the water comes in
	float Speed;      // 1 for free flow
	float Foam;       // 0 to 1
	float Edge;       // 0 at the water's edge to 1 inside
	float3 Normal;
	float Depth;     // Water above the rock, world units
	float2 UV;       // Vanilla's texture coordinates
	float FlowUsed;  // How much the flow's texture coordinates are used (none on steep water)
	float Pad;
};

cbuffer FallSurfaceCB : register(b7)
{
	float Time;
	float UScale;      // World units across the flow per repeat of the texture
	float VScale;      // World units along the flow per repeat
	float BaseSpeed;   // World units per second of free-flowing water
	float WaveHeight;  // Standing waves where fast water runs shallow
	float WaveLength;
	float RippleHeight;  // Ripples carried by the flow
	float RippleLength;
	float BulgeHeight;   // The water mounds over rock just under it
	float ShallowDepth;  // Water shallower than this over rock waves and mounds
	float ContactWidth;  // World units over which the water fades where it meets rock in front of it
	float Brightness;
	uint DebugMode;       // 0 off; 1: speed as red, foam as green, edge as blue; 2: the streamlines as bands
	float VanillaScroll;  // Repeats per second vanilla's texture scrolls along v
	float2 Pad;
};

StructuredBuffer<SurfaceVertex> Vertices : register(t100);
Texture2D<float4> Diffuse : register(t101);
Texture2D<float4> NormalMap : register(t102);
Texture2D<float> SceneDepth : register(t103);
SamplerState Wrap : register(s0);

static const float TWO_PI = 6.2831853;
static const float3 ClearColor = float3(0.12, 0.4, 0.8);   // Clear falling water
static const float3 WhiteColor = float3(0.32, 0.68, 1.0);  // Aerated, streaked water

struct VSOutput
{
	float4 Position: SV_POSITION;
	float3 WorldPosition: POSITION0;  // Camera-relative
	float3 Normal: NORMAL0;
	float Across: TEXCOORD0;
	float Travel: TEXCOORD1;
	float Speed: TEXCOORD2;
	float Foam: TEXCOORD3;
	float Edge: TEXCOORD4;
	float2 UV: TEXCOORD5;
	float FlowUsed: TEXCOORD6;
};

VSOutput VSMain(uint vertexID : SV_VertexID)
{
	const SurfaceVertex v = Vertices[vertexID];
	const float shallow = saturate(1.0 - v.Depth / ShallowDepth);
	const float fast = saturate(v.Speed - 0.6);
	const float along = v.Travel * BaseSpeed;  // World units along the flow, at free speed
	// Standing waves stay where the rock makes them; ripples move with the water
	const float wave = WaveHeight * shallow * fast * sin(TWO_PI * along / WaveLength);
	const float ripple = RippleHeight * sin(TWO_PI * (v.Travel - Time) * BaseSpeed / RippleLength);
	const float bulge = BulgeHeight * shallow;
	const float3 position = v.Position + v.Normal * ((wave + ripple + bulge) * v.Edge + 2.0);

	VSOutput output;
	output.WorldPosition = position - FrameBuffer::CameraPosAdjust.xyz;
	output.Position = mul(FrameBuffer::CameraViewProj, float4(output.WorldPosition, 1.0));
	output.Normal = v.Normal;
	output.Across = v.Across;
	output.Travel = v.Travel;
	output.Speed = v.Speed;
	output.Foam = v.Foam;
	output.Edge = v.Edge;
	output.UV = v.UV;
	output.FlowUsed = v.FlowUsed;
	return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
	// Along the flow the texture scrolls with the water, stretching where it speeds up
	// Vanilla's falling-water texture runs along u (its tall falls span three times more u than v)
	const float2 uv = float2((input.Travel - Time) * BaseSpeed / VScale, input.Across / UScale);
	// Vanilla's own coordinates where the flow's are not used (steep water, off the flow)
	const float2 vanillaUV = float2(input.UV.x - Time * VanillaScroll, input.UV.y);

	// Debug views
	if (DebugMode == 1)
		return float4(saturate(input.Speed * 0.5), saturate(input.Foam), input.Edge, 1.0);
	if (DebugMode == 2) {
		const float bands = 0.5 + 0.5 * cos(input.Across * (TWO_PI / 32.0));
		const float marks = 0.5 + 0.5 * cos(input.Travel * BaseSpeed * (TWO_PI / 64.0));
		return float4(lerp(float3(0.2, 0.5, 1.0), float3(1.0, 0.4, 0.2), saturate(input.Speed - 0.5)) * (0.35 + 0.65 * bands) * (0.7 + 0.3 * marks), 1.0);
	}

	// The surface's frame: the flow's own directions, from how the texture coordinates run over it
	const float3 dPdx = ddx(input.WorldPosition);
	const float3 dPdy = ddy(input.WorldPosition);
	const float2 dUVdx = ddx(uv);
	const float2 dUVdy = ddy(uv);
	float3 geometric = normalize(input.Normal);
	const float3 tangent = normalize(dPdx * dUVdy.y - dPdy * dUVdx.y);  // Along u
	const float3 bitangent = normalize(cross(geometric, tangent));
	const float3 detail = lerp(NormalMap.Sample(Wrap, vanillaUV).xyz, NormalMap.Sample(Wrap, uv).xyz, input.FlowUsed) * 2.0 - 1.0;
	float3 normal = normalize(tangent * detail.x + bitangent * detail.y + geometric * max(detail.z, 0.3));
	const float3 view = normalize(input.WorldPosition);
	if (dot(normal, view) > 0.0)
		normal = -normal;

	// Vanilla's falling-water texture as how white the water is: clear water tinted, its streaks white
	const float4 texel = lerp(Diffuse.Sample(Wrap, vanillaUV), Diffuse.Sample(Wrap, uv), input.FlowUsed);
	const float whiteness = saturate(smoothstep(0.15, 0.85, dot(texel.rgb, float3(0.3, 0.5, 0.2))) + saturate(input.Foam) * 0.6);
	// Colours matched to vanilla's falls under the same light (measured in game)
	const float3 albedo = Color::Diffuse(lerp(ClearColor, WhiteColor, whiteness));
	const float3 sunDirection = normalize(SharedData::DirLightDirection.xyz);
	// Falling water is lit mostly by the sky; the sun only brightens it a little, without its colour
	// (vanilla's falls barely answer the sun, and a low orange sun turned the crests brown)
	const float3 sun = Color::RGBToLuminance(SharedData::DirLightColor.rgb) * saturate(dot(normal, sunDirection)) * 0.25;
	// The sky's light, its average colour only: its directional terms read warm ground bounce here
	const float3 ambient = max(float3(SharedData::AmbientSHR.x, SharedData::AmbientSHG.x, SharedData::AmbientSHB.x), 0.0) * 0.886;
	const float3 halfway = normalize(sunDirection - view);
	const float specular = pow(saturate(dot(normal, halfway)), 80.0) * 0.6;
	const float fresnel = 0.02 + 0.98 * pow(1.0 - saturate(dot(-view, normal)), 5.0);
	// The sky in it, at grazing angles mostly
	const float3 sky = ambient;
	const float3 color = (albedo * (ambient + sun) + SharedData::DirLightColor.rgb * specular + sky * (0.15 + 0.85 * fresnel)) * Brightness;

	// Soft where it meets rock in front of it, and at its edges
	const float sceneDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
	const float depth = SharedData::GetScreenDepth(input.Position.z);
	const float contact = saturate((sceneDepth - depth) / ContactWidth);
	// The edge fades over its last cells, so the grid's steps along the banks do not show
	const float alpha = smoothstep(0.05, 0.9, input.Edge) * (DebugMode == 3 ? 1.0 : contact);
	if (DebugMode == 4)
		return float4(contact.xxx, 1.0);
	if (alpha <= 0.002)
		discard;
	return float4(color * alpha, alpha);  // Premultiplied
}
