#include "ShallowWaterSolver.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace FluidFieldSim
{
	namespace
	{
		constexpr std::uint32_t GROUP_SIZE = 8;  // FLUIDFIELD_GROUP_SIZE in Common.hlsli
		constexpr std::uint32_t MAX_SRVS = 4;    // t0..t3; the map sits at t4, fine detail at t5, the flowmap at t6
		constexpr std::uint32_t MAP_SLOT = 4;
		constexpr std::uint32_t BOUND_SRVS = 7;
		constexpr std::uint32_t MAX_UAVS = 3;

		ID3D11ShaderResourceView* const NULL_SRVS[BOUND_SRVS] = {};
		ID3D11UnorderedAccessView* const NULL_UAVS[MAX_UAVS] = {};
	}

	ShallowWaterSolver::Layer ShallowWaterSolver::CreateLayer(ID3D11Device* device, std::uint32_t a_resolution, DXGI_FORMAT format, const NameFn& name, const char* label, bool unordered)
	{
		D3D11_TEXTURE2D_DESC desc{
			.Width = a_resolution,
			.Height = a_resolution,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = format,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | (unordered ? D3D11_BIND_UNORDERED_ACCESS : 0u),
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};

		Layer layer;
		winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, layer.texture.put()));
		winrt::check_hresult(device->CreateShaderResourceView(layer.texture.get(), nullptr, layer.srv.put()));
		if (unordered)
			winrt::check_hresult(device->CreateUnorderedAccessView(layer.texture.get(), nullptr, layer.uav.put()));

		const std::string resourceName = std::string("FluidField::") + label;
		name(layer.texture.get(), resourceName.c_str());
		name(layer.srv.get(), (resourceName + " SRV").c_str());
		if (unordered)
			name(layer.uav.get(), (resourceName + " UAV").c_str());
		return layer;
	}

	void ShallowWaterSolver::CreateResources(ID3D11Device* device, std::uint32_t a_resolution, std::uint32_t a_mapResolution, std::uint32_t a_obstacleResolution, const NameFn& name)
	{
		ReleaseResources();

		depth[0] = CreateLayer(device, a_resolution, DXGI_FORMAT_R32_FLOAT, name, "WaterDepth0");
		depth[1] = CreateLayer(device, a_resolution, DXGI_FORMAT_R32_FLOAT, name, "WaterDepth1");
		velocity[0] = CreateLayer(device, a_resolution, DXGI_FORMAT_R32G32_FLOAT, name, "WaterVelocity0");
		velocity[1] = CreateLayer(device, a_resolution, DXGI_FORMAT_R32G32_FLOAT, name, "WaterVelocity1");
		cellMap[0] = CreateLayer(device, a_resolution, DXGI_FORMAT_R32G32_FLOAT, name, "WaterCellMap0");
		cellMap[1] = CreateLayer(device, a_resolution, DXGI_FORMAT_R32G32_FLOAT, name, "WaterCellMap1");
		surface = CreateLayer(device, a_resolution, DXGI_FORMAT_R16G16B16A16_FLOAT, name, "WaterSurface");
		flow = CreateLayer(device, a_resolution, DXGI_FORMAT_R16G16_FLOAT, name, "WaterFlow");
		map = CreateLayer(device, a_mapResolution, DXGI_FORMAT_R32G32_FLOAT, name, "WaterMap", false);
		obstacles = CreateLayer(device, a_obstacleResolution, DXGI_FORMAT_R32G32B32A32_FLOAT, name, "WaterObstacles");
		for (std::size_t i = 0; i < debugViews.size(); i++)
			debugViews[i] = CreateLayer(device, a_resolution, DXGI_FORMAT_R8G8B8A8_UNORM, name, (std::string("DebugView") + DEBUG_VIEW_NAMES[i]).c_str());

		D3D11_BUFFER_DESC cbDesc{
			.ByteWidth = sizeof(GridConstants),
			.Usage = D3D11_USAGE_DYNAMIC,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
			.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
			.MiscFlags = 0,
			.StructureByteStride = 0
		};
		winrt::check_hresult(device->CreateBuffer(&cbDesc, nullptr, gridConstants.put()));
		name(gridConstants.get(), "FluidField::WaterGridConstants");
		cbDesc.ByteWidth = sizeof(WaterConstants);
		winrt::check_hresult(device->CreateBuffer(&cbDesc, nullptr, waterConstants.put()));
		name(waterConstants.get(), "FluidField::WaterConstants");
		cbDesc.ByteWidth = sizeof(DebugConstants);
		winrt::check_hresult(device->CreateBuffer(&cbDesc, nullptr, debugConstants.put()));
		name(debugConstants.get(), "FluidField::WaterDebugConstants");

		D3D11_BUFFER_DESC emitterDesc{
			.ByteWidth = sizeof(WaterEmitter) * MAX_EMITTERS,
			.Usage = D3D11_USAGE_DYNAMIC,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE,
			.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
			.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
			.StructureByteStride = sizeof(WaterEmitter)
		};
		winrt::check_hresult(device->CreateBuffer(&emitterDesc, nullptr, emitterBuffer.put()));
		name(emitterBuffer.get(), "FluidField::WaterEmitters");
		D3D11_SHADER_RESOURCE_VIEW_DESC emitterSRVDesc{
			.Format = DXGI_FORMAT_UNKNOWN,
			.ViewDimension = D3D11_SRV_DIMENSION_BUFFER,
			.Buffer = { .FirstElement = 0, .NumElements = MAX_EMITTERS }
		};
		winrt::check_hresult(device->CreateShaderResourceView(emitterBuffer.get(), &emitterSRVDesc, emitterSRV.put()));
		name(emitterSRV.get(), "FluidField::WaterEmitters SRV");

		resolution = a_resolution;
		mapResolution = a_mapResolution;
		obstacleResolution = a_obstacleResolution;
		Reset();
	}

	bool ShallowWaterSolver::CompileShaders(const CompileFn& compile)
	{
		ReleaseShaders();
		const std::pair<winrt::com_ptr<ID3D11ComputeShader>*, const wchar_t*> shaders[] = {
			{ &forcesCS, L"WaterForcesCS.hlsl" },
			{ &advectCS, L"WaterAdvectCS.hlsl" },
			{ &heightCS, L"WaterHeightCS.hlsl" },
			{ &velocityCS, L"WaterVelocityCS.hlsl" },
			{ &outputCS, L"WaterOutputCS.hlsl" },
			{ &debugViewCS, L"WaterDebugViewCS.hlsl" },
		};
		for (auto& [program, fileName] : shaders) {
			if (auto shader = compile(fileName))
				program->attach(shader);
		}
		return HasShaders();
	}

	bool ShallowWaterSolver::HasShaders() const
	{
		return forcesCS && advectCS && heightCS && velocityCS && outputCS && debugViewCS;
	}

	void ShallowWaterSolver::ReleaseShaders()
	{
		forcesCS = nullptr;
		advectCS = nullptr;
		heightCS = nullptr;
		velocityCS = nullptr;
		outputCS = nullptr;
		debugViewCS = nullptr;
	}

	void ShallowWaterSolver::ReleaseResources()
	{
		depth = {};
		velocity = {};
		cellMap = {};
		surface = {};
		flow = {};
		map = {};
		obstacles = {};
		debugViews = {};
		gridConstants = nullptr;
		waterConstants = nullptr;
		debugConstants = nullptr;
		emitterBuffer = nullptr;
		emitterSRV = nullptr;
		resolution = 0;
		mapResolution = 0;
		obstacleResolution = 0;
		Reset();
	}

	void ShallowWaterSolver::Reset()
	{
		// The forces pass starts every cell from vanilla water while history is invalid. The map is
		// forgotten too: after a load it describes the previous location, and starting from it would
		// flood or drain the grid once the new map arrives.
		historyValid = false;
		stepped = false;
		hasDebugViews = false;
		mapUploaded = false;
		obstaclesNeedClear = true;
	}

	void ShallowWaterSolver::ClearObstacles(ID3D11DeviceContext* context)
	{
		if (!obstacles.uav)
			return;
		const float zero[4] = {};
		context->ClearUnorderedAccessViewFloat(obstacles.uav.get(), zero);
		obstaclesNeedClear = false;
	}

	void ShallowWaterSolver::UploadObstacleBlock(ID3D11DeviceContext* context, std::int32_t originX, std::int32_t originY, std::uint32_t size, const float* texels, float a_spacing)
	{
		if (!obstacles.texture || obstacleResolution == 0 || size == 0 || obstacleResolution % size != 0)
			return;
		const auto x = static_cast<std::uint32_t>(originX) & (obstacleResolution - 1);
		const auto y = static_cast<std::uint32_t>(originY) & (obstacleResolution - 1);
		const D3D11_BOX box{ .left = x, .top = y, .front = 0, .right = x + size, .bottom = y + size, .back = 1 };
		context->UpdateSubresource(obstacles.texture.get(), 0, &box, texels, size * sizeof(float) * 4, 0);
		obstacleSpacing = a_spacing;
	}

	void ShallowWaterSolver::UploadMap(ID3D11DeviceContext* context, std::span<const float> bedAndSurface, float a_mapSpacing)
	{
		if (!map.texture || bedAndSurface.size() < static_cast<std::size_t>(mapResolution) * mapResolution * 2)
			return;
		context->UpdateSubresource(map.texture.get(), 0, nullptr, bedAndSurface.data(), mapResolution * sizeof(float) * 2, 0);
		mapSpacing = a_mapSpacing;
		mapUploaded = true;
	}

	void ShallowWaterSolver::UpdateBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer, const void* data, std::size_t size)
	{
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, data, size);
			context->Unmap(buffer, 0);
		}
	}

	void ShallowWaterSolver::BeginStep(ID3D11DeviceContext* context, const StepParams& grid, const WaterParams& params, std::span<const WaterEmitter> emitters)
	{
		const auto emitterCount = static_cast<std::uint32_t>(std::min<std::size_t>(emitters.size(), MAX_EMITTERS));
		const float dt = grid.deltaTime;

		const GridConstants gridData{
			.gridMin = { grid.gridMin[0], grid.gridMin[1] },
			.prevGridMin = { historyValid ? prevGridMin[0] : grid.gridMin[0], historyValid ? prevGridMin[1] : grid.gridMin[1] },
			.gridRes = resolution,
			.cellSize = grid.cellSize,
			.deltaTime = dt,
			.velocityDecay = std::exp(-params.damping * dt),
			.historyValid = historyValid ? 1u : 0u,
			.emitterCount = emitterCount,
			.pad = {}
		};
		UpdateBuffer(context, gridConstants.get(), &gridData, sizeof(gridData));

		const WaterConstants waterData{
			.gravity = params.gravity,
			.maxDepth = params.maxDepth,
			// Keep every face within half a cell per step
			.maxSpeed = dt > 0.0f ? 0.5f * grid.cellSize / dt : 0.0f,
			.wetThreshold = params.wetThreshold,
			.current = { params.current[0], params.current[1] },
			.currentRelax = params.currentRelax,
			.levelRelax = params.levelRelax,
			.mapRes = mapResolution,
			.mapSpacing = mapSpacing,
			.edgeWidth = params.edgeWidth,
			.edgeRelax = params.edgeRelax,
			.obstacleRes = obstacleResolution,
			.obstacleSpacing = obstacleSpacing,
			.riverSpeed = params.riverSpeed,
			.flowmapEnabled = params.flowmap ? 1u : 0u,
			.flowmapCells = { params.flowmapCells[0], params.flowmapCells[1] },
			.flowmapOffset = { params.flowmapOffset[0], params.flowmapOffset[1] },
			.vorticity = params.vorticity,
			.pad1 = {}
		};
		UpdateBuffer(context, waterConstants.get(), &waterData, sizeof(waterData));

		if (emitterCount > 0)
			UpdateBuffer(context, emitterBuffer.get(), emitters.data(), sizeof(WaterEmitter) * emitterCount);

		prevGridMin[0] = grid.gridMin[0];
		prevGridMin[1] = grid.gridMin[1];
		historyValid = true;
		stepped = true;

		ID3D11Buffer* cbs[] = { gridConstants.get(), waterConstants.get() };
		context->CSSetConstantBuffers(0, 2, cbs);
		if (obstaclesNeedClear)
			ClearObstacles(context);
		ID3D11ShaderResourceView* mapSRVs[] = { map.srv.get(), obstacles.srv.get(), params.flowmap };
		context->CSSetShaderResources(MAP_SLOT, 3, mapSRVs);
	}

	void ShallowWaterSolver::EndStep(ID3D11DeviceContext* context)
	{
		ID3D11Buffer* nullCBs[2] = {};
		context->CSSetShaderResources(0, BOUND_SRVS, NULL_SRVS);
		context->CSSetUnorderedAccessViews(0, MAX_UAVS, NULL_UAVS, nullptr);
		context->CSSetConstantBuffers(0, 2, nullCBs);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	void ShallowWaterSolver::Dispatch(ID3D11DeviceContext* context, ID3D11ComputeShader* shader, std::span<ID3D11ShaderResourceView* const> srvs, std::span<ID3D11UnorderedAccessView* const> uavs)
	{
		// Unbind the previous pass's outputs before they are read, or D3D11 silently nulls the new SRVs
		context->CSSetUnorderedAccessViews(0, MAX_UAVS, NULL_UAVS, nullptr);

		ID3D11ShaderResourceView* boundSRVs[MAX_SRVS] = {};
		std::copy_n(srvs.begin(), std::min<std::size_t>(srvs.size(), MAX_SRVS), boundSRVs);
		context->CSSetShaderResources(0, MAX_SRVS, boundSRVs);

		ID3D11UnorderedAccessView* boundUAVs[MAX_UAVS] = {};
		std::copy_n(uavs.begin(), std::min<std::size_t>(uavs.size(), MAX_UAVS), boundUAVs);
		context->CSSetUnorderedAccessViews(0, MAX_UAVS, boundUAVs, nullptr);
		context->CSSetShader(shader, nullptr, 0);

		const std::uint32_t groups = (resolution + GROUP_SIZE - 1) / GROUP_SIZE;
		context->Dispatch(groups, groups, 1);
	}

	void ShallowWaterSolver::Forces(ID3D11DeviceContext* context)
	{
		ID3D11ShaderResourceView* srvs[] = { depth[currentDepth].srv.get(), velocity[currentVelocity].srv.get(), cellMap[currentCellMap].srv.get(), emitterSRV.get() };
		ID3D11UnorderedAccessView* uavs[] = { depth[currentDepth ^ 1].uav.get(), velocity[currentVelocity ^ 1].uav.get(), cellMap[currentCellMap ^ 1].uav.get() };
		Dispatch(context, forcesCS.get(), srvs, uavs);
		currentDepth ^= 1;
		currentVelocity ^= 1;
		currentCellMap ^= 1;
	}

	void ShallowWaterSolver::Advect(ID3D11DeviceContext* context)
	{
		ID3D11ShaderResourceView* srvs[] = { velocity[currentVelocity].srv.get() };
		ID3D11UnorderedAccessView* uavs[] = { velocity[currentVelocity ^ 1].uav.get() };
		Dispatch(context, advectCS.get(), srvs, uavs);
		currentVelocity ^= 1;
	}

	void ShallowWaterSolver::Height(ID3D11DeviceContext* context)
	{
		ID3D11ShaderResourceView* srvs[] = { depth[currentDepth].srv.get(), velocity[currentVelocity].srv.get() };
		ID3D11UnorderedAccessView* uavs[] = { depth[currentDepth ^ 1].uav.get() };
		Dispatch(context, heightCS.get(), srvs, uavs);
		currentDepth ^= 1;
	}

	void ShallowWaterSolver::Velocity(ID3D11DeviceContext* context)
	{
		ID3D11ShaderResourceView* srvs[] = { depth[currentDepth].srv.get(), velocity[currentVelocity].srv.get(), cellMap[currentCellMap].srv.get() };
		ID3D11UnorderedAccessView* uavs[] = { velocity[currentVelocity ^ 1].uav.get() };
		Dispatch(context, velocityCS.get(), srvs, uavs);
		currentVelocity ^= 1;
	}

	void ShallowWaterSolver::WriteOutputs(ID3D11DeviceContext* context)
	{
		if (!HasResources() || !outputCS || !stepped)
			return;

		ID3D11Buffer* cbs[] = { gridConstants.get(), waterConstants.get() };
		context->CSSetConstantBuffers(0, 2, cbs);
		ID3D11ShaderResourceView* srvs[] = { depth[currentDepth].srv.get(), velocity[currentVelocity].srv.get(), cellMap[currentCellMap].srv.get() };
		ID3D11UnorderedAccessView* uavs[] = { surface.uav.get(), flow.uav.get() };
		Dispatch(context, outputCS.get(), srvs, uavs);
		EndStep(context);
	}

	void ShallowWaterSolver::RenderDebugViews(ID3D11DeviceContext* context, const WaterDebugScales& scales)
	{
		if (!HasResources() || !debugViewCS || !stepped)
			return;

		const DebugConstants data{ scales.depth, scales.flow, scales.wave, 0.0f };
		UpdateBuffer(context, debugConstants.get(), &data, sizeof(data));

		ID3D11Buffer* cbs[] = { gridConstants.get(), waterConstants.get(), debugConstants.get() };
		context->CSSetConstantBuffers(0, 3, cbs);
		ID3D11ShaderResourceView* srvs[] = { depth[currentDepth].srv.get(), surface.srv.get(), flow.srv.get() };
		ID3D11UnorderedAccessView* uavs[] = { debugViews[0].uav.get(), debugViews[1].uav.get(), debugViews[2].uav.get() };
		Dispatch(context, debugViewCS.get(), srvs, uavs);

		ID3D11Buffer* nullCBs[3] = {};
		context->CSSetConstantBuffers(0, 3, nullCBs);
		EndStep(context);
		hasDebugViews = true;
	}
}
