#include "FluidField.h"

#include "../I18n/I18n.h"
#include "Menu.h"
#include "Profiler.h"
#include "State.h"
#include "Utils/D3D.h"

#define I18N_KEY_PREFIX "feature.fluid_field."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FluidField::Settings,
	Enabled,
	GridResolution,
	GridSize,
	JacobiIterations,
	WindDamping,
	TestPuffStrength,
	TestPuffRadius,
	DepthMapEnabled,
	VerifyLandscape)

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

	ImGui::SeparatorText(T(TKEY("test_emitter"), "Test Emitter"));

	if (ImGui::Button(T(TKEY("test_puff"), "Puff of Wind")))
		queuedTestPuff = true;
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("test_puff_tooltip"), "Temporary test: blows a gust of wind from the player in the direction they face. Turn on the layer overlay to see it."));
	}

	ImGui::SliderFloat(T(TKEY("test_puff_strength"), "Puff Strength"), &settings.TestPuffStrength, 0.0f, 5000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("test_puff_radius"), "Puff Radius"), &settings.TestPuffRadius, 16.0f, 2048.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);

	ImGui::SeparatorText(T(TKEY("depth_map"), "Depth Map (Experimental)"));

	ImGui::Checkbox(T(TKEY("depth_map_enabled"), "Build Depth Map"), &settings.DepthMapEnabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("depth_map_enabled_tooltip"), "Copies the landscape heights of the loaded cells around the player. Work in progress: nothing uses them yet."));
	}
	ImGui::Checkbox(T(TKEY("verify_landscape"), "Verify Landscape Heights"), &settings.VerifyLandscape);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("verify_landscape_tooltip"), "Debug: compares each newly copied cell with the game's own height query and logs the difference."));
	}
	ImGui::Text(T(TKEY("landscape_cells"), "Cached cells: %u"), static_cast<uint>(landscape.GetCellCount()));
	if (const auto& stats = landscape.GetVerifyStats(); stats.cellsChecked > 0) {
		ImGui::Text(T(TKEY("landscape_verify_result"), "Checked %u cells: vertex error %.2f, interpolated %.2f units, largest anchor %.0f (%u of %u samples missed)"),
			stats.cellsChecked, stats.maxVertexError, stats.maxInterpolatedError, stats.maxAbsAnchor, stats.samplesMissed, stats.samplesChecked);
	}

	ImGui::SeparatorText(T(TKEY("debug"), "Debug"));

	ImGui::Checkbox(T(TKEY("show_layer_overlay"), "Show Layer Overlay"), &showDebugOverlay);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("show_layer_overlay_tooltip"), "Draws every simulation layer on screen, north up, centred on the player. Not saved."));
	}
	ImGui::SliderFloat(T(TKEY("overlay_scale"), "Overlay Scale"), &debugOverlayScale, 0.5f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("overlay_velocity_range"), "Velocity Range"), &debugVelocityRange, 10.0f, 5000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
	ImGui::SliderFloat(T(TKEY("overlay_divergence_range"), "Divergence Range"), &debugDivergenceRange, 0.01f, 20.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
	ImGui::SliderFloat(T(TKEY("overlay_pressure_range"), "Pressure Range"), &debugPressureRange, 1.0f, 50000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("overlay_range_tooltip"), "The value drawn at full colour in the overlay."));
	}
}

void FluidField::DrawOverlay()
{
	if (!IsOverlayVisible())
		return;

	const float uiScale = Util::GetUIScale();
	const float padding = ThemeManager::Constants::OVERLAY_WINDOW_POSITION * uiScale;
	const auto* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - padding, viewport->WorkPos.y + padding), ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));

	// Click-through during play; movable while the menu is open
	ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoCollapse;
	if (!Menu::GetSingleton()->IsEnabled)
		flags |= ImGuiWindowFlags_NoInputs;

	if (!ImGui::Begin(T(TKEY("overlay_title"), "Fluid Field Layers###FluidFieldLayers"), nullptr, flags)) {
		ImGui::End();
		return;
	}

	ImGui::Text(T(TKEY("overlay_grid_info"), "%u x %u texels, %.1f units each, south-west cell (%d, %d)"),
		settings.GridResolution, settings.GridResolution, GetCellSize(), lastGridMin[0], lastGridMin[1]);

	const float imageSize = 192.0f * uiScale * debugOverlayScale;
	const float res = static_cast<float>(settings.GridResolution);
	const ImU32 markerColor = ImGui::GetColorU32(ImGuiCol_Text);
	const std::array<const char*, 3> layerNames = {
		T(TKEY("layer_velocity"), "Velocity"),
		T(TKEY("layer_divergence"), "Divergence"),
		T(TKEY("layer_pressure"), "Pressure")
	};

	for (std::size_t i = 0; i < layerNames.size(); i++) {
		if (i > 0)
			ImGui::SameLine();

		ImGui::BeginGroup();
		ImGui::TextUnformatted(layerNames[i]);
		ImGui::Image(reinterpret_cast<ImTextureID>(solver.GetDebugViewSRV(i)), ImVec2(imageSize, imageSize));

		// Player marker and facing; images are north up, so texel y is flipped
		const ImVec2 imageMin = ImGui::GetItemRectMin();
		const ImVec2 marker(imageMin.x + lastPlayerTexel[0] / res * imageSize, imageMin.y + (1.0f - lastPlayerTexel[1] / res) * imageSize);
		const float markerRadius = 3.0f * uiScale;
		const float facingLength = 10.0f * uiScale;
		auto* drawList = ImGui::GetWindowDrawList();
		drawList->AddCircle(marker, markerRadius, markerColor);
		drawList->AddLine(marker, ImVec2(marker.x + std::sin(lastHeading) * facingLength, marker.y - std::cos(lastHeading) * facingLength), markerColor);
		ImGui::EndGroup();
	}

	ImGui::TextUnformatted(T(TKEY("overlay_legend_velocity"), "Velocity: hue is direction (east red, north green, west cyan, south violet), brightness is speed."));
	ImGui::TextUnformatted(T(TKEY("overlay_legend_scalar"), "Divergence and pressure: red is positive, blue is negative."));

	ImGui::End();
}

