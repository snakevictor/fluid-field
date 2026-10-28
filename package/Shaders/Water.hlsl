#if defined(HORIZON_FIX)
namespace HorizonFix
{
	// Depth (z/w) that water folded back from beyond the far clip plane lands at: eight
	// depth quanta inside the far plane, exactly representable in both D24 and D32F
	// buffers - behind everything the scene rendered, in front of the depth clear.
	static const float FoldedDepth = 1.0 - 8.0 / 16777216.0;

	// Scene depth at or beyond this counts as "nothing rendered behind the water": the
	// clear value, folded far water (FoldedDepth), or an external plugin's depth-clamped
	// far-water backdrop a few quanta inside that. The margin is a few world units at the
	// far plane, where no real surface can render.
	static const float EmptyDepthThreshold = 1.0 - 64.0 / 16777216.0;
}
#endif

#if defined(UNDERWATERMASK)

struct VS_INPUT
{
	float4 Position: POSITION0;
};

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
};

#	ifdef VSHADER

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	float z = min(1, 1e-4 * max(0, input.Position.z - 70000)) * 0.5 + input.Position.z;
	vsout.Position = float4(input.Position.xy, z, 1);

	return vsout;
}
#	endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#	ifdef PSHADER
PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	psout.Color = 1;

	return psout;
}
#	endif

#else

#	include "Common/FrameBuffer.hlsli"
#	include "Common/MotionBlur.hlsli"
#	include "Common/Permutation.hlsli"
#	include "Common/Random.hlsli"
#	include "Common/Shading.hlsli"
#	include "Common/Color.hlsli"

#	define WATER

#	include "Common/SharedData.hlsli"

// D159: Fluid Field's see-through falls drawn by the river's shader (FallRefraction): the refracting technique only. The
// per-vertex and flowmap variants, Water Effects' parallax and Wetness Effects' ripples assume a level surface and are off.
#	if defined(FLUID_FIELD_FALLS) && !(defined(SPECULAR) && NUM_SPECULAR_LIGHTS == 0)
#		undef FLUID_FIELD_FALLS
#	endif
#	if defined(FLUID_FIELD_FALLS)
#		if !defined(FLUID_FIELD)
#			define FLUID_FIELD
#		endif
#		undef FLOWMAP
#		undef BLEND_NORMALS
#		undef VC
#		undef NORMAL_TEXCOORD
#		undef WADING
#		undef VERTEX_ALPHA_DEPTH
#		undef WATER_EFFECTS
#		undef WETNESS_EFFECTS
#	endif

struct VS_INPUT
{
#	if defined(SPECULAR) || defined(UNDERWATER) || defined(STENCIL) || defined(SIMPLE)
	float4 Position: POSITION0;
#		if defined(NORMAL_TEXCOORD)
	float2 TexCoord0: TEXCOORD0;
#		endif
#		if defined(VC)
	float4 Color: COLOR0;
#		endif
#	endif

#	if defined(LOD)
	float4 Position: POSITION0;
#		if defined(VC)
	float4 Color: COLOR0;
#		endif
#	endif
};

struct VS_OUTPUT
{
#	if defined(SPECULAR) || defined(UNDERWATER)
	float4 HPosition: SV_POSITION0;
#		if !defined(UNIFIED_WATER)
	float4 FogParam: COLOR0;
#		endif
	float4 WPosition: TEXCOORD0;
	float4 TexCoord1: TEXCOORD1;
	float4 TexCoord2: TEXCOORD2;
#		if defined(WADING) || (defined(FLOWMAP) && (defined(REFRACTIONS) || defined(BLEND_NORMALS))) || (defined(VERTEX_ALPHA_DEPTH) && defined(VC)) || ((defined(SPECULAR) && NUM_SPECULAR_LIGHTS == 0) && defined(FLOWMAP) /*!defined(NORMAL_TEXCOORD) && !defined(BLEND_NORMALS) && !defined(VC)*/)
	float4 TexCoord3: TEXCOORD3;
#		endif
#		if defined(FLOWMAP)
	nointerpolation float2 TexCoord4: TEXCOORD4;
#		endif
#		if NUM_SPECULAR_LIGHTS == 0
	float4 MPosition: TEXCOORD5;
#		endif
#	endif

#	if defined(SIMPLE)
	float4 HPosition: SV_POSITION0;
	float4 FogParam: COLOR0;
	float4 WPosition: TEXCOORD0;
	float4 TexCoord1: TEXCOORD1;
	float4 TexCoord2: TEXCOORD2;
	float4 MPosition: TEXCOORD5;
#	endif

#	if defined(LOD)
	float4 HPosition: SV_POSITION0;
	float4 FogParam: COLOR0;
	float4 WPosition: TEXCOORD0;
	float4 TexCoord1: TEXCOORD1;
#	endif

#	if defined(STENCIL)
	float4 HPosition: SV_POSITION0;
	float4 WorldPosition: POSITION1;
	float4 PreviousWorldPosition: POSITION2;
#	endif

	float4 NormalsScale: TEXCOORD8;

#	if defined(FLUID_FIELD_FALLS)
	float3 FallNormal: TEXCOORD9;        // D159: the sheet's geometric normal, world axes
	float2 FallTexCoord: TEXCOORD10;     // ...its authored texture coordinates (FallGround::Flow's input)
	float3 FallFade: TEXCOORD11;         // ...FallGround::Fade: x seam fade, y over the water below, z its foot
	float4 FallData: TEXCOORD12;         // ...x water or air behind, y under its source water, z to the nearest other sheet
	float4 FallOutward: TEXCOORD13;      // ...normal turned outward; w 1 where another sheet lies behind its inner side
	float3 FallTangent: TEXCOORD14;      // D174: the way texture v runs, world axes
	float4 FallWobble: TEXCOORD15;       // D188: FallGround::WobbleAt (z in world units)
	float2 FallWobbleSizes: TEXCOORD16;  // FallGround::WobbleSizes, world units: the lighting follows all the wobble
	float3 FallPrevious: TEXCOORD17;     // its place a frame ago (camera-relative, the previous camera's): the motion vectors
	float FallClip: SV_ClipDistance0;
#	endif
};

#	ifdef VSHADER

cbuffer PerTechnique : register(b0)
{
	float4 QPosAdjust : packoffset(c0);
};

cbuffer PerMaterial : register(b1)
{
	float4 VSFogParam : packoffset(c0);
	float4 VSFogNearColor : packoffset(c1);
	float4 VSFogFarColor : packoffset(c2);
	float4 NormalsScroll0 : packoffset(c3);
	float4 NormalsScroll1 : packoffset(c4);
	float4 NormalsScale : packoffset(c5);
};

cbuffer PerGeometry : register(b2)
{
	row_major float4x4 World : packoffset(c0);
	row_major float4x4 PreviousWorld : packoffset(c4);
	row_major float4x4 WorldViewProj : packoffset(c8);
	float3 ObjectUV : packoffset(c12);
	float4 CellTexCoordOffset : packoffset(c13);
};

#		if defined(FLUID_FIELD_FALLS)
#			include "FluidField/FallGround.hlsli"

// D159: FallRefraction's per-piece constants (FallSurface.hlsl's FallSurfaceCB): the model to world transform
cbuffer FallSurfaceCB : register(b7)
{
	row_major float3x4 FallWorld : packoffset(c0);
};

struct FallSurfaceVertex
{
	float3 Position;  // Model space
	float2 TexCoord;  // Vanilla's
	float3 Normal;    // Model space
	float3 Tangent;   // D174: the direction v runs, model space (FallRefraction::Vertex)
};

StructuredBuffer<FallSurfaceVertex> FallVertices : register(t125);

/**
 * D159: a falls sheet's vertex, moved as FallSurface.hlsl moves it (seams, swell, wobble at the frame's latched phase, cut),
 * output as the river's vertex shader outputs its own.
 */
VS_OUTPUT main(uint vertexID : SV_VertexID)
{
	VS_OUTPUT vsout = (VS_OUTPUT)0;
	vsout.NormalsScale = NormalsScale;

	const FallSurfaceVertex fallVertex = FallVertices[vertexID];
	const float3 model = fallVertex.Position + FallGround::Shift(vertexID, FallGround::Scroll.x, FallWorld[2].xyz);
	const float3 relative = mul(FallWorld, float4(model, 1.0)) - FrameBuffer::CameraPosAdjust.xyz;
	const float4 worldViewPos = mul(FrameBuffer::CameraViewProj, float4(relative, 1.0));
	vsout.HPosition = worldViewPos;

#			if !defined(UNIFIED_WATER)
	float fogDistanceFactor = min(VSFogFarColor.w, pow(saturate(length(worldViewPos.xyz) * VSFogParam.y - VSFogParam.x), NormalsScale.w));
	vsout.FogParam.xyz = lerp(VSFogNearColor.xyz, VSFogFarColor.xyz, fogDistanceFactor);
	vsout.FogParam.w = fogDistanceFactor;
#			endif

	vsout.WPosition = float4(relative, length(relative));
	vsout.MPosition = float4(relative, 1.0);
	vsout.TexCoord1 = NormalsScroll0;
	vsout.TexCoord2 = float4(NormalsScroll1.xy, worldViewPos.w, 0.0);

	vsout.FallNormal = mul((float3x3)FallWorld, fallVertex.Normal);
	vsout.FallTangent = mul((float3x3)FallWorld, fallVertex.Tangent);
	vsout.FallWobble = FallGround::WobbleAt(vertexID, FallGround::Scroll.x);
	vsout.FallWobble.z *= length(FallWorld[0].xyz);  // its size in world units
	vsout.FallWobbleSizes = FallGround::WobbleSizes(vertexID, FallGround::Scroll.x, FallWorld[2].xyz, fallVertex.Normal) * length(FallWorld[0].xyz);
	vsout.FallPrevious = mul(FallWorld, float4(fallVertex.Position + FallGround::Shift(vertexID, FallGround::PreviousPhase, FallWorld[2].xyz), 1.0)) -
	                     FrameBuffer::CameraPreviousPosAdjust.xyz;
	vsout.FallTexCoord = fallVertex.TexCoord;
	vsout.FallFade.xy = FallGround::Fade(vertexID);
	vsout.FallFade.y = min(vsout.FallFade.y, 512.0);  // D139: the "none" value held before interpolation
	vsout.FallFade.z = (!FallGround::Enabled || FallGround::VertexBase == FallGround::NO_VERTEX_CLIP) ? 1.0 : fmod(FallGround::VertexClip[FallGround::VertexBase + vertexID].a.w, 2.0);
	// the depth behind moves with the vertex: pushed out along its normal (swell, wobble) the gap behind it widens, pulled in it narrows
	const float3 fallShift = FallGround::Shift(vertexID, FallGround::Scroll.x, FallWorld[2].xyz);
	const float fallShiftOut = dot(fallShift, fallVertex.Normal) / max(dot(fallVertex.Normal, fallVertex.Normal), 1e-6) * length(FallWorld[0].xyz);
	vsout.FallData = float4(max(FallGround::Behind(vertexID) + fallShiftOut, 0.0), min(FallGround::BelowSource(vertexID), 512.0), min(FallGround::ToOtherSheet(vertexID), 512.0), 0.0);
	vsout.FallOutward = float4(vsout.FallNormal * FallGround::OutwardSign(vertexID), FallGround::InnerCovered(vertexID));
	vsout.FallClip = FallGround::ClipDistance(relative, vertexID);
	return vsout;
}
#		else
VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout = (VS_OUTPUT)0;

	vsout.NormalsScale = NormalsScale;

	float4 inputPosition = float4(input.Position.xyz, 1.0);
	float4 worldPos = mul(World, inputPosition);
	float4 worldViewPos = mul(WorldViewProj, inputPosition);

	float heightMult = min((1.0 / 10000.0) * max(worldViewPos.z - 70000, 0), 1);

	vsout.HPosition.xy = worldViewPos.xy;
	vsout.HPosition.z = heightMult * 0.5 + worldViewPos.z;
	vsout.HPosition.w = worldViewPos.w;

#			if defined(HORIZON_FIX)
	vsout.HPosition.z = min(vsout.HPosition.z, vsout.HPosition.w * HorizonFix::FoldedDepth);
#			endif

#			if defined(STENCIL)
	vsout.WorldPosition = worldPos;
	vsout.PreviousWorldPosition = mul(PreviousWorld, inputPosition);
#			else

#				if !defined(UNIFIED_WATER)
	float fogDistanceFactor = min(VSFogFarColor.w, pow(saturate(length(worldViewPos.xyz) * VSFogParam.y - VSFogParam.x), NormalsScale.w));
	vsout.FogParam.xyz = lerp(VSFogNearColor.xyz, VSFogFarColor.xyz, fogDistanceFactor);
	vsout.FogParam.w = fogDistanceFactor;
#				endif

	vsout.WPosition.xyz = worldPos.xyz;
	vsout.WPosition.w = length(worldPos.xyz);

#				if defined(LOD)
	float4 posAdjust =
		ObjectUV.x ? 0.0 : (QPosAdjust.xyxy + worldPos.xyxy) / NormalsScale.xxyy;

	vsout.TexCoord1.xyzw = NormalsScroll0 + posAdjust;
#				else
#					if !defined(SPECULAR) || (NUM_SPECULAR_LIGHTS == 0)
	vsout.MPosition.xyzw = inputPosition.xyzw;
#					endif

	float2 posAdjust = worldPos.xy + QPosAdjust.xy;

	float2 scrollAdjust1 = posAdjust / NormalsScale.xx;
	float2 scrollAdjust2 = posAdjust / NormalsScale.yy;
	float2 scrollAdjust3 = posAdjust / NormalsScale.zz;

#					if defined(UNIFIED_WATER) && defined(NORMAL_TEXCOORD)
	float2 cellShift = float2(floor(ObjectUV.z * 0.5), floor((ObjectUV.z - 1.0) * 0.5));
	float2 scaledUV = input.TexCoord0.xy * ObjectUV.z - cellShift;
#					endif

#					if !(defined(FLOWMAP) && (defined(REFRACTIONS) || defined(BLEND_NORMALS) || defined(DEPTH) || NUM_SPECULAR_LIGHTS == 0))
#						if defined(NORMAL_TEXCOORD)
	float3 normalsScale = 0.001 * NormalsScale.xyz;
	if (ObjectUV.x) {
		scrollAdjust1 = input.TexCoord0.xy / normalsScale.xx;
		scrollAdjust2 = input.TexCoord0.xy / normalsScale.yy;
		scrollAdjust3 = input.TexCoord0.xy / normalsScale.zz;
	}
#						else
	if (ObjectUV.x) {
		scrollAdjust1 = 0.0;
		scrollAdjust2 = 0.0;
		scrollAdjust3 = 0.0;
	}
#						endif
#					endif

	vsout.TexCoord1 = 0.0;
	vsout.TexCoord2 = 0.0;
#					if defined(FLOWMAP)
#						if !(((defined(SPECULAR) || NUM_SPECULAR_LIGHTS == 0) || (defined(UNDERWATER) && defined(REFRACTIONS))) && !defined(NORMAL_TEXCOORD))
#							if defined(BLEND_NORMALS)
	vsout.TexCoord1.xy = NormalsScroll0.xy + scrollAdjust1;
	vsout.TexCoord1.zw = NormalsScroll0.zw + scrollAdjust2;
	vsout.TexCoord2.xy = NormalsScroll1.xy + scrollAdjust3;
#							else
	vsout.TexCoord1.xy = NormalsScroll0.xy + scrollAdjust1;
	vsout.TexCoord1.zw = 0.0;
	vsout.TexCoord2.xy = 0.0;
#							endif
#						endif
#						if !defined(NORMAL_TEXCOORD)
	vsout.TexCoord3 = 0.0;
#						elif defined(WADING)
#							if defined(UNIFIED_WATER)
	float2 wadingUV = (input.TexCoord0.xy - 0.5f) * 0.5f;
	vsout.TexCoord2.zw = (CellTexCoordOffset.xy + wadingUV) / ObjectUV.xy;
	vsout.TexCoord3.xy = CellTexCoordOffset.zw + wadingUV;
#							else
	vsout.TexCoord2.zw = ((-0.5 + input.TexCoord0.xy) * 0.1 + CellTexCoordOffset.xy) +
	                     float2(CellTexCoordOffset.z, -CellTexCoordOffset.w + ObjectUV.x) / ObjectUV.xx;
	vsout.TexCoord3.xy = -0.25 + (input.TexCoord0.xy * 0.5 + ObjectUV.yz);
#							endif
	vsout.TexCoord3.zw = input.TexCoord0.xy;
#						elif (defined(REFRACTIONS) || NUM_SPECULAR_LIGHTS == 0 || defined(BLEND_NORMALS))
#							if defined(UNIFIED_WATER)
	float2 dims = float2(ObjectUV.x, ObjectUV.y);
	vsout.TexCoord2.zw = (CellTexCoordOffset.xy + scaledUV) / dims;
	vsout.TexCoord3.xy = CellTexCoordOffset.zw + scaledUV;
	vsout.TexCoord3.zw = scaledUV;
