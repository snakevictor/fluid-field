#pragma once

#include "FallCapture.h"
#include "LandscapeCache.h"
#include "ShallowWaterSolver.h"
#include "WaterSurfaces.h"

#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ImDrawList;

namespace FluidFieldSim
{
	/** @brief D35: where falling water meets the water below, for its pushes on the simulated water. */
	struct FallPlunge
	{
		float position[3];   ///< On the water below, world units
		float direction[2];  ///< Horizontal way the falling water runs, unit (or zero)
		float speed;         ///< World units per second it arrives at
		float strength;      ///< 0 to 1
		float reach = 1.0f;  ///< D98: foam reach relative to a plunge's (1.5 where a mesh crosses a water surface)
	};

	/**
	 * @brief D25: waterfall sites rebuilt from vanilla's layout, and the cached sites' sheets drawn in their place.
	 *
	 * S1: loaded waterfall and rapids effect references, classified by model path. S2: falling water and skirts within
	 * CLUSTER_RADIUS form one site; mist, splashes and rapids join the nearest fall, else cluster within
	 * RAPIDS_CLUSTER_RADIUS (wider links whole gorges). S3: nearby water planes give the levels; a drop under
	 * RAPIDS_THRESHOLD is rapids. S4: FallCapture casts the rock under each site with a drop. Main thread.
	 */
	class FallSites
	{
	public:
		/** @brief A world-space triangle. */
		using Triangle = std::array<RE::NiPoint3, 3>;

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
			std::vector<uint32_t> members;  ///< Indices into refs
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
		/** @brief D172: the shared landscape height cache the rock test reads. */
		void SetLandscape(const LandscapeCache* a_landscape) { landscape = a_landscape; }
		void Clear();

		/** @brief A site's ground capture; null for rapids and failed sites. */
		const FallCapture* GetCapture(const Site& a_site) const;
		/** @brief Counts for the menu: ground captures complete, and in all. */
		void CountCaptures(uint32_t& a_complete, uint32_t& a_total) const;

		/** @brief D26: whether a geometry is a falling-water mesh at a ground-captured site; the pointer is only compared. */
		bool IsGrounded(const RE::BSGeometry* a_geometry) const { return grounded.contains(a_geometry); }
		/** @brief D56: a sheet's first index into GetVertexClips (per vertex: cuts, fades, water height, shifts). */
		bool GetVertexClip(const RE::BSGeometry* a_geometry, uint32_t& a_base) const
		{
			const auto it = vertexClipBase.find(a_geometry);
			if (it == vertexClipBase.end())
				return false;
			a_base = it->second;
			return true;
		}
		const std::vector<std::array<float, 16>>& GetVertexClips() const { return vertexClips; }
		/** @brief D61: a cached site's sheet. */
		struct LitMesh
		{
			RE::NiPointer<RE::BSGeometry> geometry;
		};
		/** @brief The cached sites' sheets the falls pass draws. */
		const std::vector<LitMesh>& GetLitMeshes() const { return litMeshList; }
		/** @brief Whether the falls pass draws the cached sites; their vanilla pieces hide meanwhile. */
		void SetTakeover(bool a_on);
		/** @brief D78: the pool surface over a world point, or false where there is none yet. */
		bool PoolLevelAt(float a_x, float a_y, float& a_level) const;
		/** @brief D112: the height the shaders cut the falls' foot at, or false with no pool. */
		bool PoolCutAt(float a_x, float a_y, float& a_level) const;
		/** @brief D66: every engine water plane over a box: min x, min y, max x, max y, height. */
		void CollectWaterPlanes(float a_minX, float a_minY, float a_maxX, float a_maxY, std::vector<std::array<float, 5>>& a_planes) const;

		/** @brief D179: a sheet's flow bake on its texture grid (u across, v down), as the GPU reads it. */
		struct FlowBake
		{
			uint32_t width = 0;
			uint32_t height = 0;
			float uvMin[2]{};
			float uvSize[2]{};
			std::vector<float> texels;  ///< RGBA32F, w x (h x 5): flow, fields, stretch and frame, plunges, contact
			uint64_t stamp = 0;         ///< New whenever the texels change on the render thread (0: not yet stamped)
		};
		const std::unordered_map<const RE::BSGeometry*, FlowBake>& GetFlowBakes() const { return flowBakes; }
		/** @brief Changes whenever GetFlowBakes changes. */
		uint64_t GetFlowBakeVersion() const { return flowBakeVersion; }
		/** @brief Changes whenever GetVertexClips grows. */
		uint64_t GetVertexClipVersion() const { return vertexClipVersion; }
		/** @brief Changes whenever the captured ground or the grounded meshes change. */
		uint64_t GetGroundVersion() const { return groundVersion; }
		/** @brief Every site's captured ground on an a_size square grid from a_origin; FallCapture::NONE where none. */
		void BuildGroundMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_heights) const;

		/** @brief D35: the plunges' pushes on the simulated water, in a level's texels, nearest the grid's centre first. */
		void AppendWaterEmitters(std::vector<WaterEmitter>& a_emitters, const int32_t a_gridMin[2], float a_cellSize, uint32_t a_gridRes, float a_strength,
			uint32_t a_maxEmitters, bool a_foam) const;

		/** @brief The parts of a vanilla falling-water model, by what draws them. */
		enum class Layer : uint8_t
		{
			Inner,     ///< No effect shader (the model's inner or helper surfaces)
			Sheet,     ///< The white-water sheet (FXWhiteWater)
			Streaks,   ///< Other effect-shaded surfaces: the streaky fluid layers (FXfluidTile)
			Particles  ///< Particle systems and parts with no triangles
		};
		static Layer LayerOf(RE::BSGeometry* a_geometry);
		static constexpr float NO_SURFACE = -1.0e7f;

