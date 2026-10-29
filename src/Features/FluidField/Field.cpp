#include "Field.h"

#include "Features/FluidFieldGroundFog.h"
#include "Features/FluidFieldShallowWater.h"
#include "Features/FluidFieldWaterfalls.h"
#include "Profiler.h"
#include "State.h"
#include "Utils/D3D.h"

namespace FluidFieldSim
{
	Field& Field::Get()
	{
		static Field field;
		return field;
	}

	bool Field::AnyActive()
	{
		return globals::features::fluidFieldWaterfalls.IsActive() || globals::features::fluidFieldShallowWater.IsActive() ||
		       globals::features::fluidFieldGroundFog.IsActive();
	}

	void Field::RegisterListeners()
	{
		static bool registered = false;
		if (registered)
			return;
		auto ui = globals::game::ui;
		if (!ui) {
			logger::warn("[Fluid Field] UI event source not found; the field will not reset after loads");
			return;
		}
		static MenuOpenCloseEventHandler handler;
		ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(&handler);
		registered = true;
	}

	RE::BSEventNotifyControl Field::MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
	{
		// Every load leaves the player somewhere new; start the next step from rest
		if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME && !a_event->opening)
			Field::Get().loads++;
		return RE::BSEventNotifyControl::kContinue;
	}

	bool Field::ConsumeLoad(uint32_t& a_seen) const
	{
		const uint32_t current = loads.load();
		if (a_seen == current)
			return false;
		a_seen = current;
		return true;
	}

	Field::PrepassScope::PrepassScope(const Feature& a_feature) :
		feature(a_feature), ready(Field::Get().Update())
	{
	}

	Field::PrepassScope::~PrepassScope()
	{
		// The simulation span covers every enabled feature's Prepass; they run in this order
		const Feature* last = nullptr;
		if (globals::features::fluidFieldGroundFog.IsActive())
			last = &globals::features::fluidFieldGroundFog;
		else if (globals::features::fluidFieldShallowWater.IsActive())
			last = &globals::features::fluidFieldShallowWater;
		else if (globals::features::fluidFieldWaterfalls.IsActive())
			last = &globals::features::fluidFieldWaterfalls;
		if (last == &feature)
			Field::Get().gpuSpans.End(globals::d3d::context, GpuSpans::Simulation);
	}

	bool Field::Update()
	{
		const uint32_t frame = globals::state->frameCount;
		if (frame == updatedFrame)
			return ready;
		updatedFrame = frame;
		ready = false;
		released = false;

		if (!gpuSpans.IsReady())
			gpuSpans.Initialize(globals::d3d::device);
		// D192: close last frame's spans, read finished ones, time this frame's simulation to the last Prepass' end
		gpuSpans.EndFrame(globals::d3d::context);
		gpuSpans.Begin(globals::d3d::context, GpuSpans::Simulation);

		auto state = globals::state;
		if (state->isMapMenuOpen || state->IsMainOrLoadingMenuOpen())
			return false;

		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player)
			return false;

		if (ConsumeLoad(landscapeLoads)) {
			landscape.Clear();
			water.Clear();
		}
		{
			ZoneScopedN("FluidField - Landscape Cache");
			globals::profiler->BeginPass("FluidField::Landscape Cache");
			landscape.Update();
			globals::profiler->EndPass();
		}
		{
			ZoneScopedN("FluidField - Water Surfaces");
			globals::profiler->BeginPass("FluidField::Water Surfaces");
			water.Update();
			globals::profiler->EndPass();
		}
		playerPosition = player->GetPosition();

		const auto ui = globals::game::ui;
		const bool paused = ui && ui->GameIsPaused();
		deltaTime = (globals::game::deltaTime && !paused) ? std::min(*globals::game::deltaTime, MAX_STEP_SECONDS) : 0.0f;
		ready = true;
		return true;
	}

	void Field::ReleaseIfUnused()
	{
		if (released || AnyActive())
			return;
		released = true;
		if (landscape.GetCellCount() > 0)
			landscape.Clear();
		if (water.GetSurfaceCount() > 0 || water.GetCellWaterCount() > 0)
			water.Clear();
		gpuSpans.Release();
		cameraSnapshot = nullptr;
		updatedFrame = UINT32_MAX;
		ready = false;
	}

	void Field::SnapshotCamera()
	{
		// Snapshot the main camera while it is bound: after the world draws, the per-frame buffer can hold another view's camera
		auto* perFrame = *globals::game::perFrame;
		if (!perFrame)
			return;
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

	void Field::DrawBeforeWater()
	{
		auto& waterfalls = globals::features::fluidFieldWaterfalls;
		// The falls and the fog draw after the world, with this frame's main camera
		if (waterfalls.IsActive() || globals::features::fluidFieldGroundFog.IsActive())
			SnapshotCamera();
		if (waterfalls.IsActive())
			waterfalls.DrawBeforeWater();
		// D192: time the engine's water (and its Water.hlsl falls) until DrawAfterWorld
		gpuSpans.Begin(globals::d3d::context, GpuSpans::EngineWater);
	}

	void Field::DrawAfterWorld()
	{
		if (!globals::state->inWorld)
			return;
		gpuSpans.End(globals::d3d::context, GpuSpans::EngineWater);
		if (!globals::game::renderer)
			return;
		// The see-through falls first, then the ground fog over them
		if (auto& waterfalls = globals::features::fluidFieldWaterfalls; waterfalls.IsActive())
			waterfalls.DrawAfterWorld();
		if (auto& groundFog = globals::features::fluidFieldGroundFog; groundFog.IsActive())
			groundFog.DrawAfterWorld();
	}
}
