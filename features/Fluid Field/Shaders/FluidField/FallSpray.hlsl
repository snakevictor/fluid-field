// Fluid Field fall spray and mist (D31): GPU particles thrown where the falls foam (impacts, rock in
// the way, the joins with the water above and below). Stateless: each particle's position follows
// from its emitter, a hash of its index and the time, so nothing is simulated or stored. Spray is
// small droplets thrown up and out, falling back under gravity; mist is large soft puffs that rise
// and fade. Drawn as camera-facing quads after the falls' surface, sharing its constants; droplets
// are drawn smeared along their motion (D33), as the eye sees fast spray, and lit white.

#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "FluidField/FallEffects.hlsli"

struct Emitter
{
	float3 Position;
	float Strength;    // 0 to 1
	float3 Direction;  // The water's motion, unit
	float Speed;       // World units per second
	float3 Normal;
	float Kind;  // 0: a drop (the water leaves the surface running on), 1: a plunge (thrown up and out), 2: a strike on rock or another piece
};

StructuredBuffer<Emitter> Emitters : register(t100);
Texture2D<float> SceneDepth : register(t103);
// D183: the water simulation's near level (its surface: slopes, height over vanilla's, coverage; its flow: xy, foam z) and its
// map of vanilla's surface (WaterMap's g): the spray and mist come from the foamy water itself (the user: from the water surface)
Texture2D<float4> WaterSurfaceTex : register(t66);
Texture2D<float4> WaterFlowTex : register(t67);
Texture2D<float4> WaterMapTex : register(t104);

/** D183: the simulated water at a world position: its surface height, flow (xy), foam and coverage; false where there is none. */
bool SampleWater(float2 a_world, out float a_height, out float2 a_flow, out float a_foam, out float a_coverage)
{
	a_height = 0.0;
	a_flow = 0.0;
	a_foam = 0.0;
	a_coverage = 0.0;
	const uint enabled = SharedData::fluidFieldSettings.Enabled;
	const int res = int(SharedData::fluidFieldSettings.GridRes);
	const float cellSize = max(SharedData::fluidFieldSettings.CellSize, 1e-3);
	if (enabled == 0 || res <= 1 || MapRes == 0)
		return false;
	const float2 cellPos = a_world / cellSize - 0.5;
	const int2 c = int2(floor(cellPos));
	const int2 logical = c - SharedData::fluidFieldSettings.GridMin;
	if (any(logical < 0) || any(logical >= res - 1))
		return false;
	const float2 t = cellPos - float2(c);
	const int mask = res - 1;
	const float4 s00 = WaterSurfaceTex.Load(int3(c & mask, 0)), s10 = WaterSurfaceTex.Load(int3((c + int2(1, 0)) & mask, 0));
	const float4 s01 = WaterSurfaceTex.Load(int3((c + int2(0, 1)) & mask, 0)), s11 = WaterSurfaceTex.Load(int3((c + int2(1, 1)) & mask, 0));
	const float4 surface = lerp(lerp(s00, s10, t.x), lerp(s01, s11, t.x), t.y);
	const float4 f00 = WaterFlowTex.Load(int3(c & mask, 0)), f10 = WaterFlowTex.Load(int3((c + int2(1, 0)) & mask, 0));
	const float4 f01 = WaterFlowTex.Load(int3((c + int2(0, 1)) & mask, 0)), f11 = WaterFlowTex.Load(int3((c + int2(1, 1)) & mask, 0));
	const float4 flow = lerp(lerp(f00, f10, t.x), lerp(f01, f11, t.x), t.y);
	// Vanilla's surface from the map (bilinear; none where a corner has none)
	const float2 m = a_world / MapSpacing;
	const int2 mi = int2(floor(m));
	const float2 mt = m - float2(mi);
	const int mmask = int(MapRes) - 1;
	const float v00 = WaterMapTex.Load(int3(mi & mmask, 0)).y, v10 = WaterMapTex.Load(int3((mi + int2(1, 0)) & mmask, 0)).y;
	const float v01 = WaterMapTex.Load(int3((mi + int2(0, 1)) & mmask, 0)).y, v11 = WaterMapTex.Load(int3((mi + int2(1, 1)) & mmask, 0)).y;
	if (min(min(v00, v10), min(v01, v11)) < -1.0e7)
		return false;
	a_height = lerp(lerp(v00, v10, mt.x), lerp(v01, v11, mt.x), mt.y) + surface.z;
	a_flow = flow.xy;
	a_foam = flow.z;
	a_coverage = saturate(surface.w);
	return a_coverage > 0.05;
}

