#include "FluidFieldWaterfalls.h"

#include "../I18n/I18n.h"
#include "FluidField/Field.h"
#include "Menu.h"
#include "Profiler.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

#include "DynamicCubemaps.h"

#include <DirectXPackedVector.h>

#define I18N_KEY_PREFIX "feature.fluid_field_waterfalls."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FluidFieldWaterfalls::Settings,
	Enabled,
	WaterfallStrength,
	HideUnderwaterFalls,
	FallWhitening,
	FallSpeed,
	FallFoamStart,
	FallAerationFoam,
	FallAerationFrom,
	FallAerationMilk,
	ExportSiteInputs,
	HotReloadBakes)

void FluidFieldWaterfalls::DrawSettings()
{
	const auto tooltip = [](const char* a_text) {
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", a_text);
	};

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	tooltip(T(TKEY("enabled_tooltip"), "Draws the waterfalls as clear, flowing water shaded like the river they pour from."));

	ImGui::SeparatorText(T(TKEY("waterfall_look"), "Water"));
	ImGui::Checkbox(T(TKEY("fall_whitening"), "Foam on the Falls"), &settings.FallWhitening);
	tooltip(T(TKEY("fall_whitening_tooltip"), "White water on the falls where they strike rock, below rock, into the water below and as they run and fall."));
	ImGui::SliderFloat(T(TKEY("fall_foam_start"), "Foam Start Height"), &settings.FallFoamStart, 0.0f, 300.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("fall_foam_start_tooltip"), "How far the water falls, in world units, before it takes air or foam: the crest stays clear above it. Baked falls only."));
	ImGui::SliderFloat(T(TKEY("fall_aeration_foam"), "Aeration Foam"), &settings.FallAerationFoam, 0.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("fall_aeration_foam_tooltip"), "How much of the bake's aeration turns to white foam: higher makes the lower falls frothier."));
	ImGui::SliderFloat(T(TKEY("fall_aeration_from"), "Aeration Threshold"), &settings.FallAerationFrom, 0.0f, 0.95f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("fall_aeration_from_tooltip"), "The bake's aeration where that foam begins: lower starts the foam higher up the drop."));
	ImGui::SliderFloat(T(TKEY("fall_aeration_milk"), "Aeration Milk"), &settings.FallAerationMilk, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("fall_aeration_milk_tooltip"), "How milky aeration makes the clear water (0: stays clear)."));
	ImGui::SliderFloat(T(TKEY("fall_speed"), "Falls Speed"), &settings.FallSpeed, 0.5f, 3.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("fall_speed_tooltip"), "How fast the falls' water runs, against vanilla's own speed. The whole flow speeds up alike, so the texture never stretches."));
	ImGui::Checkbox(T(TKEY("hide_underwater_falls"), "Hide Buried Falls"), &settings.HideUnderwaterFalls);
	tooltip(T(TKEY("hide_underwater_falls_tooltip"), "Hides the parts of vanilla's waterfall meshes under the water above or below them, or deep in the rock."));
	ImGui::SliderFloat(T(TKEY("waterfall_strength"), "Plunge Strength"), &settings.WaterfallStrength, 0.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("waterfall_strength_tooltip"), "How hard the falls churn, push and foam the water they land in."));
}

#undef I18N_KEY_PREFIX

FluidFieldWaterfalls::Settings FluidFieldWaterfalls::Sanitize(Settings a_settings)
{
	const Settings defaults{};
	a_settings.WaterfallStrength = std::isfinite(a_settings.WaterfallStrength) ? std::clamp(a_settings.WaterfallStrength, 0.0f, 3.0f) : defaults.WaterfallStrength;
	// The look floats reach the shaders as they are, so only non-finite values are replaced
	for (auto [value, fallback] : { std::pair{ &a_settings.FallSpeed, defaults.FallSpeed }, std::pair{ &a_settings.FallFoamStart, defaults.FallFoamStart },
			 std::pair{ &a_settings.FallAerationFoam, defaults.FallAerationFoam }, std::pair{ &a_settings.FallAerationFrom, defaults.FallAerationFrom },
			 std::pair{ &a_settings.FallAerationMilk, defaults.FallAerationMilk } }) {
		if (!std::isfinite(*value))
			*value = fallback;
	}
	return a_settings;
}