#							else
	vsout.TexCoord2.zw = (CellTexCoordOffset.xy + input.TexCoord0.xy) / ObjectUV.xx;
	vsout.TexCoord3.xy = CellTexCoordOffset.zw + input.TexCoord0.xy;
	vsout.TexCoord3.zw = input.TexCoord0.xy;
#							endif
#						endif
	vsout.TexCoord4 = ObjectUV.xy;
#					else
	vsout.TexCoord1.xy = NormalsScroll0.xy + scrollAdjust1;
	vsout.TexCoord1.zw = NormalsScroll0.zw + scrollAdjust2;
	vsout.TexCoord2.xy = NormalsScroll1.xy + scrollAdjust3;
	vsout.TexCoord2.z = worldViewPos.w;
	vsout.TexCoord2.w = 0;
#						if (defined(WADING) || (defined(VERTEX_ALPHA_DEPTH) && defined(VC)))
	vsout.TexCoord3 = 0.0;
#							if (defined(NORMAL_TEXCOORD) && ((!defined(BLEND_NORMALS) && !defined(VERTEX_ALPHA_DEPTH)) || defined(WADING)))
	vsout.TexCoord3.xy = input.TexCoord0;
#							endif
#							if defined(VERTEX_ALPHA_DEPTH) && defined(VC)
	vsout.TexCoord3.z = input.Color.w;
#							endif
#						endif
#					endif
#				endif
#			endif

	return vsout;
}
#		endif  // FLUID_FIELD_FALLS

#	endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
#	if defined(UNDERWATER) || defined(SIMPLE) || defined(LOD) || defined(SPECULAR)
	float4 Lighting: SV_Target0;
#	endif

#	if defined(STENCIL)
	float4 WaterMask: SV_Target0;
	float2 MotionVector: SV_Target1;
#	elif defined(FLUID_FIELD_FALLS)
	float2 MotionVector: SV_Target1;  // the sheet's own (its wobble), where the falls pass binds them (FallRefraction)
#	endif
};

#	ifdef PSHADER

SamplerState ReflectionSampler : register(s0);
SamplerState RefractionSampler : register(s1);
#		if defined(FLUID_FIELD_FALLS)
// Falls debug (Waterfall Shading: Quadrants): one shading term over the whole screen, chosen by FALL_DEBUG_VIEW
#			define FALL_DEBUG_VIEW 0
#		endif
SamplerState DisplacementSampler : register(s2);
SamplerState CubeMapSampler : register(s3);
SamplerState Normals01Sampler : register(s4);
SamplerState Normals02Sampler : register(s5);
SamplerState Normals03Sampler : register(s6);
SamplerState DepthSampler : register(s7);
SamplerState FlowMapSampler : register(s8);
SamplerState FlowMapNormalsSampler : register(s9);
SamplerState SSRReflectionSampler : register(s10);
SamplerState RawSSRReflectionSampler : register(s11);

Texture2D<float4> ReflectionTex : register(t0);
Texture2D<float4> RefractionTex : register(t1);
Texture2D<float4> DisplacementTex : register(t2);
TextureCube<float4> CubeMapTex : register(t3);
Texture2D<float4> Normals01Tex : register(t4);
Texture2D<float4> Normals02Tex : register(t5);
Texture2D<float4> Normals03Tex : register(t6);
Texture2D<float4> DepthTex : register(t7);
Texture2D<float4> FlowMapTex : register(t8);
Texture2D<float4> FlowMapNormalsTex : register(t9);
Texture2D<float4> SSRReflectionTex : register(t10);
Texture2D<float4> RawSSRReflectionTex : register(t11);

cbuffer PerTechnique : register(b0)
{
	float4 VPOSOffset : packoffset(c0);  // inverse main render target width and height in xy, 0 in zw
	float4 PosAdjust : packoffset(c1);   // inverse framebuffer range in w
	float4 CameraDataWater : packoffset(c2);
	float4 SunDir : packoffset(c3);
	float4 SunColor : packoffset(c4);
}

cbuffer PerMaterial : register(b1)
{
	float4 ShallowColor : packoffset(c0);
	float4 DeepColor : packoffset(c1);
	float4 ReflectionColor : packoffset(c2);
	float4 FresnelRI : packoffset(c3);    // Fresnel amount in x, specular power in z
	float4 BlendRadius : packoffset(c4);  // flowmap scale in y, specular radius in z
	float4 VarAmounts : packoffset(c5);   // Sun specular power in x, reflection amount in y, alpha in z, refraction magnitude in w
	float4 NormalsAmplitude : packoffset(c6);
	float4 WaterParams : packoffset(c7);   // noise falloff in x, reflection magnitude in y, sun sparkle power in z, framebuffer range in w
	float4 FogNearColor : packoffset(c8);  // above water fog amount in w
	float4 FogFarColor : packoffset(c9);
	float4 FogParam : packoffset(c10);      // above water fog distance far in z, above water fog range in w
	float4 DepthControl : packoffset(c11);  // depth reflections factor in x, depth refractions factor in y, depth normals factor in z, depth specular lighting factor in w
	float4 SSRParams : packoffset(c12);     // fWaterSSRIntensity in x, fWaterSSRBlurAmount in y, inverse main render target width and height in zw
	float4 SSRParams2 : packoffset(c13);    // fWaterSSRNormalPerturbationScale in x
}

cbuffer PerGeometry : register(b2)
{
	float4x4 TextureProj : packoffset(c0);
	float4 ReflectPlane[1] : packoffset(c4);
	float4 ProjData : packoffset(c5);
	float4 LightPos[8] : packoffset(c6);
	float4 LightColor[8] : packoffset(c14);
}

#		if defined(FLUID_FIELD_FALLS)
#			include "FluidField/FallShading.hlsli"

// D159: FallRefraction's per-piece constants: whether the piece has a normal map and whether the front surface (D125) is bound
cbuffer FallSurfaceCB : register(b7)
{
	uint FallHasNormal : packoffset(c8.w);
	uint FallHasFront : packoffset(c9.x);
};

Texture2D<float4> FallNormalTex : register(t122);    // The piece's own normal map (vanilla's falls), on its flow
Texture2D<float4> FallFrontDepth : register(t105);   // D125: the scene's depth with the falls' front surface drawn in
Texture2D<float4> FallBehindDepth : register(t112);  // D174: the falls' surface nearest behind their front (cleared to far where none)
Texture2D<float4> FallFoamTex : register(t113);      // D174: the river's own Normals01, read as the foam's bubbles

/** D159: the sheet's geometric normal at this pixel, turned to the camera: the falls' "up" wherever the river's is (0, 0, 1). */
static float3 FallUp = float3(0, 0, 1);
/** D159: the plane through this pixel of the sheet, as ReflectPlane[0] (camera-relative): the depth behind is measured from it. */
static float4 FallPlane = float4(0, 0, 1, 0);
// Baked falls: how far the water has begun to fall (0 on the crest: the river plane's own shading, untouched; 1 once it drops)
static float FallFalling = 1.0;
/** D189: how far this pixel's reflected ray spreads (fast and aerated water, FallShading::SSR_ROUGH_*). */
static float FallRoughness = 0.0;
#		endif

/** The surface's plane (camera-relative, n.p + w = 0), from which the water behind it is measured. */
float4 GetWaterPlane()
{
#		if defined(FLUID_FIELD_FALLS)
	return FallPlane;
#		else
	return ReflectPlane[0];
#		endif
}

#		define SampColorSampler Normals01Sampler
#		define LinearSampler Normals01Sampler

#		if defined(SKYLIGHTING)
#			include "Skylighting/Skylighting.hlsli"
#		endif

#		if defined(EXP_HEIGHT_FOG)
#			include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#		endif

#		include "Common/ShadowSampling.hlsli"

#		if defined(SIMPLE) || defined(UNDERWATER) || defined(LOD) || defined(SPECULAR)
float GetWaterFogFade()
{
#			if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		return ExponentialHeightFog::GetVanillaFogFade(PosAdjust.w);
	}
#			endif
	return PosAdjust.w;
}

#			if defined(LOD)
#				undef FLUID_FIELD
#			endif

#			if defined(FLUID_FIELD)
#				include "FluidField/WaterNormal.hlsli"
#			endif

#			if defined(FLOWMAP)

/**
 * Structure containing complete flowmap information
 */
struct FlowmapData
{
	float4 color;       // Raw flowmap color (R=flow_x, G=flow_y, B=flow_strength, A=flow_mask)
	float2 flowVector;  // Flow vector (coordinate space depends on source function)
};

/**
 * Gets raw flowmap data before UV-space coordinate transformation
 *
 * @param input Pixel shader input containing texture coordinates
 * @param uvShift UV offset for sampling the flowmap texture
 * @return FlowmapData with raw components:
 *         - color: Raw flowmap texture sample (RG=rotation, B=strength, A=mask)
 *         - flowVector: Base flow vector before any coordinate transformation
 *                      Ready for direct application of rotation matrix for world positioning
 *
 * @details This function provides flowmap data in its original coordinate space, suitable
 *          for world-space positioning effects (like ripple movement). The flowVector has
 *          NOT been transformed for UV-space normal sampling - that transformation is only
 *          applied in GetFlowmapDataUV() which uses transpose for UV coordinate perturbation.
 *
 *          Use this function when you need to apply the rotation matrix directly for
 *          world-space effects without needing to reverse any existing transformations.
 *
 * @see GetFlowmapDataUV() for UV-space normal sampling (applies transpose transformation)
 */
FlowmapData GetFlowmapDataTextureSpace(PS_INPUT input, float2 uvShift)
{
	FlowmapData data;
	data.color = FlowMapTex.SampleLevel(FlowMapSampler, input.TexCoord2.zw + uvShift, 0);
	data.flowVector = (64 * input.TexCoord3.xy) * sqrt(1.01 - data.color.z);
	// NOTE: flowVector is NOT transformed yet - this is the raw vector before rotation matrix
	return data;
}
/**
 * Samples flowmap texture and calculates UV-space flow data for texture sampling
 *
 * @param input Pixel shader input containing texture coordinates and world position data
 * @param uvShift UV offset for sampling the flowmap texture (used for animation/variation)
 * @return FlowmapData Complete flowmap information with UV-space flow vector
 *
 * @details This function:
 *          - Samples the flowmap texture at the specified UV coordinates
 *          - Decodes flow direction from RG channels (remapped from [0,1] to [-1,1])
 *          - Calculates flow strength using the blue channel with sqrt falloff
 *          - Applies transpose rotation matrix to transform flow direction to UV space
 *          - Scales flow vector by world position and strength factors
 *
 * @note Flowmap format:
 *       - Red channel: Flow direction X component (0.5 = no flow, 0/1 = negative/positive flow)
 *       - Green channel: Flow direction Y component (0.5 = no flow, 0/1 = negative/positive flow)
 *       - Blue channel: Flow strength (0 = no flow, 1 = maximum flow)
 *       - Alpha channel: Flow mask/intensity multiplier
 */
FlowmapData GetFlowmapDataUV(PS_INPUT input, float2 uvShift)
{
	FlowmapData data = GetFlowmapDataTextureSpace(input, uvShift);
	float2 flowSinCos = data.color.xy * 2 - 1;
	float2x2 flowRotationMatrix = float2x2(flowSinCos.x, flowSinCos.y, -flowSinCos.y, flowSinCos.x);
	data.flowVector = mul(transpose(flowRotationMatrix), data.flowVector);
	return data;
}
// ----------------------------------------------------------------
// Flowmap Parallax Functions
// ----------------------------------------------------------------

/**
 * Samples height from flowmap texture using the same 4-sample blend as flowmap normals
 * This ensures height transitions match the normal transitions exactly
 *
 * @param input PS_INPUT for flowmap coordinate access
 * @param normalMul The blend weights from the flowmap system (same as used for normals)
 * @param uvShift The UV shift value (1 / (128 * flowmapDimensions))
 * @param mipLevel Mip level for texture sampling
 */
float GetFlowmapHeightBlended(PS_INPUT input, float2 normalMul, float2 uvShift, float mipLevel)
{
	// Sample height using the EXACT same UV computation as GetFlowmapNormal
	// This ensures the height blending matches the normal blending perfectly

	// Sample 0: uvShift, multiplier=9.92, offset=0
	FlowmapData flowData0 = GetFlowmapDataUV(input, uvShift);
	float2 uv0 = 0 + (flowData0.flowVector - float2(9.92 * ((0.001 * ReflectionColor.w) * flowData0.color.w), 0));
	float height0 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv0, mipLevel).w;

	// Sample 1: float2(0, uvShift.y), multiplier=10.64, offset=0.27
	FlowmapData flowData1 = GetFlowmapDataUV(input, float2(0, uvShift.y));
	float2 uv1 = 0.27 + (flowData1.flowVector - float2(10.64 * ((0.001 * ReflectionColor.w) * flowData1.color.w), 0));
	float height1 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv1, mipLevel).w;

	// Sample 2: 0.0.xx, multiplier=8, offset=0
	FlowmapData flowData2 = GetFlowmapDataUV(input, 0.0.xx);
	float2 uv2 = 0 + (flowData2.flowVector - float2(8 * ((0.001 * ReflectionColor.w) * flowData2.color.w), 0));
	float height2 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv2, mipLevel).w;

	// Sample 3: float2(uvShift.x, 0), multiplier=8.48, offset=0.62
	FlowmapData flowData3 = GetFlowmapDataUV(input, float2(uvShift.x, 0));
	float2 uv3 = 0.62 + (flowData3.flowVector - float2(8.48 * ((0.001 * ReflectionColor.w) * flowData3.color.w), 0));
	float height3 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv3, mipLevel).w;

	// Use the EXACT same blending formula as flowmap normals
	float blendedHeight =
		normalMul.y * (normalMul.x * height2 + (1 - normalMul.x) * height3) +
		(1 - normalMul.y) * (normalMul.x * height1 + (1 - normalMul.x) * height0);

	return blendedHeight;
}

// Keep this for compatibility - just forwards to the proper function
float GetFlowmapHeightBarycentric(PS_INPUT input, float2 flowmapDimensions, float2 baseUV, float mipLevel)
{
	// This is now unused - we use GetFlowmapHeightBlended directly
	return FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, baseUV, mipLevel).w;
}

/**
 * Computes mip level from continuous (non-flowmapped) UVs.
 * Flow-rotated UVs have discontinuous ddx/ddy and must not be used for LOD.
 */
float GetFlowmapMipLevel(float2 baseUV)
{
	float2 textureDims;
	FlowMapNormalsTex.GetDimensions(textureDims.x, textureDims.y);

	float2 texCoordsPerSize = baseUV * textureDims;
	float2 dxSize = ddx(texCoordsPerSize);
	float2 dySize = ddy(texCoordsPerSize);
	float2 dTexCoords = dxSize * dxSize + dySize * dySize;
	float minTexCoordDelta = max(dTexCoords.x, dTexCoords.y);
	return max(0.5 * log2(minTexCoordDelta), 0);
}

/**
 * Samples height from flowmap texture (riverflow.dds alpha channel)
 * Uses the same UV calculation as GetFlowmapNormal for consistency
 */

/**
 * Generates flowmap-based normal (no parallax - flowmap normals are not parallax-shifted).
 * Mip LOD uses non-flowmapped TexCoord3 derivatives — flow-rotated UVs break hardware/manual
 * mip selection (stuck mip 0 shimmer, or excessive LOD + UV shrink that flattens normals).
 */
float3 GetFlowmapNormal(PS_INPUT input, float2 uvShift, float multiplier, float offset, float2 dx, float2 dy)
{
	FlowmapData flowData = GetFlowmapDataUV(input, uvShift);
	float2 uv = offset + (flowData.flowVector - float2(multiplier * ((0.001 * ReflectionColor.w) * flowData.color.w), 0));

	// Match UV footprint: flowVector scales by this (see GetFlowmapDataTextureSpace)
	float flowStrength = sqrt(1.01 - flowData.color.z);
	float mipScale = exp2(SharedData::MipBias) * flowStrength;

	return float3(FlowMapNormalsTex.SampleGrad(FlowMapNormalsSampler, uv, dx * mipScale, dy * mipScale).xy, flowData.color.z);
}

/**
 * Gets flowmap data with world-space flow vector for positioning effects
 *
 * @param input Pixel shader input containing texture coordinates
 * @param uvShift UV offset for flowmap sampling (used for animation phases)
 * @return FlowmapData Complete flowmap information with world-space flow vector
 *
 * @details This function:
 *          - Samples raw flowmap data (before UV-space transformations)
 *          - Decodes flow direction from flowmap RG channels
 *          - Applies component-wise directional transformation
 *          - Returns complete flowmap data with world-space flow vector
 *
 * @note Use this for effects that need to move with water current (ripples, debris, foam, etc.)
 *       For UV-space normal sampling, use GetFlowmapDataUV() instead
 */
