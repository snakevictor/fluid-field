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
#include "FluidField/FallShading.hlsli"

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
	uint HasFront;  // D125: FrontDepth holds the falls' front surface
	uint3 FrontPadding;
	float3 NoiseScale;  // D129: the water type's normal maps (Water.hlsl's Normals01-03): world units per repeat
	float NoiseTime;    // Seconds
	float3 NoiseAmplitude;
	uint HasNoise;
	float3 SkyFogNearColor;  // D151: the sky's distance fog (as the water shader's SkyFogNearColor, SkyFogFarColor and FogParam)
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
};

StructuredBuffer<FallSurfaceVertex> Vertices : register(t125);
Texture2D<float4> SceneColor : register(t100);  // The scene as drawn before the falls
Texture2D<float> SceneDepth : register(t101);
Texture2D<float4> DiffuseTex : register(t121);  // The piece's own material (vanilla's falls texture)
Texture2D<float4> NormalTex : register(t122);   // D159: t121-t122, off Light Limit Fix's local shadows (t102-t103)
TextureCube<float4> EnvironmentTex : register(t104);
Texture2D<float> FrontDepth : register(t105);
Texture2D<float4> Noise1Tex : register(t106);  // D129: the water type's normal maps
Texture2D<float4> Noise2Tex : register(t107);
Texture2D<float4> Noise3Tex : register(t108);      // D125: the scene's depth with the falls' front surface drawn in
Texture2D<float> NextSheetDepth : register(t112);  // D174: the falls' surface nearest behind their front (cleared to far where none)

struct VSOutput
{
	float4 Position: SV_POSITION;
	float3 WorldPosition: TEXCOORD0;  // Camera-relative
	float2 TexCoord: TEXCOORD1;
	float3 Normal: TEXCOORD2;
	float3 Fade: TEXCOORD3;  // FallGround::Fade: the seam fade, and the height above the water it falls into; z: D98 its foot (VertexClip's a.w)
	float Clip: SV_ClipDistance0;
	float Behind: TEXCOORD7;        // D104: baked open water or air behind its outer side, world units
	float BelowSource: TEXCOORD8;   // D124: how far under the water its fall pours out of (FallGround::NO_WATER: none)
	float ToOtherSheet: TEXCOORD9;  // D128: how far to the nearest other shown sheet (FallGround::NO_WATER: none near)
	float4 Outward: TEXCOORD6;      // D100: the normal turned to the mesh's outer side; D103: w 1 where a sheet lies behind its inner side
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
	output.Fade.y = min(output.Fade.y, 512.0);  // D139: as BelowSource (D138): 1e4 interpolated into a hard line
	// D98: as the cut (FallGround::ClipDistance, D71), the pools foam a mesh only at its foot
	output.Fade.z = (!FallGround::Enabled || FallGround::VertexBase == FallGround::NO_VERTEX_CLIP) ? 1.0 : fmod(FallGround::VertexClip[FallGround::VertexBase + vertexID].a.w, 2.0);  // D100: +2 marks an inward winding
	output.Clip = FallGround::ClipDistance(relative, vertexID);
	output.Behind = FallGround::Behind(vertexID);
	// D138: the "none" values (FallGround::NO_WATER, 1e4) held to 512 before interpolation: across a triangle from a vertex
	// with one to a vertex without, 1e4 drew the band's end as a hard straight line inside the triangle (SS 1017)
	output.BelowSource = min(FallGround::BelowSource(vertexID), 512.0);
	output.ToOtherSheet = min(FallGround::ToOtherSheet(vertexID), 512.0);
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

/** D155: screen-space reflection march: steps, the first step and its growth (world units), and how thick a surface is. */
static const int SSR_STEPS = 24;
static const float SSR_FIRST_STEP = 8.0;
static const float SSR_GROWTH = 1.18;
static const float SSR_THICKNESS = 48.0;

/**
 * D155: the scene's colour where a reflected ray from a_position (camera-relative) along a_direction first passes behind
 * the scene's depth, and how sure that is (a: 0 where it leaves the screen, goes too far or meets nothing).
 */
float4 ScreenReflection(float3 a_position, float3 a_direction, float2 a_dimensions, float2 a_region)
{
	float3 p = a_position;
	float step = SSR_FIRST_STEP;
	[loop] for (int i = 0; i < SSR_STEPS; i++)
	{
		p += a_direction * step;
		step *= SSR_GROWTH;
		const float4 clip = mul(FrameBuffer::CameraViewProj, float4(p, 1.0));
		if (clip.w <= 1.0)
			break;
		const float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
		if (any(uv <= 0.0) || any(uv >= 1.0))
			break;
		const float2 pixel = uv * a_region;
		const float sceneDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(pixel, 0)));
		const float behind = clip.w - sceneDepth;
		if (behind > 0.0 && behind < SSR_THICKNESS + step) {
			const float2 edge = min(uv, 1.0 - uv);
			const float fade = saturate(min(edge.x, edge.y) * 10.0) * (1.0 - float(i) / float(SSR_STEPS));
			return float4(SceneColor.SampleLevel(ClampSampler, pixel / a_dimensions, 0).rgb, fade);
		}
	}
	return 0.0;
}

