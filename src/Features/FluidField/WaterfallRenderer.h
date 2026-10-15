#pragma once

#include "WaterfallSources.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>

namespace FluidFieldSim
{
	/**
	 * @brief Draws the waterfall curtains (D23) into the scene after the world.
	 *
	 * One instanced draw: every curtain is a CURTAIN_COLUMNS x CURTAIN_ROWS grid of points in a
	 * structured buffer, expanded into triangles in the vertex shader. Depth tested against the
	 * scene without writing, premultiplied-alpha blended. Every piece of pipeline state it touches
	 * is saved first and restored after, so the engine's own state tracking stays right.
	 */
	class WaterfallRenderer
	{
	public:
		/** @brief Uploads the curtains when their version changed. */
		void Upload(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const WaterfallSources& a_sources);

		/**
		 * @brief Draws the curtains.
		 * @param a_camera The main camera's per-frame constants (b12), snapshotted during the deferred passes.
		 */
		void Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target, ID3D11DepthStencilView* a_depth, ID3D11Buffer* a_camera,
			ID3D11Buffer* a_shared, ID3D11Buffer* a_features, float a_time, float a_opacity);

		void Release();
		void ReleaseShaders();
		bool HasCurtains() const { return curtainCount > 0; }

	private:
		bool EnsureShaders(ID3D11Device* a_device);
		bool EnsureStates(ID3D11Device* a_device);

		struct alignas(16) Constants
		{
			uint32_t columns;
			uint32_t rows;
			float time;
			float opacity;
		};

		winrt::com_ptr<ID3D11Buffer> points;
		winrt::com_ptr<ID3D11ShaderResourceView> pointsSRV;
		uint32_t capacity = 0;  ///< Points the buffer holds
		uint32_t curtainCount = 0;
		uint64_t uploadedVersion = UINT64_MAX;

		winrt::com_ptr<ID3D11Buffer> constants;
		winrt::com_ptr<ID3D11VertexShader> vertexShader;
		winrt::com_ptr<ID3D11PixelShader> pixelShader;
		winrt::com_ptr<ID3D11BlendState> blend;
		winrt::com_ptr<ID3D11DepthStencilState> depthState;
		winrt::com_ptr<ID3D11RasterizerState> raster;
		bool shaderFailed = false;
	};
}
