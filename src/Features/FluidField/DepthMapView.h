#pragma once

#include <array>
#include <memory>
#include <vector>

class Texture2D;

namespace FluidFieldSim
{
	class BedRaycasts;
	class MeshCut;
	class LandscapeCache;
	class WaterSurfaces;

	/**
	 * @brief Debug images of the depth map (R1, D7 stage 3): bed, water surface, depth and raycast results.
	 *
	 * Built on the CPU from the landscape cache, water surfaces and bed raycasts over a square
	 * centred on the player, north up, and uploaded to small textures for the layer overlay. Each
	 * rebuild is spread over several frames so no frame pays for all of it. Debug only: it exists
	 * while the overlay is shown, and is released otherwise.
	 */
	class DepthMapView
	{
	public:
		static constexpr uint32_t RESOLUTION = 128;
		static constexpr std::size_t IMAGE_COUNT = 4;

		/** @brief Statistics of the last refresh, shown under the images. */
		struct Stats
		{
			uint32_t bedMissing = 0;   ///< Texels with neither landscape nor a raycast bed
			uint32_t waterObject = 0;  ///< Wet texels under a water object
			uint32_t cellWater = 0;    ///< Wet texels under cell water only
			uint32_t buried = 0;       ///< Texels where a water plane lies under the bed (not wet)
			uint32_t submerged = 0;    ///< Texels where the water surface is above the bed
			float maxDepth = 0.0f;
		};

		DepthMapView();
		~DepthMapView();

		/**
		 * @brief Advances the current rebuild by one slice, or starts a new one every few calls.
		 * @param a_centreX, a_centreY World position at the centre of the images.
		 * @param a_referenceHeight Height shown as mid-grey in the bed image (the player's feet).
		 * @param a_size World units per image side.
		 * @param a_rays Raycast bed results, or null when raycasting is off.
		 */
		void Update(ID3D11DeviceContext* a_context, const LandscapeCache& a_landscape, const WaterSurfaces& a_water, const BedRaycasts* a_rays, const MeshCut* a_meshCut,
			float a_centreX, float a_centreY, float a_referenceHeight, float a_size);

		void Release();
		bool HasImages() const { return hasImages; }
		ID3D11ShaderResourceView* GetSRV(std::size_t a_index) const;
		const Stats& GetStats() const { return stats; }

	private:
		static constexpr uint32_t REFRESH_INTERVAL = 10;  ///< Calls between the end of one rebuild and the start of the next
		static constexpr uint32_t SLICES = 4;             ///< Calls one rebuild is spread over

		bool EnsureTextures();
		/** @brief Fills one row of the bed buffer (which has a one-texel border). */
		void FillBedRow(uint32_t a_row, const LandscapeCache& a_landscape, const WaterSurfaces& a_water, const BedRaycasts* a_rays, const MeshCut* a_meshCut);

		std::array<std::unique_ptr<Texture2D>, IMAGE_COUNT> textures;
		bool createFailed = false;
		bool hasImages = false;
		uint32_t callsUntilRefresh = 0;
		Stats stats;

		// The rebuild in progress; its placement is fixed when it starts
		bool rebuilding = false;
		uint32_t nextRow = 0;
		uint32_t bedRowsFilled = 0;
		float originX = 0.0f;
		float originY = 0.0f;
		float texel = 0.0f;
		float referenceHeight = 0.0f;
		std::vector<float> bed;
		std::array<std::vector<uint32_t>, IMAGE_COUNT> pixels;
		Stats pending;
	};
}
