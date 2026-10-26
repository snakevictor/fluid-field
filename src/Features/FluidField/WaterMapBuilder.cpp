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
		/** @brief Bed depth assumed under water where neither landscape nor a ray gives one. */
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
					// Meshes rising out of the water are left to the fine obstacle layer: at this spacing a pillar would blur into a blob
					else if (ray->kind == BedRaycasts::HitKind::Static && !(hasWater && ray->height > surface))
						bed = std::isfinite(bed) ? std::max(bed, ray->height) : ray->height;
				}
				if (!hasWater)
					surface = ShallowWaterSolver::NO_WATER;
				if (!std::isfinite(bed))
					bed = hasWater ? surface - UNKNOWN_BED_DEPTH : 0.0f;

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
		data.swap(build);
		nextRow = 0;
		builtOrigin[0] = originLattice[0];
		builtOrigin[1] = originLattice[1];
		builtVersion = buildingVersion;
		return true;
	}
}
