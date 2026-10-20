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
		std::vector<float> swell;        ///< D82: per texel how much the water piles up (pressed together, striking rock): 0 where it runs free
		std::vector<float> disturbance;  ///< D85: how stirred the water is, from where it piled up or struck rock upstream, carried down its strand and fading: the wobble's size
		std::vector<int32_t> crest;      ///< D79: per column, the row its water starts at (-1: none)
		std::vector<uint8_t> leadIn;     ///< D79: texels before their column's crest
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

	/**
	 * @brief D79: each column's crest (the row its water starts at, -1 for none) and the lead-in before it (vanilla's
	 * sheets climb a hidden face under the upper water to their lip). Only position and covered are read.
	 * @return The lead-in's texels.
	 */
	uint32_t FindCrests(const Input& a_input, std::vector<int32_t>& a_crest, std::vector<uint8_t>& a_leadIn);

	/**
	 * @brief D83: the lead-in texels no strand comes near: more than a_reach texels across from any texel past its
	 * crest in the same row (crests lie on different rows, so a row hidden in one column runs in the next, and its
	 * rock walls that water in). The rock test passes them by. Only position and covered are read.
	 */
	std::vector<uint8_t> HiddenLeadIn(const Input& a_input, int32_t a_reach);

	/** @brief D82: this code's build, for the bake cache (a rebuilt sheet bakes afresh). */
	const char* Version();
}
