#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace FluidFieldSim
{
	/**
	 * @brief D25: the rock under one waterfall site, a heightfield from downward Havok rays over the site plus MARGIN.
	 *
	 * Rays start only CEILING above the site's top so bridges and arches don't become dams. Cast within a time budget;
	 * holes filled from neighbours, spikes removed by a 3x3 median.
	 */
	class FallCapture
	{
	public:
		static constexpr float CELL = 16.0f;
		static constexpr float MARGIN = 256.0f;
		static constexpr float CEILING = 128.0f;
		static constexpr float BELOW = 512.0f;  ///< Rays reach this far below the site's bottom level
		static constexpr float NONE = -1.0e7f;

		/** @brief Lays out the capture over a world box; rays run from a_top + CEILING down to a_bottom - BELOW. */
		void Begin(const float a_min[2], const float a_max[2], float a_top, float a_bottom);
		/** @brief Casts rays until a_budgetMs is used; cleans up once all are cast. True when complete. */
		bool Cast(double a_budgetMs);
		/** @brief D185: whether its cell's physics world is loaded. */
		bool Castable() const;
		bool IsComplete() const { return complete; }
		float GetProgress() const { return heights.empty() ? 0.0f : static_cast<float>(next) / static_cast<float>(heights.size()); }

		/** @brief The ground's height at a world position, bilinear; false outside the capture or where unknown. */
		bool Sample(float a_x, float a_y, float& a_height) const;

		float GetOriginX() const { return originX; }
		float GetOriginY() const { return originY; }
		uint32_t GetWidth() const { return width; }
		uint32_t GetHeight() const { return height; }
		float At(uint32_t a_x, uint32_t a_y) const { return heights[static_cast<std::size_t>(a_y) * width + a_x]; }

		/** @brief Diagnostics: ray hits by collision layer and how many found nothing solid. */
		std::string DescribeRays() const;
		/** @brief Writes the cleaned heights: width, height (uint32), origin x, y, cell (float), then rows. */
		bool Dump(const std::string& a_path) const;
		/** @brief D185: reads a Dump file if it matches the layout from Begin; the capture is then complete. */
		bool Load(const std::string& a_path);

		bool reported = false;  ///< D185: completion handled (logged, meshes regrounded, saved)
		std::string cachePath;  ///< D185: on-disk cache path (empty: not cached)

	private:
		void CleanUp();

		float originX = 0.0f;
		float originY = 0.0f;
		uint32_t width = 0;
		uint32_t height = 0;
		float top = 0.0f;
		float bottom = 0.0f;
		std::vector<float> heights;
		std::size_t next = 0;
		bool complete = false;
		uint32_t layerHits[64]{};   ///< Ray hits by collision layer, solid or passed through
		uint32_t misses = 0;        ///< Rays that found nothing solid
		uint32_t fallsSkipped = 0;  ///< D56: hits on the falls' own collision, passed through
		uint32_t smoothed = 0;      ///< Texels the median moved by more than 16 units
	};
}
