// Fluid Field refractive falls (D91): the falls' lit pieces drawn after the world as see-through water, the way
// the river under them looks (Water.hlsl): what lies behind refracted from a copy of the scene and hidden by the
// water's own colour with depth, fresnel reflection of the surroundings, the sun's glint, and white water from each
// piece's flow bake. The vertices move as in the lit passes (FallGround: seam shifts, swell and wobble at the
// frame's latched phase) and the texture runs on the same flow, so the falls look the same apart from their
// material. FallRefraction draws it, premultiplied, without writing depth.
// D98: every piece takes the water type it pours out of (FluidField::DrawRefractiveFalls), and composes its colour
// as Water.hlsl does: shallow to deep and seen-through to coloured by the above-water fog over the water's depth,
// the water's colours unlit (the weather's water multiplier lights them), fresnel over the reflection, the glint.

#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "FluidField/FallGround.hlsli"

cbuffer FallSurfaceCB : register(b7)
{
	row_major float3x4 World;  // Model to world (absolute), rotation times scale and translation
	float3 ShallowColor;       // The water type's colours, 0 to 1, times the weather's water multiplier (Water.hlsl's ShallowColor, DeepColor)
	float FogFar;              // Its above-water fog distance far (FogParam.z): water this deep is wholly its own colour
	float3 DeepColor;
	float FogRange;             // ...far less near (FogParam.w)
	float3 ReflectionColor;     // Its reflection colour times its reflection amount: the reflection without a cubemap
	float FogAmount;            // Its above-water fog amount (FogNearColor.w)
	float FresnelF0;            // Its fresnel amount (FresnelRI.x): reflectance looking straight at the water
	float SunSpecularPower;     // VarAmounts.x
	float SunSpecularStrength;  // 1 - exp(-sun specular magnitude), as Water.hlsl weighs the sun with DeepColor.w
	float ReflectionMagnitude;  // WaterParams.y: how much the surface's detail tilts the reflection
	float RefractionStrength;   // How strongly the surface bends what is seen through it
	float StreakFoam;           // How much of vanilla's streak texture reads as white water
	float DepthScale;           // How much deeper than its path through it the falling water reads (Water Depth)
	float MaxPath;              // World units of water behind the surface counted at most
	float SheetThickness;       // World units the sheet itself is thick, square on
	uint HasEnvironment;
	uint HasDiffuse;
	uint HasNormal;
};

struct FallSurfaceVertex
{
	float3 Position;  // Model space
	float2 TexCoord;  // Vanilla's
	float3 Normal;    // Model space
};

StructuredBuffer<FallSurfaceVertex> Vertices : register(t125);
Texture2D<float4> SceneColor : register(t100);  // The scene as drawn before the falls
Texture2D<float> SceneDepth : register(t101);
Texture2D<float4> DiffuseTex : register(t102);  // The piece's own material (vanilla's falls texture)
Texture2D<float4> NormalTex : register(t103);
TextureCube<float4> EnvironmentTex : register(t104);

SamplerState WrapSampler : register(s0);
SamplerState ClampSampler : register(s1);

struct VSOutput
{
	float4 Position: SV_POSITION;
	float3 WorldPosition: TEXCOORD0;  // Camera-relative
	float2 TexCoord: TEXCOORD1;
	float3 Normal: TEXCOORD2;
	float3 Fade: TEXCOORD3;  // FallGround::Fade: the seam fade, and the height above the water it falls into; z: D98 its foot (VertexClip's a.w)
	float Clip: SV_ClipDistance0;
	float Outward: TEXCOORD6;  // D100: +1 where the winding faces the mesh's outer side, -1 where in
};

#if defined(VSHADER)
VSOutput VSMain(uint vertexID : SV_VertexID)
{
	const FallSurfaceVertex v = Vertices[vertexID];
	// The same move as the lit passes (Lighting.hlsl, Utility.hlsl): seams, swell and wobble at the latched phase
	const float3 model = v.Position + FallGround::Shift(vertexID, FallGround::Scroll.x);
	const float3 world = mul(World, float4(model, 1.0));
	const float3 relative = world - FrameBuffer::CameraPosAdjust.xyz;

	VSOutput output;
	output.Position = mul(FrameBuffer::CameraViewProj, float4(relative, 1.0));
	output.WorldPosition = relative;
	output.TexCoord = v.TexCoord;
	output.Normal = mul((float3x3)World, v.Normal);
	output.Fade.xy = FallGround::Fade(vertexID);
	// D98: as the cut (FallGround::ClipDistance, D71), the pools foam a mesh only at its foot
	output.Fade.z = (!FallGround::Enabled || FallGround::VertexBase == FallGround::NO_VERTEX_CLIP) ? 1.0 : fmod(FallGround::VertexClip[FallGround::VertexBase + vertexID].a.w, 2.0);  // D100: +2 marks an inward winding
	output.Clip = FallGround::ClipDistance(relative, vertexID);
	output.Outward = FallGround::OutwardSign(vertexID);
	return output;
}
#endif  // VSHADER

