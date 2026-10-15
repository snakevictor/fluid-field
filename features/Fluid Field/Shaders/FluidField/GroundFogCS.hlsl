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

cbuffer GroundFogCB : register(b0)
{
	int2 GridMin;       // World cell of the field's logical texel (0, 0)
	uint GridRes;       // Texels per side, a power of two
	float CellSize;     // World units per texel
	float Density;      // Extinction per world unit at the ground where the field is 1
	float Thickness;    // Height of the layer's top above the ground where the field is 1
	float MaxHeight;    // Highest ground in the grid, world units
	uint StepCount;     // Samples along each ray
	float3 Albedo;      // Colour of the fog's scattering, times its brightness
	float MaxDistance;  // Rays stop here, world units (inside the field)
};

Texture2D<float> DepthTexture : register(t0);
Texture2D<float> FogField : register(t1);
Texture2D<float> GroundHeight : register(t2);
Texture2D<float2> Wind : register(t3);
RWTexture2D<float4> MainRW : register(u0);

/** Fraction of the field's width over which the fog fades out toward its edge. */
static const float EDGE_FADE = 0.2;
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
static const float WISP_SIZE = 140.0;
static const float WISP_CONTRAST = 0.45;
/** Seconds between restarts of each of the wisps' two phases as the wind carries them. */
static const float WISP_PERIOD = 2.0;
/** Fastest the wisps drift, world units per second, so fast gusts do not smear them. */
static const float WISP_MAX_DRIFT = 200.0;
/** Ground below this is unknown land: no fog there. */
static const float NO_GROUND = -1.0e6;

/** Field value and ground height at an absolute world position, bilinear; fog fades to 0 at the grid edge. */
void SampleField(float2 worldXY, out float fog, out float ground)
{
	const float2 cellPosition = worldXY / CellSize - 0.5;
	const float2 logical = cellPosition - float2(GridMin);
	const float fromEdge = min(min(logical.x, logical.y), min(GridRes - 1 - logical.x, GridRes - 1 - logical.y));
	fog = 0.0;
	ground = NO_GROUND * 10.0;
	if (fromEdge <= 0.0)
		return;
	const int2 c = int2(floor(cellPosition));
	const float2 t = cellPosition - float2(c);
	const int mask = int(GridRes) - 1;
	const int3 c00 = int3(c & mask, 0);
	const int3 c10 = int3((c + int2(1, 0)) & mask, 0);
	const int3 c01 = int3((c + int2(0, 1)) & mask, 0);
	const int3 c11 = int3((c + int2(1, 1)) & mask, 0);
	fog = lerp(lerp(FogField.Load(c00), FogField.Load(c10), t.x), lerp(FogField.Load(c01), FogField.Load(c11), t.x), t.y);
	const float4 g = float4(GroundHeight.Load(c00), GroundHeight.Load(c10), GroundHeight.Load(c01), GroundHeight.Load(c11));
	// Unknown ground at any corner keeps the fog away rather than smearing a fake slope
	ground = any(g < NO_GROUND) ? NO_GROUND * 10.0 : lerp(lerp(g.x, g.y, t.x), lerp(g.z, g.w, t.x), t.y);
	fog *= smoothstep(0.0, 1.0, fromEdge / (EDGE_FADE * GridRes));
}

/** The wind at an absolute world position, bilinear; zero outside the grid. */
float2 SampleWind(float2 worldXY)
{
	const float2 cellPosition = worldXY / CellSize - 0.5;
	const int2 c = int2(floor(cellPosition));
	const float2 t = cellPosition - float2(c);
	const int mask = int(GridRes) - 1;
	return lerp(lerp(Wind.Load(int3(c & mask, 0)), Wind.Load(int3((c + int2(1, 0)) & mask, 0)), t.x),
		lerp(Wind.Load(int3((c + int2(0, 1)) & mask, 0)), Wind.Load(int3((c + int2(1, 1)) & mask, 0)), t.x), t.y);
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
	const float n0 = ValueNoise(uv - drift * phase0) * 0.6 + ValueNoise(uv * 2.3 - drift * phase0 * 2.3) * 0.4;
	const float n1 = ValueNoise(uv - drift * phase1 + 7.1) * 0.6 + ValueNoise(uv * 2.3 - drift * phase1 * 2.3 + 3.7) * 0.4;
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
	const float lowestZ = camera.z + min(0.0, direction.z * rayLength);
	if (lowestZ > MaxHeight + Thickness * (1.0 + PILE_RISE) * (1.0 + LIFT) || rayLength <= 0.0)
		return;

	const float stepLength = rayLength / StepCount;
	const float jitter = Random::InterleavedGradientNoise(float2(dispatchID.xy), SharedData::FrameCount);

	// Light: the sun, scattered toward the eye, and the ambient light's average
	const float3 sun = SharedData::DirLightColor.xyz * HenyeyGreenstein(dot(direction, SharedData::DirLightDirection.xyz), PHASE_G) * 4.0 * Math::PI;
	const float3 ambient = max(float3(SharedData::AmbientSHR.x, SharedData::AmbientSHG.x, SharedData::AmbientSHB.x), 0.0) * 0.886;
	const float3 light = Albedo * (sun * SUN_SHARE * (1.0 - SharedData::InInterior) + ambient);

	float transmittance = 1.0;
	float3 inscattering = 0.0;
	[loop] for (uint i = 0; i < StepCount; i++)
	{
		const float3 position = camera + direction * ((i + jitter) * stepLength);
		float fog, ground;
		SampleField(position.xy, fog, ground);
		// A layer on the ground with a soft top: taller where fog is packed, lifted where the air churns
		const float height = position.z - ground;
		if (fog <= 0.0 || height > Thickness * 4.0)
			continue;
		const float2 wind = SampleWind(position.xy);
		const float churn = saturate(length(wind) / LIFT_SPEED);
		const float top = Thickness * lerp(0.4, 1.0, saturate(fog)) * (1.0 + PILE_RISE * saturate(fog - 1.0)) * (1.0 + LIFT * churn);
		const float wisps = lerp(1.0 - WISP_CONTRAST, 1.0 + WISP_CONTRAST, Wisps(position, wind));
		const float extinction = Density * fog * wisps * (1.0 - smoothstep(top * (1.0 - TOP_SOFTNESS), top, height));
		if (extinction <= 0.0)
			continue;
		const float stepTransmittance = exp(-extinction * stepLength);
		inscattering += transmittance * (1.0 - stepTransmittance) * light;
		transmittance *= stepTransmittance;
		if (transmittance < 0.01)
			break;
	}

	if (transmittance >= 0.999)
		return;
	const float4 scene = MainRW[dispatchID.xy];
	MainRW[dispatchID.xy] = float4(scene.rgb * transmittance + inscattering, scene.a);
}
