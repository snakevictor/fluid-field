#include "FluidField.h"

#include "../I18n/I18n.h"
#include "Menu.h"
#include "Profiler.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

#include "DynamicCubemaps.h"
#include "UnifiedWater.h"
#include "UnifiedWater/Flowmap.h"

#include <DirectXPackedVector.h>
#include <bit>
#include <numbers>

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
	VerifyLandscape,
	VerifyWater,
	RaycastBed,
	RaysPerFrame,
	RaySpacing,
	LogDepthMap,
	MeshCutObstacles,
	WaterEnabled,
	WakeStrength,
	TestCurrentSpeed,
	TestCurrentAngle,
	WaterResolution,
	WaterSize,
	FarWater,
	FarWaterScale,
	SurfaceNormalStrength,
	WaveDamping,
	SplashStrength,
	RiverSpeed,
	CurrentPull,
	Turbulence,
	FlowDetail,
	FoamStrength,
	WaterTint,
	InteractiveFog,
	FogDensity,
	FogThickness,
	FogBrightness,
	FogStir,
	FogRefillSeconds,
	ReplaceVanillaFoam,
	Waterfalls,
	WaterfallStrength,
	HideUnderwaterFalls,
	FallWhitening,
	FallSpeed,
	FallFoamStart,
	FallAerationFoam,
	FallAerationFrom,
	FallAerationMilk,
	WaterfallDebug)

void FluidField::DrawSettings()
{
	const auto tooltip = [](const char* a_text) {
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", a_text);
	};
	const auto errorText = [](const char* a_text) { ImGui::TextColored(Menu::GetSingleton()->GetTheme().StatusPalette.Error, "%s", a_text); };

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	tooltip(T(TKEY("enabled_tooltip"), "Simulates water, wind and ground fog around the player."));
	if (shaderCompileFailed)
		errorText(T(TKEY("shader_compile_failed"), "Fluid Field shaders failed to compile, so the simulation is off. Check CommunityShaders.log."));
	if (resourceCreateFailed)
		errorText(T(TKEY("resource_create_failed"), "Fluid Field could not create its textures, so the simulation is off. Check CommunityShaders.log."));
	if (waterShaderFailed)
		errorText(T(TKEY("water_shader_failed"), "Water shaders failed to compile, so the water layer is off. Check CommunityShaders.log."));
	if (waterResourceFailed)
		errorText(T(TKEY("water_resource_failed"), "Fluid Field could not create the water textures, so the water layer is off. Check CommunityShaders.log."));

	// Water
	if (ImGui::CollapsingHeader(T(TKEY("water"), "Water"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::PushID("WaterSection");
		ImGui::Checkbox(T(TKEY("water_enabled"), "Simulate Water"), &settings.WaterEnabled);
		tooltip(T(TKEY("water_enabled_tooltip"), "Simulates the water around the player: currents, waves, wakes and foam on vanilla's water."));

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
		ImGui::PopID();
	}

	// Waterfalls
	if (ImGui::CollapsingHeader(T(TKEY("waterfalls_header"), "Waterfalls"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::PushID("WaterfallsSection");
		ImGui::Checkbox(T(TKEY("waterfalls"), "Waterfalls"), &settings.Waterfalls);
		tooltip(T(TKEY("waterfalls_tooltip"), "Waterfalls churn, push and foam the water they land in."));

		ImGui::SeparatorText(T(TKEY("waterfall_look"), "Water"));
		ImGui::Checkbox(T(TKEY("fall_whitening"), "Foam on the Falls"), &settings.FallWhitening);
		ImGui::SliderFloat(T(TKEY("fall_foam_start"), "Foam Start Height"), &settings.FallFoamStart, 0.0f, 300.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("fall_foam_start_tooltip"), "How far the water falls, in world units, before it takes air or foam: the crest stays clear above it. Baked falls only."));
		ImGui::SliderFloat(T(TKEY("fall_aeration_foam"), "Aeration Foam"), &settings.FallAerationFoam, 0.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("fall_aeration_foam_tooltip"), "How much of the bake's aeration turns to white foam: higher makes the lower falls frothier."));
		ImGui::SliderFloat(T(TKEY("fall_aeration_from"), "Aeration Threshold"), &settings.FallAerationFrom, 0.0f, 0.95f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("fall_aeration_from_tooltip"), "The bake's aeration where that foam begins: lower starts the foam higher up the drop."));
		ImGui::SliderFloat(T(TKEY("fall_aeration_milk"), "Aeration Milk"), &settings.FallAerationMilk, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("fall_aeration_milk_tooltip"), "How milky aeration makes the clear water (0: stays clear)."));
		tooltip(T(TKEY("fall_whitening_tooltip"), "White water on the falls where they strike rock, below rock, into the water below and as they run and fall."));
		ImGui::SliderFloat(T(TKEY("fall_speed"), "Falls Speed"), &settings.FallSpeed, 0.5f, 3.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("fall_speed_tooltip"), "How fast the falls' water runs, against vanilla's own speed. The whole flow speeds up alike, so the texture never stretches."));
		ImGui::Checkbox(T(TKEY("hide_underwater_falls"), "Hide Buried Falls"), &settings.HideUnderwaterFalls);
		tooltip(T(TKEY("hide_underwater_falls_tooltip"), "Hides the parts of vanilla's waterfall meshes under the water above or below them, or deep in the rock."));
		ImGui::SliderFloat(T(TKEY("waterfall_strength"), "Plunge Strength"), &settings.WaterfallStrength, 0.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("waterfall_strength_tooltip"), "How hard the falls churn, push and foam the water they land in."));
		ImGui::PopID();
	}

	// Ground fog
	if (ImGui::CollapsingHeader(T(TKEY("fog"), "Ground Fog"))) {
		ImGui::PushID("FogSection");
		ImGui::Checkbox(T(TKEY("interactive_fog"), "Ground Fog"), &settings.InteractiveFog);
		tooltip(T(TKEY("interactive_fog_tooltip"), "Fog lying on the ground around you, in banks and clearings. It parts and swirls around moving actors and drifts with the weather's wind."));
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
		ImGui::PopID();
	}

	// Quality: grid sizes, bed and obstacle detection
	if (ImGui::CollapsingHeader(T(TKEY("quality"), "Quality"))) {
		ImGui::PushID("QualitySection");
		ImGui::SeparatorText(T(TKEY("water_quality"), "Water"));
		const auto waterResolutionLabel = std::format("{0} x {0}", settings.WaterResolution);
		if (ImGui::BeginCombo(T(TKEY("water_resolution"), "Water Resolution"), waterResolutionLabel.c_str())) {
			for (const uint resolution : GRID_RESOLUTIONS) {
				const auto label = std::format("{0} x {0}", resolution);
				if (ImGui::Selectable(label.c_str(), resolution == settings.WaterResolution)) {
					settings.WaterResolution = resolution;
					waterResourceFailed = false;
				}
			}
			ImGui::EndCombo();
		}
		tooltip(T(TKEY("water_resolution_tooltip"), "Texels per side of each water level. Higher shows finer detail and costs more GPU time."));
		ImGui::SliderFloat(T(TKEY("water_size"), "Water Size"), &settings.WaterSize, MIN_GRID_SIZE, MAX_GRID_SIZE, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("water_size_tooltip"), "Width of the detailed water around the player, in game units: obstacles, bed detail and wakes."));
		ImGui::Text(T(TKEY("water_cell_size"), "Water cell size: %.1f units"), GetWaterCellSize());
		ImGui::Checkbox(T(TKEY("far_water"), "Far Water"), &settings.FarWater);
		tooltip(T(TKEY("far_water_tooltip"), "A coarser water simulation around the detailed one, so rivers flow and churn further out before vanilla takes over."));
		ImGui::SliderFloat(T(TKEY("far_water_scale"), "Far Water Scale"), &settings.FarWaterScale, 2.0f, 8.0f, "%.1fx", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("far_water_scale_tooltip"), "How much wider the far water is than the detailed water, at the same resolution."));

		ImGui::SeparatorText(T(TKEY("wind"), "Wind and Fog Grid"));
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
		tooltip(T(TKEY("grid_resolution_tooltip"), "Texels per side. Higher values show finer detail and cost more GPU time. Changing it clears the field."));
		ImGui::SliderFloat(T(TKEY("grid_size"), "Size"), &settings.GridSize, MIN_GRID_SIZE, MAX_GRID_SIZE, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("grid_size_tooltip"), "Width of the area simulated around the player, in game units. Changing it clears the field."));
		ImGui::Text(T(TKEY("cell_size"), "Cell size: %.1f units"), GetCellSize());
		ImGui::SliderScalar(T(TKEY("jacobi_iterations"), "Pressure Iterations"), ImGuiDataType_U32, &settings.JacobiIterations, &MIN_JACOBI_ITERATIONS, &MAX_JACOBI_ITERATIONS, "%u", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("jacobi_iterations_tooltip"), "More iterations make the wind swirl more accurately and cost more GPU time."));
		ImGui::SliderFloat(T(TKEY("wind_damping"), "Wind Damping"), &settings.WindDamping, 0.0f, 5.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("wind_damping_tooltip"), "How quickly wind dies down. Higher values calm it sooner."));
		if (ImGui::Button(T(TKEY("reset_field"), "Reset Field")))
			queuedReset = true;

		ImGui::SeparatorText(T(TKEY("depth_map"), "Bed and Obstacles"));
		ImGui::Checkbox(T(TKEY("depth_map_enabled"), "Build Depth Map"), &settings.DepthMapEnabled);
		tooltip(T(TKEY("depth_map_enabled_tooltip"), "Maps the riverbed and vanilla water around the player, which the water simulation stands on."));
		ImGui::Checkbox(T(TKEY("raycast_bed"), "Raycast Bed"), &settings.RaycastBed);
		tooltip(T(TKEY("raycast_bed_tooltip"), "Casts rays down from the water surface to find rocks, banks and riverbed meshes that the landscape heights miss."));
		ImGui::SliderScalar(T(TKEY("rays_per_frame"), "Rays Per Frame"), ImGuiDataType_U32, &settings.RaysPerFrame, &MIN_RAYS_PER_FRAME, &MAX_RAYS_PER_FRAME, "%u", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("ray_spacing"), "Ray Spacing"), &settings.RaySpacing, MIN_RAY_SPACING, MAX_RAY_SPACING, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("ray_spacing_tooltip"), "World units between rays. Smaller finds smaller rocks and casts more rays. Changing it clears the results."));
		ImGui::Checkbox(T(TKEY("mesh_cut"), "Cut Meshes at the Water Line"), &settings.MeshCutObstacles);
		tooltip(T(TKEY("mesh_cut_tooltip"), "Finds pillars, posts, walls and boats by cutting their meshes at the water height. Rays then only measure the bed."));
		ImGui::PopID();
	}

	// Debug: views, toggles and counts
	if (ImGui::CollapsingHeader(T(TKEY("debug"), "Debug"))) {
		ImGui::PushID("DebugSection");
		ImGui::SeparatorText(T(TKEY("debug_water"), "Water"));
		{
			const char* waterViews[] = { T(TKEY("water_debug_off"), "Off"), T(TKEY("water_debug_flow"), "Flow"), T(TKEY("water_debug_foam"), "Foam"),
				T(TKEY("water_debug_height"), "Surface Height") };
			ImGui::Combo(T(TKEY("water_debug_view"), "Water Debug View"), &waterDebugView, waterViews, IM_ARRAYSIZE(waterViews));
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("water_debug_view_tooltip"), "Draws the shallow water simulation on the water itself: the flow (colour by direction, lines moving with it), its foam, or the surface's height (blue below rest, red above)."));
		}
		ImGui::SeparatorText(T(TKEY("debug_waterfalls"), "Waterfalls"));
		{
			const char* fallDebugModes[] = { T(TKEY("waterfall_debug_off"), "Off"), T(TKEY("waterfall_debug_bake"), "Mesh Flow Bake"), T(TKEY("waterfall_debug_direction"), "Flow Direction"),
				T(TKEY("waterfall_debug_vanilla"), "Vanilla Texture (No Flow)"), T(TKEY("waterfall_debug_depth"), "Depth Behind (See-Through Falls)"),
				T(TKEY("waterfall_debug_quadrants"), "Quadrants (See-Through Falls)") };
			settings.WaterfallDebug = std::clamp(settings.WaterfallDebug, 0, 5);
			ImGui::Combo(T(TKEY("waterfall_debug"), "Waterfall Shading"), &settings.WaterfallDebug, fallDebugModes, IM_ARRAYSIZE(fallDebugModes));
			tooltip(T(TKEY("waterfall_debug_tooltip"), "Paints the falling water with its flow. Mesh Flow Bake: red how the streaks spread, green the run time (a band per second), blue foam, white the wake of rock; dark magenta where a mesh has no bake. Flow Direction: the way the texture moves as colour (x red, y green, up blue), brighter for faster. Vanilla Texture: the falls without the flow. Depth Behind: with See-Through Falls, each sheet by the open water or air behind it to the first rock or ground: red under 50 units, yellow to 150, green to 300, blue to 600 (8.5 m), magenta where nothing was hit."));
			ImGui::SliderFloat(T(TKEY("plunge_foam_behind"), "Plunge Foam Behind (tuning)"), &plungeFoamBehind, -64.0f, 256.0f, "%.0f");
			tooltip(T(TKEY("plunge_foam_behind_tooltip"), "World units the foam where the falls meet the water below is moved behind their line, against their flow. Not saved."));
			ImGui::SliderFloat(T(TKEY("plunge_foam_radius"), "Plunge Foam Radius (tuning)"), &plungeFoamRadius, 0.25f, 4.0f, "%.2f");
			tooltip(T(TKEY("plunge_foam_radius_tooltip"), "How far round each point the falls meet the water below it foams, as a scale. Not saved."));

			uint32_t captured, captures;
			fallSites.CountCaptures(captured, captures);
			ImGui::Text(T(TKEY("waterfall_sites"), "Sites: %u falls, %u cascades, %u rapids, %u no bottom, %u failed; ground captured for %u of %u"),
				fallSites.CountKind(FluidFieldSim::FallSites::Kind::Fall), fallSites.CountKind(FluidFieldSim::FallSites::Kind::Cascade), fallSites.CountKind(FluidFieldSim::FallSites::Kind::Rapids),
				fallSites.CountKind(FluidFieldSim::FallSites::Kind::NoBottom), fallSites.CountKind(FluidFieldSim::FallSites::Kind::Failed), captured, captures);
		}

		ImGui::SeparatorText(T(TKEY("debug_layers"), "Simulation Layers"));
		ImGui::Checkbox(T(TKEY("show_layer_overlay"), "Show Layer Overlay"), &showDebugOverlay);
		tooltip(T(TKEY("show_layer_overlay_tooltip"), "Draws every simulation layer on screen, north up, centred on the player. Not saved."));
		ImGui::SliderFloat(T(TKEY("overlay_scale"), "Overlay Scale"), &debugOverlayScale, 0.5f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("overlay_velocity_range"), "Velocity Range"), &debugVelocityRange, 10.0f, 5000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat(T(TKEY("overlay_divergence_range"), "Divergence Range"), &debugDivergenceRange, 0.01f, 20.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat(T(TKEY("overlay_pressure_range"), "Pressure Range"), &debugPressureRange, 1.0f, 50000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
		tooltip(T(TKEY("overlay_range_tooltip"), "The value drawn at full colour in the overlay."));

		ImGui::SeparatorText(T(TKEY("debug_checks"), "Counts"));
		ImGui::Text(T(TKEY("landscape_cells"), "Cached cells: %u"), static_cast<uint>(landscape.GetCellCount()));
		ImGui::Text(T(TKEY("water_surfaces"), "Water surfaces: %u flat, %u sloped (not used), %u cells with water"), static_cast<uint>(water.GetSurfaceCount()), water.GetSlopedCount(),
			static_cast<uint>(water.GetCellWaterCount()));
		ImGui::Text(T(TKEY("vanilla_foam_hidden"), "Vanilla foam effects hidden: %u"), vanillaFoam.GetHiddenCount());
		if (settings.RaycastBed && bedRays.GetSampleCount() > 0) {
			const auto counts = bedRays.CountSamples();
			const auto& rayStats = bedRays.GetStats();
			ImGui::Text(T(TKEY("raycast_counts"), "Bed rays: %u terrain, %u meshes, %u out of reach, %u under ground, %u no water; %u cast last frame"), counts.terrain, counts.statics, counts.noHit,
				counts.skipped, counts.noWater, rayStats.raysLastFrame);
		}
		if (settings.MeshCutObstacles) {
			const auto& cutStats = meshCut.GetStats();
			ImGui::Text(T(TKEY("mesh_cut_stats"), "Mesh cut: %u references, %u shapes (%u skipped), %u closed loops, %u solid points, %u waiting; %.2f ms last frame"), cutStats.references,
				cutStats.shapes, cutStats.shapesWithoutData, cutStats.closedLoops, cutStats.solidPoints, cutStats.pending, cutStats.lastUpdateMs);
		}
		if (const auto& stats = landscape.GetVerifyStats(); stats.cellsChecked > 0) {
			ImGui::Text(T(TKEY("landscape_verify_result"), "Checked %u cells: vertex error %.2f, interpolated %.2f units, largest anchor %.0f (%u of %u samples missed)"), stats.cellsChecked,
				stats.maxVertexError, stats.maxInterpolatedError, stats.maxAbsAnchor, stats.samplesMissed, stats.samplesChecked);
		}
		if (const auto& stats = water.GetVerifyStats(); stats.runs > 0) {
			ImGui::Text(T(TKEY("water_verify_result"), "Water checked %u times: error %.2f units, plane vs bound %.2f (%u of %u samples missed)"), stats.runs, stats.maxError,
				stats.maxPlaneDifference, stats.samplesMissed, stats.samplesChecked);
		}
		ImGui::PopID();
	}
}

