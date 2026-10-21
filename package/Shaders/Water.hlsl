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

// D159: Fluid Field's see-through falls drawn by the river's own shader: the refracting surface technique only, on the
// falls' sheets (FallRefraction binds the river's captured state, the sheets' vertices and their flow). Water.hlsl's
// per-vertex and flowmap variants do not apply to a sheet (FallRefraction asks for the descriptor without them), and
// neither do Water Effects' parallax nor Wetness Effects' rain ripples (made for a level surface)
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
	float3 FallNormal: TEXCOORD9;     // D159: the sheet's geometric normal, world axes
	float2 FallTexCoord: TEXCOORD10;  // ...its texture coordinates as authored (FallGround::Flow's input)
	float3 FallFade: TEXCOORD11;      // ...FallGround::Fade (x the seam fade, y over the water below), z its foot
	float4 FallData: TEXCOORD12;      // ...x the baked water or air behind, y under the water it pours out of, z to the nearest other sheet
	float4 FallOutward: TEXCOORD13;   // ...the normal turned to its outer side, w 1 where another sheet lies behind its inner side
	float3 FallTangent: TEXCOORD14;   // D174: the direction its texture's v runs, world axes (the frame its detail normal is laid on)
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

// D159: FallRefraction's per-piece constants (FallSurface.hlsl's FallSurfaceCB): only its model to world transform here
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
 * D159: a falls sheet's vertex, moved as the lit passes and FallSurface.hlsl move it (FallGround: seams, swell and wobble
 * at the frame's latched phase, the cut), handed to the river's pixel shader as the river's own vertex shader hands it:
 * camera-relative positions, the water type's normal map scroll (the sheet's flow coordinates are added per pixel), vanilla's fog.
 */
VS_OUTPUT main(uint vertexID : SV_VertexID)
{
	VS_OUTPUT vsout = (VS_OUTPUT)0;
	vsout.NormalsScale = NormalsScale;

	const FallSurfaceVertex fallVertex = FallVertices[vertexID];
	const float3 model = fallVertex.Position + FallGround::Shift(vertexID, FallGround::Scroll.x);
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
	vsout.FallTexCoord = fallVertex.TexCoord;
	vsout.FallFade.xy = FallGround::Fade(vertexID);
	vsout.FallFade.y = min(vsout.FallFade.y, 512.0);  // D139: the "none" value held before interpolation
	vsout.FallFade.z = (!FallGround::Enabled || FallGround::VertexBase == FallGround::NO_VERTEX_CLIP) ? 1.0 : fmod(FallGround::VertexClip[FallGround::VertexBase + vertexID].a.w, 2.0);
	vsout.FallData = float4(FallGround::Behind(vertexID), min(FallGround::BelowSource(vertexID), 512.0), min(FallGround::ToOtherSheet(vertexID), 512.0), 0.0);
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
#	endif
};

#	ifdef PSHADER

SamplerState ReflectionSampler : register(s0);
SamplerState RefractionSampler : register(s1);
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

