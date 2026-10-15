#pragma once

#include <cstdint>
#include <vector>

namespace FluidFieldSim
{
	/**
	 * @brief The steady flow over one waterfall site (D28), solved once, off the main thread.
	 *
	 * Water flows downhill: the drive is the unit downhill direction of the falling water's surface
	 * (vanilla's meshes seen from above), weaker where it is nearly flat. A pressure projection (the
	 * Helmholtz decomposition, as in a fluid solver's pressure step) makes that flow divergence-free
	 * and tangent to the rock: it cannot pass through a boulder or a bank, so it parts round boulders
	 * and closes up behind them, and where two streams meet it merges them. Water comes in where the
	 * surface is near the site's top and leaves near its bottom (open boundaries, pressure 0).
	 *
	 * The result is a stream function: its contours are the streamlines. With the drive a unit speed,
	 * it reads in world units across the flow, so a texture's across coordinate can be replaced by it
	 * and its streaks follow the streamlines.
	 */
	class FallStream
	{
	public:
		enum Cell : uint8_t
		{
			Wall = 0,    ///< No water: rock standing out of it, a bank, or no falling water above
			Water = 1,   ///< Water the flow passes through
			Inflow = 2,  ///< Open boundary where the water comes in
			Outflow = 3  ///< Open boundary where it leaves
		};

		struct Input
		{
			uint32_t width = 0;
			uint32_t height = 0;
			float cell = 16.0f;          ///< World units per cell
			std::vector<float> surface;  ///< Water surface height per cell (any value on walls)
			std::vector<uint8_t> kind;   ///< Cell per cell
		};

		struct Result
		{
			uint32_t width = 0;
			uint32_t height = 0;
			std::vector<float> psi;    ///< Stream function per cell, world units across the flow
			std::vector<float> speed;  ///< Flow speed per cell, 1 for free unit flow; 0 on walls
			std::vector<float> foam;   ///< 0 to 1: where the water piles against rock, behind it, and where it squeezes through
			std::vector<uint8_t> water;
			uint32_t iterations = 0;
			float residual = 0.0f;  ///< Largest pressure change in the last sweep, world units
		};

		static constexpr uint32_t MAX_ITERATIONS = 4000;
		static constexpr float TOLERANCE = 0.01f;      ///< Stop once the pressure moves less than this per sweep
		static constexpr float SOR = 1.9f;             ///< Over-relaxation
		static constexpr int32_t SMOOTH_CELLS = 3;     ///< The surface is smoothed over this many cells before its slope is taken
		static constexpr float FLAT_SLOPE = 0.08f;     ///< Below this slope the drive weakens in proportion
		static constexpr float STALL_SPEED = 0.45f;    ///< Slower than this beside rock: water piles up or eddies, and foams
		static constexpr float SQUEEZE_SPEED = 1.35f;  ///< Faster than this: squeezed through a gap, and foams
		static constexpr int32_t ROCK_REACH = 3;       ///< Cells from rock within which stalled water foams

		static Result Solve(const Input& a_input);
	};
}
