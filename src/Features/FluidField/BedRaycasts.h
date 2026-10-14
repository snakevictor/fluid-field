#pragma once

#include <cstdint>
#include <unordered_map>

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
	 * frame. Runs on the main thread under the physics world's read lock.
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

		const Stats& GetStats() const { return stats; }
		/** @brief Counts the cached results; walks every sample, so call it for display only. */
		Counts CountSamples() const;
		std::size_t GetSampleCount() const { return samples.size(); }

	private:
		/** @brief CPU time the rays of one update may take, so moving fast cannot blow the budget. */
		static constexpr double RAY_TIME_BUDGET_MS = 0.07;
		/** @brief Larger allowance for the first updates after a load or teleport, so the area refills quickly. */
		static constexpr double BURST_TIME_BUDGET_MS = 0.5;
		static constexpr uint32_t BURST_UPDATES = 90;
		/** @brief Updates between summary log lines. */
		static constexpr uint32_t LOG_SUMMARY_INTERVAL = 300;
		/** @brief Most hit log lines per summary interval. */
		static constexpr uint32_t MAX_HIT_LOGS_PER_INTERVAL = 40;

		/** @brief First static hit along a vertical ray; non-static hits are stepped through. */
		struct RayHit
		{
			bool hit = false;
			float z = 0.0f;
			RE::COL_LAYER layer = RE::COL_LAYER::kUnidentified;
			const RE::hkpCollidable* collidable = nullptr;
		};

		static uint64_t MakeKey(int32_t a_x, int32_t a_y);
		RayHit CastVertical(RE::hkpWorld* a_world, float a_worldX, float a_worldY, float a_fromZ, float a_toZ);
		Sample Cast(float a_worldX, float a_worldY, float a_surface, bool a_log);
		void LogHit(float a_worldX, float a_worldY, float a_surface, const RayHit& a_hit, const char* a_what);

		std::unordered_map<uint64_t, Sample> samples;
		float spacing = 0.0f;
		uint32_t sweepCursor = 0;
		Stats stats;

		// Sweep bookkeeping: the sweep stops once a full pass around an unmoved centre finds nothing new
		int32_t lastCentre[2]{ INT32_MIN, INT32_MIN };
		int32_t prunedCentre[2]{ INT32_MIN, INT32_MIN };
		uint64_t lastWaterSignature = 0;
		uint32_t newThisPass = 0;
		bool idle = false;
		uint32_t burstUpdates = BURST_UPDATES;

		// Logging
		uint32_t updatesUntilSummary = LOG_SUMMARY_INTERVAL;
		uint32_t hitLogsThisInterval = 0;
		uint64_t raysAtLastSummary = 0;
		double rayMsSinceSummary = 0.0;
	};
}
