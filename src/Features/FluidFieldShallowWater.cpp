#include "FluidFieldShallowWater.h"

#include "../I18n/I18n.h"
#include "FluidField/Field.h"
#include "FluidFieldWaterfalls.h"
#include "Menu.h"
#include "Profiler.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

#include "UnifiedWater.h"
#include "UnifiedWater/Flowmap.h"

#include <bit>

#define I18N_KEY_PREFIX "feature.fluid_field_shallow_water."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FluidFieldShallowWater::Settings,
	Enabled,
	RiverSpeed,
	CurrentPull,
	Turbulence,
	WaveDamping,
	SurfaceNormalStrength,
	FlowDetail,
	FoamStrength,
	WaterTint,
	ReplaceVanillaFoam,
	WakeStrength,
	SplashStrength,
	WaterResolution,
	WaterSize,
	FarWater,
	FarWaterScale,
	RaycastBed,
	RaysPerFrame,
	RaySpacing,
	MeshCutObstacles)

void FluidFieldShallowWater::DrawSettings()
{
	const auto tooltip = [](const char* a_text) {
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", a_text);
	};
	const auto errorText = [](const char* a_text) { ImGui::TextColored(Menu::GetSingleton()->GetTheme().StatusPalette.Error, "%s", a_text); };

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	tooltip(T(TKEY("enabled_tooltip"), "Simulates the water around the player: currents, waves, wakes and foam on vanilla's water."));
	if (waterShaderFailed)
		errorText(T(TKEY("water_shader_failed"), "Water shaders failed to compile, so the water layer is off. Check CommunityShaders.log."));
	if (waterResourceFailed)
		errorText(T(TKEY("water_resource_failed"), "Fluid Field could not create the water textures, so the water layer is off. Check CommunityShaders.log."));

	ImGui::SeparatorText(T(TKEY("water_flow"), "Flow"));
	ImGui::SliderFloat(T(TKEY("river_speed"), "River Speed"), &settings.RiverSpeed, 0.0f, 400.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("river_speed_tooltip"), "How fast rivers flow, from Unified Water's flowmap, in game units per second."));
	ImGui::SliderFloat(T(TKEY("current_pull"), "Current Pull"), &settings.CurrentPull, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("current_pull_tooltip"), "How firmly the water is held to the river's course. Lower lets rocks and pillars shape the flow more."));
	ImGui::SliderFloat(T(TKEY("turbulence"), "Turbulence"), &settings.Turbulence, 0.0f, 5.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("turbulence_tooltip"), "Keeps eddies and swirls alive behind obstacles. 0 lets the flow smooth out."));
	ImGui::SliderFloat(T(TKEY("wave_damping"), "Wave Damping"), &settings.WaveDamping, 0.0f, 5.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("wave_damping_tooltip"), "How quickly waves and wakes die down. Higher values keep them close to whatever made them."));

	ImGui::SeparatorText(T(TKEY("water_look"), "Look"));
	ImGui::SliderFloat(T(TKEY("surface_normal_strength"), "Wave Strength"), &settings.SurfaceNormalStrength, 0.0f, 8.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("surface_normal_strength_tooltip"), "How strongly the simulated waves tilt the water's surface. 0 hides them; the simulation keeps running."));
	ImGui::SliderFloat(T(TKEY("flow_detail"), "Flow Detail"), &settings.FlowDetail, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("flow_detail_tooltip"), "Surface detail that moves with the simulated flow, so currents and eddies show on the water."));
	ImGui::SliderFloat(T(TKEY("foam_strength"), "Foam"), &settings.FoamStrength, 0.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("foam_strength_tooltip"), "Foam made by the simulated water where it hits obstacles, breaks over rapids, and around wakes, splashes and waterfalls. It drifts with the flow and fades."));
	ImGui::SliderFloat(T(TKEY("water_tint"), "Water Tint"), &settings.WaterTint, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("water_tint_tooltip"), "Tints what is seen through all water with the water's own colour, deepening with depth. 0 keeps vanilla's clear look."));
	ImGui::Checkbox(T(TKEY("replace_vanilla_foam"), "Replace Vanilla Foam"), &settings.ReplaceVanillaFoam);
	tooltip(T(TKEY("replace_vanilla_foam_tooltip"), "Hides vanilla's placed rapids foam, splash and spray effects where the water is simulated."));

	ImGui::SeparatorText(T(TKEY("water_interaction"), "Interaction"));
	ImGui::SliderFloat(T(TKEY("wake_strength"), "Wake Strength"), &settings.WakeStrength, 0.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("wake_strength_tooltip"), "How strongly actors push the water when moving through it."));
	ImGui::SliderFloat(T(TKEY("splash_strength"), "Splash Strength"), &settings.SplashStrength, 0.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("splash_strength_tooltip"), "How hard jumping or falling into the water, or climbing out of it, pushes the water."));

	if (ImGui::CollapsingHeader(T(TKEY("quality"), "Quality"))) {
		ImGui::PushID("QualitySection");
		ImGui::SeparatorText(T(TKEY("water_quality"), "Water"));
		const auto waterResolutionLabel = std::format("{0} x {0}", settings.WaterResolution);
		if (ImGui::BeginCombo(T(TKEY("water_resolution"), "Water Resolution"), waterResolutionLabel.c_str())) {
			for (const uint resolution : FluidFieldSim::Field::GRID_RESOLUTIONS) {
				const auto label = std::format("{0} x {0}", resolution);
				if (ImGui::Selectable(label.c_str(), resolution == settings.WaterResolution)) {
					settings.WaterResolution = resolution;
					waterResourceFailed = false;
				}
			}
			ImGui::EndCombo();
		}
		tooltip(T(TKEY("water_resolution_tooltip"), "Texels per side of each water level. Higher shows finer detail and costs more GPU time."));
		ImGui::SliderFloat(T(TKEY("water_size"), "Water Size"), &settings.WaterSize, FluidFieldSim::Field::MIN_GRID_SIZE, FluidFieldSim::Field::MAX_GRID_SIZE, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("water_size_tooltip"), "Width of the detailed water around the player, in game units: obstacles, bed detail and wakes."));
		ImGui::Text(T(TKEY("water_cell_size"), "Water cell size: %.1f units"), GetWaterCellSize());
		ImGui::Checkbox(T(TKEY("far_water"), "Far Water"), &settings.FarWater);
		tooltip(T(TKEY("far_water_tooltip"), "A coarser water simulation around the detailed one, so rivers flow and churn further out before vanilla takes over."));
		ImGui::SliderFloat(T(TKEY("far_water_scale"), "Far Water Scale"), &settings.FarWaterScale, 2.0f, 8.0f, "%.1fx", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("far_water_scale_tooltip"), "How much wider the far water is than the detailed water, at the same resolution."));

		ImGui::SeparatorText(T(TKEY("depth_map"), "Bed and Obstacles"));
		ImGui::Checkbox(T(TKEY("raycast_bed"), "Raycast Bed"), &settings.RaycastBed);
		tooltip(T(TKEY("raycast_bed_tooltip"), "Casts rays down from the water surface to find rocks, banks and riverbed meshes that the landscape heights miss."));
		ImGui::SliderScalar(T(TKEY("rays_per_frame"), "Rays Per Frame"), ImGuiDataType_U32, &settings.RaysPerFrame, &MIN_RAYS_PER_FRAME, &MAX_RAYS_PER_FRAME, "%u", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("ray_spacing"), "Ray Spacing"), &settings.RaySpacing, MIN_RAY_SPACING, MAX_RAY_SPACING, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("ray_spacing_tooltip"), "World units between rays. Smaller finds smaller rocks and casts more rays. Changing it clears the results."));
		ImGui::Checkbox(T(TKEY("mesh_cut"), "Cut Meshes at the Water Line"), &settings.MeshCutObstacles);
		tooltip(T(TKEY("mesh_cut_tooltip"), "Finds pillars, posts, walls and boats by cutting their meshes at the water height. Rays then only measure the bed."));
		ImGui::PopID();
	}
}

