// Fluid Field waterfall fog (D38): the mist that hangs over falling water and boils up where it lands.
// Its own layer, apart from the ground fog: it lies over the water the falls run into and out of, as
// thick as that water is white (the water simulation's foam, which the plunges churn up and the flow
// carries off, and the falls' own foam where they strike rock, meet still water or vanilla's pieces
// meet), taller where it is thicker, and broken into wisps drifting with the flow. Vanilla's own
// waterfall mist effects add fog where they stand. Each pixel marches its view ray to the scene's
// depth through it and blends the fog in, drawn after the falls' spray with the same constants.

#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "FluidField/FallGround.hlsli"

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
	float FogDensity;  // Extinction per world unit where the water is wholly white
	float FogHeight;   // World units the fog stands above the water where it is thickest
	uint FogSteps;     // Samples along each ray
	uint MistVolumeCount;
};

Texture2D<float> SceneDepth : register(t103);
// The water simulation's near level (as the water shader reads it, FluidField/WaterNormal.hlsli): surface
// slopes, height above vanilla and coverage; and flow and foam. Toroidal storage
Texture2D<float4> WaterSurfaceTex : register(t66);
Texture2D<float4> WaterFlowTex : register(t67);
StructuredBuffer<float4> MistVolumes : register(t104);  // Per volume: centre and strength, then half extents

/** Farthest the fog is marched, world units. */
static const float FOG_RANGE = 3000.0;
/** World units across one wisp, and seconds between restarts of each of the wisps' two drifting phases. */
static const float WISP_SIZE = 90.0;
static const float WISP_PERIOD = 3.0;
/** How thick vanilla's mist volumes are, as a share of the fog over wholly white water: their bounds are big boxes. */
static const float MIST_VOLUME_SHARE = 0.12;
/** World units the fog round a strike stands above it, and how far up and down it spreads from there (D39). */
static const float SOURCE_LIFT = 20.0;
static const float SOURCE_SPREAD = 45.0;

float Hash3(float3 a_p)
{
	a_p = frac(a_p * 0.3183099 + 0.1);
	a_p *= 17.0;
	return frac(a_p.x * a_p.y * a_p.z * (a_p.x + a_p.y + a_p.z));
}

/** Smooth value noise, 0 to 1. */
float Noise3(float3 a_p)
{
	const float3 i = floor(a_p);
	const float3 f = a_p - i;
	const float3 u = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(lerp(Hash3(i), Hash3(i + float3(1, 0, 0)), u.x), lerp(Hash3(i + float3(0, 1, 0)), Hash3(i + float3(1, 1, 0)), u.x), u.y),
		lerp(lerp(Hash3(i + float3(0, 0, 1)), Hash3(i + float3(1, 0, 1)), u.x), lerp(Hash3(i + float3(0, 1, 1)), Hash3(i + float3(1, 1, 1)), u.x), u.y), u.z);
}

/** Wisps drifting with the water's flow, in two phases that restart before they shear apart. */
float Wisps(float3 a_position, float2 a_flow)
{
	const float cycle = Time / WISP_PERIOD;
	const float phaseA = frac(cycle);
	const float phaseB = frac(cycle + 0.5);
	const float3 base = float3(a_position.xy, a_position.z * 2.0) / WISP_SIZE;
	const float2 drift = a_flow * WISP_PERIOD / WISP_SIZE;
	const float a = Noise3(base - float3(drift * phaseA, Time * 0.05)) * 0.65 + Noise3(base * 2.3 - float3(drift * phaseA * 2.3, Time * 0.08)) * 0.35;
	const float b = Noise3(base + 17.3 - float3(drift * phaseB, Time * 0.05)) * 0.65 + Noise3(base * 2.3 + 5.1 - float3(drift * phaseB * 2.3, Time * 0.08)) * 0.35;
	return lerp(a, b, abs(1.0 - 2.0 * phaseA));
}

/** The simulated water's surface (x, y slopes, z height above vanilla, w coverage) and its flow and foam (x, y flow, z foam) at a world position; false outside it. */
bool WaterAt(float2 a_position, out float4 a_surface, out float4 a_flowFoam)
{
	a_surface = 0.0;
	a_flowFoam = 0.0;
	const int res = int(SharedData::fluidFieldSettings.GridRes);
	const float cellSize = SharedData::fluidFieldSettings.CellSize;
	if (SharedData::fluidFieldSettings.Enabled == 0 || res <= 1 || cellSize <= 0.0)
		return false;
	const float2 cell = a_position / cellSize - 0.5;
	const int2 logical = int2(floor(cell)) - SharedData::fluidFieldSettings.GridMin;
	if (any(logical < 0) || any(logical >= res - 1))
		return false;
	const int mask = res - 1;
	const int2 c = int2(floor(cell));
	const float2 f = cell - float2(c);
	a_surface = lerp(lerp(WaterSurfaceTex.Load(int3(c & mask, 0)), WaterSurfaceTex.Load(int3((c + int2(1, 0)) & mask, 0)), f.x),
		lerp(WaterSurfaceTex.Load(int3((c + int2(0, 1)) & mask, 0)), WaterSurfaceTex.Load(int3((c + int2(1, 1)) & mask, 0)), f.x), f.y);
	a_flowFoam = lerp(lerp(WaterFlowTex.Load(int3(c & mask, 0)), WaterFlowTex.Load(int3((c + int2(1, 0)) & mask, 0)), f.x),
		lerp(WaterFlowTex.Load(int3((c + int2(0, 1)) & mask, 0)), WaterFlowTex.Load(int3((c + int2(1, 1)) & mask, 0)), f.x), f.y);
	const float fade = SharedData::fluidFieldSettings.Fade;
	a_surface *= fade;
	a_flowFoam *= fade;
	return true;
}

