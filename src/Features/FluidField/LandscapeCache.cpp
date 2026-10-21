#include "LandscapeCache.h"

#include "EngineGrid.h"

namespace FluidFieldSim
{
	namespace
	{
		/**
		 * @brief Height of cell vertex (x, y) from the engine's four 17x17 quadrants.
		 * Quadrant bit 0 is the east half and bit 1 the north half; quadrants share their edge
		 * vertices, so the middle row and column are read from the west and south quadrants.
		 */
		float GetLandVertexHeight(const RE::TESObjectLAND::LoadedLandData& a_data, int32_t a_x, int32_t a_y)
		{
			constexpr int32_t quadSide = 17;
			constexpr int32_t half = quadSide - 1;
			const int32_t qx = a_x > half ? 1 : 0;
			const int32_t qy = a_y > half ? 1 : 0;
			const int32_t lx = a_x - qx * half;
			const int32_t ly = a_y - qy * half;
			return a_data.heights[qx | (qy << 1)][ly * quadSide + lx];
		}
	}

	void LandscapeCache::Clear()
	{
		cells.clear();
		worldSpace = nullptr;
	}

	const LandscapeCache::Cell* LandscapeCache::Find(int32_t a_cellX, int32_t a_cellY) const
	{
		const auto it = cells.find(MakeCellKey(a_cellX, a_cellY));
		return it != cells.end() ? &it->second : nullptr;
	}

	LandscapeCache LandscapeCache::Subset(float a_minX, float a_minY, float a_maxX, float a_maxY) const
	{
		LandscapeCache out;
		out.worldSpace = worldSpace;
		const auto x0 = static_cast<int32_t>(std::floor(a_minX / CELL_UNITS)), x1 = static_cast<int32_t>(std::floor(a_maxX / CELL_UNITS));
		const auto y0 = static_cast<int32_t>(std::floor(a_minY / CELL_UNITS)), y1 = static_cast<int32_t>(std::floor(a_maxY / CELL_UNITS));
		for (int32_t y = y0; y <= y1; y++) {
			for (int32_t x = x0; x <= x1; x++) {
				if (const Cell* cell = Find(x, y))
					out.cells.emplace(MakeCellKey(x, y), *cell);
			}
		}
		return out;
	}

	bool LandscapeCache::SampleHeight(float a_worldX, float a_worldY, float& a_height) const
	{
		const int32_t cellX = static_cast<int32_t>(std::floor(a_worldX / CELL_UNITS));
		const int32_t cellY = static_cast<int32_t>(std::floor(a_worldY / CELL_UNITS));
		const Cell* cell = Find(cellX, cellY);
		if (!cell)
			return false;

		const float fx = (a_worldX - static_cast<float>(cellX * CELL_UNITS)) / VERT_SPACING;
		const float fy = (a_worldY - static_cast<float>(cellY * CELL_UNITS)) / VERT_SPACING;
		const int32_t x0 = std::clamp(static_cast<int32_t>(fx), 0, VERTS_PER_SIDE - 2);
		const int32_t y0 = std::clamp(static_cast<int32_t>(fy), 0, VERTS_PER_SIDE - 2);
		const float tx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
		const float ty = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);

