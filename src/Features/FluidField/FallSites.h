#pragma once

#include "FallCapture.h"
#include "FallStream.h"
#include "FallSurfaceRenderer.h"
#include "ShallowWaterSolver.h"
#include "WindSolver.h"

#include <array>
#include <cstdint>
#include <future>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ImDrawList;

namespace FluidFieldSim
{
	class WaterSurfaces;

	/**
	 * @brief Waterfall sites reconstructed from vanilla's layout (D25, stages S1 to S3 of
	 * `plans/waterfall-replacement.md`).
	 *
	 * S1, inventory: the loaded references whose model is a waterfall or rapids effect, classified by
	 * model path, with world bounds from their triangles. S2, clustering (union-find): falling water
	 * and skirts within CLUSTER_RADIUS of each other are one site; mist, splashes and rapids join the
	 * nearest fall within CLUSTER_RADIUS, or else cluster among themselves within
	 * RAPIDS_CLUSTER_RADIUS (rivers are lined with them, so a wider radius links whole gorges). S3, pairing: the water planes around a
	 * site give its levels; the highest near its top is the top level, the lowest near its bottom the
	 * bottom level, and more than two distinct heights make a cascade of tiers. A drop under
	 * RAPIDS_THRESHOLD is rapids. Rebuilt when the loaded references or water change. S4, the rock
	 * under each fall, cascade and fall without a bottom, is FallCapture's, cast a little every update
	 * and kept while the site stays the same; D26 grounds the site's falling-water meshes on it. Runs on
	 * the main thread.
	 */
	class FallSites
	{
	public:
		/** @brief What a classified effect reference is. */
		enum class FxClass : uint8_t
		{
			Body,    ///< Falling water (waterfallbody, waterfallthin, waterfalltall, waterwall)
			Skirt,   ///< The curtain of foam at a fall's base
			Mist,    ///< Mist and mist blasts
			Splash,  ///< Splashes and churn
			Rapids,  ///< Rapids foam
			Count
		};

		struct FxRef
		{
			uint32_t formID;
			FxClass cls;
			float min[3];  ///< World bounds
			float max[3];
		};

		enum class Kind : uint8_t
		{
			Fall,      ///< One drop between two water levels
			Cascade,   ///< Several drops (tiers)
			Rapids,    ///< Drop under RAPIDS_THRESHOLD: water simulation and foam only
			NoBottom,  ///< No water below: the fall ends on ground
			Failed     ///< A stage failed: the site stays vanilla
		};

		struct Site
		{
			uint32_t id;
			std::vector<uint32_t> members;  ///< Indices into GetRefs()
			uint32_t classCounts[static_cast<int>(FxClass::Count)]{};
			float min[3];  ///< Bounds of every member
			float max[3];
			float centroid[3];  ///< Of the falling water
			float fallMin[3];   ///< Bounds of the falling water (all members when there is none)
			float fallMax[3];
			std::vector<float> levels;  ///< Distinct water heights, highest first: top, intermediate tiers, bottom
			/** @brief A water plane paired with the site, trimmed to the area around its falling water. */
			struct Plane
			{
				float min[2];
				float max[2];
				float height;
			};
			std::vector<Plane> planes;
			std::string candidates;  ///< For the log: planes within a wider search and what pairing made of them
			float top = 0.0f;
			float bottom = 0.0f;
			bool topFromMesh = false;  ///< No plane near the top: the top of the meshes stands in
			Kind kind = Kind::Failed;
			const char* failure = nullptr;  ///< Why, when Failed: "S3: no water plane", ...
			uint64_t captureKey = 0;        ///< Its FallCapture, when it has a drop to capture
		};

		/** @brief Rescans every few updates; rebuilds the sites when the references or water changed. */
		void Update(const WaterSurfaces& a_water);
		void Clear();

		const std::vector<FxRef>& GetRefs() const { return refs; }
		const std::vector<Site>& GetSites() const { return sites; }
		/** @brief A site's ground capture; null for rapids and failed sites. */
		const FallCapture* GetCapture(const Site& a_site) const;
		/** @brief Counts for the menu: ground captures complete, and in all. */
		void CountCaptures(uint32_t& a_complete, uint32_t& a_total) const;

