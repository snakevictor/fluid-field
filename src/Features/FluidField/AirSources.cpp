#include "AirSources.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace FluidFieldSim
{
	namespace
	{
		/** @brief Slower than this (world units per second) stirs nothing. */
		constexpr float MIN_SPEED = 20.0f;
		/** @brief Faster than this between two steps is a teleport, not movement. */
		constexpr float MAX_SPEED = 1500.0f;
		/** @brief Per-second rate at which a body part drags the air around it toward its own velocity. */
		constexpr float PUSH_RATE = 6.0f;
		/** @brief Body parts more than this far above the actor's feet are above the ground fog and stir nothing. */
		constexpr float MAX_HEIGHT_ABOVE_FEET = 140.0f;

		/** @brief A skeleton node that stirs the fog, and the radius it stirs (world units, at scale 1). */
		struct BodyPart
		{
			const char* node;
			float radius;
		};

		/** @brief The parts of a humanoid skeleton that move through ground fog: feet, lower legs and hips. */
		constexpr std::array<BodyPart, 5> HUMANOID_PARTS = { {
			{ "NPC L Foot [Lft ]", 12.0f },
			{ "NPC R Foot [Rft ]", 12.0f },
			{ "NPC L Calf [LClf]", 11.0f },
			{ "NPC R Calf [RClf]", 11.0f },
			{ "NPC Pelvis [Pelv]", 18.0f },
		} };

		/** @brief Smallest radius the whole body stirs when the skeleton has none of the parts above (creatures). */
		constexpr float MIN_BODY_RADIUS = 24.0f;

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

		// One emitter per moving body part, so the fog follows the legs and feet rather than the
		// actor's collision capsule. History is kept per actor and part.
		const auto emit = [&](uint64_t a_key, const RE::NiPoint3& a_position, float a_radius) {
			auto& seen = history[a_key];
			const bool hasPrevious = seen.lastSeen == updateCounter - 1;
			const RE::NiPoint3 previous = seen.position;
			seen.position = a_position;
			seen.lastSeen = updateCounter;
			if (!hasPrevious || emitters.size() >= a_maxEmitters)
				return;
			const float vx = (a_position.x - previous.x) / a_deltaTime;
			const float vy = (a_position.y - previous.y) / a_deltaTime;
			const float speed = std::sqrt(vx * vx + vy * vy);
			// Only movement disturbs the air and the fog; a part at rest lets the fog settle back
			if (speed < MIN_SPEED || speed > MAX_SPEED)
				return;
			const float push = std::min(PUSH_RATE * a_deltaTime, 1.0f) * a_strength;
			emitters.push_back({ .position = { a_position.x / a_cellSize - static_cast<float>(a_gridMin[0]), a_position.y / a_cellSize - static_cast<float>(a_gridMin[1]) },
				.radius = a_radius / a_cellSize,
				.pad0 = speed,  // Read as the part's speed by the fog pass
				.velocity = { vx * push, vy * push },
				.pad1 = {} });
		};

		for (const auto& candidate : candidates) {
			if (emitters.size() >= a_maxEmitters)
				break;
			auto* actor = candidate.actor;
			const uint64_t actorKey = static_cast<uint64_t>(actor->GetFormID()) << 8;
			const float scale = actor->GetScale();
			const float feet = actor->GetPosition().z;
			auto* root = actor->Get3D(false);

			bool foundPart = false;
			if (root) {
				for (std::size_t i = 0; i < HUMANOID_PARTS.size(); i++) {
					auto* node = root->GetObjectByName(HUMANOID_PARTS[i].node);
					if (!node)
						continue;
					foundPart = true;
					const auto& position = node->world.translate;
					if (position.z - feet > MAX_HEIGHT_ABOVE_FEET * scale)
						continue;
					emit(actorKey | i, position, HUMANOID_PARTS[i].radius * scale);
				}
			}
			// Creatures without a humanoid skeleton stir the fog with their whole body
			if (!foundPart) {
				const auto boundMin = actor->GetBoundMin();
				const auto boundMax = actor->GetBoundMax();
				const float halfWidth = 0.5f * std::max(boundMax.x - boundMin.x, boundMax.y - boundMin.y) * scale;
				emit(actorKey | 0xFF, actor->GetPosition(), std::max(halfWidth, MIN_BODY_RADIUS));
			}
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
