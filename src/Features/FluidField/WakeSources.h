#pragma once

#include "ShallowWaterSolver.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace FluidFieldSim
{
	class WaterSurfaces;

	/**
	 * @brief Water emitters from the actors in and at the water (D17).
	 *
	 * The player and the actors in the high process (NPCs, horses, creatures) make a wake while
	 * they move through the water with part of their body above it: the top of the bounding box
	 * must be above the surface, so a fully submerged swimmer, fish or object makes none. A wake
	 * is sized from the actor's bound. An actor whose feet cross the surface going down (a jump
	 * or fall into the water) pushes the water down at the entry point, scaled by its speed; one
	 * leaving it upward makes a smaller push. Nearest actors first, up to the solver's limit.
	 * Runs on the main thread.
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

		uint32_t GetWakeCount() const { return wakeCount; }
		uint32_t GetSplashCount() const { return splashCount; }

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
		uint32_t wakeCount = 0;
		uint32_t splashCount = 0;
	};
}