#undef I18N_KEY_PREFIX

FluidFieldShallowWater::Settings FluidFieldShallowWater::Sanitize(Settings a_settings)
{
	using FluidFieldSim::Field;
	const Settings defaults{};

	a_settings.RaysPerFrame = std::clamp(a_settings.RaysPerFrame, MIN_RAYS_PER_FRAME, MAX_RAYS_PER_FRAME);
	a_settings.RaySpacing = std::isfinite(a_settings.RaySpacing) ? std::clamp(a_settings.RaySpacing, MIN_RAY_SPACING, MAX_RAY_SPACING) : defaults.RaySpacing;
	a_settings.WakeStrength = std::isfinite(a_settings.WakeStrength) ? std::clamp(a_settings.WakeStrength, 0.0f, 3.0f) : defaults.WakeStrength;

	if (std::ranges::find(Field::GRID_RESOLUTIONS, a_settings.WaterResolution) == Field::GRID_RESOLUTIONS.end())
		a_settings.WaterResolution = defaults.WaterResolution;
	a_settings.WaterSize = std::isfinite(a_settings.WaterSize) ? std::clamp(a_settings.WaterSize, Field::MIN_GRID_SIZE, Field::MAX_GRID_SIZE) : defaults.WaterSize;
	a_settings.FarWaterScale = std::isfinite(a_settings.FarWaterScale) ? std::clamp(a_settings.FarWaterScale, 2.0f, 8.0f) : defaults.FarWaterScale;
	a_settings.SurfaceNormalStrength = std::isfinite(a_settings.SurfaceNormalStrength) ? std::clamp(a_settings.SurfaceNormalStrength, 0.0f, 8.0f) : defaults.SurfaceNormalStrength;
	a_settings.WaveDamping = std::isfinite(a_settings.WaveDamping) ? std::clamp(a_settings.WaveDamping, 0.0f, 5.0f) : defaults.WaveDamping;
	a_settings.SplashStrength = std::isfinite(a_settings.SplashStrength) ? std::clamp(a_settings.SplashStrength, 0.0f, 3.0f) : defaults.SplashStrength;
	a_settings.RiverSpeed = std::isfinite(a_settings.RiverSpeed) ? std::clamp(a_settings.RiverSpeed, 0.0f, 400.0f) : defaults.RiverSpeed;
	a_settings.CurrentPull = std::isfinite(a_settings.CurrentPull) ? std::clamp(a_settings.CurrentPull, 0.0f, 2.0f) : defaults.CurrentPull;
	a_settings.Turbulence = std::isfinite(a_settings.Turbulence) ? std::clamp(a_settings.Turbulence, 0.0f, 5.0f) : defaults.Turbulence;
	a_settings.FlowDetail = std::isfinite(a_settings.FlowDetail) ? std::clamp(a_settings.FlowDetail, 0.0f, 2.0f) : defaults.FlowDetail;
	a_settings.FoamStrength = std::isfinite(a_settings.FoamStrength) ? std::clamp(a_settings.FoamStrength, 0.0f, 3.0f) : defaults.FoamStrength;
	a_settings.WaterTint = std::isfinite(a_settings.WaterTint) ? std::clamp(a_settings.WaterTint, 0.0f, 1.0f) : defaults.WaterTint;
	return a_settings;
}

