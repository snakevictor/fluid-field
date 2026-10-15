#include "WaterMapBuilder.h"

#include <algorithm>
#include <bit>

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

		/** @brief Half-width of the square over which a texel's openness (how much of it is water) is measured. */
		constexpr float OPENNESS_RADIUS = 2048.0f;
		/** @brief Openness up to which water keeps its full river current: a channel between banks. */
		constexpr float OPENNESS_FULL_FLOW = 0.3f;
		/** @brief Openness from which water is still: a lake or the sea, spreading in every direction. */
		constexpr float OPENNESS_STILL = 0.6f;
	}

	void WaterMapBuilder::ApplyWidthFactor(std::vector<float>& a_map, uint32_t a_resolution, const int32_t a_origin[2])
	{
		// Water slows where it spreads out, as a river does when it opens into a lake: the current
		// fades with the fraction of water around each texel. Unified Water's flowmap is as strong
		// under Lake Ilinalta as under the rapids, and some lake water types are marked as flowing,
		// so this is what tells the two apart. Summed-area table over the lattice, counting only
		// texels inside the map.
		const uint32_t n = a_resolution;
		const uint32_t mask = n - 1;
		const auto texel = [&](uint32_t a_column, uint32_t a_row) {
			const auto tx = static_cast<uint32_t>(a_origin[0] + static_cast<int32_t>(a_column)) & mask;
			const auto ty = static_cast<uint32_t>(a_origin[1] + static_cast<int32_t>(a_row)) & mask;
			return (static_cast<std::size_t>(ty) * n + tx) * MAP_CHANNELS;
		};
		std::vector<uint32_t> wet(static_cast<std::size_t>(n + 1) * (n + 1), 0);
		for (uint32_t row = 0; row < n; row++) {
			uint32_t rowSum = 0;
			for (uint32_t column = 0; column < n; column++) {
				const std::size_t index = texel(column, row);
				const float surface = a_map[index + 1];
				rowSum += (surface != ShallowWaterSolver::NO_WATER && surface > a_map[index]) ? 1u : 0u;
				wet[static_cast<std::size_t>(row + 1) * (n + 1) + column + 1] = wet[static_cast<std::size_t>(row) * (n + 1) + column + 1] + rowSum;
			}
		}
		const auto radius = static_cast<int32_t>(OPENNESS_RADIUS / MAP_SPACING);
		for (uint32_t row = 0; row < n; row++) {
			const auto r0 = static_cast<uint32_t>(std::max(0, static_cast<int32_t>(row) - radius));
			const auto r1 = static_cast<uint32_t>(std::min(static_cast<int32_t>(n), static_cast<int32_t>(row) + radius + 1));
			for (uint32_t column = 0; column < n; column++) {
				const std::size_t index = texel(column, row);
				if (a_map[index + 2] <= 0.0f)
					continue;
				const auto c0 = static_cast<uint32_t>(std::max(0, static_cast<int32_t>(column) - radius));
				const auto c1 = static_cast<uint32_t>(std::min(static_cast<int32_t>(n), static_cast<int32_t>(column) + radius + 1));
				const auto at = [&](uint32_t a_r, uint32_t a_c) { return wet[static_cast<std::size_t>(a_r) * (n + 1) + a_c]; };
				const uint32_t count = at(r1, c1) - at(r0, c1) - at(r1, c0) + at(r0, c0);
				const float openness = static_cast<float>(count) / static_cast<float>((r1 - r0) * (c1 - c0));
				const float t = std::clamp((OPENNESS_STILL - openness) / (OPENNESS_STILL - OPENNESS_FULL_FLOW), 0.0f, 1.0f);
				a_map[index + 2] *= t * t * (3.0f - 2.0f * t);
			}
		}
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
		resolution = 0;
		nextRow = 0;
		builtOrigin[0] = builtOrigin[1] = INT32_MIN;
		builtVersion = 0;
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
				build[index + 2] = hasWater ? flowScale : 0.0f;
			}
		}
		nextRow = endRow;

		if (nextRow < n)
			return false;
		ApplyWidthFactor(build, n, originLattice);
		data.swap(build);
		nextRow = 0;
		builtOrigin[0] = originLattice[0];
		builtOrigin[1] = originLattice[1];
		builtVersion = buildingVersion;
		return true;
	}
}
