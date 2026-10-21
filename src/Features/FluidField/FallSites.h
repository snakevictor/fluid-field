#pragma once

#include "FallCapture.h"
#include "FallSurfaceRenderer.h"
#include "ShallowWaterSolver.h"

#include <array>
#include <cfloat>
#include <chrono>
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
		const std::vector<std::array<float, 16>>& GetVertexClips() const { return vertexClips; }
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
		/** @brief D112: the level the shaders cut the falls' foot at over a world point (BuildPoolMap's, less FallGround::POOL_CUT), or false where there is no pool. */
		bool PoolCutAt(float a_x, float a_y, float& a_level) const;
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
		/**
		 * @brief D106: each lit mesh's depth behind it, per vertex: Havok rays from a vertex in every DEPTH_CELL cell, from
		 * DEPTH_START off one face through the vertex along its normal, the first rock or ground beyond it; both ways
		 * (the shaders take the inner side's), spread smoothly to the vertices between. A few milliseconds a frame.
		 */
		void DepthStep();
		struct DepthJob
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			uint32_t base = 0;
			std::vector<RE::NiPoint3> positions, normals;  ///< World; normals by winding, unit
			std::vector<std::vector<uint32_t>> ring;
			std::vector<uint32_t> samples;
			std::size_t next = 0;
			std::vector<float> plus, minus;  ///< Clearance along and against the winding normal
			std::vector<uint8_t> known;
			std::vector<std::array<RE::NiPoint3, 3>> rays;  ///< D108: each ray's start, end and (x) whether it hit, for the overlay
			// TEMPORARY D106 diagnostics
			std::vector<float> hitDistances, missDown;
			uint32_t hits = 0, misses = 0, missDownHits = 0, downward = 0;
			double tiltSum = 0.0;
		};
		std::vector<RE::NiPointer<RE::BSGeometry>> depthQueue;
		std::unordered_map<const RE::BSGeometry*, std::vector<RE::NiPoint3>> outwardNormals;                 ///< D109: each lit mesh's consistent outward normals, world (SideFlags)
		std::unordered_map<const RE::BSGeometry*, std::vector<std::array<RE::NiPoint3, 3>>> depthRaysShown;  ///< D108: the overlay's rays
		std::unique_ptr<DepthJob> depthJob;
		uint32_t depthRays = 0, depthHits = 0;
		bool depthAudited = true;                          ///< TEMPORARY D110
		static constexpr float DEPTH_UNDERSIDE_Z = -0.3f;  ///< D111: a vertex whose outer side faces further down than this (z of its unit normal) casts no depth ray
		static constexpr float DEPTH_CELL = 30.0f;         ///< D106: world units: one sampled vertex per cell this size
		static constexpr float DEPTH_START = 40.0f;        ///< D106: the ray starts this far off the other face
		static constexpr int DEPTH_FILL_PASSES = 60;       ///< D106: rounds spreading the samples to the vertices between
		/** @brief D57: the nearest rock along a ray from a point, world units, or a negative value for none (the falls' own collision passed through). */
		float RockAlong(RE::hkpWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_direction, float a_reach, int* a_crossings = nullptr, float* a_last = nullptr) const;
		std::unordered_map<const RE::BSGeometry*, uint32_t> vertexClipBase;
		std::vector<std::array<float, 16>> vertexClips;
		/**
		 * @brief D112: why each vertex of vertexClips is hidden, kept apart so each can be worked out again: the cuts a.x
		 * composes (the water below, inside another sheet, the lead-in), and two the CPU knows but the GPU applies its own way (the
		 * pools' cut at the foot, per frame in FallGround::ClipDistance; the rock, by the depth test).
		 */
		struct VertexCut
		{
			float water = NO_CUT;      ///< ClipStep: the engine's water at the mesh's foot (D68)
			float inside = NO_CUT;     ///< D113: VolumeCut: inside the space another sheet encloses (with the rock), past VOLUME_MARGIN (was D98's curl cut)
			float lead = NO_CUT;       ///< ComputePushes: the hidden lead-in under the river (D97)
			float pool = NO_CUT;       ///< ComputePoolCuts: under our pools at the foot, as the shaders cut it (not in a.x)
			float submerged = NO_CUT;  ///< D126: CullStep: inside our pools' water near the bed (the river over a lead-in): under it by LEAD_CUT_DEPTH or more
			float rock = NO_CUT;       ///< D131: BuryStep: under the landscape (by its height, from BURY_LAND under it) or inside a static (parity, from BURY_MARGIN in); in a.x
			float island = NO_CUT;     ///< D138: CleanupSheet: a small piece of kept sheet cut off from its sheet's main body; in a.x
			uint8_t rockCause = 0;     ///< D138: 1 the landscape cut it, 2 a static (for the log)
			uint8_t votes = 0;         ///< D138: axes that called it inside a static
			float ground = -FLT_MAX;   ///< D126: BuryStep: the landscape's height under it (-FLT_MAX: none, or an interior)
			uint8_t buried = 0;        ///< BuryStep: under the landscape, or no ray from open air reaches it (not in a.x)
		};
		std::vector<VertexCut> vertexCuts;      ///< D112: parallel to vertexClips
		static constexpr float NO_CUT = 38.0f;  ///< D112: a part of VertexCut that hides nothing (CLIP_MARGIN + CLIP_RANGE, ClipStep's "not cut")
		/** @brief D112: a.x of every vertex again from its parts (the least of the water's, the inside's and the lead-in's). */
		void ComposeCut(std::size_t a_index);
		/**
		 * @brief D112: whether a vertex of the falls is drawn at all, as the GPU draws it: not cut (a.x, and the pools' cut at
		 * its foot), not inside rock, not faded out at a seam. The one test the depth rays and the inner sides use.
		 */
		bool VertexShown(std::size_t a_index) const;
		/**
		 * @brief D126: whether a vertex is part of the falls body's outer shell: not buried (landscape, statics), not inside another
		 * sheet, not the lead-in under the river. Unlike VertexShown it ignores the cuts at the waterline (the plunges and their
		 * foam belong there) and the seam fade. The bake's domain and every emitter (spray, plunges, lips) keep to it.
		 */
		bool VertexInShell(std::size_t a_index) const;
		/** @brief D126: the landscape's height under a point (its own collision, straight down from a_above, past everything else), or -FLT_MAX. */
		/** @brief D131: what LandHeightAt's ray met, for the log. */
		struct LandProbe
		{
			int steps = 0;
			uint32_t firstLayer = 0;
			bool firstReferenced = false;
			float firstHeight = 0.0f;
			float engine = 0.0f;  ///< D138: TES::GetLandHeight where the ray stopped
			int disagreed = 0;    ///< D138: unreferenced ground-layer hits passed by, the engine's height elsewhere
		};
		float LandHeightAt(RE::hkpWorld* a_world, float a_x, float a_y, float a_above, LandProbe* a_probe = nullptr) const;
		/** @brief D131: the distances along a ray at which it meets a static's face (stepping past each), appended to a_hits; a_flip measures them from the ray's far end. */
		struct StaticHit
		{
			float distance = 0.0f;  ///< Along the ray from its start
			float facing = 0.0f;    ///< D138: the face's normal along the ray (negative: its front)
		};
		void StaticHits(RE::hkpWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_direction, float a_reach, std::vector<StaticHit>& a_hits) const;
		/**
		 * @brief D126: VertexInShell rasterised onto a bake's texel grid (as StartBake lays the triangles in), 1 where kept; empty
		 * where the mesh has no culling yet. a_hash, if given, gets a hash of the mesh's kept vertices (the bake cache's key).
		 */
		std::vector<uint8_t> ShellTexels(RE::BSGeometry* a_geometry, uint32_t a_width, uint32_t a_height, const float a_uvMin[2], const float a_uvSize[2], uint64_t* a_hash) const;
		/**
		 * @brief D112: which vertices lie inside rock, a few milliseconds a frame. D126: under the landscape (its collision straight
		 * down). D131: or inside a static by parity: of the six axes, four or more cross an odd number of static faces between
		 * the vertex and BURY_REACH out (cast from both ends, each face counted once). Both cut in a.x (VertexCut::rock), from
		 * BURY_LAND under the landscape and BURY_MARGIN inside a static. ClipStep's rock cut was dropped (D70), and the bake's
		 * test needs rock on both sides of the sheet (D59), so a sheet running into a cliff was never known as hidden.
		 */
		void BuryStep();
		struct BuryJob
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			uint32_t base = 0;
			std::vector<RE::NiPoint3> positions, normals;  ///< World, at rest; normals unit (vanilla's, else the winding's)
			std::size_t next = 0;
			/** @brief D131: a vertex the landscape test left standing, for the log. */
			struct Sample
			{
				RE::NiPoint3 at;
				float ground = 0.0f, over = 0.0f;
				int steps = 0;
				uint32_t firstLayer = 0;
				bool firstReferenced = false;
				float firstHeight = 0.0f;
			};
			std::vector<Sample> samples;  ///< The lowest four over the landscape
			uint32_t landMissing = 0, landStepped = 0, landDisagreed = 0;
		};
		std::vector<RE::NiPointer<RE::BSGeometry>> buryQueue;
		std::unique_ptr<BuryJob> buryJob;
		uint32_t buriedCount = 0, buryTested = 0, buryRays = 0, buriedGround = 0;
		static constexpr float BURY_REACH = 1000.0f;       ///< D112: how far the burial rays reach from a vertex (D126: 500, was 160; D131: 1000, the parity test's far ends out of the big cliffs)
		static constexpr float BURY_MARGIN = 24.0f;        ///< D112: rock this near short of the vertex does not bury it (vanilla's sheets dip into the rock they lie on, as BAKE_ROCK_MARGIN)
		static constexpr float BURY_LAND = 8.0f;           ///< D126: a vertex this far under the landscape's surface is buried (D113's 12 on level ground only, from the height grid)
		static constexpr float LAND_AGREE = 16.0f;         ///< D138: a ground hit counts as the landscape only within this of TES::GetLandHeight there
		static constexpr float LAND_RAY_ABOVE = 4000.0f;   ///< D126: the landscape ray starts this far over the point...
		static constexpr float LAND_RAY_DROP = 20000.0f;   ///< D126: ...and looks this far down
		static constexpr float SUBMERGED_BED = 64.0f;      ///< D126: a vertex at most this far over the landscape, under our pools' water, lies in the river (not the air under a lip)
		static constexpr float SUBMERGED_COLUMN = 256.0f;  ///< D131: an engine water plane counts for the submerged cut where it stands at most this far over the landscape (a river over its bed)
		static constexpr float POOL_CUT_DEPTH = 8.0f;      ///< D112: FallGround::POOL_CUT: the shaders cut the foot this far under the pools' surface
		/** @brief D112: a lit mesh as the culling sees it: world positions at rest and as drawn, outer sides, open edges, bins. */
		struct CullSheet
		{
			const RE::BSGeometry* geometry = nullptr;
			uint32_t base = 0;
			std::vector<RE::NiPoint3> positions, drawn, outward, shown;  ///< shown: the normal the see-through pass reads (vanilla's, or the winding's)
			std::vector<std::array<uint16_t, 3>> triangles;
			std::vector<int8_t> side;   ///< Each triangle's outer side (+1 its winding), the same over the sheet (OrientSheet)
			std::vector<uint8_t> open;  ///< Vertices on an open edge
			float lo[3]{ FLT_MAX, FLT_MAX, FLT_MAX }, hi[3]{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
			float top = -FLT_MAX, area = 0.0f;
			std::unordered_map<int64_t, std::vector<uint32_t>> bins;  ///< Its triangles by SEAM_BIN cell (each in every cell its bound touches)
			std::vector<std::array<float, 5>> planes;                 ///< D131: the engine's water planes over it (CollectWaterPlanes)
		};
		/** @brief D112: one lit mesh as the culling sees it (D113: built one mesh a step by CullStep). */
		CullSheet BuildCullSheet(RE::BSGeometry* a_geometry, uint32_t a_base);
		/**
		 * @brief D112, D113: everything that decides which vertices show is to be worked out again from what is known now:
		 * asked whenever a cut or the rock becomes known (the seams, the rock test, the lead-in cuts, new pools). CullStep
		 * does the work over several frames (D113: 700 ms on one frame in game), restarting if asked again meanwhile.
		 */
		void RequestCulling() { cullRequested = true; }
		/**
		 * @brief D113: the culling, a few milliseconds a frame, in order: the sheets (one a step), the pools' cut, the inside
		 * cut (VolumeCut), a.x, then the sides (SideFlags). The GPU is handed the result once all are done.
		 */
		void CullStep();
		struct CullJob
		{
			std::vector<CullSheet> sheets;
			int stage = 0;
			std::size_t sheet = 0, vertex = 0;  ///< Where the current stage is
			uint32_t inside = 0, tested = 0, inward = 0, covered = 0, sideTested = 0, submerged = 0, meeting = 0;
			uint32_t islands = 0, islandVertices = 0, holes = 0, holeVertices = 0;  ///< D138: CleanupSheet's
			std::chrono::steady_clock::time_point started;
			double busy = 0.0;  ///< Milliseconds of work over its frames
		};
		std::unique_ptr<CullJob> cullJob;
		bool cullRequested = false;
		bool CullBusy() const { return cullRequested || cullJob != nullptr; }
		bool buryDone = true;  ///< D112: every queued mesh's rock test is done and the culling has been asked to read it
		/** @brief D112: VertexCut::pool for one vertex: its foot under the pools' surface, where the shaders cut it. */
		void PoolCut(const CullSheet& a_sheet, uint32_t a_vertex);
		/**
		 * @brief D113: VertexCut::inside for one vertex: whether it lies in the space another sheet encloses (the falls body is
		 * the union of the C-shaped sheets, and only its outer surface should show). Rays from the vertex along the six axes,
		 * against each other sheet whose bound holds it: none may enter that sheet through its outer face, and at least
		 * VOLUME_VOTES must leave it through its outer face (the rest open on the rock or the water the sheet is closed by).
		 * Inside, cut from VOLUME_MARGIN within the other's surface, so the two meet a little past their crossing, sealed.
		 */
		void VolumeCut(const std::vector<CullSheet>& a_sheets, std::size_t a_sheet, uint32_t a_vertex, CullJob& a_job);
		static constexpr float VOLUME_REACH = 400.0f;  ///< D113: world units each ray looks for the other sheet
		static constexpr int VOLUME_VOTES = 3;         ///< D113: rays of six that must leave the other sheet through its outer face
		static constexpr float VOLUME_MARGIN = 20.0f;  ///< D113: a vertex this near the other sheet's surface, inside it, still shows (about one of the dense copy's edges: no opening at the crossing)
		/**
		 * @brief D138: after every cut, on one sheet's graph (welded by position): small kept pieces cut off its main body are cut
		 * (VertexCut::island), small cut pieces enclosed by kept vertices and cut only by rock or another sheet are restored.
		 * Logs what cut its vertices and its largest pieces cut by rock or a sheet.
		 */
		void CleanupSheet(const CullSheet& a_sheet, CullJob& a_job);
		static constexpr std::size_t ISLAND_MOST_VERTICES = 64;  ///< D138: a kept piece off the main body this small (welded vertices) is cut...
		static constexpr float ISLAND_MOST_FRACTION = 0.01f;     ///< D138: ...or this small a share of the sheet's kept vertices
		static constexpr std::size_t BODY_SCRAP_VERTICES = 256;  ///< D138: a part's main body this small (welded vertices)...
		static constexpr float BODY_SCRAP_FRACTION = 0.03f;      ///< D138: ...and this small a share of its part as authored is a scrap the cuts left: cut too
		static constexpr std::size_t HOLE_MOST_VERTICES = 32;    ///< D138: an enclosed piece cut by rock or a sheet this small is restored...
		static constexpr float HOLE_MOST_FRACTION = 0.002f;      ///< D138: ...or this small a share of the sheet's kept vertices
		/** @brief D126: VertexClip d.w for one vertex: the distance from where it is drawn to the nearest shown surface of another sheet, up to MEET_REACH, else NO_WATER. */
		void MeetDistance(const std::vector<CullSheet>& a_sheets, std::size_t a_sheet, uint32_t a_vertex, CullJob& a_job);
		static constexpr float MEET_REACH = 64.0f;  ///< D126: world units within which another sheet's surface counts as meeting this one
		static constexpr float MEET_MOVE = 32.0f;   ///< D126: how far the drawn positions may lie from the binned rest ones (the seams' and the swell's moves)
		/** @brief D103, D113: a.w's sides for one vertex: its normal faces in (+2), another shown sheet's outer face covers its inner side (+4). */
		void SideFlags(const std::vector<CullSheet>& a_sheets, std::size_t a_sheet, uint32_t a_vertex, CullJob& a_job);
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
		static constexpr float FADE_REACH = 32.0f;             ///< D67: an open edge's vertex this near another mesh's surface fades
		static constexpr float SEAM_BIN = 48.0f;               ///< D91: world units per cell of the seams' triangle bins
		static constexpr float INSIDE_REACH = 45.0f;           ///< D103: another sheet's outer face this near behind a vertex's inner side covers it (D98-D107's curl cut reach; D113: the curl cut is VolumeCut now)
		static constexpr float COVER_NEAR = 6.0f;              ///< D103: a sheet behind a vertex's inner side counts from this far (its own surface is not one)
		static constexpr int EDGE_SMOOTH_PASSES = 80;          ///< D86: rounds smoothing each mesh's open edge as a curve (D92: 80: on the dense copies' 20-unit edges, 10 smoothed only 60 units)...
		static constexpr float EDGE_SMOOTH_MOST = 60.0f;       ///< D86: ...moving a vertex at most this far (D92: 60)
		static constexpr float EDGE_BLEND_REACH = 120.0f;      ///< D94: world units the open edge's moves fade into the surface over...
		static constexpr int EDGE_BLEND_PASSES = 60;           ///< D94: ...in this many rounds
		static constexpr float MOST_SHIFT = 120.0f;            ///< D96: a vertex's composed move past this (model units) is a bug: it is left still
		static constexpr float LEAD_CUT_DEPTH = 16.0f;         ///< D97: a hidden lead-in vertex this far under the water over it is cut
		static constexpr float LEAD_WATER_OVER_CREST = 48.0f;  ///< D103: ...by water no higher than this over its column's crest (a plane's bound reaches past its water)
		std::vector<RE::NiPointer<RE::BSGeometry>> clipQueue;
		uint64_t vertexClipVersion = 0;
		uint32_t clipTested = 0;
		static constexpr float SOURCE_CROSSING_BAND = 96.0f;  ///< D141: a water level this near a mesh's top is the water it pours out of: no crossing foam there
		static constexpr float SOURCE_DEPTH_MOST = 240.0f;    ///< D138: the most a water plane may lie over a falls vertex and still be the water it pours out of
		static constexpr float SOURCE_REACH = 96.0f;          ///< D124: world units past a water plane's bound a falls vertex still counts as pouring out of it (the lip leans out over the edge)
		static constexpr float CLIP_REACH = 240.0f;           ///< D56: world units each ray out of a vertex looks for rock
		static constexpr float CLIP_MARGIN = 6.0f;            ///< A vertex this far inside rock still shows (water lying on it)
		static constexpr float CLIP_RANGE = 32.0f;            ///< The most a vertex counts as in or out, so the cut stays near the rock's face
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
			std::vector<uint8_t> untested;             ///< D83: hidden lead-in texels no strand comes near: the rock test passes them by
			std::vector<float> clearFront, clearBack;  ///< D104: world units from each texel to the first rock along its normal (front) and against it (back), up to CLEAR_REACH
			std::filesystem::path cache;               ///< D82: where its finished bake is kept (empty: not kept)
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
			std::vector<uint8_t> rock;                 ///< D75: texels inside rock (the debug arrows skip them)
			std::vector<float> swell;                  ///< D82: how much the water piles up there, softened (the vertex push)
			std::vector<float> disturbance;            ///< D85: how stirred the water is there (the wobble's size), softened
			std::vector<float> clearFront, clearBack;  ///< D104: as BakeJob's: the open water or air on each side of the sheet (its depth behind, once its outer side is known)
		};
		static constexpr uint32_t BAKE_CACHE_VERSION = 8;  ///< D82: bump whenever a bake would come out different (D85: 2, the disturbance; D91: 3, the evenly split copies; D104: 4, the clearances; D126: 7, the domain was the culled shell; D132: 8, the whole mesh again)
		static constexpr float CLEAR_REACH = 600.0f;       ///< D104: world units the clearance rays look for rock
		static constexpr float CLEAR_START = 40.0f;        ///< D105: the clearance rays start this far off the other side of the sheet
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
		static constexpr float WOBBLE_SLOPE = 0.5f;        ///< D99: the wobble's size climbs at most this many units per unit along the mesh...
		static constexpr float PUSH_SLOPE = 0.5f;          ///< D99: ...and the swell's push likewise (from 0 at open edges and rock: no spikes over one triangle)
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
