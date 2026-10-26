#pragma once

#include <cstdint>
#include <vector>

namespace FluidFieldSim::FallStrands
{
	/** D190: calm, level run (world units) over which the recent fall clears by e; calm from CALM_FROM of level. */
	inline constexpr float CALM_RUN = 200.0f;
	inline constexpr float CALM_FROM = 0.75f;

	/**
	 * @brief D80: a falls mesh's texture grid as rasterised by its bake (v runs downstream).
	 * Plain types so the offline harness (tools/harness/strand) can run this code.
	 */
	struct Input
	{
		uint32_t width = 0, height = 0;
		float texelV = 0.0f;              ///< Texture v per row
		const float* position = nullptr;  ///< World x, y, z per texel (RE::NiPoint3's layout)
		const uint8_t* covered = nullptr;
		const uint8_t* rock = nullptr;
	};

	/** @brief D80: the strand sheet's steady state, gathered onto the texture grid. */
	struct Output
	{
		/// Per texel: source lip column, run from it (texture v, D67), aeration, foam, turn (texels across per row)
		/// and speed (D192: world units/s over 300)
		std::vector<float> seed, along, aeration, foam, turn, speed;
		std::vector<float> fallen;       ///< D189: height fallen down steep faces; D190: clears over calm runs
		std::vector<float> calm;         ///< D190: calm run since this mesh's lip (world units)
		std::vector<float> swell;        ///< D82: how much the water piles up; 0 where it runs free
		std::vector<float> disturbance;  ///< D85: stirring carried down from pile-ups and strikes; drives the wobble
		std::vector<int32_t> crest;      ///< D79: per column, the row its water starts at (-1: none)
		std::vector<uint8_t> leadIn;     ///< D79: texels before their column's crest
		uint32_t strands = 0, nodes = 0, reachedFoot = 0, openTexels = 0, leadTexels = 0, inflows = 0;
		float longestRun = 0.0f;           ///< Seconds
		std::vector<float> nodeX;          ///< Strands x rows: across position (texels)
		std::vector<uint8_t> nodeAlive;    ///< Strands x rows
		std::vector<float> nodeThickness;  ///< Strands x rows: world units (discharge over speed and spacing)
	};

	/**
	 * @brief D80: marches the water down a falls mesh as non-crossing strands of equal discharge, under gravity, bed
	 * friction and drag, steering round rock and healing back to their columns behind it.
	 */
	Output March(const Input& a_input);

	/**
	 * @brief D79: each column's crest row (-1: none) and the hidden lead-in above it. Reads only position and covered.
	 * @return The number of lead-in texels.
	 */
	uint32_t FindCrests(const Input& a_input, std::vector<int32_t>& a_crest, std::vector<uint8_t>& a_leadIn);

	/**
	 * @brief D83: lead-in texels more than a_reach across from any running texel in their row; the rock test skips
	 * them. Reads only position and covered.
	 */
	std::vector<uint8_t> HiddenLeadIn(const Input& a_input, int32_t a_reach);

	/** @brief D82: this code's version, part of the bake cache key. */
	const char* Version();
}
