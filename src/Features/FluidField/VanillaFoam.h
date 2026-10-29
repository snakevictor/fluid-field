#pragma once

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace FluidFieldSim
{
	class WaterSurfaces;

	/**
	 * @brief D19: hides vanilla's placed foam and spray effects where Shallow Water simulates the water.
	 *
	 * Their looping textures ignore the flow, so their effect parts are culled (`SetAppCulled`) and our foam takes over; lit
	 * geometry in the same mesh (FXrapidsRocks' rocks) stays. Nothing is saved: a reference is shown again when it leaves the
	 * area or the option is off. Waterfall bodies and low mist stay (the surface sim doesn't make them). Main thread.
	 */
	class VanillaFoam
	{
	public:
		/**
		 * @brief Hides foam effects on water within the square a_halfSize around the centre and shows those that left it.
		 * Rescans every SCAN_INTERVAL updates.
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
