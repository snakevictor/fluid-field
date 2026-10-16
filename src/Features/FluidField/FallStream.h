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
	 * and its streaks follow the streamlines. With it comes the travel time along the streamlines from
	 * where the water comes in (by fast sweeping), the texture's along coordinate: together they lay a
	 * texture on the flow without shear. Where the falls meet still water (where they come in and
	 * leave), the water foams over MEET_CELLS, masking the join.
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
			std::vector<float> bed;      ///< Rock height per cell
			std::vector<uint8_t> kind;   ///< Cell per cell
		};

		struct Result
		{
			uint32_t width = 0;
			uint32_t height = 0;
			std::vector<float> psi;       ///< Stream function per cell, world units across the flow
			std::vector<float> speed;     ///< Flow speed per cell, 1 for free unit flow; 0 on walls
			std::vector<float> foam;      ///< 0 to 1: where the water piles against rock, behind it, and where it squeezes through
			std::vector<float> rockFoam;  ///< The foam from rock alone (not where the falls meet still water), where the water strikes it (D35)
			std::vector<uint8_t> water;
			std::vector<float> vx, vy;  ///< Flow velocity per cell (unit drive)
			std::vector<float> level;   ///< The water surface, lightly smoothed over the water (for the mesh)
			std::vector<float> depth;   ///< Water surface above the rock
			std::vector<float> travel;  ///< Seconds the water takes to reach each cell from where it comes in, at BASE_SPEED
			std::vector<float> head;    ///< The height the water here has fallen from (D33): the highest surface upstream, less what it loses on the way
			std::vector<float> edge;    ///< 0 at the water's edge (beside rock or a bank) to 1 EDGE_CELLS in
			uint32_t iterations = 0;
			float residual = 0.0f;  ///< Largest pressure change in the last sweep, world units
		};

		static constexpr uint32_t MAX_ITERATIONS = 12000;
		static constexpr uint32_t PSI_SWEEPS = 400;      ///< Least-squares sweeps over the stream function after integrating it
		static constexpr float TOLERANCE = 0.01f;        ///< Stop once the pressure moves less than this per sweep
		static constexpr float SOR = 1.9f;               ///< Over-relaxation
		static constexpr int32_t SMOOTH_CELLS = 3;       ///< The surface is smoothed over this many cells before its slope is taken
		static constexpr float FLAT_SLOPE = 0.08f;       ///< Below this slope the drive weakens in proportion
		static constexpr float STALL_SPEED = 0.45f;      ///< Slower than this beside rock: water piles up or eddies, and foams
		static constexpr float SQUEEZE_SPEED = 1.35f;    ///< Faster than this: squeezed through a gap, and foams
		static constexpr int32_t ROCK_REACH = 3;         ///< Cells from rock within which stalled water foams
		static constexpr float BASE_SPEED = 420.0f;      ///< World units per second of free-flowing water (unit drive)
		static constexpr float SLOWEST = 0.25f;          ///< Slowest speed counted in the travel time, as a fraction of free flow
		static constexpr uint32_t TRAVEL_ROUNDS = 24;    ///< Rounds of four sweeps for the travel time
		static constexpr int32_t EDGE_CELLS = 2;         ///< Cells over which the water fades in from its edge
		static constexpr int32_t MEET_CELLS = 2;         ///< Cells of foam where the falls meet still water (where they come in and leave)
		static constexpr float HEAD_LOSS = 0.35f;        ///< Height the water's head loses per world unit it runs (friction): a steep drop keeps its speed, a long run slows
		static constexpr float HEAD_MOST = 900.0f;       ///< Most height a head stands above the surface
		static constexpr uint32_t COURSE_SWEEPS = 6000;  ///< Most sweeps for the course potential (D34)
		static constexpr float COURSE_FORWARD = 0.35f;   ///< Least speed along the course, as a fraction of free flow: the water never runs back

		static Result Solve(const Input& a_input);
	};
}
