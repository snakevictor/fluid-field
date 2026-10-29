// Fluid Field water, pass 3: conservative depth update from the face fluxes, upwind depth capped at MaxDepth.

#include "FluidField/WaterCommon.hlsli"

Texture2D<float> DepthIn : register(t0);
Texture2D<float2> Velocity : register(t1);
RWTexture2D<float> DepthOut : register(u0);

float Upwind(float faceVelocity, float upstreamIfPositive, float upstreamIfNegative)
{
	return faceVelocity * min(faceVelocity > 0.0 ? upstreamIfPositive : upstreamIfNegative, MaxDepth);
}

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);
	float depth = DepthIn[id];

	float dW = LoadDepth(DepthIn, logical + int2(-1, 0));
	float dE = LoadDepth(DepthIn, logical + int2(1, 0));
	float dS = LoadDepth(DepthIn, logical + int2(0, -1));
	float dN = LoadDepth(DepthIn, logical + int2(0, 1));

	float uW = Velocity[id].x;
	float uE = LoadClamped(Velocity, logical + int2(1, 0)).x;
	float vS = Velocity[id].y;
	float vN = LoadClamped(Velocity, logical + int2(0, 1)).y;

	float fluxW = Upwind(uW, dW, depth);
	float fluxE = Upwind(uE, depth, dE);
	float fluxS = Upwind(vS, dS, depth);
	float fluxN = Upwind(vN, depth, dN);

	depth -= DeltaTime / CellSize * (fluxE - fluxW + fluxN - fluxS);
	DepthOut[id] = max(depth, 0.0);
}
