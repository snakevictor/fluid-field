#include "SimSurfaceRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace FluidFieldSim
{
	namespace
	{
		constexpr UINT CB_SLOT = 7;
		constexpr UINT TEXTURE_SLOT = 100;  // Depth, cell map, flow, fall bed, scene copy
		constexpr UINT TEXTURE_COUNT = 5;
		constexpr UINT SHARED_SLOT = 5;  // SharedData, then FeatureData at 6
		constexpr UINT CAMERA_SLOT = 12;
		/** @brief Most vertices across an area's grid (it is clipped to the level anyway). */
		constexpr int32_t MAX_SIDE = 1024;
		/** @brief Cells between vertices of the surface mesh. */
		constexpr int32_t VERTEX_STRIDE = 2;
	}

	void SimSurfaceRenderer::Release()
	{
		constants = nullptr;
		blend = nullptr;
		depthState = nullptr;
		raster = nullptr;
		sampler = nullptr;
		sceneCopy = nullptr;
		sceneCopySRV = nullptr;
		ReleaseShaders();
	}

	void SimSurfaceRenderer::ReleaseShaders()
	{
		vertexShader = nullptr;
		pixelShader = nullptr;
		shaderFailed = false;
	}

	bool SimSurfaceRenderer::EnsureShaders()
	{
		if (vertexShader && pixelShader)
			return true;
		if (shaderFailed)
			return false;
		vertexShader.attach(reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\SimSurface.hlsl", {}, "vs_5_0", "VSMain")));
		pixelShader.attach(reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\SimSurface.hlsl", {}, "ps_5_0", "PSMain")));
		if (!vertexShader || !pixelShader) {
			logger::error("[Fluid Field] Simulated water surface shaders failed to compile; falls are not drawn");
			vertexShader = nullptr;
			pixelShader = nullptr;
			shaderFailed = true;
			return false;
		}
		return true;
	}

	bool SimSurfaceRenderer::EnsureStates(ID3D11Device* a_device)
	{
		if (blend && depthState && raster && sampler && constants)
			return true;
		D3D11_BLEND_DESC blendDesc{};
		auto& target = blendDesc.RenderTarget[0];
		target.BlendEnable = TRUE;
		target.SrcBlend = D3D11_BLEND_ONE;  // Premultiplied
		target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		target.BlendOp = D3D11_BLEND_OP_ADD;
		target.SrcBlendAlpha = D3D11_BLEND_ZERO;
		target.DestBlendAlpha = D3D11_BLEND_ONE;
		target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		depthDesc.DepthEnable = TRUE;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;  // Depth runs from 0 near to 1 far (the sky)
		D3D11_RASTERIZER_DESC rasterDesc{};
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.CullMode = D3D11_CULL_NONE;
		rasterDesc.DepthClipEnable = TRUE;
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		D3D11_BUFFER_DESC cbDesc{ .ByteWidth = sizeof(Constants), .Usage = D3D11_USAGE_DYNAMIC, .BindFlags = D3D11_BIND_CONSTANT_BUFFER, .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE, .MiscFlags = 0, .StructureByteStride = 0 };
		if (FAILED(a_device->CreateBlendState(&blendDesc, blend.put())) || FAILED(a_device->CreateDepthStencilState(&depthDesc, depthState.put())) ||
			FAILED(a_device->CreateRasterizerState(&rasterDesc, raster.put())) || FAILED(a_device->CreateSamplerState(&samplerDesc, sampler.put())) ||
			FAILED(a_device->CreateBuffer(&cbDesc, nullptr, constants.put())))
			return false;
		Util::SetResourceName(constants.get(), "FluidField::SimSurfaceConstants");
		return true;
	}

	bool SimSurfaceRenderer::EnsureSceneCopy(ID3D11Device* a_device, ID3D11Texture2D* a_target)
	{
		D3D11_TEXTURE2D_DESC wanted{};
		a_target->GetDesc(&wanted);
		if (sceneCopy) {
			D3D11_TEXTURE2D_DESC existing{};
			sceneCopy->GetDesc(&existing);
			if (existing.Width == wanted.Width && existing.Height == wanted.Height && existing.Format == wanted.Format)
				return true;
		}
		sceneCopy = nullptr;
		sceneCopySRV = nullptr;
		wanted.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		wanted.MiscFlags = 0;
		wanted.MipLevels = 1;
		wanted.CPUAccessFlags = 0;
		wanted.Usage = D3D11_USAGE_DEFAULT;
		if (FAILED(a_device->CreateTexture2D(&wanted, nullptr, sceneCopy.put())) || FAILED(a_device->CreateShaderResourceView(sceneCopy.get(), nullptr, sceneCopySRV.put()))) {
			sceneCopy = nullptr;
			sceneCopySRV = nullptr;
			return false;
		}
		Util::SetResourceName(sceneCopy.get(), "FluidField::SimSurfaceSceneCopy");
		Util::SetResourceName(sceneCopySRV.get(), "FluidField::SimSurfaceSceneCopy SRV");
		return true;
	}

	void SimSurfaceRenderer::Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11Texture2D* a_targetTexture, ID3D11RenderTargetView* a_target, ID3D11DepthStencilView* a_depth,
		ID3D11Buffer* a_camera, ID3D11Buffer* a_shared, ID3D11Buffer* a_features, const Level& a_level, std::span<const FallBed::Area> a_areas, float a_time, float a_brightness)
	{
		if (a_areas.empty() || !a_targetTexture || !a_target || !a_depth || !a_camera || !a_level.depth || !a_level.cellMap || !a_level.flow || !a_level.fallBed)
			return;
		if (!EnsureShaders() || !EnsureStates(a_device) || !EnsureSceneCopy(a_device, a_targetTexture))
			return;

		// The scene behind the water, for refraction
		a_context->CopyResource(sceneCopy.get(), a_targetTexture);
		D3D11_TEXTURE2D_DESC targetDesc{};
		a_targetTexture->GetDesc(&targetDesc);

		// Save what is touched
		winrt::com_ptr<ID3D11RenderTargetView> savedTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
		winrt::com_ptr<ID3D11DepthStencilView> savedDepth;
		a_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, reinterpret_cast<ID3D11RenderTargetView**>(savedTargets), savedDepth.put());
		winrt::com_ptr<ID3D11BlendState> savedBlend;
		float savedFactor[4];
		UINT savedMask = 0;
		a_context->OMGetBlendState(savedBlend.put(), savedFactor, &savedMask);
		winrt::com_ptr<ID3D11DepthStencilState> savedDepthState;
		UINT savedStencilRef = 0;
		a_context->OMGetDepthStencilState(savedDepthState.put(), &savedStencilRef);
		winrt::com_ptr<ID3D11RasterizerState> savedRaster;
		a_context->RSGetState(savedRaster.put());
		winrt::com_ptr<ID3D11VertexShader> savedVS;
		winrt::com_ptr<ID3D11PixelShader> savedPS;
		a_context->VSGetShader(savedVS.put(), nullptr, nullptr);
		a_context->PSGetShader(savedPS.put(), nullptr, nullptr);
		D3D11_PRIMITIVE_TOPOLOGY savedTopology;
		a_context->IAGetPrimitiveTopology(&savedTopology);
		winrt::com_ptr<ID3D11InputLayout> savedLayout;
		a_context->IAGetInputLayout(savedLayout.put());
		winrt::com_ptr<ID3D11Buffer> savedVSCB[3], savedPSCB[3];
		a_context->VSGetConstantBuffers(SHARED_SLOT, 3, reinterpret_cast<ID3D11Buffer**>(savedVSCB));
		a_context->PSGetConstantBuffers(SHARED_SLOT, 3, reinterpret_cast<ID3D11Buffer**>(savedPSCB));
		winrt::com_ptr<ID3D11Buffer> savedVSCamera, savedPSCamera;
		a_context->VSGetConstantBuffers(CAMERA_SLOT, 1, savedVSCamera.put());
		a_context->PSGetConstantBuffers(CAMERA_SLOT, 1, savedPSCamera.put());
		winrt::com_ptr<ID3D11ShaderResourceView> savedVSTextures[TEXTURE_COUNT], savedPSTextures[TEXTURE_COUNT];
		a_context->VSGetShaderResources(TEXTURE_SLOT, TEXTURE_COUNT, reinterpret_cast<ID3D11ShaderResourceView**>(savedVSTextures));
		a_context->PSGetShaderResources(TEXTURE_SLOT, TEXTURE_COUNT, reinterpret_cast<ID3D11ShaderResourceView**>(savedPSTextures));
		winrt::com_ptr<ID3D11SamplerState> savedSampler;
		a_context->PSGetSamplers(0, 1, savedSampler.put());

		// Draw
		a_context->OMSetRenderTargets(1, &a_target, a_depth);
		const float factor[4] = {};
		a_context->OMSetBlendState(blend.get(), factor, 0xFFFFFFFF);
		a_context->OMSetDepthStencilState(depthState.get(), 0);
		a_context->RSSetState(raster.get());
		a_context->IASetInputLayout(nullptr);
		a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		a_context->VSSetShader(vertexShader.get(), nullptr, 0);
		a_context->PSSetShader(pixelShader.get(), nullptr, 0);
		ID3D11Buffer* buffers[3] = { a_shared, a_features, constants.get() };
		a_context->VSSetConstantBuffers(SHARED_SLOT, 3, buffers);
		a_context->PSSetConstantBuffers(SHARED_SLOT, 3, buffers);
		a_context->VSSetConstantBuffers(CAMERA_SLOT, 1, &a_camera);
		a_context->PSSetConstantBuffers(CAMERA_SLOT, 1, &a_camera);
		ID3D11ShaderResourceView* textures[TEXTURE_COUNT] = { a_level.depth, a_level.cellMap, a_level.flow, a_level.fallBed, sceneCopySRV.get() };
		a_context->VSSetShaderResources(TEXTURE_SLOT, TEXTURE_COUNT, textures);
		a_context->PSSetShaderResources(TEXTURE_SLOT, TEXTURE_COUNT, textures);
		ID3D11SamplerState* samplerState = sampler.get();
		a_context->PSSetSamplers(0, 1, &samplerState);

		const auto gridMax0 = a_level.gridMin[0] + static_cast<int32_t>(a_level.resolution) - 2;
		const auto gridMax1 = a_level.gridMin[1] + static_cast<int32_t>(a_level.resolution) - 2;
		for (const auto& area : a_areas) {
			// The area's cells inside the level, one cell in from its edge so neighbours exist
			const int32_t x0 = std::max(static_cast<int32_t>(std::floor(area.min[0] / a_level.cellSize)), a_level.gridMin[0] + 1);
			const int32_t y0 = std::max(static_cast<int32_t>(std::floor(area.min[1] / a_level.cellSize)), a_level.gridMin[1] + 1);
			const int32_t x1 = std::min(static_cast<int32_t>(std::ceil(area.max[0] / a_level.cellSize)), gridMax0);
			const int32_t y1 = std::min(static_cast<int32_t>(std::ceil(area.max[1] / a_level.cellSize)), gridMax1);
			// A vertex every VERTEX_STRIDE cells
			const int32_t across = std::min((x1 - x0) / VERTEX_STRIDE + 1, MAX_SIDE);
			const int32_t down = std::min((y1 - y0) / VERTEX_STRIDE + 1, MAX_SIDE);
			if (across < 2 || down < 2)
				continue;
			const Constants data{ { x0, y0 }, { across, down }, a_level.resolution, a_level.cellSize, a_time, a_brightness, VERTEX_STRIDE,
				{ 1.0f / static_cast<float>(targetDesc.Width), 1.0f / static_cast<float>(targetDesc.Height) }, 0.0f };
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(a_context->Map(constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
				continue;
			std::memcpy(mapped.pData, &data, sizeof(data));
			a_context->Unmap(constants.get(), 0);
			a_context->Draw(static_cast<UINT>((across - 1) * (down - 1) * 6), 0);
		}

		// Restore
		ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
		for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
			targets[i] = savedTargets[i].get();
		a_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, savedDepth.get());
		a_context->OMSetBlendState(savedBlend.get(), savedFactor, savedMask);
		a_context->OMSetDepthStencilState(savedDepthState.get(), savedStencilRef);
		a_context->RSSetState(savedRaster.get());
		a_context->VSSetShader(savedVS.get(), nullptr, 0);
		a_context->PSSetShader(savedPS.get(), nullptr, 0);
		a_context->IASetPrimitiveTopology(savedTopology);
		a_context->IASetInputLayout(savedLayout.get());
		ID3D11Buffer* vsCB[3] = { savedVSCB[0].get(), savedVSCB[1].get(), savedVSCB[2].get() };
		ID3D11Buffer* psCB[3] = { savedPSCB[0].get(), savedPSCB[1].get(), savedPSCB[2].get() };
		a_context->VSSetConstantBuffers(SHARED_SLOT, 3, vsCB);
		a_context->PSSetConstantBuffers(SHARED_SLOT, 3, psCB);
		ID3D11Buffer* vsCamera = savedVSCamera.get();
		ID3D11Buffer* psCamera = savedPSCamera.get();
		a_context->VSSetConstantBuffers(CAMERA_SLOT, 1, &vsCamera);
		a_context->PSSetConstantBuffers(CAMERA_SLOT, 1, &psCamera);
		ID3D11ShaderResourceView* vsTextures[TEXTURE_COUNT];
		ID3D11ShaderResourceView* psTextures[TEXTURE_COUNT];
		for (UINT i = 0; i < TEXTURE_COUNT; i++) {
			vsTextures[i] = savedVSTextures[i].get();
			psTextures[i] = savedPSTextures[i].get();
		}
		a_context->VSSetShaderResources(TEXTURE_SLOT, TEXTURE_COUNT, vsTextures);
		a_context->PSSetShaderResources(TEXTURE_SLOT, TEXTURE_COUNT, psTextures);
		ID3D11SamplerState* restoredSampler = savedSampler.get();
		a_context->PSSetSamplers(0, 1, &restoredSampler);
	}
}
