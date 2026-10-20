#pragma once

#include <array>
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
		/** @brief D64 debug views, drawn over everything (through the ground, so what runs under it shows). */
		enum class DebugView : int
		{
			Off,
			Wireframe,  ///< The sheet's grid (yellow; red where a vertex lies under the captured ground)
			Lip,        ///< The detected edge of the upper water (magenta), the seeds the columns start from (green), vanilla's lips (cyan), and the routes (orange in the air, blue on the ground)
			Normals,    ///< The sheet's faces coloured by their normal, as a normal map (x red, y green, z blue)
			All
		};
		/** @brief Debug: draws the chosen views of every sheet over the view. */
		void DrawDebug(ImDrawList* a_drawList, DebugView a_view) const;

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
		static constexpr float POOL_TUCK = 10.0f;         ///< D64: world units under the water below a column ends
		static constexpr float EDGE_DROP = 8.0f;          ///< D64: the ground this far under the upper water's surface beside it is where the water spills over
		static constexpr float EDGE_NEAR_LIPS = 96.0f;    ///< D64: only edge cells this near a vanilla lip are the fall's (other spills are other waters')
		static constexpr float EDGE_DOWNSTREAM = 0.3f;    ///< D64: ...and only where the water spills the way the fall goes
		static constexpr float BOUND_MARGIN = 192.0f;     ///< D64: world units round the vanilla pieces a column may run before it ends
		static constexpr float FOOT_REACH = 24.0f;        ///< D64: a column this near the vanilla pieces' foot has landed
		static constexpr float BREAK_LENGTH = 4.0f;       ///< D64: a grid edge this many times its column spacing at the lip is a tear: no faces there
		static constexpr float LIP_UPSTREAM = 300.0f;     ///< D65: world units upstream of vanilla's lips the lip is looked for from...
		static constexpr float LIP_DOWNSTREAM = 900.0f;   ///< ...and how far past them
		static constexpr float RIVER_DEPTH = 160.0f;      ///< D64: ground no deeper than this under the upper water is its riverbed: the column rides the surface over it
		static constexpr float LANDED = 80.0f;            ///< D64: a column on the ground this near the lower water's level has landed in it
		static constexpr float STEER_BELOW = 24.0f;       ///< D64: a column is steered by the ground only once it is this far under the upper water
		static constexpr float FILL_STEP = 0.05f;         ///< D64: world units each filled cell stands over the one it drains to
		static constexpr float WATER_DEPTH = 12.0f;       ///< D66: world units the sheet rides over the ground (its thickness) at the top's least speed...
		static constexpr float MOST_DEPTH = 48.0f;        ///< ...and at most, for a fast fall
		static constexpr float LEVEL_SAME = 8.0f;         ///< D66: water planes this near in height are one water
		static constexpr float SPILL_OUT = 48.0f;         ///< D66: world units past a spill cell the ground must lie LEAST_DROP under the water (a drop, not a bank)
		static constexpr float SPLIT_ROCK = 256.0f;       ///< D66: a stretch of the lip this long with no spill (a big rock) splits it into two falls; shorter, one sheet passes it
		static constexpr float TRIGGER_REACH = 256.0f;    ///< D66: a spill is a fall only with a vanilla lip this near (vanilla is the trigger)
		static constexpr float PIECE_REACH = 1500.0f;     ///< D66: world units from a fall's lip a vanilla piece may lie and still be one of its segments
		static constexpr float BAND = 32.0f;              ///< D66: world units of height per band of vanilla's width
		static constexpr float SPACING_PULL = 0.25f;      ///< D66: how much each column is drawn, per step, to its place across the sheet's width
		static constexpr float ROUTE_STEP = 8.0f;         ///< D64: world units per tracing step
		static constexpr uint32_t ROUTE_STEPS = 600;
		static constexpr float STEER = 0.35f;          ///< D64: how much a slow column's heading turns to the downhill flow each step (less when fast)
		static constexpr float SLIDE_FRICTION = 0.8f;  ///< D64: per second, sliding on the ground

	private:
		struct Sheet
		{
			RE::NiPointer<RE::BSTriShape> shape;
			std::vector<RE::NiPointer<RE::BSGeometry>> originals;  ///< The vanilla pieces it replaces, hidden
			RE::NiPointer<RE::NiNode> parent;
			std::vector<std::vector<RE::NiPoint3>> columns;  ///< Debug: the draped columns
			std::vector<std::vector<uint8_t>> columnRock;
			std::vector<RE::NiPoint3> grid;                       ///< Debug: the drawn grid in world space, ROWS x COLUMNS
			std::vector<RE::NiPoint3> normals;                    ///< ...and its normals
			std::vector<uint8_t> underground;                     ///< ...and where a vertex lies under the captured ground
			std::vector<RE::NiPoint3> edge;                       ///< Debug: the detected edge of the upper water
			std::vector<RE::NiPoint3> seeds;                      ///< ...the columns' starts on it
			std::vector<RE::NiPoint3> lips;                       ///< ...and vanilla's lips
			std::vector<std::array<RE::NiPoint3, 2>> waterEdges;  ///< D65 debug: the upper water mesh's open edges near the fall (white)
			std::vector<RE::NiPoint3> waterLip;                   ///< D65 debug: where the upper water meets the drop (red)
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
			float speed = 150.0f;  ///< World units per second the water at the top moves at
			uint32_t siteId = 0;
			std::vector<RE::NiPoint3> lip;             ///< D66: the upper water's cells it spills from, at its surface
			RE::NiPoint3 flow;                         ///< D66: the way it spills, level
			std::vector<std::array<float, 5>> waters;  ///< D66: the site's water planes (min x, min y, max x, max y, height)
			std::vector<float> bandLo, bandHi;         ///< D66: vanilla's extent across per BAND under the upper water (lo > hi: none there)
		};
		std::unique_ptr<Sheet> Build(const FallSites& a_sites, const Fall& a_fall);
		/**
		 * @brief D65: the upper water's own mesh near a fall: every engine water object at its level, its triangles in
		 * the world, and the edges of them only one triangle uses (the mesh's border). Logged once per fall.
		 */
		static void UpperWaterMesh(float a_level, const RE::NiPoint3& a_near, float a_reach, std::vector<std::array<RE::NiPoint3, 3>>& a_triangles,
			std::vector<std::array<RE::NiPoint3, 2>>& a_edges);
		/** @brief D63: how many times a ray crosses rock (anything solid but the falls' own collision) on its way. */
		static int RockCrossings(RE::hkpWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to);
		/**
		 * @brief D64: the columns routed over the site's captured ground, from the upper water's real edge to the water
		 * below: the ground's hollows filled so every cell drains to the lower water, the water released along the
		 * edge near the fall's lips and spread downhill, each column traced from the edge with the edge's speed, steered
		 * by the downhill flow (less when fast), thrown on an arc off drops, never lower than the ground plus the water's
		 * depth, ending in the water below. False where the site's ground is not known.
		 */
		static bool Route(const FallSites& a_sites, const Fall& a_fall, std::vector<Column>& a_columns, std::vector<RE::NiPoint3>* a_edge = nullptr,
			std::vector<RE::NiPoint3>* a_seeds = nullptr);
		/** @brief The sheet draped from the brink over the rock to the water below (see the class description). */
		static void Drape(RE::hkpWorld* a_world, const FallSites& a_sites, const std::vector<RE::NiPoint3>& a_starts, const RE::NiPoint3& a_flow, const Fall& a_fall, std::vector<Column>& a_columns);
		static void Remove(Sheet& a_sheet);

		std::vector<std::unique_ptr<Sheet>> sheets;
		RE::NiPointer<RE::BSTriShape> testQuad;
		RE::NiPointer<RE::NiNode> testQuadParent;
		/** @brief D66: the falls of every solved site: one per stretch of a water plane's border it spills over (with a vanilla lip near), down to the next water. */
		void FindFalls(const FallSites& a_sites);
		std::vector<Fall> pending;  ///< Falls still to build, one a frame
		uint64_t groundVersion = UINT64_MAX;
		uint32_t readyStreams = UINT32_MAX;  ///< The sites' solved flows (their pools) when the sheets were last built
		float arcScale = 1.0f;               ///< The arc's speed scale the sheets were built with
	};
}
