#include "VanillaFoam.h"

#include "EngineGrid.h"
#include "WaterSurfaces.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <unordered_set>

namespace FluidFieldSim
{
	namespace
	{
		/** @brief How far below a reference's origin the water may be, for the effect to count as on the water. */
		constexpr float MAX_HEIGHT_ABOVE_WATER = 1024.0f;
		constexpr float MAX_DEPTH_BELOW_WATER = 512.0f;

		std::string Lowercase(std::string_view a_text)
		{
			std::string result(a_text);
			std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return result;
		}

		const char* ModelOf(const RE::TESBoundObject* a_base)
		{
			const auto* model = a_base ? a_base->As<RE::TESModel>() : nullptr;
			return model ? model->GetModel() : nullptr;
		}

		void SetHidden(RE::TESObjectREFR& a_ref, bool a_hidden)
		{
			if (auto* root = a_ref.Get3D())
				root->SetAppCulled(a_hidden);
		}
	}

	bool VanillaFoam::IsFoamModel(std::string_view a_model)
	{
		const std::string model = Lowercase(a_model);
		if (model.find("effects\\") == std::string::npos)
			return false;
		// Falling water and ambient mist are not surface foam; they stay
		if (model.find("mistlow") != std::string::npos || model.find("waterfallbody") != std::string::npos ||
			model.find("waterfallthin") != std::string::npos || model.find("waterfalltall") != std::string::npos)
			return false;
		constexpr std::array<std::string_view, 9> patterns = { "rapids", "splash", "churn", "foam", "spray", "mistblast", "skirt", "waterfallmist", "ripple" };
		return std::ranges::any_of(patterns, [&](std::string_view a_pattern) { return model.find(a_pattern) != std::string::npos; });
	}

	void VanillaFoam::RestoreAll()
	{
		for (const auto& [formID, handle] : hidden) {
			if (const auto ref = handle.get(); ref && ref->GetFormID() == formID)
				SetHidden(*ref, false);
		}
		hidden.clear();
		updatesUntilScan = 0;
	}

	void VanillaFoam::Update(const WaterSurfaces& a_water, float a_centreX, float a_centreY, float a_halfSize)
	{
		if (updatesUntilScan > 0) {
			updatesUntilScan--;
			return;
		}
		updatesUntilScan = SCAN_INTERVAL;

		auto* tes = RE::TES::GetSingleton();
		if (!tes)
			return;

		std::unordered_set<uint32_t> inArea;
		const auto visitCell = [&](RE::TESObjectCELL* a_cell, bool a_requireAttached) {
			if (!a_cell || (a_requireAttached && !a_cell->IsAttached()))
				return;
			a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted() || !a_ref->Get3D())
					return RE::BSContainer::ForEachResult::kContinue;
				const char* model = ModelOf(a_ref->GetBaseObject());
				if (!model || !IsFoamModel(model))
					return RE::BSContainer::ForEachResult::kContinue;
				const auto position = a_ref->GetPosition();
				if (std::abs(position.x - a_centreX) > a_halfSize || std::abs(position.y - a_centreY) > a_halfSize)
					return RE::BSContainer::ForEachResult::kContinue;
				float surface = 0.0f;
				if (a_water.SampleHeight(position.x, position.y, surface) == WaterSurfaces::Source::None ||
					position.z > surface + MAX_HEIGHT_ABOVE_WATER || position.z < surface - MAX_DEPTH_BELOW_WATER)
					return RE::BSContainer::ForEachResult::kContinue;

				const uint32_t formID = a_ref->GetFormID();
				inArea.insert(formID);
				// Hide new ones, and hide again any whose 3D was reloaded since
				SetHidden(*a_ref, true);
				hidden.try_emplace(formID, a_ref->GetHandle());
				return RE::BSContainer::ForEachResult::kContinue;
			});
		};

		if (tes->interiorCell) {
			visitCell(tes->interiorCell, true);
		} else if (auto* grid = tes->gridCells) {
			for (uint32_t gx = 0; gx < grid->length; gx++) {
				for (uint32_t gy = 0; gy < grid->length; gy++)
					visitCell(GetGridCell(grid, gx, gy), true);
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (auto* worldspace = player ? player->GetWorldspace() : nullptr)
				visitCell(worldspace->persistentCell, false);
		}

		// Show again whatever left the area (or unloaded)
		for (auto it = hidden.begin(); it != hidden.end();) {
			if (!inArea.contains(it->first)) {
				if (const auto ref = it->second.get(); ref && ref->GetFormID() == it->first)
					SetHidden(*ref, false);
				it = hidden.erase(it);
			} else {
				++it;
			}
		}
	}
}