float Square(float a_value)
{
	return a_value * a_value;
}

/** D151: the sky's distance fog at a_distance world units (Water.hlsl: min(clamp, ((d - near) / (far - near))^power)), 0 to 1. */
float SkyFogAmount(float a_distance)
{
	// D152: none where Exponential Height Fog replaces the vanilla fog, as the river's water shader (Water.hlsl): there the
	// sky's raw fog tinted the falls slate blue beside the unfogged river (SS 1058)
	if (SkyFogFar <= SkyFogNear || (SharedData::exponentialHeightFogSettings.enabled && SharedData::exponentialHeightFogSettings.disableVanillaFog != 0))
		return 0.0;
	return min(SkyFogClamp, pow(saturate((a_distance - SkyFogNear) / (SkyFogFar - SkyFogNear)), SkyFogPower));
}

/** D151: a_color under the sky's fog at a_distance, as Water.hlsl fogs its colours. */
float3 Fogged(float3 a_color, float a_distance)
{
	const float amount = SkyFogAmount(a_distance);
	return lerp(a_color, Color::Fog(lerp(SkyFogNearColor, SkyFogFarColor, amount)), Color::FogAlpha(amount));
}

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

/** D127: the cubemap's mip the sheet reads where wholly aerated; mip 0 (the river's mirror) where it runs clear. */
static const float ENVIRONMENT_MIP = 1.0;

/** D102: how opaque aerated water is at least (bubbles scatter what lies behind). D127: a floor under the river's own opacity (the fog curve), which leaves a thin sheet nearly clear. */
static const float AERATION_OPACITY = 0.3;
/** D127: how much of the falls' normal map tilts the surface (it is drawn for an opaque, diffuse sheet: high contrast). */
static const float NORMAL_STRENGTH = 0.8;
/** D129: the water type's ripples on the sheet over the river's (Water.hlsl's amplitude-weighted blend). */
static const float RIPPLE_STRENGTH = 1.0;
/** D147: the fine detail ripples: world units per repeat, and how much they tilt the surface. */
static const float DETAIL_TILE = 160.0;
static const float DETAIL_STRENGTH = 0.8;
/** D147: the share of the river's reflection (fresnel) the falls take. */
static const float FALLS_REFLECTION = 1.0;  // D154: the river's (measured: the falls' depth and water parameters match the river's)

/** D131: world units per repeat of the foam's bubble pattern, and the drift's period and fastest speed (the river's foam, FluidField::GetFoamCover: 160, 0.6, 120). */
static const float FOAM_TILE = 160.0;
static const float DRIFT_PERIOD = 0.6;
static const float MAX_DRIFT = 120.0;  // D132: the river's (200 jumped visibly between phases)

/**
 * D131: a_texture's xy (-1 to 1) laid on the sheet in world space, a_scale units per repeat: tri-planar (weighted by
 * a_normal), drifted along a_drift (world units per second) in two phases crossfading, as the river's foam and flow
 * detail. Laid on the falls' own texture coordinates, the maps stretched along the flow (the glint streaked) and the
 * falls' normal map, as the foam's pattern, had horizontal bands that the foam thresholded into zebra stripes (SS 997,
 * 999). Returns the tilt in world space (xyz) and the pattern, the xy's length (w).
 */
