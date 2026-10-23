#include "FallSurfaceRenderer.h"

#include <algorithm>
#include <cmath>
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
		constexpr UINT WATER_SLOT = 66;  // D183: the simulation's surface and flow (t66-t67)
		constexpr UINT WATER_MAP_SLOT = 104;
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
		allEmitters.clear();
		refilter = true;
		depthCopy = nullptr;
		depthCopySRV = nullptr;
		depthCopyFailed = false;
		ReleaseShaders();
	}

	void FallSurfaceRenderer::ReleaseShaders()
	{
		sprayVertexShader = nullptr;
		sprayPixelShader = nullptr;
		shaderFailed = false;
	}

	bool FallSurfaceRenderer::EnsureShaders()
	{
		if (sprayVertexShader && sprayPixelShader)
			return true;
		if (shaderFailed)
			return false;
		sprayVertexShader.attach(reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\FallSpray.hlsl", {}, "vs_5_0", "VSMain")));
		sprayPixelShader.attach(reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\FallSpray.hlsl", {}, "ps_5_0", "PSMain")));
		if (!sprayVertexShader || !sprayPixelShader) {
			logger::error("[Fluid Field] Waterfall spray shaders failed to compile; no spray");
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

	void FallSurfaceRenderer::Upload(ID3D11Device*, ID3D11DeviceContext*, const std::vector<FallEmitter>& a_emitters, uint64_t a_version)
	{
		if (a_version == uploadedVersion)
			return;
		uploadedVersion = a_version;
		// Shuffled, so that when only the first emitters fit (DrawParticles), they are spread over every fall
		allEmitters = a_emitters;
		std::mt19937 random(0x5eed);
		std::shuffle(allEmitters.begin(), allEmitters.end(), random);
		refilter = true;
	}

	void FallSurfaceRenderer::UploadNear(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const float (&a_camera)[3])
	{
		// D182: only the emitters near the camera reach the GPU (all of them, 19,554 after the finer bake, cost the vertex
		// shader every frame though their particles were faded out past SPRAY_FAR): within NEAR_REACH, nearest first, at most
		// MOST_NEAR; chosen again when the camera has moved REFILTER_MOVE or the emitters changed
		const float dx0 = a_camera[0] - filteredAt[0], dy0 = a_camera[1] - filteredAt[1], dz0 = a_camera[2] - filteredAt[2];
		if (!refilter && dx0 * dx0 + dy0 * dy0 + dz0 * dz0 < REFILTER_MOVE * REFILTER_MOVE)
			return;
		refilter = false;
		std::copy_n(a_camera, 3, filteredAt);
		std::vector<std::pair<float, uint32_t>> nearby;
		for (uint32_t i = 0; i < allEmitters.size(); i++) {
			const auto& e = allEmitters[i];
			const float dx = e.position[0] - a_camera[0], dy = e.position[1] - a_camera[1], dz = e.position[2] - a_camera[2];
			const float d2 = dx * dx + dy * dy + dz * dz;
			if (d2 < NEAR_REACH * NEAR_REACH)
				nearby.emplace_back(d2, i);
		}
		if (nearby.size() > MOST_NEAR) {
			std::nth_element(nearby.begin(), nearby.begin() + MOST_NEAR, nearby.end());
			nearby.resize(MOST_NEAR);
		}
		std::sort(nearby.begin(), nearby.end(), [](const auto& a_a, const auto& a_b) { return a_a.second < a_b.second; });  // the shuffle's spread kept
		emitterCount = 0;
		if (nearby.empty()) {
			float nearest = FLT_MAX;
			for (const auto& e : allEmitters) {
				const float dx = e.position[0] - a_camera[0], dy = e.position[1] - a_camera[1], dz = e.position[2] - a_camera[2];
				nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy + dz * dz));
			}
			logger::info("[Fluid Field] Spray: none of {} emitters near the eye at ({:.0f}, {:.0f}, {:.0f}); the nearest {:.0f} away", allEmitters.size(), a_camera[0], a_camera[1], a_camera[2], nearest);
			return;
		}
		const auto count = static_cast<uint32_t>(nearby.size());
		if (count > emitterCapacity) {
			emitters = nullptr;
			emittersSRV = nullptr;
			emitterCapacity = 0;
			const uint32_t capacity = std::max(count + count / 4, 256u);
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
		std::vector<FallEmitter> chosen;
		chosen.reserve(count);
		for (const auto& entry : nearby) {
			// D183: each with its own id from where it is (FallSpray's random choices: by its place in this list they changed
			// whenever the camera moved and the list was chosen again, and the particles jumped), packed over its kind
			FallEmitter emitter = allEmitters[entry.second];
			uint32_t id = 2166136261u;
			for (int k = 0; k < 3; k++)
				id = (id ^ static_cast<uint32_t>(static_cast<int32_t>(std::floor(emitter.position[k])))) * 16777619u;
			id = (id ^ static_cast<uint32_t>(emitter.kind)) * 16777619u;
			emitter.kind += 4.0f * static_cast<float>(id & EMITTER_ID_MASK);
			chosen.push_back(emitter);
		}
		const D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(count * sizeof(FallEmitter)), 1, 1 };
		a_context->UpdateSubresource(emitters.get(), 0, &box, chosen.data(), 0, 0);
		emitterCount = count;
		logger::info("[Fluid Field] Spray: {} of {} emitters near the eye at ({:.0f}, {:.0f}, {:.0f})", count, allEmitters.size(), a_camera[0], a_camera[1], a_camera[2]);
	}

	bool FallSurfaceRenderer::EnsureDepthCopy(ID3D11Device* a_device, ID3D11DepthStencilView* a_depth)
	{
		if (depthCopyFailed || !a_depth)
			return false;
		winrt::com_ptr<ID3D11Resource> resource;
		a_depth->GetResource(resource.put());
		auto texture = resource.try_as<ID3D11Texture2D>();
		if (!texture)
			return false;
		D3D11_TEXTURE2D_DESC source{};
		texture->GetDesc(&source);
		if (depthCopy) {
			D3D11_TEXTURE2D_DESC existing{};
			depthCopy->GetDesc(&existing);
			if (existing.Width == source.Width && existing.Height == source.Height && existing.Format == source.Format)
				return true;
		}
		depthCopy = nullptr;
		depthCopySRV = nullptr;
		D3D11_DEPTH_STENCIL_VIEW_DESC view{};
		a_depth->GetDesc(&view);
		// The read format from the depth's (the engine's depth is R24G8 or R32 typeless)
		DXGI_FORMAT readFormat = DXGI_FORMAT_UNKNOWN;
		switch (view.Format) {
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
			readFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			break;
		case DXGI_FORMAT_D32_FLOAT:
			readFormat = DXGI_FORMAT_R32_FLOAT;
			break;
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
			readFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			break;
		default:
			break;
		}
		D3D11_TEXTURE2D_DESC desc = source;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = readFormat;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		if (readFormat == DXGI_FORMAT_UNKNOWN || source.SampleDesc.Count != 1 || FAILED(a_device->CreateTexture2D(&desc, nullptr, depthCopy.put())) ||
			FAILED(a_device->CreateShaderResourceView(depthCopy.get(), &srvDesc, depthCopySRV.put()))) {
			depthCopy = nullptr;
			depthCopySRV = nullptr;
			depthCopyFailed = true;
			logger::warn("[Fluid Field] Spray: could not copy the depth; mist meets the water on a hard line");
			return false;
		}
		Util::SetResourceName(depthCopy.get(), "FluidField::FallEffectsDepth");
		Util::SetResourceName(depthCopySRV.get(), "FluidField::FallEffectsDepth SRV");
		return true;
	}

	void FallSurfaceRenderer::Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const DrawParams& a_params)
	{
		// D183: fixed emitters on the water surfaces (FallSites), the ones near the camera; each switches on and off over time in the
		// shader, by the water's foam and flow there
		UploadNear(a_device, a_context, a_params.cameraPosition);
		if (emitterCount == 0 || !a_params.target || !a_params.depth || !a_params.camera || !EnsureShaders() || !EnsureStates(a_device))
			return;
		const uint32_t perEmitter = std::max(a_params.sprayPerEmitter + a_params.mistPerEmitter, 1u);

		const Constants data{ a_params.time, BRIGHTNESS, std::min(emitterCount, MAX_PARTICLES / perEmitter), a_params.sprayPerEmitter,
			a_params.mistPerEmitter, SPRAY_LIFE, SPRAY_SPEED, a_params.spraySize, SPRAY_STRETCH, a_params.sprayOpacity, MIST_LIFE, MIST_SIZE, MIST_RISE, a_params.mistOpacity,
			PARTICLE_GRAVITY, 0.0f, a_params.mapSpacing, a_params.mapResolution, { 0.0f, 0.0f } };
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
		winrt::com_ptr<ID3D11ShaderResourceView> savedVSEmitters, savedPSDepth, savedVSWater[2], savedVSMap;
		a_context->VSGetShaderResources(EMITTERS_SLOT, 1, savedVSEmitters.put());
		a_context->VSGetShaderResources(WATER_SLOT, 2, reinterpret_cast<ID3D11ShaderResourceView**>(savedVSWater));
		a_context->VSGetShaderResources(WATER_MAP_SLOT, 1, savedVSMap.put());
		a_context->PSGetShaderResources(SCENE_DEPTH_SLOT, 1, savedPSDepth.put());

		// D182: the depth as it is now (the water planes and the falls in it), for the particles' soft contact: against the opaque
		// scene's alone, mist met the water and the falls on the depth test's hard line (cards cutting through the water, the user)
		ID3D11ShaderResourceView* contactDepth = a_params.sceneDepth;
		if (EnsureDepthCopy(a_device, a_params.depth)) {
			winrt::com_ptr<ID3D11Resource> live;
			a_params.depth->GetResource(live.put());
			a_context->OMSetRenderTargets(0, nullptr, nullptr);
			a_context->CopyResource(depthCopy.get(), live.get());
			contactDepth = depthCopySRV.get();
		}

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
		a_context->PSSetShaderResources(SCENE_DEPTH_SLOT, 1, &contactDepth);
		ID3D11ShaderResourceView* water[2] = { a_params.waterSurface, a_params.waterFlow };
		a_context->VSSetShaderResources(WATER_SLOT, 2, water);
		a_context->VSSetShaderResources(WATER_MAP_SLOT, 1, &a_params.waterMap);
		DrawParticles(a_context, perEmitter);

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
		ID3D11ShaderResourceView* vsWater[2] = { savedVSWater[0].get(), savedVSWater[1].get() };
		a_context->VSSetShaderResources(WATER_SLOT, 2, vsWater);
		ID3D11ShaderResourceView* vsMap = savedVSMap.get();
		a_context->VSSetShaderResources(WATER_MAP_SLOT, 1, &vsMap);
		ID3D11ShaderResourceView* psDepth = savedPSDepth.get();
		a_context->PSSetShaderResources(SCENE_DEPTH_SLOT, 1, &psDepth);
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