#if defined(PSHADER)

/** a_vector normalised, or a_fallback where it has no length (a zero vector normalises to NaN, which bloom spreads into squares). */
float3 SafeNormalize(float3 a_vector, float3 a_fallback)
{
	const float lengthSquared = dot(a_vector, a_vector);
	return lengthSquared > 1e-12 ? a_vector * rsqrt(lengthSquared) : a_fallback;
}

/** The normal map's tangent-space normal around a_normal, from the screen derivatives (no tangents needed). */
float3 PerturbNormal(float3 a_normal, float3 a_position, float2 a_uv, float3 a_tangentNormal)
{
	const float3 dp1 = ddx(a_position);
	const float3 dp2 = ddy(a_position);
	const float2 duv1 = ddx(a_uv);
	const float2 duv2 = ddy(a_uv);
	const float3 dp2perp = cross(dp2, a_normal);
	const float3 dp1perp = cross(a_normal, dp1);
	const float3 t = dp2perp * duv1.x + dp1perp * duv2.x;
	const float3 b = dp2perp * duv1.y + dp1perp * duv2.y;
	const float scale = rsqrt(max(max(dot(t, t), dot(b, b)), 1e-20));
	return SafeNormalize(t * scale * a_tangentNormal.x + b * scale * a_tangentNormal.y + a_normal * a_tangentNormal.z, a_normal);
}

/**
 * D98: FallGround::Pool's height, with how much of its bilinear footprint is pool (0 to 1). Pool answers yes or no at
 * 0.2 of it, so the foam it lays switched on and off along the pool map's 16-unit texels: square white patches on the
 * see-through sheet, where the foam also makes it opaque (SS 889). The weight fades it in instead.
 */
float PoolWeight(float2 a_position, out float a_height)
{
	a_height = 0.0;
	const float2 texel = (a_position - FallGround::Origin) * FallGround::InverseCell;
	const int2 base = int2(floor(texel));
	if (any(base < 0) || any(base + 1 >= int(FallGround::Size)))
		return 0.0;
	const float2 t = texel - float2(base);
	const float4 corner = float4((1.0 - t.x) * (1.0 - t.y), t.x * (1.0 - t.y), (1.0 - t.x) * t.y, t.x * t.y);
	const float4 h = float4(FallGround::Heights.Load(int3(base, 0)).y, FallGround::Heights.Load(int3(base + int2(1, 0), 0)).y,
		FallGround::Heights.Load(int3(base + int2(0, 1), 0)).y, FallGround::Heights.Load(int3(base + int2(1, 1), 0)).y);
	const float4 k = corner * (h > FallGround::NONE);
	const float total = dot(k, 1.0);
	if (total < 1e-3)
		return 0.0;
	a_height = dot(k, h) / total;
	return smoothstep(0.2, 0.7, total);
}

/** The cubemap's mip the sheet reads: falling water is broken, so a little rougher than the river's mirror (mip 0). */
static const float ENVIRONMENT_MIP = 1.0;
/** The brightest the pass adds: the scene is HDR, but one runaway value is a square after bloom and the upscalers. */
static const float MAX_OUTPUT = 64.0;