void FluidFieldShallowWater::LoadSettings(json& o_json)
{
	try {
		settings = o_json;
	} catch (const json::exception& e) {
		logger::warn("[Fluid Field] Malformed Shallow Water settings, using defaults: {}", e.what());
		settings = {};
	}
	settings = Sanitize(settings);
}

void FluidFieldShallowWater::SaveSettings(json& o_json)
{
	o_json = settings;
}

void FluidFieldShallowWater::RestoreDefaultSettings()
{
	settings = {};
}

void FluidFieldShallowWater::PostPostLoad()
{
	FluidFieldSim::Field::Get().RegisterListeners();
}

void FluidFieldShallowWater::ClearShaderCache()
{
	nearWater.solver.ReleaseShaders();
	waterShaderFailed = false;
}

void FluidFieldShallowWater::Release()
{
	for (auto* level : { &nearWater, &farWater }) {
		if (level->solver.HasResources() || level->solver.HasShaders()) {
			level->solver.ReleaseResources();
			level->solver.ReleaseShaders();
		}
		level->map.Clear();
		level->awaitingMap = true;
		level->resourceFailed = false;
	}
	wakeSources.Clear();
	vanillaFoam.RestoreAll();
	waterShaderData = {};
	if (bedRays.GetSampleCount() > 0)
		bedRays.Clear();
	if (meshCut.GetFineSpacing() > 0.0f)
		meshCut.Clear();
}

