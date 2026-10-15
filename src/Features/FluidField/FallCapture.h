#pragma once

#include <cstdint>
#include <vector>

namespace FluidFieldSim
{
	/**
	 * @brief The ground under one waterfall site, and the lips its water pours over (D25, stages S4
	 * and S5 of `plans/waterfall-replacement.md`).
	 *
	 * S4, capture: a heightfield over the site's falling water plus MARGIN, CELL units per texel, from
	 * downward Havok rays that start CEILING above the site's top level (so bridges and arches over the
	 * river do not become dams) and see only rock, ground and statics. Cast a few at a time within a
	 * time budget; holes are filled from their neighbours and spikes removed with a 3x3 median.
	 *
	 * S5, lips: for each level the water pours from (every level but the bottom), the iso-contour of the
	 * ground at LIP_DEPTH below the level (the rock just under the water at the crest) by marching
	 * squares, kept where the ground on its downhill side drops more than MIN_DROP within DROP_PROBE:
	 * the edge the water falls from, rather than a bank. Chained into polylines; runs shorter than
	 * MIN_LIP are dropped. Each point carries the outward (downhill) direction.
	 */
	class FallCapture
	{
	public:
		static constexpr float CELL = 16.0f;
		static constexpr float MARGIN = 256.0f;
		static constexpr float CEILING = 128.0f;
		static constexpr float BELOW = 512.0f;  ///< Rays reach this far below the site's bottom level
		static constexpr float LIP_DEPTH = 48.0f;
		static constexpr float MIN_DROP = 96.0f;
		static constexpr float DROP_PROBE = 64.0f;
		static constexpr float MIN_LIP = 64.0f;
		static constexpr float NONE = -1.0e7f;

		struct LipPoint
		{
			float position[3];  ///< On the contour, at the level's height
			float normal[2];    ///< Outward: the way the water falls
		};
		struct Lip
		{
			float level;
			std::vector<LipPoint> points;
			float length;
		};
		/** @brief For the debug view: a piece of a level's shoreline (the ground's contour at the level), or of a lip. */
		struct Edge
		{
			float a[2];
			float b[2];
			float z;
			bool lip;
		};

		/** @brief Lays out the capture over a world box; rays run from a_top + CEILING down to a_bottom - BELOW. */
		void Begin(const float a_min[2], const float a_max[2], float a_top, float a_bottom);
		/** @brief Casts rays until a_budgetMs is used; cleans up once all are cast. True when complete. */
		bool Cast(double a_budgetMs);
		bool IsComplete() const { return complete; }
		float GetProgress() const { return heights.empty() ? 0.0f : static_cast<float>(next) / static_cast<float>(heights.size()); }

		/** @brief Extracts the lips at each of a_levels (once complete). */
		void ExtractLips(const std::vector<float>& a_levels);

		const std::vector<Lip>& GetLips() const { return lips; }
		const std::vector<Edge>& GetEdges() const { return edges; }
		/** @brief The ground's height at a world position, bilinear; false outside the capture or where unknown. */
		bool Sample(float a_x, float a_y, float& a_height) const;

		float GetOriginX() const { return originX; }
		float GetOriginY() const { return originY; }
		uint32_t GetWidth() const { return width; }
		uint32_t GetHeight() const { return height; }
		float At(uint32_t a_x, uint32_t a_y) const { return heights[static_cast<std::size_t>(a_y) * width + a_x]; }

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
		std::vector<Lip> lips;
		std::vector<Edge> edges;
	};
}