		const auto at = [&](int32_t x, int32_t y) { return cell->heights[y * VERTS_PER_SIDE + x]; };
		const float south = std::lerp(at(x0, y0), at(x0 + 1, y0), tx);
		const float north = std::lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), tx);
		a_height = std::lerp(south, north, ty);
		return true;
	}

	void LandscapeCache::Update(bool a_verify)
	{
		auto tes = RE::TES::GetSingleton();
		if (!tes || !tes->gridCells || tes->interiorCell) {
			// Interiors have no landscape
			if (!cells.empty())
				Clear();
			return;
		}

		const auto* currentWorldSpace = tes->GetRuntimeData2().worldSpace;
		if (currentWorldSpace != worldSpace) {
			cells.clear();
			worldSpace = currentWorldSpace;
		}

		auto* grid = tes->gridCells;
		const uint32_t length = grid->length;
		std::vector<uint64_t> present;
		present.reserve(static_cast<std::size_t>(length) * length);

		for (uint32_t gx = 0; gx < length; gx++) {
			for (uint32_t gy = 0; gy < length; gy++) {
				auto* cell = GetGridCell(grid, gx, gy);
				if (!cell || cell->IsInteriorCell() || !cell->IsAttached())
					continue;

				auto* land = cell->GetRuntimeData().cellLand;
				auto* coordinates = cell->GetCoordinates();
				if (!land || !land->loadedData || !coordinates)
					continue;  // Land 3D can arrive a few frames after the cell attaches; retry next update

				const uint64_t key = MakeCellKey(coordinates->cellX, coordinates->cellY);
				present.push_back(key);
				if (cells.contains(key))
					continue;

				Cell copy;
				copy.x = coordinates->cellX;
				copy.y = coordinates->cellY;
				for (int32_t y = 0; y < VERTS_PER_SIDE; y++) {
					for (int32_t x = 0; x < VERTS_PER_SIDE; x++)
						copy.heights[y * VERTS_PER_SIDE + x] = GetLandVertexHeight(*land->loadedData, x, y);
				}
				// Stored heights are relative; heightExtents.x is the cell's lowest world height
				// (confirmed against TES::GetLandHeight, see reports/r1-stage-a-checks.md)
				copy.anchor = land->loadedData->heightExtents.x - *std::ranges::min_element(copy.heights);
				for (float& height : copy.heights)
					height += copy.anchor;

				const auto& inserted = cells.emplace(key, copy).first->second;
				if (a_verify)
					Verify(inserted);
			}
		}

		// Drop cells that left the loaded grid
		std::erase_if(cells, [&](const auto& entry) { return std::ranges::find(present, entry.first) == present.end(); });
	}

	void LandscapeCache::Verify(const Cell& a_cell)
	{
		auto tes = RE::TES::GetSingleton();
		if (!tes)
			return;

		const float originX = static_cast<float>(a_cell.x * CELL_UNITS);
		const float originY = static_cast<float>(a_cell.y * CELL_UNITS);
		float vertexError = 0.0f;
		float interpolatedError = 0.0f;
		uint32_t missed = 0;

		const auto engineHeight = [&](float x, float y, float guess, float& out) {
			// The probe starts above the expected surface so a downward query finds it
			return tes->GetLandHeight(RE::NiPoint3(x, y, guess + 1024.0f), out);
		};

		// Interior vertices only; edge vertices are shared with neighbours that may not be loaded
		constexpr std::array<std::array<int32_t, 2>, 5> vertices = { { { 8, 8 }, { 24, 8 }, { 8, 24 }, { 24, 24 }, { 16, 16 } } };
		for (const auto& [vx, vy] : vertices) {
			const float cached = a_cell.heights[vy * VERTS_PER_SIDE + vx];
			float engine = 0.0f;
			if (!engineHeight(originX + vx * VERT_SPACING, originY + vy * VERT_SPACING, cached, engine)) {
				missed++;
				continue;
			}
			vertexError = std::max(vertexError, std::abs(cached - engine));
		}

		// A point between vertices: the engine interpolates over triangles, this cache bilinearly
		const float px = originX + 10.37f * VERT_SPACING;
		const float py = originY + 21.61f * VERT_SPACING;
		float sampled = 0.0f;
		float engine = 0.0f;
		if (SampleHeight(px, py, sampled) && engineHeight(px, py, sampled, engine))
			interpolatedError = std::abs(sampled - engine);
		else
			missed++;

		verifyStats.cellsChecked++;
		verifyStats.samplesChecked += static_cast<uint32_t>(vertices.size()) + 1;
		verifyStats.samplesMissed += missed;
		verifyStats.maxVertexError = std::max(verifyStats.maxVertexError, vertexError);
		verifyStats.maxInterpolatedError = std::max(verifyStats.maxInterpolatedError, interpolatedError);
		verifyStats.maxAbsAnchor = std::max(verifyStats.maxAbsAnchor, std::abs(a_cell.anchor));

		logger::info("[Fluid Field] Landscape check cell ({}, {}): vertex error {:.2f}, interpolated error {:.2f}, anchor {:.2f}, missed {}",
			a_cell.x, a_cell.y, vertexError, interpolatedError, a_cell.anchor, missed);
	}
}
