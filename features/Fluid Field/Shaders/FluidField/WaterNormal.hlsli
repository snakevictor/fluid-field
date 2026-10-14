// Fluid Field's simulated water on top of the vanilla water normal (D8, water slice step 2).
//
// WaterSurfaceTex is the shallow-water solver's output in toroidal storage: world cell c lives
// in texel c & (GridRes - 1). Each texel holds the surface slopes (world units per unit), the
// height above the vanilla surface, and a coverage that is 0 on dry cells and fades to 0 at the
// grid's edge, so the vanilla normal takes over there.

namespace FluidField
{
	Texture2D<float4> WaterSurfaceTex : register(t66);
	Texture2D<float2> WaterFlowTex : register(t67);

	/** World units one tile of the flow-carried detail normals covers. */
	static const float FLOW_DETAIL_TILE = 384.0;
	/** Seconds between restarts of each of the two detail phases. */
	static const float FLOW_DETAIL_PERIOD = 1.0;
	/** Flow speed (world units per second) at which the detail reaches full strength. */
	static const float FLOW_DETAIL_FULL_SPEED = 160.0;

	float4 LoadSurface(int2 a_cell)
	{
		const int mask = int(SharedData::fluidFieldSettings.GridRes) - 1;
		return WaterSurfaceTex.Load(int3(a_cell & mask, 0));
	}

	float2 LoadFlow(int2 a_cell)
	{
		const int mask = int(SharedData::fluidFieldSettings.GridRes) - 1;
		return WaterFlowTex.Load(int3(a_cell & mask, 0));
	}

	/** The simulated flow at an absolute world position, world units per second (bilinear). */
	float2 SampleFlowAt(float2 a_cellPosition)
	{
		const int2 cell = int2(floor(a_cellPosition));
		const float2 t = a_cellPosition - float2(cell);
		return lerp(lerp(LoadFlow(cell), LoadFlow(cell + int2(1, 0)), t.x),
			lerp(LoadFlow(cell + int2(0, 1)), LoadFlow(cell + int2(1, 1)), t.x), t.y);
	}

	/**
	 * The simulated flow, smoothed over about nine cells, for dragging detail along: the raw flow
	 * twists the texture in tight eddies, which reads as an oil film rather than water.
	 */
	float2 SampleFlow(float3 a_worldPosition)
	{
		const float2 p = a_worldPosition.xy / SharedData::fluidFieldSettings.CellSize - 0.5;
		float2 sum = SampleFlowAt(p);
		[unroll] for (int i = 0; i < 8; i++)
		{
			float angle = i * 0.785398;
			sum += SampleFlowAt(p + 4.0 * float2(cos(angle), sin(angle)));
		}
		return sum / 9.0;
	}

	/** Extra vanilla ripple amplitude per unit of shear (1/s) in the simulated flow. */
	static const float ROUGHNESS_PER_SHEAR = 0.6;

	/**
	 * How fast the simulated flow changes across a cell (1/s): high where water is forced around
	 * a pillar or rock, which is where real water breaks up instead of flowing smoothly.
	 */
	float FlowShear(float3 a_worldPosition)
	{
		const float2 p = a_worldPosition.xy / SharedData::fluidFieldSettings.CellSize - 0.5;
		const float2 dx = SampleFlowAt(p + float2(1, 0)) - SampleFlowAt(p - float2(1, 0));
		const float2 dy = SampleFlowAt(p + float2(0, 1)) - SampleFlowAt(p - float2(0, 1));
		return (length(dx) + length(dy)) / (2.0 * SharedData::fluidFieldSettings.CellSize);
	}

	/**
	 * Detail normals carried along by the simulated flow (the classic two-phase flow map): two
	 * samples of the vanilla normal texture, each dragged along the flow and restarted half a period
	 * apart, cross-faded so neither restart shows. Turbulence, eddies and the speed-up between
	 * obstacles become visible across the whole simulated surface.
	 */
	float3 FlowDetailNormal(float3 a_worldPosition, float2 a_flow)
	{
		const float time = SharedData::Timer / FLOW_DETAIL_PERIOD;
		const float phase0 = frac(time);
		const float phase1 = frac(time + 0.5);
		const float2 uv = a_worldPosition.xy / FLOW_DETAIL_TILE;
		const float2 drift = a_flow * FLOW_DETAIL_PERIOD / FLOW_DETAIL_TILE;
		const float3 n0 = Normals01Tex.SampleBias(Normals01Sampler, uv - drift * phase0, SharedData::MipBias).xyz * 2.0 - 1.0;
		const float3 n1 = Normals01Tex.SampleBias(Normals01Sampler, uv - drift * phase1 + 0.5, SharedData::MipBias).xyz * 2.0 - 1.0;
		const float w = abs(phase0 * 2.0 - 1.0);  // 1 when phase 0 restarts, so it is hidden then
		return normalize(lerp(n0, n1, w));
	}

