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
#include "FluidField/FallEffects.hlsli"
#include "FluidField/FallGround.hlsli"

Texture2D<float> SceneDepth : register(t103);
// The water simulation's near level (as the water shader reads it, FluidField/WaterNormal.hlsli): surface
// slopes, height above vanilla and coverage; and flow and foam. Toroidal storage
Texture2D<float4> WaterSurfaceTex : register(t66);
Texture2D<float4> WaterFlowTex : register(t67);

/** Farthest the fog is marched, world units. */
static const float FOG_RANGE = 6000.0;
/** World units across one wisp, and seconds between restarts of each of the wisps' two drifting phases. */
static const float WISP_SIZE = 90.0;
static const float WISP_PERIOD = 1.6;
/** How much longer the wisps are along the flow than across it, at full speed (D42). */
static const float WISP_STRETCH = 3.5;
/** D43: how far the churn warps the wisps (in wisps), how fast it changes, and how fast the mist boils upward (units a second). */
static const float CHURN = 1.6;
static const float CHURN_RATE = 1.1;
static const float BOIL_SPEED = 70.0;
/** How thick vanilla's mist volumes are, as a share of the fog over wholly white water: their bounds are big boxes. */
static const float MIST_VOLUME_SHARE = 0.12;
/** World units the fog round a strike stands above it, and how far up and down it spreads from there (D39). */
static const float SOURCE_LIFT = 30.0;
static const float SOURCE_SPREAD = 70.0;
/** How much thicker the fog round the falls' sources is than over merely white water (D41). */
static const float SOURCE_DENSITY = 8.0;
/** World units round the camera kept clear of fog, fading in over five times as far. */
static const float NEAR_CLEAR = 40.0;

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
	// D43, D44: agitated spray-mist, cheaply: one churning warp, the wisps streaming along the flow in
	// two phases, and a fine detail octave that boils upward
	const float cycle = Time / WISP_PERIOD;
	const float phaseA = frac(cycle);
	const float phaseB = frac(cycle + 0.5);
	const float speed = length(a_flow);
	const float2 along = speed > 1.0 ? a_flow / speed : float2(1.0, 0.0);
	const float2 across = float2(-along.y, along.x);
	const float stretch = lerp(1.0, WISP_STRETCH, saturate(speed / 200.0));
	float3 base = float3(dot(a_position.xy, along) / stretch, dot(a_position.xy, across), a_position.z * 1.5) / WISP_SIZE;
	base.z -= Time * BOIL_SPEED / WISP_SIZE;
	const float warp = Noise3(base * 0.7 + float3(0.0, 0.0, Time * CHURN_RATE)) - 0.5;
	base += float3(warp, -warp * 0.7, warp * 0.5) * CHURN;
	const float drift = speed / stretch * WISP_PERIOD / WISP_SIZE;
	const float a = Noise3(base - float3(drift * phaseA, 0.0, 0.0));
	const float b = Noise3(base + 17.3 - float3(drift * phaseB, 0.0, 0.0));
	const float detail = Noise3(base * 2.6 + float3(0.0, 0.0, -Time * 0.6));
	return lerp(a, b, abs(1.0 - 2.0 * phaseA)) * 0.65 + detail * 0.35;
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

/** The fog's extinction per world unit at a world position; a_parts.y: the seams' share (debug). */
float FogAt(float3 a_position, out float3 a_parts)
{
	// D44: only where vanilla's falling-water meshes collide (their seams), risen a little and churning
	a_parts = 0.0;
	float sourceHeight;
	const float source = FallGround::FogSource(a_position.xy, sourceHeight);
	if (source <= 0.01)
		return 0.0;
	const float lift = (a_position.z - sourceHeight - SOURCE_LIFT) / SOURCE_SPREAD;
	const float around = exp(-lift * lift);
	if (around <= 0.01)
		return 0.0;
	const float2 flow = FallGround::StreamAt(a_position.xy).xy * BaseSpeed;
	const float density = FogDensity * SOURCE_DENSITY * source * around * smoothstep(0.34, 0.74, Wisps(a_position, flow));
	a_parts.y = density;
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

	// D41: only the stretch of the ray inside the fog's height band is marched, so the fog shows from
	// afar without more steps, and the steps stay fine wherever the band is thin along the ray
	const float eye = FrameBuffer::CameraPosAdjust.z;
	float enter = 0.0, leave = range;
	if (abs(direction.z) > 1e-4) {
		const float a = (FogBottom - eye) / direction.z;
		const float b = (FogTop - eye) / direction.z;
		enter = max(enter, min(a, b));
		leave = min(leave, max(a, b));
	} else if (eye < FogBottom || eye > FogTop)
		discard;
	if (leave <= enter)
		discard;

	// Even steps, each jittered by a pixel's own offset so the steps blur instead of banding
	const uint steps = max(FogSteps, 4u);
	const float step = (leave - enter) / float(steps);
	const float jitter = frac(52.9829189 * frac(dot(input.Position.xy, float2(0.06711056, 0.00583715))));
	float transmittance = 1.0;
	float scattered = 0.0;
	float3 partsSeen = 0.0;  // Debug 3: the densest of each part along the ray
	[loop] for (uint s = 0; s < steps; s++)
	{
		const float t = enter + (float(s) + jitter) * step;
		const float3 position = direction * t + FrameBuffer::CameraPosAdjust.xyz;
		// None right at the camera: from inside the layer it would be a wall over the whole view (D40)
		float3 parts;
		const float density = FogAt(position, parts) * smoothstep(NEAR_CLEAR, NEAR_CLEAR * 5.0, t);
		partsSeen = max(partsSeen, parts);
		if (density <= 0.0)
			continue;
		const float absorbed = 1.0 - exp(-density * step);
		scattered += transmittance * absorbed;
		transmittance *= 1.0 - absorbed;
		if (transmittance < 0.02)
			break;
	}
	// Debug 3: what makes the fog along the ray: red the pools, green the sources, blue vanilla's mist
	if (DebugMode == 3)
		return float4(saturate(partsSeen / max(FogDensity, 1e-6)), 0.6);
	const float alpha = 1.0 - transmittance;
	if (alpha <= 0.002)
		discard;
	// White water's mist, lit by the light's brightness (as the spray)
	const float3 ambient = max(float3(SharedData::AmbientSHR.x, SharedData::AmbientSHG.x, SharedData::AmbientSHB.x), 0.0) * 0.886;
	const float light = Color::RGBToLuminance(ambient) + Color::RGBToLuminance(SharedData::DirLightColor.rgb) * 0.08;
	const float3 color = float3(0.92, 0.93, 0.94) * light * Brightness * 1.8;
	return float4(color * scattered, alpha);
}
