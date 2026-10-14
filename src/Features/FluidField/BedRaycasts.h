#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace FluidFieldSim
{
	class LandscapeCache;
	class WaterSurfaces;

	/**
	 * @brief Riverbed and obstacle heights from downward Havok raycasts where there is water (R1, D7 stage 4).
	 *
	 * Landscape heights miss placed meshes: rocks, banks, bridge piers and riverbed meshes. On a
	 * lattice around the player, wherever a water plane lies above (or near) the landscape, a ray
	 * is cast down from above the surface. If it first hits something above the water, a short ray
	 * up from just under the surface tells an overhead structure (a deck or arch with air beneath,
	 * which water flows under) from a solid rising out of the water (a rock or pier, an obstacle).
	 * Non-static hits (the water's own collision, actors, clutter) are stepped through.
	 * Results are cached per lattice point, since statics do not move, and a few rays are cast per
	 * frame. The coarse lattice comes first and takes only the first hit (the coarse map ignores meshes
	 * rising out of the water anyway). Once it is complete, lattice squares around meshes near the
	 * water within REFINE_RADIUS of the player are refined with REFINE x REFINE rays each; those rays
	 * also probe for overhead structures, so pillars and the gaps between them are resolved. A hard
	 * time limit is checked before every ray. Runs on the main thread under the physics world's read lock.
	 */
	class BedRaycasts
	{
	public:
		enum class HitKind : uint8_t
		{
			Skipped,  ///< The water plane here lies under the landscape; no ray cast
			Terrain,  ///< Landscape collision (kGround in this game, no reference)
			Static,   ///< A placed static mesh (rock, bank, pier, riverbed mesh)
			NoHit,    ///< Nothing static within reach below the surface
			NoWater   ///< No water plane here at all; no ray cast
		};

		struct Sample
		{
			float height = 0.0f;  ///< Hit height; meaningful for Terrain and Static
			HitKind kind = HitKind::Skipped;
			bool overhead = false;  ///< An overhead structure was looked under to find this bed
			bool refined = false;   ///< A fine point; coarse points never probe for overheads
		};

		/** @brief Fine points per lattice spacing, along each axis. */
		static constexpr int32_t REFINE = 4;

		/**
		 * @brief One refined lattice square: REFINE x REFINE fine points, rows south to north.
		 * Each texel is (height, fine x, fine y, kind code): kind 1 is landscape, 2 a mesh, 0 none.
		 * The fine coordinates let a consumer reject stale texels in a wrapped texture.
		 */
		struct FineBlock
		{
			int32_t originX = 0;  ///< Fine lattice coordinates of the south-west point
			int32_t originY = 0;
			std::array<float, REFINE * REFINE * 4> texels{};
		};

		struct Stats
		{
			uint64_t raysCast = 0;
			uint32_t raysLastFrame = 0;
			uint64_t stepThroughs = 0;  ///< Non-static hits stepped through
			uint64_t overheads = 0;     ///< Points where an overhead structure was looked under
		};

		/** @brief How the currently cached lattice points resolved. */
		struct Counts
		{
			uint32_t terrain = 0;
			uint32_t statics = 0;
			uint32_t noHit = 0;
			uint32_t skipped = 0;
			uint32_t noWater = 0;
		};

		/**
		 * @brief Casts up to a_maxRays new rays (and at most RAY_TIME_BUDGET_MS of them) for lattice points around the centre.
		 * Does nothing while every point around an unmoved centre is already resolved.
		 * @param a_spacing Lattice spacing in world units.
		 * @param a_extent World units per side of the square to cover.
		 * @param a_log Log what each ray hits (rate-limited) and a summary every few seconds.
		 */
		void Update(const LandscapeCache& a_landscape, const WaterSurfaces& a_water, float a_centreX, float a_centreY,
			float a_extent, float a_spacing, uint32_t a_maxRays, bool a_log);

		void Clear();

		/** @brief Cached result at the lattice point nearest to a world position, or null when not cast yet. */
		const Sample* Find(float a_worldX, float a_worldY) const;
		/** @brief The refined result nearest to a world position where one exists, otherwise the coarse one. */
		const Sample* FindDetail(float a_worldX, float a_worldY) const;

		/** @brief Refined squares completed since the last call, for upload; the list is emptied. */
		std::vector<FineBlock> TakeDirtyBlocks();
		/** @brief Marks every refined square for upload again, after the consumer lost its copy. */
		void MarkAllRefinedDirty();
		/** @brief World units between fine points. */
		float GetFineSpacing() const { return spacing / static_cast<float>(REFINE); }
		/** @brief True after a clear until the next TakeDirtyBlocks, so consumers can drop stale detail. */
		bool WasCleared() const { return cleared; }

		const Stats& GetStats() const { return stats; }
		/** @brief Counts the cached results; walks every sample, so call it for display only. */
		Counts CountSamples() const;
		std::size_t GetSampleCount() const { return samples.size(); }
		/** @brief True during the fast refill after a load or teleport, until the coarse lattice is complete. */
		bool IsRefilling() const { return burstUpdates > 0 && !coarseResolved; }

	private:
		/** @brief CPU time the rays of one update may take, so moving fast cannot blow the budget. */
		static constexpr double RAY_TIME_BUDGET_MS = 0.07;
		/** @brief Larger allowance for the first updates after a load or teleport, so the area refills quickly. */
		static constexpr double BURST_TIME_BUDGET_MS = 0.5;
		static constexpr uint32_t BURST_UPDATES = 90;
		/** @brief Squares are refined only this close to the player, where fine detail is visible. */
		static constexpr float REFINE_RADIUS = 1024.0f;
		/** @brief Updates between summary log lines. */
		static constexpr uint32_t LOG_SUMMARY_INTERVAL = 300;
		/** @brief Most hit log lines per summary interval. */
		static constexpr uint32_t MAX_HIT_LOGS_PER_INTERVAL = 40;

		/** @brief First static hit along a vertical ray; non-static hits are stepped through. */
		struct RayHit
		{
			bool hit = false;
			float z = 0.0f;
			float normalZ = 0.0f;  ///< Vertical component of the hit surface's normal
			RE::COL_LAYER layer = RE::COL_LAYER::kUnidentified;
			const RE::hkpCollidable* collidable = nullptr;
		};

		static uint64_t MakeKey(int32_t a_x, int32_t a_y);
		using Clock = std::chrono::steady_clock;

		/** @brief Casts until the first static hit; `aborted` is set if the deadline passed first. */
		RayHit CastVertical(RE::hkpWorld* a_world, float a_worldX, float a_worldY, float a_fromZ, float a_toZ, bool& a_aborted);
		/** @brief One bed sample. With a_probeOverheads, a mesh above the water is checked for air beneath it. */
		Sample Cast(float a_worldX, float a_worldY, float a_surface, bool a_log, bool a_probeOverheads, bool& a_aborted);
		/**
		 * @brief Samples one point: no water, a buried plane, or a ray.
		 * @return False if the deadline passed before the point was resolved; nothing is stored then.
		 */
		bool Resolve(const LandscapeCache& a_landscape, const WaterSurfaces& a_water, float a_worldX, float a_worldY, bool a_log, bool a_probeOverheads,
			Sample& a_sample, float& a_surface);
		/** @brief Queues the lattice squares around a coarse point for refinement. */
		void QueueRefinement(int32_t a_x, int32_t a_y);
		/** @brief Casts the fine points of one square. Returns false if the deadline passed before it was finished. */
		bool RefineSquare(const LandscapeCache& a_landscape, const WaterSurfaces& a_water, int32_t a_squareX, int32_t a_squareY, bool a_log);
		void LogHit(float a_worldX, float a_worldY, float a_surface, const RayHit& a_hit, const char* a_what);

		std::unordered_map<uint64_t, Sample> samples;
		std::unordered_map<uint64_t, Sample> fine;    ///< Refined points, keyed by fine lattice coordinates
		std::unordered_set<uint64_t> refinedSquares;  ///< Squares queued or done, keyed by coarse lattice coordinates
		std::deque<std::pair<int32_t, int32_t>> refineQueue;
		/** @brief Squares waiting until the player is within REFINE_RADIUS; coarse rays are cached, so nothing else re-queues them. */
		std::vector<std::pair<int32_t, int32_t>> deferredSquares;
		std::vector<std::pair<int32_t, int32_t>> dirtySquares;
		bool cleared = true;
		float spacing = 0.0f;
		uint32_t sweepCursor = 0;
		Stats stats;

		// Sweep bookkeeping: the sweep stops once a full pass around an unmoved centre finds nothing new
		int32_t lastCentre[2]{ INT32_MIN, INT32_MIN };
		int32_t prunedCentre[2]{ INT32_MIN, INT32_MIN };
		uint64_t lastWaterSignature = 0;
		uint32_t newThisPass = 0;
		bool idle = false;
		bool coarseResolved = false;  ///< A full coarse pass around the current centre found nothing new
		Clock::time_point deadline;   ///< This update's ray time limit
		uint32_t burstUpdates = BURST_UPDATES;

		// Logging
		uint32_t updatesUntilSummary = LOG_SUMMARY_INTERVAL;
		uint32_t hitLogsThisInterval = 0;
		uint64_t raysAtLastSummary = 0;
		double rayMsSinceSummary = 0.0;
	};
}