FlowmapData GetFlowmapDataWorldSpace(PS_INPUT input, float2 uvShift)
{
	FlowmapData data = GetFlowmapDataTextureSpace(input, uvShift);
	float2 flowDirection = -(data.color.xy * 2 - 1);    // Decode direction with 180° correction
	data.flowVector = data.flowVector * flowDirection;  // Transform to world space
	return data;
}

/**
 * Converts existing texture-space flowmap data to world-space (avoids duplicate sampling)
 *
 * @param textureSpaceData FlowmapData from GetFlowmapDataTextureSpace()
 * @return FlowmapData Complete flowmap data with world-space flow vector
 *
 * @note Use this overload when you already have texture-space flowmap data to avoid duplicate texture sampling
 */
FlowmapData GetFlowmapDataWorldSpace(FlowmapData textureSpaceData)
{
	FlowmapData data = textureSpaceData;
	float2 flowDirection = -(data.color.xy * 2 - 1);    // Decode direction with 180° correction
	data.flowVector = data.flowVector * flowDirection;  // Transform to world space
	return data;
}
#			endif

#			if defined(LOD)
#				undef WATER_EFFECTS
#				undef WETNESS_EFFECTS
#			endif

#			if defined(WATER_EFFECTS) && !defined(VC)
#				define WATER_PARALLAX
#				include "WaterEffects/WaterParallax.hlsli"
#			endif

#			if defined(DYNAMIC_CUBEMAPS)
#				include "DynamicCubemaps/DynamicCubemaps.hlsli"
#			endif

#			if defined(WETNESS_EFFECTS)
#				include "WetnessEffects/WetnessEffects.hlsli"
#			endif

// Structure to return both normal and ripple/splash color information
struct WaterNormalData
{
	float3 normal;
	float4 rippleInfo;  // xyz = scaled ripple normal (normalized normal * intensity), w = splash effect intensity
};

WaterNormalData GetWaterNormal(PS_INPUT input, float distanceFactor, float normalsDepthFactor, float3 viewDirection, float depth, float wetnessOcclusion)
{
	WaterNormalData result;
	result.rippleInfo = float4(0, 0, 0, 0);

	float3 normalScalesRcp = rcp(input.NormalsScale.xyz);

	float4 normalsAmplitude = NormalsAmplitude;
	if (SharedData::enbSettings.EnableWater) {
		normalsAmplitude *= SharedData::enbSettings.WaterWavesAmplitude;
	}

#			if defined(FLUID_FIELD) && !defined(FLUID_FIELD_FALLS)
	{
		// Fluid Field's surface deformation reshapes vanilla's own normals (D17): every normal
		// texture lookup below, base and flowmap, shifts with the simulated slope, and the ripples
		// get rougher where the surface is steep
		float2 fluidWarp;
		float fluidRoughness;
		FluidField::GetVanillaWarp(input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz, fluidWarp, fluidRoughness);
		FluidField::PixelRoughness = fluidRoughness;
		input.TexCoord1.xy += fluidWarp * normalScalesRcp.x;
		input.TexCoord1.zw += fluidWarp * normalScalesRcp.y;
		input.TexCoord2.xy += fluidWarp * normalScalesRcp.z;
#				if defined(FLOWMAP)
		// Flowmap normals use their own coordinates; convert world units with the screen derivatives
		float worldPerPixel = length(ddx(input.WPosition.xy)) + 1e-4;
		input.TexCoord3.xy += fluidWarp * (length(ddx(input.TexCoord3.xy)) / worldPerPixel);
#				endif
		normalsAmplitude.xyz *= FluidField::RippleBoost(fluidRoughness, viewDirection);
	}
#			endif

#			if defined(WATER_PARALLAX)
	float2 parallaxOffset = WaterEffects::GetParallaxOffset(input, normalsAmplitude.xyz, normalScalesRcp);
#			endif

#			if defined(FLOWMAP)
#				if defined(UNIFIED_WATER)
	float2 flowmapDimensions = input.TexCoord4.xy;
#				else
	float2 flowmapDimensions = input.TexCoord4.xx;
#				endif
	float2 uvShift = 1 / (128 * flowmapDimensions);

	// Compute flowmap parallax and create parallaxed input for normal sampling
	PS_INPUT flowmapInput = input;
	float2 flowmapParallaxOffset = float2(0, 0);
#				if defined(WATER_PARALLAX) && !defined(LOD)
	float parallaxAmount = WaterEffects::GetFlowmapParallaxAmount(input, flowmapDimensions, viewDirection);
	float2 parallaxDir = viewDirection.xy / -viewDirection.z;
	parallaxDir.y = -parallaxDir.y;
	float viewDotUp = -viewDirection.z;
	parallaxDir *= 0.008 * saturate(viewDotUp * 2.0);
	flowmapInput.TexCoord3.xy = input.TexCoord3.xy + parallaxAmount * parallaxDir;
	flowmapParallaxOffset = WaterEffects::GetFlowmapParallaxOffset(input, flowmapDimensions, viewDirection, normalsAmplitude.x, normalScalesRcp);
#				endif

	// Calculate cell blend weights using parallaxed input
	float2 normalMul = 0.5 + -(-0.5 + abs(frac(flowmapInput.TexCoord2.zw * (64 * flowmapDimensions)) * 2 - 1));

	// Same UV scale as flowVector before rotation — shared so all 4 blend samples use matching LOD
	float2 baseUV = 64 * flowmapInput.TexCoord3.xy;
	float2 flowNormalDx = ddx(baseUV);
	float2 flowNormalDy = ddy(baseUV);

	// Sample flowmap normals with parallax applied
	float3 flowmapNormal0 = GetFlowmapNormal(flowmapInput, uvShift, 9.92, 0, flowNormalDx, flowNormalDy);
	float3 flowmapNormal1 = GetFlowmapNormal(flowmapInput, float2(0, uvShift.y), 10.64, 0.27, flowNormalDx, flowNormalDy);
	float3 flowmapNormal2 = GetFlowmapNormal(flowmapInput, 0.0.xx, 8, 0, flowNormalDx, flowNormalDy);
	float3 flowmapNormal3 = GetFlowmapNormal(flowmapInput, float2(uvShift.x, 0), 8.48, 0.62, flowNormalDx, flowNormalDy);

	float2 flowmapNormalWeighted =
		normalMul.y * (normalMul.x * flowmapNormal2.xy + (1 - normalMul.x) * flowmapNormal3.xy) +
		(1 - normalMul.y) *
			(normalMul.x * flowmapNormal1.xy + (1 - normalMul.x) * flowmapNormal0.xy);
	float2 flowmapDenominator = sqrt(normalMul * normalMul + (1 - normalMul) * (1 - normalMul));
	float3 flowmapNormal =
		float3(((-0.5 + flowmapNormalWeighted) / (flowmapDenominator.x * flowmapDenominator.y)) *
				   max(0.4, normalsDepthFactor),
			0);
	flowmapNormal.z =
		sqrt(1 - flowmapNormal.x * flowmapNormal.x - flowmapNormal.y * flowmapNormal.y);
	float2 baseNormalUv = input.TexCoord1.xy;
#				if defined(WATER_PARALLAX)
	// Use flowmap-derived parallax offset for base normals
	baseNormalUv += flowmapParallaxOffset.xy * normalScalesRcp.x;
#				endif
	float3 normals1 = Normals01Tex.SampleBias(Normals01Sampler, baseNormalUv, SharedData::MipBias).xyz * 2.0 + float3(-1, -1, -2);
#			endif  // End of FLOWMAP block

#			if !defined(FLOWMAP)
#				if defined(WATER_PARALLAX)
	float3 normals1 = Normals01Tex.SampleBias(Normals01Sampler, input.TexCoord1.xy + parallaxOffset.xy * normalScalesRcp.x, SharedData::MipBias).xyz * 2.0 + float3(-1, -1, -2);
#				else
	float3 normals1 = Normals01Tex.SampleBias(Normals01Sampler, input.TexCoord1.xy, SharedData::MipBias).xyz * 2.0 + float3(-1, -1, -2);
#				endif
#			endif  // End of !FLOWMAP block
#			if defined(FLOWMAP) && !defined(BLEND_NORMALS)
#				ifdef DISABLE_FLOWMAP_NORMALS
	// FLOWMAP NORMALS DISABLED: Using only base normals (flow system still active for ripples/splashes)
	float3 finalNormal = normalize(normals1 + float3(0, 0, 1));
#				else
	// FLOWMAP NORMALS ENABLED: Blending flow-based normals with base normals
#					if defined(FLUID_FIELD)
#						if defined(FLUID_FIELD_FALLS)
	// D174: no flowmap normal on the falls (the river simulation's weight at their xy switched it on under overhangs)
	float3 finalNormal = normalize(normals1 + float3(0, 0, 1));
#						else
	float3 finalNormal = normalize(lerp(normals1 + float3(0, 0, 1), flowmapNormal, distanceFactor * FluidField::FlowmapWeight(input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz)));
#						endif
#					else
	float3 finalNormal = normalize(lerp(normals1 + float3(0, 0, 1), flowmapNormal, distanceFactor));
#					endif
#				endif
#			elif !defined(LOD)

#				if defined(WATER_PARALLAX)
	float3 normals2 = Normals02Tex.SampleBias(Normals02Sampler, input.TexCoord1.zw + parallaxOffset.xy * normalScalesRcp.y, SharedData::MipBias).xyz * 2.0 - 1.0;
	float3 normals3 = Normals03Tex.SampleBias(Normals03Sampler, input.TexCoord2.xy + parallaxOffset.xy * normalScalesRcp.z, SharedData::MipBias).xyz * 2.0 - 1.0;
#				else
	float3 normals2 = Normals02Tex.SampleBias(Normals02Sampler, input.TexCoord1.zw, SharedData::MipBias).xyz * 2.0 - 1.0;
	float3 normals3 = Normals03Tex.SampleBias(Normals03Sampler, input.TexCoord2.xy, SharedData::MipBias).xyz * 2.0 - 1.0;
#				endif

	float3 blendedNormal = normalize(float3(0, 0, 1) + normalsAmplitude.x * normals1 +
									 normalsAmplitude.y * normals2 + normalsAmplitude.z * normals3);
#				if defined(UNDERWATER)
	float3 finalNormal = blendedNormal;
#				else
	float3 finalNormal = normalize(lerp(float3(0, 0, 1), blendedNormal, normalsDepthFactor));
#				endif

#				if defined(FLOWMAP)
	float normalBlendFactor =
		normalMul.y * ((1 - normalMul.x) * flowmapNormal3.z + normalMul.x * flowmapNormal2.z) +
		(1 - normalMul.y) * (normalMul.x * flowmapNormal1.z + (1 - normalMul.x) * flowmapNormal0.z);
#					if defined(FLUID_FIELD)
	// Still simulated water keeps vanilla's wind ripples instead of flow normals
#						if defined(FLUID_FIELD_FALLS)
	normalBlendFactor = 0.0;  // D174: as above, no flowmap normal on the falls
#						else
	normalBlendFactor *= FluidField::FlowmapWeight(input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz);
#						endif
#					endif
	finalNormal = normalize(lerp(normals1 + float3(0, 0, 1), normalize(lerp(finalNormal, flowmapNormal, normalBlendFactor)), distanceFactor));
#				endif
#			else
	float3 finalNormal =
		normalize(float3(0, 0, 1) + normalsAmplitude.xxx * normals1);
#			endif

#			if defined(WADING)
#				if defined(FLOWMAP)
	float2 displacementUv = input.TexCoord3.zw;
#				else
	float2 displacementUv = input.TexCoord3.xy;
#				endif
	float3 displacement = normalize(float3(normalsAmplitude.w * (-0.5 + DisplacementTex.Sample(DisplacementSampler, displacementUv).zw),
		0.04));
#				if defined(FLUID_FIELD)
	// Fluid Field's wakes replace vanilla's displacement trail where it simulates the water (D17)
	displacement = normalize(lerp(displacement, float3(0, 0, 1), FluidField::GetCoverage(input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz)));
#				endif
	finalNormal = lerp(displacement, finalNormal, displacement.z);
#			endif

#			if defined(FLUID_FIELD) && !defined(FLUID_FIELD_FALLS)
	{
		// Fluid Field's simulated waves on top of the vanilla normal, at the parallax-shifted position
		float3 fluidFieldPosition = input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz;
#				if defined(WATER_PARALLAX)
		fluidFieldPosition.xy += parallaxOffset;
#				endif
		finalNormal = FluidField::ApplyWaterNormal(fluidFieldPosition, finalNormal);
#				if !defined(UNDERWATER)
		finalNormal = FluidField::KeepReflectionAboveHorizon(finalNormal, viewDirection, fluidFieldPosition);
#				endif
	}
#			endif

#			if defined(WETNESS_EFFECTS)
	// Wetness Effects Debug System:
	// DEBUG_WETNESS_EFFECTS Color Legend:
	// - BRIGHT MAGENTA: Ripples, BRIGHT GREEN: Splashes, CYAN: Both effects
	float4 raindropInfo = float4(0, 0, 1, 0);
	float maxRainDropDistance = SharedData::wetnessEffectsSettings.RaindropFxRange * SharedData::wetnessEffectsSettings.RaindropFxRange * 3;
	float rainDropDistance = dot(input.WPosition.xyz, input.WPosition.xyz);
	float distanceFadeout = saturate((1 - saturate(rainDropDistance / maxRainDropDistance)) * 3);
	if (finalNormal.z > 0 && SharedData::wetnessEffectsSettings.Raining > 0.0f && SharedData::wetnessEffectsSettings.EnableRaindropFx &&
		(rainDropDistance < maxRainDropDistance) && wetnessOcclusion > 0.05) {
		float rippleStrengthModifier = (wetnessOcclusion * wetnessOcclusion) * distanceFadeout;
		float3 rippleWPosition = input.WPosition.xyz + finalNormal * 16;
#				if defined(WATER_PARALLAX)
		rippleWPosition.xy += parallaxOffset;
#				endif
#				if defined(FLOWMAP)
		// Flow-following ripple enhancement: Makes raindrops follow water current
		FlowmapData worldFlowData = GetFlowmapDataWorldSpace(input, float2(0, 0));

		// Calculate flow-aware ripple offset using centralized timing logic
		// Parameters: avgFlowmapMultiplier=9.26 (average of GetWaterNormal flowmap normal multipliers: 9.92, 10.64, 8, 8.48)
		// uvToWorldScale=0.125 (1/8 - relates to 64× texture coordinate scaling factor)
		float2 flowOffset = WetnessEffects::GetFlowAwareRippleOffset(
			worldFlowData.flowVector,
			worldFlowData.color.w,      // Flow strength from flowmap alpha
			0.001 * ReflectionColor.w,  // Reflection timing scale (matches GetFlowmapNormal)
			9.26,                       // Average flowmap normal multiplier
			0.125                       // UV-to-world scale factor (1/8)
		);

		rippleWPosition.xy += flowOffset;
#				endif
		raindropInfo = WetnessEffects::GetRainDrops(rippleWPosition + FrameBuffer::CameraPosAdjust.xyz, SharedData::wetnessEffectsSettings.Time, finalNormal, rippleStrengthModifier);

		// Calculate ripple and splash color intensities
		float rippleIntensity = length(raindropInfo.xy) * rippleStrengthModifier;
		float splashIntensity = raindropInfo.w * distanceFadeout;

		// Store ripple and splash information for color effects
		result.rippleInfo.xyz = raindropInfo.xyz * rippleIntensity;
		result.rippleInfo.w = splashIntensity;
	}
	float3 rippleNormal = normalize(raindropInfo.xyz);
	finalNormal = ReorientNormal(rippleNormal, finalNormal);
#			endif

	result.normal = finalNormal;
	return result;
}

