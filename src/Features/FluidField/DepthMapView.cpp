#include "DepthMapView.h"

#include "BedRaycasts.h"
#include "Buffer.h"
#include "LandscapeCache.h"
#include "WaterSurfaces.h"

namespace FluidFieldSim
{
	namespace
	{
		/** @brief Height difference from the reference shown at the ends of the bed colour ramp. */
		constexpr float BED_RANGE = 2048.0f;
		/** @brief Water depth shown at the darkest blue. */
		constexpr float DEPTH_RANGE = 512.0f;

		uint32_t Pack(float r, float g, float b)
		{
			const auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
			return c(r) | (c(g) << 8) | (c(b) << 16) | 0xFF000000u;
		}

		constexpr uint32_t MISSING = 0xFFFF00FFu;  // Magenta: no landscape data
		constexpr uint32_t NO_WATER = 0xFF202020u;
	}

	DepthMapView::DepthMapView() = default;
	DepthMapView::~DepthMapView() = default;

	void DepthMapView::Release()
	{
		for (auto& texture : textures)
			texture.reset();
		hasImages = false;
		createFailed = false;
		callsUntilRefresh = 0;
		rebuilding = false;
		bed.clear();
		for (auto& image : pixels)
			image.clear();
	}

	ID3D11ShaderResourceView* DepthMapView::GetSRV(std::size_t a_index) const
	{
		return a_index < IMAGE_COUNT && textures[a_index] ? textures[a_index]->srv.get() : nullptr;
	}

