// Fluid Field refractive falls (D91): the falls' lit pieces drawn after the world as see-through water, matching the river
// (Water.hlsl): refracted scene behind, water colour with depth, fresnel reflection, sun glint, and white water from each
// piece's flow bake. Vertices move and the texture flows as in the lit passes (FallGround), so only the material differs.
// FallRefraction draws it premultiplied, without writing depth.
// D98: each piece takes the water type it pours out of (FluidField::DrawRefractiveFalls) and composes its colour as
// Water.hlsl does.
// D102: a port of Water.hlsl's lighting, not a look-alike (ExtractLighting, sun shadow, FluidField::FoamColor). The sheet is
// a thin, mostly opaque layer: opacity from its own thickness (Water Depth) and aeration, what lies behind barely bent.

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
Texture2D<float4> SceneColor : register(t100);  // The scene before the falls
Texture2D<float> SceneDepth : register(t101);
Texture2D<float4> DiffuseTex : register(t121);  // Vanilla's falls texture
Texture2D<float4> NormalTex : register(t122);   // D159: t121-t122, off Light Limit Fix's local shadows (t102-t103)
TextureCube<float4> EnvironmentTex : register(t104);
Texture2D<float> FrontDepth : register(t105);  // D125: the scene's depth with the falls' front surface drawn in
Texture2D<float4> Noise1Tex : register(t106);  // D129: the water type's normal maps
Texture2D<float4> Noise2Tex : register(t107);
Texture2D<float4> Noise3Tex : register(t108);
Texture2D<float4> NextSheetDepth : register(t112);  // D174: the falls' surface nearest behind their front (cleared to far where none)

struct VSOutput
{
	float4 Position: SV_POSITION;
	float3 WorldPosition: TEXCOORD0;  // Camera-relative
	float2 TexCoord: TEXCOORD1;
	float3 Normal: TEXCOORD2;
	float3 Fade: TEXCOORD3;  // FallGround::Fade: seam fade, height above the water below; z: D98 its foot (VertexClip's a.w)
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
	// Same move as the lit passes (Lighting.hlsl, Utility.hlsl)
	const float3 model = v.Position + FallGround::Shift(vertexID, FallGround::Scroll.x, World[2].xyz);
	const float3 world = mul(World, float4(model, 1.0));
	const float3 relative = world - FrameBuffer::CameraPosAdjust.xyz;