/** D183: a marker on every emitter, by kind (pink past a crest, green a drop's foot, blue a strike; dim while off), drawn over
 * everything, in place of its first particle. */
static const bool DEBUG_EMITTERS = false;

/** D183: seconds an emitter stays on or off before it is drawn again, and the least chance it is on (calm water). */
static const float SWITCH_PERIOD = 1.5;
static const float SWITCH_LEAST = 0.6;
/** D183: the simulation's foam from which water throws spray and mist, and at which it throws them in full. */
static const float SEED_FOAM_FROM = 0.03;  // the simulation's foam mostly lies between 0.05 and 0.25
static const float SEED_FOAM_FULL = 0.35;

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
	float Above: TEXCOORD5;         // World units above the water the particle came out of (plunges), fading it there
	float3 WorldOffset: TEXCOORD6;  // D181: its centre, camera-relative
	float3 Right: TEXCOORD7;        // D182: the quad's axes in the world (its corner's x and y), for its shading as a small volume
	float3 Up: TEXCOORD8;
};

/** D181: world units from the camera over which the falls' spray and mist fade out, and past which none is drawn (about 30 and 43 m). */
// D183: 6000 and 8000 (about 85 and 115 m; was 2000 and 3000: they went out in plain sight, SS 1351)
static const float SPRAY_NEAR_FADE = 6000.0;
static const float SPRAY_FAR = 8000.0;
/** D183: from this far the particles thin out, to this share of them by SPRAY_NEAR_FADE. */
static const float SPRAY_LOD_FROM = 1500.0;
static const float SPRAY_FAR_KEEP = 0.2;
/** D182: how much of the light's own colour the particles take (the rest its brightness). */
static const float PARTICLE_COLOR = 0.35;
/** D182: world units below where it was thrown a droplet from a sheet fades out over (the water and rock below lie near). */
static const float SPRAY_MOST_DROP = 48.0;
/** D183: how much harder than SpraySpeed a plunge or a strike throws its droplets (up to about 2 m). */
static const float SPRAY_LAUNCH = 3.2;
/** D183: a strike's mist against a drop foot's, and how much longer it lasts. */
static const float STRIKE_MIST = 0.6;
static const float STRIKE_MIST_LIFE = 1.6;
/** D183: how far a strike's splash leans on the water's way. */
static const float STRIKE_LEAN = 0.45;
/** D183: world units below the crest the short-arc droplets and the falling ones fade out over. */
static const float CREST_SHORT = 60.0;
static const float CREST_LONG = 400.0;
/** D183: how much wider than tall a mist puff is (a low drifting veil, not a glowing ball). */
static const float MIST_FLAT = 2.2;
/** D184: every mist's opacity against MistOpacity (the user: 30% more see-through). */
static const float MIST_SHEER = 0.7;
/** D184: where sheets meet (the user's green): world units a second its mist drifts on along the water's way (grounded, drifting
 * forward a bit), and how much bigger and denser it is than other mist. */
static const float MEET_DRIFT = 70.0;
static const float MEET_HEAVY = 1.6;
/** D184: the share of a meet's droplet slots thrown, and how hard against a strike's. */
static const float MEET_DROPS = 0.5;
static const float MEET_SPLASH = 0.7;
/** D184: how much harder than the water's speed the crests' spray is thrown out (the user's pink: lots of velocity). */
static const float CREST_LAUNCH = 1.4;
/** D184: a droplet's quad at least this many times as long as it is big, and this narrow (the user: tear-like, not round). */
static const float DROP_LONG = 2.6;
static const float DROP_THIN = 0.7;
/** D184: droplets this much bigger than SpraySize (the user: 50% bigger). */
static const float DROP_BIG = 1.5;
/** D184: a droplet's opacity as it leaves (the user: 75%), and how dark its shade side and underside are against its lit side. */
static const float DROP_OPACITY = 0.75;
static const float DROP_SHADE = 0.5;
/** D184: radians a crest's drops fan out to either side of the way it falls (a V), and world units along the crest they leave from. */
static const float CREST_FAN = 0.3;
static const float CREST_WIDTH = 14.0;

