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
	 * @brief D14: water-line obstacles cut from placed references' visual meshes (their CPU copies).
	 *
	 * Each reference crossing water is cut at the water height; fine points inside a closed loop (pillar, hull) or on any
	 * segment (a wall backed by the bank) become solid. Statics don't move, so each is cut once while loaded. Main thread only.
	 */
	class MeshCut
	{
	public:
		/** @brief Points per obstacle block side, matching the obstacle texture upload block. */
		static constexpr int32_t BLOCK = 4;
		/** @brief A solid point's height above the cut, so the water solver reads it as a wall. */
		static constexpr float SOLID_RISE = 64.0f;

		/**
		 * @brief Finds loaded references at the water line and cuts a few of them, within a time budget.
		 * @param a_fineSpacing World units between fine lattice points; a change re-cuts everything.
		 */
		void Update(const WaterSurfaces& a_water, float a_fineSpacing);

		void Clear();

		/** @brief Solid at a fine lattice point; a_top is the height the water solver should see there. */
		bool IsSolid(int32_t a_fineX, int32_t a_fineY, float& a_top) const;

		/** @brief Blocks (fine coordinates of their south-west point) whose solid points changed; the list is emptied. */
		std::vector<std::pair<int32_t, int32_t>> TakeDirtyBlocks();
		/** @brief Every block holding a solid point, for re-uploading after the consumer lost its copy. */
		std::vector<std::pair<int32_t, int32_t>> GetAllBlocks() const;
		/** @brief True after a clear until the next TakeDirtyBlocks, so consumers can drop stale blocks. */
		bool WasCleared() const { return cleared; }

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
		void Cut(RE::TESObjectREFR& a_ref, float a_surface);
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
	};
}
