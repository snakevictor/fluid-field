#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace FluidFieldSim
{
	class WaterSurfaces;

	/**
	 * @brief Water-line obstacles cut from the visual meshes of placed references (D14, D15).
	 *
	 * Static meshes keep CPU copies of their vertices and indices (BSGraphics::TriShape
	 * rawVertexData / rawIndexData). For each loaded reference that crosses a water surface,
	 * every triangle is cut by the horizontal plane at the water height; the segments are chained
	 * into loops, and the fine lattice points inside a closed loop (a pillar, a post, a hull) or
	 * on any segment (a wall face whose other side is under the bank) become solid. Statics do
	 * not move, so each reference is cut once while it stays loaded. Runs on the main thread.
	 */
	class MeshCut
	{
	public:
		struct Stats
		{
			uint32_t references = 0;  ///< References cut and currently loaded
			uint32_t shapes = 0;
			uint32_t shapesWithoutData = 0;  ///< Shapes skipped: no CPU vertex data, skinned or dynamic
			uint32_t segments = 0;
			uint32_t closedLoops = 0;
			uint32_t solidPoints = 0;  ///< Distinct solid fine points
			uint32_t pending = 0;      ///< References waiting to be cut
			double lastUpdateMs = 0.0;
		};

		/** @brief Points per obstacle block side, matching the obstacle texture upload block. */
		static constexpr int32_t BLOCK = 4;
		/** @brief A solid point's height above the cut, so the water solver reads it as a wall. */
		static constexpr float SOLID_RISE = 64.0f;

		/**
		 * @brief Finds loaded references at the water line and cuts a few of them, within a time budget.
		 * @param a_fineSpacing World units between fine lattice points; a change re-cuts everything.
		 * @param a_log Log each reference cut (form ID, model, loops, time).
		 */
		void Update(const WaterSurfaces& a_water, float a_fineSpacing, bool a_log);

		void Clear();

		/** @brief Solid at a fine lattice point; a_top is the height the water solver should see there. */
		bool IsSolid(int32_t a_fineX, int32_t a_fineY, float& a_top) const;
		/** @brief Solid at the fine lattice point nearest to a world position. */
		bool IsSolidAt(float a_worldX, float a_worldY) const;

		/** @brief Blocks (fine coordinates of their south-west point) whose solid points changed; the list is emptied. */
		std::vector<std::pair<int32_t, int32_t>> TakeDirtyBlocks();
		/** @brief Every block holding a solid point, for re-uploading after the consumer lost its copy. */
		std::vector<std::pair<int32_t, int32_t>> GetAllBlocks() const;
		/** @brief True after a clear until the next TakeDirtyBlocks, so consumers can drop stale blocks. */
		bool WasCleared() const { return cleared; }

		const Stats& GetStats() const { return stats; }
		float GetFineSpacing() const { return fineSpacing; }

	private:
		/** @brief Updates between scans of the loaded cells for references that arrived or left. */
		static constexpr uint32_t SCAN_INTERVAL = 30;
		/** @brief CPU time the cuts of one update may take; one reference is always finished once started. */
		static constexpr double TIME_BUDGET_MS = 0.5;

		struct Solid
		{
			uint32_t count = 0;  ///< References marking this point
			float top = 0.0f;
		};

		struct CutRecord
		{
			std::vector<uint64_t> points;  ///< Fine points this reference made solid
		};

		static uint64_t MakeKey(int32_t a_x, int32_t a_y);
		static std::pair<int32_t, int32_t> SplitKey(uint64_t a_key);

		/** @brief Reconciles the cut set with the references loaded now: queues arrivals, removes departures. */
		void Scan(const WaterSurfaces& a_water);
		/** @brief Cuts one reference and adds its solid points. */
		void Cut(RE::TESObjectREFR& a_ref, float a_surface, bool a_log);
		void AddPoint(uint64_t a_key, float a_top);
		void RemoveRecord(const CutRecord& a_record);
		void MarkBlockDirty(uint64_t a_pointKey);

		struct Pending
		{
			RE::ObjectRefHandle handle;  ///< Resolved just before cutting, in case the reference unloaded meanwhile
			uint32_t formID = 0;
			float surface = 0.0f;
		};

		std::unordered_map<uint32_t, CutRecord> cut;  ///< Keyed by form ID; checked against the loaded set every scan
		std::deque<Pending> queue;
		std::unordered_set<uint32_t> queued;
		std::unordered_map<uint64_t, Solid> solids;
		std::unordered_set<uint64_t> dirtyBlocks;
		float fineSpacing = 0.0f;
		uint32_t updatesUntilScan = 0;
		bool cleared = true;
		Stats stats;
	};
}
