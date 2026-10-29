#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>

namespace FluidFieldSim
{
	/**
	 * @brief CPU copy of the loaded exterior cells' landscape heights; the depth map's riverbed source (D7).
	 *
	 * Copied once when a cell attaches, dropped when it leaves. Stored heights are cell-relative; adding
	 * `heightExtents.x - min(heights)` makes them world heights.
	 */
	class LandscapeCache
	{
	public:
		static constexpr int32_t CELL_UNITS = 4096;    ///< World units per exterior cell side
		static constexpr int32_t VERTS_PER_SIDE = 33;  ///< Landscape vertices per cell side
		static constexpr int32_t VERT_SPACING = CELL_UNITS / (VERTS_PER_SIDE - 1);

		struct Cell
		{
			int32_t x = 0;
			int32_t y = 0;
			/** @brief World heights in row-major order, index `y * VERTS_PER_SIDE + x`, south-west first. */
			std::array<float, VERTS_PER_SIDE * VERTS_PER_SIDE> heights{};
			/** @brief Added to the engine's stored heights to make them world heights. */
			float anchor = 0.0f;
		};

		/** @brief Polls the engine's loaded grid and copies newly attached cells. Main thread only. */
		void Update();

		/** @brief Drops every cached cell; the next update copies the loaded grid again. */
		void Clear();

		/** @brief Cached cell at exterior cell coordinates, or null. */
		const Cell* Find(int32_t a_cellX, int32_t a_cellY) const;

		/**
		 * @brief Bilinearly interpolated landscape height at a world position.
		 * @return False when the position's cell is not cached.
		 */
		bool SampleHeight(float a_worldX, float a_worldY, float& a_height) const;

		std::size_t GetCellCount() const { return cells.size(); }

	private:
		std::unordered_map<uint64_t, Cell> cells;
		const void* worldSpace = nullptr;  ///< Identity only; never dereferenced
	};
}
