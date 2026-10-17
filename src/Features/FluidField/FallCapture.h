#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace FluidFieldSim
{
	/**
	 * @brief The rock under one waterfall site (D25 stage S4, used by D26's grounding).
	 *
	 * A heightfield over the site's falling water plus MARGIN, CELL units per texel, from downward
	 * Havok rays that start CEILING above the site's top level (so bridges and arches over the river
	 * do not become dams) and see only rock, ground and statics. Cast a few at a time within a time
	 * budget; holes are filled from their neighbours and spikes removed with a 3x3 median.
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
		bool IsComplete() const { return complete; }
		float GetProgress() const { return heights.empty() ? 0.0f : static_cast<float>(next) / static_cast<float>(heights.size()); }

		/** @brief The ground's height at a world position, bilinear; false outside the capture or where unknown. */
		bool Sample(float a_x, float a_y, float& a_height) const;

		float GetOriginX() const { return originX; }
		float GetOriginY() const { return originY; }
		uint32_t GetWidth() const { return width; }
		uint32_t GetHeight() const { return height; }
		float At(uint32_t a_x, uint32_t a_y) const { return heights[static_cast<std::size_t>(a_y) * width + a_x]; }

		/** @brief Diagnostics: what the rays hit, by collision layer (the first hit of each attempt), and how many found nothing solid. */
		std::string DescribeRays() const;
		/** @brief Diagnostics: writes the heights (after cleanup) to a_path: width, height (uint32), origin x, y, cell (float), then the heights row by row. */
		bool Dump(const std::string& a_path) const;

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
