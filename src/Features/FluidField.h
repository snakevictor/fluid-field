#pragma once

/**
 * @brief Player-centred fluid simulation shared by water, fog, smoke and buoyancy outputs.
 *
 * Phase 0 scaffold: registers the feature, its menu entry and its saved settings. It installs
 * no hooks and creates no GPU resources, so it costs nothing whether enabled or not.
 */
struct FluidField : Feature
{
	virtual inline std::string GetName() override { return "Fluid Field"; }
	virtual std::string GetDisplayName() override { return T("feature.fluid_field.name", "Fluid Field"); }
	virtual inline std::string GetShortName() override { return "FluidField"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kWater; }

	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.fluid_field.description", "A real-time fluid simulation around the player that drives interactive water, fog and smoke."),
			{ T("feature.fluid_field.key_feature_1", "Shallow water that flows around obstacles and carries wakes"),
				T("feature.fluid_field.key_feature_2", "Fog and smoke pushed by actors, shouts and spells") } };
	}

	struct Settings
	{
		bool Enabled = false;
	};

	Settings settings;

	/** @brief Draws the ImGui settings UI. */
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;
};
