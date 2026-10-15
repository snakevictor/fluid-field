#include "AirSources.h"

#include <algorithm>
#include <cmath>

namespace FluidFieldSim
{
	namespace
	{
		/** @brief Slower than this (world units per second) stirs nothing. */
		constexpr float MIN_SPEED = 20.0f;
		/** @brief Faster than this between two steps is a teleport, not movement. */
		constexpr float MAX_SPEED = 1500.0f;
		/** @brief Smallest radius an actor stirs, world units, so small creatures still part the fog. */
		constexpr float MIN_RADIUS = 24.0f;
		/** @brief Per-second rate at which an actor drags the air around it toward its own velocity. */
		constexpr float PUSH_RATE = 3.0f;

		struct Candidate
		{
			RE::Actor* actor;
			float distanceSquared;
		};
	}

	void AirSources::Clear()
	{
		history.clear();
		emitters.clear();
	}

	const std::vector<Emitter>& AirSources::Update(const RE::NiPoint3& a_playerPos, const int32_t a_gridMin[2], float a_cellSize, uint32_t a_gridRes,
		float a_deltaTime, float a_strength, uint32_t a_maxEmitters)
	{
		emitters.clear();
		updateCounter++;
		if (!(a_deltaTime > 0.0f) || !(a_cellSize > 0.0f) || a_strength <= 0.0f)
			return emitters;

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
			const auto position = actor->GetPosition();
			auto& seen = history[actor->GetFormID()];
			const bool hasPrevious = seen.lastSeen == updateCounter - 1;
			const RE::NiPoint3 previous = seen.position;
			seen.position = position;
			seen.lastSeen = updateCounter;
			if (!hasPrevious || emitters.size() >= a_maxEmitters)
				continue;

			const float vx = (position.x - previous.x) / a_deltaTime;
			const float vy = (position.y - previous.y) / a_deltaTime;
			const float speed = std::sqrt(vx * vx + vy * vy);
			if (speed < MIN_SPEED || speed > MAX_SPEED)
				continue;

			// The air over about the actor's width is dragged toward its velocity; the solver adds
			// each emitter's velocity once per step, so the impulse is the rate times the step
			const float scale = actor->GetScale();
			const auto boundMin = actor->GetBoundMin();
			const auto boundMax = actor->GetBoundMax();
			const float halfWidth = 0.5f * std::max(boundMax.x - boundMin.x, boundMax.y - boundMin.y) * scale;
			const float push = std::min(PUSH_RATE * a_deltaTime, 1.0f) * a_strength;
			emitters.push_back({ .position = { position.x / a_cellSize - static_cast<float>(a_gridMin[0]), position.y / a_cellSize - static_cast<float>(a_gridMin[1]) },
				.radius = std::max(halfWidth, MIN_RADIUS) / a_cellSize,
				.pad0 = 0.0f,
				.velocity = { vx * push, vy * push },
				.pad1 = {} });
		}

		for (auto it = history.begin(); it != history.end();) {
			if (updateCounter - it->second.lastSeen > FORGET_AFTER)
				it = history.erase(it);
			else
				++it;
		}
		return emitters;
	}
}
