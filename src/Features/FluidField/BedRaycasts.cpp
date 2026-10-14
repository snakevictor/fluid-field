#include "BedRaycasts.h"

#include "LandscapeCache.h"
#include "WaterSurfaces.h"

namespace FluidFieldSim
{
	namespace
	{
		constexpr float RAY_START_ABOVE_SURFACE = 256.0f;
		constexpr float RAY_END_BELOW_SURFACE = 1024.0f;
		/** @brief A water plane this far under the landscape is dry ground; no ray. */
		constexpr float BURIED_PLANE_MARGIN = 256.0f;
		/** @brief Non-static hits stepped through before giving up on a ray. */
		constexpr uint32_t MAX_STEP_THROUGHS = 4;
		/** @brief How far below a non-static hit the next ray segment starts. */
		constexpr float STEP_THROUGH_OFFSET = 2.0f;
		/** @brief Lattice points examined per update while looking for work. */
		constexpr uint32_t SWEEP_PER_UPDATE = 1024;

		bool IsStaticLayer(RE::COL_LAYER a_layer)
		{
			switch (a_layer) {
			case RE::COL_LAYER::kStatic:
			case RE::COL_LAYER::kAnimStatic:
			case RE::COL_LAYER::kTrees:
			case RE::COL_LAYER::kTerrain:
			case RE::COL_LAYER::kGround:
				return true;
			default:
				return false;
			}
		}
	}

	uint64_t BedRaycasts::MakeKey(int32_t a_x, int32_t a_y)
	{
		return (static_cast<uint64_t>(static_cast<uint32_t>(a_x)) << 32) | static_cast<uint32_t>(a_y);
	}

	void BedRaycasts::Clear()
	{
		samples.clear();
		sweepCursor = 0;
		stats.raysLastFrame = 0;
	}

	BedRaycasts::Counts BedRaycasts::CountSamples() const
	{
		Counts counts;
		for (const auto& [key, sample] : samples) {
			switch (sample.kind) {
			case HitKind::Terrain:
				counts.terrain++;
				break;
			case HitKind::Static:
				counts.statics++;
				break;
			case HitKind::NoHit:
				counts.noHit++;
				break;
			case HitKind::Skipped:
				counts.skipped++;
				break;
			}
		}
		return counts;
	}

	const BedRaycasts::Sample* BedRaycasts::Find(float a_worldX, float a_worldY) const
	{
		if (spacing <= 0.0f)
			return nullptr;
		const auto x = static_cast<int32_t>(std::round(a_worldX / spacing));
		const auto y = static_cast<int32_t>(std::round(a_worldY / spacing));
		const auto it = samples.find(MakeKey(x, y));
		return it != samples.end() ? &it->second : nullptr;
	}

	BedRaycasts::Sample BedRaycasts::Cast(float a_worldX, float a_worldY, float a_surface)
	{
		Sample result{ .kind = HitKind::NoHit };

		auto tes = RE::TES::GetSingleton();
		auto* cell = tes ? (tes->interiorCell ? tes->interiorCell : tes->GetCell(RE::NiPoint3(a_worldX, a_worldY, a_surface))) : nullptr;
		auto* bhkWorld = cell ? cell->GetbhkWorld() : nullptr;
		auto* world = bhkWorld ? bhkWorld->GetWorld1() : nullptr;
		if (!world)
			return result;

		const float scale = RE::bhkWorld::GetWorldScale();
		float fromZ = a_surface + RAY_START_ABOVE_SURFACE;
		const float toZ = a_surface - RAY_END_BELOW_SURFACE;

		const RE::BSReadLockGuard lock(bhkWorld->worldLock);
		for (uint32_t attempt = 0; attempt <= MAX_STEP_THROUGHS && fromZ > toZ; attempt++) {
			RE::hkpWorldRayCastInput input;
			input.from = RE::hkVector4(a_worldX * scale, a_worldY * scale, fromZ * scale, 0.0f);
			input.to = RE::hkVector4(a_worldX * scale, a_worldY * scale, toZ * scale, 0.0f);
			RE::hkpWorldRayCastOutput output;
			world->CastRay(input, output);
			stats.raysCast++;
			stats.raysLastFrame++;
			if (!output.HasHit())
				break;

			const float hitZ = fromZ + (toZ - fromZ) * output.hitFraction;
			const auto layer = output.rootCollidable->GetCollisionLayer();
			if (IsStaticLayer(layer)) {
				result.height = hitZ;
				result.kind = layer == RE::COL_LAYER::kTerrain ? HitKind::Terrain : HitKind::Static;
				return result;
			}
			// The water's own collision, an actor or clutter: continue below it
			stats.stepThroughs++;
			fromZ = hitZ - STEP_THROUGH_OFFSET;
		}
		return result;
	}

	void BedRaycasts::Update(const LandscapeCache& a_landscape, const WaterSurfaces& a_water, float a_centreX, float a_centreY,
		float a_extent, float a_spacing, uint32_t a_maxRays)
	{
		stats.raysLastFrame = 0;
		if (a_spacing != spacing) {
			samples.clear();
			sweepCursor = 0;
			spacing = a_spacing;
		}

		const int32_t half = static_cast<int32_t>(std::ceil(a_extent * 0.5f / spacing));
		const int32_t side = half * 2 + 1;
		const int32_t centreX = static_cast<int32_t>(std::round(a_centreX / spacing));
		const int32_t centreY = static_cast<int32_t>(std::round(a_centreY / spacing));
		const auto total = static_cast<uint32_t>(side * side);

		// Drop points that have fallen well outside the covered square
		if (sweepCursor == 0) {
			std::erase_if(samples, [&](const auto& entry) {
				const auto x = static_cast<int32_t>(entry.first >> 32);
				const auto y = static_cast<int32_t>(static_cast<uint32_t>(entry.first));
				return std::abs(x - centreX) > half * 2 || std::abs(y - centreY) > half * 2;
			});
		}

		uint32_t cast = 0;
		uint32_t examined = 0;
		while (examined < SWEEP_PER_UPDATE && examined < total && cast < a_maxRays) {
			const uint32_t index = sweepCursor;
			sweepCursor = (sweepCursor + 1) % total;
			examined++;

			const int32_t lx = centreX - half + static_cast<int32_t>(index % static_cast<uint32_t>(side));
			const int32_t ly = centreY - half + static_cast<int32_t>(index / static_cast<uint32_t>(side));
			const uint64_t key = MakeKey(lx, ly);
			if (samples.contains(key))
				continue;

			const float wx = static_cast<float>(lx) * spacing;
			const float wy = static_cast<float>(ly) * spacing;
			float surface = 0.0f;
			if (a_water.SampleHeight(wx, wy, surface) == WaterSurfaces::Source::None)
				continue;  // No water plane yet; may appear once water objects load

			float landscape = 0.0f;
			if (a_landscape.SampleHeight(wx, wy, landscape) && surface < landscape - BURIED_PLANE_MARGIN) {
				samples.emplace(key, Sample{ .kind = HitKind::Skipped });
				continue;
			}

			samples.emplace(key, Cast(wx, wy, surface));
			cast++;
		}
	}
}
