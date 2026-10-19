#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

namespace FluidFieldSim
{
	class FallSites;

	/**
	 * @brief D61, D63: our own waterfall sheets, one per fall, built by the engine and drawn with vanilla's material.
	 *
	 * One sheet per fall, from the upper water (surface A) to the water it falls into (surface B): the
	 * vanilla lit pieces whose lips stand at the same water level of a site are one fall. Their lips
	 * (each piece's top edge: v runs downstream on every vanilla falls mesh), spread across the mean way
	 * the water goes, give the lip line A to B.
	 *
	 * The sheet (Drape):
	 * - its top row is stitched to the upper water's edge (the brink), with an overlap row upstream on it;
	 * - from there it falls on the ballistic arc of the water's speed at the top (from the vanilla pieces'
	 *   texture scroll), down past the water below by a plunge depth that grows with the fall's speed and
	 *   never reaches under the bed;
	 * - each vertex casts a ray of PROBE_LENGTH under it (against the sheet's facing); rock within it lifts
	 *   the vertex by a share of how far the rock reaches into the ray, and nothing is left inside rock;
	 * - the sheet is smoothed, more for a faster, thicker fall, which small bumps under it disturb less;
	 * - each row's edges are pulled out (or in) to the channel's walls, found with rays to either side; where
	 *   no wall is found, that edge curls back toward the cliff in a C, so a fall off a bare cliff is not a
	 *   thin card.
	 *
	 * The grid is built by the engine's own mesh builder (D62), with a copy of the fall's vanilla lit
	 * material, under the vanilla pieces' parent; the vanilla pieces are hidden. Everything is put back on Clear.
	 */
	class FallSheets
	{
	public:
		/** @brief Builds sheets for newly grounded falls (one a frame) and drops those that went away. Main thread. */
		void Update(const FallSites& a_sites, bool a_enabled, float a_arcScale);
		/** @brief Removes every sheet and shows vanilla's pieces again. Main thread. */
		void Clear();
		std::size_t GetCount() const { return sheets.size(); }

		/**
		 * @brief D62 probe: builds a test quad with the engine's own builder (77465, a BSTriShape from arrays) in
		 * front of the player, gives it a copy of a vanilla falls piece's lit material, and attaches it to the
		 * player's cell, so we can see whether the engine draws it in every pass. A second call removes it.
		 * Main thread. 1.7.104 only.
		 */
		void ToggleTestQuad(const FallSites& a_sites);
		/** @brief Debug: each sheet's draped columns over the view (yellow in the air, blue lifted by rock) and its lip line (green). */
		void DrawDebug(ImDrawList* a_drawList) const;

		/** @brief The name our sheets carry, so the falls' own bookkeeping passes them by. */
		static constexpr const char* SHEET_NAME = "FluidFieldSheet";