		/** @brief D36: per ground-map texel (a_cell = FallCapture::CELL), the highest pool over it, else FallCapture::NONE. */
		void BuildPoolMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_heights) const;

		/** @brief D44: per ground-map texel, how strongly the falls plunge there (0 to 1). */
		void BuildPlungeMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_field, float a_behind = 0.0f, float a_radiusScale = 1.0f) const;

		/** @brief D170: per ground-map texel, the river's pull toward a fall's lip: xy velocity (units/s), z strength. */
		void BuildLipMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_field) const;
		mutable std::size_t loggedLips = SIZE_MAX;  ///< Lip count last logged

		/** @brief Writes each cached site's sheets, statics, landscape and captured rock next to the log, for tools/unify. */
		void SetExportSiteInputs(bool a_on) { exportSiteInputs = a_on; }
		/** @brief Dev: rereads a site's bake when its file changes on disk. */
		void SetHotReloadBakes(bool a_on) { hotReloadBakes = a_on; }

		static constexpr float CLUSTER_RADIUS = 512.0f;
		static constexpr float RAPIDS_CLUSTER_RADIUS = 128.0f;  ///< Rapids and splashes away from any fall cluster only this close
		static constexpr float PAIR_MARGIN = 256.0f;
		static constexpr float NEAR_LEVEL = 256.0f;       ///< How far below its falling water a plane may lie and still pair
		static constexpr float NEAR_LEVEL_ABOVE = 64.0f;  ///< How far above its falling water's top a plane may lie and still pair
		static constexpr float LEVEL_TOLERANCE = 16.0f;
		static constexpr float RAPIDS_THRESHOLD = 64.0f;
		static constexpr double CAPTURE_BUDGET_MS = 3.0;     ///< D185: ray casting per update
		static constexpr int64_t CAPTURE_CACHE_VERSION = 1;  ///< D185: bump whenever a capture would come out different
		/** @brief D185: a capture's disk cache path, hashed from its layout and nearby statics' triangles. */
		std::string CaptureCachePath(const FallCapture& a_capture, uint64_t a_key) const;

	private:
		static constexpr uint32_t SCAN_INTERVAL = 60;

		void Rebuild(const WaterSurfaces& a_water);
		void Summarise();
		void CaptureStep(bool a_canFinish);
		/** @brief Collects the geometry of the falling-water meshes at sites with captured ground. */
		void RefreshGrounded();

		std::vector<FxRef> refs;
		std::vector<Site> sites;
		uint64_t refSignature = 0;
		uint64_t waterSignature = 0;
		uint32_t updatesUntilScan = 0;
		bool exportSiteInputs = false;
		bool hotReloadBakes = false;
		std::unordered_map<uint64_t, std::string> cellSummaries;  ///< Last summary logged per cell
		std::unordered_map<uint64_t, FallCapture> captures;       ///< By Site::captureKey
		std::unordered_set<const RE::BSGeometry*> grounded;
		std::vector<LitMesh> litMeshList;
		std::vector<RE::NiPointer<RE::BSGeometry>> litHidden;  ///< D91: sheets app-culled while the falls pass draws them
		/** @brief D91: app-culls the current sheets and forgets the ones gone. */
		void ApplyLitHidden();
		bool takeover = false;                           ///< SetTakeover
		std::unordered_set<RE::FormID> unifiedSiteRefs;  ///< Every reference of a cached site, hidden on takeover
		/** @brief D56: tests queued sheets' vertices against the rock, a few ms a frame. */
		void ClipStep();
		/** @brief D106: each sheet's depth behind its vertices, from its bake, once its cuts are done. */
		void DepthStep();
		std::vector<RE::NiPointer<RE::BSGeometry>> depthQueue;
		std::unordered_map<const RE::BSGeometry*, std::vector<RE::NiPoint3>> outwardNormals;  ///< D109: each sheet's consistent outward normals, world
		std::unordered_map<const RE::BSGeometry*, uint32_t> vertexClipBase;
		std::vector<std::array<float, 16>> vertexClips;
		/** @brief D112: why each vertexClips vertex is hidden, kept apart so each cut can be redone. */
		struct VertexCut
		{
			float water = NO_CUT;      ///< ClipStep: the engine's water at the mesh's foot
			float inside = NO_CUT;     ///< VolumeCut: inside the space another sheet encloses, past VOLUME_MARGIN
			float lead = NO_CUT;       ///< ComputePushes: the hidden lead-in under the river
			float pool = NO_CUT;       ///< Under our pools at the foot, as the shaders cut it (not in a.x)
			float submerged = NO_CUT;  ///< CullStep: LEAD_CUT_DEPTH or more under our pools' water near the bed
			float rock = NO_CUT;       ///< BuryStep: BURY_LAND under the landscape, or BURY_MARGIN inside a static
			float island = NO_CUT;     ///< CleanupSheet: a small kept piece cut off from its sheet's main body
			float ground = -FLT_MAX;   ///< BuryStep: the landscape's height under it (-FLT_MAX: none, or an interior)
			uint8_t buried = 0;        ///< BuryStep: under the landscape or unreachable from open air (not in a.x)
		};
		std::vector<VertexCut> vertexCuts;      ///< Parallel to vertexClips
		static constexpr float NO_CUT = 38.0f;  ///< A VertexCut part that hides nothing (CLIP_MARGIN + CLIP_RANGE)
		/** @brief D112: recomposes a vertex's a.x as the least of its a.x parts. */
		void ComposeCut(std::size_t a_index);
		/** @brief D112: whether the GPU draws a falls vertex: not cut, not in rock, not faded at a seam. */
		bool VertexShown(std::size_t a_index) const;
		/**
		 * @brief D126: whether a vertex is on the falls body's outer shell: not buried, inside another sheet or lead-in.
		 * Unlike VertexShown it ignores the waterline cuts and seam fade, since plunges belong there.
		 */
		bool VertexInShell(std::size_t a_index) const;
		/** @brief D126: VertexInShell on a bake's texel grid (empty before culling); a_hash gets the kept vertices' hash. */
		std::vector<uint8_t> ShellTexels(RE::BSGeometry* a_geometry, uint32_t a_width, uint32_t a_height, const float a_uvMin[2], const float a_uvSize[2], uint64_t* a_hash) const;
		/**
		 * @brief D131: finds vertices under the landscape or inside a static (four of six axis rays cross an odd number of
		 * its faces within BURY_REACH), a few ms a frame.
		 */
		void BuryStep();
		std::vector<RE::NiPointer<RE::BSGeometry>> buryQueue;
		uint32_t buriedCount = 0, buryTested = 0;
		static constexpr double BURY_BUDGET_MS = 1.5;  ///< D172: the rock test's share of a frame
		const LandscapeCache* landscape = nullptr;     ///< SetLandscape
		/** @brief A run of triangles, three indices each. */
		struct IndexSpan
		{
			const uint16_t* data = nullptr;
			uint32_t count = 0;
		};
		/** @brief The mesh's triangles, as its bake reads them. */
		IndexSpan BakeIndices(RE::BSGeometry* a_geometry) const;
		static constexpr float BURY_REACH = 1000.0f;        ///< D131: parity rays must end outside big cliffs
		static constexpr float BURY_MARGIN = 24.0f;         ///< D112: shallower rock does not bury (sheets dip into it)
		static constexpr float BURY_LAND = 24.0f;           ///< D126: this far under the land is buried (runs dip in)
		static constexpr float SUBMERGED_DEPTH = 48.0f;     ///< D157: a vertex this far under the river's water, on its bed, is in the river
		static constexpr float SUBMERGED_BED = 16.0f;       ///< D157: this near the land under our pools is in the river
		static constexpr float SUBMERGED_COLUMN = 256.0f;   ///< D131: water planes this near the land count as a river
		static constexpr float SUBMERGED_OVER_TOP = 96.0f;  ///< ...and at most this far over the sheet's own top (not the river of the drop above)
		static constexpr float POOL_CUT_DEPTH = 8.0f;       ///< Matches FallGround::POOL_CUT: the foot is cut this far under the pools' surface
		/** @brief D112: a sheet as the culling sees it: world positions at rest and as drawn, outer sides, open edges, bins. */
		struct CullSheet
		{
			const RE::BSGeometry* geometry = nullptr;
			uint32_t base = 0;
			std::vector<RE::NiPoint3> positions, drawn, outward, shown;  ///< shown: the normal the falls pass reads
			std::vector<std::array<uint16_t, 3>> triangles;
			std::vector<int8_t> side;   ///< Each triangle's outer side (+1 its winding), the same over the sheet (OrientSheet)
			std::vector<uint8_t> open;  ///< Vertices on an open edge
			float lo[3]{ FLT_MAX, FLT_MAX, FLT_MAX }, hi[3]{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
			float top = -FLT_MAX, area = 0.0f;
			std::unordered_map<int64_t, std::vector<uint32_t>> bins;  ///< Its triangles by SEAM_BIN cell (each in every cell its bound touches)
			std::vector<std::array<float, 5>> planes;                 ///< D131: the engine's water planes over it
		};
		/** @brief D113: builds one sheet's CullSheet (one per CullStep). */
		CullSheet BuildCullSheet(RE::BSGeometry* a_geometry, uint32_t a_base);
		/** @brief D113: asks for the culling to be redone when a cut or the rock becomes known; workers may call it. */
		void RequestCulling() { cullRequested = true; }
		/** @brief D113: the culling, spread over frames: sheets, pools' cut, VolumeCut, a.x, SideFlags; the GPU gets it once done. */
		void CullStep();
		struct CullJob
		{
			std::vector<CullSheet> sheets;
			int stage = 0;
			std::size_t sheet = 0, vertex = 0;                                                                   ///< Where the current stage is
			std::atomic<uint32_t> inside = 0, tested = 0, inward = 0, covered = 0, sideTested = 0, meeting = 0;  ///< D185: its stages run on several threads
			uint32_t submerged = 0;
			uint32_t islands = 0, islandVertices = 0, islandTriangles = 0, holes = 0, holeVertices = 0;  ///< CleanupSheet's
			std::chrono::steady_clock::time_point started;
			double busy = 0.0;                                                       ///< Milliseconds of work over its frames
			std::vector<std::pair<RE::NiPointer<RE::BSGeometry>, uint32_t>> meshes;  ///< D172: the sheets and their bases, for stage 0 on the worker
			std::atomic<bool> cancel = false;                                        ///< Stops the worker
			std::atomic<bool> complete = false;                                      ///< The worker finished every stage
		};
		std::unique_ptr<CullJob> cullJob;
		std::future<void> cullWork;  ///< D172: stages 1-5 of cullJob on a worker
		/** @brief D172: whether the culling's worker runs; it owns vertexClips, vertexCuts and the pools meanwhile. */
		bool CullWorking() const { return cullWork.valid(); }
		std::future<void> heavyWork;  ///< D172: ComputeFades or LinkBakes on a worker
		bool linkedDue = false;       ///< LinkBakes finished: its version and plunges are due on the render thread
		/** @brief D172: what ComputePlunges leaves for ApplyPlunges. */
		struct PlungeResult
		{
			std::vector<FallPlunge> plungePoints;
			std::unordered_map<const RE::BSGeometry*, std::vector<float>> impacts, landings;  ///< D188: per bake texel: where the water lands in a pool, and its foam carried on
			uint32_t meets = 0, lips = 0;
		};
		std::unique_ptr<PlungeResult> plungeResult;  ///< The plunges' worker's result
		/**
		 * @brief D191: per sheet, what the wobble needs to add the plunges' impacts without rerunning ComputePushes (too slow):
		 * per vertex the bake's wobble size, weight, direction (model units per world unit) and texture coordinates.
		 */
		struct WobbleImpact
		{
			std::vector<float> size, weight;
			std::vector<std::array<float, 2>> uv;
			std::vector<RE::NiPoint3> direction;
		};
		std::unordered_map<const RE::BSGeometry*, WobbleImpact> wobbleImpacts;
		bool wobbleImpactsDue = false;  ///< ApplyPlunges wrote new impacts; applied once no worker holds vertexClips
		/** @brief D191: each sheet's wobble size: ComputePushes' plus WOBBLE_IMPACT by the plunges' impact. */
		void ApplyWobbleImpacts();
		/** @brief D185: where a load's time goes, per Update step and per worker. */
		enum class LoadStage : uint8_t
		{
			Scan,
			Capture,
			Clip,
			Bury,
			Cull,
			Depth,
			Bake,
			Pools,
			Finish,
			Count
		};
		enum class LoadWork : uint8_t
		{
			Pools,
			Seams,
			Cull,
			Link,
			Plunges,
			Count
		};
		struct LoadTiming
		{
			double mainMs[static_cast<std::size_t>(LoadStage::Count)]{};
			uint32_t mainFrames[static_cast<std::size_t>(LoadStage::Count)]{};
			std::atomic<int64_t> workerUs[static_cast<std::size_t>(LoadWork::Count)]{};
			std::atomic<uint32_t> workerRuns[static_cast<std::size_t>(LoadWork::Count)]{};
			std::chrono::steady_clock::time_point firstActive, lastActive;
			bool active = false;
			uint32_t reports = 0;
			void AddWorker(LoadWork a_work, std::chrono::steady_clock::time_point a_start)
			{
				const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - a_start).count();
				workerUs[static_cast<std::size_t>(a_work)] += us;
				workerRuns[static_cast<std::size_t>(a_work)]++;
			}
		};
		LoadTiming loadTiming;
		static constexpr double SETTLE_SECONDS = 10.0;  ///< D185: idle this long, the falls have settled
		/** @brief D185: whether none of the falls' work is running or queued. */
		bool LoadIdle() const;
		/** @brief D185: notes activity; logs once the falls have been idle SETTLE_SECONDS. */
		void TrackLoad();
		bool plungesDue = false;  ///< D172: several asks, one build
		void ComputePlunges(PlungeResult& o_result) const;
		void ApplyPlunges(PlungeResult& a_result);
		/** @brief D172: whether a worker owns vertexClips, vertexCuts, the pools and the bakes. */
		bool Busy() const { return cullWork.valid() || heavyWork.valid(); }
		/** @brief D172: takes in a finished heavyWork and what it leaves to the render thread. */
		void FinishHeavyWork();
		/** @brief D172: the culling's stages 1-5, on the worker. */
		void RunCullStages(CullJob& a_job);
		std::atomic<bool> cullRequested = false;
		bool CullBusy() const { return cullRequested || cullJob != nullptr; }
		bool buryDone = true;  ///< Every queued rock test is done and the culling asked to read it
		/** @brief D112: VertexCut::pool for one vertex. */
		void PoolCut(const CullSheet& a_sheet, uint32_t a_vertex);
		/**
		 * @brief D113: VertexCut::inside: whether a vertex lies in space another sheet encloses, so only the body's outer
		 * surface shows. Of six axis rays none may enter through that sheet's outer face and VOLUME_VOTES must leave by it.
		 */
		void VolumeCut(const std::vector<CullSheet>& a_sheets, std::size_t a_sheet, uint32_t a_vertex, CullJob& a_job);
		static constexpr float VOLUME_REACH = 400.0f;  ///< World units each ray looks for the other sheet
		static constexpr int VOLUME_VOTES = 4;         ///< D143: rays of six that must leave through the outer face (clearly inside only)
		static constexpr float VOLUME_MARGIN = 20.0f;  ///< This near inside the other sheet still shows, so the seam stays sealed
		/** @brief D138: after every cut, cuts small pieces off a sheet's main body and restores small holes rock or a sheet made. */
		void CleanupSheet(const CullSheet& a_sheet, CullJob& a_job);
		static constexpr std::size_t ORPHAN_MOST_TRIANGLES = 128;  ///< D143: a kept piece off its part's body this small (drawn triangles) is cut...
		static constexpr float ORPHAN_MOST_FRACTION = 0.01f;       ///< ...or this small a share of its part's drawn triangles
		static constexpr std::size_t BODY_SCRAP_TRIANGLES = 512;   ///< D138: a part's main body this small (drawn triangles)...
		static constexpr float BODY_SCRAP_FRACTION = 0.03f;        ///< ...and this small a share of its part as authored is cut too
		static constexpr std::size_t HOLE_MOST_VERTICES = 96;      ///< A hole cut by rock or a sheet this small is restored...
		static constexpr float HOLE_MOST_FRACTION = 0.005f;        ///< ...or this small a share of the sheet's kept vertices
		/** @brief D126: VertexClip d.w: distance to another sheet's nearest shown surface within MEET_REACH, else NO_WATER. */
		void MeetDistance(const std::vector<CullSheet>& a_sheets, std::size_t a_sheet, uint32_t a_vertex, CullJob& a_job);
		static constexpr float MEET_REACH = 64.0f;          ///< World units within which another sheet counts as meeting this one
		static constexpr std::size_t CULL_CHUNK = 256;      ///< D185: vertices a culling thread takes at a time
		static constexpr double CULL_DEFER_MOST = 20.0;     ///< D185: seconds a culling pass may wait for work that would ask for another
		std::chrono::steady_clock::time_point cullAskedAt;  ///< When the waiting pass was first asked for
		bool cullWaiting = false;
		static constexpr float MEET_MOVE = 32.0f;  ///< How far drawn positions may lie from the binned rest ones
		/** @brief D103: a.w's sides for one vertex: normal faces in (+2), another sheet covers its inner side (+4). */
		void SideFlags(const std::vector<CullSheet>& a_sheets, std::size_t a_sheet, uint32_t a_vertex, CullJob& a_job);
		static constexpr float GAP_REACH = 40.0f;        ///< D69: an open edge this near another mesh's, not over it, closes the gap
		static constexpr float GAP_OVERLAP = 4.0f;       ///< ...each side moving past the middle by this much
		static constexpr float WATER_KEEP = 48.0f;       ///< D68: cut this far under the water it falls into
		static constexpr float WATER_CUT_BAND = 160.0f;  ///< ...only this near its mesh's foot (a plane's bound reaches past its water)
		static constexpr float NO_WATER = 1.0e4f;        ///< D68: a vertex's height above the water below where there is none
		/** @brief D67: per sheet, open edges over a leading mesh fade to 0 so it shows through; the rest stay 1. */
		void ComputeFades();
		static constexpr float FADE_REACH = 32.0f;             ///< D67: an open edge's vertex this near another mesh's surface fades
		static constexpr float SEAM_BIN = 48.0f;               ///< D91: world units per cell of the seams' triangle bins
		static constexpr float INSIDE_REACH = 45.0f;           ///< D103: another sheet's outer face this near behind a vertex's inner side covers it
		static constexpr float COVER_NEAR = 6.0f;              ///< A sheet behind a vertex's inner side counts from this far (not its own surface)
		static constexpr int EDGE_SMOOTH_PASSES = 80;          ///< D86: rounds smoothing open edges as curves...
		static constexpr float EDGE_SMOOTH_MOST = 60.0f;       ///< ...moving a vertex at most this far
		static constexpr float EDGE_BLEND_REACH = 120.0f;      ///< D94: world units the open edge's moves fade into the surface over...
		static constexpr int EDGE_BLEND_PASSES = 60;           ///< ...in this many rounds
		static constexpr float MOST_SHIFT = 120.0f;            ///< D96: a composed move past this (model units) is a bug, so the vertex stays still
		static constexpr float LEAD_CUT_DEPTH = 16.0f;         ///< D97: a hidden lead-in vertex this far under the water over it is cut
		static constexpr float LEAD_WATER_OVER_CREST = 48.0f;  ///< ...by water no higher than this over its column's crest
		std::vector<RE::NiPointer<RE::BSGeometry>> clipQueue;
		std::atomic<uint64_t> vertexClipVersion = 0;  ///< D172: workers bump it too
		uint32_t clipTested = 0;
		static constexpr float SOURCE_CROSSING_BAND = 96.0f;  ///< D141: a water level this near a mesh's top is its source: no crossing foam there
		static constexpr float SOURCE_DEPTH_MOST = 240.0f;    ///< D138: the most a water plane may lie over a falls vertex and still be its source
		static constexpr float CREST_RISE = 64.0f;            ///< D174: the most an upper-half vertex may stand over its source (the crest's bulge)
		static constexpr float SOURCE_REACH = 96.0f;          ///< D124: world units past a plane's bound a vertex still pours out of it (the lip leans out)
		static constexpr float CLIP_MARGIN = 6.0f;            ///< A vertex this far inside rock still shows (water lying on it)
		static constexpr float CLIP_RANGE = 32.0f;            ///< The most a vertex counts as in or out, so the cut stays near the rock's face
		static constexpr double CLIP_BUDGET_MS = 3.0;
		static constexpr uint32_t BAKE_MOST_SIDE = 512;  ///< The most bake texels along a side (a whole site)
		static constexpr float SIM_GRAVITY = 686.0f;     ///< D74: world units per second squared (Skyrim's)
		static constexpr float SIM_INFLOW = 250.0f;      ///< D74: world units per second the water comes onto a mesh's top at
		static constexpr float SIM_TERMINAL = 900.0f;    ///< D74: world units per second sliding water tends to (its drag)
		struct BakeField;
		/** @brief A site's offline sheet (tools/unify), from FluidField/Sites/*.unfs, drawn in place of its members' meshes. */
		struct UnifiedCache
		{
			std::string path;
			std::vector<RE::FormID> members;      ///< The site's falls references (resolved from plugin and local ID)
			std::vector<RE::NiPoint3> positions;  ///< World space
			std::vector<RE::NiPoint3> normals;    ///< Toward the viewer
			std::vector<RE::NiPoint2> uvs;        ///< One chart: the bake's grid
			std::vector<uint8_t> flags;           ///< +1 inside rock (the bake ignores it)
			std::vector<RE::NiColorA> colors;     ///< The members' vertex colours (vanilla's water tint)
			std::vector<uint16_t> indices;
			std::shared_ptr<BakeField> bake;  ///< Its offline flow bake (.ffbk beside it), or null: the site stays vanilla
			std::filesystem::path bakePath;
			std::filesystem::file_time_type bakeTime{};  ///< The read bake's write time, for hot reload
		};
		struct UnifiedShape
		{
			RE::NiPointer<RE::BSTriShape> shape;
			RE::NiPointer<RE::NiNode> parent;
			std::vector<RE::NiPointer<RE::BSGeometry>> hidden;  ///< The members' meshes it replaces (app-culled)
		};
		std::vector<UnifiedCache> unifiedCaches;
		bool unifiedLoaded = false;
		std::unordered_map<std::size_t, UnifiedShape> unifiedOf;  ///< By cache index
		std::unordered_set<const RE::BSGeometry*> unifiedShapes;
		std::unordered_set<const RE::BSGeometry*> linkUnified;  ///< unifiedShapes as LinkBakes' worker started
		/** @brief Reads every site cache once, resolving members through the data handler. */
		void LoadUnifiedCaches();
		/** @brief Hot reload: rereads and relinks site bakes rewritten on disk. */
		void ReloadChangedBakes();
		std::chrono::steady_clock::time_point bakeCheckedAt{};
		/** @brief Attaches a cache's shape under a_root (a member's model root), cloning a_material's lit property. */
		RE::BSTriShape* AttachUnified(std::size_t a_cache, RE::NiNode* a_root, RE::BSGeometry* a_material);
		/** @brief Detaches every site shape and shows the members' meshes again. */
		void RemoveUnified();
		static constexpr int32_t FRAME_REACH = 4;  ///< D186: texels each way the flow's frame is measured over
		static constexpr int FRAME_BLUR = 3;       ///< D186: passes softening it
		/** @brief D173: the statics' render triangles over an area (not the falls' models); o_report logs each reference's fate. */
		void GatherOccluders(float a_minX, float a_minY, float a_maxX, float a_maxY, std::vector<Triangle>& o_triangles, std::string* o_report = nullptr) const;
		/** @brief Writes each falls mesh with the statics and landscape round it next to the log, for the offline harness. */
		void DumpUnifyInputs() const;
		/** @brief A sheet's bake on its texture grid, before it is linked to the meshes above. */
		struct BakeField
		{
			uint32_t width = 0, height = 0;
			float uvMin[2]{}, uvSize[2]{};
			std::vector<RE::NiPoint3> position;
			std::vector<uint8_t> covered;
			std::vector<float> turn, speed, wake, foam;  ///< Across per down (texels), speed (world units/s over 300), aeration and foam
			std::vector<float> seed, along;              ///< D80: the lip column each texel's strand came over (texels), and its run from the top (texture v)
			std::vector<RE::NiPoint3> normal;
			std::vector<uint8_t> rock;                 ///< D75: texels inside rock
			std::vector<float> swell;                  ///< D82: how much the water piles up there (the vertex push)
			std::vector<float> disturbance;            ///< D85: how stirred the water is there (the wobble's size)
			std::vector<float> fallen;                 ///< D190: world units its water has recently fallen down steep faces
			std::vector<float> calm;                   ///< D190: world units run on calm water from the lip
			std::vector<RE::NiPoint3> flow;            ///< Flow direction (world, unit)
			std::vector<float> streak;                 ///< Streaks left (1, to 0 once its water has met rock)
			std::vector<float> contact, contactTail;   ///< Distance to rock, land or the pool, and that swept down the flow
			std::vector<float> bulge;                  ///< Heave over hidden rock (0 to 1)
			std::vector<float> clearFront, clearBack;  ///< D104: open water or air on each side of the sheet
		};
		static constexpr float PLUNGE_WHITE_HEIGHT = 300.0f;  ///< D190: world units from tangent to foot over which a fall whitens fully
		static constexpr float TANGENT_STEEP = 0.5f;          ///< D190: a fall's tangent is where its normal's upright part first drops under this
		static constexpr float TANGENT_KEEP_FALLEN = 60.0f;   ///< D190: a mesh keeps its feeder's tangent while its water has fallen this much recently
		static constexpr uint32_t BAKE_CACHE_VERSION = 26;    ///< Bump whenever a bake would come out different
		static constexpr float CLEAR_REACH = 600.0f;          ///< D104: world units the clearance rays look for rock
		/** @brief Reads a bake file; null if missing, invalid or of another version. */
		static std::shared_ptr<BakeField> LoadBake(const std::filesystem::path& a_path);
		/** @brief D82: pushes each sheet's vertices out where its water piles up (the bake's swell), none at open edges. */
		void ComputePushes();
		static constexpr float PUSH_MOST = 20.0f;                     ///< D82: world units the water swells out at most
		static constexpr float PUSH_FROM = 1.0f;                      ///< D82: swell the push starts at (the sheet mostly runs 0 to 2.5)...
		static constexpr float PUSH_FULL = 3.0f;                      ///< ...and is whole at (piling against rocks: 5 to 20)
		static constexpr float WOBBLE_MOST = 28.0f;                   ///< D85: world units the water wobbles at most, where most stirred...
		static constexpr float WOBBLE_CALM = 10.0f;                   ///< ...and everywhere else
		static constexpr float WOBBLE_STIRRED = 1.5f;                 ///< D85: disturbance at which the wobble is whole
		static constexpr float BOUNCE_MOST = 45.0f;                   ///< World units the most aerated, level water bobs up and down
		static constexpr float BOUNCE_AERATION[2] = { 0.35f, 0.8f };  ///< ...over this baked aeration
		static constexpr float BOUNCE_FACING[2] = { 0.45f, 0.85f };   ///< ...and this facing (|world normal z|: plunging sheets get none)
		static constexpr std::size_t BAKE_TEXELS_PER = 20;            ///< RGBA32F per texel across the flow bake's five sections (flow, fields, frame, plunges, contact)
		static constexpr float CONTACT_NONE = 1.0e4f;                 ///< Contact distance where none is baked
		static constexpr float UNIFIED_POOL_MOST = 35.0f;             ///< World units the baked pooling (swell) pushes out at most
		static constexpr float UNIFIED_WOBBLE = 0.8f;                 ///< Share of the falls' wobble along the normal
		static constexpr float TURB_SPAN = 8.0f;                      ///< Disturbance of whole turbulence (tools/unify/layers_to_ffbk.py)
		static constexpr float TURB_WOBBLE = 40.0f;                   ///< World units more wobble where wholly turbulent...
		static constexpr float TURB_PUSH = 15.0f;                     ///< ...and swell outward
		static constexpr float TURB_EDGE_SHARE = 0.4f;                ///< Share of WOBBLE_EDGE_REACH over which that fades toward rock and the edges
		static constexpr float WOBBLE_CRASH = 36.0f;                  ///< D191: world units more where a plunge crashes white
		static constexpr float WOBBLE_CRASH_FROM = 0.25f;             ///< ...from this progress down the fall...
		static constexpr float WOBBLE_CRASH_FULL = 0.7f;              ///< ...to whole at this
		static constexpr float PROGRESS_ABOVE_MOST = 0.98f;           ///< The most a fall's progress runs above its tangent (-1: no fall)
		static constexpr int PROGRESS_SOFTEN_PASSES = 6;              ///< Passes softening the progress (it comes in blocks)
		static constexpr float POOL_LINK_TOP = 64.0f;                 ///< D191: world units a fall's top may lie from its pool's surface to pour out of it
		static constexpr float POOL_LINK_ABOVE = 200.0f;              ///< World units a fall's crest may stand over its source (vanilla's: up to ~120)
		static constexpr float POOL_LINK_LEVEL = 16.0f;               ///< World units two pools' surfaces may differ by and be the same pool
		static constexpr float POOL_LINK_REACH = 1500.0f;             ///< World units across the pool a fall's landing may lie from where another pours out
		static constexpr float POOL_CARRY_RUN = 800.0f;               ///< World units across a pool over which carried white water fades by e
		static constexpr float WOBBLE_IMPACT = 36.0f;                 ///< D191: world units more where a plunge's water lands
		static constexpr float WOBBLE_EDGE_REACH = 50.0f;             ///< D88: wobble fade-in from open edges and rock, world units
		static constexpr float WOBBLE_SLOPE = 0.5f;                   ///< D99: the wobble's size climbs at most this many units per unit along the mesh...
		static constexpr float PUSH_SLOPE = 0.5f;                     ///< ...and the swell's push likewise, so no spike over one triangle
		static constexpr float DENSE_FINE_EDGE = 12.0f;               ///< D186: world units under which an edge is fine (near rock)...
		static constexpr float PUSH_SLOPE_FINE = 0.35f;               ///< ...where the swell's push may climb this steeply (piling against rock)
		static constexpr float WOBBLE_SLOPE_FINE = 0.35f;             ///< ...and the wobble's size
		static constexpr float ROCK_REACH = 60.0f;                    ///< D186: world units over which distance from rock is measured
		static constexpr float ROCK_CONTACT = 6.0f;                   ///< World units off rock where neither swell nor wobble moves the sheet...
		static constexpr float ROCK_RAMP = 30.0f;                     ///< ...rising to whole over this far
		static constexpr int ROCK_FIELD_PASSES = 24;                  ///< Passes softening push and wobble size (many: deform over an area)
		static constexpr int WOBBLE_COORD_PASSES = 12;                ///< Passes softening the wobble's coordinates
		/** @brief D82: the vertex shifts the shaders read (vertexClips 4-6): gap shift plus push. */
		void ComposeShifts();
		std::unordered_map<const RE::BSGeometry*, std::vector<RE::NiPoint3>> shiftedNormals;  ///< D186: each mesh's normals after its pushes (model space)
		std::atomic<uint64_t> shiftedNormalsVersion = 0;
		static constexpr int SHIFTED_NORMAL_PASSES = 4;  ///< Passes softening them

	public:
		/** @brief D186: a mesh's normals after its moves and swell (model space), or null if none or a worker owns them. */
		const std::vector<RE::NiPoint3>* GetShiftedNormals(const RE::BSGeometry* a_geometry) const
		{
			if (Busy())
				return nullptr;
			const auto it = shiftedNormals.find(a_geometry);
			return it != shiftedNormals.end() ? &it->second : nullptr;
		}
		uint64_t GetShiftedNormalsVersion() const { return shiftedNormalsVersion; }

	private:
		std::unordered_map<const RE::BSGeometry*, std::vector<RE::NiPoint3>> gapShifts, vertexPushes;

		std::unordered_map<const RE::BSGeometry*, std::shared_ptr<BakeField>> bakeFields;
		/** @brief D67: links bakes into cascades (a mesh whose top lies on another's lower part carries on from it), then builds them. */
		void LinkBakes();
		static constexpr float LINK_REACH = 48.0f;  ///< D67: world units a mesh's top edge may lie from the mesh feeding it
		static constexpr int32_t LINK_ROWS = 10;    ///< D67: texel rows up from a mesh's bottom edge that may feed the next
		std::unordered_map<const RE::BSGeometry*, FlowBake> flowBakes;
		static constexpr float PLUNGE_LEVEL = 0.9f;       ///< D190: a landing only where the normal's upright part is under this (falling in, not lying on the pool)
		static constexpr float PLUNGE_LEVEL_EASE = 0.3f;  ///< ...eased in over this much of it
		static constexpr float PLUNGE_CARRY = 0.9f;       ///< D188: the landing foam carried on per texel up the sheet from the waterline
		static constexpr int PLUNGE_WIDEN = 2;            ///< D189: texels the waterline's impact is widened each way before softening
		uint64_t flowBakeVersion = 0;
		uint64_t flowBakeStamp = 0;  ///< Last FlowBake::stamp given
		/** @brief D67: links the bakes on a worker once they are in. */
		void BakeStep();
		bool linkDue = false;  ///< A bake landed since the last link

		// D78: plunge points, where each mesh enters the water below
		static constexpr float PLUNGE_BAND = 14.0f;           ///< A texel this near above the water below, or under it, is in it
		static constexpr float PLUNGE_HEAD = 40.0f;           ///< Water that has fallen at least this far down its mesh plunges
		static constexpr float PLUNGE_STEEP = 0.8f;           ///< D46: water this steep (upward normal) at the water's level plunges whatever its head
		static constexpr float PLUNGE_WATER_SPACING = 24.0f;  ///< World units between the plunge points (their pushes on the simulated water)
		static constexpr float POOL_COVER = 2.0f;             ///< D36: water under a pool's surface by more than this is the pool's
		static constexpr float POOL_BAND = 24.0f;             ///< D40: a pool's water is where the falling water lies at its level, within this...
		static constexpr float TOP_POOL_CUT = 24.0f;          ///< D50: how much deeper the falls are cut under the pool they come from
		static constexpr float POOL_SHALLOW = 160.0f;         ///< ...or where the rock lies at most this far under it (a river or pool bed, not the face of a fall)
		static constexpr int32_t POOL_LIP_REACH = 3;          ///< D53: cells round a pool's cell searched for a fall dropping below the level...
		static constexpr float POOL_LIP_DROP = 48.0f;         ///< ...this far, with none from above: the pool's lip or a curtain under it, not its water
		static constexpr float PLUNGE_FOAM_RADIUS = 64.0f;    ///< D44: world units round each plunge point that foam
		static constexpr float PLUNGE_PUSH_OFFSET = 32.0f;    ///< D56: world units downstream of the falls' line the plunge pushes the water
		static constexpr float PLUNGE_MARK = 0.001f;          ///< D56: radial speed marking a fall's emitter without spreading the water
		static constexpr float PLUNGE_PULL = 0.2f;            ///< Fraction of the falling water's speed the water below is pulled along at
		static constexpr float LIP_BAND = 40.0f;              ///< D90: a crest this near its pool's surface is a lip
		static constexpr float CROSSING_STRENGTH = 0.8f;      ///< D94: how hard the water foams where a mesh crosses a water surface...
		static constexpr float CROSSING_BACK = 32.0f;         ///< ...set back this far onto the water's side of the seam
		std::vector<FallPlunge> plungePoints;
		const WaterSurfaces* waterSurfaces = nullptr;  ///< D45: set each update, for the pools' extent
		WaterSurfaces waterCopy;                       ///< D172: the engine's water as last seen with no worker running
		uint64_t waterCopySignature = UINT64_MAX;
		/** @brief D78: a site's pools on its capture's lattice: per cell the pool's surface, NO_SURFACE where there is none. */
		struct PoolField
		{
			float originX = 0.0f;
			float originY = 0.0f;
			uint32_t width = 0;
			uint32_t height = 0;
			float top = 0.0f;  ///< D50: the site's top level; the falls' source pool is cut deeper
			std::vector<float> cover;
		};
		std::unordered_map<uint64_t, PoolField> sitePools;                        ///< By Site::captureKey
		std::unique_ptr<std::vector<std::pair<uint64_t, PoolField>>> poolResult;  ///< D172: the pools' worker's result, taken in by FinishHeavyWork
		/** @brief D53: a site's falling-water meshes seen from above on a lattice (NO_SURFACE where none), with lowest and highest. */
		void RasterSurface(const Site& a_site, float a_originX, float a_originY, float a_spacing, int32_t a_width, int32_t a_height, std::vector<float>& a_surface,
			std::vector<float>* a_lowest = nullptr, std::vector<float>* a_highest = nullptr) const;
		/**
		 * @brief D45: where a site's pools have water on its capture's lattice: the engine's water, kept over rock where the falls
		 * lie at its level or the bed is shallow, flooded through the basin, without lips or the air under a curtain. Worker-safe.
		 */
		PoolField ComputePools(const Site& a_site, const FallCapture& a_capture) const;
		/** @brief D78: works out the pools of newly captured sites. */
		void PollPools();
		static constexpr uint32_t SHOWN_LAYERS = 0x1;                                            ///< Bit per Layer shown on takeover (the inner surface)
		std::unordered_map<const RE::NiAVObject*, RE::NiPointer<RE::NiAVObject>> hiddenEffects;  ///< Culled effect 3D: whole effect references and the falls models' particles
		void RefreshHiddenEffects();
		uint64_t groundVersion = 0;
	};
}
