// Fluid Field refractive falls (D91): the falls' lit pieces drawn after the world as see-through water, the way
// the river under them looks (Water.hlsl): what lies behind refracted from a copy of the scene and hidden by the
// water's own colour with depth, fresnel reflection of the surroundings, the sun's glint, and white water from each
// piece's flow bake. The vertices move as in the lit passes (FallGround: seam shifts, swell and wobble at the
// frame's latched phase) and the texture runs on the same flow, so the falls look the same apart from their
// material. FallRefraction draws it, premultiplied, without writing depth.
// D98: every piece takes the water type it pours out of (FluidField::DrawRefractiveFalls), and composes its colour
// as Water.hlsl does: shallow to deep and seen-through to coloured by the above-water fog over the water's depth,
// the water's colours unlit (the weather's water multiplier lights them), fresnel over the reflection, the glint.
// D102: a port of Water.hlsl's lighting rather than a look-alike: the water colour goes through ShadowSampling's
// ExtractLighting and the sun's shadow (as Water.hlsl), the foam is FluidField::FoamColor of that lit colour under the
// shadowed scene light (as the river's foam), the glint and the reflection are shadowed and restrained. The sheet is a
// thin, mostly opaque layer of water: its opacity comes from its own thickness (Water Depth) and aeration, not from how
// far the rock behind it is, and what lies behind shows only faintly, barely bent (Refraction).

SamplerState WrapSampler : register(s0);
SamplerState ClampSampler : register(s1);  // Linear: also what the shadow maps are read with (LinearSampler)

#define LinearSampler ClampSampler

#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Shading.hlsli"
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
	float RefractionStrength;   // Refraction: how far the surface bends what is seen through it
	float StreakFoam;           // How much of vanilla's streak texture reads as white water
	float DepthScale;           // Water Depth: how thick (opaque, deep-coloured) the sheet reads against its thickness
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

struct VSOutput
{
	float4 Position: SV_POSITION;
	float3 WorldPosition: TEXCOORD0;  // Camera-relative
	float2 TexCoord: TEXCOORD1;
	float3 Normal: TEXCOORD2;
	float3 Fade: TEXCOORD3;  // FallGround::Fade: the seam fade, and the height above the water it falls into; z: D98 its foot (VertexClip's a.w)
	float Clip: SV_ClipDistance0;
	float Behind: TEXCOORD7;    // D104: baked open water or air behind its outer side, world units
	float4 Outward: TEXCOORD6;  // D100: the normal turned to the mesh's outer side; D103: w 1 where a sheet lies behind its inner side
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
	output.Behind = FallGround::Behind(vertexID);
	output.Outward = float4(output.Normal * FallGround::OutwardSign(vertexID), FallGround::InnerCovered(vertexID));  // D103: interpolated as a normal, no sign flipping mid-triangle
	return output;
}
#endif  // VSHADER

#if defined(PSHADER)

#	include "Common/ShadowSampling.hlsli"

// FluidField::FoamColor, the river's foam (WaterNormal.hlsli reads its normals through Water.hlsl's names)
#	define Normals01Tex NormalTex
#	define Normals01Sampler WrapSampler
#	include "FluidField/WaterNormal.hlsli"

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

/** The cubemap's mip the sheet reads: falling water is broken and foamy, so rougher than the river's mirror (mip 0). */
static const float ENVIRONMENT_MIP = 2.0;
/** The brightest the pass adds: the scene is HDR, but one runaway value is a square after bloom and the upscalers. */
static const float MAX_OUTPUT = 64.0;

