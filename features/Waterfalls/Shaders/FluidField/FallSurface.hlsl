// Fluid Field see-through falls: the sheets' vertex shader and their two depth pre-passes (FallRefraction). The river's own
// Water.hlsl shades them (its FLUID_FIELD_FALLS permutation); vertices move as there (FallGround::Shift).

SamplerState WrapSampler : register(s0);
SamplerState ClampSampler : register(s1);  // Linear; also the shadow maps' LinearSampler

#define LinearSampler ClampSampler

#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Shading.hlsli"
#include "Common/SharedData.hlsli"
#include "FluidField/FallGround.hlsli"
#include "FluidField/FallShading.hlsli"

cbuffer FallSurfaceCB : register(b7)
{
	row_major float3x4 World;  // Model to absolute world
	float3 ShallowColor;       // Water type's colours (0 to 1) times the weather's water multiplier, as Water.hlsl
	float FogFar;              // Above-water fog far (FogParam.z): water this deep is wholly its own colour
	float3 DeepColor;
	float FogRange;             // Fog far less near (FogParam.w)
	float3 ReflectionColor;     // Reflection colour times amount: the reflection without a cubemap
	float FogAmount;            // Above-water fog amount (FogNearColor.w)
	float FresnelF0;            // Fresnel amount (FresnelRI.x): reflectance head-on
	float SunSpecularPower;     // VarAmounts.x
	float SunSpecularStrength;  // 1 - exp(-sun specular magnitude), as Water.hlsl weighs the sun with DeepColor.w
	float ReflectionMagnitude;  // WaterParams.y: how much surface detail tilts the reflection
	float RefractionStrength;   // Refraction: how far the surface bends what is seen through it
	float StreakFoam;           // How much of vanilla's streak texture reads as white water
	float DepthScale;           // Water Depth: how opaque and deep-coloured the sheet reads per unit of thickness
	float MaxPath;              // Most world units of water behind the surface counted
	float SheetThickness;       // World units, square on
	uint HasEnvironment;
	uint HasDiffuse;
	uint HasNormal;
	uint HasFront;  // D125: FrontDepth holds the falls' front surface
	uint3 FrontPadding;
	float3 NoiseScale;  // D129: the water type's normal maps (Water.hlsl's Normals01-03): world units per repeat
	float NoiseTime;    // Seconds
	float3 NoiseAmplitude;
	uint HasNoise;
	float3 SkyFogNearColor;  // D151: the sky's distance fog, as Water.hlsl
	float SkyFogNear;
	float3 SkyFogFarColor;
	float SkyFogFar;  // 0: no fog
	float SkyFogPower;
	float SkyFogClamp;
	float2 SkyFogPadding;
};

struct FallSurfaceVertex
{
	float3 Position;  // Model space
	float2 TexCoord;  // Vanilla's
	float3 Normal;    // Model space
	float3 Tangent;   // D174: direction v runs, model space (FallRefraction::Vertex)
};

StructuredBuffer<FallSurfaceVertex> Vertices : register(t125);
Texture2D<float> SceneDepth : register(t101);
Texture2D<float> FrontDepth : register(t105);  // D125: the scene's depth with the falls' front surface drawn in

struct VSOutput
{
	float4 Position: SV_POSITION;
	float3 WorldPosition: TEXCOORD0;  // Camera-relative
	float2 TexCoord: TEXCOORD1;
	float3 Normal: TEXCOORD2;
	float3 Fade: TEXCOORD3;  // FallGround::Fade: seam fade, height above the water below; z its foot (D98)
	float Clip: SV_ClipDistance0;
	float Behind: TEXCOORD7;        // D104: baked open water or air behind its outer side, world units
	float BelowSource: TEXCOORD8;   // D124: depth under the water it pours from (FallGround::NO_WATER: none)
	float ToOtherSheet: TEXCOORD9;  // D128: distance to the nearest other shown sheet (FallGround::NO_WATER: none near)
	float4 Outward: TEXCOORD6;      // D100: normal turned to the mesh's outer side; D103: w 1 where a sheet lies behind its inner side
};

