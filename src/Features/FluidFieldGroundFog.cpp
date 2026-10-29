#include "FluidFieldGroundFog.h"

#include "../I18n/I18n.h"
#include "FluidField/Field.h"
#include "Menu.h"
#include "Profiler.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

#include <numbers>

#define I18N_KEY_PREFIX "feature.fluid_field_ground_fog."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FluidFieldGroundFog::Settings,
	Enabled,
	FogDensity,
	FogBrightness,
	FogThickness,
	FogStir,
	FogRefillSeconds,
	GridResolution,
	GridSize,
	JacobiIterations,
	WindDamping)

void FluidFieldGroundFog::DrawSettings()
{
	const auto tooltip = [](const char* a_text) {
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", a_text);
	};
	const auto errorText = [](const char* a_text) { ImGui::TextColored(Menu::GetSingleton()->GetTheme().StatusPalette.Error, "%s", a_text); };

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	tooltip(T(TKEY("enabled_tooltip"), "Fog lying on the ground around you, in banks and clearings. It parts and swirls around moving actors and drifts with the weather's wind."));
	if (shaderCompileFailed)
		errorText(T(TKEY("shader_compile_failed"), "Ground Fog shaders failed to compile, so the fog is off. Check CommunityShaders.log."));
	if (resourceCreateFailed)
		errorText(T(TKEY("resource_create_failed"), "Ground Fog could not create its textures, so the fog is off. Check CommunityShaders.log."));

	ImGui::SliderFloat(T(TKEY("fog_density"), "Density"), &settings.FogDensity, 0.0f, 0.2f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("fog_density_tooltip"), "How thick the fog is inside its layer, in its banks."));
	ImGui::SliderFloat(T(TKEY("fog_brightness"), "Brightness"), &settings.FogBrightness, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("fog_brightness_tooltip"), "How brightly the fog is lit by the sky and the sun."));
	ImGui::SliderFloat(T(TKEY("fog_thickness"), "Thickness"), &settings.FogThickness, 5.0f, 400.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("fog_thickness_tooltip"), "How high the fog layer reaches above the ground, in world units. Thinner patches lie lower."));
	ImGui::SliderFloat(T(TKEY("fog_stir"), "Stirring"), &settings.FogStir, 0.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("fog_stir_tooltip"), "How hard actors shove the fog aside and push the air, piling it around them and swirling it behind them."));
	ImGui::SliderFloat(T(TKEY("fog_refill"), "Refill"), &settings.FogRefillSeconds, 0.5f, 30.0f, "%.1f s", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("fog_refill_tooltip"), "How long shoved fog takes to settle back."));

	// Quality: the wind grid the fog rides on
	if (ImGui::CollapsingHeader(T(TKEY("quality"), "Quality"))) {
		ImGui::PushID("QualitySection");
		ImGui::SeparatorText(T(TKEY("wind"), "Wind and Fog Grid"));
		const auto resolutionLabel = std::format("{0} x {0}", settings.GridResolution);
		if (ImGui::BeginCombo(T(TKEY("grid_resolution"), "Resolution"), resolutionLabel.c_str())) {
			for (const uint resolution : FluidFieldSim::Field::GRID_RESOLUTIONS) {
				const auto label = std::format("{0} x {0}", resolution);
				if (ImGui::Selectable(label.c_str(), resolution == settings.GridResolution)) {
					settings.GridResolution = resolution;
					resourceCreateFailed = false;
				}
			}
			ImGui::EndCombo();
		}
		tooltip(T(TKEY("grid_resolution_tooltip"), "Texels per side. Higher values show finer detail and cost more GPU time. Changing it clears the field."));
		ImGui::SliderFloat(T(TKEY("grid_size"), "Size"), &settings.GridSize, FluidFieldSim::Field::MIN_GRID_SIZE, FluidFieldSim::Field::MAX_GRID_SIZE, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("grid_size_tooltip"), "Width of the area simulated around the player, in game units. Changing it clears the field."));
		ImGui::Text(T(TKEY("cell_size"), "Cell size: %.1f units"), GetCellSize());
		ImGui::SliderScalar(T(TKEY("jacobi_iterations"), "Pressure Iterations"), ImGuiDataType_U32, &settings.JacobiIterations, &MIN_JACOBI_ITERATIONS, &MAX_JACOBI_ITERATIONS, "%u", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("jacobi_iterations_tooltip"), "More iterations make the wind swirl more accurately and cost more GPU time."));
		ImGui::SliderFloat(T(TKEY("wind_damping"), "Wind Damping"), &settings.WindDamping, 0.0f, 5.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("wind_damping_tooltip"), "How quickly wind dies down. Higher values calm it sooner."));
		if (ImGui::Button(T(TKEY("reset_field"), "Reset Field")))
			queuedReset = true;
		ImGui::PopID();
	}
}

