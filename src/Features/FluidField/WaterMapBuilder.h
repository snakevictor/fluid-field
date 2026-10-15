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
	 * A world-aligned lattice every MAP_SPACING units around the player, stored toroidally like
	 * the simulation grid: lattice point (x, y) lives at texel (x mod res, y mod res). Each rebuild
	 * samples the depth map (landscape, bed raycasts, water surfaces) over a few calls, then the
	 * complete map is ready to upload.
	 */
	class WaterMapBuilder
	{
	public:
		/** @brief Floats per map texel: bed height, vanilla surface (NO_WATER where none), river flow scale, free water (0 here; the waterfall map's, D27). */
		static constexpr uint32_t MAP_CHANNELS = 4;
		static constexpr float MAP_SPACING = 64.0f;  ///< Matches the default bed ray spacing
		static constexpr uint32_t SLICES = 8;        ///< Calls one rebuild is spread over

		/** @brief Map texels per side for a grid of the given world size: a power of two covering it. */
		static uint32_t ResolutionFor(float a_gridSize);

		/**
		 * @brief Advances the current rebuild by one slice. A new rebuild only starts when the inputs
		 * changed (a_inputVersion) or the player moved to another lattice point.
		 * @return True when a rebuild just completed and GetData() holds a new map.
		 */
		bool Update(const LandscapeCache& a_landscape, const WaterSurfaces& a_water, const BedRaycasts* a_rays,
			float a_centreX, float a_centreY, uint32_t a_resolution, uint64_t a_inputVersion);

		void Clear();

		/** @brief (bed, surface) pairs in storage order; surface is ShallowWaterSolver::NO_WATER where there is none. */
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
