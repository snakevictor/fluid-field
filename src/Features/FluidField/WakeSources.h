#pragma once

#include "ShallowWaterSolver.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace FluidFieldSim
{
	class WaterSurfaces;

	/**
	 * @brief D17: water emitters from actors in the water.
	 *
	 * High-process actors moving through water with their bound's top above it make a wake sized from the bound (fully
	 * submerged ones make none). Feet crossing the surface push the water down by vertical speed (less when leaving).
	 * Nearest actors first, up to the solver's limit. Main thread.
	 */
	class WakeSources
	{
	public:
		struct Settings
		{
			float wakeStrength = 1.0f;    ///< Scales the velocity a wake pulls the water toward
			float splashStrength = 1.0f;  ///< Scales the push of an actor entering or leaving the water
		};

		/**
		 * @brief Builds this step's emitters in the grid's logical texel coordinates.
		 * @param a_deltaTime Seconds since the last call; positions from longer ago are not velocities.
		 */
		const std::vector<WaterEmitter>& Update(const WaterSurfaces& a_water, const RE::NiPoint3& a_playerPos, const int32_t a_gridMin[2],
			float a_cellSize, uint32_t a_gridRes, float a_deltaTime, const Settings& a_settings);

		/** @brief Forgets every actor's history, after a reset or teleport. */
		void Clear();

	private:
		struct History
		{
			RE::NiPoint3 position;
			uint32_t lastSeen = 0;  ///< Update counter when last seen
		};

		/** @brief Updates between sightings after which an actor's history is dropped. */
		static constexpr uint32_t FORGET_AFTER = 120;

		std::unordered_map<uint32_t, History> history;  ///< Keyed by actor form ID
		std::vector<WaterEmitter> emitters;
		uint32_t updateCounter = 0;
	};
}
