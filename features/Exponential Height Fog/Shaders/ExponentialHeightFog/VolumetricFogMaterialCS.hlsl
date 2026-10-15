#include "ExponentialHeightFog/VolumetricFogCSCommon.hlsli"

RWTexture3D<float4> VBufferA : register(u0);

#if defined(FLUID_FIELD)
// Fluid Field's fog density (D21): a 2D multiplier on the wind grid around the player, toroidal
// like the rest of the field. 1 is the weather's fog unchanged; actors stirring the air thin it.
Texture2D<float> FluidFieldFog : register(t6);

/** Fraction of the grid's width over which the field fades back to the weather's fog at its edge. */
static const float FLUID_FIELD_FOG_EDGE = 0.15;

float FluidFieldFogDensity(float3 positionWS)
{
	if (SharedData::fluidFieldSettings.FogEnabled == 0)
		return 1.0f;
	const float2 worldXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
	const int res = int(SharedData::fluidFieldSettings.FogGridRes);
	// Cell coordinates with texel centres at whole numbers, and the logical position in the grid
	const float2 cellPosition = worldXY / SharedData::fluidFieldSettings.FogCellSize - 0.5f;
	const float2 logical = cellPosition - float2(SharedData::fluidFieldSettings.FogGridMin);
	const float fromEdge = min(min(logical.x, logical.y), min(res - 1 - logical.x, res - 1 - logical.y));
	if (fromEdge <= 0.0f)
		return 1.0f;
	const int2 c = int2(floor(cellPosition));
	const float2 t = cellPosition - float2(c);
	const int mask = res - 1;
	const float fog = lerp(lerp(FluidFieldFog.Load(int3(c & mask, 0)), FluidFieldFog.Load(int3((c + int2(1, 0)) & mask, 0)), t.x),
		lerp(FluidFieldFog.Load(int3((c + int2(0, 1)) & mask, 0)), FluidFieldFog.Load(int3((c + int2(1, 1)) & mask, 0)), t.x), t.y);
	return lerp(1.0f, fog, smoothstep(0.0f, 1.0f, fromEdge / (FLUID_FIELD_FOG_EDGE * res)));
}
#endif

[numthreads(8, 8, 4)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (!ExponentialHeightFog::IsInsideVolumetricGrid(dispatchID))
		return;

	float viewDepth;
	float3 positionWS = ExponentialHeightFog::ComputeCellWorldPosition(dispatchID, 0.5f.xxx, viewDepth);

	float extinction = ExponentialHeightFog::EvaluateHeightFogExtinction(positionWS, FrameBuffer::CameraPosAdjust.xyz);
#if defined(FLUID_FIELD)
	extinction *= FluidFieldFogDensity(positionWS);
#endif
	float3 albedo = saturate(SharedData::exponentialHeightFogSettings.volumetricFogAlbedo.rgb);
	float3 scattering = extinction * albedo * SharedData::exponentialHeightFogSettings.volumetricFogAlbedo.a;

	VBufferA[dispatchID] = float4(scattering, extinction);
}