float3 GetWaterSpecularColor(PS_INPUT input, float3 normal, float3 viewDirection, float distanceFactor, float skylightingSpecular)
{
	if (!(Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Reflections))
		return ReflectionColor.xyz * VarAmounts.y;

#			if defined(FLUID_FIELD_FALLS)
	float3 R = reflect(viewDirection, WaterParams.y * normal + FallUp * (1 - WaterParams.y));
#			else
	float3 R = reflect(viewDirection, WaterParams.y * normal + float3(0, 0, 1 - WaterParams.y));
#			endif
#			if defined(FLUID_FIELD_FALLS)
	// D174: falling water is rough: a blurred sky
	const float reflectionMip = FallShading::REFLECTION_BLUR_MIPS;
#			else
	const float reflectionMip = 0.0;
#			endif
	float3 reflectionColor = CubeMapTex.SampleLevel(CubeMapSampler, R, reflectionMip).xyz;

#			if defined(DYNAMIC_CUBEMAPS)
	float3 dynamicCubemap;
	if (SharedData::InInterior) {
		dynamicCubemap = DynamicCubemaps::EnvTexture.SampleLevel(CubeMapSampler, R, reflectionMip).xyz;
	} else {
		float3 specularIrradiance = 1.0;
		if (skylightingSpecular < 1.0)
			specularIrradiance = Color::IrradianceToLinear(DynamicCubemaps::EnvTexture.SampleLevel(CubeMapSampler, R, reflectionMip).xyz);

		float3 specularIrradianceReflections = 1.0;
		if (skylightingSpecular > 0.0)
			specularIrradianceReflections = Color::IrradianceToLinear(DynamicCubemaps::EnvReflectionsTexture.SampleLevel(CubeMapSampler, R, reflectionMip).xyz);

		dynamicCubemap = Color::IrradianceToGamma(lerp(specularIrradiance, specularIrradianceReflections, skylightingSpecular));
	}

	float reflectionAmount = saturate(length(input.WPosition.xyz) / 1024.0);

	if (SharedData::HideSky)
		reflectionAmount = 0.0;
	reflectionColor = lerp(dynamicCubemap, reflectionColor, reflectionAmount);
#			endif

#			if defined(FLUID_FIELD_FALLS)
	{
		// D159: the engine's SSR assumes a level plane and the water mask: the falls march their own ray (D155) through the scene behind
		const float pointingDirection = dot(viewDirection, R) * 0.5 + 0.5;
		const float pointingAlignment = dot(reflect(viewDirection, FallUp), R) * 0.5 + 0.5;
		const float ssrAmount = sqrt(saturate(min(pointingAlignment, pointingDirection)));
		float2 dimensions;
		RefractionTex.GetDimensions(dimensions.x, dimensions.y);
		// D189: per-pixel, per-frame tilt on rough water and a jittered first step, resolved over frames
		const float3 spread = float3(Random::InterleavedGradientNoise(input.HPosition.xy, SharedData::FrameCount),
								  Random::InterleavedGradientNoise(input.HPosition.xy + 17.3, SharedData::FrameCount),
								  Random::InterleavedGradientNoise(input.HPosition.xy + 41.7, SharedData::FrameCount)) -
		                      0.5;
		float3 rough = FallShading::SafeNormalize(R + spread * (2.0 * FallRoughness), R);
		rough = dot(rough, FallUp) < 0.0 ? rough - FallUp * (2.0 * dot(rough, FallUp)) : rough;
		const float jitter = Random::InterleavedGradientNoise(input.HPosition.xy + 5.1, SharedData::FrameCount);
		const float4 hit = FallShading::ScreenReflection(RefractionTex, DepthTex, RefractionSampler, input.WPosition.xyz, FallShading::SafeNormalize(rough, FallUp), dimensions, dimensions * FrameBuffer::DynamicResolutionParams1.xy, jitter);
		reflectionColor = lerp(reflectionColor, max(0, hit.xyz), saturate(hit.w * distanceFactor * ssrAmount));
	}
#			elif !defined(LOD) && NUM_SPECULAR_LIGHTS == 0
	float pointingDirection = dot(viewDirection, R) * 0.5 + 0.5;
	float pointingAlignment = dot(reflect(viewDirection, float3(0, 0, 1)), R) * 0.5 + 0.5;
	float ssrAmount = sqrt(min(pointingAlignment, pointingDirection));
	float2 ssrReflectionUv = ((FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy) * SSRParams.zw) + 0.05 * normal.xy;
	float2 ssrReflectionUvDR = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(ssrReflectionUv);
	float4 ssrReflectionColorBlurred = SSRReflectionTex.Sample(SSRReflectionSampler, ssrReflectionUvDR);
	float4 ssrReflectionColorRaw = RawSSRReflectionTex.Sample(RawSSRReflectionSampler, ssrReflectionUvDR);
	float4 ssrReflectionColor = lerp(ssrReflectionColorBlurred, ssrReflectionColorRaw, ssrAmount * 0.7);
	float3 finalSsrReflectionColor = max(0, ssrReflectionColor.xyz);
	float ssrFraction = saturate(ssrReflectionColor.w * distanceFactor * ssrAmount);
	reflectionColor = lerp(reflectionColor, finalSsrReflectionColor, ssrFraction);
#			endif

	return reflectionColor;
}

// Effects 11 water: depth over which the surface reaches full opacity at the shore
static const float ShoreFadeDepth = 12.0;

float GetScreenDepthWater(float2 screenPosition)
{
	float depth = DepthTex.Load(float3(screenPosition, 0)).x;
	return (CameraDataWater.w / (-depth * CameraDataWater.z + CameraDataWater.x));
}

float3 GetLdotN(float3 normal)
{
#			if defined(UNDERWATER)
	return 1;
#			else
	if (Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Interior)
		return 1;
	return saturate(dot(SunDir.xyz, normal));
#			endif
}

float GetFresnelValue(float3 normal, float3 viewDirection)
{
#			if defined(UNDERWATER)
	float3 actualNormal = -normal;
#			else
	float3 actualNormal = normal;
#			endif
	float viewAngle = 1 - saturate(dot(-viewDirection, actualNormal));

	if (SharedData::enbSettings.EnableWater) {
		float fresnelExponent = SharedData::enbSettings.WaterFresnelMultiplier;
		// pow(0, 0) is NaN on GPU
		float fresnelRI = fresnelExponent != 0.0 ? pow(abs(FresnelRI.x), fresnelExponent) : 1.0;
		float fresnel = (1 - fresnelRI) * pow(viewAngle, 5) + fresnelRI;
		fresnel = lerp(SharedData::enbSettings.WaterFresnelMin, SharedData::enbSettings.WaterFresnelMax, fresnel);
		return fresnel * SharedData::enbSettings.WaterReflectionAmount;
	}

	return (1 - FresnelRI.x) * pow(viewAngle, 5) + FresnelRI.x;
}

struct DiffuseOutput
{
	float3 refractionColor;
	float3 refractionDiffuseColor;
	float depth;
	float refractionMul;
	float3 refractedViewDirection;
};

DiffuseOutput GetWaterDiffuseColor(PS_INPUT input, float3 normal, float3 viewDirection, inout float4 distanceMul, float refractionsDepthFactor, float fresnel, float3 viewPosition, float depth)
{
#			if defined(REFRACTIONS)
#				if defined(FLUID_FIELD_FALLS)
	// D159: the river's offset along the sheet, by the normal's tilt off the sheet's own normal (MPosition is camera-relative here)
	const float3 fallTilt = normal - dot(normal, FallUp) * FallUp;
	const float4 refractionClip = mul(FrameBuffer::CameraViewProj, float4(input.MPosition.xyz + VarAmounts.w * refractionsDepthFactor * fallTilt, 1));
	float2 refractionUvRaw = saturate(refractionClip.xy / max(refractionClip.w, 1e-4) * float2(0.5, -0.5) + 0.5);
#				else
	float4 refractionNormal = mul(transpose(TextureProj), float4((VarAmounts.w * refractionsDepthFactor * normal.xy) + input.MPosition.xy, input.MPosition.z, 1));

	float2 refractionUvRaw = float2(refractionNormal.x, refractionNormal.w - refractionNormal.y) / refractionNormal.ww;
#				endif
	float2 screenPosition = FrameBuffer::DynamicResolutionParams1.xy * (FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy);

	float2 refractionScreenPosition = FrameBuffer::DynamicResolutionParams1.xy * (refractionUvRaw / VPOSOffset.xy);
	float4 refractionWorldPosition = float4(input.WPosition.xyz * depth / viewPosition.z, 0);

#				if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH)
	float refractionDepth = GetScreenDepthWater(refractionScreenPosition);
	depth = refractionDepth;
	float refractionDepthMul = length(float3((((VPOSOffset.zw + refractionUvRaw) * 2 - 1)) * refractionDepth / ProjData.xy, refractionDepth));

	float3 refractionDepthAdjustedViewDirection = -viewDirection * refractionDepthMul;
	float refractionViewSurfaceAngle = dot(refractionDepthAdjustedViewDirection, GetWaterPlane().xyz);

	float refractionPlaneMul = (1 - GetWaterPlane().w / refractionViewSurfaceAngle);

	if (refractionPlaneMul < 0.0) {
		refractionUvRaw = FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy * VPOSOffset.xy + VPOSOffset.zw;
	} else {
		float muddiness = SharedData::enbSettings.EnableWater ? SharedData::enbSettings.WaterMuddiness : 1.0;
		distanceMul = saturate(float4(1, muddiness, 1, 1) * refractionPlaneMul * float4(length(refractionDepthAdjustedViewDirection).xx, abs(refractionViewSurfaceAngle).xx) / FogParam.z);

		refractionWorldPosition = mul(FrameBuffer::CameraViewProjInverse, float4((refractionUvRaw * 2 - 1) * float2(1, -1), DepthTex.Load(float3(refractionScreenPosition, 0)).x, 1));
		refractionWorldPosition.xyz /= refractionWorldPosition.w;
	}

#					if defined(HORIZON_FIX)
	if (DepthTex.Load(float3(refractionScreenPosition, 0)).x >= HorizonFix::EmptyDepthThreshold)
		distanceMul = 1.0.xxxx;
#					endif
#					if defined(FLUID_FIELD_FALLS)
	// D180: a falling sheet holds its own thickness along the view, not the (mostly air) path behind it, which read as deep, opaque water
	distanceMul = lerp(distanceMul, min(distanceMul, saturate(FallShading::SHEET_DEPTH / max(abs(dot(viewDirection, FallUp)), 0.25) / FogParam.z)), FallFalling);
#					endif
#				endif

	float2 refractionUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(refractionUvRaw);
	float3 refractionColor = RefractionTex.Sample(RefractionSampler, refractionUV).xyz;

#				if defined(UNDERWATER)
	float refractionMul = 0;
#				else
	float refractionMul = 1 - pow(saturate((-distanceMul.x * FogParam.z + FogParam.z) / FogParam.w), FogNearColor.w);
#				endif

	float3 refractionDiffuseColor;

	if (SharedData::enbSettings.EnableWater) {
		float3 shallowColor = ShallowColor.xyz;
		float maxValue = max(shallowColor.x, max(shallowColor.y, shallowColor.z));
		if (maxValue > 0.0)
			shallowColor /= maxValue;
		else
			shallowColor = 1.0;

		// Tinted refraction only reads near the surface; deeper water fades to the flat shallow color
		shallowColor = lerp(shallowColor.xyz * refractionColor, ShallowColor.xyz, lerp(SharedData::enbSettings.WaterMuddiness, 1.0, refractionMul));
		refractionDiffuseColor = lerp(Color::Water(shallowColor.xyz), Color::Water(DeepColor.xyz), distanceMul.y);
	} else {
		refractionDiffuseColor = lerp(Color::Water(ShallowColor.xyz), Color::Water(DeepColor.xyz), distanceMul.y);
	}

	DiffuseOutput output;
	output.refractionColor = refractionColor;
	output.refractionDiffuseColor = refractionDiffuseColor;
	output.depth = depth;
	output.refractionMul = refractionMul;
	output.refractedViewDirection = normalize(refractionWorldPosition.xyz - input.WPosition.xyz);
	return output;
#			else
	DiffuseOutput output;
	output.refractionColor = lerp(Color::Water(ShallowColor.xyz), Color::Water(DeepColor.xyz), fresnel) * GetLdotN(normal);
	output.refractionDiffuseColor = output.refractionColor;
	output.depth = 1;
	output.refractionMul = 1;
	output.refractedViewDirection = viewDirection;
	return output;
#			endif
}

float3 GetSunColor(float3 normal, float3 viewDirection, float3 worldPosition)
{
#			if defined(UNDERWATER)
	return 0.0.xxx;
#			else
	if (Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Interior)
		return 0.0.xxx;

	float3 reflectionDirection = reflect(viewDirection, normal);
#				if defined(FLUID_FIELD)
	// The glint spreads on rough simulated water and stays tight on calm water
	float reflectionMul = FluidField::SunGlint(saturate(dot(reflectionDirection, SunDir.xyz)), VarAmounts.x);
#				else
	float reflectionMul = exp2(VarAmounts.x * log2(saturate(dot(reflectionDirection, SunDir.xyz))));
#				endif

	float llDirLightMult = (SharedData::linearLightingSettings.enableLinearLighting && !SharedData::linearLightingSettings.isDirLightLinear) ? SharedData::linearLightingSettings.dirLightMult : 1.0f;
	float3 sunColor = Color::DirectionalLight((SunColor.xyz * SunDir.w) / max(llDirLightMult, 1e-5), SharedData::linearLightingSettings.isDirLightLinear) * (1.0 - exp(-DeepColor.w)) * llDirLightMult;
#				if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		sunColor *= ExponentialHeightFog::GetSunlightFogAttenuation(worldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz);
	}
#				endif
	return reflectionMul * sunColor;
#			endif
}
#		endif

#		if defined(LIGHT_LIMIT_FIX)
#			include "LightLimitFix/LightLimitFix.hlsli"
#		endif

#		if defined(IBL)
#			include "IBL/IBL.hlsli"
#		endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float2 screenPosition = FrameBuffer::DynamicResolutionParams1.xy * (FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy);

#		if defined(SIMPLE) || defined(UNDERWATER) || defined(LOD) || defined(SPECULAR)
	float3 viewDirection = normalize(input.WPosition.xyz);