	bool DepthMapView::EnsureTextures()
	{
		if (textures[0])
			return true;
		if (createFailed)
			return false;

		D3D11_TEXTURE2D_DESC desc{
			.Width = RESOLUTION,
			.Height = RESOLUTION,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{ .Format = desc.Format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D };
		srvDesc.Texture2D.MipLevels = 1;

		constexpr std::array<const char*, IMAGE_COUNT> names = { "FluidField::DepthMap Bed", "FluidField::DepthMap Water", "FluidField::DepthMap Depth", "FluidField::DepthMap Raycasts" };
		try {
			for (std::size_t i = 0; i < IMAGE_COUNT; i++) {
				textures[i] = std::make_unique<Texture2D>(desc, names[i]);
				textures[i]->CreateSRV(srvDesc);
			}
		} catch (const std::exception& e) {
			logger::error("[Fluid Field] Failed to create depth map debug textures: {}", e.what());
			Release();
			createFailed = true;
			return false;
		}
		return true;
	}

	void DepthMapView::FillBedRow(uint32_t a_row, const LandscapeCache& a_landscape, const BedRaycasts* a_rays)
	{
		constexpr uint32_t n = RESOLUTION;
		for (uint32_t x = 0; x < n + 2; x++) {
			const float wx = originX + (static_cast<float>(x) - 0.5f) * texel;
			const float wy = originY + (static_cast<float>(a_row) - 0.5f) * texel;
			float height = std::numeric_limits<float>::quiet_NaN();
			if (!a_landscape.SampleHeight(wx, wy, height))
				height = std::numeric_limits<float>::quiet_NaN();
			// A terrain hit is the engine's own triangle height; a static hit raises the bed to the mesh
			if (const auto* ray = a_rays ? a_rays->FindDetail(wx, wy) : nullptr) {
				if (ray->kind == BedRaycasts::HitKind::Terrain)
					height = ray->height;
				else if (ray->kind == BedRaycasts::HitKind::Static)
					height = std::isfinite(height) ? std::max(height, ray->height) : ray->height;
			}
			bed[a_row * (n + 2) + x] = height;
		}
	}

	void DepthMapView::Update(ID3D11DeviceContext* a_context, const LandscapeCache& a_landscape, const WaterSurfaces& a_water, const BedRaycasts* a_rays,
		float a_centreX, float a_centreY, float a_referenceHeight, float a_size)
	{
		constexpr uint32_t n = RESOLUTION;

		if (!rebuilding) {
			if (callsUntilRefresh > 0) {
				callsUntilRefresh--;
				return;
			}
			if (!EnsureTextures())
				return;

			rebuilding = true;
			nextRow = 0;
			bedRowsFilled = 0;
			texel = a_size / static_cast<float>(n);
			originX = a_centreX - a_size * 0.5f;
			originY = a_centreY - a_size * 0.5f;
			referenceHeight = a_referenceHeight;
			bed.assign((n + 2) * (n + 2), std::numeric_limits<float>::quiet_NaN());
			for (auto& image : pixels)
				image.assign(n * n, 0);
			pending = {};
		}

		const uint32_t endRow = std::min(n, nextRow + n / SLICES);
		// Output row y reads bed rows y .. y + 2 (the bed has a one-texel border)
		while (bedRowsFilled < std::min(n + 2, endRow + 2))
			FillBedRow(bedRowsFilled++, a_landscape, a_rays);

		for (uint32_t y = nextRow; y < endRow; y++) {
			for (uint32_t x = 0; x < n; x++) {
				// Row 0 is north, so the images read like a map
				const uint32_t row = n - 1 - y;
				const std::size_t out = static_cast<std::size_t>(row) * n + x;
				const float wx = originX + (static_cast<float>(x) + 0.5f) * texel;
				const float wy = originY + (static_cast<float>(y) + 0.5f) * texel;

				const auto bedAt = [&](int32_t dx, int32_t dy) { return bed[(y + 1 + dy) * (n + 2) + (x + 1 + dx)]; };
				const float h = bedAt(0, 0);
				const bool hasBed = std::isfinite(h);

				// Bed: green below the player, brown above, lit from the north-west
				if (hasBed) {
					const float t = std::clamp((h - referenceHeight) / BED_RANGE, -1.0f, 1.0f);
					const float slopeX = std::isfinite(bedAt(1, 0)) && std::isfinite(bedAt(-1, 0)) ? bedAt(1, 0) - bedAt(-1, 0) : 0.0f;
					const float slopeY = std::isfinite(bedAt(0, 1)) && std::isfinite(bedAt(0, -1)) ? bedAt(0, 1) - bedAt(0, -1) : 0.0f;
					const float shade = std::clamp(0.75f + (-slopeX + slopeY) / (4.0f * texel), 0.35f, 1.15f);
					const float r = t < 0.0f ? 0.25f + 0.15f * (1.0f + t) : 0.40f + 0.35f * t;
					const float g = t < 0.0f ? 0.45f + 0.15f * (1.0f + t) : 0.60f - 0.20f * t;
					const float b = t < 0.0f ? 0.20f : 0.20f + 0.10f * t;
					pixels[0][out] = Pack(r * shade, g * shade, b * shade);
				} else {
					pixels[0][out] = MISSING;
					pending.bedMissing++;
				}

				// Water surface: bright where the water is above the bed, dim where its plane lies under the ground
				float surface = 0.0f;
				const auto source = a_water.SampleHeight(wx, wy, surface);
				const bool wet = source != WaterSurfaces::Source::None && (!hasBed || surface > h);
				if (source == WaterSurfaces::Source::None) {
					pixels[1][out] = NO_WATER;
				} else if (!wet) {
					pixels[1][out] = source == WaterSurfaces::Source::WaterObject ? Pack(0.10f, 0.13f, 0.28f) : Pack(0.10f, 0.22f, 0.24f);
					pending.buried++;
				} else if (source == WaterSurfaces::Source::WaterObject) {
					pixels[1][out] = Pack(0.15f, 0.35f, 1.0f);
					pending.waterObject++;
				} else {
					pixels[1][out] = Pack(0.2f, 0.85f, 0.9f);
					pending.cellWater++;
				}

				// Depth: blue gets darker with depth; tan where the water's plane lies under the ground
				if (source == WaterSurfaces::Source::None) {
					pixels[2][out] = NO_WATER;
				} else if (!hasBed) {
					pixels[2][out] = MISSING;
				} else if (wet) {
					const float depth = surface - h;
					const float t = std::clamp(depth / DEPTH_RANGE, 0.0f, 1.0f);
					pixels[2][out] = Pack(0.55f - 0.5f * t, 0.85f - 0.65f * t, 1.0f - 0.45f * t);
					pending.submerged++;
					pending.maxDepth = std::max(pending.maxDepth, depth);
				} else {
					pixels[2][out] = Pack(0.55f, 0.45f, 0.30f);
				}

				// Raycasts: green terrain, purple looked under a deck, orange mesh above the water (obstacle),
				// brown mesh below it (riverbed), dark blue nothing in reach, grey dry (not cast)
				const auto* ray = a_rays ? a_rays->FindDetail(wx, wy) : nullptr;
				if (!ray) {
					pixels[3][out] = NO_WATER;
				} else if (ray->overhead) {
					pixels[3][out] = Pack(0.65f, 0.35f, 0.95f);
				} else {
					switch (ray->kind) {
					case BedRaycasts::HitKind::Terrain:
						pixels[3][out] = Pack(0.30f, 0.75f, 0.30f);
						break;
					case BedRaycasts::HitKind::Static:
						pixels[3][out] = source != WaterSurfaces::Source::None && ray->height <= surface ? Pack(0.55f, 0.38f, 0.15f) : Pack(1.0f, 0.55f, 0.10f);
						break;
					case BedRaycasts::HitKind::NoHit:
						pixels[3][out] = Pack(0.10f, 0.15f, 0.45f);
						break;
					default:
						pixels[3][out] = Pack(0.40f, 0.40f, 0.40f);
						break;
					}
				}
			}
		}
		nextRow = endRow;

		if (nextRow >= n) {
			for (std::size_t i = 0; i < IMAGE_COUNT; i++)
				a_context->UpdateSubresource(textures[i]->resource.get(), 0, nullptr, pixels[i].data(), n * sizeof(uint32_t), 0);
			stats = pending;
			hasImages = true;
			rebuilding = false;
			callsUntilRefresh = REFRESH_INTERVAL;
		}
	}
}
