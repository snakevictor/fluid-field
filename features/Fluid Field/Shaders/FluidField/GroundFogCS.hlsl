// Fluid Field ground fog (D21): fog lying on the terrain around the player, moved by the field.
//
// The fog's amount comes from the fog layer on the wind grid (carried by the wind, carved and
// stirred by actors, refilling), its shape from the ground height under the grid: a dense layer
// lying on the ground with a soft top, taller where there is more fog, so banks stand out and
// clearings dip. Each pixel marches its view ray up to the
// scene's depth through that layer, after the opaque scene is lit and before water and effects,
// and blends the fog in, lit by the sun (scattered forward) and the ambient light.

#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "FluidField/FogPattern.hlsli"

cbuffer GroundFogCB : register(b0)
{
	int2 GridMin;          // World cell of the field's logical texel (0, 0)
	uint GridRes;          // Texels per side, a power of two
	float CellSize;        // World units per texel
	float Density;         // Extinction per world unit at the ground where the field is 1
	float Thickness;       // Height of the layer's top above the ground where the field is 1
	float MaxHeight;       // Highest ground in the grid, world units
	uint StepCount;        // Samples along each ray
	float3 Albedo;         // Colour of the fog's scattering, times its brightness
	float MaxDistance;     // Rays stop here, world units (inside the ground map)
	int2 GroundMin;        // World cell (of GroundCellSize) of the ground map's texel (0, 0); it covers the whole fog area
	uint GroundRes;        // Ground map texels per side, a power of two
	float GroundCellSize;  // World units per ground map texel
	float2 PatternOffset;  // How far the resting pattern has drifted with the weather's wind, world units
	float2 FogPad;
};

Texture2D<float> DepthTexture : register(t0);
Texture2D<float> FogField : register(t1);
Texture2D<float> GroundHeight : register(t2);
Texture2D<float2> Wind : register(t3);
RWTexture2D<float4> MainRW : register(u0);

/** Fraction of the simulated area's width over which it hands over to the resting fog beyond it. */
static const float EDGE_FADE = 0.2;
/** Fraction of the ground map's width over which the fog fades out at the edge of the fog area. */
static const float AREA_FADE = 0.15;
/** Forward-scattering anisotropy (Henyey-Greenstein g): mist glows a little toward the sun. */
static const float PHASE_G = 0.25;
/** Share of the sun in the fog's light; ground fog sits low and is mostly lit by the sky. */
static const float SUN_SHARE = 0.15;
/** Fraction of the layer's height over which its top softens into clear air. */
static const float TOP_SOFTNESS = 0.5;
/** How much taller fog packed to twice its resting amount stands. */
static const float PILE_RISE = 0.8;
/** How much higher churning air lifts the fog's top, at LIFT_SPEED. */
static const float LIFT = 0.6;
static const float LIFT_SPEED = 150.0;
/** World units across one wisp in the fog, and how much the wisps vary its density. */
static const float WISP_SIZE = 70.0;
static const float WISP_CONTRAST = 0.55;
/** Seconds between restarts of each of the wisps' two phases as the wind carries them. */
static const float WISP_PERIOD = 2.0;
/** World units across one billow of the layer's top, and how much the billows raise and lower it. */
static const float BILLOW_SIZE = 220.0;
static const float BILLOW_HEIGHT = 0.35;
/** How strongly the fog above a point shades it from the sky: the layer's base is darker than its top. */
static const float SELF_SHADING = 1.0;
/** Fastest the wisps drift, world units per second, so fast gusts do not smear them. */
static const float WISP_MAX_DRIFT = 200.0;
/** Ground below this is unknown land: no fog there. */
static const float NO_GROUND = -1.0e6;

/** Bilinear ground height from the ground map; very low (no fog) where unknown or outside it. */
float SampleGround(float2 worldXY, out float areaFade)
{
	const float2 cellPosition = worldXY / GroundCellSize - 0.5;
	const float2 logical = cellPosition - float2(GroundMin);
	const float fromEdge = min(min(logical.x, logical.y), min(GroundRes - 1 - logical.x, GroundRes - 1 - logical.y));
	areaFade = smoothstep(0.0, 1.0, fromEdge / (AREA_FADE * GroundRes));
	if (fromEdge <= 0.0)
		return NO_GROUND * 10.0;
	const int2 c = int2(floor(cellPosition));
	const float2 t = cellPosition - float2(c);
	const int mask = int(GroundRes) - 1;
	const float4 g = float4(GroundHeight.Load(int3(c & mask, 0)), GroundHeight.Load(int3((c + int2(1, 0)) & mask, 0)),
		GroundHeight.Load(int3((c + int2(0, 1)) & mask, 0)), GroundHeight.Load(int3((c + int2(1, 1)) & mask, 0)));
	// Unknown ground at any corner keeps the fog away rather than smearing a fake slope
	return any(g < NO_GROUND) ? NO_GROUND * 10.0 : lerp(lerp(g.x, g.y, t.x), lerp(g.z, g.w, t.x), t.y);
}