float4 Drifted(Texture2D<float4> a_texture, float3 a_position, float3 a_normal, float3 a_drift, float a_scale, float a_offset)
{
	// D132: each patch on its own clock (a smooth offset over a few repeats), as the river's foam (D68): one clock
	// everywhere crossfaded the whole sheet at once, twice a period, and it pulsed (the user: "looping is back")
	const float2 patch = (a_position.xy + a_position.z * 0.7) / (a_scale * 2.5);
	const float2 cell = floor(patch);
	const float2 f = patch - cell;
	const float2 s = f * f * (3.0 - 2.0 * f);
	const float4 corner = frac(sin(float4(dot(cell, float2(127.1, 311.7)), dot(cell + float2(1, 0), float2(127.1, 311.7)), dot(cell + float2(0, 1), float2(127.1, 311.7)),
								   dot(cell + float2(1, 1), float2(127.1, 311.7)))) *
							   43758.5453);
	const float time = NoiseTime / DRIFT_PERIOD + lerp(lerp(corner.x, corner.y, s.x), lerp(corner.z, corner.w, s.x), s.y) * 2.0;
	const float phase0 = frac(time);
	const float phase1 = frac(time + 0.5);
	const float blend = abs(phase0 * 2.0 - 1.0);  // Each phase weighs nothing as it restarts
	float3 weights = pow(abs(a_normal), 4.0);
	weights /= max(weights.x + weights.y + weights.z, 1e-5);
	float4 result = 0.0;
	[unroll] for (int k = 0; k < 2; k++)
	{
		const float phase = k == 0 ? phase0 : phase1;
		const float3 p = (a_position - a_drift * phase * DRIFT_PERIOD) / a_scale + a_offset + 0.37 * k;
		const float2 sx = a_texture.Sample(WrapSampler, p.yz).xy * 2.0 - 1.0;
		const float2 sy = a_texture.Sample(WrapSampler, p.xz).xy * 2.0 - 1.0;
		const float2 sz = a_texture.Sample(WrapSampler, p.xy).xy * 2.0 - 1.0;
		const float4 sample = float4(weights.x * float3(0.0, sx.x, sx.y) + weights.y * float3(sy.x, 0.0, sy.y) + weights.z * float3(sz.x, sz.y, 0.0),
			weights.x * length(sx) + weights.y * length(sy) + weights.z * length(sz));
		result += sample * (k == 0 ? 1.0 - blend : blend);
	}
	return result;
}
/** D102: pixels of refraction offset per unit of tilt at Refraction 1, as a fraction of the screen height. */
static const float REFRACTION_SCREEN = 0.06;  // D120: 0.06, was 0.02: at its default a pixel or two, Refraction read as doing nothing
// D159: the white water's and the visibility's constants are in FallShading.hlsli (shared with the river shader's falls)

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