void FluidFieldShallowWater::Prepass()
{
	auto& field = FluidFieldSim::Field::Get();
	if (!settings.Enabled) {
		// Released once, so disabled costs nothing
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

	// Every load leaves the player somewhere new: find the bed again and start the water from rest
	const bool reset = field.ConsumeLoad(seenLoads);
	if (reset) {
		bedRays.Clear();
		meshCut.Clear();
	}
	const auto& landscape = field.GetLandscape();
	const auto& water = field.GetWater();
	const auto playerPos = field.GetPlayerPosition();
	bedRays.SetOverheadsAlwaysOpen(settings.MeshCutObstacles);
	if (settings.MeshCutObstacles) {
		ZoneScopedN("FluidField - Mesh Cut");
		globals::profiler->BeginPass("FluidField::Mesh Cut");
		meshCut.Update(water, GetObstacleSpacing());
		globals::profiler->EndPass();
	} else if (meshCut.GetFineSpacing() > 0.0f) {
		meshCut.Clear();
	}
	if (settings.RaycastBed) {
		ZoneScopedN("FluidField - Bed Raycasts");
		globals::profiler->BeginPass("FluidField::Bed Raycasts");
		bedRays.Update(landscape, water, playerPos.x, playerPos.y, settings.WaterSize, settings.RaySpacing, settings.RaysPerFrame);
		globals::profiler->EndPass();
	} else if (bedRays.GetSampleCount() > 0) {
		bedRays.Clear();
	}

	auto context = globals::d3d::context;
	if (reset) {
		ZoneScopedN("FluidField - Reset");
		TracyD3D11Zone(globals::state->tracyCtx, "FluidField - Reset");
		globals::profiler->BeginPass("FluidField::Reset");
		for (auto* level : { &nearWater, &farWater }) {
			level->solver.Reset();
			level->map.Clear();
			level->awaitingMap = true;
		}
		wakeSources.Clear();
		waterFade = 0.0f;
		waterShaderData.Enabled = 0;
		waterShaderData.FarEnabled = 0;
		globals::profiler->EndPass();
	}

	const float deltaTime = field.GetDeltaTime();
	if (!(deltaTime > 0.0f)) {
		// Paused: keep showing the last water outputs
		BindWaterOutputs(context);
		return;
	}

	ZoneScopedN("FluidField - Water Step");
	TracyD3D11Zone(globals::state->tracyCtx, "FluidField - Water Step");
	StepWater(context, nearWater, true, settings.WaterResolution, settings.WaterSize, playerPos, deltaTime);
	// Ease the simulated water in once it has outputs, rather than switching from vanilla at once
	waterFade = waterShaderData.Enabled ? std::min(1.0f, waterFade + deltaTime / FADE_IN_SECONDS) : 0.0f;
	if (settings.FarWater) {
		StepWater(context, farWater, false, settings.WaterResolution, settings.WaterSize * settings.FarWaterScale, playerPos, deltaTime);
	} else if (farWater.solver.HasResources()) {
		farWater.solver.ReleaseResources();
		farWater.solver.ReleaseShaders();
		farWater.map.Clear();
		farWater.awaitingMap = true;
		waterShaderData.FarEnabled = 0;
	}
	BindWaterOutputs(context);
	if (settings.ReplaceVanillaFoam) {
		// The simulated area, less the fade toward vanilla at its edge
		const float simulated = 0.5f * (settings.FarWater ? settings.WaterSize * settings.FarWaterScale : settings.WaterSize) * 0.85f;
		vanillaFoam.Update(water, playerPos.x, playerPos.y, simulated);
	} else if (vanillaFoam.GetHiddenCount() > 0) {
		vanillaFoam.RestoreAll();
	}
}

bool FluidFieldShallowWater::EnsureWater(WaterLevel& a_level, uint32_t a_resolution, float a_size, bool a_obstacles)
{
	if (waterShaderFailed || a_level.resourceFailed)
		return false;

	const uint mapResolution = FluidFieldSim::WaterMapBuilder::ResolutionFor(a_size);
	// Only the near level takes fine obstacles; the far one keeps a minimal, empty obstacle texture
	const uint obstacleResolution = a_obstacles ? std::clamp(std::bit_ceil(static_cast<uint>(std::ceil(a_size / GetObstacleSpacing()))), 16u, 2048u) : 16u;
	auto& levelSolver = a_level.solver;
	if (levelSolver.GetResolution() != a_resolution || levelSolver.GetMapResolution() != mapResolution || levelSolver.GetObstacleResolution() != obstacleResolution) {
		try {
			levelSolver.CreateResources(globals::d3d::device, a_resolution, mapResolution, obstacleResolution, [](ID3D11DeviceChild* object, const char* name) {
				Util::SetResourceName(object, "%s", name);
			});
		} catch (const winrt::hresult_error& e) {
			logger::error("[Fluid Field] Failed to create water GPU resources: {}", winrt::to_string(e.message()));
			levelSolver.ReleaseResources();
			a_level.resourceFailed = true;
			waterResourceFailed = true;
			return false;
		}
		a_level.map.Clear();
		a_level.awaitingMap = true;
	}

	if (!levelSolver.HasShaders()) {
		const bool compiled = levelSolver.CompileShaders([](const wchar_t* fileName) {
			const auto path = std::filesystem::path("Data\\Shaders\\FluidField") / fileName;
			return reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), {}, "cs_5_0"));
		});
		if (!compiled) {
			logger::error("[Fluid Field] Water compute shaders failed to compile; the water layer stays off");
			levelSolver.ReleaseShaders();
			waterShaderFailed = true;
			return false;
		}
	}
	return true;
}

