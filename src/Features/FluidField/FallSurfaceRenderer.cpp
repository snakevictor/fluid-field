#include "FallSurfaceRenderer.h"

#include "FallStream.h"

#include <algorithm>
#include <cstring>
#include <random>

namespace FluidFieldSim
{
	namespace
	{
		constexpr UINT SHARED_SLOT = 5;  // SharedData, then FeatureData at 6, then ours at 7
		constexpr UINT CAMERA_SLOT = 12;
		constexpr UINT EMITTERS_SLOT = 100;
		constexpr UINT SCENE_DEPTH_SLOT = 103;
	}

	void FallSurfaceRenderer::Release()
	{
		emitters = nullptr;
		emittersSRV = nullptr;
		emitterCapacity = 0;
		emitterCount = 0;
		uploadedVersion = UINT64_MAX;
		constants = nullptr;
		blend = nullptr;
		depthState = nullptr;
		raster = nullptr;
		ReleaseShaders();
	}

	void FallSurfaceRenderer::ReleaseShaders()
	{
		sprayVertexShader = nullptr;
		sprayPixelShader = nullptr;
		fogVertexShader = nullptr;
		fogPixelShader = nullptr;
		shaderFailed = false;
	}

	bool FallSurfaceRenderer::EnsureShaders()
	{
		if (sprayVertexShader && sprayPixelShader && fogVertexShader && fogPixelShader)
			return true;
		if (shaderFailed)
			return false;
		sprayVertexShader.attach(reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\FallSpray.hlsl", {}, "vs_5_0", "VSMain")));
		sprayPixelShader.attach(reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\FallSpray.hlsl", {}, "ps_5_0", "PSMain")));
		fogVertexShader.attach(reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\FallFog.hlsl", {}, "vs_5_0", "VSMain")));
		fogPixelShader.attach(reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\FallFog.hlsl", {}, "ps_5_0", "PSMain")));
		if (!sprayVertexShader || !sprayPixelShader || !fogVertexShader || !fogPixelShader) {
			logger::error("[Fluid Field] Waterfall effect shaders failed to compile; no spray or waterfall fog");
			ReleaseShaders();
			shaderFailed = true;
			return false;
		}
		return true;
	}

	bool FallSurfaceRenderer::EnsureStates(ID3D11Device* a_device)
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
		Util::SetResourceName(constants.get(), "FluidField::FallEffectsConstants");
		return true;
	}

	void FallSurfaceRenderer::Upload(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const std::vector<FallEmitter>& a_emitters, uint64_t a_version)
	{
		if (a_version == uploadedVersion)
			return;
		uploadedVersion = a_version;
		emitterCount = 0;
		if (a_emitters.empty())
			return;
		const auto count = static_cast<uint32_t>(a_emitters.size());
		if (count > emitterCapacity) {
			emitters = nullptr;
			emittersSRV = nullptr;
			emitterCapacity = 0;
			const uint32_t capacity = count + count / 4;
			D3D11_BUFFER_DESC desc{ .ByteWidth = static_cast<UINT>(capacity * sizeof(FallEmitter)), .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, .StructureByteStride = sizeof(FallEmitter) };
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{ .Format = DXGI_FORMAT_UNKNOWN, .ViewDimension = D3D11_SRV_DIMENSION_BUFFER, .Buffer = { .FirstElement = 0, .NumElements = capacity } };
			if (FAILED(a_device->CreateBuffer(&desc, nullptr, emitters.put())) || FAILED(a_device->CreateShaderResourceView(emitters.get(), &srvDesc, emittersSRV.put()))) {
				emitters = nullptr;
				emittersSRV = nullptr;
				return;
			}
			Util::SetResourceName(emitters.get(), "FluidField::FallEmitters");
			Util::SetResourceName(emittersSRV.get(), "FluidField::FallEmitters SRV");
			emitterCapacity = capacity;
		}
		// Shuffled, so that when only the first emitters fit (DrawParticles), they are spread over every fall
		std::vector<FallEmitter> shuffled = a_emitters;
		std::mt19937 random(0x5eed);
		std::shuffle(shuffled.begin(), shuffled.end(), random);
		const D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(count * sizeof(FallEmitter)), 1, 1 };
		a_context->UpdateSubresource(emitters.get(), 0, &box, shuffled.data(), 0, 0);
		emitterCount = count;
	}

	void FallSurfaceRenderer::Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const DrawParams& a_params)
	{
		if ((emitterCount == 0 && a_params.fogDensity <= 0.0f) || !a_params.target || !a_params.depth || !a_params.camera || !EnsureShaders() || !EnsureStates(a_device))
			return;
		const uint32_t perEmitter = std::max(a_params.sprayPerEmitter + a_params.mistPerEmitter, 1u);

		const Constants data{ a_params.time, FallStream::BASE_SPEED, BRIGHTNESS, a_params.debugMode, std::min(emitterCount, MAX_PARTICLES / perEmitter), a_params.sprayPerEmitter,
			a_params.mistPerEmitter, SPRAY_LIFE, SPRAY_SPEED, a_params.spraySize, SPRAY_STRETCH, a_params.sprayOpacity, MIST_LIFE, MIST_SIZE, MIST_RISE, a_params.mistOpacity,
			PARTICLE_GRAVITY, a_params.fogDensity, FOG_STEPS, a_params.fogBottom, a_params.fogTop, {} };
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
		winrt::com_ptr<ID3D11ShaderResourceView> savedVSEmitters, savedPSDepth;
		a_context->VSGetShaderResources(EMITTERS_SLOT, 1, savedVSEmitters.put());
		a_context->PSGetShaderResources(SCENE_DEPTH_SLOT, 1, savedPSDepth.put());

		// Draw
		a_context->OMSetRenderTargets(1, &a_params.target, a_params.depth);
		const float factor[4] = {};
		a_context->OMSetBlendState(blend.get(), factor, 0xFFFFFFFF);
		a_context->OMSetDepthStencilState(depthState.get(), 0);
		a_context->RSSetState(raster.get());
		a_context->IASetInputLayout(nullptr);
		a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ID3D11Buffer* buffers[3] = { a_params.shared, a_params.features, constants.get() };
		a_context->VSSetConstantBuffers(SHARED_SLOT, 3, buffers);
		a_context->PSSetConstantBuffers(SHARED_SLOT, 3, buffers);
		a_context->VSSetConstantBuffers(CAMERA_SLOT, 1, &a_params.camera);
		a_context->PSSetConstantBuffers(CAMERA_SLOT, 1, &a_params.camera);
		a_context->PSSetShaderResources(SCENE_DEPTH_SLOT, 1, &a_params.sceneDepth);
		DrawParticles(a_context, perEmitter);
		if (a_params.fogDensity > 0.0f)
			DrawFog(a_context);

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
		ID3D11ShaderResourceView* vsEmitters = savedVSEmitters.get();
		a_context->VSSetShaderResources(EMITTERS_SLOT, 1, &vsEmitters);
		ID3D11ShaderResourceView* psDepth = savedPSDepth.get();
		a_context->PSSetShaderResources(SCENE_DEPTH_SLOT, 1, &psDepth);
	}

	void FallSurfaceRenderer::DrawFog(ID3D11DeviceContext* a_context)
	{
		// One screen-filling triangle; the vertices come from their index alone
		a_context->VSSetShader(fogVertexShader.get(), nullptr, 0);
		a_context->PSSetShader(fogPixelShader.get(), nullptr, 0);
		a_context->Draw(3, 0);
	}

	void FallSurfaceRenderer::DrawParticles(ID3D11DeviceContext* a_context, uint32_t a_perEmitter)
	{
		if (emitterCount == 0 || !emittersSRV || a_perEmitter == 0)
			return;
		// The emitters in the vertex shader's slot; six vertices (a quad) per particle
		a_context->VSSetShader(sprayVertexShader.get(), nullptr, 0);
		a_context->PSSetShader(sprayPixelShader.get(), nullptr, 0);
		ID3D11ShaderResourceView* view = emittersSRV.get();
		a_context->VSSetShaderResources(EMITTERS_SLOT, 1, &view);
		a_context->DrawInstanced(6, std::min(emitterCount, MAX_PARTICLES / a_perEmitter) * a_perEmitter, 0, 0);
	}
}