/** The fog's extinction per world unit at a world position. */
float FogAt(float3 a_position)
{
	float density = 0.0;
	// Over the water the falls run into and out of, as white as it is
	float pool;
	if (FallGround::Pool(a_position.xy, pool)) {
		float4 surface, flowFoam;
		WaterAt(a_position.xy, surface, flowFoam);
		const float level = pool + surface.z;
		const float fallFoam = FallGround::StreamAt(a_position.xy).z;
		const float white = saturate(max(flowFoam.z * 1.5, fallFoam));
		const float top = FogHeight * (0.35 + 0.65 * white);
		const float above = a_position.z - level;
		if (white > 0.01 && above > 0.0 && above < top) {
			const float wisps = Wisps(a_position, flowFoam.xy);
			density = FogDensity * white * smoothstep(0.2, 0.75, wisps) * (1.0 - smoothstep(0.35, 1.0, above / top));
		}
	}
	// Where the falls' water strikes rock or other water, drops and plunges (D39): fog round that height,
	// risen a little
	float sourceHeight;
	const float source = FallGround::FogSource(a_position.xy, sourceHeight);
	if (source > 0.01) {
		const float lift = (a_position.z - sourceHeight - SOURCE_LIFT) / SOURCE_SPREAD;
		const float around = exp(-lift * lift);
		if (around > 0.01)
			density += FogDensity * source * around * smoothstep(0.25, 0.75, Wisps(a_position, 0.0));
	}
	// Where vanilla's waterfall mist effects stand
	for (uint i = 0; i < MistVolumeCount; i++) {
		const float4 centre = MistVolumes[i * 2];
		const float3 extent = max(MistVolumes[i * 2 + 1].xyz, 1.0);
		const float3 d = (a_position - centre.xyz) / extent;
		const float inside = 1.0 - smoothstep(0.3, 1.0, length(d));
		if (inside > 0.0)
			density += FogDensity * MIST_VOLUME_SHARE * centre.w * inside * smoothstep(0.35, 0.8, Wisps(a_position, 0.0));
	}
	return density;
}

struct VSOutput
{
	float4 Position: SV_POSITION;
};

VSOutput VSMain(uint vertexID : SV_VertexID)
{
	// One triangle over the whole screen, at the near plane so the depth test keeps all of it
	VSOutput output;
	const float2 corner = float2((vertexID << 1) & 2, vertexID & 2);
	output.Position = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
	if (FogDensity <= 0.0)
		discard;
	// The view ray, camera relative, to the scene behind the pixel
	uint2 size;
	SceneDepth.GetDimensions(size.x, size.y);
	const float depth = SceneDepth.Load(int3(input.Position.xy, 0));
	const float2 ndc = float2(input.Position.x / size.x * 2.0 - 1.0, 1.0 - input.Position.y / size.y * 2.0);
	const float4 far = mul(FrameBuffer::CameraViewProjInverse, float4(ndc, depth, 1.0));
	const float3 scene = far.xyz / far.w;
	const float sceneDistance = length(scene);
	const float3 direction = scene / max(sceneDistance, 1e-3);
	const float range = min(sceneDistance, FOG_RANGE);

	// Even steps, each jittered by a pixel's own offset so the steps blur instead of banding
	const uint steps = max(FogSteps, 4u);
	const float step = range / float(steps);
	const float jitter = frac(52.9829189 * frac(dot(input.Position.xy, float2(0.06711056, 0.00583715))));
	float transmittance = 1.0;
	float scattered = 0.0;
	[loop] for (uint s = 0; s < steps; s++)
	{
		const float t = (float(s) + jitter) * step;
		const float3 position = direction * t + FrameBuffer::CameraPosAdjust.xyz;
		const float density = FogAt(position);
		if (density <= 0.0)
			continue;
		const float absorbed = 1.0 - exp(-density * step);
		scattered += transmittance * absorbed;
		transmittance *= 1.0 - absorbed;
		if (transmittance < 0.02)
			break;
	}
	const float alpha = 1.0 - transmittance;
	if (alpha <= 0.002)
		discard;
	// White water's mist, lit by the light's brightness (as the spray)
	const float3 ambient = max(float3(SharedData::AmbientSHR.x, SharedData::AmbientSHG.x, SharedData::AmbientSHB.x), 0.0) * 0.886;
	const float light = Color::RGBToLuminance(ambient) + Color::RGBToLuminance(SharedData::DirLightColor.rgb) * 0.15;
	const float3 color = float3(0.93, 0.96, 1.0) * light * Brightness * 1.8;
	return float4(color * scattered, alpha);
}
