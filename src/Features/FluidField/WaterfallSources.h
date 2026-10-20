#pragma once

#include "ShallowWaterSolver.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace FluidFieldSim
{
	class WaterSurfaces;

	/**
	 * @brief Waterfalls that drive the simulated water where they land (D22).
	 *
	 * Vanilla's falling water stays as it is: its body meshes (FXWaterfallBody*, FXWaterfallThin*)
	 * are found among the loaded references, and each one's plunge (where its lowest vertices are),
	 * width and fall direction are read once from its geometry, with its bounds as a fallback. Bodies
	 * landing on water are merged into falls, and each fall becomes a water emitter: a pulsing push
	 * at the plunge, a current spreading out from it and running on downstream, and foam. Runs on the
	 * main thread.
	 */
	class WaterfallSources
	{
	public:
		/** @brief A waterfall landing on water. */
		struct Fall
		{
			float plunge[3]{};     ///< Where it meets the water, world units (z is the water surface)
			float width = 0.0f;    ///< Width of the falling water at the plunge, world units
			float direction[2]{};  ///< Horizontal direction it falls toward (unit length), or zero
		};

		/** @brief Rescans the loaded waterfalls around a point every few updates. */
		void Update(const WaterSurfaces& a_water, float a_centreX, float a_centreY, float a_halfSize);

		/**
		 * @brief This step's emitters for one water level, in its logical texel coordinates.
		 * @param a_strength Scales every effect (0: none).
		 */
		void AppendEmitters(std::vector<WaterEmitter>& a_emitters, const int32_t a_gridMin[2], float a_cellSize, uint32_t a_gridRes, float a_strength,
			uint32_t a_maxEmitters, bool a_foam) const;

		void Clear();

		const std::vector<Fall>& GetFalls() const { return falls; }

	private:
		/** @brief A body's shape, read once while its 3D is loaded. */
		struct Body
		{
			float plunge[3]{};
			float width = 0.0f;
			float direction[2]{};
			float lowest = 0.0f;  ///< Lowest point of the body, world units
		};

		/** @brief Updates between scans of the loaded references. */
		static constexpr uint32_t SCAN_INTERVAL = 60;
		/** @brief Most falls kept, nearest first. */
		static constexpr std::size_t MAX_FALLS = 6;

		static bool ReadBody(RE::TESObjectREFR& a_ref, Body& a_body);

		std::unordered_map<uint32_t, Body> bodies;  ///< Keyed by form ID
		std::vector<Fall> falls;
		uint32_t updatesUntilScan = 0;
	};
}