void FluidField::DrawOverlay()
{
	if (!(showDebugOverlay && settings.Enabled && solver.HasResources() && hasDebugViews))
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

	if (depthView.HasImages()) {
		const std::array<const char*, FluidFieldSim::DepthMapView::IMAGE_COUNT> depthNames = {
			T(TKEY("layer_bed"), "Bed"),
			T(TKEY("layer_water"), "Water Surface"),
			T(TKEY("layer_depth"), "Depth"),
			T(TKEY("layer_raycasts"), "Raycasts")
		};
		for (std::size_t i = 0; i < depthNames.size(); i++) {
			if (i > 0)
				ImGui::SameLine();
			ImGui::BeginGroup();
			ImGui::TextUnformatted(depthNames[i]);
			ImGui::Image(reinterpret_cast<ImTextureID>(depthView.GetSRV(i)), ImVec2(imageSize, imageSize));

			// The depth map images are centred on the player
			const ImVec2 imageMin = ImGui::GetItemRectMin();
			const ImVec2 marker(imageMin.x + imageSize * 0.5f, imageMin.y + imageSize * 0.5f);
			auto* drawList = ImGui::GetWindowDrawList();
			drawList->AddCircle(marker, 3.0f * uiScale, markerColor);
			drawList->AddLine(marker, ImVec2(marker.x + std::sin(lastHeading) * 10.0f * uiScale, marker.y - std::cos(lastHeading) * 10.0f * uiScale), markerColor);
			ImGui::EndGroup();
		}
		const auto& depthStats = depthView.GetStats();
		ImGui::TextUnformatted(T(TKEY("overlay_legend_bed"), "Bed: green below the player, brown above, magenta where no landscape is loaded."));
		ImGui::TextUnformatted(T(TKEY("overlay_legend_water"), "Water: bright blue is wet under a water object, bright cyan wet under cell water only; dim is a water plane under the ground."));
		ImGui::TextUnformatted(T(TKEY("overlay_legend_depth"), "Depth: lighter is shallow, darker is deep; tan is water plane under the ground."));
		ImGui::TextUnformatted(T(TKEY("overlay_legend_raycasts"), "Raycasts: red a mesh-cut obstacle, green terrain, purple under a deck or arch, orange solid above the water, yellow above the water but not checked (ignored), brown a mesh below the water, dark blue nothing in reach, grey no ray."));
		ImGui::Text(T(TKEY("overlay_depth_stats"), "Wet %u texels (%u water object, %u cell water), deepest %.0f units; %u plane under ground; %u without a bed"),
			depthStats.submerged, depthStats.waterObject, depthStats.cellWater, depthStats.maxDepth, depthStats.buried, depthStats.bedMissing);
	} else if (settings.DepthMapEnabled || settings.WaterEnabled) {
		ImGui::TextUnformatted(T(TKEY("overlay_depth_waiting"), "Depth map: waiting for data."));
	} else {
		ImGui::TextUnformatted(T(TKEY("overlay_depth_off"), "Depth map: turn on Build Depth Map to see the bed, water surface and depth here."));
	}

	if (nearWater.solver.HasDebugViews()) {
		const float waterRes = static_cast<float>(settings.WaterResolution);
		const float waterCell = GetWaterCellSize();
		const float waterTexel[2] = { lastPlayerPos[0] / waterCell - static_cast<float>(nearWater.gridMin[0]),
			lastPlayerPos[1] / waterCell - static_cast<float>(nearWater.gridMin[1]) };
		const std::array<const char*, FluidFieldSim::ShallowWaterSolver::DEBUG_VIEW_NAMES.size()> waterNames = {
			T(TKEY("layer_water_depth"), "Water Depth"),
			T(TKEY("layer_water_flow"), "Flow"),
			T(TKEY("layer_waves"), "Waves")
		};
		for (std::size_t i = 0; i < waterNames.size(); i++) {
			if (i > 0)
				ImGui::SameLine();
			ImGui::BeginGroup();
			ImGui::TextUnformatted(waterNames[i]);
			ImGui::Image(reinterpret_cast<ImTextureID>(nearWater.solver.GetDebugViewSRV(i)), ImVec2(imageSize, imageSize));
			const ImVec2 imageMin = ImGui::GetItemRectMin();
			const ImVec2 marker(imageMin.x + waterTexel[0] / waterRes * imageSize, imageMin.y + (1.0f - waterTexel[1] / waterRes) * imageSize);
			auto* drawList = ImGui::GetWindowDrawList();
			drawList->AddCircle(marker, 3.0f * uiScale, markerColor);
			drawList->AddLine(marker, ImVec2(marker.x + std::sin(lastHeading) * 10.0f * uiScale, marker.y - std::cos(lastHeading) * 10.0f * uiScale), markerColor);
			ImGui::EndGroup();
		}
		ImGui::TextUnformatted(T(TKEY("overlay_legend_water_sim"), "Water depth: light is shallow, dark is deep. Flow: hue is direction, brightness speed. Waves: red above vanilla water, blue below, foam white."));
	}

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
	a_settings.RaysPerFrame = std::clamp(a_settings.RaysPerFrame, MIN_RAYS_PER_FRAME, MAX_RAYS_PER_FRAME);
	a_settings.RaySpacing = std::isfinite(a_settings.RaySpacing) ? std::clamp(a_settings.RaySpacing, MIN_RAY_SPACING, MAX_RAY_SPACING) : defaults.RaySpacing;
	a_settings.WakeStrength = std::isfinite(a_settings.WakeStrength) ? std::clamp(a_settings.WakeStrength, 0.0f, 3.0f) : defaults.WakeStrength;
	a_settings.TestCurrentSpeed = std::isfinite(a_settings.TestCurrentSpeed) ? std::clamp(a_settings.TestCurrentSpeed, 0.0f, 400.0f) : defaults.TestCurrentSpeed;
	a_settings.TestCurrentAngle = std::isfinite(a_settings.TestCurrentAngle) ? std::clamp(a_settings.TestCurrentAngle, 0.0f, 360.0f) : defaults.TestCurrentAngle;

	if (std::ranges::find(GRID_RESOLUTIONS, a_settings.WaterResolution) == GRID_RESOLUTIONS.end())
		a_settings.WaterResolution = defaults.WaterResolution;
	a_settings.WaterSize = std::isfinite(a_settings.WaterSize) ? std::clamp(a_settings.WaterSize, MIN_GRID_SIZE, MAX_GRID_SIZE) : defaults.WaterSize;
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
	a_settings.WaterfallStrength = std::isfinite(a_settings.WaterfallStrength) ? std::clamp(a_settings.WaterfallStrength, 0.0f, 3.0f) : defaults.WaterfallStrength;
	a_settings.FogDensity = std::isfinite(a_settings.FogDensity) ? std::clamp(a_settings.FogDensity, 0.0f, 0.2f) : defaults.FogDensity;
	a_settings.FogBrightness = std::isfinite(a_settings.FogBrightness) ? std::clamp(a_settings.FogBrightness, 0.0f, 2.0f) : defaults.FogBrightness;
	a_settings.FogThickness = std::isfinite(a_settings.FogThickness) ? std::clamp(a_settings.FogThickness, 5.0f, 400.0f) : defaults.FogThickness;
	a_settings.FogStir = std::isfinite(a_settings.FogStir) ? std::clamp(a_settings.FogStir, 0.0f, 3.0f) : defaults.FogStir;
	a_settings.FogRefillSeconds = std::isfinite(a_settings.FogRefillSeconds) ? std::clamp(a_settings.FogRefillSeconds, 0.5f, 30.0f) : defaults.FogRefillSeconds;
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
	Hooks::Install();
}

