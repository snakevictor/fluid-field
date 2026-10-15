#include "WaterMapBuilder.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

#include "BedRaycasts.h"
#include "LandscapeCache.h"
#include "ShallowWaterSolver.h"
#include "WaterSurfaces.h"

namespace FluidFieldSim
{
	namespace
	{
		/** @brief Bed assumed under water where neither landscape nor a ray says otherwise. */
		constexpr float UNKNOWN_BED_DEPTH = 1024.0f;

		/** @brief Spacing of the lattice openness is measured and remembered on, world units. */
		constexpr float OPENNESS_SPACING = 256.0f;
		/** @brief Ring radii openness samples the water on, world units; the outermost is the reach of the measure. */
		constexpr std::array<float, 5> OPENNESS_RADII = { 512.0f, 1024.0f, 2048.0f, 3072.0f, 4096.0f };
		/** @brief Directions sampled on each ring. */
		constexpr uint32_t OPENNESS_DIRECTIONS = 16;
		/** @brief Openness up to which water keeps its full river current: a channel between banks reads about 0.1 to 0.25. */
		constexpr float OPENNESS_FULL_FLOW = 0.3f;
		/** @brief Openness from which water is still: a lake shore reads about 0.5, open water more. */
		constexpr float OPENNESS_STILL = 0.55f;
	}

	uint32_t WaterMapBuilder::ResolutionFor(float a_gridSize)
	{
		const auto needed = static_cast<uint32_t>(std::ceil(a_gridSize / MAP_SPACING));
		return std::clamp(std::bit_ceil(needed), 16u, 512u);
	}

	void WaterMapBuilder::Clear()
	{
		data.clear();
		build.clear();
		openness.clear();
		resolution = 0;
		nextRow = 0;
		builtOrigin[0] = builtOrigin[1] = INT32_MIN;
		builtVersion = 0;
	}