int32_t FluidFieldShallowWater::GetObstacleRatio() const
{
	const float rayFine = settings.RaySpacing / static_cast<float>(FluidFieldSim::BedRaycasts::REFINE);
	return std::clamp(static_cast<int32_t>(std::lround(rayFine / GetWaterCellSize())), 1, 4);
}

float FluidFieldShallowWater::GetObstacleSpacing() const
{
	return settings.RaySpacing / static_cast<float>(FluidFieldSim::BedRaycasts::REFINE * GetObstacleRatio());
}

std::array<float, FluidFieldSim::MeshCut::BLOCK * FluidFieldSim::MeshCut::BLOCK * 4> FluidFieldShallowWater::ComposeObstacleBlock(int32_t a_originX, int32_t a_originY) const
{
	constexpr int32_t n = FluidFieldSim::MeshCut::BLOCK;
	const int32_t ratio = GetObstacleRatio();
	std::array<float, n * n * 4> texels{};
	for (int32_t j = 0; j < n; j++) {
		for (int32_t i = 0; i < n; i++) {
			const int32_t fx = a_originX + i;
			const int32_t fy = a_originY + j;
			float* texel = &texels[static_cast<std::size_t>(j * n + i) * 4];
			texel[1] = static_cast<float>(fx);
			texel[2] = static_cast<float>(fy);
			float top = 0.0f;
			if (settings.MeshCutObstacles && meshCut.IsSolid(fx, fy, top)) {
				texel[0] = top;
				texel[3] = 2.0f;
			} else if (settings.RaycastBed) {
				// The nearest bed-ray fine point; obstacle points are `ratio` times finer
				const auto rx = static_cast<int32_t>(std::lround(static_cast<double>(fx) / ratio));
				const auto ry = static_cast<int32_t>(std::lround(static_cast<double>(fy) / ratio));
				bedRays.FindFine(rx, ry, texel[0], texel[3]);
			}
		}
	}
	return texels;
}