VSOutput VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	static const float2 CORNERS[6] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1) };
	const uint perEmitter = SprayPerEmitter + MistPerEmitter;
	const uint emitterIndex = instanceID / perEmitter;
	const uint slot = instanceID % perEmitter;
	Emitter emitter = Emitters[min(emitterIndex, EmitterCount - 1)];
	// D183: its kind, and its own id over it (FallSurfaceRenderer: from where it is; its list's order changes as the camera moves)
	const uint packed = uint(round(emitter.Kind));
	const uint kind = packed & 3u;
	const uint id = packed >> 2;
	// D184: a crest throws spray only, in its mist slots too (the user: many more of its splashes)
	const bool mist = slot >= SprayPerEmitter && kind != 0;
	// D183: a strike's mist lingers (the user's blue: a bit of mist that lasts longer than the plunges')
	const float life = mist ? MistLife * (kind == 2 ? STRIKE_MIST_LIFE : 1.0) : SprayLife;

	// Each particle is born again every life, at a staggered phase, with fresh random choices
	const uint seed = (id * perEmitter + slot) * 7919u;
	const float phase = Hash(seed);
	const float cycle = (Time + phase * life) / life;
	const uint generation = uint(floor(cycle));
	const float age = frac(cycle) * life;
	const uint born = seed ^ (generation * 104729u);
	const float r0 = Hash(born + 1u), r1 = Hash(born + 2u), r2 = Hash(born + 3u), r3 = Hash(born + 4u);

	// D183: its emitter, a fixed point on the water's surface (FallSites: where the falls plunge, cross a surface, or their water
	// meets rock or land at it), switched on and off over time: each SWITCH_PERIOD, on with a chance rising with the simulated
	// foam and flow there (without the simulation, its own strength)
	{
		float height, foam, coverage;
		float2 flow;
		float chance = emitter.Strength;
		if (SampleWater(emitter.Position.xy, height, flow, foam, coverage))
			chance = saturate(emitter.Strength * (SWITCH_LEAST + smoothstep(SEED_FOAM_FROM, SEED_FOAM_FULL, foam) + length(flow) / FAST_WATER));
		const float birth = Time - age;
		const uint epoch = uint(floor(birth / SWITCH_PERIOD + Hash(id * 2654435761u + 11u)));
		const bool on = Hash(id * 2654435761u ^ (epoch * 2246822519u)) < chance;
		emitter.Strength = on ? emitter.Strength : 0.0;
	}
	if (DEBUG_EMITTERS && slot == 0) {
		static const float2 MARK[6] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1) };
		const float3 at = emitter.Position - FrameBuffer::CameraPosAdjust.xyz;
		const float3 r = normalize(FrameBuffer::CameraView[0].xyz), u = normalize(FrameBuffer::CameraView[1].xyz);
		const float mark = 3.0 + 0.004 * length(at);
		VSOutput marker = (VSOutput)0;
		marker.Position = mul(FrameBuffer::CameraViewProj, float4(at + (r * MARK[vertexID].x + u * MARK[vertexID].y) * mark, 1.0));
		marker.Position.z = 1e-5 * marker.Position.w;  // over everything
		marker.Corner = MARK[vertexID];
		marker.Alpha = 1.0;
		marker.Mist = -1.0 - float(kind);  // -1 crest, -2 foot, -3 strike
		marker.Seed = emitter.Strength > 0.0 ? 1.0 : 0.35;
		return marker;
	}

	// Weaker emitters throw less often
	// D47: the faster the water that throws it, the more spray, thrown harder, bigger and denser: slow
	// water barely throws any, fast water throws it all
	const float fast = saturate(emitter.Speed / FAST_WATER);
	const bool active = !mist || r3 < emitter.Strength * (0.45 + 0.55 * fast);  // D184: every droplet thrown (the user: more splashes)  // D53: slow water throws some too, so spray is not all in a few places

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
		if (kind == 1) {
			// D184: where two sheets meet (the user's green): staying low over the water there, drifting a little on along the water's
			// way and spreading, slowed by the air, fading as it goes
			const float3 ahead = dot(flat, flat) > 1e-4 ? normalize(flat) : spread;
			const float3 v0 = ahead * MEET_DRIFT * (0.5 + 0.7 * r1) + spread * (15.0 + 25.0 * r2) + float3(0, 0, 10.0 + 20.0 * r3);
			const float tau = 1.0;
			position += v0 * tau * (1.0 - exp(-age / tau));
			size = MistSize * MEET_HEAVY * (0.6 + 1.2 * t) * (0.7 + 0.6 * r2);
			alpha = MistOpacity * MEET_HEAVY * smoothstep(0.0, 0.15, t) * (1.0 - smoothstep(0.25, 1.0, t));
		} else {
			// D183: off the struck rock, rising
			const float3 v0 = flat * emitter.Speed * (0.2 + 0.2 * r2) + spread * (30.0 + 30.0 * r2) + float3(0, 0, 20.0 + 30.0 * r1);
			const float tau = 0.8;
			// D182: rising slowly as it drifts (it settled with the falling water's motion, down under the water and the ground, SS 1316)
			position += v0 * tau * (1.0 - exp(-age / tau)) + float3(0, 0, MistRise * 0.15 * age);
			size = MistSize * (0.8 + 1.4 * t) * (0.7 + 0.6 * r2) * 0.7;
			// none past the crests (the user's map)
			alpha = MistOpacity * smoothstep(0.0, 0.3, t) * (1.0 - smoothstep(0.35, 1.0, t)) * (kind == 2 ? STRIKE_MIST : 0.0);
		}
	} else {
		float3 v0;
		if (kind == 0) {
			// D184: where the crest turns down, shot out the way its surface faces, level to barely down, fast (the user's pink)
			float3 shot = float3(up.xy, 0.0);
			shot = dot(shot, shot) > 1e-4 ? normalize(shot) : flat;
			shot = dot(shot, flat) < 0.0 ? flat : normalize(shot + flat);
			// fanned out in a V (the user: not one path): each drop turned across by up to CREST_FAN either way, tipped from a bit
			// up to well down, and thrown from along the crest beside the emitter
			const float3 across = normalize(cross(float3(0, 0, 1), shot));
			const float turn = (r0 * 2.0 - 1.0) * CREST_FAN;
			const float3 fan = shot * cos(turn) + across * sin(turn);
			v0 = normalize(fan + float3(0, 0, lerp(0.3, -0.25, r2))) * emitter.Speed * CREST_LAUNCH * (0.45 + 0.85 * r1);
			position += across * ((Hash(born + 6u) * 2.0 - 1.0) * CREST_WIDTH);
		} else if (kind == 1) {
			// D184: where sheets meet, droplets thrown with the mist (the user): fanned like a crest's, always forward along the
			// water's way, but steeper up
			float3 ahead = dot(flat, flat) > 1e-4 ? normalize(flat) : side;
			const float3 across = normalize(cross(float3(0, 0, 1), ahead));
			const float turn = (r0 * 2.0 - 1.0) * CREST_FAN;
			v0 = normalize(ahead * cos(turn) + across * sin(turn) + float3(0, 0, lerp(0.6, 1.3, r2))) * SpraySpeed * SPRAY_LAUNCH * MEET_SPLASH * (0.5 + 0.7 * r1);
			position += across * ((Hash(born + 6u) * 2.0 - 1.0) * CREST_WIDTH);
		} else {
			// D183: thrown up off the rock, leaning on the way the water ran into it (the user's blue)
			const float3 lift = normalize(float3(0.0, 0.0, 1.0) + flat * STRIKE_LEAN);
			v0 = (lift + spread * 0.3) * SpraySpeed * SPRAY_LAUNCH * (0.45 + 0.55 * r2) * (0.6 + 0.4 * fast);
		}
		position += v0 * age + float3(0, 0, -0.5 * Gravity * age * age);
		motion = v0 + float3(0, 0, -Gravity * age);
		size = SpraySize * DROP_BIG * (0.5 + 1.0 * r2);
		alpha = SprayOpacity * DROP_OPACITY * (0.7 + 0.3 * fast) * smoothstep(0.0, 0.08, t) * (1.0 - smoothstep(0.55, 1.0, t)) * (kind == 1 ? (Hash(seed + 7u) < MEET_DROPS ? 1.0 : 0.0) : 1.0);
		if (kind <= 1)
			alpha *= 1.0 - smoothstep(0.1, 1.0, t);  // D184: a crest's drops thin out as they fly (the user)
		// D182: gone once it falls back below where it was thrown (a plunge's is the water's surface: it fell on under the water
		// and the ground, SS 1316); from a sheet it may fall a little further, fading out
		const float drop = emitter.Position.z - position.z;
		// (one thrown straight up from a water surface, as a plunge's or one lifted onto it, D182, is gone at the surface)
		// D183: past a crest, half go a short arc and half fall the whole drop (the user: a mix)
		const float most = kind == 0 ? (r1 < 0.5 ? CREST_SHORT : CREST_LONG) : (kind == 1 ? 12.0 : SPRAY_MOST_DROP);
		alpha *= 1.0 - smoothstep(0.0, most, drop);
	}

	// D181: only near the camera (the user: every loaded fall's particles cost the GPU): faded out between SPRAY_NEAR_FADE and
	// SPRAY_FAR world units from it, nothing drawn past
	const float cameraDistance = length(emitter.Position - FrameBuffer::CameraPosAdjust.xyz);
	alpha *= 1.0 - smoothstep(SPRAY_NEAR_FADE, SPRAY_FAR, cameraDistance);
	if (mist)
		alpha *= MIST_SHEER;
	// D183: farther off, fewer and bigger (a fixed share of each emitter's particles, by their own hash: none flicker)
	const float keep = lerp(1.0, SPRAY_FAR_KEEP, smoothstep(SPRAY_LOD_FROM, SPRAY_NEAR_FADE, cameraDistance));
	if (Hash(seed + 5u) > keep)
		alpha = 0.0;
	size *= rsqrt(keep);

	VSOutput output;
	if (!active || alpha <= 0.001) {
		// Nothing to draw: a degenerate quad outside the view, so it costs no pixels
		output = (VSOutput)0;
		output.Position = float4(2.0, 2.0, 2.0, 1.0);
		return output;
	}

	// A camera-facing quad; a droplet's is stretched along its motion as seen from the camera
	const float3 relative = position - FrameBuffer::CameraPosAdjust.xyz;
	output.WorldOffset = relative;
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
	if (!mist)
		length2 = max(length2, size * DROP_LONG * lerp(0.6, 1.5, Hash(born + 9u)));  // D184: never round (the user: tear-like), some longer than others
	const float2 corner = CORNERS[vertexID];
	const float wide = mist ? MIST_FLAT : DROP_THIN * lerp(0.6, 1.5, Hash(born + 8u));  // D184: some wider, some thinner
	const float tall = mist ? 1.0 / sqrt(MIST_FLAT) : 1.0;
	const float3 world = relative + right * corner.x * (size * 0.5 * wide) + viewUp * corner.y * (length2 * 0.5 * tall);

	output.Position = mul(FrameBuffer::CameraViewProj, float4(world, 1.0));
	output.Corner = corner;
	output.Right = right;
	output.Up = viewUp;
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
	if (input.Mist < -0.5) {
		if (length(input.Corner) > 1.0)
			discard;
		const float3 colors[3] = { float3(1.0, 0.1, 0.9), float3(0.1, 1.0, 0.2), float3(0.1, 0.3, 1.0) };
		return float4(colors[clamp(int(round(-input.Mist - 1.0)), 0, 2)] * input.Seed, 1.0);
	}
	const float radius = length(input.Corner);
	float shape;
	if (input.Mist > 0.5) {
		// Mist (D36): a soft, uneven wisp, not a disc: a falloff broken up by a few drifting waves
		const float2 c = input.Corner;
		const float wisp = 0.5 + 0.25 * sin(c.x * 2.3 + input.Seed * 19.0) * sin(c.y * 2.9 + input.Seed * 7.0) + 0.25 * sin((c.x + c.y) * 4.1 + input.Seed * 31.0);
		// D181: a soft gaussian puff, not a card: its edge fades long before the quad's (the cards' outlines showed), thinner
		shape = exp(-radius * radius * 3.0) * saturate(wisp * 1.4 - 0.2) * 0.6;
	} else {
		// D184: a tear: a round head ahead (y up the quad is its motion) narrowing to a thin tail behind
		const float width = lerp(0.18, 1.0, smoothstep(-1.0, 0.45, input.Corner.y));
		const float tear = length(float2(input.Corner.x / width, input.Corner.y));
		shape = (1.0 - smoothstep(0.4, 1.0, tear)) * (0.8 + 0.2 * sin(input.Seed * 40.0 + input.Corner.y * 6.0));
	}
	// Soft where it meets anything in front of it, and fading into the water it rose from
	const float sceneDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
	const float depth = SharedData::GetScreenDepth(input.Position.z);
	// D181: mist fades far into what it meets and near the camera (a puff in your face is a flat card)
	// D182: over a short reach (the contact's depth holds the water now, D182, which the particles rise from: over 300 the mist
	// never came clear of it, nor the spray over 6)
	const float soft = saturate((sceneDepth - depth) / (input.Mist > 0.5 ? 40.0 : 2.0)) * saturate(input.Above / (input.Mist > 0.5 ? 20.0 : 4.0)) *
	                   (input.Mist > 0.5 ? smoothstep(60.0, 260.0, depth) : 1.0);
	const float alpha = saturate(input.Alpha * shape * soft);
	if (alpha <= 0.002)
		discard;
	// D182: shaded as a small volume of water in the air (the user: catch the light, have depth): a sphere's normal across the
	// quad, lit by the sun with a soft wrap (its shade side darker), forward scattering toward the sun (mist far more), and the
	// sky's and sun's colour taken in part (in full the sky's blue made it glow blue, D50, and the sun orange at dusk)
	const float3 toCamera = normalize(-input.WorldOffset);
	const float2 c = input.Corner;
	const float r2 = saturate(dot(c, c));
	const float3 n = normalize(input.Right * c.x + input.Up * c.y + toCamera * sqrt(1.0 - r2));
	const float3 sun = SharedData::DirLightDirection.xyz;
	const float3 ambient = max(float3(SharedData::AmbientSHR.x, SharedData::AmbientSHG.x, SharedData::AmbientSHB.x), 0.0) * 0.886;
	const float3 sunColor = SharedData::DirLightColor.rgb;
	const float3 ambientTint = lerp(Color::RGBToLuminance(ambient).xxx, ambient, PARTICLE_COLOR);
	const float3 sunTint = lerp(Color::RGBToLuminance(sunColor).xxx, sunColor, PARTICLE_COLOR);
	const bool isMist = input.Mist > 0.5;
	// Thin mist is lit through, so its shade side barely darkens; a droplet is a lens, lit on its sun side
	const float wrap = isMist ? 0.75 : 0.35;
	const float diffuse = saturate((dot(n, sun) + wrap) / (1.0 + wrap));
	const float g = isMist ? 0.65 : 0.8;  // Henyey-Greenstein: how strongly forward
	const float cosine = dot(-toCamera, sun);
	const float phase = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosine, 1e-3), 1.5) / 12.566;
	// The sun weighs little in the base (it lit the mist in shadow too, D50); its scattering carries the glow toward it
	float3 color = float3(0.94, 0.95, 0.96) * (ambientTint * lerp(0.7, 1.0, n.z * 0.5 + 0.5) + sunTint * (0.12 * diffuse + phase * (isMist ? 0.45 : 0.5)));
	color *= Brightness * (isMist ? 1.2 : 2.2);
	if (isMist)
		color *= lerp(0.85, 1.0, n.z * 0.5 + 0.5);  // D183: its underside a little darker, no glowing heart
	else {
		// D184: a droplet shaded as a small body (the user: like the mist): darker away from the sun and underneath
		color *= lerp(DROP_SHADE, 1.0, diffuse) * lerp(0.75, 1.0, n.z * 0.5 + 0.5);
		color += sunTint * pow(saturate(dot(reflect(-toCamera, n), sun)), 40.0) * 0.6 * Brightness;  // a droplet's glint
	}
	return float4(color * alpha, alpha);  // Premultiplied
}
