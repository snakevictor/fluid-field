// Fluid Field fall spray and mist (D31): GPU particles thrown where the falls foam (impacts, rock in
// the way, the joins with the water above and below). Stateless: each particle's position follows
// from its emitter, a hash of its index and the time, so nothing is simulated or stored. Spray is
// small droplets thrown up and out, falling back under gravity; mist is large soft puffs that rise
// and fade. Drawn as camera-facing quads after the falls' surface, sharing its constants; droplets
// are drawn smeared along their motion (D33), as the eye sees fast spray, and lit white.

#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

struct Emitter
{
	float3 Position;
	float Strength;    // 0 to 1
	float3 Direction;  // The water's motion, unit
	float Speed;       // World units per second
	float3 Normal;
	float Kind;  // 0: a drop (the water leaves the surface running on), 1: a plunge (thrown up and out), 2: a strike on rock or another piece
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
	float MistOpacity;
	float SprayStretch;
	float SprayOpacity;
	float Pad;
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

/** D47: water this fast (world units per second) throws spray at full rate. */
static const float FAST_WATER = 700.0;
/** D48: droplets closer to the camera than this vanish (fading in over four times as far). */
static const float NEAR_FADE = 60.0;

struct VSOutput
{
	float4 Position: SV_POSITION;
	float2 Corner: TEXCOORD0;  // -1 to 1 across the quad
	float Alpha: TEXCOORD1;
	float Mist: TEXCOORD2;  // 1 for mist, 0 for spray
	float ViewDepth: TEXCOORD3;
	float Seed: TEXCOORD4;
	float Above: TEXCOORD5;  // World units above the water the particle came out of (plunges), fading it there
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

	// Weaker emitters throw less often
	// D47: the faster the water that throws it, the more spray, thrown harder, bigger and denser: slow
	// water barely throws any, fast water throws it all
	const float fast = saturate(emitter.Speed / FAST_WATER);
	const bool active = r3 < emitter.Strength * (0.15 + 0.85 * fast);
	const uint kind = uint(round(emitter.Kind));

	// A frame across the surface, for the random spread
	const float3 up = normalize(emitter.Normal);
	const float3 side = normalize(cross(up, abs(up.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0)));
	const float3 forward = cross(side, up);
	const float angle = r0 * 6.2831853;
	const float3 spread = side * cos(angle) + forward * sin(angle);
	const float3 flowVelocity = emitter.Direction * emitter.Speed;
	const float3 flat = float3(emitter.Direction.xy, 0.0);

	// D35: everything leaves with the water's own motion, then gravity (spray) or drag (mist) takes over
	float3 position = emitter.Position + spread * (r1 * 8.0);
	float3 motion = 0.0;  // World units per second now, along which a droplet is smeared
	float size;
	float alpha;
	const float t = age / life;
	if (mist) {
		// Mist is carried off with the water and slows in the air: x = v0 tau (1 - e^(-t / tau)); it settles a little
		float3 v0;
		if (kind == 1)
			v0 = flat * emitter.Speed * 0.3 + spread * (40.0 + 60.0 * r2) + up * (20.0 + 40.0 * r1);  // Billows out from the plunge
		else
			v0 = flowVelocity * (0.5 + 0.3 * r2) + spread * 25.0;
		const float tau = 0.8;
		position += v0 * tau * (1.0 - exp(-age / tau)) + float3(0, 0, -MistRise * 0.25 * age);
		size = MistSize * (0.4 + 0.9 * t) * (0.7 + 0.6 * r2) * (kind == 1 ? 1.0 : 0.6);
		alpha = MistOpacity * smoothstep(0.0, 0.2, t) * (1.0 - smoothstep(0.4, 1.0, t)) * emitter.Strength * (kind == 1 ? 1.0 : 0.6);
	} else {
		float3 v0;
		if (kind == 0)
			v0 = flowVelocity * (0.85 + 0.3 * r1) + spread * emitter.Speed * 0.05 + up * 20.0 * r2;  // Runs on off the drop
		else if (kind == 1)
			v0 = (up * (0.4 + 1.0 * r2) + spread * (0.2 + 0.5 * r1)) * SpraySpeed * (0.5 + 1.5 * fast) + flat * emitter.Speed * 0.2;  // Thrown up and out
		else
			v0 = flowVelocity * 0.5 + (up * 0.8 + spread * 0.3) * min(max(emitter.Speed, 150.0), 600.0) * (0.15 + 0.35 * r2) * (0.6 + 0.6 * fast);  // Kicked up where it strikes, staying near the water
		position += v0 * age + float3(0, 0, -0.5 * Gravity * age * age);
		motion = v0 + float3(0, 0, -Gravity * age);
		size = SpraySize * (0.5 + 1.0 * r2);
		alpha = SprayOpacity * (0.7 + 0.3 * fast) * smoothstep(0.0, 0.08, t) * (1.0 - smoothstep(0.55, 1.0, t));
	}

