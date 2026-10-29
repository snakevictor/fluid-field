#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace FluidFieldSim
{
	/**
	 * @brief Where vanilla water is around the player, and at what height: the depth map's surface layer (D7).
	 *
	 * From the engine's water objects (`TESWaterSystem::waterObjects`), falling back to each loaded cell's water height.
	 * Sloped bounds (rapids, waterfalls) are counted but skipped.
	 */
	class WaterSurfaces
	{
	public:
		/** @brief Footprint of one flat water object's bound, at its surface height. */
		struct Surface
		{
			float minX = 0.0f;
			float minY = 0.0f;
			float maxX = 0.0f;
			float maxY = 0.0f;
			float height = 0.0f;       ///< Bound centre height, as Skin's water lookup uses
			float planeHeight = 0.0f;  ///< Height implied by the water object's plane, for the check
			uint32_t waterFormID = 0;  ///< The water object's water type, for the check's log
			float flowScale = 0.0f;    ///< 1 when the water type uses the flowmap (rivers); 0 for lakes and the sea
		};

		enum class Source : uint8_t
		{
			None,
			WaterObject,
			CellWater
		};

		/** @brief Rebuilds the surface list from the engine's water objects every few updates. Main thread only. */
		void Update();

		/** @brief Drops every surface; the next update rebuilds immediately. */
		void Clear();

		/**
		 * @brief Water surface height at a world position.
		 * @return Where the height came from; `Source::None` when there is no water there.
		 */
		Source SampleHeight(float a_worldX, float a_worldY, float& a_height) const;
		/**
		 * @brief As SampleHeight, plus flow scale: 1 for flowmap water types (rivers), 0 for lakes, sea and cell water
		 * (vanilla renders those still even over world flowmap data).
		 */
		Source SampleHeight(float a_worldX, float a_worldY, float& a_height, float& a_flowScale) const;

		/** @brief A water plane overlapping a query box: its footprint and height. */
		struct Plane
		{
			float minX, minY, maxX, maxY;
			float height;
			Source source;
		};

		/** @brief Every water plane overlapping a world box: water objects and touched cells' water (an interior's covers all). */
		void CollectPlanes(float a_minX, float a_minY, float a_maxX, float a_maxY, std::vector<Plane>& a_planes) const;

		std::size_t GetSurfaceCount() const { return surfaces.size(); }
		uint32_t GetSlopedCount() const { return slopedCount; }
		std::size_t GetCellWaterCount() const { return cellWater.size(); }
		/** @brief Changes whenever the set of surfaces changes. */
		uint64_t GetSignature() const { return lastSignature; }

	private:
		static constexpr uint32_t REBUILD_INTERVAL = 30;  ///< Updates between rebuilds

		void Rebuild();

		std::vector<Surface> surfaces;
		uint32_t slopedCount = 0;
		uint32_t slopedFormID = 0;  ///< Water type of the last sloped bound skipped, for the check's log
		/** @brief Water height of each loaded exterior cell with water, keyed by packed cell coordinates. */
		std::unordered_map<uint64_t, float> cellWater;
		/** @brief Water height of the current interior, when it has water. */
		float interiorWater = 0.0f;
		bool hasInteriorWater = false;

		uint32_t updatesUntilRebuild = 0;
		uint64_t lastSignature = 0;
	};
}