#undef I18N_KEY_PREFIX

FluidFieldGroundFog::Settings FluidFieldGroundFog::Sanitize(Settings a_settings)
{
	using FluidFieldSim::Field;
	const Settings defaults{};

	if (std::ranges::find(Field::GRID_RESOLUTIONS, a_settings.GridResolution) == Field::GRID_RESOLUTIONS.end())
		a_settings.GridResolution = defaults.GridResolution;

	a_settings.GridSize = std::isfinite(a_settings.GridSize) ? std::clamp(a_settings.GridSize, Field::MIN_GRID_SIZE, Field::MAX_GRID_SIZE) : defaults.GridSize;
	a_settings.JacobiIterations = std::clamp(a_settings.JacobiIterations, MIN_JACOBI_ITERATIONS, MAX_JACOBI_ITERATIONS);
	a_settings.WindDamping = std::isfinite(a_settings.WindDamping) ? std::clamp(a_settings.WindDamping, 0.0f, 5.0f) : defaults.WindDamping;
	a_settings.FogDensity = std::isfinite(a_settings.FogDensity) ? std::clamp(a_settings.FogDensity, 0.0f, 0.2f) : defaults.FogDensity;
	a_settings.FogBrightness = std::isfinite(a_settings.FogBrightness) ? std::clamp(a_settings.FogBrightness, 0.0f, 2.0f) : defaults.FogBrightness;
	a_settings.FogThickness = std::isfinite(a_settings.FogThickness) ? std::clamp(a_settings.FogThickness, 5.0f, 400.0f) : defaults.FogThickness;
	a_settings.FogStir = std::isfinite(a_settings.FogStir) ? std::clamp(a_settings.FogStir, 0.0f, 3.0f) : defaults.FogStir;
	a_settings.FogRefillSeconds = std::isfinite(a_settings.FogRefillSeconds) ? std::clamp(a_settings.FogRefillSeconds, 0.5f, 30.0f) : defaults.FogRefillSeconds;
	return a_settings;
}

void FluidFieldGroundFog::LoadSettings(json& o_json)
{
	try {
		settings = o_json;
	} catch (const json::exception& e) {
		logger::warn("[Fluid Field] Malformed Ground Fog settings, using defaults: {}", e.what());
		settings = {};
	}
	settings = Sanitize(settings);
}

void FluidFieldGroundFog::SaveSettings(json& o_json)
{
	o_json = settings;
}

void FluidFieldGroundFog::RestoreDefaultSettings()
{
	settings = {};
}

void FluidFieldGroundFog::PostPostLoad()
{
	FluidFieldSim::Field::Get().RegisterListeners();
}

void FluidFieldGroundFog::ClearShaderCache()
{
	solver.ReleaseShaders();
	shaderCompileFailed = false;
	groundFogCS = nullptr;
	groundFogFailed = false;
}

