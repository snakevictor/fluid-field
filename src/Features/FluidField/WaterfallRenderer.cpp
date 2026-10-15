#include "WaterfallRenderer.h"

#include <cstring>

namespace FluidFieldSim
{
	namespace
	{
		constexpr UINT CB_SLOT = 7;
		constexpr UINT POINTS_SLOT = 100;
		constexpr UINT SHARED_SLOT = 5;  // SharedData, then FeatureData at 6
		constexpr UINT CAMERA_SLOT = 12;
	}

	void WaterfallRenderer::Release()
	{
		points = nullptr;
		pointsSRV = nullptr;
		capacity = 0;
		curtainCount = 0;
		uploadedVersion = UINT64_MAX;
		constants = nullptr;
		blend = nullptr;
		depthState = nullptr;
		raster = nullptr;
		ReleaseShaders();
	}

	void WaterfallRenderer::ReleaseShaders()
	{
		vertexShader = nullptr;
		pixelShader = nullptr;
		shaderFailed = false;
	}

	bool WaterfallRenderer::EnsureShaders(ID3D11Device*)
	{
		if (vertexShader && pixelShader)
			return true;
		if (shaderFailed)
			return false;
		vertexShader.attach(reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\Waterfall.hlsl", {}, "vs_5_0", "VSMain")));
		pixelShader.attach(reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\Waterfall.hlsl", {}, "ps_5_0", "PSMain")));
		if (!vertexShader || !pixelShader) {
			logger::error("[Fluid Field] Waterfall shaders failed to compile; waterfalls stay vanilla");
			vertexShader = nullptr;
			pixelShader = nullptr;
			shaderFailed = true;
			return false;
		}
		return true;
	}

	bool WaterfallRenderer::EnsureStates(ID3D11Device* a_device)
	{
		if (blend && depthState && raster && constants)
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
		D3D11_BUFFER_DESC cbDesc{ .ByteWidth = sizeof(Constants), .Usage = D3D11_USAGE_DYNAMIC, .BindFlags = D3D11_BIND_CONSTANT_BUFFER, .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE, .MiscFlags = 0, .StructureByteStride = 0 };
		if (FAILED(a_device->CreateBlendState(&blendDesc, blend.put())) || FAILED(a_device->CreateDepthStencilState(&depthDesc, depthState.put())) ||
			FAILED(a_device->CreateRasterizerState(&rasterDesc, raster.put())) || FAILED(a_device->CreateBuffer(&cbDesc, nullptr, constants.put())))
			return false;
		Util::SetResourceName(constants.get(), "FluidField::WaterfallConstants");
		return true;
	}

	void WaterfallRenderer::Upload(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const WaterfallSources& a_sources)
	{
		if (a_sources.GetCurtainVersion() == uploadedVersion)
			return;
		uploadedVersion = a_sources.GetCurtainVersion();
		const auto& curtains = a_sources.GetCurtains();
		constexpr uint32_t perCurtain = WaterfallSources::CURTAIN_COLUMNS * WaterfallSources::CURTAIN_ROWS;
		curtainCount = static_cast<uint32_t>(curtains.size() / perCurtain);
		if (curtainCount == 0)
			return;
		const auto count = curtainCount * perCurtain;
		if (count > capacity) {
			points = nullptr;
			pointsSRV = nullptr;
			D3D11_BUFFER_DESC desc{ .ByteWidth = static_cast<UINT>(count * sizeof(WaterfallSources::CurtainPoint)), .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, .StructureByteStride = sizeof(WaterfallSources::CurtainPoint) };
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{ .Format = DXGI_FORMAT_UNKNOWN, .ViewDimension = D3D11_SRV_DIMENSION_BUFFER, .Buffer = { .FirstElement = 0, .NumElements = count } };
			if (FAILED(a_device->CreateBuffer(&desc, nullptr, points.put())) || FAILED(a_device->CreateShaderResourceView(points.get(), &srvDesc, pointsSRV.put()))) {
				points = nullptr;
				pointsSRV = nullptr;
				capacity = 0;
				curtainCount = 0;
				return;
			}
			Util::SetResourceName(points.get(), "FluidField::WaterfallCurtains");
			Util::SetResourceName(pointsSRV.get(), "FluidField::WaterfallCurtains SRV");
			capacity = count;
		}
		const D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(count * sizeof(WaterfallSources::CurtainPoint)), 1, 1 };
		a_context->UpdateSubresource(points.get(), 0, &box, curtains.data(), 0, 0);
	}

	void WaterfallRenderer::Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target, ID3D11DepthStencilView* a_depth, ID3D11Buffer* a_camera,
		ID3D11Buffer* a_shared, ID3D11Buffer* a_features, float a_time, float a_opacity)
	{
		if (curtainCount == 0 || !pointsSRV || !a_target || !a_depth || !a_camera || !EnsureShaders(a_device) || !EnsureStates(a_device))
			return;

		const Constants data{ WaterfallSources::CURTAIN_COLUMNS, WaterfallSources::CURTAIN_ROWS, a_time, a_opacity };
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return;
		std::memcpy(mapped.pData, &data, sizeof(data));
		a_context->Unmap(constants.get(), 0);

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
		winrt::com_ptr<ID3D11ShaderResourceView> savedVSPoints;
		a_context->VSGetShaderResources(POINTS_SLOT, 1, savedVSPoints.put());

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
		ID3D11ShaderResourceView* srv = pointsSRV.get();
		a_context->VSSetShaderResources(POINTS_SLOT, 1, &srv);
		constexpr UINT vertices = (WaterfallSources::CURTAIN_COLUMNS - 1) * (WaterfallSources::CURTAIN_ROWS - 1) * 6;
		a_context->DrawInstanced(vertices, curtainCount, 0, 0);

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
		ID3D11ShaderResourceView* vsPoints = savedVSPoints.get();
		a_context->VSSetShaderResources(POINTS_SLOT, 1, &vsPoints);
	}
}
