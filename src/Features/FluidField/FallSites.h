#pragma once

#include "FallCapture.h"
#include "FallSurfaceRenderer.h"
#include "ShallowWaterSolver.h"

#include <array>
#include <cstdint>
#include <filesystem>
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
		const std::vector<std::array<float, 12>>& GetVertexClips() const { return vertexClips; }
		/** @brief D61: a grounded site's lit falls piece. */
		struct LitMesh
		{
			RE::NiPointer<RE::BSGeometry> geometry;
		};
		/** @brief D61: the grounded sites' lit falls pieces (their dense copies where built, D73). */
		const std::vector<LitMesh>& GetLitMeshes() const { return litMeshList; }
		/**
		 * @brief D91 (refractive falls): hides the lit falls pieces from the engine's passes (app-culled), because the
		 * refractive forward pass draws them instead and must see what lies behind them; false shows them again. Only
		 * the pieces hidden here are shown again (an original drawn by its dense copy stays hidden).
		 */
		void SetLitHidden(bool a_hidden);
		/**
		 * @brief D91 experiment: dense copies built from now on carry a blended NiAlphaProperty (0x10ED, borrowed from
		 * the model's effect layers) attached before the engine sets the copy up, so it may register with the blended
		 * pass (realtime-integration-design.md 5.2, cause 1). Copies already built keep what they had.
		 */
		void SetBlendedCopies(bool a_on) { blendedCopies = a_on; }
		/** @brief D91: the alpha flags the falls' effect layers carry: blended, SrcAlpha / InvSrcAlpha. */
		static constexpr uint16_t BLENDED_ALPHA_FLAGS = 0x10ED;
		/** @brief D61: the surface of the pool over a world point (a site's pools, D78: ComputePools), or false where there is none (yet). */
		bool PoolLevelAt(float a_x, float a_y, float& a_level) const;
		/** @brief D66: every engine water plane (water objects and cells' water) over a box: min x, min y, max x, max y, height. */
		void CollectWaterPlanes(float a_minX, float a_minY, float a_maxX, float a_maxY, std::vector<std::array<float, 5>>& a_planes) const;

		/**
		 * @brief D59: a lit falls mesh's flow, solved once on its own texture grid (u across, v down), with the
		 * rock in it found by rays against the game's collision: so the water parts round a boulder through a
		 * curtain as well as on a flat run. D67: per texel, as floats, the
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

		/** @brief D30: the falls' spray emitters (D78: from the lit meshes' sims, BuildPlunges); changes with GetSurfaceVersion. */
		const std::vector<FallEmitter>& GetSurfaceEmitters() const { return sprayEmitters; }

		/**
		 * @brief D35: the plunges' pushes on the simulated water, in a water level's logical texel
		 * coordinates: splashes that heave it down and throw it out, the water pulled along the way the
		 * falls run, and foam. Nearest the grid's centre first, up to a_maxEmitters in all.
		 */
		void AppendWaterEmitters(std::vector<WaterEmitter>& a_emitters, const int32_t a_gridMin[2], float a_cellSize, uint32_t a_gridRes, float a_strength,
			uint32_t a_maxEmitters, bool a_foam) const;

		uint64_t GetSurfaceVersion() const { return emitterVersion; }

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
		static constexpr float NO_SURFACE = -1.0e7f;

		/** @brief Counts for the menu: sites by kind. */
		uint32_t CountKind(Kind a_kind) const;

		/**
		 * @brief Draws the debug views in the world, within a_range of the camera. a_sites: classified
		 * references as boxes by class, site footprints at their levels, and a label per site. a_view 1:
		 * the spray emitters by kind and the plunges' pushes on the water below (D36, D78).
		 */
		void DrawDebug(ImDrawList* a_drawList, float a_range, bool a_sites, int a_view) const;
		/**
		 * @brief D75 debug: the falls' lit meshes as drawn (their dense copies) over everything: 1 their triangles'
		 * wireframe, 2 their sim's flow as arrows in the world (length and colour by speed: blue slow, red fast;
		 * white where it strikes rock), 3 both. Meshes within a_range of the camera.
		 */
		void DrawMeshDebug(ImDrawList* a_drawList, float a_range, int a_mode) const;
		/** @brief D75: which debug arrows the camera sees (a ray from it meets no rock first), checked a few hundred a frame. */
		mutable std::unordered_map<uint64_t, uint8_t> arrowSeen;
		mutable RE::NiPoint3 arrowEye;
		static constexpr int ARROW_RAYS = 400;  ///< D75: occlusion rays per frame for the debug arrows

		/**
		 * @brief D36: the pools' surface over a ground map's lattice (a_cell must be FallCapture::CELL): per texel the
		 * highest pool of any site covering it (ComputePools), else FallCapture::NONE. Falling water under it is hidden.
		 */
		void BuildPoolMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_heights) const;

		/** @brief D44: per texel of a ground map's lattice, how strongly the falls plunge into the water there (0 to 1), round the plunge points (D78). */
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
		std::vector<RE::NiPointer<RE::BSGeometry>> litHidden;  ///< D91: the lit pieces SetLitHidden app-culled
		bool litHiddenWanted = false;
		bool blendedCopies = false;  ///< D91 experiment (SetBlendedCopies)
		/** @brief D91: app-culls the current lit pieces (or shows the ones it culled again) as SetLitHidden asked. */
		void ApplyLitHidden();
		/** @brief D91 experiment: a blended alpha property for a dense copy of a_geometry (a clone of its model's effect layers'), or null. */
		RE::NiAlphaProperty* MakeBlendedAlpha(RE::BSGeometry* a_geometry) const;
		/** @brief D56: tests the queued lit meshes' vertices against the rock, a few milliseconds a frame. */
		void ClipStep();
		/** @brief D57: the nearest rock along a ray from a point, world units, or a negative value for none (the falls' own collision passed through). */
		float RockAlong(RE::hkpWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_direction, float a_reach, int* a_crossings = nullptr, float* a_last = nullptr) const;
		std::unordered_map<const RE::BSGeometry*, uint32_t> vertexClipBase;
		std::vector<std::array<float, 12>> vertexClips;
		static constexpr float GAP_REACH = 40.0f;        ///< D69: an open edge this near another mesh's open edge, and not over it, closes the gap
		static constexpr float GAP_OVERLAP = 4.0f;       ///< D69: ...each side moving past the middle by this much
		static constexpr float WATER_KEEP = 6.0f;        ///< D68: falling water is cut this far under the water it falls into (D95: 6, was 24: see-through water showed the sheet under it)
		static constexpr float WATER_CUT_BAND = 160.0f;  ///< D70: ...only this near its mesh's foot (a plane's bound reaches past its water, D63)
		static constexpr float NO_WATER = 1.0e4f;        ///< D68: a vertex's height above the water below where there is none
		/**
		 * @brief D67: each tested lit mesh's overlap fade: its open edges that lie on another falls mesh leading it
		 * (higher, or as high and larger) fade to 0, so that mesh shows through; the rest stay 1.
		 */
		void ComputeFades();
		static constexpr float FADE_REACH = 32.0f;         ///< D67: an open edge's vertex this near another mesh's surface fades
		static constexpr float SEAM_BIN = 48.0f;           ///< D91: world units per cell of the seams' triangle bins
		static constexpr float INSIDE_REACH = 150.0f;      ///< D98: a vertex behind a leading mesh's outer face within this is inside its curl, cut...
		static constexpr float INSIDE_MARGIN = 6.0f;       ///< D98: ...past this behind it (one just behind, touching, stays)
		static constexpr int EDGE_SMOOTH_PASSES = 80;      ///< D86: rounds smoothing each mesh's open edge as a curve (D92: 80: on the dense copies' 20-unit edges, 10 smoothed only 60 units)...
		static constexpr float EDGE_SMOOTH_MOST = 60.0f;   ///< D86: ...moving a vertex at most this far (D92: 60)
		static constexpr float EDGE_BLEND_REACH = 120.0f;  ///< D94: world units the open edge's moves fade into the surface over...
		static constexpr int EDGE_BLEND_PASSES = 60;       ///< D94: ...in this many rounds
		static constexpr float MOST_SHIFT = 120.0f;        ///< D96: a vertex's composed move past this (model units) is a bug: it is left still
		static constexpr float LEAD_CUT_DEPTH = 4.0f;      ///< D97: a hidden lead-in vertex this far under the water over it is cut
		std::vector<RE::NiPointer<RE::BSGeometry>> clipQueue;
		uint64_t vertexClipVersion = 0;
		uint32_t clipTested = 0;
		static constexpr float CLIP_REACH = 240.0f;  ///< D56: world units each ray out of a vertex looks for rock
		static constexpr float CLIP_MARGIN = 6.0f;   ///< A vertex this far inside rock still shows (water lying on it)
		static constexpr float CLIP_RANGE = 32.0f;   ///< The most a vertex counts as in or out, so the cut stays near the rock's face
		static constexpr double CLIP_BUDGET_MS = 3.0;
		static constexpr float BAKE_TEXEL = 10.0f;        ///< D59: world units per texel of a mesh's flow bake
		static constexpr uint32_t BAKE_MOST_SIDE = 192;   ///< ...and the most texels along a side
		static constexpr float BAKE_ROCK_MARGIN = 24.0f;  ///< A texel this far inside rock is still water (vanilla's sheets dip into the rock they lie on)
		static constexpr double BAKE_BUDGET_MS = 3.0;
		static constexpr std::size_t BAKE_LEAST_ROCK = 6;  ///< D60: rock islands of this many texels or fewer are specks, taken as water
		static constexpr float SIM_GRAVITY = 686.0f;       ///< D74: world units per second squared (Skyrim's)
		static constexpr float SIM_INFLOW = 250.0f;        ///< D74: world units per second the water comes onto a mesh's top at
		static constexpr float SIM_TERMINAL = 900.0f;      ///< D74: world units per second sliding water tends to (its drag)
		/** @brief D59: a mesh's flow bake in progress: its texture grid rasterised, its texels being tested against the rock. */
		struct BakeJob
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			uint32_t width = 0, height = 0;
			float uvMin[2]{}, uvSize[2]{};
			std::vector<RE::NiPoint3> position, normal;
			std::vector<uint8_t> covered, rock;
			std::vector<uint8_t> untested;  ///< D83: hidden lead-in texels no strand comes near: the rock test passes them by
			std::filesystem::path cache;    ///< D82: where its finished bake is kept (empty: not kept)
			std::size_t next = 0;
			bool worldMissing = false;  ///< Diagnostics: the wait for its cell's collision world logged
			// D68: the render meshes of the statics round the sheet (boulders, the walls beside it), binned from above
			std::vector<std::array<RE::NiPoint3, 3>> solids;
			std::vector<uint32_t> solidRef;
			float solidOrigin[2]{};
			int32_t solidBins[2]{};
			std::vector<std::vector<uint32_t>> bins;
		};
		static constexpr float SOLID_BIN = 32.0f;     ///< D68: world units per bin of the statics' triangles
		static constexpr float SOLID_REACH = 64.0f;   ///< D68: world units round a sheet its statics are gathered
		static constexpr float SOLID_MARGIN = 40.0f;  ///< D68: a texel this deep inside a static's mesh is rock (vanilla's sheets dip into the cliffs they lie against)
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
		static constexpr float DENSE_EDGE = 20.0f;  ///< D73: world units the dense copy's edges are at most (when the vertex budget allows; D91: 20, was 40, split only where longer)
		static constexpr int DENSE_LEVELS = 6;      ///< D73: splitting passes at most (D91: adaptive, so passes only refine what is still long)
		/** @brief D73: builds and attaches a dense copy of a lit mesh; false where it cannot (no CPU data, or no engine builder). */
		bool Densify(RE::BSGeometry* a_geometry);
		/** @brief D73: removes every dense copy and shows the originals again. */
		void RemoveDense();
		/** @brief D68: gathers the statics' triangles round a bake job's sheet (main thread). */
		void GatherSolids(BakeJob& a_job) const;
		/** @brief D68: whether a point lies inside a static's render mesh, deeper than BAKE_ROCK_MARGIN (upward ray parity per mesh). */
		static bool InsideSolid(const BakeJob& a_job, const RE::NiPoint3& a_point, bool a_lying = false);
		static constexpr float SOLID_COVER = 120.0f;  ///< D76: a lying texel with a static's underside this near above it is under that rock (a wall)
		std::vector<RE::NiPointer<RE::BSGeometry>> bakeQueue;
		std::unique_ptr<BakeJob> bakeJob;
		/** @brief D67: a mesh's solved flow on its texture grid, before it is linked to the meshes above it. */
		struct BakeField
		{
			uint32_t width = 0, height = 0;
			float uvMin[2]{}, uvSize[2]{};
			std::vector<RE::NiPoint3> position;
			std::vector<uint8_t> covered;
			std::vector<float> turn, speed, wake, foam;  ///< Across per down (texels), speed (vanilla's 1), aeration (D80) and foam
			std::vector<float> seed, along;              ///< D80: the lip column each texel's strand came over (texels), and its run from the mesh's top (texture v)
			std::vector<RE::NiPoint3> normal;
			std::vector<uint8_t> rock;       ///< D75: texels inside rock (the debug arrows skip them)
			std::vector<float> swell;        ///< D82: how much the water piles up there, softened (the vertex push)
			std::vector<float> disturbance;  ///< D85: how stirred the water is there (the wobble's size), softened
		};
		static constexpr uint32_t BAKE_CACHE_VERSION = 3;  ///< D82: bump whenever a bake would come out different (D85: 2, the disturbance; D91: 3, the evenly split copies)
		/** @brief D82: where a bake job's finished bake is kept on disk: by its reference, model part, placement, the rock and ground round it, and the version. */
		static std::filesystem::path BakeCachePath(const BakeJob& a_job);
		static bool SaveBake(const std::filesystem::path& a_path, const BakeField& a_field);
		static std::shared_ptr<BakeField> LoadBake(const std::filesystem::path& a_path);
		uint32_t bakesCached = 0, bakesSolved = 0;
		/**
		 * @brief D82: each lit mesh's vertices pushed out along their normals where its water piles up (the bake's swell),
		 * in its own space, broad shapes only; none at its open edges (cracks between meshes)
		 */
		void ComputePushes();
		static constexpr float PUSH_MOST = 20.0f;          ///< D82: world units the water swells out at most (D83: 20, was 12)
		static constexpr float PUSH_FROM = 1.0f;           ///< D82: swell the push starts at (the sheet mostly runs 0 to 2.5)...
		static constexpr float PUSH_FULL = 3.0f;           ///< D82: ...and is whole at (piling against rock, squeezing between rocks: 5 to 20; D83: 3, was 4)
		static constexpr int32_t LEAD_TEST_REACH = 12;     ///< D83: hidden lead-in texels this far across from running water still get the rock test
		static constexpr int PUSH_BLUR = 2;                ///< D82: passes softening the swell to broad shapes (D84: 2, was 4: steeper, rounder mounds)
		static constexpr float WOBBLE_MOST = 28.0f;        ///< D85: world units the water wobbles at most, where it is most stirred (D90: 28)...
		static constexpr float WOBBLE_CALM = 10.0f;        ///< D85: ...and everywhere else (its breathing; D90: 10, the fades took most of it)
		static constexpr float WOBBLE_STIRRED = 1.5f;      ///< D85: disturbance at which the wobble is whole
		static constexpr float WOBBLE_EDGE_REACH = 50.0f;  ///< D88: world units in from the mesh's open edges (D89: and from rock) over which the wobble fades in (D90: 50, was 120)
		/** @brief D82: the vertex shifts the shaders read (vertexClips 4-6): the seams' gap shift (D69) and the push together. */
		void ComposeShifts();
		std::unordered_map<const RE::BSGeometry*, std::vector<RE::NiPoint3>> gapShifts, vertexPushes;
		/** @brief D82 diagnostics: each fall's own vanilla scroll, measured on its hidden original and on its copy, logged once. */
		void ProbeScrolls();
		struct ScrollProbe
		{
			float offset = 0.0f, copyOffset = 0.0f, travelled = 0.0f, copyTravelled = 0.0f, scale = 0.0f;
			double start = -1.0, last = -1.0;
			bool logged = false;
		};
		std::unordered_map<const RE::BSGeometry*, ScrollProbe> scrollProbes;

		std::vector<std::pair<RE::NiPointer<RE::BSGeometry>, std::future<std::shared_ptr<BakeField>>>> bakesPending;
		std::unordered_map<const RE::BSGeometry*, std::shared_ptr<BakeField>> bakeFields;
		/**
		 * @brief D67: the solved meshes linked into cascades: a mesh whose top edge lies on the lower part of another
		 * is fed by it, and its streaks and run time carry on from that mesh's; then each mesh's bake is built.
		 */
		void LinkBakes();
		static constexpr float LINK_REACH = 48.0f;  ///< D67: world units a mesh's top edge may lie from the mesh feeding it
		static constexpr int32_t LINK_ROWS = 10;    ///< D67: texel rows up from a mesh's bottom edge that may feed the next
		std::unordered_map<const RE::BSGeometry*, FlowBake> flowBakes;
		uint64_t flowBakeVersion = 0;
		uint32_t bakedRockTexels = 0, bakedTexels = 0;
		/** @brief D59: rasterises the next queued mesh, tests its texels against the rock, then solves it off the main thread. */
		void BakeStep();
		/** @brief D83: BakeStep's work: lands finished bakes, then starts or continues the next job. */
		void BakeWork();
		bool linkDue = false;  ///< D83: a bake landed since the last link
		static std::unique_ptr<BakeJob> StartBake(RE::BSGeometry* a_geometry);
		static std::shared_ptr<BakeField> SolveBake(const BakeJob& a_job);
		std::unordered_set<std::string> loggedParts;  ///< Diagnostics: falling-water mesh parts already logged

		/**
		 * @brief D78: from the lit meshes' sims (bakeFields): the plunge points, where each mesh goes into the water below
		 * (texels on its foot at or under that water, coming from above it), grouped every PLUNGE_WATER_SPACING, with the
		 * sim's way there and the speed of its fall; and the spray emitters, at the plunges and where its water strikes
		 * rock (the bake's foam). Run when the bakes are linked, the pools are found, or meshes go.
		 */
		void BuildPlunges();
		static constexpr float PLUNGE_BAND = 14.0f;           ///< A texel this near above the water below, or under it, is in it
		static constexpr float PLUNGE_HEAD = 40.0f;           ///< Water that has fallen at least this far down its mesh plunges
		static constexpr float PLUNGE_STEEP = 0.8f;           ///< Water this steep (upward normal) at the water's level plunges whatever its head (D46)
		static constexpr float PLUNGE_SPACING = 16.0f;        ///< World units between plunge emitters
		static constexpr float PLUNGE_WATER_SPACING = 24.0f;  ///< World units between the plunge points (their pushes on the simulated water)
		static constexpr float OBSTACLE_FOAM = 0.6f;          ///< Rock foam at least this much throws spray
		static constexpr float OBSTACLE_SPACING = 28.0f;
		static constexpr float FALL_VIEW_RANGE = 2500.0f;   ///< World units round the camera the fall debug view draws (D36)
		static constexpr float POOL_COVER = 2.0f;           ///< Water lying under a pool's surface by more than this is the pool's (D36)
		static constexpr float POOL_BAND = 24.0f;           ///< D40: a pool's water is where vanilla's falling water lies at its level, within this...
		static constexpr float TOP_POOL_CUT = 24.0f;        ///< D50: how much deeper the falls are cut under the pool they come from
		static constexpr float POOL_SHALLOW = 160.0f;       ///< ...or where the rock lies at most this far under it (a river or pool bed, not the face of a fall)
		static constexpr int32_t POOL_LIP_REACH = 3;        ///< D53: cells round a pool's cell its falls are looked at: a fall dropping away below the level...
		static constexpr float POOL_LIP_DROP = 48.0f;       ///< ...this far, with none coming in from above it, is the pool's lip or a curtain under it: not the pool's water
		static constexpr float PLUNGE_FOAM_RADIUS = 64.0f;  ///< World units round each point where the falls meet the water below that it foams (D44, D47)
		static constexpr float PLUNGE_PUSH_OFFSET = 32.0f;  ///< D56: world units downstream of the falls' line the plunge pushes the water
		static constexpr float PLUNGE_MARK = 0.001f;        ///< D56: radial speed marking a fall's emitter (its own foam rate) without spreading the water
		static constexpr float PLUNGE_PULL = 0.2f;          ///< Fraction of the falling water's speed the water below is pulled along at
		static constexpr float LIP_BAND = 40.0f;            ///< D90: a crest this near its pool's surface is a lip, where the water leaves the pool (D91: 40, was 24)...
		static constexpr float LIP_STRENGTH = 0.6f;         ///< D90: ...stirring it this much (a plunge's 0.25 to 1; D91: 0.6, was 0.35)
		static constexpr float LIP_UPSTREAM = 40.0f;        ///< D92: world units upstream of the crest its foam is made
		static constexpr float CROSSING_STRENGTH = 0.8f;    ///< D94: how hard the water foams where a mesh crosses a water surface (D100: back to 0.8)...
		static constexpr float CROSSING_BACK = 32.0f;       ///< D100: ...set back this far onto the water's side of the seam
		std::vector<FallEmitter> sprayEmitters;
		std::vector<FallPlunge> plungePoints;
		uint64_t emitterVersion = 0;
		const WaterSurfaces* waterSurfaces = nullptr;  ///< Set each update: the engine's water, for the pools' extent (D45)
		/** @brief D78: a site's pools on its capture's lattice: per cell the pool's surface, NO_SURFACE where there is none. */
		struct PoolField
		{
			float originX = 0.0f;
			float originY = 0.0f;
			uint32_t width = 0;
			uint32_t height = 0;
			float top = 0.0f;  ///< The site's top level: the pool the falls come from is cut deeper (D50)
			std::vector<float> cover;
		};
		std::unordered_map<uint64_t, PoolField> sitePools;  ///< By Site::captureKey
		/** @brief Vanilla's falling-water meshes of a site seen from above on a lattice (NO_SURFACE where none), and the lowest and highest of them along their edges too (D53). */
		void RasterSurface(const Site& a_site, float a_originX, float a_originY, float a_spacing, int32_t a_width, int32_t a_height, std::vector<float>& a_surface,
			std::vector<float>* a_lowest = nullptr, std::vector<float>* a_highest = nullptr) const;
		/**
		 * @brief D40, D45, D52, D53: where a site's pools have water, on its capture's lattice: the engine's water height
		 * sampled per cell, kept where the rock lies under it and the falls lie at its level or the bed is shallow, and
		 * flooded through the basin; a pool's lip and the air under a curtain falling from it are kept out.
		 */
		void ComputePools(const Site& a_site, const FallCapture& a_capture);
		/** @brief D78: works out the pools of newly captured sites. */
		void PollPools();
		bool hideEffects = false;
		uint32_t layerMask = 0x1;                                                                ///< Bit per Layer: the falling-water model parts shown while effects are hidden
		std::unordered_map<const RE::NiAVObject*, RE::NiPointer<RE::NiAVObject>> hiddenEffects;  ///< Culled effect 3D: whole effect references, and the spray particles in the falling-water models
		void RefreshHiddenEffects();
		uint64_t groundVersion = 0;
	};
}