void FluidFieldShallowWater::UploadObstacles(ID3D11DeviceContext* a_context, const RE::NiPoint3& a_playerPos)
{
	constexpr int32_t n = FluidFieldSim::MeshCut::BLOCK;
	const float fineSpacing = GetObstacleSpacing();
	const int32_t ratio = GetObstacleRatio();
	const auto blockKey = [](int32_t a_originX, int32_t a_originY) {
		return (static_cast<uint64_t>(static_cast<uint32_t>(a_originX)) << 32) | static_cast<uint32_t>(a_originY);
	};

	if (nearWater.solver.ObstaclesNeedClear() || bedRays.WasCleared() || meshCut.WasCleared()) {
		nearWater.solver.ClearObstacles(a_context);
		bedRays.MarkAllRefinedDirty();
		residentMeshBlocks.clear();
		meshWindowCentre[0] = meshWindowCentre[1] = INT32_MIN;
	}

	// A refined ray square covers ratio x ratio obstacle blocks (plus the half-point rounding at its edges)
	for (const auto& block : bedRays.TakeDirtyBlocks()) {
		const auto floorDiv = [](int32_t a_value, int32_t a_divisor) {
			return a_value >= 0 ? a_value / a_divisor : -((-a_value + a_divisor - 1) / a_divisor);
		};
		const int32_t half = ratio / 2;
		const int32_t minX = floorDiv(block.originX * ratio - half, n) * n;
		const int32_t minY = floorDiv(block.originY * ratio - half, n) * n;
		const int32_t maxX = floorDiv((block.originX + FluidFieldSim::BedRaycasts::REFINE) * ratio + half, n) * n;
		const int32_t maxY = floorDiv((block.originY + FluidFieldSim::BedRaycasts::REFINE) * ratio + half, n) * n;
		for (int32_t y = minY; y <= maxY; y += n) {
			for (int32_t x = minX; x <= maxX; x += n) {
				const auto texels = ComposeObstacleBlock(x, y);
				nearWater.solver.UploadObstacleBlock(a_context, x, y, n, texels.data(), fineSpacing);
			}
		}
	}
	if (!settings.MeshCutObstacles)
		return;

	// Mesh-cut blocks stay inside the texture's window less a block, so no two share a texel of the wrapped texture
	const int32_t windowHalf = static_cast<int32_t>(nearWater.solver.GetObstacleResolution() / 2) - n;
	const int32_t centreX = static_cast<int32_t>(std::floor(a_playerPos.x / fineSpacing / n)) * n;
	const int32_t centreY = static_cast<int32_t>(std::floor(a_playerPos.y / fineSpacing / n)) * n;
	const auto inWindow = [&](int32_t a_x, int32_t a_y) {
		return std::abs(a_x - centreX) <= windowHalf && std::abs(a_y - centreY) <= windowHalf;
	};
	constexpr std::size_t MAX_UPLOADS = 512;
	std::size_t uploads = 0;
	const auto upload = [&](int32_t a_x, int32_t a_y) {
		const auto texels = ComposeObstacleBlock(a_x, a_y);
		nearWater.solver.UploadObstacleBlock(a_context, a_x, a_y, n, texels.data(), fineSpacing);
		residentMeshBlocks.insert(blockKey(a_x, a_y));
		uploads++;
	};

	for (const auto& [x, y] : meshCut.TakeDirtyBlocks()) {
		if (inWindow(x, y))
			upload(x, y);
		else
			residentMeshBlocks.erase(blockKey(x, y));
	}

	// Upload blocks entering the window, capped per frame; small moves wait since walking every block is not free
	const int32_t rescanDistance = std::max(windowHalf / 8, n);
	if (meshWindowCentre[0] == INT32_MIN || std::abs(centreX - meshWindowCentre[0]) >= rescanDistance || std::abs(centreY - meshWindowCentre[1]) >= rescanDistance) {
		bool finished = true;
		for (const auto& [x, y] : meshCut.GetAllBlocks()) {
			const uint64_t key = blockKey(x, y);
			if (!inWindow(x, y)) {
				residentMeshBlocks.erase(key);
			} else if (!residentMeshBlocks.contains(key)) {
				if (uploads >= MAX_UPLOADS) {
					finished = false;
					continue;
				}
				upload(x, y);
			}
		}
		if (finished) {
			meshWindowCentre[0] = centreX;
			meshWindowCentre[1] = centreY;
		}
	}
}

FluidFieldShallowWater::WaterShaderData FluidFieldShallowWater::GetCommonBufferData() const
{
	if (!loaded || !settings.Enabled)
		return {};
	WaterShaderData data = waterShaderData;
	data.WaterTint = settings.WaterTint;
	data.Fade = waterFade;
	return data;
}

void FluidFieldShallowWater::BindWaterOutputs(ID3D11DeviceContext* a_context) const
{
	if (!waterShaderData.Enabled)
		return;
	const bool hasFar = waterShaderData.FarEnabled != 0;
	ID3D11ShaderResourceView* srvs[] = { nearWater.solver.GetSurfaceSRV(), nearWater.solver.GetFlowSRV(),
		hasFar ? farWater.solver.GetSurfaceSRV() : nullptr, hasFar ? farWater.solver.GetFlowSRV() : nullptr };
	a_context->PSSetShaderResources(66, 4, srvs);
}

