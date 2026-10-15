#pragma once

#include "FallCapture.h"

#include <cstdint>
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
		 * @brief The rock in falling water's way, from a ground map (BuildGroundMap's): per texel the push
		 * away from rock that stands out of its bed (world units, x and y), how much rock is there (0 to
		 * 1, smoothed), and how far it stands out (0 to 1). The bed is the ground blurred over
		 * BED_RADIUS; rock more than PROMINENCE_FLOOR above it stands out, fully at PROMINENCE_FULL.
		 */
		static void BuildRockField(const std::vector<float>& a_heights, uint32_t a_size, float a_cell, std::vector<float>& a_field);
		static constexpr float BED_RADIUS = 128.0f;
		static constexpr float PROMINENCE_FLOOR = 8.0f;
		static constexpr float PROMINENCE_FULL = 48.0f;
		static constexpr float OBSTACLE_RADIUS = 96.0f;  ///< How far round rock its presence is smoothed (twice): it reaches about twice this
		static constexpr float PUSH = 1536.0f;           ///< Push per unit of presence change per texel: PUSH_MOST at the rock's edge
		static constexpr float PUSH_MOST = 128.0f;       ///< World units

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
		uint64_t groundVersion = 0;
	};
}
