#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace FluidFieldSim
{
	/**
	 * @brief The rock under waterfalls, for the shallow-water solver (D24).
	 *
	 * Each fall area is mapped by downward Havok rays on a LATTICE-unit lattice, a few per frame within
	 * a time budget, and remembered. A float texture aligned to a water level (toroidal like it: world
	 * cell c in texel c & (res - 1)) holds the rock's height in cells inside the areas, bilinear between
	 * lattice points, and NO_FALL_BED elsewhere. As the level scrolls only the cells new to it are
	 * filled; cells under newly cast rays are refilled. Runs on the thread that steps the water.
	 */
	class FallBed
	{
	public:
		/** @brief Written where there is no fall bed (matches NO_FALL_BED in WaterCommon.hlsli, with margin). */
		static constexpr float NONE = -1.0e7f;
		/** @brief World units between rays. */
		static constexpr float LATTICE = 16.0f;

		/** @brief An area to map: a world-space box and the heights the rays run between. */
		struct Area
		{
			float min[2];
			float max[2];
			float top;
			float bottom;
		};

		/** @brief Replaces the areas; lattice points not yet cast are queued. */
		void SetAreas(const std::vector<Area>& a_areas);

		/** @brief Casts queued rays for up to a_budgetMs. */
		void CastPending(double a_budgetMs);

		/**
		 * @brief Brings the texture up to date with the level's placement and uploads what changed.
		 * @return False if the texture could not be created.
		 */
		bool Upload(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const int32_t a_gridMin[2], uint32_t a_resolution, float a_cellSize);

		void Clear();

		/** @brief The texture, or null while there are no areas. */
		ID3D11ShaderResourceView* GetSRV() const { return areas.empty() ? nullptr : srv.get(); }
		const std::vector<Area>& GetAreas() const { return areas; }
		std::size_t GetPendingCount() const { return pending.size(); }
		std::size_t GetSampleCount() const { return samples.size(); }
		/** @brief Lattice points whose ray found rock or ground. Walks every sample, so for display only. */
		std::size_t CountHits() const;

	private:
		static uint64_t Key(int32_t a_x, int32_t a_y) { return (static_cast<uint64_t>(static_cast<uint32_t>(a_x)) << 32) | static_cast<uint32_t>(a_y); }
		/** @brief The rock's height at a world position, bilinear between cast lattice points; false where any is missing. */
		bool Sample(float a_x, float a_y, float& a_height) const;
		bool InAnyArea(float a_x, float a_y) const;

		struct Pending
		{
			int32_t x, y;
			float top, bottom;
		};

		std::vector<Area> areas;
		std::unordered_map<uint64_t, float> samples;  ///< Rock height per lattice point; NONE where the ray hit nothing
		std::vector<Pending> pending;
		float dirtyMin[2]{ FLT_MAX, FLT_MAX };  ///< World box of rays cast since the last upload
		float dirtyMax[2]{ -FLT_MAX, -FLT_MAX };
		bool areasChanged = false;

		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		std::vector<float> heights;  ///< Toroidal, as the texture
		uint32_t resolution = 0;
		float cellSize = 0.0f;
		int32_t filledMin[2]{};
		bool filled = false;
	};
}
