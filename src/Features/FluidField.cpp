#include "FluidField.h"

#include "../I18n/I18n.h"

#define I18N_KEY_PREFIX "feature.fluid_field."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FluidField::Settings,
	Enabled)

void FluidField::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enabled_tooltip"), "Runs the fluid simulation. It has no visible effect yet."));
	}
}

#undef I18N_KEY_PREFIX

void FluidField::LoadSettings(json& o_json)
{
	settings = o_json;
}

void FluidField::SaveSettings(json& o_json)
{
	o_json = settings;
}

void FluidField::RestoreDefaultSettings()
{
	settings = {};
}
