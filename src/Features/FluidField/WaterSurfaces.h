#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace FluidFieldSim
{
	/**
	 * @brief Where vanilla water is around the player, and at what height.
	 *
	 * Built from the engine's water objects (`TESWaterSystem::waterObjects`: placed water and cell
	 * water planes near the player), with each loaded cell's water height as the fallback. This is
	 * the surface layer of the depth map (R1, D7 stage 2): together with the landscape bed it gives
	 * water depth. Sloped water (rapids, waterfalls) is counted but not used yet; blending it is R3's.
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
		};

		enum class Source : uint8_t
		{
			None,
			WaterObject,
			CellWater
		};

		/** @brief Result of comparing surface heights with the engine's own water query. */
		struct VerifyStats
		{
			uint32_t runs = 0;
			uint32_t samplesChecked = 0;
			uint32_t samplesMissed = 0;       ///< Engine query found no water where a surface claims some
			float maxError = 0.0f;            ///< Surface height vs the engine
			float maxPlaneDifference = 0.0f;  ///< Bound centre vs plane-derived height
		};

		/**
		 * @brief Rebuilds the surface list from the engine's water objects every few updates.
		 * @param a_verify After each rebuild that changed the list, compare with the engine and log.
		 * Call on the main thread; it reads the engine's water system and cell grid.
		 */
		void Update(bool a_verify);

		/** @brief Drops every surface; the next update rebuilds immediately. */
		void Clear();

		/**
		 * @brief Water surface height at a world position.
		 * @return Where the height came from; `Source::None` when there is no water there.
		 */
		Source SampleHeight(float a_worldX, float a_worldY, float& a_height) const;

		std::size_t GetSurfaceCount() const { return surfaces.size(); }
		uint32_t GetSlopedCount() const { return slopedCount; }
		std::size_t GetCellWaterCount() const { return cellWater.size(); }
		const VerifyStats& GetVerifyStats() const { return verifyStats; }

	private:
		static constexpr uint32_t REBUILD_INTERVAL = 30;  ///< Updates between rebuilds

		void Rebuild(bool a_verify);
		void Verify();

		std::vector<Surface> surfaces;
		uint32_t slopedCount = 0;
		/** @brief Water height of each loaded exterior cell with water, keyed by packed cell coordinates. */
		std::unordered_map<uint64_t, float> cellWater;
		/** @brief Water height of the current interior, when it has water. */
		float interiorWater = 0.0f;
		bool hasInteriorWater = false;

		uint32_t updatesUntilRebuild = 0;
		uint64_t lastSignature = 0;
		VerifyStats verifyStats;
	};
}
