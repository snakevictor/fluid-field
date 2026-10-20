#include "FallRefraction.h"

#include <DirectXPackedVector.h>
#include <algorithm>
#include <cstring>

namespace FluidFieldSim
{
	namespace
	{
		constexpr UINT SHARED_SLOT = 5;  // SharedData, FeatureData at 6, ours at 7
		constexpr UINT CAMERA_SLOT = 12;
		constexpr UINT GROUND_SLOT = 13;  // FallGroundCB
		constexpr UINT SRV_FIRST = 120;   // FallGround's: t120 the pools (Heights), t123 VertexClip, t124 FlowBake; t125 our vertices
		constexpr UINT SRV_COUNT = 6;
		constexpr UINT HEIGHTS_SLOT = 120;
		constexpr UINT VERTEX_CLIP_SLOT = 123;
		constexpr UINT BAKE_SLOT = 124;
		constexpr UINT VERTICES_SLOT = 125;
		constexpr UINT OWN_FIRST = 100;  // t100 the scene's colour ... t104 the environment (D3D11 has 128 slots)
		constexpr UINT OWN_COUNT = 5;
		constexpr UINT SCENE_COLOR_SLOT = 100;
		constexpr UINT DIFFUSE_SLOT = 102;
		constexpr UINT ENVIRONMENT_SLOT = 104;
		constexpr UINT SAMPLER_COUNT = 2;  // s0 wrap, s1 clamp

		/** @brief Everything Draw touches, saved on construction and put back on destruction. */
		struct SavedState
		{
			ID3D11DeviceContext* context;
			winrt::com_ptr<ID3D11RenderTargetView> targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
			winrt::com_ptr<ID3D11DepthStencilView> depth;
			winrt::com_ptr<ID3D11BlendState> blend;
			float blendFactor[4]{};
			UINT sampleMask = 0;
			winrt::com_ptr<ID3D11DepthStencilState> depthState;
			UINT stencilRef = 0;
			winrt::com_ptr<ID3D11RasterizerState> raster;
			winrt::com_ptr<ID3D11VertexShader> vs;
			winrt::com_ptr<ID3D11PixelShader> ps;
			D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
			winrt::com_ptr<ID3D11InputLayout> layout;
			winrt::com_ptr<ID3D11Buffer> indexBuffer;
			DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
			UINT indexOffset = 0;
			winrt::com_ptr<ID3D11Buffer> vsShared[3], psShared[3], vsCamera, psCamera, vsGround, psGround;
			winrt::com_ptr<ID3D11ShaderResourceView> vsViews[SRV_COUNT], psViews[SRV_COUNT], psOwnViews[OWN_COUNT];
			winrt::com_ptr<ID3D11SamplerState> samplers[SAMPLER_COUNT];

			explicit SavedState(ID3D11DeviceContext* a_context) :
				context(a_context)
			{
				context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, reinterpret_cast<ID3D11RenderTargetView**>(targets), depth.put());
				context->OMGetBlendState(blend.put(), blendFactor, &sampleMask);
				context->OMGetDepthStencilState(depthState.put(), &stencilRef);
				context->RSGetState(raster.put());
				context->VSGetShader(vs.put(), nullptr, nullptr);
				context->PSGetShader(ps.put(), nullptr, nullptr);
				context->IAGetPrimitiveTopology(&topology);
				context->IAGetInputLayout(layout.put());
				context->IAGetIndexBuffer(indexBuffer.put(), &indexFormat, &indexOffset);
				context->VSGetConstantBuffers(SHARED_SLOT, 3, reinterpret_cast<ID3D11Buffer**>(vsShared));
				context->PSGetConstantBuffers(SHARED_SLOT, 3, reinterpret_cast<ID3D11Buffer**>(psShared));
				context->VSGetConstantBuffers(CAMERA_SLOT, 1, vsCamera.put());
				context->PSGetConstantBuffers(CAMERA_SLOT, 1, psCamera.put());
				context->VSGetConstantBuffers(GROUND_SLOT, 1, vsGround.put());
				context->PSGetConstantBuffers(GROUND_SLOT, 1, psGround.put());
				context->VSGetShaderResources(SRV_FIRST, SRV_COUNT, reinterpret_cast<ID3D11ShaderResourceView**>(vsViews));
				context->PSGetShaderResources(SRV_FIRST, SRV_COUNT, reinterpret_cast<ID3D11ShaderResourceView**>(psViews));
				context->PSGetShaderResources(OWN_FIRST, OWN_COUNT, reinterpret_cast<ID3D11ShaderResourceView**>(psOwnViews));
				context->PSGetSamplers(0, SAMPLER_COUNT, reinterpret_cast<ID3D11SamplerState**>(samplers));
			}