	/**
	 * The simulated surface at an absolute world position, bilinear between texel centres.
	 * Returns false outside the simulated area; then a_surface is zero (no coverage).
	 */
	bool SampleSurface(float3 a_worldPosition, out float4 a_surface)
	{
		a_surface = 0;
		if (SharedData::fluidFieldSettings.Enabled == 0)
			return false;

		const float2 cellPosition = a_worldPosition.xy / SharedData::fluidFieldSettings.CellSize - 0.5;
		const int2 cell = int2(floor(cellPosition));
		const int2 logical = cell - SharedData::fluidFieldSettings.GridMin;
		const int lastCell = int(SharedData::fluidFieldSettings.GridRes) - 1;
		if (any(logical < 0) || any(logical >= lastCell))
			return false;

		const float2 t = cellPosition - float2(cell);
		a_surface = lerp(lerp(LoadSurface(cell), LoadSurface(cell + int2(1, 0)), t.x),
			lerp(LoadSurface(cell + int2(0, 1)), LoadSurface(cell + int2(1, 1)), t.x), t.y);
		return true;
	}

	/** How much Fluid Field simulates the water here: 0 outside its grid, on dry cells and at the grid edge. */
	float GetCoverage(float3 a_worldPosition)
	{
		float4 surface;
		return SampleSurface(a_worldPosition, surface) ? saturate(surface.w) : 0.0;
	}

	/** World units vanilla's normal lookups shift per unit of simulated surface slope. */
	static const float WARP_PER_SLOPE = 48.0;
	/** Extra vanilla ripple amplitude per unit of simulated slope (capped at double). */
	static const float ROUGHNESS_PER_SLOPE = 3.0;

	/**
	 * How the simulated surface reshapes vanilla's own normals at a world position (D17): a shift
	 * of vanilla's normal-texture lookups (world units) toward the downslope, so its ripple pattern
	 * bends and compresses around wakes, eddies and standing waves, and an extra roughness where
	 * the surface is steep. Both fade with coverage, so vanilla is untouched outside the grid.
	 */
	void GetVanillaWarp(float3 a_worldPosition, out float2 a_warp, out float a_roughness)
	{
		a_warp = 0;
		a_roughness = 0;
		float4 surface;
		if (!SampleSurface(a_worldPosition, surface))
			return;
		const float coverage = saturate(surface.w);
		a_warp = -surface.xy * WARP_PER_SLOPE * coverage;
		// Rougher where the surface is steep, and where the flow shears around obstacles (up to x2.5)
		a_roughness = min(saturate(length(surface.xy) * ROUGHNESS_PER_SLOPE) + saturate(FlowShear(a_worldPosition) * ROUGHNESS_PER_SHEAR), 1.5) * coverage;
	}

	/**
	 * Adds Fluid Field's wave normal to a water normal at an absolute world position.
	 * Returns a_normal unchanged outside the simulated area or where it has no water.
	 */
	float3 ApplyWaterNormal(float3 a_worldPosition, float3 a_normal)
	{
		float4 surface;
		if (!SampleSurface(a_worldPosition, surface))
			return a_normal;
		const float coverage = saturate(surface.w);
		if (coverage <= 0.0)
			return a_normal;

		const float strength = SharedData::fluidFieldSettings.NormalStrength;
		const float3 waveNormal = normalize(float3(-surface.x * strength, -surface.y * strength, 1.0));
		float3 combined = normalize(ReorientNormal(waveNormal, a_normal));

		// Surface detail carried by the simulated flow, stronger where the water moves faster
		const float flowDetail = SharedData::fluidFieldSettings.FlowDetail;
		if (flowDetail > 0.0) {
			const float2 flow = SampleFlow(a_worldPosition);
			// Slow water keeps some detail, and the ramp to full is wide, so flow rates blend
			const float amount = flowDetail * lerp(0.4, 1.0, smoothstep(0.0, FLOW_DETAIL_FULL_SPEED * 1.5, length(flow)));
			if (amount > 0.0) {
				const float3 detail = FlowDetailNormal(a_worldPosition, flow);
				const float3 detailNormal = normalize(float3(detail.xy * amount, 1.0));
				combined = normalize(ReorientNormal(detailNormal, combined));
			}
		}
		return normalize(lerp(a_normal, combined, coverage));
	}
}
