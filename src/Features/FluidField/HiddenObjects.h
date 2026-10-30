#pragma once

#include <unordered_map>

namespace FluidFieldSim
{
	/**
	 * @brief Hides scene objects (`SetAppCulled`) on behalf of several owners: Waterfalls and Shallow Water hide some of the
	 * same vanilla effect parts.
	 *
	 * An object shows again only once no owner wants it hidden, and only if it was shown before the first one hid it: effect
	 * models hide their own helper geometry (the particle emitters' rings), which must stay hidden. Runtime only, nothing is
	 * saved. Main thread.
	 */
	class HiddenObjects
	{
	public:
		/** @brief Hides a_object for one more owner. Each Hide is paired with one Show by the same owner. */
		static void Hide(RE::NiAVObject* a_object);
		/** @brief Releases one owner's hide; the last restores the object's own state. */
		static void Show(RE::NiAVObject* a_object);
		/** @brief Hides an object this owner already holds again, in case something else showed it. */
		static void Reassert(RE::NiAVObject* a_object);

	private:
		struct Entry
		{
			RE::NiPointer<RE::NiAVObject> object;
			uint32_t owners = 0;
			bool wasHidden = false;  ///< Hidden before any owner (by its model)
		};
		static std::unordered_map<const RE::NiAVObject*, Entry>& Entries();
	};
}