/** D149: how much deeper than its depth falling water reads (turbid with bubbles and sediment). */
static const float FALLS_MURK = 1.0;  // D154: 1 (D149-D153's 4 and 12 were fudges: measured, the falls' depth along the view ray and their water's fog match the river's)
/** D153: world units of the falls' water path (FALLS_MURK in it) at which their colour is wholly the water's deep colour. */
static const float FALLS_DEEP_AT = 160.0;
/** D150: world units of water (the path, FALLS_MURK in it) that stay clear, the range over which the murk comes in, and how milky and how dark the bed reads at its fullest. */
static const float MURK_CLEAR = 8.0;
static const float MURK_RANGE = 48.0;
static const float MURK_MILK = 0.0;  // D153: 0 (was 0.12): it greyed the sheets  // D151: 0.12 (was 0.45): the foam colour is the bright scene light; at noon the bed read as flat beige (SS 1057)
static const float MURK_DARK = 0.0;  // D154: off (the river lays no such step)

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

	// D125: only the falls' front surface is drawn: a layer behind it is the same body of water (an overlapping mesh's
	// inner layer or a C's far wall, there in no real water). D126: however far behind: with a 48-unit gap, a C's far wall
	// hundreds of units back still showed through it (SS 987); the user preferred none
	if (HasFront && FallShading::BehindFront(SharedData::GetScreenDepth(input.Position.z), SharedData::GetScreenDepth(FrontDepth.Load(int3(input.Position.xy, 0)))))
		discard;

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
	if (FallShading::HiddenInnerSide(input.Outward, toCamera))
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
		tangentNormal.xy *= NORMAL_STRENGTH;  // D127: 0.3 (was 0.6): nearer the river's gentle ripples
		normal = PerturbNormal(geometric, input.WorldPosition, flow.uvA, SafeNormalize(tangentNormal, float3(0.0, 0.0, 1.0)));
	}
	// D129: the river's fine ripples: its water type's three normal maps blended as Water.hlsl does (each map times its
	// amplitude). D131: in world space at their own scale, drifting along the flow (Drifted), not on the falls' texture
	// coordinates (stretched along the flow)
	const float3 drift = flow.velocity * min(1.0, MAX_DRIFT / max(length(flow.velocity), 1e-3));
	[branch] if (HasNoise)
	{
		const float3 tilt = (Drifted(Noise1Tex, position, geometric, drift, NoiseScale.x, 0.0).xyz * NoiseAmplitude.x +
								Drifted(Noise2Tex, position, geometric, drift, NoiseScale.y, 0.31).xyz * NoiseAmplitude.y +
								Drifted(Noise3Tex, position, geometric, drift, NoiseScale.z, 0.67).xyz * NoiseAmplitude.z) *
		                    RIPPLE_STRENGTH;
		normal = SafeNormalize(normal + tilt - normal * dot(normal, tilt), normal);
		// D147: the river's fine detail (its flow detail, 384-unit tiles, two drifting phases): small ripples the sun breaks
		// into sparkle; the water type's maps alone are hundreds to thousands of units across, a slow wobble (SS 1046)
		const float3 detail = Drifted(Noise3Tex, position, geometric, drift, DETAIL_TILE, 0.53).xyz * DETAIL_STRENGTH;
		normal = SafeNormalize(normal + detail - normal * dot(normal, detail), normal);
	}
	const float facing = saturate(dot(normal, toCamera));
	const float cosine = abs(dot(geometric, toCamera));

	// White water (D34, D42, D59): aerated as it runs and falls, foamed at rock, in a rock's wake and where it meets
	// the water below; with "Foam on the Falls" off, the streaks of vanilla's own texture still whiten it
	const float aerated = FallShading::Aerated(flow.runTime);
	// D121: not by aeration (the aerated colour, D120, is the falling water's body; as foam it speckled every sheet)
	// D145: not the bake's foam and wake: they mark every sheet's rims and crests as rock (a source diagnostic, SS 1040:
	// the crest foam none of D138-D143 touched). Back with the bake's own aeration
	// D146: the drop's bands (pool, foot, share of the drop) are off: on a cascade the top river counts as every drop's
	// source, so the lower crests read as mid-drop and foamed. Foam on the falls is to come from the bake's own aeration
	// D141, D143, D145: near the lip nothing foams (FallShading::Lip); D121, D128, D146: where the water breaks, along its
	// cut lines, seams and where sheets meet, only where it already falls (FallShading::LineWhite, D159: shared with the
	// river shader's falls)
	const float lip = FallShading::Lip(input.BelowSource, flow.runTime);

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
	// D174: foam wherever the sheet touches something, per pixel (FallShading::ContactStrength)
	const float sheetGap = max(SharedData::GetScreenDepth(NextSheetDepth.Load(int3(input.Position.xy, 0))) - sheetDepth, 0.0);
	const float contact = FallShading::ContactStrength(sheetGap, straightBehind, input.Fade.y, lip);
	// D120: without Foam on the Falls, none: the streaks' foam is the scene's full light against the water's dim pre-lit
	// colour, so even 15% of it laid a pale veil over every sheet (SS 971)
	// D122: covered as the river's foam (FluidField::GetFoamCover): dense foam solidly, thin foam only in the bubbles and
	// streaks of a pattern, here vanilla's normal map running on the baked flow (the river's runs on its simulated flow)
	// D127: fuzzier, as the river's: two blurred samples of the pattern at different scales blended (the river blends two
	// phases), and a wider soft band; one sharp sample broke the foam into crisp shapes
	// D137: the falls' own normal map again (D127's pattern): the zebra bands it was dropped for were the bake's foam and
	// wake fields (D136), not the pattern. Two blurred samples, each a whole number of repeats along the scroll, so it
	// moves exactly with the water and never resets; the lace where there is no normal map
	// D174: the river's foam pattern on the sheet, drifting on the flow (the water type's normal map: Noise1Tex)
	const float3 sheetNormal = SafeNormalize(input.Normal, float3(0.0, 0.0, 1.0));
	float foam = 0.0;
	[branch] if (contact > 0.02)
	{
		const float3 world = input.WorldPosition + FrameBuffer::CameraPosAdjust.xyz;
		const float pattern = HasNoise ? FallShading::FoamPattern(Noise1Tex, WrapSampler, world, sheetNormal, flow.velocity, SharedData::Timer, SharedData::MipBias) :
		                                 FallShading::FoamPattern(NormalTex, WrapSampler, world, sheetNormal, flow.velocity, SharedData::Timer, SharedData::MipBias);
		foam = FallShading::FoamCover(contact, pattern);
	}
	const float3 viewNormal = mul((float3x3)FrameBuffer::CameraView, normal - geometric);
	const float2 offset = viewNormal.xy * float2(1.0, -1.0) * RefractionStrength * REFRACTION_SCREEN * region.y * saturate(straightBehind / 8.0) / max(sheetDepth / 4096.0, 1.0);  // D120: shrinks from 4096 units, was 1024
	float2 pixel = clamp(input.Position.xy + offset, 0.5, region - 0.5);
	float behindDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(pixel, 0)));
	if (behindDepth < sheetDepth) {
		pixel = input.Position.xy;
		behindDepth = straightDepth;
	}
	const float3 behind = SceneColor.SampleLevel(ClampSampler, pixel / dimensions, 0).rgb;

	// D127: the sheet's water composed as the river's (Water.hlsl): its opacity the water type's fog curve over the water
	// the ray crosses (refractionMul) with Fluid Field's water tint (TintedRefractionWeight), its colour shallow to deep
	// linearly over the fog's far distance; aerated water at least AERATION_OPACITY opaque. D105-D121's separate
	// absorption made the falls solid tinted sheets where the river beside them reads clear
	// D148: the water the view ray crosses as the river measures it (Water.hlsl's distanceMul: the scene's depth behind the
	// surface along the ray, at the refracted pixel), so the falls darken with depth and show their bed where shallow as
	// the river does (SS 1049); where the sheet has left its rock (a curtain, air behind), its own thickness. D147's floor
	// of 120 units painted every sheet deep and dark (petroleum, SS 1050)
	const float rayScale = length(input.WorldPosition) / max(sheetDepth, 1e-3);
	const float rayBehind = max(behindDepth - sheetDepth, 0.0) * rayScale;
	// D149: times FALLS_MURK: falling water is turbid (bubbles, stirred sediment), and the thin water over the sheet's rock
	// stayed under 20% opaque where the river, deep, is murky (SS 1052, an opacity diagnostic)
	// D158: always the view ray's depth, as the river (no curtain rule): D120's "past 48 units behind, air" took the gap
	// behind most sheets (the depth rays: medians 47-218) for air and cleared them as soon as the depth job finished, where
	// that gap is the pool's water
	const float path = min(rayBehind, FogFar) * DepthScale * FALLS_MURK;
	const float refractionMul = 1.0 - pow(saturate((FogFar - path) / FogRange), FogAmount);
	const float opacity = max(FluidField::TintedRefractionWeight(refractionMul, path), AERATION_OPACITY * aerated);

	// The water's colour lit as Water.hlsl lights it: the colours as the engine hands them (the weather's water multiplier
	// in them), split by ExtractLighting into the sun's share and the ambient's, the sun's share shadowed
	const float3 shallow = Color::Water(ShallowColor);
	const float3 deep = Color::Water(DeepColor);
	float3 dirColor, ambientColor;
	// D153: shallow to deep over FALLS_DEEP_AT (the river's FogFar is hundreds of units; the falls' thin water stayed its
	// shallow, light grey-olive everywhere where the river beside them goes near black, SS 1063-1064)
	ShadowSampling::ExtractLighting(lerp(shallow, deep, saturate(path / FogFar)), dirColor, ambientColor);  // D154: the river's mix (Water.hlsl: over FogFar)
	const float3 lightDirection = SafeNormalize(SharedData::DirLightDirection.xyz, float3(0.0, 0.0, 1.0));
	const float3 sun = SharedData::InInterior ? 0.0 : ShadowSampling::GetDirectionalLighting();
	const float3 sceneLight = sun * shadow + ShadowSampling::GetAmbientLighting();
	const float3 waterLit = WithinLight(dirColor * shadow + ambientColor, sceneLight);
	// D127: aerated as the river's water (FluidField::ApplyAeration): toward its foam colour, a little (D120's re-saturated
	// 1.5x colour was the largest mismatch beside the river)
	const float3 foamColor = FluidField::FoamColor(waterLit, sceneLight);
	float3 waterColor = lerp(waterLit, foamColor, aerated * FallShading::AERATION_LIFT);
	if (SharedData::enbSettings.EnableWater) {
		// Water.hlsl's sun scattered in the water body
		const float scatter = saturate(dot(normal, lightDirection) * 0.5 + 0.5) * saturate(dot(-toCamera, lightDirection) * 0.5 + 0.5);
		waterColor += DeepColor * scatter * SharedData::DirLightColor.xyz * shadow * SharedData::enbSettings.WaterSunLightingMultiplier;
	}

	// Reflection (Water.hlsl's GetWaterSpecularColor and GetFresnelValue): the surroundings, the surface's detail tilting
	// the reflection by the reflection magnitude (Water.hlsl leans toward the flat water's up; here, the sheet's own
	// normal); the water type's reflection colour without Dynamic Cubemaps (vanilla's water without reflections).
	// D127: the river's weight (fresnel, scaled with the water's opacity below), mip 0 where clear, dulled by foam
	const float3 reflectionNormal = SafeNormalize(lerp(geometric, normal, ReflectionMagnitude), geometric);
	const float3 reflected = reflect(-toCamera, reflectionNormal);
	float3 environment = HasEnvironment ? EnvironmentTex.SampleLevel(ClampSampler, reflected, ENVIRONMENT_MIP * aerated).rgb : WithinLight(ReflectionColor, sceneLight);
	environment = clamp(environment, 0.0, FallShading::MAX_OUTPUT);  // NaN or a runaway texel becomes 0 or the cap, never a square
	// D155: screen-space reflections over the cubemap, as the river's (Water.hlsl's SSR): the reflected ray marched through
	// the scene's depth, the scene's colour where it meets something. The engine's SSR is made for a level water plane;
	// the falls' sheets lean every way, so they march their own. Without it they mirrored only the sky (grey-blue)
	{
		const float4 hit = ScreenReflection(input.WorldPosition, reflected, dimensions, region);
		// The river's weight: more where the reflection points along the view (Water.hlsl's ssrAmount)
		const float pointing = saturate(dot(-toCamera, reflected) * 0.5 + 0.5);
		environment = lerp(environment, hit.rgb, hit.a * sqrt(pointing));
	}
	const float viewAngle = 1.0 - facing;
	float fresnel = (1.0 - FresnelF0) * pow(viewAngle, 5.0) + FresnelF0;
	if (SharedData::enbSettings.EnableWater) {
		const float exponent = SharedData::enbSettings.WaterFresnelMultiplier;
		const float fresnelRI = exponent != 0.0 ? pow(abs(FresnelF0), exponent) : 1.0;
		fresnel = lerp(SharedData::enbSettings.WaterFresnelMin, SharedData::enbSettings.WaterFresnelMax, (1.0 - fresnelRI) * pow(viewAngle, 5.0) + fresnelRI) * SharedData::enbSettings.WaterReflectionAmount;
	}
	// D147: falling water's broken surface reflects less than the river's level one: seen grazing (as from above) its full
	// fresnel made every steep sheet a blue-grey mirror of the sky, beside the river's dark water colour (SS 1046)
	float3 color = lerp(waterColor, environment, saturate(fresnel) * FALLS_REFLECTION * (1.0 - 0.5 * foam));

	// The sun's glint (Water.hlsl's GetSunColor): the directional light as ShadowSampling gives it (the moon at night),
	// the water type's sun specular power and magnitude, in the surface's shadow; matte on foam. D127: the river's
	// (FluidField::SunGlint: widened and energy kept where the water is rough, here aerated), at its full strength
	const float3 sunReflected = reflect(-toCamera, normal);
	FluidField::PixelRoughness = 0.0;  // D147: the river's tight glint (aeration widened and dimmed it to nothing)
	float3 glint = sun * FluidField::SunGlint(saturate(dot(sunReflected, lightDirection)), SunSpecularPower) * SunSpecularStrength * shadow;
	if (SharedData::enbSettings.EnableWater)
		glint *= SharedData::enbSettings.WaterSunSpecularMultiplier;
	color += glint * (1.0 - 0.9 * foam);
	// D127: over what lies behind by the water's opacity, reflection and glint with it (Water.hlsl's final lerp)
	// D150: what lies behind seen through the water as the river sees its bed: lifted toward the water's milky foam colour
	// (Water.hlsl's ApplyAeration on the refraction) and dimmed toward the water's colour with depth, past the first few
	// tens of units clear (the user: clear for the first 30 cm, then dark, milky and murky; the falls showed their bed)
	const float murk = saturate((path - MURK_CLEAR) / MURK_RANGE);
	// D152: not fogged again: the scene behind was drawn with the sky's fog already (fogged twice it went slate blue,
	// SS 1058); only the water's own colour takes it, below
	const float3 behindSeen = lerp(lerp(behind, foamColor, MURK_MILK * murk), waterColor, MURK_DARK * murk);
	color = lerp(behindSeen, Fogged(color, length(input.WorldPosition)), opacity);

	// White water: D122, the river's foam colour from the same inputs, opaque over the rest (as ApplyFoamBlended)
	color = lerp(color, foamColor, foam);
	color = clamp(color, 0.0, FallShading::MAX_OUTPUT);  // D3D's min and max return the other value for NaN

	// Faded at seams over a mesh leading it (D67, blended here instead of dithered) and where it meets what lies
	// behind (no hard line where it enters the rock). D99: foam keeps the contact soft too, only shorter: added to it,
	// foam made the line where the sheet enters a boulder wholly opaque and white, tracing the faceted intersection of
	// the sheet's triangles with the rock's as a white sawtooth; and the water fades out toward its cut
	// D158: no lip fade (D141): the front-surface pre-pass (PSDepth) still claimed those pixels, so the fade punched holes
	// along the crests (SS 1086); FallShading::Alpha has none
	const float alpha = FallShading::Alpha(input.Fade.x, straightBehind, foam, input.Clip);
	if (alpha <= 0.002)
		discard;
	// D108: Waterfall Shading debug "Depth Behind": the open water or air behind the sheet. D110: in bands of colour (a grey
	// ramp read wrong through the tonemap): red under 50 units, yellow to 150, green to 300, blue to 600, magenta at the
	// reach (nothing hit)
	if (FallGround::DebugMode == 4) {
		return float4(FallShading::DepthBehindBand(input.Behind) * 0.5, 1.0);
	}
	return float4(color * alpha, alpha);  // Premultiplied
}

