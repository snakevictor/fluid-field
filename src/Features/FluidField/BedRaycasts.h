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
	 * @brief Riverbed and obstacle heights from downward Havok raycasts where there is water (D7).
	 *
	 * Landscape heights miss placed meshes (rocks, banks, piers, riverbed meshes). A ray is cast down at each lattice point
	 * with water; a hit above the water is probed from below to tell an overhead (deck, arch: water flows under) from a
	 * solid (rock, pier). Non-static hits are stepped through. Statics don't move, so results are cached. The coarse
	 * lattice takes only the first hit; then squares around meshes near the water within REFINE_RADIUS are refined
	 * REFINE x REFINE, probing overheads so pillars and gaps resolve. A time limit is checked before every ray.
	 * Main thread, under the physics world's read lock.
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

		/**
		 * @brief Casts up to a_maxRays new rays, within RAY_TIME_BUDGET_MS, for lattice points around the centre.
		 * Idle while every point around an unmoved centre is resolved.
		 * @param a_spacing Lattice spacing in world units.
		 * @param a_extent World units per side of the square to cover.
		 */
		void Update(const LandscapeCache& a_landscape, const WaterSurfaces& a_water, float a_centreX, float a_centreY,
			float a_extent, float a_spacing, uint32_t a_maxRays);

		void Clear();

		/** @brief Cached result at the lattice point nearest to a world position, or null when not cast yet. */
		const Sample* Find(float a_worldX, float a_worldY) const;

		/** @brief Refined squares completed since the last call, for upload; the list is emptied. */
		std::vector<FineBlock> TakeDirtyBlocks();
		/** @brief Marks every refined square for upload again, after the consumer lost its copy. */
		void MarkAllRefinedDirty();
		/** @brief World units between fine points. */
		float GetFineSpacing() const { return spacing / static_cast<float>(REFINE); }
		/** @brief True after a clear until the next TakeDirtyBlocks, so consumers can drop stale detail. */
		bool WasCleared() const { return cleared; }
		/**
		 * @brief Refined result at a fine lattice point as an obstacle texel (height, kind code: 1 landscape, 2 mesh).
		 * @return False when that point has not been refined.
		 */
		bool FindFine(int32_t a_fineX, int32_t a_fineY, float& a_height, float& a_kind) const;
		/** @brief With mesh-cut obstacles, every mesh above the water is looked under, none judged solid. Clears results on change. */
		void SetOverheadsAlwaysOpen(bool a_open);

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
		Sample Cast(float a_worldX, float a_worldY, float a_surface, bool a_probeOverheads, bool& a_aborted);
		/**
		 * @brief Samples one point: no water, a buried plane, or a ray.
		 * @return False if the deadline passed before the point was resolved; nothing is stored then.
		 */
		bool Resolve(const LandscapeCache& a_landscape, const WaterSurfaces& a_water, float a_worldX, float a_worldY, bool a_probeOverheads,
			Sample& a_sample, float& a_surface);
		/** @brief Queues the lattice squares around a coarse point for refinement. */
		void QueueRefinement(int32_t a_x, int32_t a_y);
		/** @brief Casts the fine points of one square. Returns false if the deadline passed before it was finished. */
		bool RefineSquare(const LandscapeCache& a_landscape, const WaterSurfaces& a_water, int32_t a_squareX, int32_t a_squareY);

		std::unordered_map<uint64_t, Sample> samples;
		std::unordered_map<uint64_t, Sample> fine;    ///< Refined points, keyed by fine lattice coordinates
		std::unordered_set<uint64_t> refinedSquares;  ///< Squares queued or done, keyed by coarse lattice coordinates
		std::deque<std::pair<int32_t, int32_t>> refineQueue;
		/** @brief Squares waiting until the player is within REFINE_RADIUS; coarse rays are cached, so nothing else re-queues them. */
		std::vector<std::pair<int32_t, int32_t>> deferredSquares;
		std::vector<std::pair<int32_t, int32_t>> dirtySquares;
		bool cleared = true;
		bool overheadsAlwaysOpen = false;
		float spacing = 0.0f;
		uint32_t sweepCursor = 0;

		// Sweep bookkeeping: the sweep stops once a full pass around an unmoved centre finds nothing new
		int32_t lastCentre[2]{ INT32_MIN, INT32_MIN };
		int32_t prunedCentre[2]{ INT32_MIN, INT32_MIN };
		uint64_t lastWaterSignature = 0;
		uint32_t newThisPass = 0;
		bool idle = false;
		bool coarseResolved = false;  ///< A full coarse pass around the current centre found nothing new
		Clock::time_point deadline;   ///< This update's ray time limit
		uint32_t burstUpdates = BURST_UPDATES;
	};
}