float4 PSMain(VSOutput input, bool frontFace : SV_IsFrontFace) : SV_Target
{
	const float3 position = input.WorldPosition + FrameBuffer::CameraPosAdjust.xyz;
	const float3 toCamera = SafeNormalize(-input.WorldPosition, float3(0.0, 0.0, 1.0));

	// The flow, as in the lit pass: the texture coordinates the water reads, its run time, wake and foam
	// D96: Flow takes the coordinates as the lit pass has them, after vanilla's scroll and zoom (it takes them back out):
	// handed the mesh's raw ones, the texture slid with vanilla's changing scroll against the flow
	const FallGround::FlowSample flow = FallGround::Flow(input.TexCoord / max(FallGround::InverseScale, 1e-4) + FallGround::TexOffset, position);
	const float4 diffuse = HasDiffuse ? DiffuseTex.Sample(WrapSampler, flow.uvA) : float4(0.5, 0.5, 0.5, 1.0);

	// The surface: the mesh's normal toward the viewer, with vanilla's normal map running on the flow
	float3 geometric = SafeNormalize(input.Normal, toCamera);
	// D100: only a sheet's outer side shows: seen from inside its curl (behind it, or another sheet's inner part showing
	// through), it is not drawn, so crossing and nested sheets read as one body of water
	if (dot(geometric * (input.Outward >= 0.0 ? 1.0 : -1.0), toCamera) < -0.05)
		discard;
	if (!frontFace)
		geometric = -geometric;
	if (dot(geometric, toCamera) < 0.0)
		geometric = SafeNormalize(geometric - toCamera * (dot(geometric, toCamera) * 1.01), toCamera);
	float3 normal = geometric;
	[branch] if (HasNormal)
	{
		float3 tangentNormal = NormalTex.Sample(WrapSampler, flow.uvA).xyz * 2.0 - 1.0;
		tangentNormal.xy *= 0.6;  // Vanilla's map is drawn for an opaque, diffuse sheet; softer reads as water
		normal = PerturbNormal(geometric, input.WorldPosition, flow.uvA, SafeNormalize(tangentNormal, float3(0.0, 0.0, 1.0)));
	}
	const float facing = saturate(dot(normal, toCamera));

	// White water (D34, D42, D59): aerated as it runs and falls, foamed at rock, in a rock's wake and where it meets
	// the water below; with "Foam on the Falls" off, the streaks of vanilla's own texture still whiten it
	const float aerated = 1.0 - exp(-flow.runTime / FallGround::AERATION_SECONDS);
	float white = FallGround::Whitening ? saturate(max(flow.foam, max(aerated * FallGround::AERATION_WHITE, flow.wake * 0.6))) : 0.0;
	float poolHeight;
	const float pool = FallGround::Whitening ? PoolWeight(position.xy, poolHeight) * saturate(input.Fade.z) : 0.0;
	if (pool > 0.0) {
		const float above = position.z - poolHeight;
		white = max(white, pool * (above >= 0.0 ? 1.0 - saturate(above / FallGround::PoolFoamHeight) : 1.0 - saturate(-above / FallGround::POOL_CUT)));
	}
	if (FallGround::Whitening && input.Fade.y < FallGround::NO_WATER * 0.5)
		white = max(white, 1.0 - saturate(input.Fade.y / FallGround::PoolFoamHeight));
	const float lace = FallGround::Lace(flow);
	const float cover = white > 0.0 ? saturate(smoothstep(1.0 - white, 1.0 - white * 0.6, lace) + smoothstep(0.75, 1.0, white)) : 0.0;
	const float streaks = saturate((Color::RGBToLuminance(diffuse.rgb) - 0.35) * 1.8) * StreakFoam * (0.4 + 0.6 * aerated);
	const float foam = saturate(max(cover * 0.9, streaks));

	// What lies behind, refracted (Water.hlsl's RefractionTex and DepthTex): offset by the surface's normal as seen
	// from the camera, more with more water behind; a sample that lands on something in front of the falls is not used
	float2 dimensions;
	SceneColor.GetDimensions(dimensions.x, dimensions.y);
	const float2 region = dimensions * FrameBuffer::DynamicResolutionParams1.xy;  // The rendered part under dynamic resolution
	const float sheetDepth = SharedData::GetScreenDepth(input.Position.z);
	const float straightDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
	const float straightBehind = max(straightDepth - sheetDepth, 0.0);
	const float3 viewNormal = mul((float3x3)FrameBuffer::CameraView, normal - geometric);
	const float2 offset = viewNormal.xy * float2(1.0, -1.0) * RefractionStrength * 0.06 * region.y * saturate(straightBehind / 48.0) / max(sheetDepth / 256.0, 1.0);
	float2 pixel = clamp(input.Position.xy + offset, 0.5, region - 0.5);
	float behindDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(pixel, 0)));
	if (behindDepth < sheetDepth) {
		pixel = input.Position.xy;
		behindDepth = straightDepth;
	}
	const float3 behind = SceneColor.SampleLevel(ClampSampler, pixel / dimensions, 0).rgb;

	// D98: the water's colour over what lies behind, as Water.hlsl's GetWaterDiffuseColor: the water along the view ray
	// (between the surface and what lies behind, a curtain far in front of its rock counting at most MaxPath, and the
	// sheet's own thickness), read DepthScale times deeper, goes shallow to deep over the fog's far distance and hides
	// what lies behind by the fog's curve (refractionMul). The colours are the water type's, unlit as Water.hlsl takes
	// them (the weather's water multiplier lights them; ExtractLighting only splits them for the shadow)
	const float rayScale = length(input.WorldPosition) / max(sheetDepth, 1e-3);
	const float path = min(max(behindDepth - sheetDepth, 0.0) * rayScale, MaxPath) + SheetThickness / max(abs(dot(geometric, toCamera)), 0.2);
	const float waterPath = path * DepthScale;
	const float3 shallow = Color::Water(ShallowColor);
	const float3 deep = Color::Water(DeepColor);
	const float3 waterColor = lerp(shallow, deep, saturate(waterPath / FogFar));
	const float refractionMul = 1.0 - pow(saturate((FogFar - waterPath) / FogRange), FogAmount);
	float3 body = lerp(behind, waterColor, refractionMul);
	const float3 lightDirection = SafeNormalize(SharedData::DirLightDirection.xyz, float3(0.0, 0.0, 1.0));
	const float llDirLightMult = (SharedData::linearLightingSettings.enableLinearLighting && !SharedData::linearLightingSettings.isDirLightLinear) ? SharedData::linearLightingSettings.dirLightMult : 1.0;
	const float3 sun = SharedData::InInterior ? 0.0 : Color::DirectionalLight(SharedData::DirLightColor.xyz / max(llDirLightMult, 1e-5), SharedData::linearLightingSettings.isDirLightLinear) * llDirLightMult;
	if (SharedData::enbSettings.EnableWater) {
		// Water.hlsl's sun scattered in the water body
		const float scatter = saturate(dot(normal, lightDirection) * 0.5 + 0.5) * saturate(dot(-toCamera, lightDirection) * 0.5 + 0.5);
		body += DeepColor * scatter * SharedData::DirLightColor.xyz * SharedData::enbSettings.WaterSunLightingMultiplier;
	}

	// Reflection (Water.hlsl's GetWaterSpecularColor and GetFresnelValue): the surroundings, the surface's detail tilting
	// the reflection by the reflection magnitude (Water.hlsl leans toward the flat water's up; here, the sheet's own
	// normal); the water type's reflection colour without Dynamic Cubemaps (vanilla's water without reflections)
	const float3 reflectionNormal = SafeNormalize(lerp(geometric, normal, ReflectionMagnitude), geometric);
	const float3 reflected = reflect(-toCamera, reflectionNormal);
	float3 environment = HasEnvironment ? EnvironmentTex.SampleLevel(ClampSampler, reflected, ENVIRONMENT_MIP).rgb : ReflectionColor;
	environment = clamp(environment, 0.0, MAX_OUTPUT);  // NaN or a runaway texel becomes 0 or the cap, never a square
	const float viewAngle = 1.0 - facing;
	float fresnel = (1.0 - FresnelF0) * pow(viewAngle, 5.0) + FresnelF0;
	if (SharedData::enbSettings.EnableWater) {
		const float exponent = SharedData::enbSettings.WaterFresnelMultiplier;
		const float fresnelRI = exponent != 0.0 ? pow(abs(FresnelF0), exponent) : 1.0;
		fresnel = lerp(SharedData::enbSettings.WaterFresnelMin, SharedData::enbSettings.WaterFresnelMax, (1.0 - fresnelRI) * pow(viewAngle, 5.0) + fresnelRI) * SharedData::enbSettings.WaterReflectionAmount;
	}
	float3 color = lerp(body, environment, saturate(fresnel));

	// The sun's glint (Water.hlsl's GetSunColor): the water type's sun specular power and magnitude; matte on foam
	const float3 sunReflected = reflect(-toCamera, normal);
	float3 glint = sun * pow(saturate(dot(sunReflected, lightDirection)), SunSpecularPower) * SunSpecularStrength;
	if (SharedData::enbSettings.EnableWater)
		glint *= SharedData::enbSettings.WaterSunSpecularMultiplier;
	color += glint * (1.0 - 0.9 * foam);

	// White water over it, lit by the light's brightness (as the spray), in the water's own hue
	const float3 ambient = max(Color::Ambient(max(SharedData::GetAmbient(normal), 0.0)), 0.0);
	const float3 hue = shallow / max(max(shallow.r, max(shallow.g, shallow.b)), 1e-3);
	const float3 foamLight = ambient + sun * saturate(dot(normal, lightDirection) * 0.6 + 0.4);
	const float3 foamColor = lerp(float3(0.9, 0.9, 0.9), hue * 0.9, 0.2) * foamLight;
	color = lerp(color, foamColor, foam);
	color = clamp(color, 0.0, MAX_OUTPUT);  // D3D's min and max return the other value for NaN

	// Faded at seams over a mesh leading it (D67, blended here instead of dithered) and where it meets what lies
	// behind (no hard line where it enters the rock). D99: foam keeps the contact soft too, only shorter: added to it,
	// foam made the line where the sheet enters a boulder wholly opaque and white, tracing the faceted intersection of
	// the sheet's triangles with the rock's as a white sawtooth; and the water fades out toward its cut
	const float contact = saturate(straightBehind / 4.0);
	const float alpha = saturate(input.Fade.x) * lerp(contact, sqrt(contact), foam) * FallGround::CutFade(input.Clip);
	if (alpha <= 0.002)
		discard;
	return float4(color * alpha, alpha);  // Premultiplied
}
#endif  // PSHADER