	float WaterMapBuilder::Openness(const LandscapeCache& a_landscape, const WaterSurfaces& a_water, int32_t a_latticeX, int32_t a_latticeY)
	{
		const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(a_latticeX)) << 32) | static_cast<uint32_t>(a_latticeY);
		if (const auto it = openness.find(key); it != openness.end())
			return it->second.value;

		// The fraction of water on rings around the point. A river is a band through the rings, so few
		// of their samples are water; a lake surrounds the point. Samples over unloaded land are skipped.
		const float cx = static_cast<float>(a_latticeX) * OPENNESS_SPACING;
		const float cy = static_cast<float>(a_latticeY) * OPENNESS_SPACING;
		uint32_t known = 0;
		uint32_t wet = 0;
		for (const float radius : OPENNESS_RADII) {
			for (uint32_t d = 0; d < OPENNESS_DIRECTIONS; d++) {
				const float angle = (static_cast<float>(d) + 0.5f) * 6.2831853f / OPENNESS_DIRECTIONS;
				const float x = cx + radius * std::cos(angle);
				const float y = cy + radius * std::sin(angle);
				float ground = 0.0f;
				if (!a_landscape.SampleHeight(x, y, ground))
					continue;
				known++;
				float surface = 0.0f;
				if (a_water.SampleHeight(x, y, surface) != WaterSurfaces::Source::None && surface > ground)
					wet++;
			}
		}
		const uint32_t total = static_cast<uint32_t>(OPENNESS_RADII.size()) * OPENNESS_DIRECTIONS;
		const float result = known > 0 ? static_cast<float>(wet) / static_cast<float>(known) : 0.0f;
		// Points whose surroundings are not yet mostly loaded are measured again on the next rebuild
		openness.emplace(key, OpennessEntry{ result, known * 4 >= total * 3 });
		return result;
	}

	float WaterMapBuilder::CurrentFactor(const LandscapeCache& a_landscape, const WaterSurfaces& a_water, float a_worldX, float a_worldY)
	{
		// Water slows where it spreads out, as a river does when it opens into a lake (D20).
		// Unified Water's flowmap is as strong under Lake Ilinalta as under the rapids, and some lake
		// water types are marked as flowing, so openness is what tells the two apart.
		const float fx = a_worldX / OPENNESS_SPACING;
		const float fy = a_worldY / OPENNESS_SPACING;
		const auto ix = static_cast<int32_t>(std::floor(fx));
		const auto iy = static_cast<int32_t>(std::floor(fy));
		const float tx = fx - static_cast<float>(ix);
		const float ty = fy - static_cast<float>(iy);
		const float o00 = Openness(a_landscape, a_water, ix, iy);
		const float o10 = Openness(a_landscape, a_water, ix + 1, iy);
		const float o01 = Openness(a_landscape, a_water, ix, iy + 1);
		const float o11 = Openness(a_landscape, a_water, ix + 1, iy + 1);
		const float open = (o00 * (1.0f - tx) + o10 * tx) * (1.0f - ty) + (o01 * (1.0f - tx) + o11 * tx) * ty;
		const float t = std::clamp((OPENNESS_STILL - open) / (OPENNESS_STILL - OPENNESS_FULL_FLOW), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	bool WaterMapBuilder::Update(const LandscapeCache& a_landscape, const WaterSurfaces& a_water, const BedRaycasts* a_rays,
		float a_centreX, float a_centreY, uint32_t a_resolution, uint64_t a_inputVersion)
	{
		const uint32_t n = a_resolution;
		if (n != resolution) {
			Clear();
			resolution = n;
		}

		if (nextRow == 0) {
			const int32_t originX = static_cast<int32_t>(std::round(a_centreX / MAP_SPACING)) - static_cast<int32_t>(n / 2);
			const int32_t originY = static_cast<int32_t>(std::round(a_centreY / MAP_SPACING)) - static_cast<int32_t>(n / 2);
			// Nothing changed since the last map: no work
			if (!data.empty() && a_inputVersion == builtVersion && originX == builtOrigin[0] && originY == builtOrigin[1])
				return false;
			build.assign(static_cast<std::size_t>(n) * n * MAP_CHANNELS, 0.0f);
			std::erase_if(openness, [](const auto& a_entry) { return !a_entry.second.settled; });
			originLattice[0] = originX;
			originLattice[1] = originY;
			buildingVersion = a_inputVersion;
		}

		const uint32_t endRow = std::min(n, nextRow + std::max(1u, n / SLICES));
		for (uint32_t row = nextRow; row < endRow; row++) {
			const int32_t ly = originLattice[1] + static_cast<int32_t>(row);
			for (uint32_t column = 0; column < n; column++) {
				const int32_t lx = originLattice[0] + static_cast<int32_t>(column);
				const float wx = static_cast<float>(lx) * MAP_SPACING;
				const float wy = static_cast<float>(ly) * MAP_SPACING;

				float surface = 0.0f;
				float flowScale = 0.0f;
				const bool hasWater = a_water.SampleHeight(wx, wy, surface, flowScale) != WaterSurfaces::Source::None;

				float bed = std::numeric_limits<float>::quiet_NaN();
				if (!a_landscape.SampleHeight(wx, wy, bed))
					bed = std::numeric_limits<float>::quiet_NaN();
				if (const auto* ray = a_rays ? a_rays->Find(wx, wy) : nullptr) {
					if (ray->kind == BedRaycasts::HitKind::Terrain)
						bed = ray->height;
					// Meshes rising out of the water are left to the fine obstacle layer: blended at this
					// spacing, a pillar would spread into a blob over its neighbours
					else if (ray->kind == BedRaycasts::HitKind::Static && !(hasWater && ray->height > surface))
						bed = std::isfinite(bed) ? std::max(bed, ray->height) : ray->height;
				}
				if (!hasWater)
					surface = ShallowWaterSolver::NO_WATER;
				if (!std::isfinite(bed))
					bed = hasWater ? surface - UNKNOWN_BED_DEPTH : 0.0f;

				// Toroidal storage: the texel of lattice point (lx, ly) is (lx mod n, ly mod n)
				const auto tx = static_cast<uint32_t>(lx) & (n - 1);
				const auto ty = static_cast<uint32_t>(ly) & (n - 1);
				const std::size_t index = (static_cast<std::size_t>(ty) * n + tx) * MAP_CHANNELS;
				build[index] = bed;
				build[index + 1] = surface;
				build[index + 2] = hasWater && flowScale > 0.0f ? flowScale * CurrentFactor(a_landscape, a_water, wx, wy) : 0.0f;
			}
		}
		nextRow = endRow;

		if (nextRow < n)
			return false;
		data.swap(build);
		nextRow = 0;
		builtOrigin[0] = originLattice[0];
		builtOrigin[1] = originLattice[1];
		builtVersion = buildingVersion;
		return true;
	}
}