		static constexpr uint32_t GUIDE_COLUMNS = 32;     ///< Columns of the draped sheet
		static constexpr uint32_t DRAPE_ROWS = 48;        ///< Rows of the draped sheet
		static constexpr uint32_t COLUMNS = 96;           ///< Vertices across the drawn grid
		static constexpr uint32_t ROWS = 72;              ///< Vertices along it
		static constexpr float GRAVITY = 686.0f;          ///< World units per second squared (Skyrim's)
		static constexpr float SCROLL = 0.15f;            ///< Vanilla's scroll, texture coordinates per second (D56)
		static constexpr float SLOWEST_TOP = 200.0f;      ///< World units per second the water at the top moves at least (vanilla's lip spray leaves at 210)...
		static constexpr float FASTEST_TOP = 900.0f;      ///< ...and at most
		static constexpr float LEVEL_REACH = 64.0f;       ///< A lip this far under a water level is that level's
		static constexpr float LEAST_DROP = 48.0f;        ///< The water below lies at least this far under the lip's
		static constexpr float UPSTREAM = 24.0f;          ///< World units upstream of the lip each column starts looking for the brink
		static constexpr float BRINK_REACH = 600.0f;      ///< ...and how far downstream it looks
		static constexpr float OVERLAP = 32.0f;           ///< World units the sheet's top row reaches back over the upper water
		static constexpr float PROBE_LENGTH = 24.0f;      ///< World units each vertex looks under itself for rock
		static constexpr float PROBE_REACH = 400.0f;      ///< World units out from each vertex its ray starts (outside any rock)
		static constexpr float CLEARANCE = 4.0f;          ///< World units the sheet keeps above the rock at least
		static constexpr float SLOW_LIFT = 0.75f;         ///< Share of the rock's reach into the ray a slow fall is lifted by...
		static constexpr float FAST_LIFT = 0.4f;          ///< ...and a fast one
		static constexpr int SLOW_SMOOTHING = 2;          ///< Smoothing passes for a slow, thin fall...
		static constexpr int FAST_SMOOTHING = 8;          ///< ...and a fast, thick one
		static constexpr float PLUNGE_PER_SPEED = 0.08f;  ///< World units deep the sheet plunges per world unit per second it lands at...
		static constexpr float LEAST_PLUNGE = 8.0f;       ///< ...at least...
		static constexpr float MOST_PLUNGE = 240.0f;      ///< ...and at most
		static constexpr float BED_CLEARANCE = 4.0f;      ///< World units the plunge stays above the bed
		static constexpr float WALL_REACH = 1200.0f;      ///< World units to either side of a row its walls are looked for
		static constexpr float WALL_TUCK = 6.0f;          ///< World units the edges reach into the wall
		static constexpr float CURL = 0.3f;               ///< How far back a free edge curls, as a share of the sheet's half width...
		static constexpr float MOST_CURL = 120.0f;        ///< ...in world units at most
		static constexpr float CURL_BAND = 0.25f;         ///< Share of the sheet's width that curls at a free edge
		static constexpr float ACROSS_REPEAT = 512.0f;    ///< World units across per texture repeat

	private:
		struct Sheet
		{
			RE::NiPointer<RE::BSTriShape> shape;
			std::vector<RE::NiPointer<RE::BSGeometry>> originals;  ///< The vanilla pieces it replaces, hidden
			RE::NiPointer<RE::NiNode> parent;
			std::vector<std::vector<RE::NiPoint3>> columns;  ///< Debug: the draped columns
			std::vector<std::vector<uint8_t>> columnRock;
		};

		/** @brief One draped column: world points and the seconds the water has run to each. */
		struct Column
		{
			std::vector<RE::NiPoint3> points;
			std::vector<float> times;
			std::vector<uint8_t> onRock;  ///< Per point: lifted by rock (1) or free (0)
			float u = 0.0f;               ///< The texture coordinate across
		};

		/** @brief One fall: the vanilla pieces it replaces, the upper water's level and the lower's. */
		struct Fall
		{
			std::vector<RE::NiPointer<RE::BSGeometry>> pieces;
			float upper = 0.0f;
			float lower = 0.0f;
			float speed = 150.0f;       ///< World units per second the water at the top moves at
			std::vector<float> levels;  ///< Every water level of the site under the upper one (its tiers' and other falls' upper waters)
		};
		std::unique_ptr<Sheet> Build(const FallSites& a_sites, const Fall& a_fall);
		/** @brief D63: how many times a ray crosses rock (anything solid but the falls' own collision) on its way. */
		static int RockCrossings(RE::hkpWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to);
		/** @brief The sheet draped from the brink over the rock to the water below (see the class description). */
		static void Drape(RE::hkpWorld* a_world, const FallSites& a_sites, const std::vector<RE::NiPoint3>& a_starts, const RE::NiPoint3& a_flow, const Fall& a_fall, std::vector<Column>& a_columns);
		static void Remove(Sheet& a_sheet);

		std::vector<std::unique_ptr<Sheet>> sheets;
		RE::NiPointer<RE::BSTriShape> testQuad;
		RE::NiPointer<RE::NiNode> testQuadParent;
		std::vector<Fall> pending;  ///< Falls still to build, one a frame
		uint64_t groundVersion = UINT64_MAX;
		uint32_t readyStreams = UINT32_MAX;  ///< The sites' solved flows (their pools) when the sheets were last built
		float arcScale = 1.0f;               ///< The arc's speed scale the sheets were built with
	};
}
