#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>

namespace FluidFieldSim
{
	/**
	 * @brief CPU copy of the landscape heights of the loaded exterior cells around the player.
	 *
	 * Each attached cell's `TESObjectLAND` heights are copied once when the cell appears in the
	 * engine's loaded grid, and dropped when it leaves. This is the riverbed source for the depth
	 * map (R1, D7 stage 1). The engine stores the heights relative to the cell; adding
	 * `heightExtents.x - min(heights)` turns them into world heights, which `Verify` confirms
	 * against `TES::GetLandHeight`.
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

		/** @brief Result of comparing cached heights with the engine's own height query. */
		struct VerifyStats
		{
			uint32_t cellsChecked = 0;
			uint32_t samplesChecked = 0;
			uint32_t samplesMissed = 0;         ///< Engine query returned no height
			float maxVertexError = 0.0f;        ///< Cached world height at a vertex vs the engine
			float maxInterpolatedError = 0.0f;  ///< Bilinear sample between vertices vs the engine's triangles
			float maxAbsAnchor = 0.0f;          ///< Largest relative-to-world offset seen
		};

		/**
		 * @brief Polls the engine's loaded grid and copies newly attached cells.
		 * @param a_verify Compare each newly copied cell with `TES::GetLandHeight` and log the result.
		 * Call on the main thread; it reads the engine's cell grid.
		 */
		void Update(bool a_verify);

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

		/** @brief D172: a copy holding only the cached cells over a world area (for a worker; the cache itself changes each frame). */
		LandscapeCache Subset(float a_minX, float a_minY, float a_maxX, float a_maxY) const;
		const VerifyStats& GetVerifyStats() const { return verifyStats; }

	private:
		void Verify(const Cell& a_cell);

		std::unordered_map<uint64_t, Cell> cells;
		const void* worldSpace = nullptr;  ///< Identity only; never dereferenced
		VerifyStats verifyStats;
	};
}