bool FluidField::IsOverlayVisible() const
{
	return showDebugOverlay && settings.Enabled && solver.HasResources() && hasDebugViews;
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
	a_settings.TestPuffStrength = std::isfinite(a_settings.TestPuffStrength) ? std::clamp(a_settings.TestPuffStrength, 0.0f, 5000.0f) : defaults.TestPuffStrength;
	a_settings.TestPuffRadius = std::isfinite(a_settings.TestPuffRadius) ? std::clamp(a_settings.TestPuffRadius, 16.0f, 2048.0f) : defaults.TestPuffRadius;

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
		hasDebugViews = false;
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
	hasDebugViews = false;
}

void FluidField::Prepass()
{
	if (!settings.Enabled) {
		// Disabled costs nothing: no textures, no shaders, no dispatches
		if (solver.HasResources() || solver.HasShaders())
			ReleaseSolver();
		if (landscape.GetCellCount() > 0)
			landscape.Clear();
		return;
	}

	auto state = globals::state;
	if (state->isMapMenuOpen || state->IsMainOrLoadingMenuOpen())
		return;

	auto player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return;

	if (settings.DepthMapEnabled) {
		if (queuedLandscapeClear.exchange(false))
			landscape.Clear();
		ZoneScopedN("FluidField - Landscape Cache");
		globals::profiler->BeginPass("FluidField::Landscape Cache");
		landscape.Update(settings.VerifyLandscape);
		globals::profiler->EndPass();
	} else if (landscape.GetCellCount() > 0) {
		landscape.Clear();
	}

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
		globals::profiler->BeginPass("FluidField::Reset");
		solver.Reset(context);
		globals::profiler->EndPass();
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

	// Test puff: a gust centred on the player, blowing the way they face (heading 0 faces +Y, north)
	std::optional<FluidFieldSim::Emitter> testPuff;
	if (std::exchange(queuedTestPuff, false)) {
		const float heading = player->GetAngleZ();
		testPuff = FluidFieldSim::Emitter{
			.position = { playerPos.x / cellSize - static_cast<float>(gridMin[0]), playerPos.y / cellSize - static_cast<float>(gridMin[1]) },
			.radius = settings.TestPuffRadius / cellSize,
			.pad0 = 0.0f,
			.velocity = { std::sin(heading) * settings.TestPuffStrength, std::cos(heading) * settings.TestPuffStrength },
			.pad1 = {}
		};
	}

	FluidFieldSim::StepParams params{
		.gridMin = { gridMin[0], gridMin[1] },
		.cellSize = cellSize,
		.deltaTime = deltaTime,
		.velocityDecay = std::exp(-settings.WindDamping * deltaTime),
		.jacobiIterations = settings.JacobiIterations,
		.emitters = testPuff ? std::span<const FluidFieldSim::Emitter>(&*testPuff, 1) : std::span<const FluidFieldSim::Emitter>()
	};

	{
		ZoneScopedN("FluidField - Wind Step");
		TracyD3D11Zone(state->tracyCtx, "FluidField - Wind Step");
		solver.Step(context, params, [&](const char* passName, auto&& runPass) {
#ifdef TRACY_ENABLE
			ZoneTransientN(fluidFieldPassZone, passName, true);
			TracyD3D11ZoneTransientS(state->tracyCtx, fluidFieldPassGpuZone, passName, 0, state->tracyCtx != nullptr);
#endif
			// The built-in profiler cannot nest passes, so each solver pass is timed on its own
			globals::profiler->BeginPass(std::format("FluidField::{}", passName));
			runPass();
			globals::profiler->EndPass();
		});
	}

	lastGridMin[0] = gridMin[0];
	lastGridMin[1] = gridMin[1];
	lastPlayerTexel[0] = playerPos.x / cellSize - static_cast<float>(gridMin[0]);
	lastPlayerTexel[1] = playerPos.y / cellSize - static_cast<float>(gridMin[1]);
	lastHeading = player->GetAngleZ();

	if (showDebugOverlay) {
		ZoneScopedN("FluidField - Debug View");
		TracyD3D11Zone(state->tracyCtx, "FluidField - Debug View");
		globals::profiler->BeginPass("FluidField::Debug View");
		solver.RenderDebugViews(context, { .velocity = 1.0f / debugVelocityRange, .divergence = 1.0f / debugDivergenceRange, .pressure = 1.0f / debugPressureRange });
		globals::profiler->EndPass();
		hasDebugViews = true;
	}
}

RE::BSEventNotifyControl FluidField::MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	// Every load leaves the player somewhere new; start the next step from rest
	if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME && !a_event->opening) {
		globals::features::fluidField.queuedReset = true;
		globals::features::fluidField.queuedLandscapeClear = true;
	}

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
