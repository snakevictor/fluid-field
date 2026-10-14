#include "WakeSources.h"

#include "WaterSurfaces.h"

#include <algorithm>
#include <cmath>

namespace FluidFieldSim
{
	namespace
	{
		/** @brief Slower than this (world units per second) makes no wake. */
		constexpr float MIN_WAKE_SPEED = 20.0f;
		/** @brief Faster than this between two steps is a teleport, not movement. */
		constexpr float MAX_SPEED = 1500.0f;
		/** @brief Feet may be this far above the surface and still touch the water (a wading stride). */
		constexpr float FEET_ABOVE_SURFACE = 32.0f;
		/** @brief Smallest wake radius, world units, so tiny creatures still disturb the water. */
		constexpr float MIN_WAKE_RADIUS = 12.0f;
		/** @brief A wake is this fraction of the actor's horizontal half-width. */
		constexpr float WAKE_RADIUS_SCALE = 0.6f;
		/** @brief Vertical speed (world units per second) below which crossing the surface is not a splash. */
		constexpr float MIN_SPLASH_SPEED = 150.0f;
		/** @brief Depth pushed down at the entry point per unit of downward speed. */
		constexpr float SPLASH_DEPTH_PER_SPEED = 0.1f;
		/** @brief Most depth one splash pushes down, world units. */
		constexpr float MAX_SPLASH_DEPTH = 120.0f;
		/** @brief A splash covers this many times the actor's wake radius. */
		constexpr float SPLASH_RADIUS_SCALE = 2.5f;
		/** @brief Leaving the water pushes less than entering it. */
		constexpr float EXIT_SPLASH_SCALE = 0.4f;

		struct Candidate
		{
			RE::Actor* actor;
			float distanceSquared;
		};
	}

	void WakeSources::Clear()
	{
		history.clear();
		emitters.clear();
		wakeCount = splashCount = 0;
	}

	const std::vector<WaterEmitter>& WakeSources::Update(const WaterSurfaces& a_water, const RE::NiPoint3& a_playerPos, const int32_t a_gridMin[2],
		float a_cellSize, uint32_t a_gridRes, float a_deltaTime, const Settings& a_settings)
	{
		emitters.clear();
		wakeCount = splashCount = 0;
		updateCounter++;
		if (!(a_deltaTime > 0.0f) || !(a_cellSize > 0.0f))
			return emitters;

		// Every loaded actor inside the grid, nearest the player first
		const float halfGrid = a_cellSize * static_cast<float>(a_gridRes) * 0.5f;
		std::vector<Candidate> candidates;
		const auto consider = [&](RE::Actor* a_actor) {
			if (!a_actor || !a_actor->Is3DLoaded() || a_actor->IsDisabled())
				return;
			const auto position = a_actor->GetPosition();
			if (std::abs(position.x - a_playerPos.x) > halfGrid || std::abs(position.y - a_playerPos.y) > halfGrid)
				return;
			candidates.push_back({ a_actor, a_playerPos.GetSquaredDistance(position) });
		};
		if (auto* processLists = RE::ProcessLists::GetSingleton()) {
			for (auto& handle : processLists->highActorHandles)
				consider(handle.get().get());
		}
		consider(RE::PlayerCharacter::GetSingleton());
		std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.distanceSquared < b.distanceSquared; });

		for (const auto& candidate : candidates) {
			auto* actor = candidate.actor;
			const uint32_t formID = actor->GetFormID();
			const auto position = actor->GetPosition();

			// Velocity from the last sighting; a first sighting or a teleport gives none
			auto& seen = history[formID];
			const bool hasPrevious = seen.lastSeen == updateCounter - 1;
			const RE::NiPoint3 previous = seen.position;
			seen.position = position;
			seen.lastSeen = updateCounter;
			if (!hasPrevious || emitters.size() >= ShallowWaterSolver::MAX_EMITTERS)
				continue;
			const RE::NiPoint3 velocity = (position - previous) / a_deltaTime;
			if (velocity.Length() > MAX_SPEED)
				continue;

			float surface = 0.0f;
			if (a_water.SampleHeight(position.x, position.y, surface) == WaterSurfaces::Source::None)
				continue;

			// The actor's bound, scaled: its top decides whether it is fully under water
			const float scale = actor->GetScale();
			const auto boundMin = actor->GetBoundMin();
			const auto boundMax = actor->GetBoundMax();
			const float top = position.z + boundMax.z * scale;
			const float halfWidth = 0.5f * std::max(boundMax.x - boundMin.x, boundMax.y - boundMin.y) * scale;
			const float radius = std::max(halfWidth * WAKE_RADIUS_SCALE, MIN_WAKE_RADIUS) / a_cellSize;
			const float texel[2] = { position.x / a_cellSize - static_cast<float>(a_gridMin[0]), position.y / a_cellSize - static_cast<float>(a_gridMin[1]) };

			// Entering or leaving the water: the feet crossed the surface this step
			const bool wasAbove = previous.z > surface;
			const bool isAbove = position.z > surface;
			if (a_settings.splashStrength > 0.0f && wasAbove != isAbove && std::abs(velocity.z) > MIN_SPLASH_SPEED) {
				const float push = std::min(std::abs(velocity.z) * SPLASH_DEPTH_PER_SPEED, MAX_SPLASH_DEPTH) * a_settings.splashStrength * (isAbove ? EXIT_SPLASH_SCALE : 1.0f);
				emitters.push_back({ .position = { texel[0], texel[1] },
					.radius = radius * SPLASH_RADIUS_SCALE,
					.depthRate = -push / a_deltaTime,  // Spread over this frame's sub-steps
					.velocity = { velocity.x, velocity.y },
					.pad1 = {} });
				splashCount++;
				continue;
			}

			// A wake: in the water, not fully under it, and moving
			const float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);
			if (a_settings.wakeStrength <= 0.0f || speed < MIN_WAKE_SPEED || position.z > surface + FEET_ABOVE_SURFACE || top < surface)
				continue;
			emitters.push_back({ .position = { texel[0], texel[1] },
				.radius = radius,
				.depthRate = 0.0f,
				.velocity = { velocity.x * a_settings.wakeStrength, velocity.y * a_settings.wakeStrength },
				.pad1 = {} });
			wakeCount++;
		}

		// Drop actors not seen for a while
		for (auto it = history.begin(); it != history.end();) {
			if (updateCounter - it->second.lastSeen > FORGET_AFTER)
				it = history.erase(it);
			else
				++it;
		}
		return emitters;
	}
}