/**
 * How much fog lies at an absolute world position: the simulation inside the simulated area,
 * the resting pattern beyond it, handed over smoothly near its edge.
 */
float SampleFog(float2 worldXY)
{
	const float resting = FogPattern::Resting(worldXY, PatternOffset);
	const float2 cellPosition = worldXY / CellSize - 0.5;
	const float2 logical = cellPosition - float2(GridMin);
	const float fromEdge = min(min(logical.x, logical.y), min(GridRes - 1 - logical.x, GridRes - 1 - logical.y));
	if (fromEdge <= 0.0)
		return resting;
	const int2 c = int2(floor(cellPosition));
	const float2 t = cellPosition - float2(c);
	const int mask = int(GridRes) - 1;
	const float simulated = lerp(lerp(FogField.Load(int3(c & mask, 0)), FogField.Load(int3((c + int2(1, 0)) & mask, 0)), t.x),
		lerp(FogField.Load(int3((c + int2(0, 1)) & mask, 0)), FogField.Load(int3((c + int2(1, 1)) & mask, 0)), t.x), t.y);
	return lerp(resting, simulated, smoothstep(0.0, 1.0, fromEdge / (EDGE_FADE * GridRes)));
}

/** The wind at an absolute world position, bilinear; zero outside the grid. */
float2 SampleWind(float2 worldXY)
{
	const float2 cellPosition = worldXY / CellSize - 0.5;
	const float2 logical = cellPosition - float2(GridMin);
	if (any(logical < 0.0) || any(logical >= float(GridRes) - 1.0))
		return 0.0;
	const int2 c = int2(floor(cellPosition));
	const float2 t = cellPosition - float2(c);
	const int mask = int(GridRes) - 1;
	return lerp(lerp(Wind.Load(int3(c & mask, 0)), Wind.Load(int3((c + int2(1, 0)) & mask, 0)), t.x),
		lerp(Wind.Load(int3((c + int2(0, 1)) & mask, 0)), Wind.Load(int3((c + int2(1, 1)) & mask, 0)), t.x), t.y);
}

/**
 * Wisps inside the fog, carried by the wind (two phases restarted half a period apart, cross-faded
 * so neither restart shows): the fog's density varies around its mean, so its churning shows.
 */
float Wisps(float3 position, float2 wind)
{
	const float2 drift = wind * min(1.0, WISP_MAX_DRIFT / max(length(wind), 1e-3)) * WISP_PERIOD / WISP_SIZE;
	const float time = SharedData::Timer / WISP_PERIOD;
	const float phase0 = frac(time);
	const float phase1 = frac(time + 0.5);
	const float2 uv = position.xy / WISP_SIZE + position.z / (WISP_SIZE * 1.5);
	const float n0 = FogPattern::ValueNoise(uv - drift * phase0) * 0.5 + FogPattern::ValueNoise(uv * 2.3 - drift * phase0 * 2.3) * 0.3 + FogPattern::ValueNoise(uv * 5.1 - drift * phase0 * 5.1 + 1.9) * 0.2;
	const float n1 = FogPattern::ValueNoise(uv - drift * phase1 + 7.1) * 0.5 + FogPattern::ValueNoise(uv * 2.3 - drift * phase1 * 2.3 + 3.7) * 0.3 + FogPattern::ValueNoise(uv * 5.1 - drift * phase1 * 5.1 + 9.2) * 0.2;
	return lerp(n0, n1, abs(phase0 * 2.0 - 1.0));
}

