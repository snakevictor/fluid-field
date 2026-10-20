#pragma once

#include <cstdint>
#include <vector>

namespace FluidFieldSim::FallStrands
{
	/**
	 * @brief D80: one vanilla falls mesh's texture grid, as its bake rasterised it (texture v runs downstream).
	 * Plain types, so the offline harness (scratchpad strandbake.cpp) runs the same code on the bake dumps.
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
		/// Per texel: the lip column (texels, fractional) its water came over, the run from there (texture v, at the
		/// texture's speed: D67's along), aeration, foam where it strikes rock or piles up, its turn (texels across
		/// per row; the debug arrows) and speed (vanilla's 1)
		std::vector<float> seed, along, aeration, foam, turn, speed;
		std::vector<int32_t> crest;   ///< D79: per column, the row its water starts at (-1: none)
		std::vector<uint8_t> leadIn;  ///< D79: texels before their column's crest
		uint32_t strands = 0, nodes = 0, reachedFoot = 0, openTexels = 0, leadTexels = 0, inflows = 0;
		float longestRun = 0.0f;           ///< Seconds
		std::vector<float> nodeX;          ///< Strands x rows: across position (texels)
		std::vector<uint8_t> nodeAlive;    ///< Strands x rows
		std::vector<float> nodeThickness;  ///< Strands x rows: world units (discharge over speed and spacing)
	};

	/**
	 * @brief D80: the water on a vanilla falls mesh as a sheet of strands (streamlines): each column's crest feeds
	 * strands of equal discharge, marched down the mesh a row at a time by gravity along its real shape, bed friction
	 * and air drag; steered round rock they see coming, never crossing (pressed together they thicken and foam),
	 * healing back toward their own column behind it, leaving the wake open meanwhile; aerating as they fall.
	 */
	Output March(const Input& a_input);
}
