#include "GroundHeightMap.h"

#include "LandscapeCache.h"
#include "WaterSurfaces.h"

#include <algorithm>

namespace FluidFieldSim
{
	void GroundHeightMap::Release()
	{
		texture = nullptr;
		srv = nullptr;
		heights.clear();
		resolution = 0;
		filled = false;
		maxHeight = NO_GROUND;
	}

	bool GroundHeightMap::Update(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const LandscapeCache& a_landscape, const WaterSurfaces& a_water, const int32_t a_gridMin[2],
		uint32_t a_resolution, float a_cellSize)
	{
		if (a_resolution != resolution || a_cellSize != cellSize || !texture) {
			Release();
			D3D11_TEXTURE2D_DESC desc{
				.Width = a_resolution,
				.Height = a_resolution,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = DXGI_FORMAT_R32_FLOAT,
				.SampleDesc = { 1, 0 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_SHADER_RESOURCE,
				.CPUAccessFlags = 0,
				.MiscFlags = 0
			};
			if (FAILED(a_device->CreateTexture2D(&desc, nullptr, texture.put())) || FAILED(a_device->CreateShaderResourceView(texture.get(), nullptr, srv.put()))) {
				Release();
				return false;
			}
			Util::SetResourceName(texture.get(), "FluidField::GroundHeight");
			Util::SetResourceName(srv.get(), "FluidField::GroundHeight SRV");
			resolution = a_resolution;
			cellSize = a_cellSize;
			heights.assign(static_cast<std::size_t>(resolution) * resolution, NO_GROUND);
		}

		const int32_t n = static_cast<int32_t>(resolution);
		const int32_t mask = n - 1;
		const bool retry = updatesUntilRetry == 0;
		updatesUntilRetry = retry ? RETRY_INTERVAL : updatesUntilRetry - 1;
		bool changed = false;

		// Sample cells new since the last fill (all, the first time) and, on a retry, those still over unloaded land
		for (int32_t y = 0; y < n; y++) {
			const int32_t cellY = a_gridMin[1] + y;
			const bool rowWasInGrid = filled && cellY >= filledMin[1] && cellY < filledMin[1] + n;
			for (int32_t x = 0; x < n; x++) {
				const int32_t cellX = a_gridMin[0] + x;
				// Rows already in the grid: skip straight past the columns that were too, unless retrying
				if (rowWasInGrid && !retry && cellX >= filledMin[0] && cellX < filledMin[0] + n) {
					x = filledMin[0] + n - a_gridMin[0] - 1;
					continue;
				}
				const bool wasInGrid = rowWasInGrid && cellX >= filledMin[0] && cellX < filledMin[0] + n;
				float& height = heights[static_cast<std::size_t>(cellY & mask) * n + (cellX & mask)];
				if (wasInGrid && !(retry && height == NO_GROUND))
					continue;
				float ground = 0.0f;
				const float worldX = (static_cast<float>(cellX) + 0.5f) * cellSize;
				const float worldY = (static_cast<float>(cellY) + 0.5f) * cellSize;
				float value = a_landscape.SampleHeight(worldX, worldY, ground) ? ground : NO_GROUND;
				// Fog lies on water where there is water over the ground
				float surface = 0.0f;
				if (value != NO_GROUND && a_water.SampleHeight(worldX, worldY, surface) != WaterSurfaces::Source::None && surface > value)
					value = surface;
				if (value != height) {
					height = value;
					changed = true;
				}
			}
		}
		filledMin[0] = a_gridMin[0];
		filledMin[1] = a_gridMin[1];
		filled = true;

		if (changed) {
			maxHeight = *std::max_element(heights.begin(), heights.end());
			a_context->UpdateSubresource(texture.get(), 0, nullptr, heights.data(), static_cast<UINT>(resolution * sizeof(float)), 0);
		}
		return true;
	}
}