float HenyeyGreenstein(float cosTheta, float g)
{
	const float g2 = g * g;
	return (1.0 - g2) / (4.0 * Math::PI * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= uint2(SharedData::BufferDim.xy)))
		return;

	float2 uv = float2(dispatchID.xy + 0.5) * SharedData::BufferDim.zw;
	uv *= FrameBuffer::DynamicResolutionParams2.xy;

	// The pixel's surface, camera-relative, and the ray to it
	const float depth = DepthTexture[dispatchID.xy];
	float4 positionCS = mul(FrameBuffer::CameraViewProjInverse, float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1));
	const float3 surface = positionCS.xyz / positionCS.w;
	const float surfaceDistance = length(surface);
	const float3 direction = surface / max(surfaceDistance, 1e-3);
	const float rayLength = min(depth >= 1.0 ? MaxDistance : surfaceDistance, MaxDistance);

	// Skip rays that stay above the fog
	const float3 camera = FrameBuffer::CameraPosAdjust.xyz;
	const float bandTop = MaxHeight + Thickness * (1.0 + PILE_RISE) * (1.0 + LIFT) * (1.0 + BILLOW_HEIGHT);
	const float lowestZ = camera.z + min(0.0, direction.z * rayLength);
	if (lowestZ > bandTop || rayLength <= 0.0)
		return;
	// Only march where the ray is inside the fog's band: from where it comes down into it, if the
	// camera is above it, to where it climbs out of it or hits the scene
	float rayStart = 0.0;
	float rayEnd = rayLength;
	if (camera.z > bandTop)
		rayStart = (camera.z - bandTop) / max(-direction.z, 1e-4);
	else if (direction.z > 1e-4)
		rayEnd = min(rayEnd, (bandTop - camera.z) / direction.z);
	if (rayEnd <= rayStart)
		return;
	const float span = rayEnd - rayStart;

	const float jitter = Random::InterleavedGradientNoise(float2(dispatchID.xy), SharedData::FrameCount);

	// Light: the sun, scattered toward the eye, and the ambient light's average
	const float3 sun = SharedData::DirLightColor.xyz * HenyeyGreenstein(dot(direction, SharedData::DirLightDirection.xyz), PHASE_G) * 4.0 * Math::PI;
	const float3 ambient = max(float3(SharedData::AmbientSHR.x, SharedData::AmbientSHG.x, SharedData::AmbientSHB.x), 0.0) * 0.886;
	const float3 light = Albedo * (sun * SUN_SHARE * (1.0 - SharedData::InInterior) + ambient);

	float transmittance = 1.0;
	float3 inscattering = 0.0;
	[loop] for (uint i = 0; i < StepCount; i++)
	{
		// Samples packed toward the start of the span (near the camera), where detail shows most
		const float u = (i + jitter) / StepCount;
		const float stepLength = span * 2.0 * u / StepCount;
		const float3 position = camera + direction * (rayStart + span * u * u);
		float areaFade;
		const float ground = SampleGround(position.xy, areaFade);
		const float fog = SampleFog(position.xy) * areaFade;
		// A layer on the ground with a soft top: taller where fog is packed, lifted where the air churns
		const float height = position.z - ground;
		if (fog <= 0.0 || height > Thickness * 4.0)
			continue;
		const float2 wind = SampleWind(position.xy);
		const float churn = saturate(length(wind) / LIFT_SPEED);
		// Billows: the top rises and falls, drifting with the wind, so the layer is not a flat sheet
		const float billow = Wisps(float3(position.xy * (WISP_SIZE / BILLOW_SIZE), 0.0), wind);
		const float top = Thickness * lerp(0.4, 1.0, saturate(fog)) * (1.0 + PILE_RISE * saturate(fog - 1.0)) * (1.0 + LIFT * churn) *
		                  lerp(1.0 - BILLOW_HEIGHT, 1.0 + BILLOW_HEIGHT, billow);
		const float wisps = lerp(1.0 - WISP_CONTRAST, 1.0 + WISP_CONTRAST, Wisps(position, wind));
		const float extinction = Density * fog * wisps * (1.0 - smoothstep(top * (1.0 - TOP_SOFTNESS), top, height));
		if (extinction <= 0.0)
			continue;
		// Light reaching this point from above passes through the fog over it: the base is darker
		const float above = max(top - height, 0.0);
		const float shading = exp(-SELF_SHADING * Density * fog * above);
		const float stepTransmittance = exp(-extinction * stepLength);
		inscattering += transmittance * (1.0 - stepTransmittance) * light * shading;
		transmittance *= stepTransmittance;
		if (transmittance < 0.01)
			break;
	}

	if (transmittance >= 0.999)
		return;
	const float4 scene = MainRW[dispatchID.xy];
	MainRW[dispatchID.xy] = float4(scene.rgb * transmittance + inscattering, scene.a);
}