/** D102: per world unit of thickness (at Water Depth 1), how much light the sheet absorbs: 8 units square on let a quarter through. */
static const float ABSORPTION = 0.08;  // D120: per world unit of the layer (D105 0.012 against the whole gap). D121: 0.08 (was 0.03, too clear beside the river): a curtain's 8 units let 53% through, 32 on rock 8%
/** D102: world units deep the water reads per unit of thickness, against its water type's fog far (shallow to deep). */
static const float DEPTH_PER_THICKNESS = 1.0;  // D105: the apparent depth is the path itself
/** D102: how much more opaque aerated water is (bubbles scatter what lies behind). */
static const float AERATION_OPACITY = 0.6;  // D105: 0.3; D120: 0.6 again, the air in falling water is its body (a curtain's 8 units: 70% opaque)
/** D102: pixels of refraction offset per unit of tilt at Refraction 1, as a fraction of the screen height. */
static const float REFRACTION_SCREEN = 0.06;  // D120: 0.06, was 0.02: at its default a pixel or two, Refraction read as doing nothing
/** D102: the reflection's share of the fresnel weight, and the glint's strength: modest, the falls are not glossy. */
static const float REFLECTION_WEIGHT = 0.5;
static const float GLINT_WEIGHT = 0.5;
/**
 * D120: how much brighter aerated water reads than the river's own lit colour, in its water type's hue: falling water
 * shows the light its bubbles scatter, and the river's (dim, pre-lit) colour barely tinted the sheet. Scaled from that
 * lit colour (the weather's water multiplier: dark at night, where vanilla's colours unlit glowed, D102); the scene's
 * light itself is on another scale (at 0.3 of it the sheets blew out)
 */
static const float SCATTER_BRIGHTNESS = 1.5;
/** D102: the foam's most cover. */
static const float FOAM_MOST = 0.85;  // D121: 0.85 (was 0.6)
/** D121: the foam's brightness over the river's lit water colour, and how much of the water's hue it keeps. */
static const float FOAM_BRIGHTNESS = 5.0;
static const float FOAM_HUE = 0.15;
/** D121: world units from a cut line (FallGround::ClipDistance) over which the water foams, and from a mesh behind it (the screen's depth). */
static const float CUT_FOAM_REACH = 24.0;
static const float CONTACT_FOAM_REACH = 20.0;
/** D121: how white a seam's overlap and a mesh's contact foam at most. */
static const float SEAM_FOAM = 0.7;
static const float CONTACT_FOAM = 0.9;

/**
 * D102: world units of water between the sheet and what lies behind it, along the view ray: the one input the
 * absorption and colour take from behind the sheet. Now the scene depth's difference; a baked per-vertex distance
 * to the rock behind can replace it here.
 */
float BehindDepth(VSOutput a_input, float a_sheetDepth, float a_behindDepth)
{
	// D104: the baked depth behind (the bake's rays off the sheet's inner side to the first rock): steady, whatever the
	// view; the screen-depth difference only where it is less (something nearer than the rock, e.g. another sheet)
	const float rayScale = length(a_input.WorldPosition) / max(a_sheetDepth, 1e-3);
	// D106: the baked depth alone (the screen depth's conversion is not valid in this pass: it read as noise)
	return max(a_input.Behind, 0.0);
}

/** D120: past this gap behind (world units), the sheet has left the rock: what lies between is air, not water. */
static const float LAYER_GAP = 48.0;

/**
 * D102: the water a ray crosses. D120: the water layer over the cosine. Where the sheet lies on rock, the depth behind
 * it (the rays, D106) is the layer itself; where it has left the rock (a curtain), the gap is air and only the sheet's
 * own thickness counts. Counted whole (D105), the gap made every curtain 80-100% water colour, a fog no slider moved
 */
float SheetPath(float a_behind, float a_cosine)
{
	const float onRock = max(min(a_behind, MaxPath), SheetThickness);
	const float layer = lerp(onRock, SheetThickness, smoothstep(LAYER_GAP * 0.5, LAYER_GAP, a_behind));
	return layer / max(a_cosine, 0.25);
}

/**
 * D102: a_color no brighter than the light falling on it (a_light, by luminance): water lit by the moon cannot outshine
 * the moonlight. A guard for colours that come unlit (the water type's under the weather's water multiplier, its
 * reflection colour): where the multiplier already darkens them, as the river's at night, it changes nothing.
 */