bool FluidFieldGroundFog::EnsureSolver()
{
	if (shaderCompileFailed || resourceCreateFailed)
		return false;

	if (solver.GetResolution() != settings.GridResolution) {
		try {
			solver.CreateResources(globals::d3d::device, settings.GridResolution, [](ID3D11DeviceChild* object, const char* name) {
				Util::SetResourceName(object, "%s", name);
			});
		} catch (const winrt::hresult_error& e) {
			logger::error("[Fluid Field] Failed to create GPU resources: {}", winrt::to_string(e.message()));
			solver.ReleaseResources();
			resourceCreateFailed = true;
			return false;
		}
		// New textures hold undefined data
		queuedReset = true;
	}

	if (!solver.HasShaders()) {
		const bool compiled = solver.CompileShaders([](const wchar_t* fileName) {
			const auto path = std::filesystem::path("Data\\Shaders\\FluidField") / fileName;
			return reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), {}, "cs_5_0"));
		});
		if (!compiled) {
			logger::error("[Fluid Field] Compute shaders failed to compile; the simulation stays off");
			solver.ReleaseShaders();
			shaderCompileFailed = true;
			return false;
		}
	}

	return true;
}

void FluidFieldGroundFog::Release()
{
	solver.ReleaseResources();
	solver.ReleaseShaders();
	lastCellSize = 0.0f;
	groundHeight.Release();
	groundFogCS = nullptr;
	groundFogCB = nullptr;
	fogReady = false;
}

void FluidFieldGroundFog::Prepass()
{
	auto& field = FluidFieldSim::Field::Get();
	if (!settings.Enabled) {
		// Disabled costs nothing: released once, then no textures, no shaders, no dispatches
		if (!released) {
			Release();
			released = true;
		}
		field.ReleaseIfUnused();
		return;
	}
	released = false;

	const FluidFieldSim::Field::PrepassScope scope(*this);
	if (!scope.IsReady())
		return;

	// Every load leaves the player somewhere new; start the wind from rest
	if (field.ConsumeLoad(seenLoads))
		queuedReset = true;

	if (!EnsureSolver())
		return;

	auto context = globals::d3d::context;

	const float cellSize = GetCellSize();
	if (cellSize != lastCellSize) {
		// Grid coordinates are in cells, so content from another cell size is meaningless
		queuedReset = true;
		lastCellSize = cellSize;
	}

	if (queuedReset.exchange(false)) {
		ZoneScopedN("FluidField - Reset");
		TracyD3D11Zone(globals::state->tracyCtx, "FluidField - Reset");
		globals::profiler->BeginPass("FluidField::Reset");
		solver.Reset(context);
		airSources.Clear();
		globals::profiler->EndPass();
	}

	const float deltaTime = field.GetDeltaTime();
	if (!(deltaTime > 0.0f))
		return;

	// Centre the grid on the player, snapped to whole cells so texels never resample
	const auto playerPos = field.GetPlayerPosition();
	const int32_t halfRes = static_cast<int32_t>(settings.GridResolution / 2);
	const int32_t gridMin[2] = {
		static_cast<int32_t>(std::floor(playerPos.x / cellSize)) - halfRes,
		static_cast<int32_t>(std::floor(playerPos.y / cellSize)) - halfRes
	};

	// Moving actors push the air (D21), one slot short of the most
	const std::vector<FluidFieldSim::Emitter>& windEmitters = airSources.Update(playerPos, gridMin, cellSize, settings.GridResolution, deltaTime, settings.FogStir,
		FluidFieldSim::WindSolver::MAX_EMITTERS - 1);

	// The fog: carried by the wind and the weather's own wind, refilling behind actors
	fogParams = {};
	fogParams.refillRate = 1.0f / settings.FogRefillSeconds;
	fogParams.stirRate *= settings.FogStir;
	fogParams.displaceSpeed *= settings.FogStir;
	fogPatternOffset[0] += fogParams.drift[0] * deltaTime;
	fogPatternOffset[1] += fogParams.drift[1] * deltaTime;
	fogParams.patternOffset[0] = fogPatternOffset[0];
	fogParams.patternOffset[1] = fogPatternOffset[1];
	if (const auto* sky = RE::Sky::GetSingleton(); sky && sky->currentWeather) {
		// Wind speed and direction are stored as 0-255; direction 0 blows from the north
		const float speed = static_cast<float>(sky->currentWeather->data.windSpeed) / 255.0f * MAX_WEATHER_DRIFT;
		const float heading = static_cast<float>(sky->currentWeather->data.windDirection) / 256.0f * 2.0f * std::numbers::pi_v<float>;
		fogParams.drift[0] = -std::sin(heading) * speed;
		fogParams.drift[1] = -std::cos(heading) * speed;
	}

	FluidFieldSim::StepParams params{
		.gridMin = { gridMin[0], gridMin[1] },
		.cellSize = cellSize,
		.deltaTime = deltaTime,
		.velocityDecay = std::exp(-settings.WindDamping * deltaTime),
		.jacobiIterations = settings.JacobiIterations,
		.emitters = windEmitters,
		.fog = &fogParams
	};

	{
		ZoneScopedN("FluidField - Wind Step");
		TracyD3D11Zone(globals::state->tracyCtx, "FluidField - Wind Step");
		solver.Step(context, params, [&](const char* passName, auto&& runPass) {
#ifdef TRACY_ENABLE
			ZoneTransientN(fluidFieldPassZone, passName, true);
			TracyD3D11ZoneTransientS(globals::state->tracyCtx, fluidFieldPassGpuZone, passName, 0, globals::state->tracyCtx != nullptr);
#endif
			// The built-in profiler cannot nest passes, so each solver pass is timed on its own
			globals::profiler->BeginPass(std::format("FluidField::{}", passName));
			runPass();
			globals::profiler->EndPass();
		});
	}

	// The ground the fog lies on, wider than the simulated area
	fogReady = false;
	globals::profiler->BeginPass("FluidField::Fog Ground");
	const int32_t groundHalf = static_cast<int32_t>(FOG_GROUND_RESOLUTION / 2);
	fogGroundMin[0] = static_cast<int32_t>(std::floor(playerPos.x / FOG_GROUND_CELL)) - groundHalf;
	fogGroundMin[1] = static_cast<int32_t>(std::floor(playerPos.y / FOG_GROUND_CELL)) - groundHalf;
	fogReady = groundHeight.Update(globals::d3d::device, context, field.GetLandscape(), field.GetWater(), fogGroundMin, FOG_GROUND_RESOLUTION, FOG_GROUND_CELL);
	globals::profiler->EndPass();
	fogGridMin[0] = gridMin[0];
	fogGridMin[1] = gridMin[1];
}