#			if defined(FLUID_FIELD_FALLS)
	// D159: only the front surface shows (D125, D126), not an inner side over another sheet (D100, D103)
	const float fallDepth = SharedData::GetScreenDepth(input.HPosition.z);
	if (FallHasFront && FallShading::BehindFront(fallDepth, SharedData::GetScreenDepth(FallFrontDepth.Load(int3(input.HPosition.xy, 0)).x)))
		discard;
	if (FallShading::HiddenInnerSide(input.FallOutward, -viewDirection))
		discard;
	float3 fallGeometric = FallShading::SafeNormalize(input.FallNormal, -viewDirection);
	if (dot(fallGeometric, viewDirection) > 0.0)
		fallGeometric = -fallGeometric;  // D120: turned to the camera (the meshes' winding is not consistent)
	FallUp = fallGeometric;
	FallPlane = float4(fallGeometric, -dot(fallGeometric, input.WPosition.xyz));

	// The water type's normal maps run on the sheet's flow coordinates (a whole number of repeats along the scroll, D130)
	const FallGround::FlowSample fallFlow = FallGround::Flow(input.FallTexCoord / max(FallGround::InverseScale, 1e-4) + FallGround::TexOffset, input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz);
	// D188: across widened by the stretch so the texture keeps its proportions on fast water (along would shear)
	const float2 fallEvenUv = float2(fallFlow.uvA.x / clamp(fallFlow.stretch, FallShading::FLOW_STRETCH_RANGE.x, FallShading::FLOW_STRETCH_RANGE.y), fallFlow.uvA.y);
	input.TexCoord1.xy += fallEvenUv * FallShading::FlowScale(input.NormalsScale.x);
	input.TexCoord1.zw += fallEvenUv * FallShading::FlowScale(input.NormalsScale.y);
	input.TexCoord2.xy += fallEvenUv * FallShading::FlowScale(input.NormalsScale.z);

	// The falls' white water and fades (FallShading). D179, D189: aeration from the bake (drops, pile-ups, stirred water, landings,
	// the bake's foam), eased so its patch edges draw no lines
	const float fallPlungeAir = saturate(fallFlow.plunge.x * FallShading::PLUNGE_AERATION_IMPACT + fallFlow.plunge.y * FallShading::PLUNGE_AERATION_CARRIED);
	// D189: how far the water has dropped: the river's look at the crest, its own as it falls.
	// D190, D191: a slide's own drop only where the mesh never turns steep (above a tangent it left a band across the curtain)
	const float fallSliding = fallFlow.plunge.w <= -0.99 ? FallShading::DroppedSliding(fallFlow.plunge.z, fallFlow.runTime) : 0.0;
	// D189: streaks down the fall
	// A whole number of the foam's tiles along per scroll cycle (MILK_STREAK_ALONG rounded to it), or the streaks jump as the phase wraps
	const float fallStreakTiles = FallShading::FlowScale(FallShading::FOAM_TILE).y;
	const float fallStreakAlong = max(round(FallShading::MILK_STREAK_ALONG * fallStreakTiles), 1.0) / fallStreakTiles;
	const float fallStreaks = FallShading::FlowFoamPattern(FallFoamTex, Normals01Sampler, fallFlow.uvA * float2(FallShading::MILK_STREAK_ACROSS, fallStreakAlong) + 0.71, SharedData::MipBias, fallFlow.stretch);
	// D191: white water in ribbons down the flow, widening until they merge near the foot
	const float fallColumns = FallShading::RibbonField(fallFlow.uvA, fallStreaks, FallGround::Time);
	// No air on the crest: white water and aeration only once the water has fallen (the bake's height fallen; unbaked meshes keep theirs)
	const float fallCrestClear = FallGround::BakeOn ? smoothstep(0.0, max(FallGround::AerationLook.x, 1.0), fallFlow.plunge.z) : 1.0;
	// the crest the river plane's own until it begins to fall
	FallFalling = FallGround::BakeOn ? smoothstep(0.0, FallShading::CREST_RIVER_FALL, fallFlow.plunge.z) : 1.0;
	// The bake's aeration turned to foam here (the bake says where; the settings how much)
	// from the height each column's water has fallen (it only grows down the flow, so the white's edges run with it), not the bake's
	// blurred aeration field, whose contours lay sideways across the fall and held still
	// a unified sheet's aeration is its own bake (the aeration layer: the drop and the churn); others from the height fallen
	// (only near-whole aeration whitens its surface; less stays under it, as milk and bubbles)
	const float fallBakeAir = FallGround::BakeOn ? (FallGround::BakeVelocity ? smoothstep(max(FallGround::AerationLook.w, FallShading::SURFACE_WHITE_FROM), 1.0, fallFlow.fields.w) :
																			   smoothstep(FallGround::AerationLook.w, 1.0, 1.0 - exp(-max(fallFlow.plunge.z - FallGround::AerationLook.x, 0.0) / FallShading::WHITE_BREAKUP))) *
	                                                   fallCrestClear :
	                                               0.0;
	// Baked falls: the white water's ribbons widen with the bake's aeration (from the threshold, scaled by Aeration Foam), not the old
	// drop phases; nearer the foot and where the water is stirred they break up in a faster churn that drifts across the flow (the noise
	// repeats along with the scroll, so it never skips)
	float2 fallRibbons;
	float fallDropped;
	float fallRibbonField = 0.0;  // debug (FALL_DEBUG_VIEW 15)
	// a unified sheet's water lying on rock or land (its depth behind): its churn foams there
	const float fallOnRock = FallGround::BakeOn && FallGround::BakeVelocity ? 1.0 - smoothstep(FallShading::ON_ROCK.x, FallShading::ON_ROCK.y, input.FallData.x) : 0.0;
	float fallStreakTone = 1.0;  // baked falls: a streak's own brightness (its column's, while it is still a streak)
	[branch] if (FallGround::BakeOn)
	{
		fallDropped = smoothstep(FallGround::AerationLook.x, FallGround::AerationLook.x + FallShading::BAKED_DROP_FULL, fallFlow.plunge.z);
		// the pockets between merged ribbons curve: their pattern warped across the flow by a slow noise (whole cells along, so it
		// repeats with the scroll), in rounder cells; where the ribbons are still thin streaks (low reach) it stays straight
		const float reachNow = min(saturate(fallBakeAir * fallBakeAir * FallGround::AerationLook.y), FallShading::RIBBON_MOST_REACH);
		const float curve = smoothstep(FallShading::POCKET_CURVE_FROM, FallShading::RIBBON_MOST_REACH, reachNow);
		const float warp = (FallShading::RibbonNoise(float2(fallFlow.uvA.x * FallShading::POCKET_WARP_CELLS.x, fallFlow.uvA.y * FallShading::POCKET_WARP_CELLS.y), FallShading::POCKET_WARP_CELLS.y) - 0.5) * 2.0;
		const float2 pocketUv = fallFlow.uvA + float2(warp * FallShading::POCKET_WARP * curve, 0.0);
		// each noise at its own fixed cells, their results blended (blending the cells by position warped the noise into rings); both
		// foam in and out in place (FallShading::Morphing), not drifting across the surface
		const float churn = lerp(FallShading::Morphing(fallFlow.uvA, FallShading::CHURN_FREQUENCY, FallGround::Time), FallShading::Morphing(pocketUv, FallShading::POCKET_FREQUENCY, FallGround::Time + 7.3), curve);
		// where the water is churned (the bake's stirring: behind rock, where its strands scatter) the ribbons hand over to the foam's
		// pattern instead of breaking into speckle
		const float stirred = smoothstep(FallShading::STIR_FROM, FallShading::STIR_FULL, fallFlow.fields.z);
		const float lower = saturate(max(smoothstep(FallShading::CHURN_FROM, FallShading::CHURN_FULL, fallFlow.plunge.z), curve));
		// pockets close before the water reaches the pool (their gaps morph shut over POCKET_CLOSE above it), never open at the front edge
		const float poolAbove = FallShading::AboveWater(input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz, input.FallFade.z, input.FallFade.y, input.FallData.y);
		const float closing = curve * (1.0 - smoothstep(FallShading::POCKET_CLOSE.x, FallShading::POCKET_CLOSE.y, poolAbove)) * (1.0 - fallFlow.streak);
		// the streaks from the drop: straight (no bend), thinner and more of them, morphing in place, each fading in and out on its own
		// slow timer (STREAK_LIFE) so they come and go rather than the set shifting at once
		const float streakLife = FallShading::RibbonNoise(float2(fallFlow.column * FallShading::STREAK_FREQUENCY.x, FallGround::Time / FallShading::STREAK_LIFE), 1e4);
		const float2 streakUv = float2(fallFlow.column, fallFlow.uvA.y);  // the streaks' own columns: straight down, parted by rock
		// on or off over a short window (STREAK_SWITCH): fully on it is bright and crisp, off it is gone
		const float streakOn = smoothstep(1.0 - FallShading::STREAK_ON_SHARE - FallShading::STREAK_SWITCH, 1.0 - FallShading::STREAK_ON_SHARE + FallShading::STREAK_SWITCH, streakLife);
		fallStreakTone = lerp(1.0, lerp(FallShading::STREAK_TONE.x, FallShading::STREAK_TONE.y, FallShading::RibbonNoise(float2(fallFlow.column * FallShading::STREAK_FREQUENCY.x, 0.37), 1e4)) * lerp(FallShading::STREAK_FADING_TONE, 1.0, streakOn), 1.0 - curve);
		const float streaks = lerp(FallShading::MorphingColumns(streakUv, FallShading::STREAK_FREQUENCY, FallGround::Time + 3.7), fallStreaks, FallShading::RIBBON_STREAK_DETAIL) *
		                      lerp(FallShading::STREAK_FADE_LOW, FallShading::STREAK_FADE_HIGH, streakOn);
		// stirred water fades into foam (white, the foam's own texture), not into a noise's blots
		// (a unified sheet's turbulence heaves the vertices and aerates under the surface instead: it whitens no surface itself)
		// (a unified sheet keeps its streaks wherever its bake keeps them alive: no churn or pockets take over there)
		const float field = lerp(lerp(lerp(streaks, churn, lower * FallShading::CHURN_SHARE * (1.0 - fallFlow.streak)), 1.0, closing), 1.0, FallGround::BakeVelocity ? 0.0 : stirred * FallShading::STIR_WHITE);
		fallRibbonField = field;
		// squared: half the curtain bakes above 0.7, so a linear reach merged the ribbons almost at once; thin through mid aeration,
		// merged where it is near whole
		// capped short of merged (RIBBON_MOST_REACH): even wholly aerated water keeps gaps of milky clear water moving between them
		// soft, ragged edges: the threshold broken by the fine streaks, over a wider edge
		// the reach varies with a noise riding the water (whole cells along: it repeats with the scroll), so the tongues' ends run down
		// and re-form rather than holding where the bake's aeration crosses the threshold
		const float reachRide = lerp(FallShading::REACH_RIDE.x, FallShading::REACH_RIDE.y, FallShading::RibbonNoise(float2(fallFlow.column * FallShading::REACH_RIDE_CELLS.x, fallFlow.uvA.y * FallShading::REACH_RIDE_CELLS.y), FallShading::REACH_RIDE_CELLS.y));
		// thin streaks from soon after the lip on their own (narrow, well apart), before the aeration has built
		// and widening into one another as the aeration builds (lower, on the lobes they melt into the aerated water)
		// each streak from its own height, widening steadily from a point (a V pointing up, not a flat cut top)
		const float streakStart = FallGround::AerationLook.x + FallShading::RibbonNoise(float2(fallFlow.column * FallShading::STREAK_FREQUENCY.x, 5.1), 1e4) * FallShading::STREAK_START_SPREAD;
		const float streakReach = lerp(FallShading::STREAK_TOP_REACH, FallShading::RIBBON_MOST_REACH,
									  FallGround::BakeVelocity ? smoothstep(FallShading::STREAK_MERGE.x, FallShading::STREAK_MERGE.y, fallFlow.fields.w) : 0.0) *
		                          saturate((fallFlow.plunge.z - streakStart) / FallShading::STREAK_RISE) * fallFlow.streak;  // and none once it has met rock
		const float reach = min(max(saturate(fallBakeAir * fallBakeAir * FallGround::AerationLook.y * reachRide), streakReach), FallShading::RIBBON_MOST_REACH);
		fallRibbons = FallShading::RibbonsEdge(field + (fallStreaks - 0.5) * FallShading::EDGE_BREAK * lerp(1.0, FallShading::STREAK_CRISP, streakOn), reach,
			FallShading::RIBBON_EDGE_SOFT * lerp(1.0, FallShading::STREAK_CRISP, streakOn));
		// frayed edges: only a ribbon's inside is solid, toward its edge it thins into fibres running with the water
		const float fibre = FallShading::FlowFoamPattern(FallFoamTex, Normals01Sampler, fallFlow.uvA * float2(FallShading::STREAK_FIBRE_ACROSS, fallStreakAlong * 2.0) + 0.29, SharedData::MipBias, fallFlow.stretch);
		// and bubbly: round holes from the foam's own pattern (kept round by the flow's stretch), many at the edge, a few inside
		const float bubbles = smoothstep(FallShading::STREAK_BUBBLES.x, FallShading::STREAK_BUBBLES.y,
			FallShading::FlowFoamPattern(FallFoamTex, Normals01Sampler, fallFlow.uvA * FallShading::STREAK_BUBBLE_SCALE + 0.53, SharedData::MipBias, fallFlow.stretch));
		const float solid = smoothstep(FallShading::STREAK_SOLID.x, FallShading::STREAK_SOLID.y, fallRibbons.x);
		fallRibbons.x *= lerp(smoothstep(FallShading::STREAK_FIBRE.x, FallShading::STREAK_FIBRE.y, fibre) * bubbles, lerp(1.0, bubbles, FallShading::STREAK_BUBBLES_INSIDE), solid);
	}
	else
	{
		fallRibbons = FallShading::Ribbons(fallColumns, FallShading::RibbonReach(fallFlow.plunge.w, fallSliding));
		fallDropped = 0.0;
	}
	const float fallWhite = fallRibbons.x * fallCrestClear;
	fallDropped = FallGround::BakeOn ? max(fallDropped, fallWhite) : max(max(FallShading::PhaseAerated(fallFlow.plunge.w), fallSliding), fallWhite);
	// D192: rapids: undropped fast or stirred water gets the dropped water's texture and air, not its colour
	const float fallChurn = max(fallDropped, FallShading::Rapids(fallFlow.fields, fallFlow.plunge.w, 1.0 - abs(FallUp.z)));
	// D191: the dropped water's milk, aeration and bubbles kept between ribbons (little) and in their haze (all)
	const float fallGap = lerp(1.0, lerp(FallShading::RIBBON_GAP, 1.0, fallRibbons.y), fallDropped);
	// D190: the run's aeration grows with the drop (the crest stays the river's)
	const float fallAerated = max(max(max(smoothstep(0.0, 1.0, fallFlow.fields.w) * 0.7 * lerp(FallShading::DROP_CREST_AERATION, 1.0, fallDropped), FallShading::Aerated(fallFlow.runTime) * 0.3 * fallChurn) * fallGap,
									  smoothstep(FallShading::STIRRED_FROM, FallShading::STIRRED_FULL, fallFlow.fields.z) * FallShading::STIRRED_AERATION),
								  max(max(fallPlungeAir, saturate(fallFlow.foam) * FallShading::BAKE_FOAM_AERATION), fallWhite * FallShading::PLUNGE_WHITE_AERATION)) *
	                          fallCrestClear;
	// D186: eased inward: faint at a patch's rim, whole well inside it
	const float fallAeratedLook = fallAerated * smoothstep(FallShading::AERATION_EASE_FROM, FallShading::AERATION_EASE_FULL, fallAerated);
	// D189: rough where fast and aerated (its reflected rays spread)
	FallRoughness = saturate(fallFlow.fields.x * 300.0 / 600.0) * FallShading::SSR_ROUGH_SPEED * fallChurn + fallAeratedLook * FallShading::SSR_ROUGH_AERATION;
	// The sheet's axes: along its flow (D186: the bake's frame) and across it
	const float3 fallWay = dot(fallFlow.frame, fallFlow.frame) > 0.01 ? fallFlow.frame : input.FallTangent;
	const float3 fallAlong = FallShading::SafeNormalize(fallWay - FallUp * dot(fallWay, FallUp), float3(0.0, 0.0, -1.0));
	const float3 fallAcross = cross(fallAlong, FallUp);
	// The view across the sheet in flow coordinates per unit of depth (the bubbles' parallax), held back at grazing angles
	float2 fallViewShift;
	{
		const float3 toEye = -viewDirection;
		const float facing = max(dot(toEye, FallUp), 0.35);
		fallViewShift = -float2(dot(toEye, fallAcross), dot(toEye, fallAlong)) / facing / FallShading::FALLS_UV_WORLD;
	}
	// Screen pixels per world unit (the bubbles' size on screen)
	const float fallPixelsPerUnit = 1.0 / max(length(fwidth(input.WPosition.xyz)) * 0.7071, 1e-4);
	// D189: few at the crest, more as it falls
	const float fallBubbleAeration = saturate(fallAerated * lerp(FallShading::DROP_CREST_BUBBLES, FallShading::DROP_BUBBLES, fallChurn) * fallGap + fallWhite * FallShading::PLUNGE_WHITE_BUBBLES);
	// (none on a unified sheet for now: its foam's floated; they come back with its under-water look)
	const float4 fallBubbleSample = FallGround::BakeVelocity ? 0.0 : FallShading::Bubbles(fallFlow.uvA, fallViewShift, fallBubbleAeration, fallPixelsPerUnit);
	const float fallBubbles = fallBubbleSample.x;
	// D191: dropped water is never a quiet crest, whatever pool it runs out of
	const float fallLip = max(FallShading::Lip(input.FallData.y, fallFlow.runTime), fallDropped);
	const float fallBehind = max(SharedData::GetScreenDepth(DepthTex.Load(int3(input.HPosition.xy, 0)).x) - fallDepth, 0.0);
	// D174: foam where the sheet touches the next sheet, rock, ground or the water below, in the river's foam pattern. The vertices'
	// distance to other sheets (D128) covers crossings wider than SheetGap's pixel reach
	const float fallSheetGap = FallShading::SheetGap(FallBehindDepth, int2(input.HPosition.xy), fallDepth, input.FallData.z);
	const float fallAboveWater = FallShading::AboveWater(input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz, input.FallFade.z, input.FallFade.y, input.FallData.y);
	// D189: rock foam piles on the upstream face and trails the wake, little on the sides
	const float fallRockShare = saturate(FallShading::ROCK_FOAM_SIDE + fallFlow.fields.y * FallShading::ROCK_FOAM_PILE + fallFlow.wake * FallShading::ROCK_FOAM_WAKE);
	float fallContact = FallShading::ContactStrength(fallSheetGap, fallBehind, fallAboveWater, fallLip, input.FallClip, fallRockShare, FallShading::CUT_FOAM_SHARE);
	// a unified sheet: subtle at its edges where they run level, pronounced at its lower lip
	[branch] if (FallGround::BakeVelocity)
	{
		const float2 parts = FallShading::ContactParts(fallSheetGap, fallBehind, fallAboveWater, fallLip, input.FallClip, fallRockShare, FallShading::CUT_FOAM_SHARE);
		const float edge = parts.x * FallShading::CONTACT_EDGE * FallShading::EdgeLevel(parts.x, input.WPosition.xyz);
		const float lip = parts.y * FallShading::CONTACT_LIP;
		fallContact = max(edge * edge, lip * lip);
	}
	// D179: aerated water foams too, in the same pattern, not on the lip
	const float fallFoamStretch = fallFlow.stretch;  // D182: no shear (FlowFoamPattern wraps the coordinate)
	// D186: solid where the contact is strong; D188: and where the water lands, carried on down the flow
	const float fallLanding = saturate(fallFlow.plunge.x * FallShading::PLUNGE_FOAM_IMPACT + fallFlow.plunge.y * FallShading::PLUNGE_FOAM_CARRIED) * fallLip;
	// D189: and the bake's own foam (strikes, squeezes, rejoins)
	const float fallBakeFoam = saturate(fallFlow.foam * FallShading::BAKE_FOAM) * fallLip;
	// D190, D191: and the white water's ribbons
	const float fallRockChurn = fallOnRock * smoothstep(FallShading::STIR_FROM, FallShading::STIR_FULL, fallFlow.fields.z) * FallShading::ROCK_CHURN_FOAM * fallLip;
	// (STREAKS_ONLY_FOAM: a unified sheet's foam from its streaks alone, while they are tuned)
	// (its baked foam stays: where its streaks die)
	const float fallOtherFoam = FallShading::STREAKS_ONLY_FOAM && FallGround::BakeVelocity ? max(fallBakeFoam, fallContact) : max(max(max(fallLanding, fallContact), fallBakeFoam), fallRockChurn);
	const float fallFoamSource = max(fallOtherFoam, fallWhite * FallShading::PLUNGE_WHITE_FOAM * fallLip);
	const float fallFoamDrive = fallFoamSource * lerp(FallShading::FoamBreakup(FallFoamTex, Normals01Sampler, fallFlow.uvA, SharedData::MipBias, 1.0), 1.0, smoothstep(0.45, 0.85, fallFoamSource));
	// D190: read where the view meets the raised lumps (parallax)
	const float2 fallFoamUv = FallShading::FoamParallax(FallFoamTex, Normals01Sampler, fallFlow.uvA, fallViewShift, SharedData::MipBias, fallFoamStretch);
	float fallFoamPattern = FallShading::FlowFoamPattern(FallFoamTex, Normals01Sampler, fallFoamUv, SharedData::MipBias, fallFoamStretch);
	const float fallRibbonPattern = fallFoamPattern;  // on the ribbons' own coordinates (straight down their columns)
	// a unified sheet: its foam carried along the baked flow on the mesh's even uv (bends round rock, spreads on the lobes, no stretch)
	[branch] if (FallGround::BakeOn && FallGround::BakeVelocity)
	{
		// all its foam one layer: every kind on the streaks' own grain, churning in place (two layouts crossfaded, as the aeration's;
		// the flow coordinates already bend round rock: a grain carried along the flow apart moved differently and spun)
		float2 jumpA, jumpB;
		const float churnWeight = FallShading::FoamChurn(FallGround::Time, jumpA, jumpB);
		const float churned = FallShading::ChurnBlend(FallShading::FlowFoamPattern(FallFoamTex, Normals01Sampler, fallFoamUv + jumpB, SharedData::MipBias, fallFoamStretch),
			FallShading::FlowFoamPattern(FallFoamTex, Normals01Sampler, fallFoamUv + jumpA, SharedData::MipBias, fallFoamStretch), churnWeight);
		fallFoamPattern = churned;
	}
	else[branch] if (FallGround::BakeOn)
	{
		// baked falls: the foam's grain churns as it runs (two layouts crossfaded, each jumping across while unseen), not a rigid
		// texture sliding down the sheet
		const float cycle = FallGround::Time / FallShading::FOAM_MORPH_SECONDS;
		const float w = abs(frac(cycle) * 2.0 - 1.0);
		const float jumpA = FallShading::BubbleHash(float2(floor(cycle), 4.3)) * 7.0, jumpB = FallShading::BubbleHash(float2(floor(cycle + 0.5), 9.1)) * 7.0;
		const float a = FallShading::FlowFoamPattern(FallFoamTex, Normals01Sampler, fallFoamUv + float2(jumpA, 0.0), SharedData::MipBias, fallFoamStretch);
		const float b = FallShading::FlowFoamPattern(FallFoamTex, Normals01Sampler, fallFoamUv + float2(jumpB, 0.0), SharedData::MipBias, fallFoamStretch);
		fallFoamPattern = lerp(b, a, 1.0 - w);
	}
	// D189: the foam's relief: a height, its normal and the height toward the sun (self-shadowing)
	const float3 fallFoamNormal = FallShading::FoamReliefNormal(FallUp, input.WPosition.xyz, fallFoamPattern);
	const float2 fallToSun = float2(dot(SharedData::DirLightDirection.xyz, fallAcross), dot(SharedData::DirLightDirection.xyz, fallAlong)) / FallShading::FALLS_UV_WORLD;
	const float fallFoamSunHeight = FallShading::FlowFoamPattern(FallFoamTex, Normals01Sampler, fallFoamUv + fallToSun * FallShading::FOAM_SHADOW_REACH, SharedData::MipBias, fallFoamStretch);
	// D183: contact foam has a crisp edge, aerated foam a long soft ramp. D186, D189: aeration as layers under the surface, parallaxed,
	// stronger as the water drops and where it lands
	const float fallAerationDrive = (max(max(fallAeratedLook * FallShading::AERATION_FOAM * 2.0 * lerp(1.0, FallShading::DROP_AERATION_FOAM, fallChurn), fallPlungeAir * FallShading::PLUNGE_AERATION_FOAM),
										 fallBakeAir * FallGround::AerationLook.y * lerp(FallShading::RIBBON_GAP_FOAM, 1.0, fallRibbons.y)) *
									 fallLip *
									 FallShading::FoamBreakup(FallFoamTex, Normals01Sampler, fallFlow.uvA, SharedData::MipBias, 1.0));
	// D189: the deeper layers dimmer
	// a unified sheet: straight from its baked aeration (the falls' drive above stays under the cover's threshold at its values)
	// gapped by brief clear patches opening and shutting in place, clumped by a slow noise, its grain churning (not sliding)
	float3 fallAerationVolume;
	[branch] if (FallGround::BakeVelocity)
	{
		// the most aerated water nearly covered: driven past whole, its gaps, clumps and breakup easing off
		const float heavy = smoothstep(FallShading::AERATION_HEAVY.x, FallShading::AERATION_HEAVY.y, fallFlow.fields.w);
		const float gap = FallShading::AerationGaps(fallFlow.uvA, FallGround::Time) * (1.0 - 0.7 * heavy);
		const float clump = lerp(lerp(1.0 - FallShading::AERATION_CLUMP, 1.0, FallShading::Morphing(fallFlow.uvA, FallShading::AERATION_CLUMP_CELLS, FallGround::Time + 5.1)), 1.0, heavy);
		const float drive = smoothstep(FallShading::UNIFIED_AERATION_DRIVE.x, FallShading::UNIFIED_AERATION_DRIVE.y, fallFlow.fields.w) * lerp(1.0, FallShading::AERATION_HEAVY_DRIVE, heavy) *
		                    fallLip * (1.0 - gap) * clump * lerp(FallShading::FoamBreakup(FallFoamTex, Normals01Sampler, fallFlow.uvA, SharedData::MipBias, 1.0), 1.0, heavy);
		float2 jumpA, jumpB;
		const float churnWeight = FallShading::FoamChurn(FallGround::Time, jumpA, jumpB);
		fallAerationVolume = lerp(
			FallShading::AerationVolume(FallFoamTex, Normals01Sampler, fallFlow.uvA + jumpB, fallViewShift, drive, SharedData::MipBias, fallFoamStretch, FallShading::UNIFIED_AERATION_LAYERS, 1.0, FallShading::AERATION_LAYER_OPACITY),
			FallShading::AerationVolume(FallFoamTex, Normals01Sampler, fallFlow.uvA + jumpA, fallViewShift, drive, SharedData::MipBias, fallFoamStretch, FallShading::UNIFIED_AERATION_LAYERS, 1.0, FallShading::AERATION_LAYER_OPACITY),
			churnWeight);
	}
	else fallAerationVolume = FallShading::AerationVolume(FallFoamTex, Normals01Sampler, fallFlow.uvA, fallViewShift, fallAerationDrive, SharedData::MipBias, fallFoamStretch);
	const float fallAerationFoam = fallAerationVolume.x;
	// a unified sheet: its streaks' foam with the aeration's soft cover (one look), clumped, a little see-through
	const float fallStreakClump = FallGround::BakeVelocity ? lerp(1.0 - FallShading::STREAK_CLUMP, 1.0, FallShading::Morphing(fallFlow.uvA, FallShading::AERATION_CLUMP_CELLS, FallGround::Time + 2.3)) : 1.0;
	const float fallSurfaceFoam = fallFoamDrive > 0.02 ? (FallGround::BakeVelocity ?
																 saturate(FallShading::AerationCover(fallFoamDrive * FallShading::STREAK_FOAM_DRIVE * fallStreakClump, fallFoamPattern) * FallShading::STREAK_OPACITY / FallShading::FOAM_OPACITY) :
																 FallShading::ContactCover(fallFoamDrive, fallFoamPattern)) :
	                                                     0.0;
	const float fallFoam = max(fallSurfaceFoam, fallAerationFoam);
	// D189: surface foam fully lit, the volume dimmer with depth
	const float fallSurfaceShare = saturate(fallSurfaceFoam / max(fallFoam, 1e-4));
	const float fallFoamShade = lerp(fallAerationVolume.y, 1.0, fallSurfaceShare);
	// D176: under the water below only the foam shows, fading with it (the plane is flat, so the height interpolates exactly)
	const float fallAlpha = FallShading::Alpha(input.FallFade.x, fallBehind, fallFoam, input.FallClip) * (fallAboveWater < 0.0 ? fallFoam : 1.0);
	if (fallAlpha <= 0.002)
		discard;
	const float3 waterUp = FallUp;