void FluidFieldWaterfalls::LoadSettings(json& o_json)
{
	try {
		settings = o_json;
	} catch (const json::exception& e) {
		logger::warn("[Fluid Field] Malformed Waterfalls settings, using defaults: {}", e.what());
		settings = {};
	}
	settings = Sanitize(settings);
}

void FluidFieldWaterfalls::SaveSettings(json& o_json)
{
	o_json = settings;
}

void FluidFieldWaterfalls::RestoreDefaultSettings()
{
	settings = {};
}

void FluidFieldWaterfalls::PostPostLoad()
{
	FluidFieldSim::Field::Get().RegisterListeners();
	Hooks::Install();
}

void FluidFieldWaterfalls::Hooks::BSWaterShader_SetupGeometry::thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
{
	func(a_shader, a_pass, a_renderFlags);
	if (auto& waterfalls = globals::features::fluidFieldWaterfalls; waterfalls.loaded)
		waterfalls.SetupWaterGeometry(a_pass);
}

void FluidFieldWaterfalls::Hooks::Install()
{
	stl::write_vfunc<0x6, BSWaterShader_SetupGeometry>(RE::VTABLE_BSWaterShader[0]);
	logger::info("[Fluid Field] Installed hooks");
}

void FluidFieldWaterfalls::ClearShaderCache()
{
	fallRefraction.ReleaseShaders();
}

void FluidFieldWaterfalls::Release()
{
	fallSites.Clear();
	ReleaseFallGround();
	fallRefraction.Release();
	riverCaptures.clear();
	riverPieces.clear();
	waterGeometry = nullptr;
	fallWaterTypes.clear();
}

