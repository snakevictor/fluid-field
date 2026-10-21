#pragma once

#include "LandscapeCache.h"

#include <array>
#include <cstdint>
#include <vector>

namespace FluidFieldSim
{
	/**
	 * @brief D173: which vertices of the falls' sheets can be seen from outside the falls, the rock and the ground.
	 *
	 * The outer shell of the union of the sheets, the statics round them and the landscape: a vertex is outside when a ray
	 * from it escapes the area along at least one of a set of directions spread over the sphere, met by no triangle (the
	 * sheets' own and the statics') and staying over the landscape. Sheets wholly inside other sheets, tucked behind a
	 * curtain against the cliff or under the ground are seen from nowhere. A gap between sheets lets a ray out: the test
	 * keeps rather than eats. Pure CPU geometry (no Havok, no engine): it runs on a worker.
	 */
	class FallVisibility
	{
	public:
		using Triangle = std::array<RE::NiPoint3, 3>;

		/** @brief One sheet: its vertices (world) and the triangles that are drawn (indices into them). */
		struct Sheet
		{
			const std::vector<RE::NiPoint3>* positions = nullptr;
			const std::vector<std::array<uint32_t, 3>>* triangles = nullptr;
		};

		/** @brief Directions over the sphere, about evenly spaced (a Fibonacci lattice). */
		static constexpr int DIRECTIONS = 96;
		/** @brief World units a ray starts off its vertex along its way (past the vertex's own triangles). */
		static constexpr float START = 2.0f;
		/** @brief World units round the sheets the rays look for anything that hides them before they count as escaped. */
		static constexpr float REACH = 2500.0f;
		/** @brief World units a ray may dip under the landscape before it counts as stopped by it. */
		static constexpr float LAND_MARGIN = 8.0f;
		/** @brief World units between the landscape samples along a ray. */
		static constexpr float LAND_STEP = 48.0f;

		/**
		 * @brief For each sheet, a flag per vertex: 1 seen from outside, 0 seen from nowhere (only vertices some drawn
		 * triangle uses are tested; the rest read 1). a_others: the statics' triangles round the sheets (world). a_land:
		 * the landscape round them, or null (an interior). Runs on several threads; returns the vertices tested and rays.
		 */
		static std::vector<std::vector<uint8_t>> Visible(const std::vector<Sheet>& a_sheets, const std::vector<Triangle>& a_others, const LandscapeCache* a_land, uint64_t* o_tested = nullptr,
			uint64_t* o_rays = nullptr);
	};
}
