#include "WaterMapBuilder.h"

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
	}

	bool WaterMapBuilder::Update(const LandscapeCache& a_landscape, const WaterSurfaces& a_water, const BedRaycasts* a_rays,
		float a_centreX, float a_centreY, uint32_t a_resolution)
	{
		const uint32_t n = a_resolution;
		if (n != resolution) {
			Clear();
			resolution = n;
		}

		if (nextRow == 0) {
			build.assign(static_cast<std::size_t>(n) * n * 2, 0.0f);
			originLattice[0] = static_cast<int32_t>(std::round(a_centreX / MAP_SPACING)) - static_cast<int32_t>(n / 2);
			originLattice[1] = static_cast<int32_t>(std::round(a_centreY / MAP_SPACING)) - static_cast<int32_t>(n / 2);
		}

		const uint32_t endRow = std::min(n, nextRow + std::max(1u, n / SLICES));
		for (uint32_t row = nextRow; row < endRow; row++) {
			const int32_t ly = originLattice[1] + static_cast<int32_t>(row);
			for (uint32_t column = 0; column < n; column++) {
				const int32_t lx = originLattice[0] + static_cast<int32_t>(column);
				const float wx = static_cast<float>(lx) * MAP_SPACING;
				const float wy = static_cast<float>(ly) * MAP_SPACING;

				float bed = std::numeric_limits<float>::quiet_NaN();
				if (!a_landscape.SampleHeight(wx, wy, bed))
					bed = std::numeric_limits<float>::quiet_NaN();
				if (const auto* ray = a_rays ? a_rays->Find(wx, wy) : nullptr) {
					if (ray->kind == BedRaycasts::HitKind::Terrain)
						bed = ray->height;
					else if (ray->kind == BedRaycasts::HitKind::Static)
						bed = std::isfinite(bed) ? std::max(bed, ray->height) : ray->height;
				}

				float surface = 0.0f;
				const bool hasWater = a_water.SampleHeight(wx, wy, surface) != WaterSurfaces::Source::None;
				if (!hasWater)
					surface = ShallowWaterSolver::NO_WATER;
				if (!std::isfinite(bed))
					bed = hasWater ? surface - UNKNOWN_BED_DEPTH : 0.0f;

				// Toroidal storage: the texel of lattice point (lx, ly) is (lx mod n, ly mod n)
				const auto tx = static_cast<uint32_t>(lx) & (n - 1);
				const auto ty = static_cast<uint32_t>(ly) & (n - 1);
				const std::size_t index = (static_cast<std::size_t>(ty) * n + tx) * 2;
				build[index] = bed;
				build[index + 1] = surface;
			}
		}
		nextRow = endRow;

		if (nextRow < n)
			return false;
		data.swap(build);
		nextRow = 0;
		return true;
	}
}
