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
		/** @brief A first hit this far above the surface may be overhead; closer is the water line itself. */
		constexpr float ABOVE_SURFACE_MARGIN = 4.0f;
		/** @brief Where the probes under and over the surface start, relative to it. */
		constexpr float PROBE_OFFSET = 4.0f;
		/** @brief An underside closer to the water than this touches it: solid (a pillar, a low deck edge), not open. */
		constexpr float UNDERSIDE_CLEARANCE = 8.0f;
		/** @brief Meshes whose top is above this depth under the surface shape the flow, so refine around them. */
		constexpr float REFINE_DEPTH = 64.0f;

		/** @brief Landscape collision: kGround in this game (with no reference), kTerrain in principle. */
		bool IsLandscapeLayer(RE::COL_LAYER a_layer)
		{
			return a_layer == RE::COL_LAYER::kGround || a_layer == RE::COL_LAYER::kTerrain;
		}

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
		fine.clear();
		refinedSquares.clear();
		refineQueue.clear();
		dirtySquares.clear();
		cleared = true;
		sweepCursor = 0;
		stats.raysLastFrame = 0;
		lastCentre[0] = lastCentre[1] = INT32_MIN;
		prunedCentre[0] = prunedCentre[1] = INT32_MIN;
		newThisPass = 0;
		idle = false;
		coarseResolved = false;
		burstUpdates = BURST_UPDATES;
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

	BedRaycasts::RayHit BedRaycasts::CastVertical(RE::hkpWorld* a_world, float a_worldX, float a_worldY, float a_fromZ, float a_toZ, bool& a_aborted)
	{
		RayHit result;
		if (a_aborted)
			return result;
		const float scale = RE::bhkWorld::GetWorldScale();
		const float direction = a_toZ < a_fromZ ? -1.0f : 1.0f;
		float fromZ = a_fromZ;
		for (uint32_t attempt = 0; attempt <= MAX_STEP_THROUGHS && (a_toZ - fromZ) * direction > 0.0f; attempt++) {
			// Checked before every ray, so no point can overshoot the frame by more than one ray
			if (Clock::now() > deadline) {
				a_aborted = true;
				return result;
			}
			RE::hkpWorldRayCastInput input;
			input.from = RE::hkVector4(a_worldX * scale, a_worldY * scale, fromZ * scale, 0.0f);
			input.to = RE::hkVector4(a_worldX * scale, a_worldY * scale, a_toZ * scale, 0.0f);
			RE::hkpWorldRayCastOutput output;
			a_world->CastRay(input, output);
			stats.raysCast++;
			stats.raysLastFrame++;
			if (!output.HasHit())
				break;

			const float hitZ = fromZ + (a_toZ - fromZ) * output.hitFraction;
			const auto layer = output.rootCollidable->GetCollisionLayer();
			if (IsStaticLayer(layer)) {
				result = { .hit = true, .z = hitZ, .layer = layer, .collidable = output.rootCollidable };
				return result;
			}
			// The water's own collision, an actor or clutter: continue past it
			stats.stepThroughs++;
			fromZ = hitZ + direction * STEP_THROUGH_OFFSET;
		}
		return result;
	}

	void BedRaycasts::LogHit(float a_worldX, float a_worldY, float a_surface, const RayHit& a_hit, const char* a_what)
	{
		if (hitLogsThisInterval >= MAX_HIT_LOGS_PER_INTERVAL)
			return;
		hitLogsThisInterval++;
		const auto* refr = a_hit.collidable ? RE::TESHavokUtilities::FindCollidableRef(*a_hit.collidable) : nullptr;
		const auto* base = refr ? refr->GetBaseObject() : nullptr;
		const char* name = base ? base->GetName() : "";
		const char* editorID = base ? base->GetFormEditorID() : "";
		logger::info("[Fluid Field] Ray at ({:.0f}, {:.0f}) surface {:.0f}: {} layer {} at {:.0f} ({:.0f} below surface), ref {:08X} '{}' ({})",
			a_worldX, a_worldY, a_surface, a_what, static_cast<uint32_t>(a_hit.layer), a_hit.z, a_surface - a_hit.z,
			refr ? refr->GetFormID() : 0u, name ? name : "", editorID ? editorID : "");
	}

	const BedRaycasts::Sample* BedRaycasts::FindDetail(float a_worldX, float a_worldY) const
	{
		if (spacing <= 0.0f)
			return nullptr;
		const float fineSpacing = GetFineSpacing();
		const auto fx = static_cast<int32_t>(std::round(a_worldX / fineSpacing));
		const auto fy = static_cast<int32_t>(std::round(a_worldY / fineSpacing));
		if (const auto it = fine.find(MakeKey(fx, fy)); it != fine.end())
			return &it->second;
		return Find(a_worldX, a_worldY);
	}

	std::vector<BedRaycasts::FineBlock> BedRaycasts::TakeDirtyBlocks()
	{
		std::vector<FineBlock> blocks;
		blocks.reserve(dirtySquares.size());
		for (const auto& [sx, sy] : dirtySquares) {
			FineBlock block{ .originX = sx * REFINE, .originY = sy * REFINE };
			for (int32_t j = 0; j < REFINE; j++) {
				for (int32_t i = 0; i < REFINE; i++) {
					const int32_t fx = block.originX + i;
					const int32_t fy = block.originY + j;
					float* texel = &block.texels[static_cast<std::size_t>(j * REFINE + i) * 4];
					texel[1] = static_cast<float>(fx);
					texel[2] = static_cast<float>(fy);
					if (const auto it = fine.find(MakeKey(fx, fy)); it != fine.end()) {
						texel[0] = it->second.height;
						texel[3] = it->second.kind == HitKind::Terrain ? 1.0f : it->second.kind == HitKind::Static ? 2.0f :
						                                                                                             0.0f;
					}
				}
			}
			blocks.push_back(block);
		}
		dirtySquares.clear();
		cleared = false;
		return blocks;
	}

	void BedRaycasts::MarkAllRefinedDirty()
	{
		dirtySquares.clear();
		for (const uint64_t key : refinedSquares)
			dirtySquares.emplace_back(static_cast<int32_t>(key >> 32), static_cast<int32_t>(static_cast<uint32_t>(key)));
	}

	void BedRaycasts::QueueRefinement(int32_t a_x, int32_t a_y)
	{
		// The four squares around the point, so an obstacle on either side of it is covered
		for (int32_t sy = a_y - 1; sy <= a_y; sy++) {
			for (int32_t sx = a_x - 1; sx <= a_x; sx++) {
				if (refinedSquares.insert(MakeKey(sx, sy)).second)
					refineQueue.emplace_back(sx, sy);
			}
		}
	}

	bool BedRaycasts::Resolve(const LandscapeCache& a_landscape, const WaterSurfaces& a_water, float a_worldX, float a_worldY, bool a_log, bool a_probeOverheads,
		Sample& a_sample, float& a_surface)
	{
		if (a_water.SampleHeight(a_worldX, a_worldY, a_surface) == WaterSurfaces::Source::None) {
			a_sample = Sample{ .kind = HitKind::NoWater };
			return true;
		}
		float landscape = 0.0f;
		if (a_landscape.SampleHeight(a_worldX, a_worldY, landscape) && a_surface < landscape - BURIED_PLANE_MARGIN) {
			a_sample = Sample{ .kind = HitKind::Skipped };
			return true;
		}
		bool aborted = false;
		a_sample = Cast(a_worldX, a_worldY, a_surface, a_log, a_probeOverheads, aborted);
		return !aborted;
	}

	bool BedRaycasts::RefineSquare(const LandscapeCache& a_landscape, const WaterSurfaces& a_water, int32_t a_squareX, int32_t a_squareY, bool a_log)
	{
		const float fineSpacing = GetFineSpacing();
		for (int32_t j = 0; j < REFINE; j++) {
			for (int32_t i = 0; i < REFINE; i++) {
				const int32_t fx = a_squareX * REFINE + i;
				const int32_t fy = a_squareY * REFINE + j;
				const uint64_t key = MakeKey(fx, fy);
				if (fine.contains(key))
					continue;  // Done in an earlier, interrupted pass over this square
				Sample sample;
				float surface = 0.0f;
				if (!Resolve(a_landscape, a_water, static_cast<float>(fx) * fineSpacing, static_cast<float>(fy) * fineSpacing, a_log, true, sample, surface))
					return false;
				fine.emplace(key, sample);
			}
		}
		dirtySquares.emplace_back(a_squareX, a_squareY);
		return true;
	}

	BedRaycasts::Sample BedRaycasts::Cast(float a_worldX, float a_worldY, float a_surface, bool a_log, bool a_probeOverheads, bool& a_aborted)
	{
		Sample result{ .kind = HitKind::NoHit };

		auto tes = RE::TES::GetSingleton();
		auto* cell = tes ? (tes->interiorCell ? tes->interiorCell : tes->GetCell(RE::NiPoint3(a_worldX, a_worldY, a_surface))) : nullptr;
		auto* bhkWorld = cell ? cell->GetbhkWorld() : nullptr;
		auto* world = bhkWorld ? bhkWorld->GetWorld1() : nullptr;
		if (!world)
			return result;

		const float bottom = a_surface - RAY_END_BELOW_SURFACE;
		const RE::BSReadLockGuard lock(bhkWorld->worldLock);

		RayHit hit = CastVertical(world, a_worldX, a_worldY, a_surface + RAY_START_ABOVE_SURFACE, bottom, a_aborted);
		if (!hit.hit)
			return result;

		// Landscape never overhangs water, so only a mesh above the water can be overhead
		if (a_probeOverheads && hit.z > a_surface + ABOVE_SURFACE_MARGIN && !IsLandscapeLayer(hit.layer)) {
			// Something above the water: look up from just under the surface. An underside below the
			// top means air over the water (a deck, an arch); nothing, or only the top itself, means a
			// solid rising out of the water (a rock, a pier), which stays an obstacle.
			const RayHit up = CastVertical(world, a_worldX, a_worldY, a_surface - PROBE_OFFSET, hit.z + 1.0f, a_aborted);
			// An underside touching the water is solid (a pillar, a low deck edge); one clear of it is open
			if (up.hit && up.z < hit.z - ABOVE_SURFACE_MARGIN && up.z > a_surface + UNDERSIDE_CLEARANCE) {
				stats.overheads++;
				result.overhead = true;
				if (a_log)
					LogHit(a_worldX, a_worldY, a_surface, hit, "overhead, looking under");
				hit = CastVertical(world, a_worldX, a_worldY, std::min(up.z, a_surface + PROBE_OFFSET), bottom, a_aborted);
				if (!hit.hit)
					return result;
			}
		}

		const bool landscape = IsLandscapeLayer(hit.layer);
		if (a_log && !landscape)
			LogHit(a_worldX, a_worldY, a_surface, hit, "static");
		result.height = hit.z;
		result.kind = landscape ? HitKind::Terrain : HitKind::Static;
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
			coarseResolved = false;
		}
		if (centreX != lastCentre[0] || centreY != lastCentre[1]) {
			// A jump of more than half the covered square is a teleport: refill quickly
			if (lastCentre[0] != INT32_MIN && (std::abs(centreX - lastCentre[0]) > half || std::abs(centreY - lastCentre[1]) > half))
				burstUpdates = BURST_UPDATES;
			lastCentre[0] = centreX;
			lastCentre[1] = centreY;
			idle = false;
			coarseResolved = false;
		}

		const bool burst = burstUpdates > 0;
		if (burst)
			burstUpdates--;
		const double timeBudget = burst ? BURST_TIME_BUDGET_MS : RAY_TIME_BUDGET_MS;
		const uint32_t maxRays = burst ? a_maxRays * 8 : a_maxRays;
		const auto updateStart = Clock::now();
		deadline = updateStart + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(timeBudget));

		if (!refineQueue.empty())
			idle = false;
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
				std::erase_if(fine, [&](const auto& entry) {
					const auto x = static_cast<int32_t>(entry.first >> 32) / REFINE;
					const auto y = static_cast<int32_t>(static_cast<uint32_t>(entry.first)) / REFINE;
					return std::abs(x - centreX) > half * 2 || std::abs(y - centreY) > half * 2;
				});
				std::erase_if(refinedSquares, [&](uint64_t key) {
					const auto x = static_cast<int32_t>(key >> 32);
					const auto y = static_cast<int32_t>(static_cast<uint32_t>(key));
					return std::abs(x - centreX) > half * 2 || std::abs(y - centreY) > half * 2;
				});
			}

			uint32_t cast = 0;
			uint32_t examined = 0;
			while (!coarseResolved && examined < SWEEP_PER_UPDATE && examined < total && cast < maxRays && Clock::now() < deadline) {
				const uint32_t index = sweepCursor;
				sweepCursor = (sweepCursor + 1) % total;
				examined++;
				if (sweepCursor == 0) {
					// A whole pass found nothing to add: the coarse lattice around this centre is complete
					if (newThisPass == 0) {
						coarseResolved = true;
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
				Sample sample;
				float surface = 0.0f;
				if (!Resolve(a_landscape, a_water, wx, wy, a_log, false, sample, surface)) {
					newThisPass--;  // Out of time; this point is retried on the next pass
					break;
				}
				samples.emplace(key, sample);
				if (sample.kind == HitKind::Terrain || sample.kind == HitKind::Static || sample.kind == HitKind::NoHit)
					cast++;
				if (sample.kind == HitKind::Static && sample.height > surface - REFINE_DEPTH)
					QueueRefinement(lx, ly);
			}

			// Then refine around structures near the player, where the detail is visible
			const float refineRadius = REFINE_RADIUS / spacing;
			const float playerX = a_centreX / spacing;
			const float playerY = a_centreY / spacing;
			while (coarseResolved && !refineQueue.empty() && Clock::now() < deadline) {
				const auto [sx, sy] = refineQueue.front();
				refineQueue.pop_front();
				const float dx = static_cast<float>(sx) + 0.5f - playerX;
				const float dy = static_cast<float>(sy) + 0.5f - playerY;
				if (dx * dx + dy * dy > refineRadius * refineRadius) {
					refinedSquares.erase(MakeKey(sx, sy));  // Queued again when the coarse ray is recast nearby
					continue;
				}
				if (!RefineSquare(a_landscape, a_water, sx, sy, a_log)) {
					refineQueue.emplace_front(sx, sy);  // Out of time; finish it next update
					break;
				}
			}
			if (coarseResolved && refineQueue.empty())
				idle = true;
		}
		const double rayMs = std::chrono::duration<double, std::milli>(Clock::now() - updateStart).count();

		if (!a_log)
			return;
		rayMsSinceSummary += rayMs;
		if (updatesUntilSummary > 0) {
			updatesUntilSummary--;
			return;
		}
		updatesUntilSummary = LOG_SUMMARY_INTERVAL;
		const auto counts = CountSamples();
		logger::info("[Fluid Field] Bed rays: {} cast in the last {} updates ({:.3f} ms total), {} stepped through and {} overheads overall; cached {} terrain, {} mesh, {} out of reach, {} buried plane, {} no water; idle {}",
			stats.raysCast - raysAtLastSummary, LOG_SUMMARY_INTERVAL, rayMsSinceSummary, stats.stepThroughs, stats.overheads,
			counts.terrain, counts.statics, counts.noHit, counts.skipped, counts.noWater, idle ? "yes" : "no");
		raysAtLastSummary = stats.raysCast;
		rayMsSinceSummary = 0.0;
		hitLogsThisInterval = 0;
	}
}
