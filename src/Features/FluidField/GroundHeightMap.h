#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <vector>

namespace FluidFieldSim
{
	class LandscapeCache;
	class WaterSurfaces;

	/**
	 * @brief Ground height under the wind grid, for the ground fog (D21): the landscape, or the water
	 * surface where there is water over it, so fog lies on rivers and lakes as mist.
	 *
	 * A float texture on the wind grid, stored toroidally like the field: world cell c lives in
	 * texel c & (res - 1). As the grid scrolls only the cells that came into it are sampled from
	 * the landscape cache; cells over land that is not loaded yet are retried every few updates.
	 * Unknown ground is NO_GROUND, far below anything, so no fog forms there. Runs on the main
	 * thread; the texture is updated on the render thread's context.
	 */
	class GroundHeightMap
	{
	public:
		/** @brief Height written where the landscape is not known (no fog forms there). */
		static constexpr float NO_GROUND = -1.0e7f;

		/**
		 * @brief Brings the map up to date with the grid's placement and uploads what changed.
		 * @return False if the texture could not be created.
		 */
		bool Update(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const LandscapeCache& a_landscape, const WaterSurfaces& a_water, const int32_t a_gridMin[2],
			uint32_t a_resolution, float a_cellSize);

		void Release();

		ID3D11ShaderResourceView* GetSRV() const { return srv.get(); }
		/** @brief Highest known ground in the map, so the fog pass can skip rays that stay above the fog. */
		float GetMaxHeight() const { return maxHeight; }

	private:
		/** @brief Updates between retries of cells over unloaded land. */
		static constexpr uint32_t RETRY_INTERVAL = 60;

		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		std::vector<float> heights;  ///< Toroidal, as the texture
		uint32_t resolution = 0;
		float cellSize = 0.0f;
		int32_t filledMin[2]{};  ///< Grid placement the map was last filled for
		bool filled = false;
		uint32_t updatesUntilRetry = 0;
		float maxHeight = NO_GROUND;
	};
}