/**
 * D125: the front surface's depth, for the shading pass to draw only the nearest sheet (EQUAL): a sheet hides the
 * sheets behind it, so the falls read as one body of water, not transparent to themselves. Discards what PSMain hides
 * (the inner side over another sheet, the cut, and where it fades out by more than half), so a faded or cut front does
 * not hide what lies behind it.
 */
void PSDepth(VSOutput input)
{
	const float3 toCamera = SafeNormalize(-input.WorldPosition, float3(0.0, 0.0, 1.0));
	if (FallShading::HiddenInnerSide(input.Outward, toCamera))
		discard;
	const float sheetDepth = SharedData::GetScreenDepth(input.Position.z);
	const float straightBehind = max(SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0))) - sheetDepth, 0.0);
	if (saturate(input.Fade.x) * saturate(straightBehind / 4.0) * FallGround::CutFade(input.Clip) < 0.5)
		discard;
}

/**
 * D174: the falls' surface nearest behind their front (the second layer, for the foam where sheets meet): PSDepth's
 * discards, and the front itself (within FallShading::FRONT_EPSILON of FrontDepth) left out; the depth test keeps the nearest.
 */
void PSBehindDepth(VSOutput input)
{
	PSDepth(input);
	if (!FallShading::BehindFront(SharedData::GetScreenDepth(input.Position.z), SharedData::GetScreenDepth(FrontDepth.Load(int3(input.Position.xy, 0)))))
		discard;
}
#endif  // PSHADER