#			else
	const float3 waterUp = float3(0, 0, 1);
#			endif

	float distanceFactor = saturate(lerp(FrameBuffer::FrameParams.w, 1, (length(input.WPosition.xyz) - 8192) / (WaterParams.x - 8192)));
	float4 distanceMul = saturate(lerp(VarAmounts.z, 1, -(distanceFactor - 1))).xxxx;
	float distanceBlendFactor = distanceFactor;
#			if defined(UNIFIED_WATER)
	distanceBlendFactor = 1.0f;
#			endif

	bool isSpecular = false;

	float depth = 0;

#			if defined(DEPTH)
#				if defined(VERTEX_ALPHA_DEPTH)
#					if defined(VC)
	distanceMul = saturate(input.TexCoord3.z);
#					endif
#				else
	distanceMul = 0;

	depth = GetScreenDepthWater(screenPosition);
	float2 depthOffset =
		FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy * VPOSOffset.xy + VPOSOffset.zw;
	float depthMul = length(float3((depthOffset * 2 - 1) * depth / ProjData.xy, depth));
	float3 depthAdjustedViewDirection = -viewDirection * depthMul;
	float viewSurfaceAngle = dot(depthAdjustedViewDirection, GetWaterPlane().xyz);

	float planeMul = (1 - GetWaterPlane().w / viewSurfaceAngle);
	distanceMul = saturate(
		planeMul * float4(length(depthAdjustedViewDirection).xx, abs(viewSurfaceAngle).xx) /
		FogParam.z);

#					if defined(HORIZON_FIX)
	if (DepthTex.Load(float3(screenPosition, 0)).x >= HorizonFix::EmptyDepthThreshold)
		distanceMul = 1.0.xxxx;
#					endif
#				endif
#			endif

#			if defined(UNDERWATER)
	float4 depthControl = float4(0, 1, 1, 0);
#			elif defined(LOD)
	float4 depthControl = float4(1, 0, 0, 1);
#			elif defined(SPECULAR) && (NUM_SPECULAR_LIGHTS != 0)
	float4 depthControl = float4(0, 0, 1, 0);
#			else
	float4 depthControl = DepthControl * (distanceMul - 1) + 1;
#			endif
	float3 viewPosition = mul(FrameBuffer::CameraView, float4(input.WPosition.xyz, 1)).xyz;
	float2 screenUV = FrameBuffer::ViewToUV(viewPosition);
	const bool inWorld = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld);

#			if defined(SKYLIGHTING)
	float wetnessOcclusion = 1.0;

	float3 positionMSSkylight = input.WPosition.xyz;

	sh2 skylightingSH = Skylighting::SampleNoBias(positionMSSkylight);
	float skylighting = SphericalHarmonics::Unproject(skylightingSH, waterUp);

	float skylightingDiffuse = Skylighting::EvaluateDiffuse(skylightingSH, waterUp, Skylighting::GetFadeOutFactor(input.WPosition.xyz));

	wetnessOcclusion = inWorld ? pow(saturate(skylighting), 2) : 0;
#			endif

#			if defined(SKYLIGHTING)
	WaterNormalData waterData = GetWaterNormal(input, distanceBlendFactor, depthControl.z, viewDirection, depth, wetnessOcclusion);
#			else
	WaterNormalData waterData = GetWaterNormal(input, distanceBlendFactor, depthControl.z, viewDirection, depth, inWorld);
#			endif

	float3 normal = waterData.normal;
#			if defined(FLUID_FIELD_FALLS)
	{
		// (FALLS_NORMALS_ONLY: a unified sheet without the river's normal maps, its own falls normal map alone)
		if (FallShading::FALLS_NORMALS_ONLY && FallGround::BakeVelocity && FallHasNormal)
			normal = float3(0.0, 0.0, 1.0);
		// D159: normals in the sheet's frame (along its flow, over its normal); the piece's own normal map runs on the flow too (D127)
		[branch] if (FallHasNormal)
		{
			// D188: stretch evened out, a sharper mip and a finer layer near the camera
			float3 own = FallShading::FlowNormalEven(FallNormalTex, Normals01Sampler, fallFlow.uvA, fallFlow.stretch, 1.0, SharedData::MipBias + FallShading::FLOW_NORMAL_MIP_BIAS);
			const float detailNear = 1.0 - smoothstep(FallShading::FLOW_DETAIL_NEAR, FallShading::FLOW_DETAIL_FAR, length(input.WPosition.xyz));
			[branch] if (detailNear > 0.0)
			{
				const float3 fine = FallShading::FlowNormalEven(FallNormalTex, Normals01Sampler, fallFlow.uvA + float2(0.17, 0.31), fallFlow.stretch, FallShading::FLOW_DETAIL_SCALE,
					SharedData::MipBias + FallShading::FLOW_NORMAL_MIP_BIAS);
				own.xy += fine.xy * FallShading::FLOW_DETAIL_STRENGTH * detailNear;
			}
			own.xy *= FallShading::NORMAL_STRENGTH * lerp(FallShading::DROP_CREST_NORMAL, 1.0, fallChurn);  // D189, D192: the river's look at the crest, not on rapids
			normal = FallShading::SafeNormalize(float3(normal.xy + own.xy, normal.z * max(own.z, 0.1)), normal);
		}
		// D186: on the bake's flow frame; the mesh's tangent only where the bake has none
		const float3 fallFrame = dot(fallFlow.frame, fallFlow.frame) > 0.01 ? fallFlow.frame : input.FallTangent;
		normal = FallShading::SheetNormal(FallUp, fallFrame, normal);  // D174: not PerturbNormal's per-triangle frame
		// D179: the bake's swell bulges the sheet between vertices
		normal = FallShading::RaiseNormal(normal, input.WPosition.xyz, fallFlow.fields.y * FallShading::SWELL_HEIGHT);
		// D188: tilted by the wobble's slope, so the light follows the moving vertices
		normal = FallShading::WobbleNormal(normal, FallUp, fallFrame, input.FallWobble, input.FallWobbleSizes, fallFlow.stretch);
	}
#			endif

#			if defined(SKYLIGHTING)
	sh2 specularLobe = SphericalHarmonics::FauxSpecularLobe(normal, -viewDirection, 0.0);
	float skylightingSpecular = Skylighting::EvaluateSpecular(skylightingSH, specularLobe, Skylighting::GetFadeOutFactor(input.WPosition.xyz));
#			endif

	float fresnel = GetFresnelValue(normal, viewDirection);
#			if defined(FLUID_FIELD_FALLS)
	// D190: white water is matte
	fresnel *= (1.0 - fallWhite * FallShading::WHITE_REFLECTION_LOSS) * (1.0 - fallDropped * FallShading::BODY_REFLECTION_LOSS);  // D191: and the falling body's
#			endif

#			if defined(SPECULAR) && (NUM_SPECULAR_LIGHTS != 0)
	float3 finalColor = 0.0.xxx;

#				if !defined(LIGHT_LIMIT_FIX)
	[unroll] for (int lightIndex = 0; lightIndex < NUM_SPECULAR_LIGHTS; ++lightIndex)
	{
		float3 lightVector = LightPos[lightIndex].xyz - (PosAdjust.xyz + input.WPosition.xyz);
		float3 lightDirection = normalize(normalize(lightVector) - viewDirection);
		float lightFade = saturate(length(lightVector) / LightPos[lightIndex].w);
		float lightColorMul = (1 - lightFade * lightFade);
		float LdotN = saturate(dot(lightDirection, normal));
		float3 lightColor = (Color::PointLight(LightColor[lightIndex].xyz) * pow(LdotN, FresnelRI.z)) * lightColorMul;
		finalColor += lightColor;
	}
#				endif

	finalColor *= fresnel;