			~SavedState()
			{
				ID3D11RenderTargetView* rawTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
				for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
					rawTargets[i] = targets[i].get();
				context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rawTargets, depth.get());
				context->OMSetBlendState(blend.get(), blendFactor, sampleMask);
				context->OMSetDepthStencilState(depthState.get(), stencilRef);
				context->RSSetState(raster.get());
				context->VSSetShader(vs.get(), nullptr, 0);
				context->PSSetShader(ps.get(), nullptr, 0);
				context->IASetPrimitiveTopology(topology);
				context->IASetInputLayout(layout.get());
				context->IASetIndexBuffer(indexBuffer.get(), indexFormat, indexOffset);
				ID3D11Buffer* vsCB[3] = { vsShared[0].get(), vsShared[1].get(), vsShared[2].get() };
				ID3D11Buffer* psCB[3] = { psShared[0].get(), psShared[1].get(), psShared[2].get() };
				context->VSSetConstantBuffers(SHARED_SLOT, 3, vsCB);
				context->PSSetConstantBuffers(SHARED_SLOT, 3, psCB);
				ID3D11Buffer* single = vsCamera.get();
				context->VSSetConstantBuffers(CAMERA_SLOT, 1, &single);
				single = psCamera.get();
				context->PSSetConstantBuffers(CAMERA_SLOT, 1, &single);
				single = vsGround.get();
				context->VSSetConstantBuffers(GROUND_SLOT, 1, &single);
				single = psGround.get();
				context->PSSetConstantBuffers(GROUND_SLOT, 1, &single);
				ID3D11ShaderResourceView* vsRaw[SRV_COUNT];
				ID3D11ShaderResourceView* psRaw[SRV_COUNT];
				for (UINT i = 0; i < SRV_COUNT; i++) {
					vsRaw[i] = vsViews[i].get();
					psRaw[i] = psViews[i].get();
				}
				context->VSSetShaderResources(SRV_FIRST, SRV_COUNT, vsRaw);
				context->PSSetShaderResources(SRV_FIRST, SRV_COUNT, psRaw);
				ID3D11ShaderResourceView* ownRaw[OWN_COUNT];
				for (UINT i = 0; i < OWN_COUNT; i++)
					ownRaw[i] = psOwnViews[i].get();
				context->PSSetShaderResources(OWN_FIRST, OWN_COUNT, ownRaw);
				ID3D11SamplerState* rawSamplers[SAMPLER_COUNT] = { samplers[0].get(), samplers[1].get() };
				context->PSSetSamplers(0, SAMPLER_COUNT, rawSamplers);
			}
		};
	}

	void FallRefraction::Release()
	{
		meshes.clear();
		failedMeshes.clear();
		sceneCopy = nullptr;
		sceneCopySRV = nullptr;
		sceneCopyFailed = false;
		constants = nullptr;
		groundConstants = nullptr;
		groundSize = 0;
		blend = nullptr;
		depthState = nullptr;
		raster = nullptr;
		wrapSampler = nullptr;
		clampSampler = nullptr;
		ReleaseShaders();
	}

	void FallRefraction::ReleaseShaders()
	{
		vertexShader = nullptr;
		pixelShader = nullptr;
		shaderFailed = false;
	}

	void FallRefraction::Prune(const std::unordered_set<const RE::BSGeometry*>& a_keep)
	{
		std::erase_if(meshes, [&](const auto& a_entry) { return !a_keep.contains(a_entry.first); });
		std::erase_if(failedMeshes, [&](const RE::BSGeometry* a_geometry) { return !a_keep.contains(a_geometry); });
	}

	bool FallRefraction::EnsureShaders()
	{
		if (vertexShader && pixelShader)
			return true;
		if (shaderFailed)
			return false;
		vertexShader.attach(reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\FallSurface.hlsl", {}, "vs_5_0", "VSMain")));
		pixelShader.attach(reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\FallSurface.hlsl", {}, "ps_5_0", "PSMain")));
		if (!vertexShader || !pixelShader) {
			logger::error("[Fluid Field] Refractive falls shaders failed to compile; the falls stay lit by the engine");
			ReleaseShaders();
			shaderFailed = true;
			return false;
		}
		return true;
	}

	bool FallRefraction::EnsureStates(ID3D11Device* a_device)
	{
		if (blend && depthState && raster && constants && wrapSampler && clampSampler)
			return true;
		D3D11_BLEND_DESC blendDesc{};
		auto& target = blendDesc.RenderTarget[0];
		target.BlendEnable = TRUE;
		target.SrcBlend = D3D11_BLEND_ONE;  // Premultiplied: the shader composites what is behind itself, alpha only fades the edges
		target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		target.BlendOp = D3D11_BLEND_OP_ADD;
		target.SrcBlendAlpha = D3D11_BLEND_ZERO;
		target.DestBlendAlpha = D3D11_BLEND_ONE;
		target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		depthDesc.DepthEnable = TRUE;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;  // Depth runs from 0 near to 1 far
		D3D11_RASTERIZER_DESC rasterDesc{};
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.CullMode = D3D11_CULL_NONE;  // The sheets are seen from both sides
		rasterDesc.DepthClipEnable = TRUE;
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_ANISOTROPIC;
		samplerDesc.MaxAnisotropy = 8;
		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		D3D11_SAMPLER_DESC clampDesc = samplerDesc;
		clampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		clampDesc.MaxAnisotropy = 1;
		clampDesc.AddressU = clampDesc.AddressV = clampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		D3D11_BUFFER_DESC cbDesc{ .ByteWidth = sizeof(Constants), .Usage = D3D11_USAGE_DYNAMIC, .BindFlags = D3D11_BIND_CONSTANT_BUFFER, .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE, .MiscFlags = 0, .StructureByteStride = 0 };
		blend = nullptr;
		depthState = nullptr;
		raster = nullptr;
		constants = nullptr;
		wrapSampler = nullptr;
		clampSampler = nullptr;
		if (FAILED(a_device->CreateBlendState(&blendDesc, blend.put())) || FAILED(a_device->CreateDepthStencilState(&depthDesc, depthState.put())) ||
			FAILED(a_device->CreateRasterizerState(&rasterDesc, raster.put())) || FAILED(a_device->CreateBuffer(&cbDesc, nullptr, constants.put())) ||
			FAILED(a_device->CreateSamplerState(&samplerDesc, wrapSampler.put())) || FAILED(a_device->CreateSamplerState(&clampDesc, clampSampler.put()))) {
			constants = nullptr;
			return false;
		}
		Util::SetResourceName(blend.get(), "FluidField::FallRefractionBlend");
		Util::SetResourceName(depthState.get(), "FluidField::FallRefractionDepthState");
		Util::SetResourceName(raster.get(), "FluidField::FallRefractionRaster");
		Util::SetResourceName(constants.get(), "FluidField::FallRefractionConstants");
		Util::SetResourceName(wrapSampler.get(), "FluidField::FallRefractionWrapSampler");
		Util::SetResourceName(clampSampler.get(), "FluidField::FallRefractionClampSampler");
		return true;
	}

	bool FallRefraction::EnsureSceneCopy(ID3D11Device* a_device, ID3D11Texture2D* a_source, ID3D11RenderTargetView* a_target)
	{
		D3D11_TEXTURE2D_DESC source{};
		a_source->GetDesc(&source);
		if (source.SampleDesc.Count != 1 || source.ArraySize != 1) {
			if (!sceneCopyFailed)
				logger::warn("[Fluid Field] Refractive falls: the scene target is multisampled or an array; the falls stay lit by the engine");
			sceneCopyFailed = true;
			return false;
		}
		if (sceneCopy) {
			D3D11_TEXTURE2D_DESC existing{};
			sceneCopy->GetDesc(&existing);
			if (existing.Width == source.Width && existing.Height == source.Height && existing.Format == source.Format && existing.MipLevels == source.MipLevels)
				return sceneCopySRV != nullptr;
		}
		sceneCopy = nullptr;
		sceneCopySRV = nullptr;
		D3D11_TEXTURE2D_DESC desc = source;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;
		D3D11_RENDER_TARGET_VIEW_DESC targetDesc{};
		a_target->GetDesc(&targetDesc);
		D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
		viewDesc.Format = targetDesc.Format;  // The target may be typeless; its view says how it is read
		viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		viewDesc.Texture2D.MostDetailedMip = 0;
		viewDesc.Texture2D.MipLevels = 1;
		if (FAILED(a_device->CreateTexture2D(&desc, nullptr, sceneCopy.put())) || FAILED(a_device->CreateShaderResourceView(sceneCopy.get(), &viewDesc, sceneCopySRV.put()))) {
			sceneCopy = nullptr;
			sceneCopySRV = nullptr;
			if (!sceneCopyFailed)
				logger::warn("[Fluid Field] Refractive falls: could not copy the scene target; the falls stay lit by the engine");
			sceneCopyFailed = true;
			return false;
		}
		Util::SetResourceName(sceneCopy.get(), "FluidField::FallRefractionScene");
		Util::SetResourceName(sceneCopySRV.get(), "FluidField::FallRefractionScene SRV");
		return true;
	}

	bool FallRefraction::BuildMesh(ID3D11Device* a_device, RE::BSGeometry* a_geometry, MeshBuffers& o_mesh) const
	{
		auto* triShape = a_geometry ? a_geometry->AsTriShape() : nullptr;
		auto* renderer = a_geometry ? a_geometry->GetGeometryRuntimeData().rendererData : nullptr;
		if (!triShape || !renderer || !renderer->rawVertexData || !renderer->rawIndexData || !renderer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_UV) ||
			renderer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_SKINNED))
			return false;
		const auto& counts = triShape->GetTrishapeRuntimeData();
		if (counts.vertexCount == 0 || counts.triangleCount == 0)
			return false;
		auto& desc = renderer->vertexDesc;  // GetSize is not const
		const uint32_t stride = desc.GetSize();
		const uint32_t positionOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
		const uint32_t uvOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_TEXCOORD0);
		const bool haveNormals = desc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_NORMAL);
		const uint32_t normalOffset = haveNormals ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_NORMAL) : 0;
		std::vector<Vertex> vertices(counts.vertexCount);
		for (uint32_t v = 0; v < counts.vertexCount; v++) {
			const uint8_t* data = renderer->rawVertexData + static_cast<std::size_t>(v) * stride;
			std::memcpy(vertices[v].position, data + positionOffset, sizeof(vertices[v].position));
			const auto* uv = reinterpret_cast<const uint16_t*>(data + uvOffset);
			vertices[v].uv[0] = DirectX::PackedVector::XMConvertHalfToFloat(uv[0]);
			vertices[v].uv[1] = DirectX::PackedVector::XMConvertHalfToFloat(uv[1]);
			if (haveNormals) {
				const uint8_t* n = data + normalOffset;
				for (int k = 0; k < 3; k++)
					vertices[v].normal[k] = n[k] / 255.0f * 2.0f - 1.0f;
			} else {
				vertices[v].normal[0] = vertices[v].normal[1] = vertices[v].normal[2] = 0.0f;
			}
		}
		std::vector<uint16_t> indices;
		indices.reserve(static_cast<std::size_t>(counts.triangleCount) * 3);
		for (uint32_t t = 0; t < counts.triangleCount; t++) {
			const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(t) * 3;
			if (index[0] >= counts.vertexCount || index[1] >= counts.vertexCount || index[2] >= counts.vertexCount)
				continue;
			indices.insert(indices.end(), index, index + 3);
			if (!haveNormals) {
				// Face normals summed, normalised in the shader
				const RE::NiPoint3 p0(vertices[index[0]].position[0], vertices[index[0]].position[1], vertices[index[0]].position[2]);
				const RE::NiPoint3 p1(vertices[index[1]].position[0], vertices[index[1]].position[1], vertices[index[1]].position[2]);
				const RE::NiPoint3 p2(vertices[index[2]].position[0], vertices[index[2]].position[1], vertices[index[2]].position[2]);
				const RE::NiPoint3 face = (p1 - p0).Cross(p2 - p0);
				for (int k = 0; k < 3; k++) {
					vertices[index[k]].normal[0] += face.x;
					vertices[index[k]].normal[1] += face.y;
					vertices[index[k]].normal[2] += face.z;
				}
			}
		}
		if (indices.empty())
			return false;
		const D3D11_BUFFER_DESC vertexDesc{ .ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Vertex)), .Usage = D3D11_USAGE_IMMUTABLE, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, .StructureByteStride = sizeof(Vertex) };
		const D3D11_SUBRESOURCE_DATA vertexData{ vertices.data(), 0, 0 };
		const D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{ .Format = DXGI_FORMAT_UNKNOWN, .ViewDimension = D3D11_SRV_DIMENSION_BUFFER, .Buffer = { .FirstElement = 0, .NumElements = static_cast<UINT>(vertices.size()) } };
		const D3D11_BUFFER_DESC indexDesc{ .ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint16_t)), .Usage = D3D11_USAGE_IMMUTABLE, .BindFlags = D3D11_BIND_INDEX_BUFFER, .CPUAccessFlags = 0, .MiscFlags = 0, .StructureByteStride = 0 };
		const D3D11_SUBRESOURCE_DATA indexData{ indices.data(), 0, 0 };
		if (FAILED(a_device->CreateBuffer(&vertexDesc, &vertexData, o_mesh.vertices.put())) ||
			FAILED(a_device->CreateShaderResourceView(o_mesh.vertices.get(), &viewDesc, o_mesh.verticesSRV.put())) ||
			FAILED(a_device->CreateBuffer(&indexDesc, &indexData, o_mesh.indices.put())))
			return false;
		Util::SetResourceName(o_mesh.vertices.get(), "FluidField::FallRefractionVertices");
		Util::SetResourceName(o_mesh.verticesSRV.get(), "FluidField::FallRefractionVertices SRV");
		Util::SetResourceName(o_mesh.indices.get(), "FluidField::FallRefractionIndices");
		o_mesh.vertexCount = counts.vertexCount;
		o_mesh.indexCount = static_cast<uint32_t>(indices.size());
		return true;
	}

	void FallRefraction::Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const DrawParams& a_params, const std::vector<MeshDraw>& a_meshes)
	{
		if (a_meshes.empty() || !a_params.target || !a_params.targetTexture || !a_params.depth || !a_params.sceneDepth || !a_params.camera || !EnsureShaders() ||
			!EnsureStates(a_device) || !EnsureSceneCopy(a_device, a_params.targetTexture, a_params.target))
			return;

		// The pieces' buffers, built once each (a piece whose address now holds other geometry is built again)
		std::vector<std::pair<const MeshDraw*, const MeshBuffers*>> drawable;
		drawable.reserve(a_meshes.size());
		for (const auto& draw : a_meshes) {
			auto* triShape = draw.geometry ? draw.geometry->AsTriShape() : nullptr;
			if (!triShape || !draw.ground || draw.groundSize == 0 || failedMeshes.contains(draw.geometry))
				continue;
			auto it = meshes.find(draw.geometry);
			if (it != meshes.end() && it->second.vertexCount != triShape->GetTrishapeRuntimeData().vertexCount) {
				meshes.erase(it);
				it = meshes.end();
			}
			if (it == meshes.end()) {
				MeshBuffers built;
				if (!BuildMesh(a_device, draw.geometry, built)) {
					failedMeshes.insert(draw.geometry);
					continue;
				}
				it = meshes.emplace(draw.geometry, std::move(built)).first;
			}
			drawable.emplace_back(&draw, &it->second);
		}
		if (drawable.empty())
			return;

		// The per-piece FallGroundCB, sized to what the caller hands over
		const uint32_t wantedGround = (a_meshes.front().groundSize + 15u) & ~15u;
		if (!groundConstants || groundSize != wantedGround) {
			groundConstants = nullptr;
			groundSize = 0;
			D3D11_BUFFER_DESC cbDesc{ .ByteWidth = wantedGround, .Usage = D3D11_USAGE_DYNAMIC, .BindFlags = D3D11_BIND_CONSTANT_BUFFER, .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE, .MiscFlags = 0, .StructureByteStride = 0 };
			if (FAILED(a_device->CreateBuffer(&cbDesc, nullptr, groundConstants.put())))
				return;
			Util::SetResourceName(groundConstants.get(), "FluidField::FallRefractionGround");
			groundSize = wantedGround;
		}

		SavedState saved(a_context);

		// What the falls refract: the scene as drawn so far (the falls' own lit pieces are hidden, D91)
		a_context->OMSetRenderTargets(0, nullptr, nullptr);
		a_context->CopyResource(sceneCopy.get(), a_params.targetTexture);

		a_context->OMSetRenderTargets(1, &a_params.target, a_params.depth);
		const float factor[4] = {};
		a_context->OMSetBlendState(blend.get(), factor, 0xFFFFFFFF);
		a_context->OMSetDepthStencilState(depthState.get(), 0);
		a_context->RSSetState(raster.get());
		a_context->IASetInputLayout(nullptr);
		a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		a_context->VSSetShader(vertexShader.get(), nullptr, 0);
		a_context->PSSetShader(pixelShader.get(), nullptr, 0);
		ID3D11Buffer* buffers[3] = { a_params.shared, a_params.features, constants.get() };
		a_context->VSSetConstantBuffers(SHARED_SLOT, 3, buffers);
		a_context->PSSetConstantBuffers(SHARED_SLOT, 3, buffers);
		a_context->VSSetConstantBuffers(CAMERA_SLOT, 1, &a_params.camera);
		a_context->PSSetConstantBuffers(CAMERA_SLOT, 1, &a_params.camera);
		ID3D11Buffer* ground = groundConstants.get();
		a_context->VSSetConstantBuffers(GROUND_SLOT, 1, &ground);
		a_context->PSSetConstantBuffers(GROUND_SLOT, 1, &ground);
		ID3D11SamplerState* samplers[SAMPLER_COUNT] = { wrapSampler.get(), clampSampler.get() };
		a_context->PSSetSamplers(0, SAMPLER_COUNT, samplers);
		a_context->VSSetShaderResources(HEIGHTS_SLOT, 1, &a_params.poolHeights);
		a_context->VSSetShaderResources(VERTEX_CLIP_SLOT, 1, &a_params.vertexClip);
		a_context->PSSetShaderResources(HEIGHTS_SLOT, 1, &a_params.poolHeights);
		ID3D11ShaderResourceView* sceneViews[2] = { sceneCopySRV.get(), a_params.sceneDepth };  // t100, t101
		a_context->PSSetShaderResources(SCENE_COLOR_SLOT, 2, sceneViews);
		a_context->PSSetShaderResources(ENVIRONMENT_SLOT, 1, &a_params.environment);

		Constants data{};
		std::copy_n(a_params.shallowColor, 3, data.shallowColor);
		std::copy_n(a_params.deepColor, 3, data.deepColor);
		data.clarity = std::max(a_params.clarity, 1.0f);
		data.refraction = a_params.refraction;
		data.fresnel = a_params.fresnel;
		data.specularPower = a_params.specularPower;
		data.specularStrength = a_params.specularStrength;
		data.streakFoam = a_params.streakFoam;
		data.hasEnvironment = a_params.environment ? 1u : 0u;
		data.maxPath = MAX_PATH;
		data.sheetThickness = SHEET_THICKNESS;

		for (const auto& [draw, mesh] : drawable) {
			std::copy_n(draw->world, 12, data.world);
			data.hasDiffuse = draw->diffuse ? 1u : 0u;
			data.hasNormal = draw->normal ? 1u : 0u;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(a_context->Map(constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
				continue;
			std::memcpy(mapped.pData, &data, sizeof(data));
			a_context->Unmap(constants.get(), 0);
			if (FAILED(a_context->Map(groundConstants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
				continue;
			std::memcpy(mapped.pData, draw->ground, std::min(draw->groundSize, groundSize));
			a_context->Unmap(groundConstants.get(), 0);

			ID3D11ShaderResourceView* vertices = mesh->verticesSRV.get();
			a_context->VSSetShaderResources(VERTICES_SLOT, 1, &vertices);
			a_context->PSSetShaderResources(BAKE_SLOT, 1, &draw->bake);
			ID3D11ShaderResourceView* textures[2] = { draw->diffuse, draw->normal };  // t102, t103
			a_context->PSSetShaderResources(DIFFUSE_SLOT, 2, textures);
			a_context->IASetIndexBuffer(mesh->indices.get(), DXGI_FORMAT_R16_UINT, 0);
			a_context->DrawIndexed(mesh->indexCount, 0, 0);
		}
	}
}