		/**
		 * @brief Grounding (D26): whether a geometry is part of a vanilla falling-water mesh at a site
		 * whose ground is captured. Its vertices are then bent onto the rock in the effect shader.
		 * Pointers are compared only, never followed.
		 */
		bool IsGrounded(const RE::BSGeometry* a_geometry) const { return grounded.contains(a_geometry); }
		/**
		 * @brief D53: whether the falls' lit surfaces are drawn alpha-blended (their own alpha property
		 * switched to blending while grounded), so the shader can make them translucent in patches.
		 */
		void SetTranslucent(bool a_translucent);
		/** @brief Whether a grounded geometry draws blended (SetTranslucent took on it). */
		bool IsTranslucent(const RE::BSGeometry* a_geometry) const { return translucentGeometry.contains(a_geometry); }
		/**
		 * @brief D56: where a lit falls mesh's per-vertex rock cut starts in GetVertexClips, once tested.
		 * Each value is how far the vertex stands out of the rock (negative inside it), for SV_ClipDistance,
		 * and (D67) how much of it shows where it overlaps another falls mesh (1 whole, 0 gone: its edges
		 * over a mesh that leads it are dithered out, so the two blend instead of meeting at a hard line),
		 * and (D68) how far it stands above the water it falls into (NO_WATER where there is none); then
		 * (D69) how far it moves, in its mesh's own space, to close a gap to the mesh beside it.
		 */
		bool GetVertexClip(const RE::BSGeometry* a_geometry, uint32_t& a_base) const
		{
			const auto it = vertexClipBase.find(a_geometry);
			if (it == vertexClipBase.end())
				return false;
			a_base = it->second;
			return true;
		}
		const std::vector<std::array<float, 8>>& GetVertexClips() const { return vertexClips; }
		/** @brief D61: a grounded site's lit falls piece, and the water level its site ends in. */
		struct LitMesh
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			float bottom = 0.0f;
			uint32_t siteId = 0;
			std::vector<float> levels;  ///< The site's water levels, highest first
		};
		const std::vector<LitMesh>& GetLitMeshes() const { return litMeshList; }
		/** @brief D61: the surface of the pool over a world point (a site's solved flow's cover), or false where there is none (yet). */
		bool PoolLevelAt(float a_x, float a_y, float& a_level) const;
		/** @brief D64: a solved site's captured ground (top-down, FallCapture::CELL apart) and its pools' surface per cell (NO_SURFACE where none). */
		struct SiteGround
		{
			float originX = 0.0f, originY = 0.0f, cell = 16.0f;
			uint32_t width = 0, height = 0;
			const std::vector<float>* bed = nullptr;
			const std::vector<float>* cover = nullptr;
		};
		bool GetSiteGround(uint32_t a_siteId, SiteGround& a_ground) const;
		/**
		 * @brief D63: the footprint of the engine's water object at a level over a point (its mesh's extent), or false
		 * where none is (cell water covers whole cells and is not counted). a_bounds: min x, min y, max x, max y.
		 */
		bool WaterObjectBounds(float a_x, float a_y, float a_level, float a_reach, float a_bounds[4]) const;
		/** @brief D66: every engine water plane (water objects and cells' water) over a box: min x, min y, max x, max y, height. */
		void CollectWaterPlanes(float a_minX, float a_minY, float a_maxX, float a_maxY, std::vector<std::array<float, 5>>& a_planes) const;
		/** @brief D61: how many sites' flows are solved (their pools known). */
		uint32_t CountReadyStreams() const;

