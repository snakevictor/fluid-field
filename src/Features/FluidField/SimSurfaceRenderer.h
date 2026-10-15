#pragma once

#include "FallBed.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <span>

namespace FluidFieldSim
{
	/**
	 * @brief Draws the simulated water where vanilla has no water plane (D24), such as water pouring
	 * down a waterfall's rock: a heightfield mesh per fall area, after the world.
	 *
	 * The scene is copied first so the water can refract what is behind it. Every piece of pipeline
	 * state it touches is saved first and restored after, so the engine's own tracking stays right.
	 */
	class SimSurfaceRenderer
	{
	public:
		/** @brief The water level to draw: its placement and its latest state. */
		struct Level
		{
			int32_t gridMin[2];
			uint32_t resolution;
			float cellSize;
			ID3D11ShaderResourceView* depth;
			ID3D11ShaderResourceView* cellMap;
			ID3D11ShaderResourceView* flow;
			ID3D11ShaderResourceView* fallBed;
		};

		/**
		 * @brief Draws the water over each area.
		 * @param a_targetTexture The colour target, copied for refraction.
		 * @param a_camera The main camera's per-frame constants (b12), snapshotted during the deferred passes.
		 */
		void Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11Texture2D* a_targetTexture, ID3D11RenderTargetView* a_target, ID3D11DepthStencilView* a_depth,
			ID3D11Buffer* a_camera, ID3D11Buffer* a_shared, ID3D11Buffer* a_features, const Level& a_level, std::span<const FallBed::Area> a_areas, float a_time, float a_brightness);

		void Release();
		void ReleaseShaders();

	private:
		bool EnsureShaders();
		bool EnsureStates(ID3D11Device* a_device);
		bool EnsureSceneCopy(ID3D11Device* a_device, ID3D11Texture2D* a_target);

		struct alignas(16) Constants
		{
			int32_t cellMin[2];
			int32_t cellCount[2];
			uint32_t gridRes;
			float cellSize;
			float time;
			float brightness;
			float screenRcp[2];
			float pad[2];
		};
		static_assert(sizeof(Constants) == 48);

		winrt::com_ptr<ID3D11Buffer> constants;
		winrt::com_ptr<ID3D11VertexShader> vertexShader;
		winrt::com_ptr<ID3D11PixelShader> pixelShader;
		winrt::com_ptr<ID3D11BlendState> blend;
		winrt::com_ptr<ID3D11DepthStencilState> depthState;
		winrt::com_ptr<ID3D11RasterizerState> raster;
		winrt::com_ptr<ID3D11SamplerState> sampler;
		winrt::com_ptr<ID3D11Texture2D> sceneCopy;
		winrt::com_ptr<ID3D11ShaderResourceView> sceneCopySRV;
		bool shaderFailed = false;
	};
}
