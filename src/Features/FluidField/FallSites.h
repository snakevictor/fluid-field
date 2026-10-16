#pragma once

#include "FallCapture.h"
#include "FallStream.h"
#include "FallSurfaceRenderer.h"

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
		/** @brief Changes whenever the captured ground or the grounded meshes change. */
		uint64_t GetGroundVersion() const { return groundVersion; }
		/**
		 * @brief The captured ground of every site, sampled every a_cell units over a_size x a_size texels
		 * from a_origin (world); FallCapture::NONE where none is captured.
		 */
		void BuildGroundMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_heights) const;

		/**
		 * @brief The steady flow of every site whose flow is solved (D28), on a ground map's lattice
		 * (a_cell must be FallCapture::CELL): per texel the stream function (world units across the flow),
		 * the speed (1 for free flow), foam (0 to 1), and 1 where there is water; zeros elsewhere.
		 */
		void BuildStreamMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_field) const;

		/**
		 * @brief D30: the falls' own water surfaces, one mesh per site whose flow is solved, all in one
		 * list; changes with GetSurfaceVersion.
		 */
		const std::vector<FallSurfaceVertex>& GetSurfaceVertices() const { return surfaceVertices; }
		const std::vector<uint32_t>& GetSurfaceIndices() const { return surfaceIndices; }
		uint64_t GetSurfaceVersion() const { return surfaceVersion; }
		/** @brief Vanilla's falling-water textures (from the lit surface of its models), for the fall surfaces; null until one is seen. */
		ID3D11ShaderResourceView* GetSurfaceDiffuse() const;
		ID3D11ShaderResourceView* GetSurfaceNormal() const;
		/** @brief Whether the fall surfaces replace vanilla's falling-water meshes: those of sites with a surface are hidden. */
		void SetReplaceSurface(bool a_replace);

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
		static constexpr float STREAM_MIN_DEPTH = 4.0f;      ///< Water must stand this far above the rock to flow there
		static constexpr float STREAM_INFLOW_BAND = 64.0f;   ///< Water within this of the site's top comes in there
		static constexpr float STREAM_OUTFLOW_BAND = 32.0f;  ///< Water within this of the site's bottom leaves there
		static constexpr float NO_SURFACE = -1.0e7f;

		/** @brief Counts for the menu: sites by kind. */
		uint32_t CountKind(Kind a_kind) const;

		/**
		 * @brief Draws the stages' debug view in the world: classified references as boxes by class,
		 * site footprints at their levels, and a label per site. Within a_range of the camera.
		 */
		void DrawDebug(ImDrawList* a_drawList, float a_range) const;

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
			uint32_t culled = 0;  ///< Source triangles dropped where another piece lies on them
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
			std::vector<SourceTriangle> sources;
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
		static constexpr float FALL_SPEED = 600.0f;       ///< World units per second of drop counted into the travel time, so steep water streaks too
		static constexpr float MEET_FOAM_HEIGHT = 20.0f;  ///< Water within this of a site's level (where it meets a body of water) foams
		std::vector<std::future<SurfaceMesh>> retiredMeshes;
		/** @brief Gathers every site's surface into one list. */
		void RefreshSurfaces();
		std::vector<FallSurfaceVertex> surfaceVertices;
		std::vector<uint32_t> surfaceIndices;
		uint64_t surfaceVersion = 0;
		bool replaceSurface = false;
		RE::NiPointer<RE::NiSourceTexture> surfaceDiffuse;
		RE::NiPointer<RE::NiSourceTexture> surfaceNormal;
		std::unordered_map<uint64_t, Stream> streams;          ///< By Site::captureKey
		std::vector<std::future<FallStream::Result>> retired;  ///< Solves of sites that went away, left to finish
		/** @brief Vanilla's falling-water meshes of a site seen from above, and its pools, on a lattice; NO_SURFACE where none. */
		void RasterSurface(const Site& a_site, float a_originX, float a_originY, float a_spacing, int32_t a_width, int32_t a_height, std::vector<float>& a_surface) const;
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
