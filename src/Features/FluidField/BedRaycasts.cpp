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
		lastCentre[0] = lastCentre[1] = INT32_MIN;
		prunedCentre[0] = prunedCentre[1] = INT32_MIN;
		newThisPass = 0;
		idle = false;
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
			case HitKind::NoWater:
				counts.noWater++;
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

	BedRaycasts::Sample BedRaycasts::Cast(float a_worldX, float a_worldY, float a_surface, bool a_log)
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
			const bool isStatic = IsStaticLayer(layer);

			if (a_log && hitLogsThisInterval < MAX_HIT_LOGS_PER_INTERVAL && layer != RE::COL_LAYER::kTerrain) {
				hitLogsThisInterval++;
				const auto* refr = RE::TESHavokUtilities::FindCollidableRef(*output.rootCollidable);
				const auto* base = refr ? refr->GetBaseObject() : nullptr;
				const char* name = base ? base->GetName() : "";
				const char* editorID = base ? base->GetFormEditorID() : "";
				logger::info("[Fluid Field] Ray at ({:.0f}, {:.0f}) surface {:.0f}: {} layer {} at {:.0f} ({:.0f} below surface), ref {:08X} '{}' ({})",
					a_worldX, a_worldY, a_surface, isStatic ? "static" : "stepped through", static_cast<uint32_t>(layer), hitZ, a_surface - hitZ,
					refr ? refr->GetFormID() : 0u, name ? name : "", editorID ? editorID : "");
			}

			if (isStatic) {
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
		float a_extent, float a_spacing, uint32_t a_maxRays, bool a_log)
	{
		stats.raysLastFrame = 0;
		if (a_spacing != spacing) {
			Clear();
			spacing = a_spacing;
		}

		const int32_t half = static_cast<int32_t>(std::ceil(a_extent * 0.5f / spacing));
		const int32_t side = half * 2 + 1;
		const int32_t centreX = static_cast<int32_t>(std::round(a_centreX / spacing));
		const int32_t centreY = static_cast<int32_t>(std::round(a_centreY / spacing));
		const auto total = static_cast<uint32_t>(side * side);
		sweepCursor %= total;

		// New water surfaces can turn dry or waterless points into ones that need a ray
		if (a_water.GetSignature() != lastWaterSignature) {
			lastWaterSignature = a_water.GetSignature();
			std::erase_if(samples, [](const auto& entry) { return entry.second.kind == HitKind::NoWater || entry.second.kind == HitKind::Skipped; });
			idle = false;
		}
		if (centreX != lastCentre[0] || centreY != lastCentre[1]) {
			lastCentre[0] = centreX;
			lastCentre[1] = centreY;
			idle = false;
		}

		double rayMs = 0.0;
		if (!idle) {
			// Drop points that have fallen well outside the covered square, once per lattice step moved
			if (std::abs(centreX - prunedCentre[0]) > half / 4 || std::abs(centreY - prunedCentre[1]) > half / 4) {
				prunedCentre[0] = centreX;
				prunedCentre[1] = centreY;
				std::erase_if(samples, [&](const auto& entry) {
					const auto x = static_cast<int32_t>(entry.first >> 32);
					const auto y = static_cast<int32_t>(static_cast<uint32_t>(entry.first));
					return std::abs(x - centreX) > half * 2 || std::abs(y - centreY) > half * 2;
				});
			}

			uint32_t cast = 0;
			uint32_t examined = 0;
			while (examined < SWEEP_PER_UPDATE && examined < total && cast < a_maxRays && rayMs < RAY_TIME_BUDGET_MS) {
				const uint32_t index = sweepCursor;
				sweepCursor = (sweepCursor + 1) % total;
				examined++;
				if (sweepCursor == 0) {
					// A whole pass found nothing to add: everything around this centre is resolved
					if (newThisPass == 0) {
						idle = true;
						break;
					}
					newThisPass = 0;
				}

				const int32_t lx = centreX - half + static_cast<int32_t>(index % static_cast<uint32_t>(side));
				const int32_t ly = centreY - half + static_cast<int32_t>(index / static_cast<uint32_t>(side));
				const uint64_t key = MakeKey(lx, ly);
				if (samples.contains(key))
					continue;
				newThisPass++;

				const float wx = static_cast<float>(lx) * spacing;
				const float wy = static_cast<float>(ly) * spacing;
				float surface = 0.0f;
				if (a_water.SampleHeight(wx, wy, surface) == WaterSurfaces::Source::None) {
					samples.emplace(key, Sample{ .kind = HitKind::NoWater });
					continue;
				}

				float landscape = 0.0f;
				if (a_landscape.SampleHeight(wx, wy, landscape) && surface < landscape - BURIED_PLANE_MARGIN) {
					samples.emplace(key, Sample{ .kind = HitKind::Skipped });
					continue;
				}

				const auto begin = std::chrono::steady_clock::now();
				samples.emplace(key, Cast(wx, wy, surface, a_log));
				rayMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
				cast++;
			}
		}

		if (!a_log)
			return;
		rayMsSinceSummary += rayMs;
		if (updatesUntilSummary > 0) {
			updatesUntilSummary--;
			return;
		}
		updatesUntilSummary = LOG_SUMMARY_INTERVAL;
		const auto counts = CountSamples();
		logger::info("[Fluid Field] Bed rays: {} cast in the last {} updates ({:.3f} ms total), {} stepped through overall; cached {} terrain, {} mesh, {} out of reach, {} buried plane, {} no water; idle {}",
			stats.raysCast - raysAtLastSummary, LOG_SUMMARY_INTERVAL, rayMsSinceSummary, stats.stepThroughs,
			counts.terrain, counts.statics, counts.noHit, counts.skipped, counts.noWater, idle ? "yes" : "no");
		raysAtLastSummary = stats.raysCast;
		rayMsSinceSummary = 0.0;
		hitLogsThisInterval = 0;
	}
}
