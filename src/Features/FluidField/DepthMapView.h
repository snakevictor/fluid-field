#pragma once

#include <array>
#include <memory>

class Texture2D;

namespace FluidFieldSim
{
	class LandscapeCache;
	class WaterSurfaces;

	/**
	 * @brief Debug images of the depth map (R1, D7 stage 3): landscape bed, water surface and depth.
	 *
	 * Built on the CPU from the landscape cache and water surfaces over a square centred on the
	 * player, north up, and uploaded to small textures for the layer overlay. Debug only: it exists
	 * while the overlay is shown, and is released otherwise.
	 */
	class DepthMapView
	{
	public:
		static constexpr uint32_t RESOLUTION = 128;
		static constexpr std::size_t IMAGE_COUNT = 3;

		/** @brief Statistics of the last refresh, shown under the images. */
		struct Stats
		{
			uint32_t bedMissing = 0;   ///< Texels outside the cached landscape
			uint32_t waterObject = 0;  ///< Texels covered by a water object
			uint32_t cellWater = 0;    ///< Texels covered only by cell water
			uint32_t submerged = 0;    ///< Texels where the water surface is above the bed
			float maxDepth = 0.0f;
		};

		DepthMapView();
		~DepthMapView();

		/**
		 * @brief Refreshes the images every few calls.
		 * @param a_centreX, a_centreY World position at the centre of the images.
		 * @param a_referenceHeight Height shown as mid-grey in the bed image (the player's feet).
		 * @param a_size World units per image side.
		 */
		void Update(ID3D11DeviceContext* a_context, const LandscapeCache& a_landscape, const WaterSurfaces& a_water,
			float a_centreX, float a_centreY, float a_referenceHeight, float a_size);

		void Release();
		bool HasImages() const { return hasImages; }
		ID3D11ShaderResourceView* GetSRV(std::size_t a_index) const;
		const Stats& GetStats() const { return stats; }

	private:
		static constexpr uint32_t REFRESH_INTERVAL = 10;  ///< Calls between refreshes

		bool EnsureTextures();

		std::array<std::unique_ptr<Texture2D>, IMAGE_COUNT> textures;
		bool createFailed = false;
		bool hasImages = false;
		uint32_t callsUntilRefresh = 0;
		Stats stats;
	};
}