		/**
		 * @brief D59: a lit falls mesh's flow, solved once on its own texture grid (u across, v down), with the
		 * rock in it found by rays against the game's collision: so the water parts round a boulder through a
		 * curtain as well as on a flat run, which the top-down solve cannot see. D67: per texel, as floats, the
		 * texture coordinates the water there reads (a straight scroll of the second runs it along the flow):
		 * across, the streak it is on, carried down the flow from where it came in (so streaks part round a
		 * boulder and close behind it), and along, the time it has run since the top of its cascade, in texture
		 * coordinates at the shared scroll (so it runs faster where the flow is faster, and one mesh's water
		 * flows on into the next: both carry on from the mesh above, LinkBakes); then the wake of the rock
		 * above, and foam (the rock's rim, water piling against it and squeezing past it).
		 */
		struct FlowBake
		{
			uint32_t width = 0;
			uint32_t height = 0;
			float uvMin[2]{};
			float uvSize[2]{};
			std::vector<float> texels;  ///< RGBA32F, width x height
		};
		const std::unordered_map<const RE::BSGeometry*, FlowBake>& GetFlowBakes() const { return flowBakes; }
		/** @brief D67: the mesh at the top of the cascade a lit falls mesh is part of (itself where nothing feeds it). */
		const RE::BSGeometry* GetChainRoot(const RE::BSGeometry* a_geometry) const
		{
			const auto it = chainRoot.find(a_geometry);
			return it == chainRoot.end() ? a_geometry : it->second;
		}
		/** @brief Changes whenever GetFlowBakes changes. */
		uint64_t GetFlowBakeVersion() const { return flowBakeVersion; }
		/** @brief Changes whenever GetVertexClips grows. */
		uint64_t GetVertexClipVersion() const { return vertexClipVersion; }
		/** @brief Changes whenever the captured ground or the grounded meshes change. */
		uint64_t GetGroundVersion() const { return groundVersion; }
		/**
		 * @brief The captured ground of every site, sampled every a_cell units over a_size x a_size texels
		 * from a_origin (world); FallCapture::NONE where none is captured.
		 */
		void BuildGroundMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_heights) const;

		/**
		 * @brief The steady flow of every site whose flow is solved (D28), on a ground map's lattice
		 * (a_cell must be FallCapture::CELL): per texel the velocity (x, y; 1 for free flow), foam plus 2
		 * where there is water (so 2 to 3 on water, 0 elsewhere), and the head (D33), world height.
		 */
		void BuildStreamMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_field) const;

		/**
		 * @brief D30: every site's merged falls surface (vanilla's falling-water meshes, subdivided, with
		 * their flow), all in one list, and their spray emitters; changes with GetSurfaceVersion.
		 */
		const std::vector<FallSurfaceVertex>& GetSurfaceVertices() const { return surfaceVertices; }
		const std::vector<FallEmitter>& GetSurfaceEmitters() const { return surfaceEmitters; }

		/**
		 * @brief D35: the plunges' pushes on the simulated water, in a water level's logical texel
		 * coordinates: splashes that heave it down and throw it out, the water pulled along the way the
		 * falls run, and foam. Nearest the grid's centre first, up to a_maxEmitters in all.
		 */
		void AppendWaterEmitters(std::vector<WaterEmitter>& a_emitters, const int32_t a_gridMin[2], float a_cellSize, uint32_t a_gridRes, float a_strength,
			uint32_t a_maxEmitters, bool a_foam) const;

		/** @brief D35: world units per texture repeat along u and v of a falling-water mesh (its own texel density), or false if unknown. */
		bool GetTextureScale(const RE::BSGeometry* a_geometry, float a_scale[2]) const;
		uint64_t GetSurfaceVersion() const { return surfaceVersion; }

		/**
		 * @brief Debug: hides vanilla's mist, skirt, splash and rapids meshes, and the spray particles
		 * inside the falling-water models, so the falling water
		 * can be judged alone. Shown again when turned off, and on Clear.
		 */
		void SetHideEffects(bool a_hide, uint32_t a_layerMask);

		/** @brief The parts of a vanilla falling-water model, by what draws them. */
		enum class Layer : uint8_t
		{
			Inner,     ///< No effect shader (the model's inner or helper surfaces)
			Sheet,     ///< The white-water sheet (FXWhiteWater)
			Streaks,   ///< Other effect-shaded surfaces: the streaky fluid layers (FXfluidTile)
			Particles  ///< Spray: particle systems and parts with no triangles
		};
		static Layer LayerOf(RE::BSGeometry* a_geometry);
		static constexpr float STREAM_MIN_DEPTH = -2.0f;     ///< Rock is a wall only where it stands this far through the water's surface (D38: rock lying under the falls does not count)
		static constexpr float STREAM_INFLOW_BAND = 64.0f;   ///< Water within this of the site's top comes in there
		static constexpr float STREAM_OUTFLOW_BAND = 32.0f;  ///< Water within this of the site's bottom leaves there
		static constexpr float NO_SURFACE = -1.0e7f;

		/** @brief Counts for the menu: sites by kind. */
		uint32_t CountKind(Kind a_kind) const;

		/**
		 * @brief Draws the debug views in the world, within a_range of the camera. a_sites: classified
		 * references as boxes by class, site footprints at their levels, and a label per site. a_view 1:
		 * vanilla's falling-water triangles by piece, red inside rock; 2: their seams, where they cross
		 * the water levels, and the spray emitters by kind (D36).
		 */
		void DrawDebug(ImDrawList* a_drawList, float a_range, bool a_sites, int a_view) const;

		/**
		 * @brief D36: the pools' surface over a ground map's lattice: per texel the highest water plane
		 * of any site covering it, else FallCapture::NONE. Falling water under it is hidden.
		 */
		void BuildPoolMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_heights) const;

		/** @brief D44: per texel of a ground map's lattice, how strongly the falls plunge into the water there (0 to 1). */
		void BuildPlungeMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_field) const;

		/** @brief Prints each cell's summary to the game console too, not only the log. */
		void SetConsoleSummary(bool a_print) { consoleSummary = a_print; }

		static constexpr float CLUSTER_RADIUS = 512.0f;
		static constexpr float RAPIDS_CLUSTER_RADIUS = 128.0f;  ///< Rapids and splashes away from any fall cluster only this close
		static constexpr float PAIR_MARGIN = 256.0f;
		static constexpr float NEAR_LEVEL = 256.0f;       ///< How far below its falling water a plane may lie and still pair
		static constexpr float NEAR_LEVEL_ABOVE = 64.0f;  ///< How far above its falling water's top a plane may lie and still pair
		static constexpr float LEVEL_TOLERANCE = 16.0f;
		static constexpr float RAPIDS_THRESHOLD = 64.0f;
		static constexpr double CAPTURE_BUDGET_MS = 1.0;  ///< Ray casting per update

	private:
		static constexpr uint32_t SCAN_INTERVAL = 60;

		void Rebuild(const WaterSurfaces& a_water);
		void Summarise();
		void CaptureStep();
		/** @brief Collects the geometry of the falling-water meshes at sites with captured ground. */
		void RefreshGrounded();

		std::vector<FxRef> refs;
		std::vector<Site> sites;
		uint64_t refSignature = 0;
		uint64_t waterSignature = 0;
		uint32_t updatesUntilScan = 0;
		bool consoleSummary = false;
		std::unordered_map<uint64_t, std::string> cellSummaries;  ///< Last summary logged per cell
		std::unordered_map<uint64_t, FallCapture> captures;       ///< By Site::captureKey
		std::unordered_set<const RE::BSGeometry*> grounded;
		std::vector<LitMesh> litMeshList;
		bool translucent = false;
		std::unordered_set<const RE::BSGeometry*> translucentGeometry;
		/** @brief The alpha properties switched to blending, with their own flags to restore. */
		std::unordered_map<RE::NiAlphaProperty*, std::pair<RE::NiPointer<RE::NiAlphaProperty>, uint16_t>> blendedAlpha;
		std::vector<RE::NiPointer<RE::BSGeometry>> borrowedAlpha;  ///< D54: lit surfaces given their own layers' alpha property, to give back
		void RestoreAlpha();
		/** @brief D56: tests the queued lit meshes' vertices against the rock, a few milliseconds a frame. */
		void ClipStep();
		/** @brief D56: how far a point on the falls stands out of the rock (negative inside), from rays out of it. */
		float RockClip(RE::hkpWorld* a_world, const RE::NiPoint3& a_point, const RE::NiPoint3& a_normal, bool& a_inside) const;
		/** @brief D57: the nearest rock along a ray from a point, world units, or a negative value for none (the falls' own collision passed through). */
		float RockAlong(RE::hkpWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_direction, float a_reach, int* a_crossings = nullptr, float* a_last = nullptr) const;
		std::unordered_map<const RE::BSGeometry*, uint32_t> vertexClipBase;
		std::vector<std::array<float, 8>> vertexClips;
		static constexpr float GAP_REACH = 40.0f;        ///< D69: an open edge this near another mesh's open edge, and not over it, closes the gap
		static constexpr float GAP_OVERLAP = 4.0f;       ///< D69: ...each side moving past the middle by this much
		static constexpr float WATER_KEEP = 24.0f;       ///< D68: falling water is cut this far under the water it falls into
		static constexpr float WATER_CUT_BAND = 160.0f;  ///< D70: ...only this near its mesh's foot (a plane's bound reaches past its water, D63)
		static constexpr bool ROCK_CUT = false;          ///< D70: the per-vertex rock cut (D56) holed sheets where its parity misfired; rock hides water inside it anyway
		static constexpr float NO_WATER = 1.0e4f;        ///< D68: a vertex's height above the water below where there is none
		/**
		 * @brief D67: each tested lit mesh's overlap fade: its open edges that lie on another falls mesh leading it
		 * (higher, or as high and larger) fade to 0, so that mesh shows through; the rest stay 1.
		 */
		void ComputeFades();
		static constexpr float FADE_REACH = 32.0f;  ///< D67: an open edge's vertex this near another mesh's surface fades
		std::vector<RE::NiPointer<RE::BSGeometry>> clipQueue;
		uint64_t vertexClipVersion = 0;
		uint32_t clipTested = 0;
		uint32_t clipInside = 0;
		static constexpr float CLIP_REACH = 240.0f;  ///< D56: world units each ray out of a vertex looks for rock
		static constexpr float CLIP_MARGIN = 6.0f;   ///< A vertex this far inside rock still shows (water lying on it)
		static constexpr float CLIP_RANGE = 32.0f;   ///< The most a vertex counts as in or out, so the cut stays near the rock's face
		static constexpr float CLIP_INSIDE = 2.0f;   ///< D57: how far a buried vertex counts under the cut
		static constexpr double CLIP_BUDGET_MS = 3.0;
		static constexpr float BAKE_TEXEL = 10.0f;        ///< D59: world units per texel of a mesh's flow bake
		static constexpr uint32_t BAKE_MOST_SIDE = 192;   ///< ...and the most texels along a side
		static constexpr float BAKE_ROCK_MARGIN = 24.0f;  ///< A texel this far inside rock is still water (vanilla's sheets dip into the rock they lie on)
		static constexpr double BAKE_BUDGET_MS = 3.0;
		static constexpr float BAKE_LEAST_DOWN = 0.3f;     ///< The flow keeps at least this much of vanilla's forward...
		static constexpr float BAKE_MOST_TURN = 0.5f;      ///< ...and turns aside at most this across per down (D70: 27 degrees: vanilla's way down drives harder)
		static constexpr float BAKE_NEAR_TURN = 3.7f;      ///< D74: ...but right beside rock up to 75 degrees
		static constexpr float MEET_CELL = 32.0f;          ///< D72: world units per cell of the grid meshes find each other's water in
		static constexpr float MEET_REACH = 120.0f;        ///< D72: another mesh's water this near draws this mesh's flow... (D74: 120)
		static constexpr float MEET_PULL = 0.9f;           ///< D72: ...this much at its nearest
		static constexpr float BAKE_MOST_DRIFT = 64.0f;    ///< D71: texels a streak may drift across from where it came in (D74: the sim decides; only a guard)
		static constexpr float BAKE_MOST_DOWN = 1.6f;      ///< D60: and runs at most this much of it
		static constexpr std::size_t BAKE_LEAST_ROCK = 6;  ///< D60: rock islands of this many texels or fewer are specks, taken as water
		static constexpr float BAKE_BEND_NEAR = 3.0f;      ///< D60: the streaks bend in full this many texels from rock...
		static constexpr float BAKE_BEND_FAR = 14.0f;      ///< ...and not at all past this
		static constexpr int BAKE_TURN_BLUR = 6;           ///< D60: passes softening the turn (D68: more, so it cannot whorl)
		static constexpr int BAKE_WARP_BLUR = 3;           ///< D60: passes softening the warp
		static constexpr float BAKE_WARP_KEEP = 0.6f;      ///< D60: how much of the full streamline warp is kept (streaks partly run under a rock)
		static constexpr float BAKE_LEAST_SPACING = 0.5f;  ///< D60: streaks squeezed closer than this, or spread wider than BAKE_MOST_SPACING, ease back to vanilla's
		static constexpr float BAKE_MOST_SPACING = 2.0f;
		static constexpr float SIM_GRAVITY = 686.0f;      ///< D74: world units per second squared (Skyrim's)
		static constexpr float SIM_INFLOW = 250.0f;       ///< D74: world units per second the water comes onto a mesh's top at
		static constexpr float SIM_TERMINAL = 900.0f;     ///< D74: world units per second sliding water tends to (its drag)
		static constexpr int SIM_STEPS = 160;             ///< D74: steps to its steady state
		static constexpr int SIM_CG = 60;                 ///< D74: conjugate-gradient iterations per step's pressure (warm started)
		static constexpr float SIM_FRICTION = 3.0f;       ///< D74: per second, the bed's friction on the thin sheet (it damps eddies)
		static constexpr float SIM_BAND = 0.15f;          ///< D74: share of the mesh's rows (texture v) its top and foot bands are
		static constexpr float SIM_DEAD = 0.15f;          ///< D74: water moving less than this share of the median is a dead pocket: it keeps vanilla's way
		static constexpr float SIM_MOST_TURN = 5.7f;      ///< D74: the streaks turn at most this across per down (80 degrees)
		static constexpr bool FLAT_JOIN = false;          ///< D68's flat parts joining the site's flow: the sim spreads the water itself (D74)
		static constexpr float BAKE_WAKE_TEXELS = 24.0f;  ///< D60: texels down the streaks a rock's wake fades over
		/** @brief D59: a mesh's flow bake in progress: its texture grid rasterised, its texels being tested against the rock. */
		struct BakeJob
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			uint32_t width = 0, height = 0;
			float uvMin[2]{}, uvSize[2]{};
			std::vector<RE::NiPoint3> position, normal;
			std::vector<uint8_t> covered, rock;
			std::size_t next = 0;
			// D68: the render meshes of the statics round the sheet (boulders, the walls beside it), binned from above
			std::vector<std::array<RE::NiPoint3, 3>> solids;
			std::vector<uint32_t> solidRef;
			float solidOrigin[2]{};
			int32_t solidBins[2]{};
			std::vector<std::vector<uint32_t>> bins;
		};
		static constexpr float SOLID_BIN = 32.0f;      ///< D68: world units per bin of the statics' triangles
		static constexpr float SOLID_REACH = 64.0f;    ///< D68: world units round a sheet its statics are gathered
		static constexpr float FLAT_MOST_TURN = 1.5f;  ///< D68: on flat parts the flow may turn this much across per down (56 degrees) to join the main flow
		static constexpr float SOLID_MARGIN = 40.0f;   ///< D68: a texel this deep inside a static's mesh is rock (vanilla's sheets dip into the cliffs they lie against)
		/**
		 * @brief D73: a denser copy of a vanilla lit falls mesh, built by the engine (D62's builder) in its own space and
		 * drawn in its place (the original is hidden): its triangles split until no edge is longer than DENSE_EDGE, the
		 * new points on the smooth surface through its vertices' normals (PN triangles), with its texture coordinates,
		 * vertex colours and a copy of its material; everything else (cuts, seams, bakes) works on the copy.
		 */
		struct DenseCopy
		{
			RE::NiPointer<RE::BSTriShape> shape;
			RE::NiPointer<RE::BSGeometry> original;
			RE::NiPointer<RE::NiNode> parent;
		};
		std::unordered_map<const RE::BSGeometry*, DenseCopy> denseOf;  ///< By original
		std::unordered_set<const RE::BSGeometry*> denseShapes;
		bool refreshing = false;
		static constexpr bool DENSE = true;
		static constexpr float DENSE_EDGE = 40.0f;  ///< D73: world units the dense copy's edges are at most (when the vertex budget allows)
		static constexpr int DENSE_LEVELS = 1;      ///< D73: halvings at most (D74: one: three stuttered the game)
		/** @brief D73: builds and attaches a dense copy of a lit mesh; false where it cannot (no CPU data, or no engine builder). */
		bool Densify(RE::BSGeometry* a_geometry);
		/** @brief D73: removes every dense copy and shows the originals again. */
		void RemoveDense();
		/** @brief D68: gathers the statics' triangles round a bake job's sheet (main thread). */
		void GatherSolids(BakeJob& a_job) const;
		/** @brief D68: whether a point lies inside a static's render mesh, deeper than BAKE_ROCK_MARGIN (upward ray parity per mesh). */
		static bool InsideSolid(const BakeJob& a_job, const RE::NiPoint3& a_point);
		/** @brief D68: the solved site flow at a world point (direction from above), or false where there is none. */
		bool SiteFlowAt(float a_x, float a_y, float& a_vx, float& a_vy) const;
		uint32_t linkedStreams = UINT32_MAX;  ///< D68: the sites' solved flows when the bakes were last linked
		std::vector<RE::NiPointer<RE::BSGeometry>> bakeQueue;
		std::unique_ptr<BakeJob> bakeJob;
		/** @brief D67: a mesh's solved flow on its texture grid, before it is linked to the meshes above it. */
		struct BakeField
		{
			uint32_t width = 0, height = 0;
			float uvMin[2]{}, uvSize[2]{};
			std::vector<RE::NiPoint3> position;
			std::vector<uint8_t> covered;
			std::vector<float> turn, speed, wake, foam;  ///< Across per down (texels), speed (vanilla's 1), the rock's wake and foam
			std::vector<RE::NiPoint3> normal;
		};
		std::vector<std::pair<RE::NiPointer<RE::BSGeometry>, std::future<std::shared_ptr<BakeField>>>> bakesPending;
		std::unordered_map<const RE::BSGeometry*, std::shared_ptr<BakeField>> bakeFields;
		std::unordered_map<const RE::BSGeometry*, const RE::BSGeometry*> chainRoot;
		/**
		 * @brief D67: the solved meshes linked into cascades: a mesh whose top edge lies on the lower part of another
		 * is fed by it, and its streaks and run time carry on from that mesh's; then each mesh's bake is built.
		 */
		void LinkBakes();
		static constexpr float LINK_REACH = 48.0f;       ///< D67: world units a mesh's top edge may lie from the mesh feeding it
		static constexpr int32_t LINK_ROWS = 10;         ///< D67: texel rows up from a mesh's bottom edge that may feed the next
		static constexpr float BAKE_LEAST_SPEED = 0.5f;  ///< D67: the flow runs at least this much of vanilla's speed (and at most BAKE_MOST_DOWN)
		std::unordered_map<const RE::BSGeometry*, FlowBake> flowBakes;
		uint64_t flowBakeVersion = 0;
		uint32_t bakedRockTexels = 0, bakedTexels = 0;
		/** @brief D59: rasterises the next queued mesh, tests its texels against the rock, then solves it off the main thread. */
		void BakeStep();
		static std::unique_ptr<BakeJob> StartBake(RE::BSGeometry* a_geometry);
		static std::shared_ptr<BakeField> SolveBake(const BakeJob& a_job);
		std::unordered_set<std::string> loggedParts;  ///< Diagnostics: falling-water mesh parts already logged

		/** @brief A triangle of vanilla's falling-water surface, in world space. */
		struct SourceTriangle
		{
			float position[3][3];
			float normal[3][3];  ///< Smooth, facing up
			float uv[3][2];
			uint32_t piece;  ///< Which vanilla mesh it is from
		};
		struct SurfaceMesh
		{
			std::vector<FallSurfaceVertex> vertices;
			std::vector<uint32_t> indices;
			std::vector<FallEmitter> emitters;             ///< Where it throws spray and mist (D31)
			uint32_t culled = 0;                           ///< Source triangles dropped where another piece lies on them
			std::vector<float> seamFoam;                   ///< Per flow cell, 0 to 1: foam where vanilla's pieces meet (D34)
			uint32_t seams = 0;                            ///< Points found along the seams
			uint32_t impacts = 0;                          ///< Where falling water lands (D49)
			std::vector<FallPlunge> plunges;               ///< Where falling water meets the water below (D35)
			std::vector<std::array<float, 4>> seamPoints;  ///< Position and kind (0: an open edge against another piece, 1: two pieces crossing), for the debug view
			std::vector<std::array<float, 6>> waterLines;  ///< Where vanilla's pieces cross a water level, as segments, for the debug view
		};
		/** @brief A site's steady flow, solved on its capture's lattice on a worker thread. */
		struct Stream
		{
			std::future<FallStream::Result> pending;
			FallStream::Result result;
			float originX = 0.0f;
			float originY = 0.0f;
			uint32_t siteId = 0;
			bool ready = false;
			std::vector<FallSurfaceVertex> vertices;  ///< Its surface mesh, built once the flow is solved
			std::vector<uint32_t> indices;
			std::vector<FallEmitter> emitters;
			std::vector<float> seamFoam;  ///< SurfaceMesh::seamFoam
			std::vector<FallPlunge> plunges;
			std::vector<std::array<float, 4>> seamPoints;
			std::vector<std::array<float, 6>> waterLines;
			std::vector<SourceTriangle> sources;
			float top = 0.0f;           ///< The site's top level (D50)
			std::vector<float> levels;  ///< The site's water levels, where the falls meet bodies of water  ///< Vanilla's falling-water triangles, gathered when the solve starts
			std::future<SurfaceMesh> meshPending;
			bool meshReady = false;
		};
		/** @brief Vanilla's falling-water surfaces at a site: each model's lit surface, else its largest effect layer. */
		void GatherSources(const Site& a_site, std::vector<SourceTriangle>& a_sources) const;
		/**
		 * @brief One merged surface for a site (D30): vanilla's pieces, where two lie on each other only
		 * the upper kept, each subdivided evenly to about SURFACE_EDGE, welded where they meet, with the
		 * solved flow's coordinates at every vertex. Runs on a worker thread.
		 */
		static SurfaceMesh BuildMergedSurface(const FallStream::Result& a_flow, float a_originX, float a_originY, const std::vector<SourceTriangle>& a_sources, const std::vector<float>& a_levels);
		static constexpr float SURFACE_EDGE = 12.0f;       ///< Target edge length of the subdivided surface, world units
		static constexpr uint32_t SURFACE_MAX_SPLIT = 10;  ///< Most pieces a source edge is split into
		static constexpr float SURFACE_WELD = 1.0f;        ///< Vertices this close are one
		static constexpr float MERGE_FACE_UP = 0.35f;      ///< Only pieces at least this level are merged (curtains are left)
		static constexpr float MERGE_BUCKET = 64.0f;       ///< World units per bucket when finding overlaps
		static constexpr float MERGE_ABOVE = 3.0f;         ///< Only near-duplicates are dropped: another piece this close above or below covers a triangle
		static constexpr float MERGE_BELOW = 3.0f;
		static constexpr float EMITTER_SPACING = 20.0f;       ///< World units between spray emitters
		static constexpr float EMITTER_FOAM = 0.55f;          ///< Foam at least this much throws spray
		static constexpr float SEAM_STEP = 10.0f;             ///< World units between points tested along a piece's open edges (D34)
		static constexpr float SEAM_REACH = 20.0f;            ///< An open edge this close to another piece is a seam
		static constexpr float SEAM_FOAM_RADIUS = 44.0f;      ///< World units round a seam that foam (D47: wider, so the seam is covered)
		static constexpr float LIP_CELLS = 2.0f;              ///< Flow cells downstream the surface is looked at for a drop (D35)
		static constexpr float LIP_DROP = 40.0f;              ///< The surface falling this far within LIP_CELLS is a drop
		static constexpr float PLUNGE_BAND = 14.0f;           ///< World units round a lower water level where falling water plunges
		static constexpr float PLUNGE_HEAD = 40.0f;           ///< Water that has fallen at least this far below its head plunges
		static constexpr float PLUNGE_STEEP = 0.8f;           ///< Water this steep (upward normal) at a pool's level plunges whatever its head (D46)
		static constexpr float PLUNGE_SPACING = 16.0f;        ///< World units between plunge emitters
		static constexpr float PLUNGE_WATER_SPACING = 24.0f;  ///< World units between the plunge's pushes on the simulated water
		static constexpr float OBSTACLE_FOAM = 0.6f;          ///< Rock foam at least this much throws spray
		static constexpr float OBSTACLE_SPACING = 28.0f;
		static constexpr float IMPACT_FLAT = 0.7f;   ///< D49: a vertex at least this flat (upward normal) can be where water lands...
		static constexpr float IMPACT_STEEP = 0.5f;  ///< ...with water this steep within IMPACT_CELLS upstream...
		static constexpr int32_t IMPACT_CELLS = 2;
		static constexpr float IMPACT_DROP = 30.0f;         ///< ...standing at least this far above it
		static constexpr float IMPACT_SPACING = 24.0f;      ///< World units between impact points
		static constexpr float ROCK_HIT_MARGIN = 4.0f;      ///< D52: an edge of the falls this far under the rock is inside it...
		static constexpr float ROCK_HIT_DEPTH = 12.0f;      ///< ...and one end at least this far in, for a rock hit
		static constexpr float ROCK_HIT_SPACING = 20.0f;    ///< World units between rock-hit emitters
		static constexpr float FALL_VIEW_RANGE = 2500.0f;   ///< World units round the camera the fall debug views draw (D36)
		static constexpr float SEAM_CROSSING_ANGLE = 0.9f;  ///< Pieces crossing with their faces closer than this (cosine) lie along each other: no seam shows (D36)
		static constexpr float POOL_COVER = 2.0f;           ///< Water lying under a pool's surface by more than this is the pool's (D36)
		static constexpr float POOL_BAND = 24.0f;           ///< D40: a pool's water is where vanilla's falling water lies at its level, within this...
		static constexpr float TOP_POOL_CUT = 24.0f;        ///< D50: how much deeper the falls are cut under the pool they come from
		static constexpr float POOL_SHALLOW = 160.0f;       ///< ...or where the rock lies at most this far under it (a river or pool bed, not the face of a fall)
		static constexpr int32_t POOL_LIP_REACH = 3;        ///< D53: cells round a pool's cell its falls are looked at: a fall dropping away below the level...
		static constexpr float POOL_LIP_DROP = 48.0f;       ///< ...this far, with none coming in from above it, is the pool's lip or a curtain under it: not the pool's water
		static constexpr float BURIED_MARGIN = 120.0f;      ///< As FallGround.hlsli BURIED (D49)
		static constexpr float PLUNGE_FOAM_RADIUS = 64.0f;  ///< World units round each point where the falls meet a pool that the water foams (D44, D47)
		static constexpr float PLUNGE_FOAM_BAND = 40.0f;    ///< The falls' vertices this close to a pool's surface meet it (D47, D50)
		static constexpr float FOG_PLUNGE_RADIUS = 180.0f;  ///< World units round each plunge that its fog spreads (D41)
		static constexpr float PLUNGE_PUSH_OFFSET = 32.0f;  ///< D56: world units downstream of the falls' line the plunge pushes the water
		static constexpr float PLUNGE_MARK = 0.001f;        ///< D56: radial speed marking a fall's emitter (its own foam rate) without spreading the water
		static constexpr float PLUNGE_PULL = 0.2f;          ///< Fraction of the falling water's speed the water below is pulled along at
		static constexpr int FLOW_DILATE_CELLS = 6;         ///< Cells the flow is carried past the water's edge for the surface
		static constexpr float FALL_SPEED = 600.0f;         ///< World units per second of drop counted into the travel time, so steep water streaks too
		static constexpr float MEET_FOAM_HEIGHT = 20.0f;    ///< Water within this of a site's level (where it meets a body of water) foams
		std::vector<std::future<SurfaceMesh>> retiredMeshes;
		/** @brief Gathers every site's surface into one list. */
		void RefreshSurfaces();
		std::vector<FallSurfaceVertex> surfaceVertices;
		std::vector<FallEmitter> surfaceEmitters;
		const WaterSurfaces* waterSurfaces = nullptr;  ///< Set each update: the engine's water, for the pools' extent (D45)
		std::vector<FallPlunge> surfacePlunges;
		mutable std::unordered_map<const RE::BSGeometry*, std::array<float, 2>> textureScales;  ///< GatherSources: each lit mesh's world units per texture repeat
		uint64_t surfaceVersion = 0;
		std::unordered_map<uint64_t, Stream> streams;          ///< By Site::captureKey
		std::vector<std::future<FallStream::Result>> retired;  ///< Solves of sites that went away, left to finish
		/** @brief Vanilla's falling-water meshes of a site seen from above, and its pools, on a lattice; NO_SURFACE where none. */
		void RasterSurface(const Site& a_site, float a_originX, float a_originY, float a_spacing, int32_t a_width, int32_t a_height, std::vector<float>& a_surface, bool a_planes = true,
			std::vector<float>* a_lowest = nullptr, std::vector<float>* a_highest = nullptr) const;
		void StartStream(const Site& a_site, const FallCapture& a_capture);
		/** @brief Starts the flows of newly captured sites and collects finished ones. */
		void PollStreams();
		bool hideEffects = false;
		uint32_t layerMask = 0x1;                                                                ///< Bit per Layer: the falling-water model parts shown while effects are hidden
		std::unordered_map<const RE::NiAVObject*, RE::NiPointer<RE::NiAVObject>> hiddenEffects;  ///< Culled effect 3D: whole effect references, and the spray particles in the falling-water models
		void RefreshHiddenEffects();
		uint64_t groundVersion = 0;
	};
}
