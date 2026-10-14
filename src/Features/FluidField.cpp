#include "FluidField.h"

#include "../I18n/I18n.h"
#include "Menu.h"
#include "State.h"
#include "Utils/D3D.h"

#define I18N_KEY_PREFIX "feature.fluid_field."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FluidField::Settings,
	Enabled,
	GridResolution,
	GridSize,
	JacobiIterations,
	WindDamping)

void FluidField::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enabled_tooltip"), "Runs the fluid simulation. It has no visible effect yet."));
	}

	if (shaderCompileFailed)
		ImGui::TextColored(Menu::GetSingleton()->GetTheme().StatusPalette.Error, "%s", T(TKEY("shader_compile_failed"), "Fluid Field shaders failed to compile, so the simulation is off. Check CommunityShaders.log."));
	if (resourceCreateFailed)
		ImGui::TextColored(Menu::GetSingleton()->GetTheme().StatusPalette.Error, "%s", T(TKEY("resource_create_failed"), "Fluid Field could not create its textures, so the simulation is off. Check CommunityShaders.log."));

	ImGui::SeparatorText(T(TKEY("grid"), "Grid"));

	const auto resolutionLabel = std::format("{0} x {0}", settings.GridResolution);
	if (ImGui::BeginCombo(T(TKEY("grid_resolution"), "Resolution"), resolutionLabel.c_str())) {
		for (const uint resolution : GRID_RESOLUTIONS) {
			const auto label = std::format("{0} x {0}", resolution);
			if (ImGui::Selectable(label.c_str(), resolution == settings.GridResolution)) {
				settings.GridResolution = resolution;
				resourceCreateFailed = false;
			}
		}
		ImGui::EndCombo();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("grid_resolution_tooltip"), "Texels per side. Higher values show finer detail and cost more GPU time. Changing it clears the field."));
	}

	ImGui::SliderFloat(T(TKEY("grid_size"), "Size"), &settings.GridSize, MIN_GRID_SIZE, MAX_GRID_SIZE, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("grid_size_tooltip"), "Width of the area simulated around the player, in game units. Changing it clears the field."));
	}
	ImGui::Text(T(TKEY("cell_size"), "Cell size: %.1f units"), GetCellSize());

	ImGui::SeparatorText(T(TKEY("wind"), "Wind"));

	ImGui::SliderScalar(T(TKEY("jacobi_iterations"), "Pressure Iterations"), ImGuiDataType_U32, &settings.JacobiIterations, &MIN_JACOBI_ITERATIONS, &MAX_JACOBI_ITERATIONS, "%u", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("jacobi_iterations_tooltip"), "More iterations make the wind swirl more accurately and cost more GPU time."));
	}

	ImGui::SliderFloat(T(TKEY("wind_damping"), "Damping"), &settings.WindDamping, 0.0f, 5.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("wind_damping_tooltip"), "How quickly wind dies down. Higher values calm it sooner."));
	}

	if (ImGui::Button(T(TKEY("reset_field"), "Reset Field")))
		queuedReset = true;
}

#undef I18N_KEY_PREFIX

FluidField::Settings FluidField::Sanitize(Settings a_settings)
{
	const Settings defaults{};

	if (std::ranges::find(GRID_RESOLUTIONS, a_settings.GridResolution) == GRID_RESOLUTIONS.end())
		a_settings.GridResolution = defaults.GridResolution;

	a_settings.GridSize = std::isfinite(a_settings.GridSize) ? std::clamp(a_settings.GridSize, MIN_GRID_SIZE, MAX_GRID_SIZE) : defaults.GridSize;
	a_settings.JacobiIterations = std::clamp(a_settings.JacobiIterations, MIN_JACOBI_ITERATIONS, MAX_JACOBI_ITERATIONS);
	a_settings.WindDamping = std::isfinite(a_settings.WindDamping) ? std::clamp(a_settings.WindDamping, 0.0f, 5.0f) : defaults.WindDamping;

	return a_settings;
}

void FluidField::LoadSettings(json& o_json)
{
	try {
		settings = o_json;
	} catch (const json::exception& e) {
		logger::warn("[Fluid Field] Malformed settings, using defaults: {}", e.what());
		settings = {};
	}
	settings = Sanitize(settings);
}

void FluidField::SaveSettings(json& o_json)
{
	o_json = settings;
}

void FluidField::RestoreDefaultSettings()
{
	settings = {};
}

void FluidField::PostPostLoad()
{
	if (!MenuOpenCloseEventHandler::Register())
		logger::warn("[Fluid Field] Loading-screen listener not registered; the field will not reset after loads");
}

void FluidField::ClearShaderCache()
{
	solver.ReleaseShaders();
	shaderCompileFailed = false;
}

bool FluidField::EnsureSolver()
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

void FluidField::ReleaseSolver()
{
	solver.ReleaseResources();
	solver.ReleaseShaders();
	lastCellSize = 0.0f;
}

void FluidField::Prepass()
{
	if (!settings.Enabled) {
		// Disabled costs nothing: no textures, no shaders, no dispatches
		if (solver.HasResources() || solver.HasShaders())
			ReleaseSolver();
		return;
	}

	auto state = globals::state;
	if (state->isMapMenuOpen || state->IsMainOrLoadingMenuOpen())
		return;

	auto player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return;

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
		TracyD3D11Zone(state->tracyCtx, "FluidField - Reset");
		solver.Reset(context);
	}

	const auto ui = globals::game::ui;
	const bool paused = ui && ui->GameIsPaused();
	const float deltaTime = (globals::game::deltaTime && !paused) ? std::min(*globals::game::deltaTime, MAX_STEP_SECONDS) : 0.0f;
	if (!(deltaTime > 0.0f))
		return;

	// Centre the grid on the player, snapped to whole cells so texels never resample
	const auto playerPos = player->GetPosition();
	const int32_t halfRes = static_cast<int32_t>(settings.GridResolution / 2);
	const int32_t gridMin[2] = {
		static_cast<int32_t>(std::floor(playerPos.x / cellSize)) - halfRes,
		static_cast<int32_t>(std::floor(playerPos.y / cellSize)) - halfRes
	};

	FluidFieldSim::StepParams params{
		.gridMin = { gridMin[0], gridMin[1] },
		.cellSize = cellSize,
		.deltaTime = deltaTime,
		.velocityDecay = std::exp(-settings.WindDamping * deltaTime),
		.jacobiIterations = settings.JacobiIterations,
		.emitters = {}
	};

	TracyD3D11Zone(state->tracyCtx, "FluidField - Wind Step");
	solver.Step(context, params, [&](const char* passName, auto&& runPass) {
#ifdef TRACY_ENABLE
		ZoneTransientN(fluidFieldPassZone, passName, true);
		TracyD3D11ZoneTransientS(state->tracyCtx, fluidFieldPassGpuZone, passName, 0, state->tracyCtx != nullptr);
#else
		(void)passName;
#endif
		runPass();
	});
}

RE::BSEventNotifyControl FluidField::MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	// Every load leaves the player somewhere new; start the next step from rest
	if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME && !a_event->opening)
		globals::features::fluidField.queuedReset = true;

	return RE::BSEventNotifyControl::kContinue;
}

bool FluidField::MenuOpenCloseEventHandler::Register()
{
	static MenuOpenCloseEventHandler singleton;
	auto ui = globals::game::ui;

	if (!ui) {
		logger::error("[Fluid Field] UI event source not found");
		return false;
	}

	ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(&singleton);
	return true;
}
