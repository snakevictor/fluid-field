// Fluid Field ground fog (D21): fog lying on the terrain around the player, moved by the field.
//
// The fog's amount comes from the fog layer on the wind grid (carried by the wind, carved and
// stirred by actors, refilling), its shape from the ground height under the grid: density falls
// off exponentially with height above the ground. Each pixel marches its view ray up to the
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
	float Thickness;    // Height over which the fog thins to about a third
	float MaxHeight;    // Highest ground in the grid, world units
	uint StepCount;     // Samples along each ray
	float3 Albedo;      // Colour of the fog's scattering
	float MaxDistance;  // Rays stop here, world units (inside the field)
};

Texture2D<float> DepthTexture : register(t0);
Texture2D<float> FogField : register(t1);
Texture2D<float> GroundHeight : register(t2);
RWTexture2D<float4> MainRW : register(u0);

/** Fraction of the field's width over which the fog fades out toward its edge. */
static const float EDGE_FADE = 0.2;
/** Forward-scattering anisotropy (Henyey-Greenstein g): mist glows toward the sun. */
static const float PHASE_G = 0.45;
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

	// Skip rays that stay well above the fog: the fog is negligible a few thicknesses up
	const float3 camera = FrameBuffer::CameraPosAdjust.xyz;
	const float lowestZ = camera.z + min(0.0, direction.z * rayLength);
	if (lowestZ > MaxHeight + 5.0 * Thickness || rayLength <= 0.0)
		return;

	const float stepLength = rayLength / StepCount;
	const float jitter = Random::InterleavedGradientNoise(float2(dispatchID.xy), SharedData::FrameCount);

	// Light: the sun, scattered toward the eye, and the ambient light's average
	const float3 sun = SharedData::DirLightColor.xyz * HenyeyGreenstein(dot(direction, SharedData::DirLightDirection.xyz), PHASE_G) * 4.0 * Math::PI;
	const float3 ambient = max(float3(SharedData::AmbientSHR.x, SharedData::AmbientSHG.x, SharedData::AmbientSHB.x), 0.0) * 0.886;
	const float3 light = Albedo * (sun * (1.0 - SharedData::InInterior) + ambient);

	float transmittance = 1.0;
	float3 inscattering = 0.0;
	[loop] for (uint i = 0; i < StepCount; i++)
	{
		const float3 position = camera + direction * ((i + jitter) * stepLength);
		float fog, ground;
		SampleField(position.xy, fog, ground);
		const float height = position.z - ground;
		const float extinction = Density * fog * exp(-max(height, 0.0) / Thickness);
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
