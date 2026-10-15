#pragma once

#include "FallCapture.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <vector>

namespace FluidFieldSim
{
	/**
	 * @brief Draws the waterfall sheets (D25, S7) into the scene after the world.
	 *
	 * Every traced stream is a ribbon of FallCapture::STREAM_ROWS points in a structured buffer,
	 * expanded into triangles in the vertex shader, one instance per stream; neighbouring ribbons
	 * overlap into a sheet and part where the streams do. The scene behind is copied first, so the
	 * clear water refracts it. Depth tested against the scene without writing, premultiplied-alpha
	 * blended. Every piece of pipeline state it touches is saved first and restored after.
	 */
	class FallSheetRenderer
	{
	public:
		/** @brief Uploads the streams when their version changed. */
		void Upload(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const std::vector<FallCapture::StreamPoint>& a_points, uint64_t a_version);

		/**
		 * @brief Draws the sheets.
		 * @param a_targetTexture The texture behind a_target, copied for refraction.
		 * @param a_camera The main camera's per-frame constants (b12), snapshotted during the deferred passes.
		 */
		void Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target, ID3D11Texture2D* a_targetTexture, ID3D11DepthStencilView* a_depth,
			ID3D11Buffer* a_camera, ID3D11Buffer* a_shared, ID3D11Buffer* a_features, float a_time, float a_opacity);

		void Release();
		void ReleaseShaders();
		bool HasSheets() const { return streamCount > 0; }

	private:
		bool EnsureShaders(ID3D11Device* a_device);
		bool EnsureStates(ID3D11Device* a_device);
		bool EnsureSceneCopy(ID3D11Device* a_device, ID3D11Texture2D* a_target);

		struct alignas(16) Constants
		{
			uint32_t rows;
			float time;
			float opacity;
			float pad;
			float inverseSize[2];  ///< Of the target, for screen coordinates
			float pad2[2];
		};

		winrt::com_ptr<ID3D11Buffer> points;
		winrt::com_ptr<ID3D11ShaderResourceView> pointsSRV;
		uint32_t capacity = 0;  ///< Points the buffer holds
		uint32_t streamCount = 0;
		uint64_t uploadedVersion = UINT64_MAX;

		winrt::com_ptr<ID3D11Texture2D> sceneCopy;
		winrt::com_ptr<ID3D11ShaderResourceView> sceneCopySRV;

		winrt::com_ptr<ID3D11Buffer> constants;
		winrt::com_ptr<ID3D11VertexShader> vertexShader;
		winrt::com_ptr<ID3D11PixelShader> pixelShader;
		winrt::com_ptr<ID3D11BlendState> blend;
		winrt::com_ptr<ID3D11DepthStencilState> depthState;
		winrt::com_ptr<ID3D11RasterizerState> raster;
		winrt::com_ptr<ID3D11SamplerState> sampler;
		bool shaderFailed = false;
	};
}
