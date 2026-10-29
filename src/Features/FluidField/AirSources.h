#pragma once

#include "WindSolver.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace FluidFieldSim
{
	/**
	 * @brief D21: wind and fog emitters from moving actors near the player.
	 *
	 * Follows the body, not the collision capsule: each moving foot, calf and hip (whole body for creatures without a
	 * humanoid skeleton) drags the air and ploughs the fog. Nearest actors first, up to the solver's limit. Main thread.
	 */
	class AirSources
	{
	public:
		/**
		 * @brief Builds this step's emitters in the wind grid's logical texel coordinates.
		 * @param a_strength Scales how hard actors push the air (0: not at all).
		 * @param a_maxEmitters Most emitters to return.
		 */
		const std::vector<Emitter>& Update(const RE::NiPoint3& a_playerPos, const int32_t a_gridMin[2], float a_cellSize, uint32_t a_gridRes,
			float a_deltaTime, float a_strength, uint32_t a_maxEmitters);

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

		std::unordered_map<uint64_t, History> history;  ///< Keyed by actor form ID (high bits) and body part (low byte)
		std::vector<Emitter> emitters;
		uint32_t updateCounter = 0;
	};
}
