#pragma once

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace FluidFieldSim
{
	class WaterSurfaces;

	/**
	 * @brief Hides vanilla's placed foam and spray effects where Fluid Field simulates the water (D19).
	 *
	 * Vanilla fakes rapids foam, splashes and spray with effect meshes placed along rivers and at
	 * waterfalls (FXRapids, FxRapidsRing, FXrapidsRocks, FXSplash...Churn, waterfall skirts, mist
	 * blasts and spray). Their looping textures ignore the flow, so inside the simulated area their
	 * effect parts are culled (`SetAppCulled`) and Fluid Field's own foam takes over; lit geometry in
	 * the same mesh (the rocks of FXrapidsRocks) stays. Nothing is written to the
	 * save: a reference is shown again once it leaves the area, or when the option or the water is
	 * turned off. Waterfall bodies and ambient low mist stay: falling water and atmosphere are not
	 * something the surface simulation produces. Runs on the main thread.
	 */
	class VanillaFoam
	{
	public:
		/**
		 * @brief Hides foam effects within a_halfSize of the centre (a square) that sit on water, and shows
		 * again any hidden one that left it. Rescans the loaded references every few updates.
		 */
		void Update(const WaterSurfaces& a_water, float a_centreX, float a_centreY, float a_halfSize);

		/** @brief Shows every hidden reference again. */
		void RestoreAll();

		uint32_t GetHiddenCount() const { return static_cast<uint32_t>(hidden.size()); }

		/** @brief True for vanilla foam, splash and spray effect meshes (by model path). */
		static bool IsFoamModel(std::string_view a_model);

	private:
		/** @brief Updates between scans of the loaded references. */
		static constexpr uint32_t SCAN_INTERVAL = 30;

		/** @brief A hidden reference: the 3D its parts were found in, and the culled parts. */
		struct HiddenRef
		{
			RE::NiPointer<RE::NiAVObject> root;
			std::vector<RE::NiPointer<RE::NiAVObject>> parts;
		};

		/** @brief Shows a reference's culled parts again. */
		static void Show(HiddenRef& a_hidden);

		std::unordered_map<uint32_t, HiddenRef> hidden;  ///< Keyed by form ID
		uint32_t updatesUntilScan = 0;
	};
}
