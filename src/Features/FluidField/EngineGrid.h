#pragma once

#include <cstdint>

namespace FluidFieldSim
{
	/** @brief Loaded-grid cell lookup, under the terrain-cell lock that Skyrim 1.7.99 added (as UnifiedWater does). */
	inline RE::TESObjectCELL* GetGridCell(RE::GridCellArray* a_grid, uint32_t a_x, uint32_t a_y)
	{
		if (REL::Module::IsAE() && REL::Module::IsAtLeast(SKSE::RUNTIME_SSE_1_7_99)) {
			static REL::Relocation<RE::BSReadWriteLock*> terrainCellMapLock{ REL::ID(564236) };
			const RE::BSReadLockGuard lock(*terrainCellMapLock);
			return a_grid->GetCell(a_x, a_y);
		}
		return a_grid->GetCell(a_x, a_y);
	}

	/** @brief Packs exterior cell coordinates into one map key. */
	inline uint64_t MakeCellKey(int32_t a_cellX, int32_t a_cellY)
	{
		return (static_cast<uint64_t>(static_cast<uint32_t>(a_cellX)) << 32) | static_cast<uint32_t>(a_cellY);
	}
}
