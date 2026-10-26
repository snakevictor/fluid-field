#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace FluidFieldSim
{
	class BedRaycasts;
	class LandscapeCache;
	class WaterSurfaces;

	/**
	 * @brief Builds the shallow-water solver's bed and vanilla-surface map on the CPU.
	 *
	 * A world-aligned lattice every MAP_SPACING units, toroidal like the sim grid (point (x, y) at texel (x mod res, y mod res)).
	 * Each rebuild samples landscape, bed rays and water surfaces over SLICES calls.
	 */
	class WaterMapBuilder
	{
	public:
		/** @brief Floats per map texel: bed height, vanilla surface (NO_WATER where none), river flow scale, unused. */
		static constexpr uint32_t MAP_CHANNELS = 4;
		static constexpr float MAP_SPACING = 64.0f;  ///< Matches the default bed ray spacing
		static constexpr uint32_t SLICES = 8;        ///< Calls one rebuild is spread over

		/** @brief Map texels per side for a grid of the given world size: a power of two covering it. */
		static uint32_t ResolutionFor(float a_gridSize);

		/**
		 * @brief Advances the rebuild by one slice; a new one starts only when a_inputVersion or the centre lattice point changed.
		 * @return True when a rebuild just completed and GetData() holds a new map.
		 */
		bool Update(const LandscapeCache& a_landscape, const WaterSurfaces& a_water, const BedRaycasts* a_rays,
			float a_centreX, float a_centreY, uint32_t a_resolution, uint64_t a_inputVersion);

		void Clear();

		/** @brief MAP_CHANNELS floats per texel in storage order; surface is ShallowWaterSolver::NO_WATER where there is none. */
		std::span<const float> GetData() const { return data; }

	private:
		std::vector<float> data;   ///< Last completed map
		std::vector<float> build;  ///< Map being built
		uint32_t resolution = 0;
		uint32_t nextRow = 0;
		int32_t originLattice[2]{};                      ///< South-west lattice point of the rebuild in progress
		int32_t builtOrigin[2]{ INT32_MIN, INT32_MIN };  ///< Origin of the last completed map
		uint64_t builtVersion = 0;                       ///< Input version of the last completed map
		uint64_t buildingVersion = 0;
	};
}