	VSOutput output;
	output.Position = mul(FrameBuffer::CameraViewProj, float4(relative, 1.0));
	output.WorldPosition = relative;
	output.TexCoord = v.TexCoord;
	output.Normal = mul((float3x3)World, v.Normal);
	output.Fade.xy = FallGround::Fade(vertexID);
	output.Fade.y = min(output.Fade.y, 512.0);  // D139: clamped as BelowSource (D138)
	// D98: as the cut (D71), pools foam a mesh only at its foot
	output.Fade.z = (!FallGround::Enabled || FallGround::VertexBase == FallGround::NO_VERTEX_CLIP) ? 1.0 : fmod(FallGround::VertexClip[FallGround::VertexBase + vertexID].a.w, 2.0);  // D100: +2 marks an inward winding
	output.Clip = FallGround::ClipDistance(relative, vertexID);
	// the depth behind moves with the vertex (pushed out along its normal the gap behind widens), as Water.hlsl's
	const float3 shift = FallGround::Shift(vertexID, FallGround::Scroll.x, World[2].xyz);
	output.Behind = max(FallGround::Behind(vertexID) + dot(shift, v.Normal) / max(dot(v.Normal, v.Normal), 1e-6) * length(World[0].xyz), 0.0);
	// D138: "none" (NO_WATER, 1e4) clamped to 512 before interpolation, or a triangle mixing it drew a hard line
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

/** D155: screen-space reflection march: steps, first step and growth (world units), and surface thickness. */
static const int SSR_STEPS = 24;
static const float SSR_FIRST_STEP = 8.0;
static const float SSR_GROWTH = 1.18;
static const float SSR_THICKNESS = 48.0;

/**
 * D155: the scene colour where a reflected ray (camera-relative) first passes behind the scene's depth, and its confidence
 * (a: 0 off screen, too far, or no hit).
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

/** D151: the sky's distance fog amount, 0 to 1, as Water.hlsl: min(clamp, ((d - near) / (far - near))^power). */
float SkyFogAmount(float a_distance)
{
	// D152: none where Exponential Height Fog replaces vanilla fog, as Water.hlsl, or the falls tint unlike the river
	if (SkyFogFar <= SkyFogNear || (SharedData::exponentialHeightFogSettings.enabled && SharedData::exponentialHeightFogSettings.disableVanillaFog != 0))
		return 0.0;
	return min(SkyFogClamp, pow(saturate((a_distance - SkyFogNear) / (SkyFogFar - SkyFogNear)), SkyFogPower));
}

/** D151: a_color under the sky's fog, as Water.hlsl. */
float3 Fogged(float3 a_color, float a_distance)
{
	const float amount = SkyFogAmount(a_distance);
	return lerp(a_color, Color::Fog(lerp(SkyFogNearColor, SkyFogFarColor, amount)), Color::FogAlpha(amount));
}

/** Normalises, or returns a_fallback for a zero vector (its NaN spreads into squares under bloom). */
float3 SafeNormalize(float3 a_vector, float3 a_fallback)
{
	const float lengthSquared = dot(a_vector, a_vector);
	return lengthSquared > 1e-12 ? a_vector * rsqrt(lengthSquared) : a_fallback;
}

/** Applies a tangent-space normal around a_normal using screen derivatives (no tangents needed). */
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
 * D98: FallGround::Pool's height plus a 0-1 weight for how much of its footprint is pool. Pool's hard 0.2 cutoff made
 * square foam patches along the 16-unit texels; the weight fades instead.
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

/** D127: cubemap mip where wholly aerated; mip 0 (the river's mirror) where clear. */
static const float ENVIRONMENT_MIP = 1.0;

/** D102: least opacity of aerated water (bubbles scatter); D127: a floor under the river's fog-curve opacity. */
static const float AERATION_OPACITY = 0.3;
/** D127: how much the falls' normal map tilts the surface (it was drawn for an opaque, diffuse sheet). */
static const float NORMAL_STRENGTH = 0.8;
/** D129: the water type's ripples on the sheet (Water.hlsl's amplitude-weighted blend). */
static const float RIPPLE_STRENGTH = 1.0;
/** D147: fine detail ripples: world units per repeat, and tilt strength. */
static const float DETAIL_TILE = 160.0;
static const float DETAIL_STRENGTH = 0.8;
/** D147: share of the river's reflection (fresnel) the falls take. */
static const float FALLS_REFLECTION = 1.0;  // D154: the falls' depth and water parameters match the river's

/** D131: foam bubble pattern's world units per repeat, drift period (s) and top drift speed; as the river's foam (GetFoamCover). */
static const float FOAM_TILE = 160.0;
static const float DRIFT_PERIOD = 0.6;
static const float MAX_DRIFT = 120.0;  // D132: faster jumped visibly between phases

/**
 * D131: a_texture's xy (-1 to 1) laid tri-planar in world space (a_scale units per repeat), drifting along a_drift in two
 * crossfading phases, as the river's foam. The falls' own coordinates stretch along the flow into stripes.
 * @return World-space tilt (xyz) and the pattern, xy's length (w).
 */
float4 Drifted(Texture2D<float4> a_texture, float3 a_position, float3 a_normal, float3 a_drift, float a_scale, float a_offset)
{
	// D132: each patch on its own clock, as the river's foam (D68); one clock crossfaded the whole sheet at once and pulsed
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
/** D102: refraction offset per unit of tilt at Refraction 1, as a fraction of screen height. */
static const float REFRACTION_SCREEN = 0.06;  // D120: lower read as doing nothing
// D159: white water and visibility constants are in FallShading.hlsli (shared with the river shader's falls)

/** D102: world units of water between the sheet and what lies behind it; D104, D106: the baked depth behind. */
float BehindDepth(VSOutput a_input, float a_sheetDepth, float a_behindDepth)
{
	// D104: baked (rays off the sheet's inner side to the first rock), steady whatever the view
	const float rayScale = length(a_input.WorldPosition) / max(a_sheetDepth, 1e-3);
	// D106: baked only; the screen-depth conversion is not valid in this pass (it read as noise)
	return max(a_input.Behind, 0.0);
}

/** D149: how much deeper than its depth falling water reads (bubbles, sediment). */
static const float FALLS_MURK = 1.0;  // D154: the falls' depth and fog match the river's
/** D153: world units of water path at which the colour is wholly the deep colour. */
static const float FALLS_DEEP_AT = 160.0;
/** D150: world units of water path that stay clear, the range the murk comes in over, and the bed's milkiness and darkening at full murk. */
static const float MURK_CLEAR = 8.0;
static const float MURK_RANGE = 48.0;
static const float MURK_MILK = 0.0;  // D151, D153: off; it greyed the sheets
static const float MURK_DARK = 0.0;  // D154: off (the river has no such step)

/** D120: past this gap behind (world units) the sheet has left the rock, and the gap is air. */
static const float LAYER_GAP = 48.0;

/**
 * D102, D120: water a ray crosses: on rock the depth behind (D106) is the layer; off the rock (a curtain) only the sheet's
 * thickness counts, or every curtain fogged to the water colour (D105).
 */
float SheetPath(float a_behind, float a_cosine)
{
	const float onRock = max(min(a_behind, MaxPath), SheetThickness);
	const float layer = lerp(onRock, SheetThickness, smoothstep(LAYER_GAP * 0.5, LAYER_GAP, a_behind));
	return layer / max(a_cosine, 0.25);
}

/**
 * D102: limits a_color's luminance to a_light's: moonlit water cannot outshine the moon. Guards colours that arrive unlit;
 * where the weather's multiplier already darkens them, a no-op.
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

	// D125: only the falls' front surface is drawn; layers behind are the same body of water. D126: however far behind
	if (HasFront && FallShading::BehindFront(SharedData::GetScreenDepth(input.Position.z), SharedData::GetScreenDepth(FrontDepth.Load(int3(input.Position.xy, 0)))))
		discard;

	// D96: Flow takes the lit pass's coordinates (after vanilla's scroll and zoom, which it removes); raw ones slid against the flow
	const FallGround::FlowSample flow = FallGround::Flow(input.TexCoord / max(FallGround::InverseScale, 1e-4) + FallGround::TexOffset, position);
	const float4 diffuse = HasDiffuse ? DiffuseTex.Sample(WrapSampler, flow.uvA) : float4(0.5, 0.5, 0.5, 1.0);

	float3 geometric = SafeNormalize(input.Normal, toCamera);
	// D100: a sheet's inner side is hidden so crossing and nested sheets read as one body. D103: only where another sheet's
	// outer face lies behind it; a free curtain from behind still shows
	if (FallShading::HiddenInnerSide(input.Outward, toCamera))
		discard;
	// D120: face the camera; the meshes' winding is inconsistent, and a grazing normal made fresnel mirror the sky
	if (dot(geometric, toCamera) < 0.0)
		geometric = -geometric;
	float3 normal = geometric;
	[branch] if (HasNormal)
	{
		float3 tangentNormal = NormalTex.Sample(WrapSampler, flow.uvA).xyz * 2.0 - 1.0;
		tangentNormal.xy *= NORMAL_STRENGTH;  // D127
		normal = PerturbNormal(geometric, input.WorldPosition, flow.uvA, SafeNormalize(tangentNormal, float3(0.0, 0.0, 1.0)));
	}
	// D129: the water type's three normal maps blended as Water.hlsl. D131: in world space, drifting along the flow, not on
	// the falls' stretched coordinates
	const float3 drift = flow.velocity * min(1.0, MAX_DRIFT / max(length(flow.velocity), 1e-3));
	[branch] if (HasNoise)
	{
		const float3 tilt = (Drifted(Noise1Tex, position, geometric, drift, NoiseScale.x, 0.0).xyz * NoiseAmplitude.x +
								Drifted(Noise2Tex, position, geometric, drift, NoiseScale.y, 0.31).xyz * NoiseAmplitude.y +
								Drifted(Noise3Tex, position, geometric, drift, NoiseScale.z, 0.67).xyz * NoiseAmplitude.z) *
		                    RIPPLE_STRENGTH;
		normal = SafeNormalize(normal + tilt - normal * dot(normal, tilt), normal);
		// D147: the river's fine detail ripples that break the sun into sparkle; the water type's maps alone are too large
		const float3 detail = Drifted(Noise3Tex, position, geometric, drift, DETAIL_TILE, 0.53).xyz * DETAIL_STRENGTH;
		normal = SafeNormalize(normal + detail - normal * dot(normal, detail), normal);
	}
	const float facing = saturate(dot(normal, toCamera));
	const float cosine = abs(dot(geometric, toCamera));

	// White water (D34, D42, D59). D121: aeration tints the body, not foam. D145: not the bake's foam and wake (they mark
	// every rim and crest). D146: no drop bands (cascades foamed mid-drop). D141, D143, D145: nothing foams near the lip
	// (FallShading::Lip); D121, D128, D146: line foam only where it already falls (FallShading::LineWhite, D159)
	const float aerated = FallShading::Aerated(flow.runTime);
	const float lip = FallShading::Lip(input.BelowSource, flow.runTime);

	// Sun shadow (Water.hlsl's surfaceShadow): CS's directional shadows and the world's (terrain, clouds); the sheet is thin,
	// so the surface's shadow serves throughout
	float detailedShadow;
	const float shadow = ShadowSampling::GetLightingShadow(input.WorldPosition, detailedShadow) * ShadowSampling::GetWorldShadow(input.WorldPosition, FrameBuffer::CameraPosAdjust.xyz);

	// What lies behind, faintly bent by the surface's tilt; a sample landing in front of the falls is not used. D102: the
	// offset no longer scales with the gap behind or much with distance
	float2 dimensions;
	SceneColor.GetDimensions(dimensions.x, dimensions.y);
	const float2 region = dimensions * FrameBuffer::DynamicResolutionParams1.xy;  // Rendered part under dynamic resolution
	const float sheetDepth = SharedData::GetScreenDepth(input.Position.z);
	const float straightDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
	const float straightBehind = max(straightDepth - sheetDepth, 0.0);
	// D121, D174: foam wherever the sheet touches rock, another mesh or sheet, per pixel (FallShading::ContactStrength)
	const float sheetGap = FallShading::SheetGap(NextSheetDepth, int2(input.Position.xy), sheetDepth, input.ToOtherSheet);                                    // D174: as Water.hlsl
	const float aboveWater = FallShading::AboveWater(input.WorldPosition + FrameBuffer::CameraPosAdjust.xyz, input.Fade.z, input.Fade.y, input.BelowSource);  // D176
	const float contact = FallShading::ContactStrength(sheetGap, straightBehind, aboveWater, lip, input.Clip);
	// D122: covered as the river's foam (FluidField::GetFoamCover): dense foam solid, thin foam only in a pattern's bubbles.
	// D174: the pattern is the water type's normal map (Noise1Tex) drifting on the flow, else the falls' own
	const float3 sheetNormal = SafeNormalize(input.Normal, float3(0.0, 0.0, 1.0));
	float foam = 0.0;
	[branch] if (contact > 0.02)
	{
		const float3 world = input.WorldPosition + FrameBuffer::CameraPosAdjust.xyz;
		const float pattern = HasNoise ? FallShading::FoamPattern(Noise1Tex, WrapSampler, world, sheetNormal, 0.0, flow.velocity, SharedData::Timer, SharedData::MipBias) :
		                                 FallShading::FoamPattern(NormalTex, WrapSampler, world, sheetNormal, 0.0, flow.velocity, SharedData::Timer, SharedData::MipBias);
		foam = FallShading::FoamCover(contact, pattern);
	}
	const float3 viewNormal = mul((float3x3)FrameBuffer::CameraView, normal - geometric);
	const float2 offset = viewNormal.xy * float2(1.0, -1.0) * RefractionStrength * REFRACTION_SCREEN * region.y * saturate(straightBehind / 8.0) / max(sheetDepth / 4096.0, 1.0);  // D120: shrinks beyond 4096 units
	float2 pixel = clamp(input.Position.xy + offset, 0.5, region - 0.5);
	float behindDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(pixel, 0)));
	if (behindDepth < sheetDepth) {
		pixel = input.Position.xy;
		behindDepth = straightDepth;
	}
	const float3 behind = SceneColor.SampleLevel(ClampSampler, pixel / dimensions, 0).rgb;

	// D127: composed as Water.hlsl: opacity is the fog curve over the path (refractionMul) with Fluid Field's tint
	// (TintedRefractionWeight), at least AERATION_OPACITY when aerated. D148: the path is the scene depth behind along the
	// view ray at the refracted pixel, as Water.hlsl's distanceMul
	const float rayScale = length(input.WorldPosition) / max(sheetDepth, 1e-3);
	const float rayBehind = max(behindDepth - sheetDepth, 0.0) * rayScale;
	// D149: times FALLS_MURK (turbid water). D158: always the view ray's depth, as the river; the gap behind a sheet is
	// the pool's water, not air
	const float path = min(rayBehind, FogFar) * DepthScale * FALLS_MURK;
	const float refractionMul = 1.0 - pow(saturate((FogFar - path) / FogRange), FogAmount);
	const float opacity = max(FluidField::TintedRefractionWeight(refractionMul, path), AERATION_OPACITY * aerated);

	// Lit as Water.hlsl: ExtractLighting splits the colour into sun and ambient shares, the sun's shadowed
	const float3 shallow = Color::Water(ShallowColor);
	const float3 deep = Color::Water(DeepColor);
	float3 dirColor, ambientColor;
	ShadowSampling::ExtractLighting(lerp(shallow, deep, saturate(path / FogFar)), dirColor, ambientColor);  // D153, D154: shallow to deep over FogFar, as Water.hlsl
	const float3 lightDirection = SafeNormalize(SharedData::DirLightDirection.xyz, float3(0.0, 0.0, 1.0));
	const float3 sun = SharedData::InInterior ? 0.0 : ShadowSampling::GetDirectionalLighting();
	const float3 sceneLight = sun * shadow + ShadowSampling::GetAmbientLighting();
	const float3 waterLit = WithinLight(dirColor * shadow + ambientColor, sceneLight);
	// D127: aerated toward the foam colour, a little, as the river (FluidField::ApplyAeration)
	const float3 foamColor = FluidField::FoamColor(waterLit, sceneLight);
	float3 waterColor = lerp(waterLit, foamColor, aerated * FallShading::AERATION_LIFT);
	if (SharedData::enbSettings.EnableWater) {
		// Water.hlsl's sun scattered in the water body
		const float scatter = saturate(dot(normal, lightDirection) * 0.5 + 0.5) * saturate(dot(-toCamera, lightDirection) * 0.5 + 0.5);
		waterColor += DeepColor * scatter * SharedData::DirLightColor.xyz * shadow * SharedData::enbSettings.WaterSunLightingMultiplier;
	}

	// Reflection (Water.hlsl's GetWaterSpecularColor, GetFresnelValue), tilted from the sheet's own normal (not up) by
	// ReflectionMagnitude; the reflection colour without Dynamic Cubemaps. D127: mip 0 where clear, dulled by foam
	const float3 reflectionNormal = SafeNormalize(lerp(geometric, normal, ReflectionMagnitude), geometric);
	const float3 reflected = reflect(-toCamera, reflectionNormal);
	float3 environment = HasEnvironment ? EnvironmentTex.SampleLevel(ClampSampler, reflected, ENVIRONMENT_MIP * aerated).rgb : WithinLight(ReflectionColor, sceneLight);
	environment = clamp(environment, 0.0, FallShading::MAX_OUTPUT);  // NaN or a runaway texel becomes 0 or the cap, never a square
	// D155: own screen-space reflections over the cubemap; the engine's SSR assumes a level water plane
	{
		const float4 hit = ScreenReflection(input.WorldPosition, reflected, dimensions, region);
		// More where the reflection points along the view (Water.hlsl's ssrAmount)
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
	// D147: scaled by FALLS_REFLECTION, less over foam
	float3 color = lerp(waterColor, environment, saturate(fresnel) * FALLS_REFLECTION * (1.0 - 0.5 * foam));

	// Sun glint (Water.hlsl's GetSunColor), shadowed, matte on foam. D127: the river's FluidField::SunGlint
	const float3 sunReflected = reflect(-toCamera, normal);
	FluidField::PixelRoughness = 0.0;  // D147: the river's tight glint (aeration widened and dimmed it to nothing)
	float3 glint = sun * FluidField::SunGlint(saturate(dot(sunReflected, lightDirection)), SunSpecularPower) * SunSpecularStrength * shadow;
	if (SharedData::enbSettings.EnableWater)
		glint *= SharedData::enbSettings.WaterSunSpecularMultiplier;
	color += glint * (1.0 - 0.9 * foam);
	// D127: over what lies behind by opacity (Water.hlsl's final lerp). D150: the bed seen through clear water first, then
	// milky and darker with depth
	const float murk = saturate((path - MURK_CLEAR) / MURK_RANGE);
	// D152: the scene behind is already fogged; only the water's own colour takes the fog
	const float3 behindSeen = lerp(lerp(behind, foamColor, MURK_MILK * murk), waterColor, MURK_DARK * murk);
	color = lerp(behindSeen, Fogged(color, length(input.WorldPosition)), opacity);

	// White water: D122, the river's foam colour, opaque over the rest (as ApplyFoamBlended)
	color = lerp(color, foamColor, foam);
	color = clamp(color, 0.0, FallShading::MAX_OUTPUT);  // D3D's min and max return the other value for NaN

	// Faded at seams (D67, blended here, not dithered), where it meets what lies behind, and toward its cut (D99).
	// D158: no lip fade; the front pre-pass still claimed those pixels, punching holes
	const float alpha = FallShading::Alpha(input.Fade.x, straightBehind, foam, input.Clip) * (aboveWater < 0.0 ? foam : 1.0);  // D176: as Water.hlsl
	if (alpha <= 0.002)
		discard;
	// D108, D110: "Depth Behind" debug in colour bands (FallShading::DepthBehindBand)
	if (FallGround::DebugMode == 4) {
		return float4(FallShading::DepthBehindBand(input.Behind) * 0.5, 1.0);
	}
	// Quadrants: four views split over the screen. Top left the mesh's uv (addresses the bake), a cell per 0.25; top right
	// the bake's texture coordinates (across red, along green, a cell per unit, blue checker); bottom left the flow's
	// direction (as DebugMode 2); bottom right the vertex inputs (red depth behind to 300, green seam fade, blue the cut)
	if (FallGround::DebugMode == 5) {
		const float2 screen = input.Position.xy * SharedData::BufferDim.zw * FrameBuffer::DynamicResolutionParams2.xy;  // render pixels to the screen's 0-1 (dynamic resolution)
		float3 view;
		if (screen.y < 0.5 && screen.x < 0.5) {
			const float2 cell = input.TexCoord * 4.0;
			view = float3(frac(cell), (uint(floor(cell.x)) + uint(floor(cell.y))) & 1u ? 0.6 : 0.0);
		} else if (screen.y < 0.5) {
			const float2 coords = flow.uvA + float2(0.0, FallGround::Scroll.x);
			view = float3(frac(coords), (uint(floor(coords.x)) + uint(floor(coords.y))) & 1u ? 0.6 : 0.0);
		} else if (screen.x < 0.5) {
			view = (0.5 + 0.5 * flow.velocity / max(length(flow.velocity), 1e-3)) * saturate(length(flow.velocity) / FallGround::DEBUG_FLOW_SPEED);
		} else {
			view = float3(saturate(input.Behind / 300.0), saturate(input.Fade.x), saturate(input.Clip / 100.0));
		}
		return float4(view * 0.5, 1.0);
	}
	return float4(color * alpha, alpha);  // Premultiplied
}

/**
 * D125: the front surface's depth, so the shading pass (EQUAL) draws only the nearest sheet. Discards what PSMain hides
 * (inner side, cut, faded past half), so a faded or cut front hides nothing.
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
 * D174: the falls' surface nearest behind their front, for foam where sheets meet: PSDepth's discards plus the front itself
 * (within FallShading::BEHIND_LAYER_EPSILON of FrontDepth); the depth test keeps the nearest.
 */
void PSBehindDepth(VSOutput input)
{
	PSDepth(input);
	// Not BehindFront's FRONT_EPSILON: where sheets cross it hid the other sheet. The same vertex shader drew the front, so
	// its depth matches exactly
	if (SharedData::GetScreenDepth(input.Position.z) - SharedData::GetScreenDepth(FrontDepth.Load(int3(input.Position.xy, 0))) <= FallShading::BEHIND_LAYER_EPSILON)
		discard;
}
#endif  // PSHADER