// D159: FallRefraction's per-piece constants (FallSurface.hlsl's FallSurfaceCB): whether the piece has its normal map, and
// whether the falls' front surface (D125) is bound
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
	// D174: the falls take no flowmap normal: the river simulation's weight at their world xy switched it on wherever a sheet
	// overhangs simulated river
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
	// D174: falling water is rough: the sky read blurred
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
		// D159: the engine's SSR is made for a level plane and only on the water mask's pixels: the falls march their own
		// reflected ray (D155) through the scene behind them (RefractionTex, DepthTex), weighted as the river's SSR
		const float pointingDirection = dot(viewDirection, R) * 0.5 + 0.5;
		const float pointingAlignment = dot(reflect(viewDirection, FallUp), R) * 0.5 + 0.5;
		const float ssrAmount = sqrt(saturate(min(pointingAlignment, pointingDirection)));
		float2 dimensions;
		RefractionTex.GetDimensions(dimensions.x, dimensions.y);
		const float4 hit = FallShading::ScreenReflection(RefractionTex, DepthTex, RefractionSampler, input.WPosition.xyz, FallShading::SafeNormalize(R, FallUp), dimensions, dimensions * FrameBuffer::DynamicResolutionParams1.xy);
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
	// D159: the river's offset (VarAmounts.w world units per unit of tilt) along the sheet, the tilt the normal's part
	// across the sheet's own normal; projected by the camera (MPosition is camera-relative here, not the river's model space)
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
	// D159: what of the sheet shows (FallShading, as FallSurface.hlsl): only the falls' front surface (D125, D126), not an
	// inner side over another sheet (D100, D103); its normal and plane stand in for the river's up and ReflectPlane
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

	// The flow, as the lit pass reads it: the water type's normal maps run on the sheet's flow coordinates (a whole number
	// of repeats along the scroll, D130) instead of the river's world xy
	const FallGround::FlowSample fallFlow = FallGround::Flow(input.FallTexCoord / max(FallGround::InverseScale, 1e-4) + FallGround::TexOffset, input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz);
	input.TexCoord1.xy += fallFlow.uvA * FallShading::FlowScale(input.NormalsScale.x);
	input.TexCoord1.zw += fallFlow.uvA * FallShading::FlowScale(input.NormalsScale.y);
	input.TexCoord2.xy += fallFlow.uvA * FallShading::FlowScale(input.NormalsScale.z);

	// The falls' white water and fades (FallShading), from the scene's depth just behind the sheet
	const float fallAerated = FallShading::Aerated(fallFlow.runTime);
	const float fallLip = FallShading::Lip(input.FallData.y, fallFlow.runTime);
	const float fallBehind = max(SharedData::GetScreenDepth(DepthTex.Load(int3(input.HPosition.xy, 0)).x) - fallDepth, 0.0);
	// D174: foam wherever the sheet touches something (the next sheet behind, rock and ground, the water below), per pixel,
	// in the river's foam pattern laid on the sheet and drifting on the flow
	// D174: and the vertices' own distance to the nearest other sheet (D128): up close, the line where two sheets cross runs
	// wider than SheetGap's pixels reach (no sheet in the second layer along it, a dark line, the user)
	const float fallSheetGap = min(FallShading::SheetGap(FallBehindDepth, int2(input.HPosition.xy), fallDepth), input.FallData.z);
	const float fallContact = FallShading::ContactStrength(fallSheetGap, fallBehind, input.FallFade.y, fallLip, input.FallClip);
	const float fallFoam = fallContact > 0.02 ? FallShading::FoamCover(fallContact, FallShading::FoamPattern(FallFoamTex, Normals01Sampler, input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz, FallUp, fallFlow.velocity, SharedData::Timer, SharedData::MipBias)) : 0.0;
	const float fallAlpha = FallShading::Alpha(input.FallFade.x, fallBehind, fallFoam, input.FallClip);
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
		// D159: the river's normal is in its surface's frame (x, y along its normal maps' coordinates, z up); here the
		// sheet's: along its flow coordinates, over its own normal. The piece's own normal map (vanilla's falls) runs on the
		// flow too (D127's strength), laid over the water type's
		[branch] if (FallHasNormal)
		{
			float3 own = FallNormalTex.Sample(Normals01Sampler, fallFlow.uvA).xyz * 2.0 - 1.0;
			own.xy *= FallShading::NORMAL_STRENGTH;
			normal = FallShading::SafeNormalize(float3(normal.xy + own.xy, normal.z * max(own.z, 0.1)), normal);
		}
		normal = FallShading::SheetNormal(FallUp, input.FallTangent, normal);  // D174: not PerturbNormal's per-triangle frame
	}
#			endif

#			if defined(SKYLIGHTING)
	sh2 specularLobe = SphericalHarmonics::FauxSpecularLobe(normal, -viewDirection, 0.0);
	float skylightingSpecular = Skylighting::EvaluateSpecular(skylightingSH, specularLobe, Skylighting::GetFadeOutFactor(input.WPosition.xyz));
#			endif

	float fresnel = GetFresnelValue(normal, viewDirection);

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
	// D159: the falls' aeration by their run time (D127), not the river simulation's at their world xy
	diffuseOutput.refractionDiffuseColor = lerp(diffuseOutput.refractionDiffuseColor, fluidFoamLight, fallAerated * FallShading::AERATION_LIFT);
	diffuseOutput.refractionColor = lerp(diffuseOutput.refractionColor, fluidFoamLight, fallAerated * FallShading::AERATION_LIFT * 0.5);
#					else
	{
		// Bubbles in fast, foamy water make the water body lighter and milkier
		const float3 fluidPosition = input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz;
		FluidField::ApplyAeration(diffuseOutput.refractionDiffuseColor, fluidPosition, fluidFoamLight, 1.0);
		FluidField::ApplyAeration(diffuseOutput.refractionColor, fluidPosition, fluidFoamLight, 0.5);
	}
#					endif
#					if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH) && defined(REFRACTIONS) && !defined(UNDERWATER)
	// All water takes on its own colour with depth, as it looks from underwater (distanceMul.w is the
	// depth below the surface over FogParam.z)
#						if defined(FLUID_FIELD_FALLS)
	// D159: the falls' water is the view ray's path behind the sheet (D148), not a depth under a level surface
	diffuseOutput.refractionMul = FluidField::TintedRefractionWeight(diffuseOutput.refractionMul, distanceMul.x * FogParam.z);
#						else
	diffuseOutput.refractionMul = FluidField::TintedRefractionWeight(diffuseOutput.refractionMul, distanceMul.w * FogParam.z);
#						endif
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
		if (fallFoam > 0.0) {
			const float fallOpacity = lerp(waterOpacity, 1.0, fallFoam);
			finalColorPreFog = (finalColorPreFog * (waterOpacity * (1.0 - fallFoam)) + fluidFoamLight * fallFoam) / max(fallOpacity, 1e-3);
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
	psout.Lighting = float4(finalColor, isSpecular);
#			if defined(FLUID_FIELD_FALLS)
	// D159: premultiplied for the falls' blend (FallRefraction: one, one less source alpha); the shader has already laid
	// the sheet over what lies behind, its alpha only fades the edges
	finalColor = clamp(finalColor, 0.0, FallShading::MAX_OUTPUT);  // D3D's min and max return the other value for NaN
	psout.Lighting = FallGround::DebugMode == 4 ? float4(FallShading::DepthBehindBand(input.FallData.x) * 0.5, 1.0) : float4(finalColor * fallAlpha, fallAlpha);

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

	return psout;
}

#	endif

#endif
