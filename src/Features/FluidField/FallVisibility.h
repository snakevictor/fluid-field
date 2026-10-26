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
	 * A vertex is visible when a ray along any of a set of sphere directions escapes the area without meeting a sheet or
	 * static triangle or dipping under the landscape. Errs toward keeping: any gap between sheets lets a ray out.
	 * Pure CPU geometry (no Havok, no engine), so it runs on a worker.
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
		/** @brief World units a ray starts off its vertex, past the vertex's own triangles. */
		static constexpr float START = 2.0f;
		/** @brief World units beyond the sheets' bounds a ray must travel unblocked to count as escaped. */
		static constexpr float REACH = 2500.0f;
		/** @brief World units a ray may dip under the landscape before it counts as stopped by it. */
		static constexpr float LAND_MARGIN = 8.0f;
		/** @brief World units between the landscape samples along a ray. */
		static constexpr float LAND_STEP = 48.0f;

		/**
		 * @brief Per sheet, a flag per vertex: 1 visible, 0 hidden; vertices no drawn triangle uses read 1. Multithreaded.
		 * @param a_others The statics' triangles round the sheets (world).
		 * @param a_land The landscape, or null (interior).
		 */
		static std::vector<std::vector<uint8_t>> Visible(const std::vector<Sheet>& a_sheets, const std::vector<Triangle>& a_others, const LandscapeCache* a_land, uint64_t* o_tested = nullptr,
			uint64_t* o_rays = nullptr);
	};
}