	VSOutput output;
	if (!active || alpha <= 0.001) {
		// Nothing to draw: a degenerate quad outside the view, so it costs no pixels
		output = (VSOutput)0;
		output.Position = float4(2.0, 2.0, 2.0, 1.0);
		return output;
	}

	// A camera-facing quad; a droplet's is stretched along its motion as seen from the camera
	const float3 relative = position - FrameBuffer::CameraPosAdjust.xyz;
	alpha *= smoothstep(NEAR_FADE, NEAR_FADE * 4.0, length(relative));  // D48
	const float3 toCamera = -relative / max(length(relative), 1e-3);
	float3 right = normalize(float3(FrameBuffer::CameraView[0].x, FrameBuffer::CameraView[0].y, FrameBuffer::CameraView[0].z));
	float3 viewUp = normalize(float3(FrameBuffer::CameraView[1].x, FrameBuffer::CameraView[1].y, FrameBuffer::CameraView[1].z));
	float length2 = size;
	const float3 across = motion - toCamera * dot(motion, toCamera);
	if (dot(across, across) > 1.0) {
		viewUp = normalize(across);
		right = normalize(cross(viewUp, toCamera));
		length2 = size + length(across) * SprayStretch;
	}
	const float2 corner = CORNERS[vertexID];
	const float3 world = relative + right * corner.x * (size * 0.5) + viewUp * corner.y * (length2 * 0.5);

	output.Position = mul(FrameBuffer::CameraViewProj, float4(world, 1.0));
	output.Corner = corner;
	output.Alpha = alpha;
	output.Mist = mist ? 1.0 : 0.0;
	output.ViewDepth = length(world);
	output.Seed = r0;
	output.Above = kind == 1 ? world.z + FrameBuffer::CameraPosAdjust.z - (emitter.Position.z - 4.0) : 1000.0;
	return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
	if (input.Alpha <= 0.001)
		discard;
	const float radius = length(input.Corner);
	float shape;
	if (input.Mist > 0.5) {
		// Mist (D36): a soft, uneven wisp, not a disc: a falloff broken up by a few drifting waves
		const float2 c = input.Corner;
		const float wisp = 0.5 + 0.25 * sin(c.x * 2.3 + input.Seed * 19.0) * sin(c.y * 2.9 + input.Seed * 7.0) + 0.25 * sin((c.x + c.y) * 4.1 + input.Seed * 31.0);
		shape = pow(saturate(1.0 - radius), 1.6) * saturate(wisp * 1.3);
	} else {
		shape = (1.0 - smoothstep(0.35, 1.0, radius)) * (0.75 + 0.25 * sin(input.Seed * 40.0 + input.Corner.x * 5.0) * sin(input.Corner.y * 6.0));
	}
	// Soft where it meets anything in front of it, and fading into the water it rose from
	const float sceneDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
	const float depth = SharedData::GetScreenDepth(input.Position.z);
	const float soft = saturate((sceneDepth - depth) / (input.Mist > 0.5 ? 150.0 : 6.0)) * saturate(input.Above / (input.Mist > 0.5 ? 40.0 : 4.0));
	const float alpha = saturate(input.Alpha * shape * soft);
	if (alpha <= 0.002)
		discard;
	const float3 ambient = max(float3(SharedData::AmbientSHR.x, SharedData::AmbientSHG.x, SharedData::AmbientSHB.x), 0.0) * 0.886;
	// White water, lit by the light's brightness, not its colour (the sky's blue made it glow blue)
	// The sun weighs little: it lit the mist in shadow too, and glowed it orange at dusk
	const float light = Color::RGBToLuminance(ambient) + Color::RGBToLuminance(SharedData::DirLightColor.rgb) * 0.12;
	const float3 color = float3(0.94, 0.95, 0.96) * light * Brightness * (input.Mist > 0.5 ? 1.6 : 2.2);
	return float4(color * alpha, alpha);  // Premultiplied
}
