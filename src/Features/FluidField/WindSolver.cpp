#include "WindSolver.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace FluidFieldSim
{
	namespace
	{
		constexpr std::uint32_t GROUP_SIZE = 8;  // FLUIDFIELD_GROUP_SIZE in Common.hlsli
		constexpr std::uint32_t MAX_SRVS = 3;

		ID3D11ShaderResourceView* const NULL_SRVS[MAX_SRVS] = {};
		ID3D11UnorderedAccessView* const NULL_UAVS[3] = {};

		void NameView(const WindSolver::NameFn& name, ID3D11DeviceChild* view, const std::string& resourceName, const char* suffix)
		{
			name(view, (resourceName + suffix).c_str());
		}
	}

	WindSolver::Layer WindSolver::CreateLayer(ID3D11Device* device, std::uint32_t a_resolution, DXGI_FORMAT format, const NameFn& name, const char* label)
	{
		D3D11_TEXTURE2D_DESC desc{
			.Width = a_resolution,
			.Height = a_resolution,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = format,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};

		Layer layer;
		winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, layer.texture.put()));
		winrt::check_hresult(device->CreateShaderResourceView(layer.texture.get(), nullptr, layer.srv.put()));
		winrt::check_hresult(device->CreateUnorderedAccessView(layer.texture.get(), nullptr, layer.uav.put()));

		const std::string resourceName = std::string("FluidField::") + label;
		name(layer.texture.get(), resourceName.c_str());
		NameView(name, layer.srv.get(), resourceName, " SRV");
		NameView(name, layer.uav.get(), resourceName, " UAV");
		return layer;
	}

	void WindSolver::CreateResources(ID3D11Device* device, std::uint32_t a_resolution, const NameFn& name)
	{
		ReleaseResources();

		velocity[0] = CreateLayer(device, a_resolution, DXGI_FORMAT_R32G32_FLOAT, name, "Velocity0");
		velocity[1] = CreateLayer(device, a_resolution, DXGI_FORMAT_R32G32_FLOAT, name, "Velocity1");
		pressure[0] = CreateLayer(device, a_resolution, DXGI_FORMAT_R32_FLOAT, name, "Pressure0");
		pressure[1] = CreateLayer(device, a_resolution, DXGI_FORMAT_R32_FLOAT, name, "Pressure1");
		divergence = CreateLayer(device, a_resolution, DXGI_FORMAT_R32_FLOAT, name, "Divergence");
		for (std::size_t i = 0; i < debugViews.size(); i++)
			debugViews[i] = CreateLayer(device, a_resolution, DXGI_FORMAT_R8G8B8A8_UNORM, name, (std::string("DebugView") + DEBUG_VIEW_NAMES[i]).c_str());

		D3D11_BUFFER_DESC cbDesc{
			.ByteWidth = sizeof(Constants),
			.Usage = D3D11_USAGE_DYNAMIC,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
			.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
			.MiscFlags = 0,
			.StructureByteStride = 0
		};
		winrt::check_hresult(device->CreateBuffer(&cbDesc, nullptr, constants.put()));
		name(constants.get(), "FluidField::Constants");

		cbDesc.ByteWidth = sizeof(DebugConstants);
		winrt::check_hresult(device->CreateBuffer(&cbDesc, nullptr, debugConstants.put()));
		name(debugConstants.get(), "FluidField::DebugViewConstants");

		D3D11_BUFFER_DESC emitterDesc{
			.ByteWidth = sizeof(Emitter) * MAX_EMITTERS,
			.Usage = D3D11_USAGE_DYNAMIC,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE,
			.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
			.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
			.StructureByteStride = sizeof(Emitter)
		};
		winrt::check_hresult(device->CreateBuffer(&emitterDesc, nullptr, emitterBuffer.put()));
		name(emitterBuffer.get(), "FluidField::Emitters");

		D3D11_SHADER_RESOURCE_VIEW_DESC emitterSRVDesc{
			.Format = DXGI_FORMAT_UNKNOWN,
			.ViewDimension = D3D11_SRV_DIMENSION_BUFFER,
			.Buffer = { .FirstElement = 0, .NumElements = MAX_EMITTERS }
		};
		winrt::check_hresult(device->CreateShaderResourceView(emitterBuffer.get(), &emitterSRVDesc, emitterSRV.put()));
		name(emitterSRV.get(), "FluidField::Emitters SRV");

		resolution = a_resolution;
		current = 0;
		pressureResult = 0;
		historyValid = false;
	}

	bool WindSolver::CompileShaders(const CompileFn& compile)
	{
		ReleaseShaders();

		const std::pair<winrt::com_ptr<ID3D11ComputeShader>*, const wchar_t*> shaders[] = {
			{ &addForcesCS, L"AddForcesCS.hlsl" },
			{ &advectCS, L"AdvectCS.hlsl" },
			{ &divergenceCS, L"DivergenceCS.hlsl" },
			{ &jacobiCS, L"JacobiCS.hlsl" },
			{ &projectCS, L"ProjectCS.hlsl" },
			{ &debugViewCS, L"DebugViewCS.hlsl" },
		};

		for (auto& [program, fileName] : shaders) {
			if (auto shader = compile(fileName))
				program->attach(shader);
		}

		return HasShaders();
	}

	bool WindSolver::HasShaders() const
	{
		return addForcesCS && advectCS && divergenceCS && jacobiCS && projectCS && debugViewCS;
	}

	void WindSolver::ReleaseShaders()
	{
		addForcesCS = nullptr;
		advectCS = nullptr;
		divergenceCS = nullptr;
		jacobiCS = nullptr;
		projectCS = nullptr;
		debugViewCS = nullptr;
	}

	void WindSolver::ReleaseResources()
	{
		velocity = {};
		pressure = {};
		divergence = {};
		debugViews = {};
		constants = nullptr;
		debugConstants = nullptr;
		emitterBuffer = nullptr;
		emitterSRV = nullptr;
		resolution = 0;
		historyValid = false;
	}

	void WindSolver::Reset(ID3D11DeviceContext* context)
	{
		if (!HasResources())
			return;

		const float zero[4] = {};
		for (auto* layer : { &velocity[0], &velocity[1], &pressure[0], &pressure[1], &divergence })
			context->ClearUnorderedAccessViewFloat(layer->uav.get(), zero);
		for (auto& view : debugViews)
			context->ClearUnorderedAccessViewFloat(view.uav.get(), zero);

		historyValid = false;
	}

	void WindSolver::BeginStep(ID3D11DeviceContext* context, const StepParams& params)
	{
		const auto emitterCount = static_cast<std::uint32_t>(std::min<std::size_t>(params.emitters.size(), MAX_EMITTERS));

		Constants data{
			.gridMin = { params.gridMin[0], params.gridMin[1] },
			.prevGridMin = { historyValid ? prevGridMin[0] : params.gridMin[0], historyValid ? prevGridMin[1] : params.gridMin[1] },
			.gridRes = resolution,
			.cellSize = params.cellSize,
			.deltaTime = params.deltaTime,
			.velocityDecay = params.velocityDecay,
			.historyValid = historyValid ? 1u : 0u,
			.emitterCount = emitterCount,
			.pad = {}
		};

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, &data, sizeof(data));
			context->Unmap(constants.get(), 0);
		}

		if (emitterCount > 0 && SUCCEEDED(context->Map(emitterBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, params.emitters.data(), sizeof(Emitter) * emitterCount);
			context->Unmap(emitterBuffer.get(), 0);
		}

		prevGridMin[0] = params.gridMin[0];
		prevGridMin[1] = params.gridMin[1];
		historyValid = true;

		ID3D11Buffer* cb = constants.get();
		context->CSSetConstantBuffers(0, 1, &cb);
	}

	void WindSolver::EndStep(ID3D11DeviceContext* context)
	{
		ID3D11Buffer* nullCB = nullptr;
		context->CSSetShaderResources(0, MAX_SRVS, NULL_SRVS);
		context->CSSetUnorderedAccessViews(0, 1, NULL_UAVS, nullptr);
		context->CSSetConstantBuffers(0, 1, &nullCB);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	void WindSolver::Dispatch(ID3D11DeviceContext* context, ID3D11ComputeShader* shader, std::span<ID3D11ShaderResourceView* const> srvs, ID3D11UnorderedAccessView* uav)
	{
		// Unbind the previous pass's output before it is read, or D3D11 silently nulls the new SRV
		context->CSSetUnorderedAccessViews(0, 1, NULL_UAVS, nullptr);

		ID3D11ShaderResourceView* boundSRVs[MAX_SRVS] = {};
		std::copy_n(srvs.begin(), std::min<std::size_t>(srvs.size(), MAX_SRVS), boundSRVs);
		context->CSSetShaderResources(0, MAX_SRVS, boundSRVs);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(shader, nullptr, 0);

		const std::uint32_t groups = (resolution + GROUP_SIZE - 1) / GROUP_SIZE;
		context->Dispatch(groups, groups, 1);
	}

	void WindSolver::AddForces(ID3D11DeviceContext* context)
	{
		ID3D11ShaderResourceView* srvs[] = { velocity[current].srv.get(), emitterSRV.get() };
		Dispatch(context, addForcesCS.get(), srvs, velocity[current ^ 1].uav.get());
		current ^= 1;
	}

	void WindSolver::Advect(ID3D11DeviceContext* context)
	{
		ID3D11ShaderResourceView* srvs[] = { velocity[current].srv.get() };
		Dispatch(context, advectCS.get(), srvs, velocity[current ^ 1].uav.get());
		current ^= 1;
	}

	void WindSolver::Divergence(ID3D11DeviceContext* context)
	{
		ID3D11ShaderResourceView* srvs[] = { velocity[current].srv.get() };
		Dispatch(context, divergenceCS.get(), srvs, divergence.uav.get());
	}

	void WindSolver::SolvePressure(ID3D11DeviceContext* context, std::uint32_t iterations)
	{
		// Start from zero every step, so pressure never carries data across a scroll
		context->CSSetUnorderedAccessViews(0, 1, NULL_UAVS, nullptr);
		const float zero[4] = {};
		context->ClearUnorderedAccessViewFloat(pressure[0].uav.get(), zero);

		std::uint32_t source = 0;
		for (std::uint32_t i = 0; i < iterations; i++) {
			ID3D11ShaderResourceView* srvs[] = { pressure[source].srv.get(), divergence.srv.get() };
			Dispatch(context, jacobiCS.get(), srvs, pressure[source ^ 1].uav.get());
			source ^= 1;
		}
		pressureResult = source;
	}

	void WindSolver::Project(ID3D11DeviceContext* context)
	{
		ID3D11ShaderResourceView* srvs[] = { velocity[current].srv.get(), pressure[pressureResult].srv.get() };
		Dispatch(context, projectCS.get(), srvs, velocity[current ^ 1].uav.get());
		current ^= 1;
	}

	void WindSolver::RenderDebugViews(ID3D11DeviceContext* context, const DebugViewScales& scales)
	{
		// Without a step since the last reset, b0 holds no grid placement and the layers are already clear
		if (!HasResources() || !debugViewCS || !historyValid)
			return;

		const DebugConstants data{ scales.velocity, scales.divergence, scales.pressure, 0.0f };
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(debugConstants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, &data, sizeof(data));
			context->Unmap(debugConstants.get(), 0);
		}

		// b0 still holds the last step's grid placement
		ID3D11Buffer* cbs[] = { constants.get(), debugConstants.get() };
		ID3D11ShaderResourceView* srvs[MAX_SRVS] = { velocity[current].srv.get(), divergence.srv.get(), pressure[pressureResult].srv.get() };
		ID3D11UnorderedAccessView* uavs[] = { debugViews[0].uav.get(), debugViews[1].uav.get(), debugViews[2].uav.get() };

		context->CSSetConstantBuffers(0, 2, cbs);
		context->CSSetShaderResources(0, MAX_SRVS, srvs);
		context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
		context->CSSetShader(debugViewCS.get(), nullptr, 0);

		const std::uint32_t groups = (resolution + GROUP_SIZE - 1) / GROUP_SIZE;
		context->Dispatch(groups, groups, 1);

		ID3D11Buffer* nullCBs[2] = {};
		context->CSSetShaderResources(0, MAX_SRVS, NULL_SRVS);
		context->CSSetUnorderedAccessViews(0, 3, NULL_UAVS, nullptr);
		context->CSSetConstantBuffers(0, 2, nullCBs);
		context->CSSetShader(nullptr, nullptr, 0);
	}
}
