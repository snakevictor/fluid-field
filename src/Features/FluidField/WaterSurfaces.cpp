#include "WaterSurfaces.h"

#include "EngineGrid.h"

namespace FluidFieldSim
{
	namespace
	{
		/** @brief Water objects whose bound is taller than this are sloped (rapids, waterfalls); Skin uses the same cut-off. */
		constexpr float MAX_FLAT_BOUND_HALF_HEIGHT = 10.0f;
		/** @brief Cell water heights at or above this mean "unset" (as CommonLib's GetExteriorWaterHeight treats them). */
		constexpr float UNSET_WATER_HEIGHT = 2147483600.0f;

		uint64_t HashFloat(uint64_t a_hash, float a_value)
		{
			// FNV-1a over the value rounded to whole units, so tiny float noise does not count as a change
			const auto bits = static_cast<uint32_t>(static_cast<int32_t>(std::round(a_value)));
			for (int i = 0; i < 4; i++) {
				a_hash ^= (bits >> (i * 8)) & 0xFF;
				a_hash *= 1099511628211ull;
			}
			return a_hash;
		}
	}

	void WaterSurfaces::Clear()
	{
		surfaces.clear();
		cellWater.clear();
		slopedCount = 0;
		hasInteriorWater = false;
		updatesUntilRebuild = 0;
		lastSignature = 0;
	}

	void WaterSurfaces::Update(bool a_verify)
	{
		if (updatesUntilRebuild > 0) {
			updatesUntilRebuild--;
			return;
		}
		updatesUntilRebuild = REBUILD_INTERVAL;
		Rebuild(a_verify);
	}

	void WaterSurfaces::Rebuild(bool a_verify)
	{
		surfaces.clear();
		cellWater.clear();
		slopedCount = 0;
		slopedFormID = 0;
		hasInteriorWater = false;

		auto tes = RE::TES::GetSingleton();
		if (!tes)
			return;

		if (auto waterSystem = RE::TESWaterSystem::GetSingleton()) {
			// Other threads add and remove water objects; hold the lock while reading them (as UnifiedWater does)
			RE::BSSpinLockGuard guard(waterSystem->lock);
			for (const auto& waterObject : waterSystem->waterObjects) {
				if (!waterObject)
					continue;
				const auto& plane = waterObject->plane;
				const float planeHeight = std::abs(plane.normal.z) > 1e-4f ? plane.constant / plane.normal.z : std::numeric_limits<float>::quiet_NaN();
				const auto* waterType = waterObject->waterType;
				const uint32_t formID = waterType ? waterType->GetFormID() : 0;
				const float flowScale = waterType && waterType->flags.any(RE::TESWaterForm::Flag::kEnableFlowmap) ? 1.0f : 0.0f;
				// With the check on, log each water type once (how the game tells rivers from lakes and sea)
				if (a_verify && waterType && loggedWaterTypes.insert(formID).second) {
					const auto& v = waterType->linearVelocity;
					logger::info("[Fluid Field] Water type {:08X}: flowmap {}, flowmap scale {:.2f}, linear velocity ({:.1f}, {:.1f}, {:.1f}), wave amplitude {:.2f}",
						formID, flowScale > 0.0f ? "on" : "off", waterType->data.flowmapScale, v.x, v.y, v.z, waterType->amplitude);
				}
				for (const auto& bound : waterObject->multiBounds) {
					if (!bound)
						continue;
					if (bound->size.z > MAX_FLAT_BOUND_HALF_HEIGHT) {
						slopedCount++;
						slopedFormID = formID;
						continue;
					}
					surfaces.push_back({ .minX = bound->center.x - bound->size.x,
						.minY = bound->center.y - bound->size.y,
						.maxX = bound->center.x + bound->size.x,
						.maxY = bound->center.y + bound->size.y,
						.height = bound->center.z,
						.planeHeight = planeHeight,
						.waterFormID = formID,
						.flowScale = flowScale });
				}
			}
		}

		if (auto interior = tes->interiorCell) {
			const float height = interior->GetRuntimeData().waterHeight;
			if (interior->cellFlags.any(RE::TESObjectCELL::Flag::kHasWater) && height < UNSET_WATER_HEIGHT) {
				interiorWater = height;
				hasInteriorWater = true;
			}
		} else if (auto* grid = tes->gridCells) {
			for (uint32_t gx = 0; gx < grid->length; gx++) {
				for (uint32_t gy = 0; gy < grid->length; gy++) {
					auto* cell = GetGridCell(grid, gx, gy);
					if (!cell || cell->IsInteriorCell() || !cell->IsAttached())
						continue;
					auto* coordinates = cell->GetCoordinates();
					const float height = cell->GetExteriorWaterHeight();
					if (coordinates && height > -RE::NI_INFINITY)
						cellWater[MakeCellKey(coordinates->cellX, coordinates->cellY)] = height;
				}
			}
		}

		uint64_t signature = 14695981039346656037ull;
		for (const auto& surface : surfaces) {
			signature = HashFloat(signature, surface.minX);
			signature = HashFloat(signature, surface.minY);
			signature = HashFloat(signature, surface.height);
		}
		signature = HashFloat(signature, static_cast<float>(cellWater.size()));
		if (a_verify && signature != lastSignature)
			Verify();
		lastSignature = signature;
	}

