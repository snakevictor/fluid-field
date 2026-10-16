// Fluid Field fall spray and mist (D31): GPU particles thrown where the falls foam (impacts, rock in
// the way, the joins with the water above and below). Stateless: each particle's position follows
// from its emitter, a hash of its index and the time, so nothing is simulated or stored. Spray is
// small droplets thrown up and out, falling back under gravity; mist is large soft puffs that rise
// and fade. Drawn as camera-facing quads after the falls' surface, sharing its constants.

#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

struct Emitter
{
	float3 Position;
	float Strength;    // 0 to 1
	float3 Direction;  // The water's motion, unit
	float Speed;       // 1 for free flow
	float3 Normal;
	float Pad;
};

cbuffer FallSurfaceCB : register(b7)
{
	float Time;
	float UScale;
	float VScale;
	float BaseSpeed;
	float WaveHeight;
	float WaveLength;
	float RippleHeight;
	float RippleLength;
	float BulgeHeight;
	float ShallowDepth;
	float ContactWidth;
	float Brightness;
	uint DebugMode;
	float VanillaScroll;
	uint EmitterCount;
	uint SprayPerEmitter;
	uint MistPerEmitter;
	float SprayLife;
	float SpraySpeed;
	float SpraySize;
	float MistLife;
	float MistSize;
	float MistRise;
	float Gravity;
	float2 Pad;
};

StructuredBuffer<Emitter> Emitters : register(t100);
Texture2D<float> SceneDepth : register(t103);

float Hash(uint a_value)
{
	a_value ^= a_value >> 16;
	a_value *= 0x7feb352d;
	a_value ^= a_value >> 15;
	a_value *= 0x846ca68b;
	a_value ^= a_value >> 16;
	return float(a_value & 0xFFFFFF) / 16777216.0;
}

struct VSOutput
{
	float4 Position: SV_POSITION;
	float2 Corner: TEXCOORD0;  // -1 to 1 across the quad
	float Alpha: TEXCOORD1;
	float Mist: TEXCOORD2;  // 1 for mist, 0 for spray
	float ViewDepth: TEXCOORD3;
	float Seed: TEXCOORD4;
};

VSOutput VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	static const float2 CORNERS[6] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1) };
	const uint perEmitter = SprayPerEmitter + MistPerEmitter;
	const uint emitterIndex = instanceID / perEmitter;
	const uint slot = instanceID % perEmitter;
	const Emitter emitter = Emitters[min(emitterIndex, EmitterCount - 1)];
	const bool mist = slot >= SprayPerEmitter;
	const float life = mist ? MistLife : SprayLife;

	// Each particle is born again every life, at a staggered phase, with fresh random choices
	const uint seed = instanceID * 7919u;
	const float phase = Hash(seed);
	const float cycle = (Time + phase * life) / life;
	const uint generation = uint(floor(cycle));
	const float age = frac(cycle) * life;
	const uint born = seed ^ (generation * 104729u);
	const float r0 = Hash(born + 1u), r1 = Hash(born + 2u), r2 = Hash(born + 3u), r3 = Hash(born + 4u);

	// Some emitters throw little: they skip generations in proportion
	const bool active = r3 < saturate((emitter.Strength - 0.4) * 1.8);

	// A frame across the surface, for the random spread
	const float3 up = normalize(emitter.Normal);
	const float3 side = normalize(cross(up, abs(up.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0)));
	const float3 forward = cross(side, up);
	const float angle = r0 * 6.2831853;
	const float3 spread = side * cos(angle) + forward * sin(angle);

	float3 position = emitter.Position + spread * (r1 * 12.0);
	float size;
	float alpha;
	if (mist) {
		// Mist drifts with the water, rises and spreads
		position += (emitter.Direction * 30.0 * emitter.Speed + spread * 20.0) * age + float3(0, 0, MistRise * age);
		size = MistSize * (0.5 + age / life) * (0.7 + 0.6 * r2);
		const float t = age / life;
		alpha = 0.18 * smoothstep(0.0, 0.2, t) * (1.0 - smoothstep(0.5, 1.0, t)) * emitter.Strength;
	} else {
		// Spray is thrown up and out, carried with the water, and falls back
		const float speed = SpraySpeed * (0.35 + 0.65 * r2) * (0.4 + 0.6 * emitter.Strength);
		const float3 velocity = (up * (0.6 + 0.4 * r1) + spread * 0.5) * speed + emitter.Direction * 120.0 * emitter.Speed;
		position += velocity * age + float3(0, 0, -0.5 * Gravity * age * age);
		size = SpraySize * (0.6 + 0.8 * r2) * (1.0 + age / life);
		const float t = age / life;
		alpha = 0.85 * smoothstep(0.0, 0.08, t) * (1.0 - smoothstep(0.55, 1.0, t)) * emitter.Strength;
	}
	if (!active)
		alpha = 0.0;

	// A camera-facing quad: its corners along the view's right and up
	const float3 right = normalize(float3(FrameBuffer::CameraView[0].x, FrameBuffer::CameraView[0].y, FrameBuffer::CameraView[0].z));
	const float3 viewUp = normalize(float3(FrameBuffer::CameraView[1].x, FrameBuffer::CameraView[1].y, FrameBuffer::CameraView[1].z));
	const float2 corner = CORNERS[vertexID];
	const float3 world = position + (right * corner.x + viewUp * corner.y) * (size * 0.5) - FrameBuffer::CameraPosAdjust.xyz;

	VSOutput output;
	output.Position = mul(FrameBuffer::CameraViewProj, float4(world, 1.0));
	output.Corner = corner;
	output.Alpha = alpha;
	output.Mist = mist ? 1.0 : 0.0;
	output.ViewDepth = length(world);
	output.Seed = r0;
	return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
	if (input.Alpha <= 0.001)
		discard;
	const float radius = length(input.Corner);
	// Spray: a small bright droplet cluster; mist: a soft round puff
	float shape = input.Mist > 0.5 ? (1.0 - smoothstep(0.2, 1.0, radius)) : (1.0 - smoothstep(0.35, 1.0, radius));
	shape *= input.Mist > 0.5 ? 1.0 : (0.75 + 0.25 * sin(input.Seed * 40.0 + input.Corner.x * 5.0) * sin(input.Corner.y * 6.0));
	// Soft where it meets anything in front of it
	const float sceneDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
	const float depth = SharedData::GetScreenDepth(input.Position.z);
	const float soft = saturate((sceneDepth - depth) / (input.Mist > 0.5 ? 40.0 : 6.0));
	const float alpha = saturate(input.Alpha * shape * soft);
	if (alpha <= 0.002)
		discard;
	const float3 ambient = max(float3(SharedData::AmbientSHR.x, SharedData::AmbientSHG.x, SharedData::AmbientSHB.x), 0.0) * 0.886;
	// The falls' white water tone (FallSurface.hlsl's WhiteColor), brighter: spray catches the light
	const float3 color = float3(0.42, 0.75, 1.0) * (ambient + Color::RGBToLuminance(SharedData::DirLightColor.rgb) * 0.3) * Brightness * 1.4;
	return float4(color * alpha, alpha);  // Premultiplied
}
