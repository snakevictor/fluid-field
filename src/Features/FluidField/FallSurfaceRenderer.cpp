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
		constexpr UINT VERTICES_SLOT = 100;
		constexpr UINT TEXTURES_SLOT = 101;  // Diffuse, normal, scene depth
		constexpr UINT SAMPLER_SLOT = 0;
	}

	void FallSurfaceRenderer::Release()
	{
		mist = nullptr;
		mistSRV = nullptr;
		mistCount = 0;
		mistVersion = UINT64_MAX;
		vertices = nullptr;
		verticesSRV = nullptr;
		indices = nullptr;
		emitters = nullptr;
		emittersSRV = nullptr;
		emitterCapacity = 0;
		emitterCount = 0;
		vertexCapacity = 0;
		indexCapacity = 0;
		indexCount = 0;
		uploadedVersion = UINT64_MAX;
		constants = nullptr;
		blend = nullptr;
		depthState = nullptr;
		raster = nullptr;
		sampler = nullptr;
		ReleaseShaders();
	}

	void FallSurfaceRenderer::ReleaseShaders()
	{
		vertexShader = nullptr;
		pixelShader = nullptr;
		sprayVertexShader = nullptr;
		sprayPixelShader = nullptr;
		shaderFailed = false;
	}

	bool FallSurfaceRenderer::EnsureShaders()
	{
		if (vertexShader && pixelShader && sprayVertexShader && sprayPixelShader && fogVertexShader && fogPixelShader)
			return true;
		if (shaderFailed)
			return false;
		vertexShader.attach(reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\FallSurface.hlsl", {}, "vs_5_0", "VSMain")));
		pixelShader.attach(reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\FallSurface.hlsl", {}, "ps_5_0", "PSMain")));
		sprayVertexShader.attach(reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\FallSpray.hlsl", {}, "vs_5_0", "VSMain")));
		sprayPixelShader.attach(reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\FallSpray.hlsl", {}, "ps_5_0", "PSMain")));
		fogVertexShader.attach(reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\FallFog.hlsl", {}, "vs_5_0", "VSMain")));
		fogPixelShader.attach(reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\FallFog.hlsl", {}, "ps_5_0", "PSMain")));
		if (!vertexShader || !pixelShader || !sprayVertexShader || !sprayPixelShader || !fogVertexShader || !fogPixelShader) {
			logger::error("[Fluid Field] Waterfall surface shaders failed to compile; waterfalls stay vanilla");
			vertexShader = nullptr;
			pixelShader = nullptr;
			sprayVertexShader = nullptr;
			sprayPixelShader = nullptr;
			fogVertexShader = nullptr;
			fogPixelShader = nullptr;
			shaderFailed = true;
			return false;
		}
		return true;
	}

	bool FallSurfaceRenderer::EnsureStates(ID3D11Device* a_device)
	{
		if (blend && depthState && raster && constants && sampler)
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
		samplerDesc.Filter = D3D11_FILTER_ANISOTROPIC;
		samplerDesc.MaxAnisotropy = 8;
		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		D3D11_BUFFER_DESC cbDesc{ .ByteWidth = sizeof(Constants), .Usage = D3D11_USAGE_DYNAMIC, .BindFlags = D3D11_BIND_CONSTANT_BUFFER, .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE, .MiscFlags = 0, .StructureByteStride = 0 };
		if (FAILED(a_device->CreateBlendState(&blendDesc, blend.put())) || FAILED(a_device->CreateDepthStencilState(&depthDesc, depthState.put())) ||
			FAILED(a_device->CreateRasterizerState(&rasterDesc, raster.put())) || FAILED(a_device->CreateSamplerState(&samplerDesc, sampler.put())) ||
			FAILED(a_device->CreateBuffer(&cbDesc, nullptr, constants.put())))
			return false;
		Util::SetResourceName(constants.get(), "FluidField::FallSurfaceConstants");
		return true;
	}

	void FallSurfaceRenderer::Upload(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const std::vector<FallSurfaceVertex>& a_vertices, const std::vector<uint32_t>& a_indices,
		const std::vector<FallEmitter>& a_emitters, uint64_t a_version)
	{
		if (a_version == uploadedVersion)
			return;
		uploadedVersion = a_version;
		indexCount = 0;
		emitterCount = 0;
		if (!a_emitters.empty()) {
			const auto count = static_cast<uint32_t>(a_emitters.size());
			if (count > emitterCapacity) {
				emitters = nullptr;
				emittersSRV = nullptr;
				const uint32_t capacity = count + count / 4;
				D3D11_BUFFER_DESC desc{ .ByteWidth = static_cast<UINT>(capacity * sizeof(FallEmitter)), .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, .StructureByteStride = sizeof(FallEmitter) };
				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{ .Format = DXGI_FORMAT_UNKNOWN, .ViewDimension = D3D11_SRV_DIMENSION_BUFFER, .Buffer = { .FirstElement = 0, .NumElements = capacity } };
				if (SUCCEEDED(a_device->CreateBuffer(&desc, nullptr, emitters.put())) && SUCCEEDED(a_device->CreateShaderResourceView(emitters.get(), &srvDesc, emittersSRV.put()))) {
					Util::SetResourceName(emitters.get(), "FluidField::FallEmitters");
					Util::SetResourceName(emittersSRV.get(), "FluidField::FallEmitters SRV");
					emitterCapacity = capacity;
				} else {
					emitters = nullptr;
					emittersSRV = nullptr;
					emitterCapacity = 0;
				}
			}
			if (emitters) {
				// Shuffled, so that when only the first emitters fit (DrawParticles), they are spread over every fall
				std::vector<FallEmitter> shuffled = a_emitters;
				std::mt19937 random(0x5eed);
				std::shuffle(shuffled.begin(), shuffled.end(), random);
				const D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(count * sizeof(FallEmitter)), 1, 1 };
				a_context->UpdateSubresource(emitters.get(), 0, &box, shuffled.data(), 0, 0);
				emitterCount = count;
			}
		}
		if (a_vertices.empty() || a_indices.empty())
			return;
		const auto vertexCount = static_cast<uint32_t>(a_vertices.size());
		if (vertexCount > vertexCapacity) {
			vertices = nullptr;
			verticesSRV = nullptr;
			const uint32_t capacity = vertexCount + vertexCount / 4;
			D3D11_BUFFER_DESC desc{ .ByteWidth = static_cast<UINT>(capacity * sizeof(FallSurfaceVertex)), .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, .StructureByteStride = sizeof(FallSurfaceVertex) };
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{ .Format = DXGI_FORMAT_UNKNOWN, .ViewDimension = D3D11_SRV_DIMENSION_BUFFER, .Buffer = { .FirstElement = 0, .NumElements = capacity } };
			if (FAILED(a_device->CreateBuffer(&desc, nullptr, vertices.put())) || FAILED(a_device->CreateShaderResourceView(vertices.get(), &srvDesc, verticesSRV.put()))) {
				vertices = nullptr;
				verticesSRV = nullptr;
				vertexCapacity = 0;
				return;
			}
			Util::SetResourceName(vertices.get(), "FluidField::FallSurfaceVertices");
			Util::SetResourceName(verticesSRV.get(), "FluidField::FallSurfaceVertices SRV");
			vertexCapacity = capacity;
		}
		const auto count = static_cast<uint32_t>(a_indices.size());
		if (count > indexCapacity) {
			indices = nullptr;
			const uint32_t capacity = count + count / 4;
			D3D11_BUFFER_DESC desc{ .ByteWidth = static_cast<UINT>(capacity * sizeof(uint32_t)), .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_INDEX_BUFFER, .CPUAccessFlags = 0, .MiscFlags = 0, .StructureByteStride = 0 };
			if (FAILED(a_device->CreateBuffer(&desc, nullptr, indices.put()))) {
				indices = nullptr;
				indexCapacity = 0;
				return;
			}
			Util::SetResourceName(indices.get(), "FluidField::FallSurfaceIndices");
			indexCapacity = capacity;
		}
		const D3D11_BOX vertexBox{ 0, 0, 0, static_cast<UINT>(vertexCount * sizeof(FallSurfaceVertex)), 1, 1 };
		a_context->UpdateSubresource(vertices.get(), 0, &vertexBox, a_vertices.data(), 0, 0);
		const D3D11_BOX indexBox{ 0, 0, 0, static_cast<UINT>(count * sizeof(uint32_t)), 1, 1 };
		a_context->UpdateSubresource(indices.get(), 0, &indexBox, a_indices.data(), 0, 0);
		indexCount = count;
	}

	void FallSurfaceRenderer::Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const DrawParams& a_params)
	{
		if ((a_params.drawSurface && (indexCount == 0 || !verticesSRV || !indices || !a_params.diffuse)) || (!a_params.drawSurface && emitterCount == 0 && a_params.fogDensity <= 0.0f) || !a_params.target ||
			!a_params.depth || !a_params.camera || !EnsureShaders() || !EnsureStates(a_device))
			return;
		const uint32_t perEmitter = std::max(a_params.sprayPerEmitter + a_params.mistPerEmitter, 1u);

		const Constants data{ a_params.time, U_SCALE, V_SCALE, FallStream::BASE_SPEED, WAVE_HEIGHT, WAVE_LENGTH, RIPPLE_HEIGHT, RIPPLE_LENGTH, BULGE_HEIGHT, SHALLOW_DEPTH, CONTACT_WIDTH,
			BRIGHTNESS, a_params.debugMode, VANILLA_SCROLL, std::min(emitterCount, MAX_PARTICLES / perEmitter), a_params.sprayPerEmitter, a_params.mistPerEmitter, SPRAY_LIFE,
			SPRAY_SPEED, a_params.spraySize, MIST_LIFE, MIST_SIZE, MIST_RISE, PARTICLE_GRAVITY, a_params.mistOpacity, SPRAY_STRETCH, a_params.sprayOpacity, 0.0f,
			a_params.fogDensity, a_params.fogHeight, FOG_STEPS, mistCount };
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
		winrt::com_ptr<ID3D11Buffer> savedIndexBuffer;
		DXGI_FORMAT savedIndexFormat;
		UINT savedIndexOffset = 0;
		a_context->IAGetIndexBuffer(savedIndexBuffer.put(), &savedIndexFormat, &savedIndexOffset);
		winrt::com_ptr<ID3D11Buffer> savedVSCB[3], savedPSCB[3];
		a_context->VSGetConstantBuffers(SHARED_SLOT, 3, reinterpret_cast<ID3D11Buffer**>(savedVSCB));
		a_context->PSGetConstantBuffers(SHARED_SLOT, 3, reinterpret_cast<ID3D11Buffer**>(savedPSCB));
		winrt::com_ptr<ID3D11Buffer> savedVSCamera, savedPSCamera;
		a_context->VSGetConstantBuffers(CAMERA_SLOT, 1, savedVSCamera.put());
		a_context->PSGetConstantBuffers(CAMERA_SLOT, 1, savedPSCamera.put());
		winrt::com_ptr<ID3D11ShaderResourceView> savedVSVertices, savedPSTextures[3];
		a_context->VSGetShaderResources(VERTICES_SLOT, 1, savedVSVertices.put());
		a_context->PSGetShaderResources(TEXTURES_SLOT, 3, reinterpret_cast<ID3D11ShaderResourceView**>(savedPSTextures));
		winrt::com_ptr<ID3D11SamplerState> savedSampler;
		a_context->PSGetSamplers(SAMPLER_SLOT, 1, savedSampler.put());

		// Draw
		a_context->OMSetRenderTargets(1, &a_params.target, a_params.depth);
		const float factor[4] = {};
		a_context->OMSetBlendState(blend.get(), factor, 0xFFFFFFFF);
		a_context->OMSetDepthStencilState(depthState.get(), 0);
		a_context->RSSetState(raster.get());
		a_context->IASetInputLayout(nullptr);
		a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		a_context->IASetIndexBuffer(indices.get(), DXGI_FORMAT_R32_UINT, 0);
		a_context->VSSetShader(vertexShader.get(), nullptr, 0);
		a_context->PSSetShader(pixelShader.get(), nullptr, 0);
		ID3D11Buffer* buffers[3] = { a_params.shared, a_params.features, constants.get() };
		a_context->VSSetConstantBuffers(SHARED_SLOT, 3, buffers);
		a_context->PSSetConstantBuffers(SHARED_SLOT, 3, buffers);
		a_context->VSSetConstantBuffers(CAMERA_SLOT, 1, &a_params.camera);
		a_context->PSSetConstantBuffers(CAMERA_SLOT, 1, &a_params.camera);
		ID3D11ShaderResourceView* vertexView = verticesSRV.get();
		a_context->VSSetShaderResources(VERTICES_SLOT, 1, &vertexView);
		ID3D11ShaderResourceView* textures[3] = { a_params.diffuse, a_params.normal, a_params.sceneDepth };
		a_context->PSSetShaderResources(TEXTURES_SLOT, 3, textures);
		ID3D11SamplerState* wrap = sampler.get();
		a_context->PSSetSamplers(SAMPLER_SLOT, 1, &wrap);
		if (a_params.drawSurface && indexCount > 0)
			a_context->DrawIndexed(indexCount, 0, 0);
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
		a_context->IASetIndexBuffer(savedIndexBuffer.get(), savedIndexFormat, savedIndexOffset);
		ID3D11Buffer* vsCB[3] = { savedVSCB[0].get(), savedVSCB[1].get(), savedVSCB[2].get() };
		ID3D11Buffer* psCB[3] = { savedPSCB[0].get(), savedPSCB[1].get(), savedPSCB[2].get() };
		a_context->VSSetConstantBuffers(SHARED_SLOT, 3, vsCB);
		a_context->PSSetConstantBuffers(SHARED_SLOT, 3, psCB);
		ID3D11Buffer* vsCamera = savedVSCamera.get();
		ID3D11Buffer* psCamera = savedPSCamera.get();
		a_context->VSSetConstantBuffers(CAMERA_SLOT, 1, &vsCamera);
		a_context->PSSetConstantBuffers(CAMERA_SLOT, 1, &psCamera);
		ID3D11ShaderResourceView* vsVertices = savedVSVertices.get();
		a_context->VSSetShaderResources(VERTICES_SLOT, 1, &vsVertices);
		ID3D11ShaderResourceView* psTextures[3] = { savedPSTextures[0].get(), savedPSTextures[1].get(), savedPSTextures[2].get() };
		a_context->PSSetShaderResources(TEXTURES_SLOT, 3, psTextures);
		ID3D11SamplerState* psSampler = savedSampler.get();
		a_context->PSSetSamplers(SAMPLER_SLOT, 1, &psSampler);
	}

	void FallSurfaceRenderer::UploadMist(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const std::vector<std::array<float, 8>>& a_volumes, uint64_t a_version)
	{
		if (a_version == mistVersion && mist)
			return;
		mistVersion = a_version;
		if (!mist) {
			D3D11_BUFFER_DESC desc{ .ByteWidth = MAX_MIST_VOLUMES * 2 * 16, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, .StructureByteStride = 16 };
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{ .Format = DXGI_FORMAT_UNKNOWN, .ViewDimension = D3D11_SRV_DIMENSION_BUFFER, .Buffer = { .FirstElement = 0, .NumElements = MAX_MIST_VOLUMES * 2 } };
			if (FAILED(a_device->CreateBuffer(&desc, nullptr, mist.put())) || FAILED(a_device->CreateShaderResourceView(mist.get(), &srvDesc, mistSRV.put()))) {
				mist = nullptr;
				mistSRV = nullptr;
				mistCount = 0;
				return;
			}
			Util::SetResourceName(mist.get(), "FluidField::FallMistVolumes");
			Util::SetResourceName(mistSRV.get(), "FluidField::FallMistVolumes SRV");
		}
		mistCount = static_cast<uint32_t>(std::min<std::size_t>(a_volumes.size(), MAX_MIST_VOLUMES));
		if (mistCount > 0) {
			const D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(mistCount * sizeof(a_volumes[0])), 1, 1 };
			a_context->UpdateSubresource(mist.get(), 0, &box, a_volumes.data(), 0, 0);
		}
	}

	void FallSurfaceRenderer::DrawFog(ID3D11DeviceContext* a_context)
	{
		// One screen-filling triangle; the vertices come from their index alone
		a_context->VSSetShader(fogVertexShader.get(), nullptr, 0);
		a_context->PSSetShader(fogPixelShader.get(), nullptr, 0);
		ID3D11ShaderResourceView* view = mistSRV.get();
		a_context->PSSetShaderResources(TEXTURES_SLOT + 3, 1, &view);
		a_context->Draw(3, 0);
		ID3D11ShaderResourceView* none = nullptr;
		a_context->PSSetShaderResources(TEXTURES_SLOT + 3, 1, &none);
	}

	void FallSurfaceRenderer::DrawParticles(ID3D11DeviceContext* a_context, uint32_t a_perEmitter)
	{
		if (emitterCount == 0 || !emittersSRV || a_perEmitter == 0)
			return;
		// Same targets, states and constants as the surface; the emitters in the vertex shader's slot
		a_context->VSSetShader(sprayVertexShader.get(), nullptr, 0);
		a_context->PSSetShader(sprayPixelShader.get(), nullptr, 0);
		ID3D11ShaderResourceView* view = emittersSRV.get();
		a_context->VSSetShaderResources(VERTICES_SLOT, 1, &view);
		a_context->DrawInstanced(6, std::min(emitterCount, MAX_PARTICLES / a_perEmitter) * a_perEmitter, 0, 0);
	}
}