void FluidField::Hooks::BSWaterShader_SetupGeometry::thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
{
	func(a_shader, a_pass, a_renderFlags);
	if (auto& fluidField = globals::features::fluidField; fluidField.loaded)
		fluidField.SetupWaterGeometry(a_pass);
}

void FluidField::Hooks::Install()
{
	stl::write_vfunc<0x6, BSWaterShader_SetupGeometry>(RE::VTABLE_BSWaterShader[0]);
	logger::info("[Fluid Field] Installed hooks");
}

void FluidField::ClearShaderCache()
{
	solver.ReleaseShaders();
	shaderCompileFailed = false;
	groundFogCS = nullptr;
	groundFogFailed = false;
	nearWater.solver.ReleaseShaders();
	waterShaderFailed = false;
	fallRefraction.ReleaseShaders();
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

void FluidField::ClearDepthMap()
{
	if (landscape.GetCellCount() > 0)
		landscape.Clear();
	if (water.GetSurfaceCount() > 0 || water.GetCellWaterCount() > 0)
		water.Clear();
	if (bedRays.GetSampleCount() > 0)
		bedRays.Clear();
	if (meshCut.GetFineSpacing() > 0.0f)
		meshCut.Clear();
	if (depthView.HasImages())
		depthView.Release();
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
		ReleaseWater();
		ClearDepthMap();
		return;
	}
	if (!gpuSpans.IsReady())
		gpuSpans.Initialize(globals::d3d::device);
	// D192: close last frame's spans, read finished ones, time this frame's simulation to Prepass' end
	gpuSpans.EndFrame(globals::d3d::context);
	gpuSpans.Begin(globals::d3d::context, FluidFieldSim::GpuSpans::Simulation);
	struct SimulationSpan
	{
		FluidFieldSim::GpuSpans& spans;
		~SimulationSpan() { spans.End(globals::d3d::context, FluidFieldSim::GpuSpans::Simulation); }
	} simulationSpan{ gpuSpans };

	auto state = globals::state;
	if (state->isMapMenuOpen || state->IsMainOrLoadingMenuOpen())
		return;

	auto player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return;

	if (settings.DepthMapEnabled || settings.WaterEnabled || settings.InteractiveFog) {
		if (queuedLandscapeClear.exchange(false)) {
			landscape.Clear();
			water.Clear();
			bedRays.Clear();
			meshCut.Clear();
		}
		{
			ZoneScopedN("FluidField - Landscape Cache");
			globals::profiler->BeginPass("FluidField::Landscape Cache");
			landscape.Update(settings.VerifyLandscape);
			globals::profiler->EndPass();
		}
		{
			ZoneScopedN("FluidField - Water Surfaces");
			globals::profiler->BeginPass("FluidField::Water Surfaces");
			water.Update(settings.VerifyWater);
			globals::profiler->EndPass();
		}
		const auto playerPos = player->GetPosition();
		bedRays.SetOverheadsAlwaysOpen(settings.MeshCutObstacles);
		if (settings.MeshCutObstacles) {
			ZoneScopedN("FluidField - Mesh Cut");
			globals::profiler->BeginPass("FluidField::Mesh Cut");
			meshCut.Update(water, GetObstacleSpacing(), settings.LogDepthMap);
			globals::profiler->EndPass();
			// Nobody else consumes the changed blocks while the water is off
			if (!settings.WaterEnabled)
				meshCut.TakeDirtyBlocks();
		} else if (meshCut.GetFineSpacing() > 0.0f) {
			meshCut.Clear();
		}
		if (settings.RaycastBed) {
			ZoneScopedN("FluidField - Bed Raycasts");
			globals::profiler->BeginPass("FluidField::Bed Raycasts");
			bedRays.Update(landscape, water, playerPos.x, playerPos.y, settings.WaterSize, settings.RaySpacing, settings.RaysPerFrame, settings.LogDepthMap);
			globals::profiler->EndPass();
			// Nobody else consumes the refined blocks while the water is off
			if (!settings.WaterEnabled)
				bedRays.TakeDirtyBlocks();
		} else if (bedRays.GetSampleCount() > 0) {
			bedRays.Clear();
		}
		if (showDebugOverlay) {
			ZoneScopedN("FluidField - Depth Map View");
			globals::profiler->BeginPass("FluidField::Depth Map View");
			depthView.Update(globals::d3d::context, landscape, water, settings.RaycastBed ? &bedRays : nullptr, settings.MeshCutObstacles ? &meshCut : nullptr,
				playerPos.x, playerPos.y, playerPos.z, settings.WaterSize);
			globals::profiler->EndPass();
		} else if (depthView.HasImages()) {
			depthView.Release();
		}
	} else {
		ClearDepthMap();
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
		for (auto* level : { &nearWater, &farWater }) {
			level->solver.Reset();
			level->map.Clear();
			level->awaitingMap = true;
		}
		wakeSources.Clear();
		airSources.Clear();
		waterFade = 0.0f;
		waterShaderData.Enabled = 0;
		waterShaderData.FarEnabled = 0;
		globals::profiler->EndPass();
	}

	const auto ui = globals::game::ui;
	const bool paused = ui && ui->GameIsPaused();
	const float deltaTime = (globals::game::deltaTime && !paused) ? std::min(*globals::game::deltaTime, MAX_STEP_SECONDS) : 0.0f;
	if (!(deltaTime > 0.0f)) {
		// Paused: keep showing the last water outputs
		BindWaterOutputs(context);
		return;
	}

	// Centre the grid on the player, snapped to whole cells so texels never resample
	const auto playerPos = player->GetPosition();
	const int32_t halfRes = static_cast<int32_t>(settings.GridResolution / 2);
	const int32_t gridMin[2] = {
		static_cast<int32_t>(std::floor(playerPos.x / cellSize)) - halfRes,
		static_cast<int32_t>(std::floor(playerPos.y / cellSize)) - halfRes
	};

	// Test puff: a gust on the player, blowing the way they face (heading 0 is +Y, north)
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

	// Moving actors push the air (D21), leaving the test puff a slot
	std::vector<FluidFieldSim::Emitter> windEmitters;
	if (settings.InteractiveFog) {
		windEmitters = airSources.Update(playerPos, gridMin, cellSize, settings.GridResolution, deltaTime, settings.FogStir, FluidFieldSim::WindSolver::MAX_EMITTERS - 1);
	}
	if (testPuff)
		windEmitters.push_back(*testPuff);

	// The fog: carried by the wind and the weather's own wind, refilling behind actors
	const FluidFieldSim::FogParams* fog = nullptr;
	if (settings.InteractiveFog) {
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
		fog = &fogParams;
	}

	FluidFieldSim::StepParams params{
		.gridMin = { gridMin[0], gridMin[1] },
		.cellSize = cellSize,
		.deltaTime = deltaTime,
		.velocityDecay = std::exp(-settings.WindDamping * deltaTime),
		.jacobiIterations = settings.JacobiIterations,
		.emitters = windEmitters,
		.fog = fog
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
	if (settings.LogDepthMap && fog) {
		if (fogLogCountdown == 0) {
			logger::info("[Fluid Field] Ground fog: {} draw(s) last frame into colour/depth {}", fogDrawsThisFrame, fogDrawTargets);
			fogLogCountdown = 120;
		} else {
			fogLogCountdown--;
		}
	}
	fogDrawsThisFrame = 0;
	fogDrawTargets.clear();
	fogReady = false;
	if (fog) {
		// The ground the fog lies on, under the wind grid
		globals::profiler->BeginPass("FluidField::Fog Ground");
		// The ground under the whole fog area, wider than the simulated one
		const int32_t groundHalf = static_cast<int32_t>(FOG_GROUND_RESOLUTION / 2);
		fogGroundMin[0] = static_cast<int32_t>(std::floor(playerPos.x / FOG_GROUND_CELL)) - groundHalf;
		fogGroundMin[1] = static_cast<int32_t>(std::floor(playerPos.y / FOG_GROUND_CELL)) - groundHalf;
		fogReady = groundHeight.Update(globals::d3d::device, context, landscape, water, fogGroundMin, FOG_GROUND_RESOLUTION, FOG_GROUND_CELL);
		globals::profiler->EndPass();
		fogGridMin[0] = gridMin[0];
		fogGridMin[1] = gridMin[1];
	} else {
		groundHeight.Release();
	}
	lastPlayerTexel[0] = playerPos.x / cellSize - static_cast<float>(gridMin[0]);
	lastPlayerTexel[1] = playerPos.y / cellSize - static_cast<float>(gridMin[1]);
	lastHeading = player->GetAngleZ();
	lastPlayerPos[0] = playerPos.x;
	lastPlayerPos[1] = playerPos.y;

	if (settings.WaterEnabled) {
		ZoneScopedN("FluidField - Water Step");
		TracyD3D11Zone(state->tracyCtx, "FluidField - Water Step");
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
		if (settings.Waterfalls) {
			// Waterfall sites reconstructed from vanilla's layout (D25)
			fallSites.SetConsoleSummary(showFallSites);
			// Vanilla's pieces at the cached sites hide while our pass draws their falls
			if (!RefractiveFallsActive())
				fallsShown = false;
			fallSites.SetTakeover(fallsShown);
			fallSurfaceTime += deltaTime;
			// D67: one scroll for all falls at vanilla's averaged rate, so a cascade's meshes run on into each other;
			// integrated so it never jumps when the measured rate moves (D60)
			{
				float total = 0.0f, count = 0.0f;
				for (const auto& [material, scroll] : fallScrolls) {
					total += (scroll.rate > 1e-3f ? scroll.rate : DEFAULT_FALL_SCROLL) / std::max(scroll.scale, 1e-4f);
					count += 1.0f;
				}
				const float rate = count > 0.0f ? total / count : DEFAULT_FALL_SCROLL;
				fallRate = fallRate > 0.0f ? fallRate + (rate - fallRate) * std::min(deltaTime, 1.0f) : rate;
				// D90: scaled by the user's speed, one rate for every fall
				fallPhase += fallRate * std::clamp(settings.FallSpeed, 0.1f, 5.0f) * std::clamp(deltaTime, 0.0f, 0.25f);
				fallPhase -= std::floor(fallPhase);
			}
			fallSites.SetLandscape(&landscape);  // D172
			fallSites.Update(water);
			UpdateFallGround(playerPos);
		} else {
			fallSites.Clear();
		}
	} else {
		ReleaseWater();
	}

	if (showDebugOverlay) {
		ZoneScopedN("FluidField - Debug View");
		TracyD3D11Zone(state->tracyCtx, "FluidField - Debug View");
		globals::profiler->BeginPass("FluidField::Debug View");
		solver.RenderDebugViews(context, { .velocity = 1.0f / debugVelocityRange, .divergence = 1.0f / debugDivergenceRange, .pressure = 1.0f / debugPressureRange });
		if (settings.WaterEnabled)
			nearWater.solver.RenderDebugViews(context, {});
		globals::profiler->EndPass();
		hasDebugViews = true;
	}
}

bool FluidField::EnsureWater(WaterLevel& a_level, uint32_t a_resolution, float a_size, bool a_obstacles)
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

void FluidField::ReleaseWater()
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
	fallSites.Clear();
	ReleaseFallGround();
	fallRefraction.Release();
	gpuSpans.Release();
	riverCaptures.clear();
	riverPieces.clear();
	waterGeometry = nullptr;
	fallWaterTypes.clear();
	waterShaderData = {};
}

