#pragma once

#include <cstdint>
#include <vector>

namespace FluidFieldSim::FallStrands
{
	/** @brief D190: calm, level run (world units) over which the recent fall clears by e. */
	inline constexpr float CALM_RUN = 200.0f;

	/** @brief D80: a falls mesh's texture grid as rasterised by its bake (v runs downstream). */
	struct Input
	{
		uint32_t width = 0, height = 0;
		const float* position = nullptr;  ///< World x, y, z per texel (RE::NiPoint3's layout)
		const uint8_t* covered = nullptr;
	};

	/**
	 * @brief D79: each column's crest row (-1: none) and the hidden lead-in above it. Reads only position and covered.
	 * @return The number of lead-in texels.
	 */
	uint32_t FindCrests(const Input& a_input, std::vector<int32_t>& a_crest, std::vector<uint8_t>& a_leadIn);
}