float3 WithinLight(float3 a_color, float3 a_light)
{
	const float colorLuma = Color::RGBToLuminance(a_color);
	const float lightLuma = Color::RGBToLuminance(a_light);
	return colorLuma > lightLuma ? a_color * (lightLuma / max(colorLuma, 1e-5)) : a_color;
}

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
	// through), it is not drawn, so crossing and nested sheets read as one body of water. D103: only where another sheet's
	// outer face lies behind that inner side: a free curtain seen from behind or the lip from upstream still shows
	if (input.Outward.w > 0.5 && dot(SafeNormalize(input.Outward.xyz, toCamera), toCamera) < -0.05)
		discard;
	// D120: turned to the camera. Flipped by the triangle's winding first (not consistent on these meshes) and then, facing
	// away, squashed to graze the view, the normal met the eye at under 0.2 over most sheets: fresnel near 1 made each a
	// half mirror of the bright sky, the milky look
	if (dot(geometric, toCamera) < 0.0)
		geometric = -geometric;
	float3 normal = geometric;
	[branch] if (HasNormal)
	{
		float3 tangentNormal = NormalTex.Sample(WrapSampler, flow.uvA).xyz * 2.0 - 1.0;
		tangentNormal.xy *= 0.6;  // Vanilla's map is drawn for an opaque, diffuse sheet; softer reads as water
		normal = PerturbNormal(geometric, input.WorldPosition, flow.uvA, SafeNormalize(tangentNormal, float3(0.0, 0.0, 1.0)));
	}
	const float facing = saturate(dot(normal, toCamera));
	const float cosine = abs(dot(geometric, toCamera));

	// White water (D34, D42, D59): aerated as it runs and falls, foamed at rock, in a rock's wake and where it meets
	// the water below; with "Foam on the Falls" off, the streaks of vanilla's own texture still whiten it
	const float aerated = 1.0 - exp(-flow.runTime / FallGround::AERATION_SECONDS);
	// D121: not by aeration (the aerated colour, D120, is the falling water's body; as foam it speckled every sheet)
	float white = FallGround::Whitening ? saturate(max(flow.foam, flow.wake * 0.6)) : 0.0;
	float poolHeight;
	const float pool = FallGround::Whitening ? PoolWeight(position.xy, poolHeight) * saturate(input.Fade.z) : 0.0;
	if (pool > 0.0) {
		const float above = position.z - poolHeight;
		white = max(white, pool * (above >= 0.0 ? 1.0 - saturate(above / FallGround::PoolFoamHeight) : 1.0 - saturate(-above / FallGround::POOL_CUT)));
	}
	if (FallGround::Whitening && input.Fade.y < FallGround::NO_WATER * 0.5)
		white = max(white, 1.0 - saturate(input.Fade.y / FallGround::PoolFoamHeight));
	// D121: where the water breaks: along its cut lines (the lead-in leaving the river above, the foot under the pool or
	// river below, a sheet cut into another's curl, rock), where it overlaps another sheet at a seam (laid on after the
	// screen depth below: where it meets rock or any mesh, as the river foams at its shore)
	if (FallGround::Whitening) {
		white = max(white, 1.0 - saturate(input.Clip / CUT_FOAM_REACH));
		white = max(white, (1.0 - saturate(input.Fade.x)) * SEAM_FOAM);
	}
	const float lace = FallGround::Lace(flow);

	// The sun's shadow on the sheet (Water.hlsl's surfaceShadow): CS's directional shadows (Volumetric Shadows' map when
	// this variant has them) and the world's (terrain, clouds). The sheet is thin, so the surface's shadow serves throughout
	float detailedShadow;
	const float shadow = ShadowSampling::GetLightingShadow(input.WorldPosition, detailedShadow) * ShadowSampling::GetWorldShadow(input.WorldPosition, FrameBuffer::CameraPosAdjust.xyz);

	// What lies behind (Water.hlsl's RefractionTex and DepthTex), faintly bent by the surface's tilt as seen from the
	// camera; a sample that lands on something in front of the falls is not used. D102: the offset no longer grows with
	// the gap behind nor shrinks much with distance, so Refraction reads; at its default it is a few pixels
	float2 dimensions;
	SceneColor.GetDimensions(dimensions.x, dimensions.y);
	const float2 region = dimensions * FrameBuffer::DynamicResolutionParams1.xy;  // The rendered part under dynamic resolution
	const float sheetDepth = SharedData::GetScreenDepth(input.Position.z);
	const float straightDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
	const float straightBehind = max(straightDepth - sheetDepth, 0.0);
	// D121: and where it meets rock or any other mesh (the scene's depth just behind it)
	if (FallGround::Whitening)
		white = max(white, (1.0 - saturate(straightBehind / CONTACT_FOAM_REACH)) * CONTACT_FOAM);
	// D120: without Foam on the Falls, none: the streaks' foam is the scene's full light against the water's dim pre-lit
	// colour, so even 15% of it laid a pale veil over every sheet (SS 971). D121: the white water broken up by the lace
	const float cover = white > 0.0 ? saturate(smoothstep(1.0 - white, 1.0 - white * 0.6, lace) + smoothstep(0.75, 1.0, white)) : 0.0;
	const float foam = FallGround::Whitening ? saturate(cover) * FOAM_MOST : 0.0;
	const float3 viewNormal = mul((float3x3)FrameBuffer::CameraView, normal - geometric);
	const float2 offset = viewNormal.xy * float2(1.0, -1.0) * RefractionStrength * REFRACTION_SCREEN * region.y * saturate(straightBehind / 8.0) / max(sheetDepth / 4096.0, 1.0);  // D120: shrinks from 4096 units, was 1024
	float2 pixel = clamp(input.Position.xy + offset, 0.5, region - 0.5);
	float behindDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(pixel, 0)));
	if (behindDepth < sheetDepth) {
		pixel = input.Position.xy;
		behindDepth = straightDepth;
	}
	const float3 behind = SceneColor.SampleLevel(ClampSampler, pixel / dimensions, 0).rgb;

	// D102: the sheet's water. Its thickness (its own, over the cosine, and a little of the gap behind) read Water Depth
	// times thicker absorbs what lies behind (more where aerated) and, with the water type's fog curve (Water.hlsl's
	// refractionMul), makes the sheet mostly the water's own colour; shallow to deep over the fog's far distance
	const float path = SheetPath(BehindDepth(input, sheetDepth, behindDepth), cosine) * DepthScale;
	const float apparentDepth = path * DEPTH_PER_THICKNESS;
	const float fogMul = 1.0 - pow(saturate((FogFar - apparentDepth) / FogRange), FogAmount);
	const float transmitted = exp(-path * ABSORPTION) * (1.0 - AERATION_OPACITY * aerated);
	// D105: the fog curve shades the water's colour (shallow to deep) but not its opacity: at the river's fog distances it
	// saturated at once and made every sheet solid water colour
	const float opacity = 1.0 - transmitted;

	// The water's colour lit as Water.hlsl lights it: the colours as the engine hands them (the weather's water multiplier
	// in them), split by ExtractLighting into the sun's share and the ambient's, the sun's share shadowed
	const float3 shallow = Color::Water(ShallowColor);
	const float3 deep = Color::Water(DeepColor);
	float3 dirColor, ambientColor;
	ShadowSampling::ExtractLighting(lerp(shallow, deep, saturate(fogMul)), dirColor, ambientColor);
	const float3 lightDirection = SafeNormalize(SharedData::DirLightDirection.xyz, float3(0.0, 0.0, 1.0));
	const float3 sun = SharedData::InInterior ? 0.0 : ShadowSampling::GetDirectionalLighting();
	const float3 sceneLight = sun * shadow + ShadowSampling::GetAmbientLighting();
	const float3 waterLit = WithinLight(dirColor * shadow + ambientColor, sceneLight);
	// D120: aerated, the water's hue (the shallow colour, its luminance taken out) under the scene's light
	const float3 hue = shallow / max(Color::RGBToLuminance(shallow), 1e-3);
	const float3 scattered = hue * Color::RGBToLuminance(waterLit) * SCATTER_BRIGHTNESS;
	float3 waterColor = lerp(waterLit, scattered, aerated);
	if (SharedData::enbSettings.EnableWater) {
		// Water.hlsl's sun scattered in the water body
		const float scatter = saturate(dot(normal, lightDirection) * 0.5 + 0.5) * saturate(dot(-toCamera, lightDirection) * 0.5 + 0.5);
		waterColor += DeepColor * scatter * SharedData::DirLightColor.xyz * shadow * SharedData::enbSettings.WaterSunLightingMultiplier;
	}
	const float3 body = lerp(behind, waterColor, opacity);

	// Reflection (Water.hlsl's GetWaterSpecularColor and GetFresnelValue): the surroundings, the surface's detail tilting
	// the reflection by the reflection magnitude (Water.hlsl leans toward the flat water's up; here, the sheet's own
	// normal); the water type's reflection colour without Dynamic Cubemaps (vanilla's water without reflections).
	// D102: half the river's weight and a rougher mip, dulled by foam: falling water is not a mirror
	const float3 reflectionNormal = SafeNormalize(lerp(geometric, normal, ReflectionMagnitude), geometric);
	const float3 reflected = reflect(-toCamera, reflectionNormal);
	float3 environment = HasEnvironment ? EnvironmentTex.SampleLevel(ClampSampler, reflected, ENVIRONMENT_MIP).rgb : WithinLight(ReflectionColor, sceneLight);
	environment = clamp(environment, 0.0, MAX_OUTPUT);  // NaN or a runaway texel becomes 0 or the cap, never a square
	const float viewAngle = 1.0 - facing;
	float fresnel = (1.0 - FresnelF0) * pow(viewAngle, 5.0) + FresnelF0;
	if (SharedData::enbSettings.EnableWater) {
		const float exponent = SharedData::enbSettings.WaterFresnelMultiplier;
		const float fresnelRI = exponent != 0.0 ? pow(abs(FresnelF0), exponent) : 1.0;
		fresnel = lerp(SharedData::enbSettings.WaterFresnelMin, SharedData::enbSettings.WaterFresnelMax, (1.0 - fresnelRI) * pow(viewAngle, 5.0) + fresnelRI) * SharedData::enbSettings.WaterReflectionAmount;
	}
	float3 color = lerp(body, environment, saturate(fresnel) * REFLECTION_WEIGHT * (1.0 - 0.5 * foam));

	// The sun's glint (Water.hlsl's GetSunColor): the directional light as ShadowSampling gives it (the moon at night),
	// the water type's sun specular power and magnitude, in the surface's shadow; matte on foam
	const float3 sunReflected = reflect(-toCamera, normal);
	float3 glint = sun * pow(saturate(dot(sunReflected, lightDirection)), SunSpecularPower) * SunSpecularStrength * shadow * GLINT_WEIGHT;
	if (SharedData::enbSettings.EnableWater)
		glint *= SharedData::enbSettings.WaterSunSpecularMultiplier;
	color += glint * (1.0 - 0.9 * foam);

	// White water: the river's foam colour (FluidField::FoamColor: the lit water colour toward the scene light, the
	// sun's share shadowed), lifted a little further toward the light; never brighter than the scene's light
	// D121: scaled from the water's own lit brightness, as the aerated colour (LEARNINGS: the scene's light is on another
	// scale, and foam lit by it veiled every sheet), a little of the water's hue kept
	const float3 foamColor = lerp(1.0, hue, FOAM_HUE) * Color::RGBToLuminance(waterLit) * FOAM_BRIGHTNESS;
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
	// D108: Waterfall Shading debug "Depth Behind": the open water or air behind the sheet. D110: in bands of colour (a grey
	// ramp read wrong through the tonemap): red under 50 units, yellow to 150, green to 300, blue to 600, magenta at the
	// reach (nothing hit)
	if (FallGround::DebugMode == 4) {
		const float behind = input.Behind;
		const float3 band = behind >= 599.0 ? float3(1.0, 0.0, 1.0) : behind >= 300.0 ? float3(0.0, 0.2, 1.0) :
		                                                          behind >= 150.0     ? float3(0.0, 1.0, 0.0) :
		                                                          behind >= 50.0      ? float3(1.0, 1.0, 0.0) :
		                                                                                float3(1.0, 0.0, 0.0);
		return float4(band * 0.5, 1.0);
	}
	return float4(color * alpha, alpha);  // Premultiplied
}
#endif  // PSHADER
