#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

struct ID3D11Buffer;

namespace FluidFieldSim
{
	class FallSites;

	/**
	 * @brief D61: our own waterfall sheets, generated per vanilla falls piece and drawn by the engine.
	 *
	 * One sheet per fall, from the upper water (surface A) to the water it falls into (surface B): the
	 * vanilla lit pieces whose lips stand at the same water level of a site are one fall. Their lips (each
	 * piece's top edge: v runs downstream on every vanilla falls mesh), spread across the mean way the
	 * water goes, give the lip line A to B, on the upper water's surface. From the lip, guide columns are marched down
	 * the rock with rays against the game's collision: through the air on a ballistic arc, sliding on the
	 * rock (gravity along it, less friction) while it holds them, and off it again where it falls away,
	 * until they reach the pool. Their landing (C to D) is a result of the march. The columns are filled
	 * in to a dense grid, whose texture runs along the water's travel time, so vanilla's own scroll moves
	 * the texture with the water and nothing resets.
	 *
	 * The grid is given to a clone of the vanilla piece (its own buffers in the piece's vertex layout,
	 * under the same parent), so the engine draws it with vanilla's material in every pass; the vanilla
	 * piece is hidden. Everything is put back on Clear.
	 */
	class FallSheets
	{
	public:
		/** @brief Builds sheets for newly grounded pieces (one a frame) and drops those that went away. Main thread. */
		void Update(const FallSites& a_sites, bool a_enabled);
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
		/** @brief Debug: each sheet's marched columns over the view (yellow in the air, blue on the rock) and its lip line (green). */
		void DrawDebug(ImDrawList* a_drawList) const;

		/** @brief The name our sheets carry, so the falls' own bookkeeping passes them by. */
		static constexpr const char* SHEET_NAME = "FluidFieldSheet";

		static constexpr uint32_t GUIDE_COLUMNS = 32;  ///< Columns marched with rays
		static constexpr uint32_t COLUMNS = 96;        ///< Vertices across the drawn grid
		static constexpr uint32_t ROWS = 72;           ///< Vertices along it
		static constexpr float STEP = 12.0f;           ///< World units per march step
		static constexpr uint32_t MOST_STEPS = 400;
		static constexpr float GRAVITY = 686.0f;        ///< World units per second squared (Skyrim's)
		static constexpr float LIP_SPEED = 150.0f;      ///< World units per second the water leaves the lip at (vanilla's lip scroll, about 100-150)
		static constexpr float MOST_SPEED = 900.0f;     ///< World units per second at most
		static constexpr float FRICTION = 1.2f;         ///< Per second, sliding on rock
		static constexpr float FLOW_DEPTH = 6.0f;       ///< World units the sheet lies above the rock it slides on
		static constexpr float PROBE = 40.0f;           ///< World units above and below the sheet the rock under it is looked for
		static constexpr float POOL_TUCK = 10.0f;       ///< World units under the water level the sheet ends
		static constexpr float SCROLL = 0.15f;          ///< Vanilla's scroll, texture coordinates per second (D56)
		static constexpr float LEVEL_REACH = 64.0f;     ///< A lip this far under a water level is that level's
		static constexpr float LEAST_DROP = 48.0f;      ///< The water below lies at least this far under the lip's
		static constexpr float UPSTREAM = 24.0f;        ///< World units upstream of the lip each column starts, on the upper water
		static constexpr uint32_t DRAPE_ROWS = 48;      ///< Rows of the draped sheet before it is filled in
		static constexpr uint32_t DRAPE_PASSES = 4;     ///< Push-out and smoothing passes
		static constexpr float DRAPE_REACH = 400.0f;    ///< World units downstream and above each point the rock is looked for from
		static constexpr float NEIGHBOUR_PULL = 0.5f;   ///< How much each column's heading follows its neighbours' each step
		static constexpr float STALL_SPEED = 40.0f;     ///< World units per second: water this slow on level rock has stopped (a puddle), and ends
		static constexpr float ACROSS_REPEAT = 512.0f;  ///< World units across per texture repeat

	private:
		struct Sheet
		{
			RE::NiPointer<RE::BSTriShape> clone;
			std::vector<RE::NiPointer<RE::BSGeometry>> originals;  ///< The vanilla pieces it replaces, hidden
			RE::NiPointer<RE::NiNode> parent;
			RE::BSGraphics::TriShape* previousData = nullptr;  ///< The clone's own renderer data, given back before it goes
			RE::BSGraphics::TriShape data{};
			winrt::com_ptr<ID3D11Buffer> vertexBuffer;
			winrt::com_ptr<ID3D11Buffer> indexBuffer;
			std::vector<uint8_t> rawVertices;
			std::vector<uint16_t> rawIndices;
			std::vector<std::vector<RE::NiPoint3>> columns;  ///< Debug: the marched columns
			std::vector<std::vector<uint8_t>> columnRock;
		};

		/** @brief One marched column: world points and the seconds the water has run to each. */
		struct Column
		{
			std::vector<RE::NiPoint3> points;
			std::vector<float> times;
			std::vector<uint8_t> onRock;  ///< Per point: sliding on the rock (1) or in the air (0)
			float u = 0.0f;               ///< The vanilla texture coordinate across, at its lip
		};

		/** @brief One fall: the vanilla pieces it replaces, the upper water's level and the lower's. */
		struct Fall
		{
			std::vector<RE::NiPointer<RE::BSGeometry>> pieces;
			float upper = 0.0f;
			float lower = 0.0f;
			std::vector<float> levels;  ///< Every water level of the site under the upper one (its tiers' and other falls' upper waters)
		};
		std::unique_ptr<Sheet> Build(const FallSites& a_sites, const Fall& a_fall);
		/**
		 * @brief D61: the sheet draped over the rock: a smooth curtain falling from the lip line to the water below,
		 * pushed out onto the rock wherever the rock stands in front of it (rays from outside: downstream and
		 * from above, so they always meet the rock's real surface), smoothed, and laid into the pools it reaches.
		 */
		static void Drape(RE::hkpWorld* a_world, const FallSites& a_sites, const std::vector<RE::NiPoint3>& a_starts, const RE::NiPoint3& a_flow, const Fall& a_fall, std::vector<Column>& a_columns);
		/** @brief The columns marched together, a step at a time, each steered a little by its neighbours so the sheet holds together. */
		static void March(RE::hkpWorld* a_world, const FallSites& a_sites, const std::vector<RE::NiPoint3>& a_starts, const std::vector<RE::NiPoint3>& a_headings, const Fall& a_fall,
			std::vector<Column>& a_columns);
		static void Remove(Sheet& a_sheet);

		std::vector<std::unique_ptr<Sheet>> sheets;
		RE::NiPointer<RE::BSTriShape> testQuad;
		RE::NiPointer<RE::NiNode> testQuadParent;
		std::vector<Fall> pending;  ///< Falls still to build, one a frame
		uint64_t groundVersion = UINT64_MAX;
		uint32_t readyStreams = UINT32_MAX;  ///< The sites' solved flows (their pools) when the sheets were last built
	};
}
