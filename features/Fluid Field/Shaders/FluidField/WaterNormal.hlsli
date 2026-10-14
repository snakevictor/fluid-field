// Fluid Field's simulated water on top of the vanilla water normal (D8, water slice step 2).
//
// WaterSurfaceTex is the shallow-water solver's output in toroidal storage: world cell c lives
// in texel c & (GridRes - 1). Each texel holds the surface slopes (world units per unit), the
// height above the vanilla surface, and a coverage that is 0 on dry cells and fades to 0 at the
// grid's edge, so the vanilla normal takes over there.

namespace FluidField
{
	Texture2D<float4> WaterSurfaceTex : register(t66);

	float4 LoadSurface(int2 a_cell)
	{
		const int mask = int(SharedData::fluidFieldSettings.GridRes) - 1;
		return WaterSurfaceTex.Load(int3(a_cell & mask, 0));
	}

	/**
	 * Adds Fluid Field's wave normal to a water normal at an absolute world position.
	 * Returns a_normal unchanged outside the simulated area or where it has no water.
	 */
	float3 ApplyWaterNormal(float3 a_worldPosition, float3 a_normal)
	{
		if (SharedData::fluidFieldSettings.Enabled == 0)
			return a_normal;

		// Texel centres sit at +0.5 cells; bilinear over the four around the position
		const float2 cellPosition = a_worldPosition.xy / SharedData::fluidFieldSettings.CellSize - 0.5;
		const int2 cell = int2(floor(cellPosition));
		const int2 logical = cell - SharedData::fluidFieldSettings.GridMin;
		const int lastCell = int(SharedData::fluidFieldSettings.GridRes) - 1;
		if (any(logical < 0) || any(logical >= lastCell))
			return a_normal;

		const float2 t = cellPosition - float2(cell);
		const float4 surface = lerp(lerp(LoadSurface(cell), LoadSurface(cell + int2(1, 0)), t.x),
			lerp(LoadSurface(cell + int2(0, 1)), LoadSurface(cell + int2(1, 1)), t.x), t.y);
		const float coverage = saturate(surface.w);
		if (coverage <= 0.0)
			return a_normal;

		const float strength = SharedData::fluidFieldSettings.NormalStrength;
		const float3 waveNormal = normalize(float3(-surface.x * strength, -surface.y * strength, 1.0));
		const float3 combined = normalize(ReorientNormal(waveNormal, a_normal));
		return normalize(lerp(a_normal, combined, coverage));
	}
}