#				if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override specular color with debug visualization
	float3 debugColor = WetnessEffects::GetDebugWetnessColorSpecular(waterData.rippleInfo, 2.5, 4.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#				endif

	isSpecular = true;
#			else

#				if defined(SKYLIGHTING)
	float3 specularColor = GetWaterSpecularColor(input, normal, viewDirection, distanceBlendFactor, skylightingSpecular);
#				else
	float3 specularColor = GetWaterSpecularColor(input, normal, viewDirection, distanceBlendFactor, 1.0);
#				endif

	DiffuseOutput diffuseOutput = GetWaterDiffuseColor(input, normal, viewDirection, distanceMul, depthControl.y, fresnel, viewPosition, depth);

	float surfaceShadow;
	float dirShadow = ShadowSampling::Get3DFilteredShadow(input.WPosition.xyz, diffuseOutput.refractedViewDirection, input.HPosition.xy, surfaceShadow);
#				if defined(FLUID_FIELD_FALLS)
	// D190: soften the sun's shadow on the falls: the volumetric cascades resolve cliff shadows only near, blotching the falls
	surfaceShadow = lerp(1.0, surfaceShadow, FallShading::SUN_SHADOW);
	dirShadow = lerp(1.0, dirShadow, FallShading::SUN_SHADOW);
#				endif

	float3 dirColor;
	float3 ambientColor;
	ShadowSampling::ExtractLighting(diffuseOutput.refractionDiffuseColor, dirColor, ambientColor);

	dirColor *= dirShadow;

#				if defined(SKYLIGHTING)
	ambientColor = Color::IrradianceToLinear(ambientColor);
	ambientColor *= skylightingDiffuse;
	ambientColor = Color::IrradianceToGamma(ambientColor);
#				endif

	diffuseOutput.refractionDiffuseColor = dirColor + ambientColor;
#				if defined(FLUID_FIELD)
	// Fluid Field's foam colour: the water's own lit colour (dirColor + ambientColor) lifted toward the
	// scene light, so foam takes the water's hue without blowing out the exposure
	float3 fluidAmbientLight = ShadowSampling::GetAmbientLighting();
#					if defined(SKYLIGHTING)
	fluidAmbientLight = Color::IrradianceToGamma(Color::IrradianceToLinear(fluidAmbientLight) * skylightingDiffuse);
#					endif
	// Foam lies on the surface, so it takes the surface's shadow, not the refracted point's under the
	// water: that one moves with every ripple and makes the foam flicker in sunlight
	const float3 fluidFoamLight = FluidField::FoamColor(dirColor + ambientColor, ShadowSampling::GetDirectionalLighting() * surfaceShadow + fluidAmbientLight);
#					if defined(FLUID_FIELD_FALLS)
	// D189, D190: the surface foam lit over its relief
	float3 fallFoamLight = fluidFoamLight * fallFoamShade *
	                       lerp(1.0, FallShading::FoamRelief(FallUp, fallFoamNormal, fallFoamPattern, fallFoamSunHeight, SharedData::DirLightDirection.xyz, Color::RGBToLuminance(ShadowSampling::GetDirectionalLighting() * surfaceShadow), Color::RGBToLuminance(fluidAmbientLight)),
							   fallSurfaceShare);
	// D189, D190: the body takes at least some ambient light (by drop and steepness), else falls out of the sun went black
	diffuseOutput.refractionDiffuseColor = max(diffuseOutput.refractionDiffuseColor,
		fluidAmbientLight * FallShading::BODY_AMBIENT * max(fallDropped, saturate((1.0 - abs(FallUp.z)) * 1.5)));
	// D191: the crest lights up in its own colour
	diffuseOutput.refractionDiffuseColor *= 1.0 + FallShading::CREST_GLOW * FallShading::CrestGlow(fallFlow.plunge.w);
	// D190, D191: the river's aeration at the crest, from the bake's speed and aeration (the river simulation's grid stepped over
	// boulders), fading as the water drops
	{
		// Baked falls keep their crest clear (their aeration starts where they fall)
		const float riverAeration = FallGround::BakeOn ? 0.0 : saturate(fallFlow.fields.w * 0.6 + fallFlow.fields.x * 300.0 / 600.0) * FluidField::MAX_AERATION * (1.0 - fallDropped);
		diffuseOutput.refractionDiffuseColor = lerp(diffuseOutput.refractionDiffuseColor, fluidFoamLight, riverAeration);
		diffuseOutput.refractionColor = lerp(diffuseOutput.refractionColor, fluidFoamLight, 0.5 * riverAeration);
	}
	// D191: optional body tint (BODY_TINT)
	{
		const float3 body = FallShading::BodyTint(Color::Water(ShallowColor.xyz), FallShading::BODY_LIGHT * Color::RGBToLuminance(fluidFoamLight));
		diffuseOutput.refractionDiffuseColor = lerp(diffuseOutput.refractionDiffuseColor, body, fallDropped * FallShading::BODY_TINT);
	}
	// D159: the falls' own aeration (D127), not the river simulation's
	{
		// D181, D186, D189: milky as the river's shallows, from the falls' own aeration and speed; little at the crest
		const float milk = saturate((fallAeratedLook * FallShading::MILK_AERATION + (FallGround::BakeOn ? 0.0 : fallFlow.fields.x * 300.0 / 600.0 * FallShading::MILK_SPEED)) * lerp(FallShading::DROP_CREST_MILK, 1.0, fallDropped) +
									fallDropped * FallShading::DROP_MILK) *
		                   FallShading::MILK_MOST * lerp(1.0 - FallShading::MILK_STREAK, 1.0 + FallShading::MILK_STREAK, smoothstep(0.3, 0.7, fallStreaks)) * fallGap * FallGround::AerationLook.z;  // D189: the falls' own cap, in streaks
		// D191: and the haze round the ribbons (the ribbons themselves are foam)
		const float white = saturate(milk + fallRibbons.y * FallShading::RIBBON_HAZE_MILK);
		diffuseOutput.refractionDiffuseColor = lerp(diffuseOutput.refractionDiffuseColor, fluidFoamLight, white);
		diffuseOutput.refractionColor = lerp(diffuseOutput.refractionColor, fluidFoamLight, white * 0.5);
	}
	// D191: white water in tufts
	[branch] if (FallGround::BakeOn)
	{
		// baked falls: the white water in lumps, morphing in place, lit from the sun's side (brighter facing it, shadowed behind)
		const float lump = FallShading::Morphing(fallFlow.uvA, FallShading::LUMP_FREQUENCY, FallGround::Time + 1.9);
		const float lumpToSun = FallShading::Morphing(fallFlow.uvA + fallToSun * FallShading::LUMP_SUN_STEP, FallShading::LUMP_FREQUENCY, FallGround::Time + 1.9);
		const float shade = saturate(FallShading::LUMP_BASE + (lump - lumpToSun) * FallShading::LUMP_CONTRAST + (lump - 0.5) * FallShading::LUMP_BODY);
		fallFoamLight *= lerp(1.0, shade, saturate(max(fallWhite, fallSurfaceShare)));
		// the streaks: each its own brightness, a lit core dimming to its edges, and grain running down with the water
		const float core = smoothstep(FallShading::STREAK_CORE.x, FallShading::STREAK_CORE.y, fallRibbons.x);
		const float grain = lerp(1.0 - FallShading::STREAK_GRAIN, 1.0 + FallShading::STREAK_GRAIN * 0.5, smoothstep(0.25, 0.75, fallStreaks));
		fallFoamLight *= lerp(1.0, fallStreakTone * grain * lerp(FallShading::STREAK_EDGE_LIGHT, FallShading::STREAK_CORE_LIGHT, core), fallWhite);
		// thin foam at the ribbons' edges lets the water through: it takes the water's colour there, the crowns stay white
		const float thin = 1.0 - abs(fallRibbons.x * 2.0 - 1.0);
		fallFoamLight = lerp(fallFoamLight, diffuseOutput.refractionDiffuseColor * FallShading::EDGE_WATER_LIGHT, thin * FallShading::EDGE_WATER);
	}
	else fallFoamLight *= lerp(1.0, FallShading::Tufts(fallFlow.uvA), fallWhite);
	// D190: the volume's deeper foam takes the water's colour
	fallFoamLight = lerp(fallFoamLight, diffuseOutput.refractionDiffuseColor, fallAerationVolume.z * FallShading::AERATION_DEEP_TINT * (1.0 - fallSurfaceShare));
	// D179: aerated water is milky (the bubbles scatter the light inside it), less see-through, and its bubbles glint
	diffuseOutput.refractionDiffuseColor = lerp(diffuseOutput.refractionDiffuseColor, fluidFoamLight, saturate(fallAeratedLook * FallShading::BUBBLE_MILK + fallBubbles * 0.2));
	diffuseOutput.refractionMul = lerp(diffuseOutput.refractionMul, 1.0, saturate(fallAeratedLook * FallShading::BUBBLE_OPACITY));
#					else
	{
		// Bubbles in fast, foamy water make the water body lighter and milkier
		const float3 fluidPosition = input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz;
		FluidField::ApplyAeration(diffuseOutput.refractionDiffuseColor, fluidPosition, fluidFoamLight, 1.0);
		FluidField::ApplyAeration(diffuseOutput.refractionColor, fluidPosition, fluidFoamLight, 0.5);
	}
#					endif
#					if defined(FLUID_FIELD_FALLS)
	float fallRiverMul = diffuseOutput.refractionMul;  // (without depth or refractions: the river's look untinted)
#					endif
#					if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH) && defined(REFRACTIONS) && !defined(UNDERWATER)
	// All water takes on its own colour with depth, as it looks from underwater (distanceMul.w is the
	// depth below the surface over FogParam.z)
#						if defined(FLUID_FIELD_FALLS)
	// Baked falls: calm, level, unaerated water is the river plane's own (its tint by depth below the surface); the falls' own look
	// comes in as it speeds up, drops and takes air (fallMotion)
	// (at least RIVER_LOOK_DEPTH: over its rock the sheet's few units of water read as glass, not as the river)
	fallRiverMul = FluidField::TintedRefractionWeight(diffuseOutput.refractionMul, max(distanceMul.w * FogParam.z, FallShading::RIVER_LOOK_DEPTH * FallFalling));
	// D159, D180, D181: the tint by the sheet's own thickness along the view, at least SHEET_TINT_DEPTH
	diffuseOutput.refractionMul = FluidField::TintedRefractionWeight(diffuseOutput.refractionMul, max(min(distanceMul.x * FogParam.z, FallShading::SHEET_DEPTH / max(abs(dot(viewDirection, FallUp)), 0.25)), FallShading::SHEET_TINT_DEPTH));
#						else
	diffuseOutput.refractionMul = FluidField::TintedRefractionWeight(diffuseOutput.refractionMul, distanceMul.w * FogParam.z);
#						endif
#					endif
#					if defined(FLUID_FIELD_FALLS)
	[branch] if (FallGround::BakeOn)
	{
		// Baked falls: the river plane's look where the water is calm (slow, level, not dropped, no air), the falls' as it moves: a thin
		// sheet, mostly see-through, what lies behind tinted by the water's own hue, its colour over at most BAKED_CLEAR_OPACITY; the
		// ribbons' haze thickens it
		const float fallMotion = saturate(max(max(fallDropped, smoothstep(FallShading::MOTION_SPEED.x, FallShading::MOTION_SPEED.y, fallFlow.fields.x)), fallAeratedLook)) * FallFalling;
		const float3 shallow = Color::Water(ShallowColor.xyz);
		const float3 hue = shallow / max(max(shallow.r, max(shallow.g, shallow.b)), 1e-3);
		diffuseOutput.refractionColor *= lerp(1.0, hue, FallShading::BAKED_CLEAR_TINT * fallMotion);
		// its opacity by what lies behind it: over rock close behind it is a thin film the rock shows through (tinted); over a gap,
		// air or deep water behind, more of its own body colour
		const float clearOpacity = lerp(FallShading::CLEAR_OVER_ROCK, FallShading::CLEAR_OVER_GAP, smoothstep(FallShading::CLEAR_KNEE.x, FallShading::CLEAR_KNEE.y, saturate((input.FallData.x - FallShading::CLEAR_GAP.x) / (FallShading::CLEAR_GAP.y - FallShading::CLEAR_GAP.x))));
		const float fallsMul = lerp(min(diffuseOutput.refractionMul, clearOpacity), 1.0, fallRibbons.y * FallShading::RIBBON_HAZE_OPACITY);
		diffuseOutput.refractionMul = lerp(fallRiverMul, fallsMul, fallMotion);
	}
	else
	{
		// D189-D191: less see-through as it drops and in the ribbons' haze
		diffuseOutput.refractionMul = lerp(diffuseOutput.refractionMul, 1.0, max(fallDropped * FallShading::BODY_OPACITY, fallRibbons.y * FallShading::RIBBON_HAZE_OPACITY));
	}
#					endif
#				endif

#				if !defined(UNDERWATER)
	if (SharedData::enbSettings.EnableWater) {
		float3 dirScatter = saturate(dot(normal.xyz, SharedData::DirLightDirection.xyz) * 0.5 + 0.5) * saturate(dot(viewDirection.xyz, SharedData::DirLightDirection.xyz) * 0.5 + 0.5) * SharedData::DirLightColor.xyz;
		diffuseOutput.refractionDiffuseColor += DeepColor.xyz * dirScatter * surfaceShadow * SharedData::enbSettings.WaterSunLightingMultiplier;
	}
#				endif

	float3 diffuseColor = lerp(diffuseOutput.refractionColor, diffuseOutput.refractionDiffuseColor, diffuseOutput.refractionMul);

	depthControl = DepthControl * (distanceMul - 1) + 1;

	float3 specularLighting = 0;

#				if defined(LIGHT_LIMIT_FIX)
	uint lightCount = 0;

	uint clusterIndex = 0;
	if (LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
		lightCount = LightLimitFix::lightGrid[clusterIndex].lightCount;
		uint lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
		[loop] for (uint i = 0; i < lightCount; i++)
		{
			uint clusteredLightIndex = LightLimitFix::lightList[lightOffset + i];
			LightLimitFix::Light light = LightLimitFix::lights[clusteredLightIndex];
			// Water has no engine shadow mask, so only shadow casters with a cached local shadow can light it.
			const bool localShadow = light.lightFlags & LightLimitFix::LightFlags::LocalShadow;
			if (LightLimitFix::IsLightIgnored(light) || ((light.lightFlags & LightLimitFix::LightFlags::ShadowCaster) && !localShadow)) {
				continue;
			}

			float3 lightDirection = light.positionWS.xyz - input.WPosition.xyz;
			float lightDist = length(lightDirection);

			float intensityMultiplier = LightLimitFix::GetAttenuation(lightDist, light);

			float3 normalizedLightDirection = normalize(lightDirection);

			float lightShadow = 1.0;
			[branch] if (localShadow)
				lightShadow = LightLimitFix::GetLocalShadow(DepthSampler, light.localShadowIndex, input.WPosition.xyz, FrameBuffer::CameraPosAdjust.xyz, normalizedLightDirection, false, Random::InterleavedGradientNoise(input.HPosition.xy, SharedData::FrameCount));

			float3 H = normalize(normalizedLightDirection - viewDirection);
			float HdotN = saturate(dot(H, normal));

			const bool isPointLightLinear = light.lightFlags & LightLimitFix::LightFlags::Linear;
			float3 lightColor = Color::PointLight(light.color.xyz, isPointLightLinear) * pow(HdotN, FresnelRI.z) * light.fade;
			specularLighting += lightColor * intensityMultiplier * lightShadow;
		}
	}
	specularColor += specularLighting * 3;
#				endif

#				if defined(UNDERWATER)
	float3 finalSpecularColor = lerp(Color::Water(ShallowColor.xyz), specularColor, 0.5);
	float3 finalColor = saturate(1 - length(input.WPosition.xyz) * 0.002) * ((1 - fresnel) * (diffuseColor - finalSpecularColor)) + finalSpecularColor;
	// Add ripple and splash color effects for underwater
#					if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override water color with debug visualization (darker for underwater)
	float3 debugColor = WetnessEffects::GetDebugWetnessColorUnderwater(waterData.rippleInfo, 1.5, 2.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#					endif
#				else

	float3 sunColor = GetSunColor(normal, viewDirection, input.WPosition.xyz) * surfaceShadow;
#					if defined(FLUID_FIELD)
	{
		// Foam is matte: no sun glint and a dull reflection where it is dense
#						if defined(FLUID_FIELD_FALLS)
		const float fluidMatte = saturate(fallFoam * 1.5);  // D159: the falls' own foam
#						else
		const float fluidMatte = FluidField::FoamMatte(input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz);
#						endif
		sunColor *= 1.0 - 0.9 * fluidMatte;
		specularColor *= 1.0 - 0.5 * fluidMatte;
	}
#					endif

	// Reflection and sun weights. On refracting water vanilla fades both with refractionMul, which only
	// reaches 1 once the water is deep enough to fog, so shallow water lost its reflections.
	float surfaceMul = diffuseOutput.refractionMul;
	float sunMul = depthControl.w;
	bool shoreFadedSurface = false;

	if (SharedData::enbSettings.EnableWater) {
		sunColor *= SharedData::enbSettings.WaterSunSpecularMultiplier;
#					if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH) && defined(REFRACTIONS)
		// distanceMul.w is the water depth below the surface over FogParam.z, saturated
		surfaceMul = saturate(distanceMul.w * FogParam.z / max(min(ShoreFadeDepth, FogParam.z), 1e-4));
		sunMul = max(sunMul, surfaceMul);
		shoreFadedSurface = true;
#					endif
	}

#					if defined(VC)
	float specularFraction = lerp(1, fresnel * surfaceMul, distanceBlendFactor);
	float3 finalColorPreFog = lerp(diffuseColor, specularColor, specularFraction) + sunColor * sunMul;
#						if defined(FLUID_FIELD)
	FluidField::ApplyFoam(finalColorPreFog, input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz, fluidFoamLight);
#						endif

#						if !defined(UNIFIED_WATER)
	float fogDistanceFactor = input.FogParam.w;
	float3 fogColor = Color::Fog(input.FogParam.xyz);
#						else
	float fogDistanceFactor = min(FogFarColor.w, pow(saturate(length(input.WPosition.xyz) * FogParam.y - FogParam.x), FresnelRI.y));
	float3 fogColor = Color::Fog(lerp(FogNearColor.xyz, FogFarColor.xyz, fogDistanceFactor));
#						endif

	fogDistanceFactor = Color::FogAlpha(fogDistanceFactor);

#						if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		fogColor = ImageBasedLighting::GetFogIBLColor(fogColor);
	}
#						endif
#						if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFog(input.WPosition.xyz, FrameBuffer::CameraPosAdjust.xyz, fogColor, float4(input.HPosition.xy * FrameBuffer::DynamicResolutionParams2.xy, input.HPosition.z, 1));
		if (ExponentialHeightFog::ShouldDisableVanillaFog()) {
			fogColor = exponentialHeightFog.xyz;
			fogColor *= GetWaterFogFade();
			finalColorPreFog = lerp(finalColorPreFog, fogColor, exponentialHeightFog.w);
		} else {
			fogColor *= GetWaterFogFade();
			finalColorPreFog = lerp(finalColorPreFog, fogColor, fogDistanceFactor);
			float3 expFogColor = exponentialHeightFog.xyz * GetWaterFogFade();
			finalColorPreFog = lerp(finalColorPreFog, expFogColor, exponentialHeightFog.w);
		}
	} else {
		fogColor *= GetWaterFogFade();
		finalColorPreFog = lerp(finalColorPreFog, fogColor, fogDistanceFactor);
	}