void FluidFieldWaterfalls::Prepass()
{
	auto& field = FluidFieldSim::Field::Get();
	if (!settings.Enabled) {
		// Disabled costs nothing: released once, then no work
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
	const float deltaTime = field.GetDeltaTime();
	if (!(deltaTime > 0.0f))
		return;

	// Waterfall sites reconstructed from vanilla's layout (D25)
	fallSites.SetExportSiteInputs(settings.ExportSiteInputs);
	fallSites.SetHotReloadBakes(settings.HotReloadBakes);
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
	fallSites.SetLandscape(&field.GetLandscape());  // D172
	fallSites.Update(field.GetWater());
	UpdateFallGround(field.GetPlayerPosition());
}

void FluidFieldWaterfalls::AppendWaterEmitters(std::vector<FluidFieldSim::WaterEmitter>& a_emitters, const int32_t a_gridMin[2], float a_cellSize, uint32_t a_gridRes,
	uint32_t a_maxEmitters, bool a_foam) const
{
	if (IsActive())
		fallSites.AppendWaterEmitters(a_emitters, a_gridMin, a_cellSize, a_gridRes, settings.WaterfallStrength, a_maxEmitters, a_foam);
}

bool FluidFieldWaterfalls::GetPlungeMaps(PlungeMaps& o_maps) const
{
	if (!IsActive() || !fallPlungeSRV)
		return false;
	o_maps.plunge = fallPlungeSRV.get();
	o_maps.lip = fallLipSRV.get();
	o_maps.origin[0] = fallGroundOrigin[0];
	o_maps.origin[1] = fallGroundOrigin[1];
	o_maps.inverseCell = 1.0f / FALL_GROUND_CELL;
	o_maps.size = FALL_GROUND_SIZE;
	return true;
}

void FluidFieldWaterfalls::DrawBeforeWater()
{
	// D159: release last frame's river capture views before this frame's capture
	for (auto& capture : riverCaptures | std::views::values)
		capture.ReleaseViews();
	// D123: the scene before the water, for the see-through falls to refract
	if (RefractiveFallsActive() && globals::game::renderer) {
		auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		fallRefraction.CopyBeforeWater(globals::d3d::device, globals::d3d::context, main.texture, main.RTV, globals::state->frameCount);
	}
}

void FluidFieldWaterfalls::DrawAfterWorld()
{
	if (!RefractiveFallsActive() || !FluidFieldSim::Field::Get().GetCameraSnapshot())
		return;
	auto* renderer = globals::game::renderer;
	const auto& shadow = globals::game::shadowState->GetRuntimeData();
	const auto colorIndex = static_cast<uint32_t>(shadow.renderTargets[0]);
	const auto depthIndex = shadow.depthStencil;
	if (colorIndex >= RE::RENDER_TARGET::kTOTAL || depthIndex >= RE::RENDER_TARGET_DEPTHSTENCIL::kTOTAL)
		return;
	auto& target = renderer->GetRuntimeData().renderTargets[colorIndex];
	auto& depth = renderer->GetDepthStencilData().depthStencils[depthIndex];
	// D105: depth behind from the post-prepass copy; the live depth's view reads as far everywhere
	auto* behind = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV;
	// D177: writable depth view: the falls write their depth so later passes (distance fog) see the sheet, not what is behind it
	auto& gpuSpans = FluidFieldSim::Field::Get().GetGpuSpans();
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

bool FluidFieldWaterfalls::RefractiveFallsActive() const
{
	return IsActive() && fallGroundSRV && fallRefraction.IsUsable();
}

void FluidFieldWaterfalls::ReleaseFallGround()
{
	fallGroundTexture = nullptr;
	fallGroundSRV = nullptr;
	fallClipBuffer = nullptr;
	fallClipSRV = nullptr;
	fallClipCapacity = 0;
	fallClipCount = 0;
	fallClipVersion = UINT64_MAX;
	fallBakeSRVs.clear();
	fallBakeVersion = UINT64_MAX;
	fallPlungeTexture = nullptr;
	fallPlungeSRV = nullptr;
	fallLipTexture = nullptr;
	fallLipSRV = nullptr;
	fallGroundVersion = UINT64_MAX;
	// The maps' CPU copies go too (released: disabled costs nothing)
	fallGroundScratch = {};
	fallPoolScratch = {};
	fallPlungeScratch = {};
	fallLipScratch = {};
	fallGroundUpload = {};
	fallHalfUpload = {};
}

void FluidFieldWaterfalls::UpdateFallGround(const RE::NiPoint3& a_playerPos)
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
			Util::SetResourceName(fallPlungeTexture.get(), "FluidFieldWaterfalls::FallPlunge");
			Util::SetResourceName(fallPlungeSRV.get(), "FluidFieldWaterfalls::FallPlunge SRV");
		}
		// D170: the lips' draw on the river: way and speed (xy), weight (z)
		D3D11_TEXTURE2D_DESC lipDesc = desc;
		lipDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		if (FAILED(device->CreateTexture2D(&lipDesc, nullptr, fallLipTexture.put())) || FAILED(device->CreateShaderResourceView(fallLipTexture.get(), nullptr, fallLipSRV.put()))) {
			fallLipTexture = nullptr;
			fallLipSRV = nullptr;
		} else {
			Util::SetResourceName(fallLipTexture.get(), "FluidFieldWaterfalls::FallLip");
			Util::SetResourceName(fallLipSRV.get(), "FluidFieldWaterfalls::FallLip SRV");
		}
		desc.Format = DXGI_FORMAT_R32G32_FLOAT;  // The rock, and the pools' surface (D36)
		if (FAILED(device->CreateTexture2D(&desc, nullptr, fallGroundTexture.put())) || FAILED(device->CreateShaderResourceView(fallGroundTexture.get(), nullptr, fallGroundSRV.put()))) {
			logger::warn("[Fluid Field] Could not create the waterfall ground map; waterfalls are not grounded");
			ReleaseFallGround();
			return;
		}
		Util::SetResourceName(fallGroundTexture.get(), "FluidFieldWaterfalls::FallGround");
		Util::SetResourceName(fallGroundSRV.get(), "FluidFieldWaterfalls::FallGround SRV");
	}

	// D56: the falls' per-vertex rock cut, whenever more meshes are tested; reallocated only to grow
	if (fallSites.GetVertexClipVersion() != fallClipVersion && !fallSites.GetVertexClips().empty()) {
		fallClipVersion = fallSites.GetVertexClipVersion();
		const auto& clips = fallSites.GetVertexClips();
		const auto count = static_cast<UINT>(clips.size());
		if (count > fallClipCapacity) {
			fallClipBuffer = nullptr;
			fallClipSRV = nullptr;
			fallClipCapacity = 0;
			fallClipCount = 0;
			D3D11_BUFFER_DESC clipDesc{ .ByteWidth = static_cast<UINT>(count * sizeof(clips[0])), .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, .StructureByteStride = sizeof(clips[0]) };
			if (SUCCEEDED(device->CreateBuffer(&clipDesc, nullptr, fallClipBuffer.put()))) {
				Util::SetResourceName(fallClipBuffer.get(), "FluidFieldWaterfalls::FallVertexClip");
				fallClipCapacity = count;
			} else {
				fallClipBuffer = nullptr;
			}
		}
		if (fallClipBuffer) {
			// The view spans the clips only, as when the buffer was made to their size
			if (count != fallClipCount) {
				fallClipSRV = nullptr;
				fallClipCount = 0;
				D3D11_SHADER_RESOURCE_VIEW_DESC clipView{ .Format = DXGI_FORMAT_UNKNOWN, .ViewDimension = D3D11_SRV_DIMENSION_BUFFER };
				clipView.Buffer.FirstElement = 0;
				clipView.Buffer.NumElements = count;
				if (SUCCEEDED(device->CreateShaderResourceView(fallClipBuffer.get(), &clipView, fallClipSRV.put()))) {
					Util::SetResourceName(fallClipSRV.get(), "FluidFieldWaterfalls::FallVertexClip SRV");
					fallClipCount = count;
				} else {
					fallClipSRV = nullptr;
				}
			}
			const D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(count * sizeof(clips[0])), 1, 1 };
			context->UpdateSubresource(fallClipBuffer.get(), 0, &box, clips.data(), 0, 0);
		}
	}

	// D59: the lit meshes' flow bakes, one texture each, made again only when its texels changed (FlowBake::stamp)
	if (fallSites.GetFlowBakeVersion() != fallBakeVersion) {
		fallBakeVersion = fallSites.GetFlowBakeVersion();
		const auto& bakes = fallSites.GetFlowBakes();
		std::erase_if(fallBakeSRVs, [&](const auto& a_entry) { return !bakes.contains(a_entry.first); });
		for (const auto& [geometry, bake] : bakes) {
			// An unstamped bake (still being linked) is made again every time
			if (const auto it = fallBakeSRVs.find(geometry); it != fallBakeSRVs.end() && bake.stamp != 0 && it->second.stamp == bake.stamp)
				continue;
			fallBakeSRVs.erase(geometry);
			D3D11_TEXTURE2D_DESC bakeDesc{ .Width = bake.width, .Height = bake.height * static_cast<UINT>(bake.texels.size() / (static_cast<std::size_t>(bake.width) * bake.height * 4)), .MipLevels = 1, .ArraySize = 1, .Format = DXGI_FORMAT_R32G32B32A32_FLOAT, .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_IMMUTABLE, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = 0 };
			const D3D11_SUBRESOURCE_DATA bakeData{ bake.texels.data(), bake.width * 4 * sizeof(float), 0 };
			winrt::com_ptr<ID3D11Texture2D> texture;
			winrt::com_ptr<ID3D11ShaderResourceView> view;
			if (FAILED(device->CreateTexture2D(&bakeDesc, &bakeData, texture.put())) || FAILED(device->CreateShaderResourceView(texture.get(), nullptr, view.put())))
				continue;
			Util::SetResourceName(texture.get(), "FluidFieldWaterfalls::FallFlowBake");
			Util::SetResourceName(view.get(), "FluidFieldWaterfalls::FallFlowBake SRV");
			fallBakeSRVs[geometry] = { view, bake.stamp };
		}
	}

	// Recentred on the player when the captures change, or when the player has moved far
	constexpr float half = 0.5f * FALL_GROUND_SIZE * FALL_GROUND_CELL;
	const float dx = a_playerPos.x - (fallGroundOrigin[0] + half);
	const float dy = a_playerPos.y - (fallGroundOrigin[1] + half);
	if (fallSites.GetGroundVersion() != fallGroundVersion || dx * dx + dy * dy >= FALL_GROUND_RECENTRE * FALL_GROUND_RECENTRE) {
		fallGroundVersion = fallSites.GetGroundVersion();
		fallGroundOrigin[0] = std::floor((a_playerPos.x - half) / FALL_GROUND_CELL) * FALL_GROUND_CELL;
		fallGroundOrigin[1] = std::floor((a_playerPos.y - half) / FALL_GROUND_CELL) * FALL_GROUND_CELL;
		fallSites.BuildGroundMap(fallGroundOrigin[0], fallGroundOrigin[1], FALL_GROUND_CELL, FALL_GROUND_SIZE, fallGroundScratch);
		fallSites.BuildPoolMap(fallGroundOrigin[0], fallGroundOrigin[1], FALL_GROUND_CELL, FALL_GROUND_SIZE, fallPoolScratch);
		fallGroundUpload.resize(fallGroundScratch.size() * 2);
		for (std::size_t i = 0; i < fallGroundScratch.size(); i++) {
			fallGroundUpload[i * 2] = fallGroundScratch[i];
			fallGroundUpload[i * 2 + 1] = fallPoolScratch[i];
		}
		context->UpdateSubresource(fallGroundTexture.get(), 0, nullptr, fallGroundUpload.data(), FALL_GROUND_SIZE * 2 * sizeof(float), 0);
		if (fallPlungeTexture) {
			fallSites.BuildPlungeMap(fallGroundOrigin[0], fallGroundOrigin[1], FALL_GROUND_CELL, FALL_GROUND_SIZE, fallPlungeScratch);
			fallHalfUpload.resize(fallPlungeScratch.size());
			for (std::size_t i = 0; i < fallHalfUpload.size(); i++)
				fallHalfUpload[i] = DirectX::PackedVector::XMConvertFloatToHalf(fallPlungeScratch[i]);
			context->UpdateSubresource(fallPlungeTexture.get(), 0, nullptr, fallHalfUpload.data(), FALL_GROUND_SIZE * sizeof(uint16_t), 0);
		}
		if (fallLipTexture) {
			fallSites.BuildLipMap(fallGroundOrigin[0], fallGroundOrigin[1], FALL_GROUND_CELL, FALL_GROUND_SIZE, fallLipScratch);
			fallHalfUpload.resize(fallLipScratch.size());
			for (std::size_t i = 0; i < fallHalfUpload.size(); i++)
				fallHalfUpload[i] = DirectX::PackedVector::XMConvertFloatToHalf(fallLipScratch[i]);
			context->UpdateSubresource(fallLipTexture.get(), 0, nullptr, fallHalfUpload.data(), FALL_GROUND_SIZE * 4 * sizeof(uint16_t), 0);
		}
	}
	fallGroundData = { { fallGroundOrigin[0], fallGroundOrigin[1] }, 1.0f / FALL_GROUND_CELL, static_cast<float>(FALL_GROUND_SIZE), 1u,
		0u, fallSurfaceTime, FALL_POOL_FOAM, {}, {}, {}, settings.FallWhitening ? 1u : 0u, UINT32_MAX,
		settings.HideUnderwaterFalls ? 1u : 0u, { 1.0f, 1.0f }, 0u, {}, {} };
	fallGroundData.AerationLook[0] = settings.FallFoamStart;
	fallGroundData.AerationLook[1] = settings.FallAerationFoam;
	fallGroundData.AerationLook[2] = settings.FallAerationMilk;
	fallGroundData.AerationLook[3] = settings.FallAerationFrom;
}

void FluidFieldWaterfalls::PrepareFallGroundData(const RE::BSGeometry* a_geometry, RE::BSShaderProperty* a_property, ID3D11ShaderResourceView*& o_bake)
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
			o_bake = it->second.view.get();
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

void FluidFieldWaterfalls::SetupWaterGeometry(RE::BSRenderPass* a_pass)
{
	waterGeometry = a_pass ? a_pass->geometry : nullptr;
}

void FluidFieldWaterfalls::CaptureRiverDraw(RE::BSShader* a_shader, uint32_t a_vertexDescriptor, uint32_t a_pixelDescriptor)
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

bool FluidFieldWaterfalls::DrawRefractiveFalls(ID3D11RenderTargetView* a_target, ID3D11Texture2D* a_targetTexture, ID3D11DepthStencilView* a_depth, ID3D11ShaderResourceView* a_sceneDepth)
{
	auto* cameraSnapshot = FluidFieldSim::Field::Get().GetCameraSnapshot();
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
	params.camera = cameraSnapshot;
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