int32_t FluidField::GetObstacleRatio() const
{
	const float rayFine = settings.RaySpacing / static_cast<float>(FluidFieldSim::BedRaycasts::REFINE);
	return std::clamp(static_cast<int32_t>(std::lround(rayFine / GetWaterCellSize())), 1, 4);
}

float FluidField::GetObstacleSpacing() const
{
	return settings.RaySpacing / static_cast<float>(FluidFieldSim::BedRaycasts::REFINE * GetObstacleRatio());
}

std::array<float, FluidFieldSim::MeshCut::BLOCK * FluidFieldSim::MeshCut::BLOCK * 4> FluidField::ComposeObstacleBlock(int32_t a_originX, int32_t a_originY) const
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

void FluidField::UploadObstacles(ID3D11DeviceContext* a_context, const RE::NiPoint3& a_playerPos)
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

FluidField::WaterShaderData FluidField::GetCommonBufferData() const
{
	if (!loaded || !settings.Enabled)
		return {};
	if (!settings.WaterEnabled)
		return {};
	WaterShaderData data = waterShaderData;
	data.WaterTint = settings.WaterTint;
	data.Fade = waterFade;
	data.DebugView = static_cast<uint32_t>(waterDebugView);
	return data;
}

void FluidField::DrawGroundFogAfterWorld()
{
	if (!loaded || !settings.Enabled || !globals::state->inWorld)
		return;
	// D192: ends the engine-water span begun in DrawGroundFogBeforeWater
	gpuSpans.End(globals::d3d::context, FluidFieldSim::GpuSpans::EngineWater);
	auto* renderer = globals::game::renderer;
	if (!renderer)
		return;

	// D91: the refractive falls, over the finished world (water included) and under their spray
	if (RefractiveFallsActive() && cameraSnapshot) {
		const auto& shadow = globals::game::shadowState->GetRuntimeData();
		const auto colorIndex = static_cast<uint32_t>(shadow.renderTargets[0]);
		const auto depthIndex = shadow.depthStencil;
		if (colorIndex < RE::RENDER_TARGET::kTOTAL && depthIndex < RE::RENDER_TARGET_DEPTHSTENCIL::kTOTAL) {
			auto& target = renderer->GetRuntimeData().renderTargets[colorIndex];
			auto& depth = renderer->GetDepthStencilData().depthStencils[depthIndex];
			// D105: depth behind from the post-prepass copy; the live depth's view reads as far everywhere
			auto* behind = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV;
			// D177: writable depth view: the falls write their depth so later passes (distance fog) see the sheet, not what is behind it
			gpuSpans.Begin(globals::d3d::context, FluidFieldSim::GpuSpans::RefractiveFalls);
			const bool drawn = DrawRefractiveFalls(target.RTV, target.texture, depth.views[0] ? depth.views[0] : depth.readOnlyViews[0], behind ? behind : depth.depthSRV);
			gpuSpans.End(globals::d3d::context, FluidFieldSim::GpuSpans::RefractiveFalls);
			if (drawn) {
				fallsShown = true;
				fallsMissed = 0;
			} else if (++fallsMissed > FALLS_FALLBACK_FRAMES) {
				fallsShown = false;
			}
		}
	}

	if (!settings.InteractiveFog || !fogReady)
		return;
	if (fogDebugMode == FOG_DEBUG_BEFORE_WATER)
		return;  // Drawn from the deferred passes instead
	if (!renderer)
		return;
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

	fogDrawsThisFrame++;
	if (settings.LogDepthMap)
		fogDrawTargets += std::format("{}/{} ", colorIndex, depthIndex);

	// The live depth holds the water surfaces; it can only be read once it is no longer bound
	globals::d3d::context->OMSetRenderTargets(0, nullptr, nullptr);
	DrawGroundFog(main.UAV, fogDebugMode == FOG_DEBUG_PREWATER_DEPTH ? Util::GetCurrentSceneDepthSRV(false) : depth.depthSRV);
	globals::game::shadowState->GetRuntimeData().stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void FluidField::DrawGroundFogBeforeWater(ID3D11UnorderedAccessView* a_main)
{
	if (!loaded || !settings.Enabled)
		return;
	// Snapshot the main camera while it is bound: after the world draws, the per-frame buffer can hold another view's camera
	auto* perFrame = *globals::game::perFrame;
	if (perFrame) {
		D3D11_BUFFER_DESC source{};
		perFrame->GetDesc(&source);
		D3D11_BUFFER_DESC existing{};
		if (cameraSnapshot)
			cameraSnapshot->GetDesc(&existing);
		if (!cameraSnapshot || existing.ByteWidth != source.ByteWidth) {
			cameraSnapshot = nullptr;
			D3D11_BUFFER_DESC desc{ .ByteWidth = source.ByteWidth, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_CONSTANT_BUFFER, .CPUAccessFlags = 0, .MiscFlags = 0, .StructureByteStride = 0 };
			if (SUCCEEDED(globals::d3d::device->CreateBuffer(&desc, nullptr, cameraSnapshot.put())))
				Util::SetResourceName(cameraSnapshot.get(), "FluidField::CameraSnapshot");
		}
		if (cameraSnapshot)
			globals::d3d::context->CopyResource(cameraSnapshot.get(), perFrame);
	}
	// D159: release last frame's river capture views before this frame's capture
	for (auto& capture : riverCaptures | std::views::values)
		capture.ReleaseViews();
	if (fogDebugMode == FOG_DEBUG_BEFORE_WATER && settings.InteractiveFog && fogReady)
		DrawGroundFog(a_main, Util::GetCurrentSceneDepthSRV(false));
	// D123: the scene before the water, for the see-through falls to refract
	if (RefractiveFallsActive() && globals::game::renderer) {
		auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		fallRefraction.CopyBeforeWater(globals::d3d::device, globals::d3d::context, main.texture, main.RTV, globals::state->frameCount);
	}
	// D192: time the engine's water (and its Water.hlsl falls) until DrawGroundFogAfterWorld
	gpuSpans.Begin(globals::d3d::context, FluidFieldSim::GpuSpans::EngineWater);
}

void FluidField::DrawGroundFog(ID3D11UnorderedAccessView* a_main, ID3D11ShaderResourceView* a_depth)
{
	if (!loaded || !settings.Enabled || !settings.InteractiveFog || !fogReady || settings.FogDensity <= 0.0f || !a_main || !a_depth || groundFogFailed)
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
		.debugMode = static_cast<uint32_t>(fogDebugMode),
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
	ID3D11Buffer* perFrame = cameraSnapshot && fogDebugMode != FOG_DEBUG_LIVE_CAMERA ? cameraSnapshot.get() : *globals::game::perFrame;
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

void FluidField::BindWaterOutputs(ID3D11DeviceContext* a_context) const
{
	if (!waterShaderData.Enabled)
		return;
	const bool hasFar = waterShaderData.FarEnabled != 0;
	ID3D11ShaderResourceView* srvs[] = { nearWater.solver.GetSurfaceSRV(), nearWater.solver.GetFlowSRV(),
		hasFar ? farWater.solver.GetSurfaceSRV() : nullptr, hasFar ? farWater.solver.GetFlowSRV() : nullptr };
	a_context->PSSetShaderResources(66, 4, srvs);
}

void FluidField::StepWater(ID3D11DeviceContext* a_context, WaterLevel& a_level, bool a_near, uint32_t a_resolution, float a_size, const RE::NiPoint3& a_playerPos, float a_deltaTime)
{
	if (!EnsureWater(a_level, a_resolution, a_size, a_near))
		return;
	auto& levelSolver = a_level.solver;
	auto& map = a_level.map;

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
		globals::profiler->BeginPass(a_near ? "FluidField::Water Map" : "FluidField::Far Water Map");
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
	// Waterfalls drive both levels (D22); wakes come first
	if (settings.Waterfalls) {
		// D35: the falls' own plunges first, along their whole width; D22's single pushes after
		fallSites.AppendWaterEmitters(levelEmitters, a_gridMin, a_cellSize, a_resolution, settings.WaterfallStrength, FluidFieldSim::ShallowWaterSolver::MAX_EMITTERS, a_near);
	}
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
	// The near level inherits far foam for cells entering it, so foam doesn't reset as the player approaches
	// The falls' plunges foam the water along their whole width (D44)
	if (settings.Waterfalls && fallPlungeSRV) {
		params.plungeMap = fallPlungeSRV.get();
		params.plungeOrigin[0] = fallGroundOrigin[0];
		params.plungeOrigin[1] = fallGroundOrigin[1];
		params.plungeInverseCell = 1.0f / FALL_GROUND_CELL;
		params.plungeSize = FALL_GROUND_SIZE;
		params.lipMap = fallLipSRV.get();  // D170: on the plunge map's lattice
	}
	if (a_near && settings.FarWater && farWater.solver.HasMap() && waterShaderData.FarEnabled) {
		params.seedFoam = farWater.solver.GetFlowSRV();
		params.seedGridMin[0] = farWater.gridMin[0];
		params.seedGridMin[1] = farWater.gridMin[1];
		params.seedRes = settings.WaterResolution;
		params.seedCellSize = settings.WaterSize * settings.FarWaterScale / static_cast<float>(settings.WaterResolution);
	}
	// River currents from Unified Water's flowmap; the test current when there is none
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
	const float currentHeading = settings.TestCurrentAngle * std::numbers::pi_v<float> / 180.0f;
	params.current[0] = std::sin(currentHeading) * settings.TestCurrentSpeed;
	params.current[1] = std::cos(currentHeading) * settings.TestCurrentSpeed;

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
		globals::profiler->BeginPass(std::format("FluidField::{}{}", a_near ? "" : "Far ", passName));
		runPass();
		globals::profiler->EndPass();
	};
	if (a_level.warmupSubsteps > 0) {
		ZoneScopedN("FluidField - Water Warm-up");
		auto warmup = grid;
		warmup.deltaTime = WARMUP_SUBSTEP;
		const auto noTiming = [](const char*, auto&& runPass) { runPass(); };
		globals::profiler->BeginPass(a_near ? "FluidField::Water Warm-up" : "FluidField::Far Water Warm-up");
		for (uint32_t i = 0; i < a_level.warmupSubsteps; i++)
			levelSolver.Step(a_context, warmup, params, {}, noTiming);
		globals::profiler->EndPass();
		a_level.warmupSubsteps = 0;
	}
	for (uint i = 0; i < substeps; i++)
		levelSolver.Step(a_context, grid, params, emitters, wrap);

	globals::profiler->BeginPass(a_near ? "FluidField::Water Output" : "FluidField::Far Water Output");
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

void FluidField::ReleaseFallGround()
{
	fallGroundTexture = nullptr;
	fallGroundSRV = nullptr;
	fallClipBuffer = nullptr;
	fallClipSRV = nullptr;
	fallClipVersion = UINT64_MAX;
	fallBakeSRVs.clear();
	fallBakeVersion = UINT64_MAX;
	fallPlungeTexture = nullptr;
	fallPlungeSRV = nullptr;
	fallLipTexture = nullptr;
	fallLipSRV = nullptr;
	fallGroundVersion = UINT64_MAX;
}

void FluidField::UpdateFallGround(const RE::NiPoint3& a_playerPos)
{
	auto* device = globals::d3d::device;
	auto* context = globals::d3d::context;
	if (!fallGroundTexture) {
		D3D11_TEXTURE2D_DESC desc{ .Width = FALL_GROUND_SIZE, .Height = FALL_GROUND_SIZE, .MipLevels = 1, .ArraySize = 1, .Format = DXGI_FORMAT_R32_FLOAT, .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = 0 };
		D3D11_TEXTURE2D_DESC plungeDesc = desc;
		plungeDesc.Format = DXGI_FORMAT_R16_FLOAT;
		if (FAILED(device->CreateTexture2D(&plungeDesc, nullptr, fallPlungeTexture.put())) || FAILED(device->CreateShaderResourceView(fallPlungeTexture.get(), nullptr, fallPlungeSRV.put()))) {
			fallPlungeTexture = nullptr;
			fallPlungeSRV = nullptr;
		} else {
			Util::SetResourceName(fallPlungeTexture.get(), "FluidField::FallPlunge");
			Util::SetResourceName(fallPlungeSRV.get(), "FluidField::FallPlunge SRV");
		}
		// D170: the lips' draw on the river: way and speed (xy), weight (z)
		D3D11_TEXTURE2D_DESC lipDesc = desc;
		lipDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		if (FAILED(device->CreateTexture2D(&lipDesc, nullptr, fallLipTexture.put())) || FAILED(device->CreateShaderResourceView(fallLipTexture.get(), nullptr, fallLipSRV.put()))) {
			fallLipTexture = nullptr;
			fallLipSRV = nullptr;
		} else {
			Util::SetResourceName(fallLipTexture.get(), "FluidField::FallLip");
			Util::SetResourceName(fallLipSRV.get(), "FluidField::FallLip SRV");
		}
		desc.Format = DXGI_FORMAT_R32G32_FLOAT;  // The rock, and the pools' surface (D36)
		if (FAILED(device->CreateTexture2D(&desc, nullptr, fallGroundTexture.put())) || FAILED(device->CreateShaderResourceView(fallGroundTexture.get(), nullptr, fallGroundSRV.put()))) {
			logger::warn("[Fluid Field] Could not create the waterfall ground map; waterfalls are not grounded");
			ReleaseFallGround();
			return;
		}
		Util::SetResourceName(fallGroundTexture.get(), "FluidField::FallGround");
		Util::SetResourceName(fallGroundSRV.get(), "FluidField::FallGround SRV");
	}

	// D56: the falls' per-vertex rock cut, whenever more meshes are tested
	if (fallSites.GetVertexClipVersion() != fallClipVersion && !fallSites.GetVertexClips().empty()) {
		fallClipVersion = fallSites.GetVertexClipVersion();
		const auto& clips = fallSites.GetVertexClips();
		D3D11_BUFFER_DESC clipDesc{ .ByteWidth = static_cast<UINT>(clips.size() * sizeof(clips[0])), .Usage = D3D11_USAGE_IMMUTABLE, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, .StructureByteStride = sizeof(clips[0]) };
		const D3D11_SUBRESOURCE_DATA clipData{ clips.data(), 0, 0 };
		D3D11_SHADER_RESOURCE_VIEW_DESC clipView{ .Format = DXGI_FORMAT_UNKNOWN, .ViewDimension = D3D11_SRV_DIMENSION_BUFFER };
		clipView.Buffer.FirstElement = 0;
		clipView.Buffer.NumElements = static_cast<UINT>(clips.size());
		fallClipBuffer = nullptr;
		fallClipSRV = nullptr;
		if (FAILED(device->CreateBuffer(&clipDesc, &clipData, fallClipBuffer.put())) || FAILED(device->CreateShaderResourceView(fallClipBuffer.get(), &clipView, fallClipSRV.put()))) {
			fallClipBuffer = nullptr;
			fallClipSRV = nullptr;
		} else {
			Util::SetResourceName(fallClipBuffer.get(), "FluidField::FallVertexClip");
			Util::SetResourceName(fallClipSRV.get(), "FluidField::FallVertexClip SRV");
		}
	}

	// D59: the lit meshes' flow bakes, one texture each
	if (fallSites.GetFlowBakeVersion() != fallBakeVersion) {
		fallBakeVersion = fallSites.GetFlowBakeVersion();
		const auto& bakes = fallSites.GetFlowBakes();
		// every one again: a bake changes in place (relinked, its plunges written in, a site bake hot-reloaded), not only new ones
		fallBakeSRVs.clear();
		for (const auto& [geometry, bake] : bakes) {
			D3D11_TEXTURE2D_DESC bakeDesc{ .Width = bake.width, .Height = bake.height * static_cast<UINT>(bake.texels.size() / (static_cast<std::size_t>(bake.width) * bake.height * 4)), .MipLevels = 1, .ArraySize = 1, .Format = DXGI_FORMAT_R32G32B32A32_FLOAT, .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_IMMUTABLE, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = 0 };
			const D3D11_SUBRESOURCE_DATA bakeData{ bake.texels.data(), bake.width * 4 * sizeof(float), 0 };
			winrt::com_ptr<ID3D11Texture2D> texture;
			winrt::com_ptr<ID3D11ShaderResourceView> view;
			if (FAILED(device->CreateTexture2D(&bakeDesc, &bakeData, texture.put())) || FAILED(device->CreateShaderResourceView(texture.get(), nullptr, view.put())))
				continue;
			Util::SetResourceName(texture.get(), "FluidField::FallFlowBake");
			Util::SetResourceName(view.get(), "FluidField::FallFlowBake SRV");
			fallBakeSRVs[geometry] = view;
		}
	}

	// Recentred on the player when the captures change, or when the player has moved far
	constexpr float half = 0.5f * FALL_GROUND_SIZE * FALL_GROUND_CELL;
	const float dx = a_playerPos.x - (fallGroundOrigin[0] + half);
	const float dy = a_playerPos.y - (fallGroundOrigin[1] + half);
	const bool plungeTuned = plungeFoamBuilt[0] != plungeFoamBehind || plungeFoamBuilt[1] != plungeFoamRadius;  // D176: rebuilt live
	if (plungeTuned || fallSites.GetGroundVersion() != fallGroundVersion || dx * dx + dy * dy >= FALL_GROUND_RECENTRE * FALL_GROUND_RECENTRE) {
		plungeFoamBuilt[0] = plungeFoamBehind;
		plungeFoamBuilt[1] = plungeFoamRadius;
		fallGroundVersion = fallSites.GetGroundVersion();
		fallGroundOrigin[0] = std::floor((a_playerPos.x - half) / FALL_GROUND_CELL) * FALL_GROUND_CELL;
		fallGroundOrigin[1] = std::floor((a_playerPos.y - half) / FALL_GROUND_CELL) * FALL_GROUND_CELL;
		fallSites.BuildGroundMap(fallGroundOrigin[0], fallGroundOrigin[1], FALL_GROUND_CELL, FALL_GROUND_SIZE, fallGroundScratch);
		fallSites.BuildPoolMap(fallGroundOrigin[0], fallGroundOrigin[1], FALL_GROUND_CELL, FALL_GROUND_SIZE, fallPoolScratch);
		std::vector<float> interleaved(fallGroundScratch.size() * 2);
		for (std::size_t i = 0; i < fallGroundScratch.size(); i++) {
			interleaved[i * 2] = fallGroundScratch[i];
			interleaved[i * 2 + 1] = fallPoolScratch[i];
		}
		context->UpdateSubresource(fallGroundTexture.get(), 0, nullptr, interleaved.data(), FALL_GROUND_SIZE * 2 * sizeof(float), 0);
		if (fallPlungeTexture) {
			fallSites.BuildPlungeMap(fallGroundOrigin[0], fallGroundOrigin[1], FALL_GROUND_CELL, FALL_GROUND_SIZE, fallPlungeScratch, plungeFoamBehind, plungeFoamRadius);
			std::vector<uint16_t> halves(fallPlungeScratch.size());
			for (std::size_t i = 0; i < halves.size(); i++)
				halves[i] = DirectX::PackedVector::XMConvertFloatToHalf(fallPlungeScratch[i]);
			context->UpdateSubresource(fallPlungeTexture.get(), 0, nullptr, halves.data(), FALL_GROUND_SIZE * sizeof(uint16_t), 0);
		}
		if (fallLipTexture) {
			fallSites.BuildLipMap(fallGroundOrigin[0], fallGroundOrigin[1], FALL_GROUND_CELL, FALL_GROUND_SIZE, fallLipScratch);
			std::vector<uint16_t> halves(fallLipScratch.size());
			for (std::size_t i = 0; i < halves.size(); i++)
				halves[i] = DirectX::PackedVector::XMConvertFloatToHalf(fallLipScratch[i]);
			context->UpdateSubresource(fallLipTexture.get(), 0, nullptr, halves.data(), FALL_GROUND_SIZE * 4 * sizeof(uint16_t), 0);
		}
	}
	fallGroundData = { { fallGroundOrigin[0], fallGroundOrigin[1] }, 1.0f / FALL_GROUND_CELL, static_cast<float>(FALL_GROUND_SIZE), 1u,
		static_cast<uint32_t>(std::clamp(settings.WaterfallDebug, 0, 5)), fallSurfaceTime, FALL_POOL_FOAM, {}, {}, {}, settings.FallWhitening ? 1u : 0u, UINT32_MAX,
		settings.HideUnderwaterFalls ? 1u : 0u, { 1.0f, 1.0f }, 0u, {}, {} };
	fallGroundData.AerationLook[0] = settings.FallFoamStart;
	fallGroundData.AerationLook[1] = settings.FallAerationFoam;
	fallGroundData.AerationLook[2] = settings.FallAerationMilk;
	fallGroundData.AerationLook[3] = settings.FallAerationFrom;
}

void FluidField::PrepareFallGroundData(const RE::BSGeometry* a_geometry, RE::BSShaderProperty* a_property, ID3D11ShaderResourceView*& o_bake)
{
	// D32: the flowmap moves each mesh's texture itself, so vanilla's scroll is taken back out of its coordinates
	auto* property = a_property;
	auto* material = property ? property->material : nullptr;
	const uint32_t buffer = globals::game::smState ? globals::game::smState->textureTransformCurrentBuffer : 0;
	fallGroundData.TexOffset[0] = material ? material->texCoordOffset[buffer].x : 0.0f;
	fallGroundData.TexOffset[1] = material ? material->texCoordOffset[buffer].y : 0.0f;
	// D56: vanilla's scroll is the falls' forward (+v as the v offset falls), measured over >= 0.25 s as the offset wraps
	// D85: one phase per frame, latched at the first falls draw, so the depth pre-pass and lit pass get identical positions
	if (const uint32_t frame = globals::state->frameCount; frame != fallPhaseFrame) {
		fallPhaseFrame = frame;
		fallDrawPhasePrevious = fallDrawPhase;
		fallDrawPhase = fallPhase;
	}
	fallGroundData.Scroll[0] = fallDrawPhase;
	fallGroundData.Scroll[1] = fallRate > 0.0f ? fallRate : DEFAULT_FALL_SCROLL;
	fallGroundData.PreviousPhase = fallDrawPhasePrevious;
	if (material) {
		auto& scroll = fallScrolls[material];
		const float offset = material->texCoordOffset[buffer].y;
		const float elapsed = fallSurfaceTime - scroll.time;
		const float scaleNow = std::max(material->texCoordScale[buffer].y, 1e-4f);
		if (!scroll.known || elapsed < 0.0f || elapsed > 5.0f) {
			const bool first = !scroll.known;
			scroll.offset = offset;
			scroll.time = fallSurfaceTime;
			scroll.known = true;
			if (first) {
				scroll.scaleTime = fallSurfaceTime;
				scroll.scale = scaleNow;
			}
		} else if (elapsed >= 0.25f) {
			float delta = offset - scroll.offset;
			delta -= std::round(delta);
			const float rate = -delta / elapsed;
			scroll.rate = scroll.rate == 0.0f ? rate : scroll.rate + (rate - scroll.rate) * 0.3f;
			scroll.offset = offset;
			scroll.time = fallSurfaceTime;
		}
		// D60: vanilla's texture scale averaged once a frame, so its slow zoom stays out of the falls' rate (D67)
		if (scroll.scaleTime != fallSurfaceTime) {
			const float step = std::clamp(fallSurfaceTime - scroll.scaleTime, 0.0f, 0.25f);
			scroll.scale += (scaleNow - scroll.scale) * std::min(step * 0.1f, 1.0f);
			scroll.scaleTime = fallSurfaceTime;
		}
		// D67: the falls use the shared scroll; this material's only measures vanilla's rate
	}
	// D56: the per-vertex cut
	uint32_t vertexBase = 0;
	fallGroundData.VertexBase = fallClipSRV && fallSites.GetVertexClip(a_geometry, vertexBase) && vertexBase < fallSites.GetVertexClips().size() ? vertexBase : UINT32_MAX;
	// D57: vanilla's slow texture zoom is taken out too
	fallGroundData.InverseScale[0] = material && material->texCoordScale[buffer].x > 1e-4f ? 1.0f / material->texCoordScale[buffer].x : 1.0f;
	fallGroundData.InverseScale[1] = material && material->texCoordScale[buffer].y > 1e-4f ? 1.0f / material->texCoordScale[buffer].y : 1.0f;
	// D59: this mesh's flow bake, where it is solved
	o_bake = nullptr;
	fallGroundData.BakeOn = 0;
	fallGroundData.BakeVelocity = fallSites.IsUnified(a_geometry) ? 1u : 0u;
	if (const auto it = fallBakeSRVs.find(a_geometry); it != fallBakeSRVs.end()) {
		const auto& bakes = fallSites.GetFlowBakes();
		if (const auto bake = bakes.find(a_geometry); bake != bakes.end()) {
			o_bake = it->second.get();
			fallGroundData.BakeOn = 1;
			fallGroundData.BakeRect[0] = bake->second.uvMin[0];
			fallGroundData.BakeRect[1] = bake->second.uvMin[1];
			fallGroundData.BakeRect[2] = 1.0f / std::max(bake->second.uvSize[0], 1e-4f);
			fallGroundData.BakeRect[3] = 1.0f / std::max(bake->second.uvSize[1], 1e-4f);
		}
	}
	// D38: the vertex shaders' camera offset (Lighting has none of its own)
	// D44: this pass's own, since reflections use a mirrored camera
	const auto eye = globals::game::shadowState->GetRuntimeData().posAdjust.getEye();
	fallGroundData.CameraOffset[0] = eye.x;
	fallGroundData.CameraOffset[1] = eye.y;
	fallGroundData.CameraOffset[2] = eye.z;
	fallGroundData.BlendAlpha = 0.0f;
}

void FluidField::SetupWaterGeometry(RE::BSRenderPass* a_pass)
{
	waterGeometry = a_pass ? a_pass->geometry : nullptr;
}

void FluidField::CaptureRiverDraw(RE::BSShader* a_shader, uint32_t a_vertexDescriptor, uint32_t a_pixelDescriptor)
{
	// D159: cheap tests first: this runs before every draw call
	if (!FluidFieldSim::RiverCapture::IsRiverDraw(a_shader, a_pixelDescriptor) || !RefractiveFallsActive())
		return;
	// D171: one capture per water type (of the water object owning this shape), at its first draw of the frame
	const RE::TESWaterForm* waterType = nullptr;
	if (auto* waterSystem = RE::TESWaterSystem::GetSingleton(); waterSystem && waterGeometry) {
		RE::BSSpinLockGuard guard(waterSystem->lock);
		for (const auto& object : waterSystem->waterObjects) {
			if (object && object->shape.get() == waterGeometry) {
				waterType = object->waterType;
				break;
			}
		}
	}
	const uint32_t frame = globals::state->frameCount;
	auto [capture, added] = riverCaptures.try_emplace(waterType);
	if (added)
		logger::info("[Fluid Field] Falls take the river shader's state of water type {:08X} '{}'", waterType ? waterType->GetFormID() : 0, waterType ? waterType->GetFormEditorID() : "(no water object)");
	// D171: keep capturing the same river piece while it is drawn, else the falls flip between its pieces' materials
	const auto* piece = riverPieces.try_emplace(waterType, waterGeometry).first->second;
	if (capture->second.WasTried(frame) && (waterGeometry != piece || capture->second.GetGeometry() == piece))
		return;
	if (capture->second.Capture(globals::d3d::device, globals::d3d::context, a_shader, a_vertexDescriptor, a_pixelDescriptor, frame))
		capture->second.SetGeometry(waterGeometry);
}

bool FluidField::RefractiveFallsActive() const
{
	return loaded && settings.Enabled && settings.WaterEnabled && settings.Waterfalls && fallGroundSRV && fallRefraction.IsUsable();
}

namespace
{
	/** @brief D98: how far above a falls piece's bound its source water may lie. */
	constexpr float FALL_WATER_ABOVE_TOP = 64.0f;

	/**
	 * @brief D98: the water type a falls piece pours out of: the highest overlapping water object not above its top (so the
	 * river at its crest beats the pool below), else the cell's water at its top, else the worldspace's.
	 */
	const RE::TESWaterForm* FindFallWaterType(const RE::BSGeometry* a_geometry)
	{
		const auto& bound = a_geometry->worldBound;
		const bool hasBound = std::isfinite(bound.radius) && bound.radius > 1.0f;
		const RE::NiPoint3 centre = hasBound ? bound.center : a_geometry->world.translate;
		const float radius = hasBound ? bound.radius : 256.0f;
		const float top = centre.z + radius;
		const RE::TESWaterForm* best = nullptr;
		float bestHeight = -FLT_MAX;
		if (auto* waterSystem = RE::TESWaterSystem::GetSingleton()) {
			RE::BSSpinLockGuard guard(waterSystem->lock);
			for (const auto& object : waterSystem->waterObjects) {
				if (!object || !object->waterType)
					continue;
				for (const auto& box : object->multiBounds) {
					if (!box)
						continue;
					if (box->center.x + box->size.x < centre.x - radius || box->center.x - box->size.x > centre.x + radius ||
						box->center.y + box->size.y < centre.y - radius || box->center.y - box->size.y > centre.y + radius)
						continue;
					// A sloped bound (rapids into the falls) counts from its lower end; one wholly above the top is another river
					if (box->center.z - box->size.z > top + FALL_WATER_ABOVE_TOP)
						continue;
					const float height = std::min(box->center.z + box->size.z, top + FALL_WATER_ABOVE_TOP);
					if (height > bestHeight) {
						bestHeight = height;
						best = object->waterType;
					}
				}
			}
		}
		if (best)
			return best;
		auto* tes = RE::TES::GetSingleton();
		if (!tes)
			return nullptr;
		if (auto* cell = tes->GetCell(RE::NiPoint3(centre.x, centre.y, top))) {
			if (auto* extra = cell->extraList.GetByType<RE::ExtraCellWaterType>(); extra && extra->water)
				return extra->water;
		}
		auto* worldSpace = tes->GetRuntimeData2().worldSpace;
		return worldSpace ? worldSpace->worldWater : nullptr;
	}

	/**
	 * @brief D98: a water type's look as Water.hlsl reads it: colours under the weather multiplier, fog, fresnel,
	 * reflection and glint. Defaults when there is no water type.
	 */
	FluidFieldSim::FallRefraction::WaterLook MakeFallWaterLook(const RE::TESWaterForm* a_water, const float (&a_multiplier)[3])
	{
		FluidFieldSim::FallRefraction::WaterLook look;
		if (!a_water)
			return look;
		const auto& data = a_water->data;
		const auto channel = [](const RE::Color& a_color, int a_k) {
			return (a_k == 0 ? a_color.red : a_k == 1 ? a_color.green :
														a_color.blue) /
			       255.0f;
		};
		for (int k = 0; k < 3; k++) {
			look.shallowColor[k] = channel(data.shallowWaterColor, k) * a_multiplier[k];
			look.deepColor[k] = channel(data.deepWaterColor, k) * a_multiplier[k];
			look.reflectionColor[k] = channel(data.reflectionWaterColor, k) * a_multiplier[k] * data.reflectionAmount;
		}
		look.fogNear = data.aboveWaterFogDistNear;
		look.fogFar = data.aboveWaterFogDistFar;
		look.fogAmount = data.aboveWaterFogAmount;
		look.fresnel = data.fresnelAmount;
		look.sunSpecularPower = data.sunSpecularPower;
		look.sunSpecularMagnitude = data.sunSpecularMagnitude;
		look.reflectionMagnitude = data.reflectionMagnitude;
		// D129: its normal maps, scale and amplitude (the river's fine ripples)
		// D138: the form's textures are empty outside the water shader's use, so each is loaded by path and kept alive
		static std::unordered_map<std::string, RE::NiPointer<RE::NiTexture>> loaded;
		for (int k = 0; k < 3; k++) {
			RE::NiTexture* texture = a_water->noiseTextureData[k].get();
			if (!texture && !a_water->noiseTextures[k].textureName.empty()) {
				std::string path = a_water->noiseTextures[k].textureName.c_str();
				if (_strnicmp(path.c_str(), "textures\\", 9) != 0 && _strnicmp(path.c_str(), "textures/", 9) != 0)
					path = "textures\\" + path;
				auto& slot = loaded[path];
				if (!slot)
					RE::BSShaderManager::GetTexture(path.c_str(), true, slot, false);
				texture = slot.get();
			}
			auto* source = texture && texture->GetRTTI() == globals::rtti::NiSourceTextureRTTI.get() ? static_cast<RE::NiSourceTexture*>(texture) : nullptr;
			look.noise[k] = source && source->rendererTexture ? source->rendererTexture->resourceView : nullptr;
			look.noiseScale[k] = data.uvScaleA[k];
			look.noiseAmplitude[k] = data.amplitudeA[k];
		}
		// D134: log once per water type whether its normal maps were found
		static std::unordered_set<const RE::TESWaterForm*> logged;
		if (logged.insert(a_water).second)
			logger::info("[Fluid Field] Falls water '{}': normal maps {} {} {} (scale {:.0f} {:.0f} {:.0f}, amplitude {:.2f} {:.2f} {:.2f})", a_water->GetFormEditorID(),
				look.noise[0] ? "yes" : "no", look.noise[1] ? "yes" : "no", look.noise[2] ? "yes" : "no", look.noiseScale[0], look.noiseScale[1], look.noiseScale[2],
				look.noiseAmplitude[0], look.noiseAmplitude[1], look.noiseAmplitude[2]);
		return look;
	}
}

bool FluidField::DrawRefractiveFalls(ID3D11RenderTargetView* a_target, ID3D11Texture2D* a_targetTexture, ID3D11DepthStencilView* a_depth, ID3D11ShaderResourceView* a_sceneDepth)
{
	if (!RefractiveFallsActive() || !cameraSnapshot)
		return false;
	// Every lit piece's constants first (each MeshDraw points into this), then its draw
	const auto& lit = fallSites.GetLitMeshes();
	std::vector<FallGroundData> grounds;
	grounds.reserve(lit.size());
	std::vector<FluidFieldSim::FallRefraction::MeshDraw> draws;
	draws.reserve(lit.size());
	std::vector<const RE::TESWaterForm*> drawWaters;  // D171: each draw's water type
	drawWaters.reserve(lit.size());
	std::unordered_set<const RE::BSGeometry*> keep;
	// D98: each piece takes its source water's look (re-looked up every few seconds)
	if (++fallWaterTypesAge >= FALL_WATER_REFRESH) {
		fallWaterTypesAge = 0;
		fallWaterTypes.clear();
	}
	float multiplier[3] = { 1.0f, 1.0f, 1.0f };
	if (auto* sky = globals::game::sky) {
		const auto& color = sky->skyColor[RE::TESWeather::ColorTypes::kWaterMultiplier];
		multiplier[0] = color.red;
		multiplier[1] = color.green;
		multiplier[2] = color.blue;
	}
	std::unordered_map<const RE::TESWaterForm*, FluidFieldSim::FallRefraction::WaterLook> looks;
	for (const auto& piece : lit) {
		auto* geometry = piece.geometry.get();
		auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
		if (!geometry || !property || !fallSites.IsGrounded(geometry))
			continue;
		keep.insert(geometry);
		FluidFieldSim::FallRefraction::MeshDraw draw;
		draw.geometry = geometry;
		draw.normals = fallSites.GetShiftedNormals(geometry);  // D186: the light on the surface as its pushes leave it
		draw.normalsVersion = fallSites.GetShiftedNormalsVersion();
		PrepareFallGroundData(geometry, property, draw.bake);
		grounds.push_back(fallGroundData);
		const auto& world = geometry->world;
		for (int row = 0; row < 3; row++) {
			for (int column = 0; column < 3; column++)
				draw.world[row * 4 + column] = world.rotate.entry[row][column] * world.scale;
		}
		draw.world[3] = world.translate.x;
		draw.world[7] = world.translate.y;
		draw.world[11] = world.translate.z;
		if (auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(property)) {
			auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(lighting->GetBaseMaterial());
			const auto view = [](const RE::NiPointer<RE::NiSourceTexture>& a_texture) -> ID3D11ShaderResourceView* {
				return a_texture && a_texture->rendererTexture ? a_texture->rendererTexture->resourceView : nullptr;
			};
			draw.diffuse = material ? view(material->diffuseTexture) : nullptr;
			draw.normal = material ? view(material->normalTexture) : nullptr;
		}
		auto waterType = fallWaterTypes.find(geometry);
		if (waterType == fallWaterTypes.end())
			waterType = fallWaterTypes.emplace(geometry, FindFallWaterType(geometry)).first;
		auto look = looks.find(waterType->second);
		if (look == looks.end())
			look = looks.emplace(waterType->second, MakeFallWaterLook(waterType->second, multiplier)).first;
		draw.look = look->second;
		draws.push_back(draw);
		drawWaters.push_back(waterType->second);
	}
	for (std::size_t i = 0; i < draws.size(); i++) {
		draws[i].ground = &grounds[i];
		draws[i].groundSize = sizeof(FallGroundData);
	}
	fallRefraction.Prune(keep);
	std::erase_if(fallWaterTypes, [&](const auto& a_entry) { return !keep.contains(a_entry.first); });
	if (draws.empty())
		return false;

	FluidFieldSim::FallRefraction::DrawParams params;
	params.target = a_target;
	params.motion = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].RTV;
	params.targetTexture = a_targetTexture;
	params.frame = globals::state->frameCount;
	params.time = fallSurfaceTime;
	// D151: the sky's distance fog, as the river's water shader receives it (its colours blended over the day and weather)
	if (auto* sky = globals::game::sky; sky && sky->fogFar > sky->fogNear) {
		const auto& nearColor = sky->skyColor[RE::TESWeather::ColorTypes::kFogNear];
		const auto& farColor = sky->skyColor[RE::TESWeather::ColorTypes::kFogFar];
		params.skyFogNearColor[0] = nearColor.red;
		params.skyFogNearColor[1] = nearColor.green;
		params.skyFogNearColor[2] = nearColor.blue;
		params.skyFogFarColor[0] = farColor.red;
		params.skyFogFarColor[1] = farColor.green;
		params.skyFogFarColor[2] = farColor.blue;
		params.skyFogNear = sky->fogNear;
		params.skyFogFar = sky->fogFar;
		params.skyFogPower = std::isfinite(sky->fogPower) ? std::clamp(sky->fogPower, 0.01f, 16.0f) : 1.0f;
		params.skyFogClamp = std::isfinite(sky->fogClamp) ? std::clamp(sky->fogClamp, 0.0f, 1.0f) : 1.0f;
	}
	params.depth = a_depth;
	params.sceneDepth = a_sceneDepth;
	params.camera = cameraSnapshot.get();
	params.shared = globals::state->sharedDataCB->CB();
	params.features = globals::state->featureDataCB->CB();
	params.vertexClip = fallClipSRV.get();
	params.poolHeights = fallGroundSRV.get();
	// D98: the cubemap the engine's shaders get: reflections only when active and captured, else the plain one once valid;
	// an uncaptured reflections texture is undefined and shows as bright squares
	auto& dynamicCubemaps = globals::features::dynamicCubemaps;
	params.environment = nullptr;
	if (dynamicCubemaps.loaded) {
		if (dynamicCubemaps.activeReflections && dynamicCubemaps.cubemapValid[1] && dynamicCubemaps.envReflectionsTexture)
			params.environment = dynamicCubemaps.envReflectionsTexture->srv.get();
		else if (dynamicCubemaps.cubemapValid[0] && dynamicCubemaps.envTexture)
			params.environment = dynamicCubemaps.envTexture->srv.get();
	}
	// D159: falls shaded by the river's own shader (no per-vertex, flowmap or wading variants), compiled in the background; not
	// drawn until it is ready. D171: frame state (shadows, lights, SSR, camera) from any water drawn this frame; each fall's
	// material from its water type's last capture, else the frame's
	const uint32_t frame = globals::state->frameCount;
	for (const auto& [waterType, capture] : riverCaptures) {
		// D171: the kept piece was not drawn this frame, so keep the one captured instead
		if (capture.IsCaptured(frame) && capture.GetGeometry() != riverPieces[waterType]) {
			riverPieces[waterType] = static_cast<const RE::BSGeometry*>(capture.GetGeometry());
		}
	}
	const FluidFieldSim::RiverCapture* frameCapture = nullptr;
	const RE::TESWaterForm* frameWater = nullptr;
	for (const auto& [waterType, capture] : riverCaptures) {
		// Prefer a water object's capture; a typeless one (LOD) only when nothing else was drawn
		if (capture.IsCaptured(frame) && capture.GetShader() && (!frameCapture || !frameWater)) {
			frameCapture = &capture;
			frameWater = waterType;
		}
	}
	// D159: the engine's textures are not held past the falls
	const auto releaseCaptures = [&]() {
		for (auto& capture : riverCaptures | std::views::values)
			capture.ReleaseViews();
	};
	if (!frameCapture) {
		releaseCaptures();
		return false;
	}
	using Flags = SIE::ShaderCache::WaterShaderFlags;
	constexpr auto notOnSheets = static_cast<uint32_t>(Flags::Vc) | static_cast<uint32_t>(Flags::NormalTexCoord) | static_cast<uint32_t>(Flags::Wading) |
	                             static_cast<uint32_t>(Flags::VertexAlphaDepth) | static_cast<uint32_t>(Flags::Flowmap) | static_cast<uint32_t>(Flags::BlendNormals);
	constexpr auto falls = static_cast<uint32_t>(Flags::FluidFieldFalls);
	const auto shadersOf = [&](const FluidFieldSim::RiverCapture& a_capture) -> std::pair<ID3D11VertexShader*, ID3D11PixelShader*> {
		auto* vertexShader = globals::shaderCache->GetVertexShader(*a_capture.GetShader(), (a_capture.GetVertexDescriptor() & ~notOnSheets) | falls);
		auto* pixelShader = globals::shaderCache->GetPixelShader(*a_capture.GetShader(), (a_capture.GetPixelDescriptor() & ~notOnSheets) | falls);
		if (!vertexShader || !pixelShader || !vertexShader->shader || !pixelShader->shader)
			return { nullptr, nullptr };
		return { reinterpret_cast<ID3D11VertexShader*>(vertexShader->shader), reinterpret_cast<ID3D11PixelShader*>(pixelShader->shader) };
	};
	const auto [frameVertexShader, framePixelShader] = shadersOf(*frameCapture);
	if (!frameVertexShader || !framePixelShader) {
		releaseCaptures();
		return false;
	}
	params.river = frameCapture;
	params.riverVertexShader = frameVertexShader;
	params.riverPixelShader = framePixelShader;
	for (std::size_t i = 0; i < draws.size(); i++) {
		const auto own = riverCaptures.find(drawWaters[i]);
		if (own == riverCaptures.end() || !own->second.GetMaterialBuffer())
			continue;
		draws[i].riverMaterial = own->second.GetMaterialBuffer();
		if (own->second.IsCaptured(frame))
			draws[i].river = &own->second;
		const auto [vertexShader, pixelShader] = shadersOf(own->second);
		draws[i].riverVertexShader = vertexShader;
		draws[i].riverPixelShader = pixelShader;
	}
	const bool drawn = fallRefraction.Draw(globals::d3d::device, globals::d3d::context, params, draws);
	releaseCaptures();
	return drawn;
}