#						else
	fogColor *= GetWaterFogFade();
	finalColorPreFog = lerp(finalColorPreFog, fogColor, fogDistanceFactor);
#						endif

	float3 finalColor = finalColorPreFog;

#						if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override water color with debug visualization
	float3 debugColor = WetnessEffects::GetDebugWetnessColorStandard(waterData.rippleInfo, 2.0, 3.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#						endif

#					else
	float specularFraction = lerp(1, fresnel, distanceBlendFactor);
	float waterOpacity = diffuseOutput.refractionMul;
	float3 finalColorPreFog;
	if (shoreFadedSurface) {
		// Reflection takes its share first; the rest splits between the water color and the see-through refraction.
		// The sun is a reflection too, so it fades in with the surface instead of rimming see-through water.
		// Divided by the opacity because the composite below lerps from the refraction by it; the floor keeps that
		// continuous and moves at most 0.1% of the weight off the refraction.
		float reflectionWeight = specularFraction * surfaceMul;
		waterOpacity = max(lerp(diffuseOutput.refractionMul, 1.0, reflectionWeight), 1e-3);
		finalColorPreFog = (diffuseOutput.refractionDiffuseColor * (diffuseOutput.refractionMul * (1.0 - reflectionWeight)) + specularColor * reflectionWeight + sunColor * (sunMul * surfaceMul)) / waterOpacity;
	} else {
		finalColorPreFog = lerp(diffuseOutput.refractionDiffuseColor, specularColor, specularFraction) + sunColor * depthControl.w;
	}
#						if defined(FLUID_FIELD)
	{
		// Foam is opaque: it covers the see-through refraction as well
#							if defined(FLUID_FIELD_FALLS)
		// D159: the falls' own foam, laid as ApplyFoamBlended lays the river's
		if (fallAboveWater < 0.0) {
			// D176: under the water below, the foam's colour alone
			finalColorPreFog = fallFoamLight;
			waterOpacity = 1.0;
		} else if (fallFoam > 0.0) {
			// a unified sheet's foam: wet (a little of the reflection by fresnel, a broad sun sheen) and lit through (backlit into the sun,
			// wrapped round its shaded side, in the water's hue, most where it is thin)
			[branch] if (FallGround::BakeVelocity)
			{
				const float3 toSun = SharedData::DirLightDirection.xyz;
				const float3 sunLight = SharedData::DirLightColor.xyz * surfaceShadow;
				const float3 toEye = -viewDirection;
				const float sheen = pow(saturate(dot(reflect(-toSun, normal), toEye)), FallShading::FOAM_SHEEN_POWER) * FallShading::FOAM_SHEEN;
				const float3 hue = diffuseOutput.refractionDiffuseColor / max(Color::RGBToLuminance(diffuseOutput.refractionDiffuseColor), 1e-3);
				const float through = pow(saturate(dot(viewDirection, toSun)), FallShading::FOAM_SSS_POWER) * FallShading::FOAM_SSS_BACK +
				                      saturate((dot(normal, toSun) + FallShading::FOAM_SSS_WRAP) / (1.0 + FallShading::FOAM_SSS_WRAP)) * (1.0 - saturate(dot(normal, toSun))) * FallShading::FOAM_SSS_WRAP_LIGHT;
				const float thin = lerp(1.0, FallShading::FOAM_SSS_THICK, saturate(fallFoam / max(FallShading::FOAM_OPACITY, 1e-3)));
				// diffuse, light scattered inside it: wrapped past the terminator, the shaded side down to the ambient's share, never black
				const float sunLum = Color::RGBToLuminance(sunLight), ambientLum = Color::RGBToLuminance(fluidAmbientLight);
				const float wrapped = saturate((dot(normal, toSun) + FallShading::FOAM_WRAP) / (1.0 + FallShading::FOAM_WRAP));
				const float diffuse = lerp(ambientLum / max(ambientLum + sunLum, 1e-4), 1.0, wrapped);
				fallFoamLight *= lerp(1.0, diffuse, FallShading::FOAM_DIFFUSE);
				fallFoamLight = lerp(fallFoamLight, specularColor, saturate(fresnel * FallShading::FOAM_REFLECT)) + sunLight * sheen +
				                sunLight * lerp(1.0, hue, FallShading::FOAM_SSS_TINT) * through * thin;
			}
			const float fallOpacity = lerp(waterOpacity, 1.0, fallFoam);
			finalColorPreFog = (finalColorPreFog * (waterOpacity * (1.0 - fallFoam)) + fallFoamLight * fallFoam) / max(fallOpacity, 1e-3);
			waterOpacity = fallOpacity;
		}
#							else
		FluidField::ApplyFoamBlended(finalColorPreFog, waterOpacity, input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz, fluidFoamLight);
#							endif
	}
#						endif

#						if !defined(UNIFIED_WATER)
	float fogDistanceFactor = input.FogParam.w;
	float3 preFogColor = Color::Fog(input.FogParam.xyz);
#						else
	float fogDistanceFactor = min(FogFarColor.w, pow(saturate(length(input.WPosition.xyz) * FogParam.y - FogParam.x), FresnelRI.y));
	float3 preFogColor = Color::Fog(lerp(FogNearColor.xyz, FogFarColor.xyz, fogDistanceFactor));
#						endif

	fogDistanceFactor = Color::FogAlpha(fogDistanceFactor);

#						if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		preFogColor = ImageBasedLighting::GetFogIBLColor(preFogColor);
	}
#						endif
#						if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFog(input.WPosition.xyz, FrameBuffer::CameraPosAdjust.xyz, preFogColor, float4(input.HPosition.xy * FrameBuffer::DynamicResolutionParams2.xy, input.HPosition.z, 1));
		if (ExponentialHeightFog::ShouldDisableVanillaFog()) {
			preFogColor = exponentialHeightFog.xyz;
			preFogColor *= GetWaterFogFade();
			finalColorPreFog = lerp(finalColorPreFog, preFogColor, exponentialHeightFog.w);
		} else {
			preFogColor *= GetWaterFogFade();
			finalColorPreFog = lerp(finalColorPreFog, preFogColor, fogDistanceFactor);
			float3 expFogColor = exponentialHeightFog.xyz * GetWaterFogFade();
			finalColorPreFog = lerp(finalColorPreFog, expFogColor, exponentialHeightFog.w);
		}
	} else {
		preFogColor *= GetWaterFogFade();
		finalColorPreFog = lerp(finalColorPreFog, preFogColor, fogDistanceFactor);
	}
#						else
	preFogColor *= GetWaterFogFade();

	finalColorPreFog = lerp(finalColorPreFog, preFogColor, fogDistanceFactor);
#						endif

	float3 refractionColor = diffuseOutput.refractionColor;

	float fogFactor = min(FogParam.w, pow(saturate(-diffuseOutput.depth * FogParam.y - FogParam.x), FogParam.z));
	float3 fogColor = Color::Fog(lerp(FogNearColor.xyz, FogFarColor.xyz, fogFactor));
#						if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled && ExponentialHeightFog::ShouldDisableVanillaFog()) {
		fogFactor = 0;
	}
#						endif
#						if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		fogColor = ImageBasedLighting::GetFogIBLColor(fogColor);
	}
#						endif
	refractionColor = lerp(refractionColor, fogColor, Color::FogAlpha(fogFactor));

	float3 finalColor = lerp(refractionColor, finalColorPreFog, waterOpacity);
#						if defined(FLUID_FIELD_FALLS)
	// D180: bubbles over the result (blended in, the water's colour hid them)
	{
		// D180: air spheres facing the camera: silvery rims, sun highlight, lensed background
		const float3 toEye = -viewDirection;
		const float3 right = FallShading::SafeNormalize(cross(float3(0.0, 0.0, 1.0), toEye), float3(1.0, 0.0, 0.0));
		const float3 up = cross(toEye, right);
		const float3 bubble = FallShading::BubbleColor(fallBubbleSample.yz, right, up, toEye, SharedData::DirLightDirection.xyz,
			SharedData::DirLightColor.xyz * surfaceShadow, fluidFoamLight, refractionColor);
		finalColor = lerp(finalColor, bubble, saturate(fallBubbles * FallShading::BUBBLE_GLINT));
	}

#						endif
#						if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override water color with debug visualization
	float3 debugColor = WetnessEffects::GetDebugWetnessColorStandard(waterData.rippleInfo, 2.0, 3.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#						endif
#					endif

#				endif
#			endif
#			if defined(FLUID_FIELD) && !defined(FLUID_FIELD_FALLS)
	if (SharedData::fluidFieldSettings.DebugView != 0)
		finalColor = FluidField::DebugColor(input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz, finalColor, SharedData::fluidFieldSettings.DebugView);
#			endif
	psout.Lighting = float4(finalColor, isSpecular);
#			if defined(FLUID_FIELD_FALLS)
	// D159: premultiplied for FallRefraction's blend (one, one minus source alpha); alpha only fades the edges
	finalColor = clamp(finalColor, 0.0, FallShading::MAX_OUTPUT);  // D3D's min and max return the other value for NaN
	psout.Lighting = FallGround::DebugMode == 4 ? float4(FallShading::DepthBehindBand(input.FallData.x) * 0.5, 1.0) : float4(finalColor * fallAlpha, fallAlpha);
	// Quadrants (debug): one foam term full screen, by FALL_DEBUG_VIEW. 1 the flow coordinates the foam lies on (frac: red
	// across, green along); 2 the foam pattern; 3 the parallax shift of the foam's coordinates (red across, green along, x8);
	// 4 the stretch (red, quarter); 5 the foam (red surface, green aeration); 6 the mesh's own uv (a checker cell per 0.125)
	[branch] if (FallGround::DebugMode == 5 && FallGround::BakeOn && FallGround::BakeVelocity && FALL_DEBUG_VIEW >= 10)
	{
		// a unified sheet's layers one at a time (FALL_DEBUG_VIEW): 10 the flow coordinates (red across stripes, green along), 11 speed,
		// 12 turbulence, 13 aeration, 14 the ribbons' mask (red cover, green haze), 15 the streaks' field, 16 the foam grain
		float3 view = 0.0;
		if (FALL_DEBUG_VIEW == 10)
			view = float3(step(0.5, frac(fallFlow.column * 16.0)), step(0.5, frac((fallFlow.uvA.y + FallGround::Scroll.x) * 4.0)), 0.25);
		else if (FALL_DEBUG_VIEW == 11)
			view = lerp(float3(0.05, 0.1, 0.5), float3(1.0, 0.3, 0.05), saturate(fallFlow.fields.x * 300.0 / 800.0));
		else if (FALL_DEBUG_VIEW == 12)
			view = saturate(fallFlow.fields.z / 8.0).xxx * float3(1.0, 0.6, 0.2);
		else if (FALL_DEBUG_VIEW == 13)
			view = saturate(fallFlow.fields.w).xxx;
		else if (FALL_DEBUG_VIEW == 14)
			view = float3(fallRibbons.x, fallRibbons.y * 0.5, 0.0);
		else if (FALL_DEBUG_VIEW == 15)
			view = saturate(fallRibbonField).xxx;
		else
			view = fallFoamPattern.xxx;
		psout.Lighting = float4(view, 1.0);
	}
	else[branch] if (FallGround::DebugMode == 5 && FallGround::BakeOn && FallGround::BakeVelocity)
	{
		// flow view: short streaks carried along the baked flow, drawn along it, sized to the screen; colour by speed
		float2 fallAdvect0 = 0.0, fallAdvect1 = 0.0;
		const float fallAdvectWeight = FallGround::AdvectedUv(input.FallTexCoord, fallFlow.velocity, input.WPosition.xyz, FallGround::Time, fallAdvect0, fallAdvect1);
		const float2 dirUv = normalize(fallAdvect0 - fallAdvect1 + 1e-6);
		float lit = 0.0;
		[unroll] for (int k = 0; k < 2; k++)
		{
			const float2 c = (k == 0 ? fallAdvect0 : fallAdvect1) * 24.0;
			const float2 cell = floor(c), f = frac(c) - 0.5;
			const float2 jitter = float2(FallShading::BubbleHash(cell), FallShading::BubbleHash(cell + 17.0)) - 0.5;
			const float2 d = f - jitter * 0.5;
			const float2 along = float2(dot(d, dirUv), dot(d, float2(-dirUv.y, dirUv.x)));
			const float streak = smoothstep(0.08, 0.03, abs(along.y)) * smoothstep(0.35, 0.2, abs(along.x));
			lit += streak * (k == 0 ? fallAdvectWeight : 1.0 - fallAdvectWeight);
		}
		const float speed = saturate(length(fallFlow.velocity) / 800.0);
		psout.Lighting = float4(lerp(float3(0.05, 0.1, 0.2), float3(0.1, 0.6, 0.3), speed) * 0.5 + lit, 1.0);
	}
	else[branch] if (FallGround::DebugMode == 5)
	{
		float3 view = 0.0;
		if (FALL_DEBUG_VIEW == 1)
			view = float3(frac(fallFlow.uvA + float2(0.0, FallGround::Scroll.x)), 0.0);
		else if (FALL_DEBUG_VIEW == 2)
			view = fallFoamPattern.xxx;
		else if (FALL_DEBUG_VIEW == 3)
			view = float3(saturate(abs(fallFoamUv - fallFlow.uvA) * 8.0), 0.0);
		else if (FALL_DEBUG_VIEW == 4)
			view = float3(saturate(fallFlow.stretch * 0.25), 0.0, 0.0);
		else if (FALL_DEBUG_VIEW == 6) {
			const float2 cell = input.FallTexCoord * 8.0;  // the mesh's own uv: a cell per 0.125 (121 world units), ramps and checker
			view = float3(frac(cell), ((uint(floor(cell.x)) + uint(floor(cell.y))) & 1u) ? 0.8 : 0.1);
		} else
			view = float3(fallSurfaceFoam, fallAerationFoam, 0.0);
		psout.Lighting = float4(view, 1.0);
	}

#			endif
#		endif

#		if defined(STENCIL)
	float3 viewDirection = normalize(input.WorldPosition.xyz);
	float3 normal =
		normalize(cross(ddx_coarse(input.WorldPosition.xyz), ddy_coarse(input.WorldPosition.xyz)));
	float VdotN = dot(viewDirection, normal);
	psout.WaterMask = float4(0, 0, VdotN, 0);

	psout.MotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition);
#		endif

#		if defined(FLUID_FIELD_FALLS) && !defined(STENCIL)
	psout.MotionVector = MotionBlur::GetSSMotionVector(float4(input.WPosition.xyz, 1.0), float4(input.FallPrevious, 1.0));
#		endif
	return psout;
}

#	endif

#endif
