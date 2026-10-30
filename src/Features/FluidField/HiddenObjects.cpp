#include "HiddenObjects.h"

namespace FluidFieldSim
{
	std::unordered_map<const RE::NiAVObject*, HiddenObjects::Entry>& HiddenObjects::Entries()
	{
		static std::unordered_map<const RE::NiAVObject*, Entry> entries;
		return entries;
	}

	void HiddenObjects::Hide(RE::NiAVObject* a_object)
	{
		if (!a_object)
			return;
		auto [it, added] = Entries().try_emplace(a_object);
		if (added) {
			it->second.object.reset(a_object);
			it->second.wasHidden = a_object->GetAppCulled();
		}
		it->second.owners++;
		a_object->SetAppCulled(true);
	}

	void HiddenObjects::Show(RE::NiAVObject* a_object)
	{
		auto& entries = Entries();
		const auto it = entries.find(a_object);
		if (it == entries.end())
			return;
		if (--it->second.owners > 0)
			return;
		if (!it->second.wasHidden)
			a_object->SetAppCulled(false);
		entries.erase(it);
	}

	void HiddenObjects::Reassert(RE::NiAVObject* a_object)
	{
		if (a_object && Entries().contains(a_object))
			a_object->SetAppCulled(true);
	}
}