	WaterSurfaces::Source WaterSurfaces::SampleHeight(float a_worldX, float a_worldY, float& a_height) const
	{
		float flowScale = 0.0f;
		return SampleHeight(a_worldX, a_worldY, a_height, flowScale);
	}

	WaterSurfaces::Source WaterSurfaces::SampleHeight(float a_worldX, float a_worldY, float& a_height, float& a_flowScale) const
	{
		// Highest water object covering the point wins
		bool found = false;
		a_flowScale = 0.0f;
		for (const auto& surface : surfaces) {
			if (a_worldX < surface.minX || a_worldX > surface.maxX || a_worldY < surface.minY || a_worldY > surface.maxY)
				continue;
			if (!found || surface.height > a_height) {
				a_height = surface.height;
				a_flowScale = surface.flowScale;
			}
			found = true;
		}
		if (found)
			return Source::WaterObject;

		if (hasInteriorWater) {
			a_height = interiorWater;
			return Source::CellWater;
		}

		const int32_t cellX = static_cast<int32_t>(std::floor(a_worldX / 4096.0f));
		const int32_t cellY = static_cast<int32_t>(std::floor(a_worldY / 4096.0f));
		if (const auto it = cellWater.find(MakeCellKey(cellX, cellY)); it != cellWater.end()) {
			a_height = it->second;
			return Source::CellWater;
		}
		return Source::None;
	}

	void WaterSurfaces::CollectPlanes(float a_minX, float a_minY, float a_maxX, float a_maxY, std::vector<Plane>& a_planes) const
	{
		for (const auto& surface : surfaces) {
			if (surface.maxX < a_minX || surface.minX > a_maxX || surface.maxY < a_minY || surface.minY > a_maxY)
				continue;
			a_planes.push_back({ surface.minX, surface.minY, surface.maxX, surface.maxY, surface.height, Source::WaterObject });
		}
		if (hasInteriorWater) {
			a_planes.push_back({ a_minX, a_minY, a_maxX, a_maxY, interiorWater, Source::CellWater });
			return;
		}
		constexpr float CELL = 4096.0f;
		const auto x0 = static_cast<int32_t>(std::floor(a_minX / CELL));
		const auto y0 = static_cast<int32_t>(std::floor(a_minY / CELL));
		const auto x1 = static_cast<int32_t>(std::floor(a_maxX / CELL));
		const auto y1 = static_cast<int32_t>(std::floor(a_maxY / CELL));
		for (int32_t y = y0; y <= y1; y++) {
			for (int32_t x = x0; x <= x1; x++) {
				if (const auto it = cellWater.find(MakeCellKey(x, y)); it != cellWater.end())
					a_planes.push_back({ x * CELL, y * CELL, (x + 1) * CELL, (y + 1) * CELL, it->second, Source::CellWater });
			}
		}
	}

	void WaterSurfaces::Verify()
	{
		auto tes = RE::TES::GetSingleton();
		if (!tes)
			return;

		uint32_t checked = 0;
		uint32_t missed = 0;
		float maxError = 0.0f;
		float maxPlaneDifference = 0.0f;
		uint32_t maxPlaneFormID = 0;
		float maxPlaneAtX = 0.0f;
		float maxPlaneAtY = 0.0f;

		for (const auto& surface : surfaces) {
			const RE::NiPoint3 centre((surface.minX + surface.maxX) * 0.5f, (surface.minY + surface.maxY) * 0.5f, surface.height);
			if (std::isfinite(surface.planeHeight) && std::abs(surface.planeHeight - surface.height) > maxPlaneDifference) {
				maxPlaneDifference = std::abs(surface.planeHeight - surface.height);
				maxPlaneFormID = surface.waterFormID;
				maxPlaneAtX = centre.x;
				maxPlaneAtY = centre.y;
			}

			auto* cell = tes->interiorCell ? tes->interiorCell : tes->GetCell(centre);
			float engine = 0.0f;
			checked++;
			if (!cell || !cell->GetWaterHeight(centre, engine) || !(engine > -RE::NI_INFINITY)) {
				missed++;
				continue;
			}
			float ours = 0.0f;
			SampleHeight(centre.x, centre.y, ours);  // Same overlap rule the simulation will see
			maxError = std::max(maxError, std::abs(ours - engine));
		}

		verifyStats.runs++;
		verifyStats.samplesChecked += checked;
		verifyStats.samplesMissed += missed;
		verifyStats.maxError = std::max(verifyStats.maxError, maxError);
		verifyStats.maxPlaneDifference = std::max(verifyStats.maxPlaneDifference, maxPlaneDifference);

		logger::info("[Fluid Field] Water check: {} surfaces, {} sloped skipped (water {:08X}), {} cells with water, interior water {}; error {:.2f}, plane vs bound {:.2f} (water {:08X} at {:.0f}, {:.0f}), missed {} of {}",
			surfaces.size(), slopedCount, slopedFormID, cellWater.size(), hasInteriorWater ? "yes" : "no", maxError, maxPlaneDifference, maxPlaneFormID, maxPlaneAtX, maxPlaneAtY, missed, checked);
	}
}