void FluidFieldGroundFog::DrawAfterWorld()
{
	if (!fogReady)
		return;
	auto* renderer = globals::game::renderer;
	// The targets the world was just rendered into; not always kMAIN, so the fog follows whatever is bound
	const auto& shadow = globals::game::shadowState->GetRuntimeData();
	const auto colorIndex = static_cast<uint32_t>(shadow.renderTargets[0]);
	const auto depthIndex = shadow.depthStencil;
	if (colorIndex >= RE::RENDER_TARGET::kTOTAL || depthIndex >= RE::RENDER_TARGET_DEPTHSTENCIL::kTOTAL)
		return;
	auto& main = renderer->GetRuntimeData().renderTargets[colorIndex];
	auto& depth = renderer->GetDepthStencilData().depthStencils[depthIndex];
	if (!main.UAV || !depth.depthSRV)
		return;

	// The live depth holds the water surfaces; it can only be read once it is no longer bound
	globals::d3d::context->OMSetRenderTargets(0, nullptr, nullptr);
	DrawGroundFog(main.UAV, depth.depthSRV);
	globals::game::shadowState->GetRuntimeData().stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void FluidFieldGroundFog::DrawGroundFog(ID3D11UnorderedAccessView* a_main, ID3D11ShaderResourceView* a_depth)
{
	if (!IsActive() || !fogReady || settings.FogDensity <= 0.0f || !a_main || !a_depth || groundFogFailed)
		return;
	if (Util::IsInterior() || !solver.HasResources() || !groundHeight.GetSRV())
		return;

	auto* context = globals::d3d::context;
	if (!groundFogCS) {
		groundFogCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\GroundFogCS.hlsl", {}, "cs_5_0")));
		D3D11_BUFFER_DESC desc{ .ByteWidth = sizeof(GroundFogConstants), .Usage = D3D11_USAGE_DYNAMIC, .BindFlags = D3D11_BIND_CONSTANT_BUFFER, .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE, .MiscFlags = 0, .StructureByteStride = 0 };
		if (!groundFogCS || FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, groundFogCB.put()))) {
			logger::error("[Fluid Field] Ground fog shader failed to compile; the ground fog stays off");
			groundFogCS = nullptr;
			groundFogFailed = true;
			return;
		}
		Util::SetResourceName(groundFogCB.get(), "FluidField::GroundFogConstants");
	}

	const float cellSize = GetCellSize();
	const GroundFogConstants constants{
		.gridMin = { fogGridMin[0], fogGridMin[1] },
		.gridRes = settings.GridResolution,
		.cellSize = cellSize,
		.density = settings.FogDensity,
		.thickness = settings.FogThickness,
		.maxHeight = groundHeight.GetMaxHeight(),
		.stepCount = GROUND_FOG_STEPS,
		.albedo = { 0.9f * settings.FogBrightness, 0.92f * settings.FogBrightness, 0.95f * settings.FogBrightness },
		.maxDistance = 0.5f * FOG_GROUND_CELL * static_cast<float>(FOG_GROUND_RESOLUTION),
		.groundMin = { fogGroundMin[0], fogGroundMin[1] },
		.groundRes = FOG_GROUND_RESOLUTION,
		.groundCellSize = FOG_GROUND_CELL,
		.patternOffset = { fogPatternOffset[0], fogPatternOffset[1] },
		.debugMode = 0,
		.pad = 0.0f
	};
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(groundFogCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return;
	std::memcpy(mapped.pData, &constants, sizeof(constants));
	context->Unmap(groundFogCB.get(), 0);

	globals::profiler->BeginPass("FluidField::Ground Fog");
	ID3D11Buffer* cb = groundFogCB.get();
	ID3D11ShaderResourceView* srvs[] = { a_depth, solver.GetFogSRV(), groundHeight.GetSRV(), solver.GetVelocitySRV() };
	context->CSSetConstantBuffers(0, 1, &cb);
	// Bind the camera (b12) and shared/feature data (b5, b6) explicitly: earlier passes unbind or replace them
	auto* cameraSnapshot = FluidFieldSim::Field::Get().GetCameraSnapshot();
	ID3D11Buffer* perFrame = cameraSnapshot ? cameraSnapshot : *globals::game::perFrame;
	context->CSSetConstantBuffers(12, 1, &perFrame);
	ID3D11Buffer* sharedBuffers[2]{ globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->CSSetConstantBuffers(5, 2, sharedBuffers);
	context->CSSetShaderResources(0, 4, srvs);
	context->CSSetUnorderedAccessViews(0, 1, &a_main, nullptr);
	context->CSSetShader(groundFogCS.get(), nullptr, 0);
	const auto dispatch = Util::GetScreenDispatchCount(true);
	context->Dispatch(dispatch.x, dispatch.y, 1);

	ID3D11ShaderResourceView* nullSRVs[4]{};
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ID3D11Buffer* nullCB = nullptr;
	context->CSSetShaderResources(0, 4, nullSRVs);
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	context->CSSetConstantBuffers(0, 1, &nullCB);
	context->CSSetConstantBuffers(12, 1, &nullCB);
	context->CSSetShader(nullptr, nullptr, 0);
	globals::profiler->EndPass();
}
