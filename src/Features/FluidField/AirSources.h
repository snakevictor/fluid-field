#pragma once

#include "WindSolver.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace FluidFieldSim
{
	/**
	 * @brief Wind and fog emitters from the actors moving around the player (D21).
	 *
	 * Follows the body, not the collision capsule: each moving foot, lower leg and hip of a humanoid
	 * skeleton (the whole body for creatures without one) drags the air along its own velocity over
	 * about its own size and ploughs the ground fog, so legs kick through the fog as they swing.
	 * Parts high above the feet, above the fog, are left out. Nearest actors first, up to the wind
	 * solver's limit. Runs on the main thread.
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

		uint32_t GetCount() const { return static_cast<uint32_t>(emitters.size()); }

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