#if defined(VSHADER)
VSOutput VSMain(uint vertexID : SV_VertexID)
{
	const FallSurfaceVertex v = Vertices[vertexID];
	// Same move as the falls drawn by Water.hlsl
	const float3 shift = FallGround::Shift(vertexID, FallGround::Scroll.x, World[2].xyz);
	const float3 model = v.Position + shift;
	const float3 world = mul(World, float4(model, 1.0));
	const float3 relative = world - FrameBuffer::CameraPosAdjust.xyz;

	VSOutput output;
	output.Position = mul(FrameBuffer::CameraViewProj, float4(relative, 1.0));
	output.WorldPosition = relative;
	output.TexCoord = v.TexCoord;
	output.Normal = mul((float3x3)World, v.Normal);
	output.Fade.xy = FallGround::Fade(vertexID);
	output.Fade.y = min(output.Fade.y, 512.0);  // D139: clamped as BelowSource
	// D98: as the cut (D71), pools foam a mesh only at its foot; fmod drops the side flags
	output.Fade.z = (!FallGround::Enabled || FallGround::VertexBase == FallGround::NO_VERTEX_CLIP) ? 1.0 : fmod(FallGround::VertexClip[FallGround::VertexBase + vertexID].a.w, 2.0);
	output.Clip = FallGround::ClipDistance(relative, vertexID);
	// The depth behind moves with the vertex: pushed out along its normal, the gap behind widens
	output.Behind = max(FallGround::Behind(vertexID) + dot(shift, v.Normal) / max(dot(v.Normal, v.Normal), 1e-6) * length(World[0].xyz), 0.0);
	// D138: "none" (NO_WATER, 1e4) clamped before interpolation, or a triangle mixing it drew a hard line
	output.BelowSource = min(FallGround::BelowSource(vertexID), 512.0);
	output.ToOtherSheet = min(FallGround::ToOtherSheet(vertexID), 512.0);
	output.Outward = float4(output.Normal * FallGround::OutwardSign(vertexID), FallGround::InnerCovered(vertexID));  // D103: no sign flip mid-triangle
	return output;
}
#endif  // VSHADER

#if defined(PSHADER)
/**
 * D125: the front surface's depth, so the shading pass (EQUAL) draws only the nearest sheet. Discards what the shading
 * hides (inner side, cut, faded past half), so a faded or cut front hides nothing.
 */
void PSDepth(VSOutput input)
{
	const float3 toCamera = FallShading::SafeNormalize(-input.WorldPosition, float3(0.0, 0.0, 1.0));
	if (FallShading::HiddenInnerSide(input.Outward, toCamera))
		discard;
	const float sheetDepth = SharedData::GetScreenDepth(input.Position.z);
	const float straightBehind = max(SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0))) - sheetDepth, 0.0);
	if (saturate(input.Fade.x) * saturate(straightBehind / 4.0) * FallGround::CutFade(input.Clip) < 0.5)
		discard;
}

/**
 * D174: the falls' surface nearest behind their front, for foam where sheets meet: PSDepth's discards plus the front itself
 * (within FallShading::BEHIND_LAYER_EPSILON of FrontDepth); the depth test keeps the nearest.
 */
void PSBehindDepth(VSOutput input)
{
	PSDepth(input);
	// Not BehindFront's FRONT_EPSILON, which hides the other sheet where sheets cross; the front's depth matches exactly
	// (same vertex shader)
	if (SharedData::GetScreenDepth(input.Position.z) - SharedData::GetScreenDepth(FrontDepth.Load(int3(input.Position.xy, 0))) <= FallShading::BEHIND_LAYER_EPSILON)
		discard;
}
#endif  // PSHADER
