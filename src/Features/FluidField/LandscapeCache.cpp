#include "LandscapeCache.h"

#include "EngineGrid.h"

namespace FluidFieldSim
{
	namespace
	{
		/**
		 * @brief Height of cell vertex (x, y) from the engine's four 17x17 quadrants (bit 0 east, bit 1 north).
		 * Quadrants share edge vertices; the middle row and column come from the west and south quadrants.
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

	void LandscapeCache::Update()
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
				// Stored heights are relative; heightExtents.x is the cell's lowest world height (checked against GetLandHeight)
				copy.anchor = land->loadedData->heightExtents.x - *std::ranges::min_element(copy.heights);
				for (float& height : copy.heights)
					height += copy.anchor;

				cells.emplace(key, copy);
			}
		}

		// Drop cells that left the loaded grid
		std::erase_if(cells, [&](const auto& entry) { return std::ranges::find(present, entry.first) == present.end(); });
	}

}