void FluidFieldShallowWater::StepWater(ID3D11DeviceContext* a_context, WaterLevel& a_level, bool a_near, uint32_t a_resolution, float a_size, const RE::NiPoint3& a_playerPos, float a_deltaTime)
{
	if (!EnsureWater(a_level, a_resolution, a_size, a_near))
		return;
	auto& levelSolver = a_level.solver;
	auto& map = a_level.map;
	const auto& landscape = FluidFieldSim::Field::Get().GetLandscape();
	const auto& water = FluidFieldSim::Field::Get().GetWater();

	// The level's own placement: centred on the player, snapped to whole cells
	const float a_cellSize = a_size / static_cast<float>(a_resolution);
	if (a_cellSize != a_level.lastCellSize) {
		levelSolver.Reset();
		map.Clear();
		a_level.awaitingMap = true;
		a_level.lastCellSize = a_cellSize;
		if (a_near)
			wakeSources.Clear();
	}
	const int32_t halfRes = static_cast<int32_t>(a_resolution / 2);
	a_level.gridMin[0] = static_cast<int32_t>(std::floor(a_playerPos.x / a_cellSize)) - halfRes;
	a_level.gridMin[1] = static_cast<int32_t>(std::floor(a_playerPos.y / a_cellSize)) - halfRes;
	const int32_t* a_gridMin = a_level.gridMin;

	{
		ZoneScopedN("FluidField - Water Map");
		globals::profiler->BeginPass(a_near ? "FluidFieldShallowWater::Water Map" : "FluidFieldShallowWater::Far Water Map");
		const uint64_t inputVersion = water.GetSignature() ^ (static_cast<uint64_t>(bedRays.GetSampleCount()) << 20) ^ (static_cast<uint64_t>(landscape.GetCellCount()) << 52);
		if (map.Update(landscape, water, settings.RaycastBed ? &bedRays : nullptr, a_playerPos.x, a_playerPos.y, levelSolver.GetMapResolution(), inputVersion)) {
			// After a reset, wait for settled inputs so the water starts calm on the right bed
			const bool settled = !(settings.RaycastBed && bedRays.IsRefilling());
			if (!a_level.awaitingMap || settled) {
				levelSolver.UploadMap(a_context, map.GetData(), FluidFieldSim::WaterMapBuilder::MAP_SPACING);
				// Warm up a couple of seconds at once so the river already flows when it first shows
				if (a_level.awaitingMap)
					a_level.warmupSubsteps = static_cast<uint32_t>(WARMUP_SECONDS / WARMUP_SUBSTEP);
				a_level.awaitingMap = false;
			} else {
				map.Clear();  // Built too early; rebuild once the inputs settle
			}
		}
		globals::profiler->EndPass();
	}
	if (!levelSolver.HasMap())
		return;

	// Fine bed detail, and wakes and splashes from the actors around the player (D17), near level only
	levelEmitters.clear();
	if (a_near) {
		ZoneScopedN("FluidField - Water Obstacles");
		UploadObstacles(a_context, a_playerPos);
		const auto& wakes = wakeSources.Update(water, a_playerPos, a_gridMin, a_cellSize, a_resolution, a_deltaTime,
			{ .wakeStrength = settings.WakeStrength, .splashStrength = settings.SplashStrength });
		levelEmitters.assign(wakes.begin(), wakes.end());
	}
	// D35: the falls' plunges push both levels, after the wakes
	const auto& waterfalls = globals::features::fluidFieldWaterfalls;
	waterfalls.AppendWaterEmitters(levelEmitters, a_gridMin, a_cellSize, a_resolution, FluidFieldSim::ShallowWaterSolver::MAX_EMITTERS, a_near);
	const std::span<const FluidFieldSim::WaterEmitter> emitters = levelEmitters;

	FluidFieldSim::WaterParams params;
	params.damping = settings.WaveDamping;
	params.edgeWidth = std::max(16.0f, 0.12f * static_cast<float>(a_resolution));
	params.currentRelax = settings.CurrentPull;
	params.vorticity = settings.Turbulence;
	params.foamStrength = settings.FoamStrength;
	if (!a_near) {
		// Far cells are FarWaterScale times wider, so sources cover more water: less foam, held less
		params.foamStrength *= FAR_FOAM_SCALE;
		params.foamMax = FAR_FOAM_MAX;
	}
	// D44: the plunges foam the water along the falls' whole width
	if (FluidFieldWaterfalls::PlungeMaps plunges; waterfalls.GetPlungeMaps(plunges)) {
		params.plungeMap = plunges.plunge;
		params.plungeOrigin[0] = plunges.origin[0];
		params.plungeOrigin[1] = plunges.origin[1];
		params.plungeInverseCell = plunges.inverseCell;
		params.plungeSize = plunges.size;
		params.lipMap = plunges.lip;  // D170: on the plunge map's lattice
	}
	// Cells entering the near level take the far level's foam, so it doesn't reset as the player approaches
	if (a_near && settings.FarWater && farWater.solver.HasMap() && waterShaderData.FarEnabled) {
		params.seedFoam = farWater.solver.GetFlowSRV();
		params.seedGridMin[0] = farWater.gridMin[0];
		params.seedGridMin[1] = farWater.gridMin[1];
		params.seedRes = settings.WaterResolution;
		params.seedCellSize = settings.WaterSize * settings.FarWaterScale / static_cast<float>(settings.WaterResolution);
	}
	// River currents from Unified Water's flowmap; still water when there is none
	RE::NiPointer<RE::NiSourceTexture> flowmapTexture;
	const auto& unifiedWater = globals::features::unifiedWater;
	if (settings.RiverSpeed > 0.0f && unifiedWater.loaded) {
		if (const auto* flowmap = unifiedWater.GetFlowmap(); flowmap && flowmap->TryGetFlowmap(flowmapTexture) && flowmapTexture && flowmapTexture->rendererTexture &&
															 flowmapTexture->rendererTexture->resourceView) {
			params.flowmap = flowmapTexture->rendererTexture->resourceView;
			params.flowmapCells[0] = static_cast<float>(flowmap->GetWidth());
			params.flowmapCells[1] = static_cast<float>(flowmap->GetHeight());
			params.flowmapOffset[0] = flowmap->GetOffsetX();
			params.flowmapOffset[1] = flowmap->GetOffsetY();
			params.riverSpeed = settings.RiverSpeed;
		}
	}

	// Short sub-steps keep waves on deep water stable
	constexpr float MAX_SUBSTEP = 0.012f;
	const uint substeps = std::clamp(static_cast<uint>(std::ceil(a_deltaTime / MAX_SUBSTEP)), 1u, 4u);
	const FluidFieldSim::StepParams grid{
		.gridMin = { a_gridMin[0], a_gridMin[1] },
		.cellSize = a_cellSize,
		.deltaTime = a_deltaTime / static_cast<float>(substeps),
		.velocityDecay = 1.0f,
		.jacobiIterations = 0,
		.emitters = {}
	};
	const auto wrap = [&](const char* passName, auto&& runPass) {
		globals::profiler->BeginPass(std::format("FluidFieldShallowWater::{}{}", a_near ? "" : "Far ", passName));
		runPass();
		globals::profiler->EndPass();
	};
	if (a_level.warmupSubsteps > 0) {
		ZoneScopedN("FluidField - Water Warm-up");
		auto warmup = grid;
		warmup.deltaTime = WARMUP_SUBSTEP;
		const auto noTiming = [](const char*, auto&& runPass) { runPass(); };
		globals::profiler->BeginPass(a_near ? "FluidFieldShallowWater::Water Warm-up" : "FluidFieldShallowWater::Far Water Warm-up");
		for (uint32_t i = 0; i < a_level.warmupSubsteps; i++)
			levelSolver.Step(a_context, warmup, params, {}, noTiming);
		globals::profiler->EndPass();
		a_level.warmupSubsteps = 0;
	}
	for (uint i = 0; i < substeps; i++)
		levelSolver.Step(a_context, grid, params, emitters, wrap);

	globals::profiler->BeginPass(a_near ? "FluidFieldShallowWater::Water Output" : "FluidFieldShallowWater::Far Water Output");
	levelSolver.WriteOutputs(a_context);
	if (a_near) {
		waterShaderData.GridMin[0] = a_gridMin[0];
		waterShaderData.GridMin[1] = a_gridMin[1];
		waterShaderData.GridRes = a_resolution;
		waterShaderData.CellSize = a_cellSize;
		waterShaderData.NormalStrength = settings.SurfaceNormalStrength;
		waterShaderData.Enabled = levelSolver.HasMap() ? 1u : 0u;
		waterShaderData.FlowDetail = settings.FlowDetail;
	} else {
		waterShaderData.FarGridMin[0] = a_gridMin[0];
		waterShaderData.FarGridMin[1] = a_gridMin[1];
		waterShaderData.FarGridRes = a_resolution;
		waterShaderData.FarCellSize = a_cellSize;
		waterShaderData.FarEnabled = levelSolver.HasMap() ? 1u : 0u;
	}
	globals::profiler->EndPass();
}
