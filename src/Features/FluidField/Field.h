#pragma once

#include "GpuSpans.h"
#include "LandscapeCache.h"
#include "WaterSurfaces.h"

struct Feature;

namespace FluidFieldSim
{
	/**
	 * @brief What the Waterfalls, Shallow Water and Ground Fog features share: the landscape and water surface caches, GPU
	 * timings, the main camera's snapshot, the loading-screen resets and the player's position this frame.
	 *
	 * Not a feature: each one's Prepass opens a PrepassScope while it is enabled, which updates the field at most once a
	 * frame. With none enabled it holds nothing.
	 */
	class Field
	{
	public:
		/** @brief The one field. */
		static Field& Get();

		/** @brief Whether any of the three features is loaded and enabled. */
		static bool AnyActive();

		/** @brief Registers the loading-screen listener (once, whichever feature asks first). */
		void RegisterListeners();

		/**
		 * @brief Brackets a feature's Prepass: updates the field once a frame, and closes the simulation's GPU span after
		 * the last enabled feature's Prepass.
		 */
		class PrepassScope
		{
		public:
			explicit PrepassScope(const Feature& a_feature);
			~PrepassScope();
			PrepassScope(const PrepassScope&) = delete;
			PrepassScope& operator=(const PrepassScope&) = delete;
			/** @brief False in menus, loading screens and without a player: the feature skips its step. */
			bool IsReady() const { return ready; }

		private:
			const Feature& feature;
			bool ready = false;
		};

		/** @brief Releases everything once no feature is enabled; cheap to call every frame. */
		void ReleaseIfUnused();

		/** @brief True once per loading screen for each caller's counter, so each feature resets on its own schedule. */
		bool ConsumeLoad(uint32_t& a_seen) const;

		const LandscapeCache& GetLandscape() const { return landscape; }
		const WaterSurfaces& GetWater() const { return water; }
		GpuSpans& GetGpuSpans() { return gpuSpans; }
		/** @brief The main camera's per-frame constants as copied before the water, or null. */
		ID3D11Buffer* GetCameraSnapshot() const { return cameraSnapshot.get(); }
		const RE::NiPoint3& GetPlayerPosition() const { return playerPosition; }
		/** @brief This frame's step in seconds, clamped; 0 while paused. */
		float GetDeltaTime() const { return deltaTime; }

		/** @brief From the deferred passes: snapshots the camera, then the falls' work before the water. */
		void DrawBeforeWater();
		/** @brief After the world (water included): the falls, then the ground fog. */
		void DrawAfterWorld();

		/** @brief Longest step simulated in one frame, so a hitch does not fling the simulation. */
		static constexpr float MAX_STEP_SECONDS = 1.0f / 20.0f;
		/** @brief Allowed grid resolutions. The shaders wrap with a bitmask, so each must be a power of two. */
		static constexpr std::array<uint32_t, 5> GRID_RESOLUTIONS = { 64, 128, 256, 512, 1024 };
		/** @brief Grid widths in world units, for the water and wind grids alike. */
		static constexpr float MIN_GRID_SIZE = 1024.0f;
		static constexpr float MAX_GRID_SIZE = 16384.0f;

	private:
		Field() = default;

		/** @brief Updates the shared caches once a frame. False when the features should not step. */
		bool Update();
		void SnapshotCamera();

		LandscapeCache landscape;
		WaterSurfaces water;
		GpuSpans gpuSpans;
		winrt::com_ptr<ID3D11Buffer> cameraSnapshot;
		RE::NiPoint3 playerPosition;
		float deltaTime = 0.0f;
		uint32_t updatedFrame = UINT32_MAX;
		bool ready = false;
		bool released = true;

		/** @brief Loading screens seen; incremented on the UI thread. */
		std::atomic<uint32_t> loads = 0;
		uint32_t landscapeLoads = UINT32_MAX;

		class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
		};
	};
}
