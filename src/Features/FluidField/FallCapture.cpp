#include "FallCapture.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace FluidFieldSim
{
	namespace
	{
		/** @brief Largest capture side, texels: a site this big is captured over its centre only. */
		constexpr uint32_t MAX_SIDE = 512;
		constexpr int FILL_PASSES = 8;

		bool IsSolidLayer(RE::COL_LAYER a_layer)
		{
			switch (a_layer) {
			case RE::COL_LAYER::kStatic:
			case RE::COL_LAYER::kAnimStatic:
			case RE::COL_LAYER::kTerrain:
			case RE::COL_LAYER::kGround:
				return true;
			default:
				return false;
			}
		}
	}

	void FallCapture::Begin(const float a_min[2], const float a_max[2], float a_top, float a_bottom)
	{
		const float centre[2] = { 0.5f * (a_min[0] + a_max[0]), 0.5f * (a_min[1] + a_max[1]) };
		uint32_t sides[2];
		for (int k = 0; k < 2; k++)
			sides[k] = std::min(static_cast<uint32_t>(std::ceil((a_max[k] - a_min[k] + 2.0f * MARGIN) / CELL)) + 1, MAX_SIDE);
		width = sides[0];
		height = sides[1];
		originX = std::floor((centre[0] - 0.5f * (width - 1) * CELL) / CELL) * CELL;
		originY = std::floor((centre[1] - 0.5f * (height - 1) * CELL) / CELL) * CELL;
		top = a_top + CEILING;
		bottom = a_bottom - BELOW;
		heights.assign(static_cast<std::size_t>(width) * height, NONE);
		next = 0;
		complete = false;
	}

	bool FallCapture::Cast(double a_budgetMs)
	{
		if (complete)
			return true;
		auto* tes = RE::TES::GetSingleton();
		const RE::NiPoint3 centre(originX + 0.5f * width * CELL, originY + 0.5f * height * CELL, bottom);
		auto* cell = tes ? (tes->interiorCell ? tes->interiorCell : tes->GetCell(centre)) : nullptr;
		auto* bhkWorld = cell ? cell->GetbhkWorld() : nullptr;
		auto* world = bhkWorld ? bhkWorld->GetWorld1() : nullptr;
		if (!world)
			return false;
		{
			const RE::BSReadLockGuard lock(bhkWorld->worldLock);
			const float scale = RE::bhkWorld::GetWorldScale();
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(a_budgetMs);
			while (next < heights.size() && std::chrono::steady_clock::now() < deadline) {
				const float x = originX + static_cast<float>(next % width) * CELL;
				const float y = originY + static_cast<float>(next / width) * CELL;
				// The highest rock or ground below the ceiling, through anything else (water, actors, clutter)
				float hit = NONE;
				float fromZ = top;
				for (int attempt = 0; attempt < 4 && fromZ > bottom; attempt++) {
					RE::hkpWorldRayCastInput input;
					input.from = RE::hkVector4(x * scale, y * scale, fromZ * scale, 0.0f);
					input.to = RE::hkVector4(x * scale, y * scale, bottom * scale, 0.0f);
					RE::hkpWorldRayCastOutput output;
					world->CastRay(input, output);
					if (!output.HasHit())
						break;
					const float hitZ = fromZ + (bottom - fromZ) * output.hitFraction;
					if (output.rootCollidable && IsSolidLayer(output.rootCollidable->GetCollisionLayer())) {
						hit = hitZ;
						break;
					}
					fromZ = hitZ - 1.0f;
				}
				heights[next++] = hit;
			}
		}
		if (next < heights.size())
			return false;
		CleanUp();
		complete = true;
		return true;
	}

	void FallCapture::CleanUp()
	{
		const auto w = static_cast<int32_t>(width);
		const auto h = static_cast<int32_t>(height);
		const auto index = [&](int32_t a_x, int32_t a_y) { return static_cast<std::size_t>(a_y) * width + a_x; };

		// Holes (rays that found no rock) take the mean of their known neighbours, growing inward
		std::vector<float> scratch = heights;
		for (int pass = 0; pass < FILL_PASSES; pass++) {
			bool any = false;
			for (int32_t y = 0; y < h; y++) {
				for (int32_t x = 0; x < w; x++) {
					if (heights[index(x, y)] > NONE)
						continue;
					float sum = 0.0f;
					int count = 0;
					for (int32_t dy = -1; dy <= 1; dy++) {
						for (int32_t dx = -1; dx <= 1; dx++) {
							const int32_t nx = x + dx;
							const int32_t ny = y + dy;
							if (nx < 0 || ny < 0 || nx >= w || ny >= h || heights[index(nx, ny)] <= NONE)
								continue;
							sum += heights[index(nx, ny)];
							count++;
						}
					}
					if (count >= 3) {
						scratch[index(x, y)] = sum / static_cast<float>(count);
						any = true;
					}
				}
			}
			heights = scratch;
			if (!any)
				break;
		}

		// Spikes (a stray rock, a fence post) go with a 3x3 median of the known values
		for (int32_t y = 0; y < h; y++) {
			for (int32_t x = 0; x < w; x++) {
				if (heights[index(x, y)] <= NONE)
					continue;
				float values[9];
				int count = 0;
				for (int32_t dy = -1; dy <= 1; dy++) {
					for (int32_t dx = -1; dx <= 1; dx++) {
						const int32_t nx = x + dx;
						const int32_t ny = y + dy;
						if (nx >= 0 && ny >= 0 && nx < w && ny < h && heights[index(nx, ny)] > NONE)
							values[count++] = heights[index(nx, ny)];
					}
				}
				std::nth_element(values, values + count / 2, values + count);
				scratch[index(x, y)] = values[count / 2];
			}
		}
		heights.swap(scratch);
	}

	bool FallCapture::Sample(float a_x, float a_y, float& a_height) const
	{
		const float fx = (a_x - originX) / CELL;
		const float fy = (a_y - originY) / CELL;
		const auto ix = static_cast<int32_t>(std::floor(fx));
		const auto iy = static_cast<int32_t>(std::floor(fy));
		if (ix < 0 || iy < 0 || ix + 1 >= static_cast<int32_t>(width) || iy + 1 >= static_cast<int32_t>(height))
			return false;
		const float tx = fx - static_cast<float>(ix);
		const float ty = fy - static_cast<float>(iy);
		const float h00 = At(ix, iy), h10 = At(ix + 1, iy), h01 = At(ix, iy + 1), h11 = At(ix + 1, iy + 1);
		if (h00 <= NONE || h10 <= NONE || h01 <= NONE || h11 <= NONE)
			return false;
		a_height = (h00 * (1.0f - tx) + h10 * tx) * (1.0f - ty) + (h01 * (1.0f - tx) + h11 * tx) * ty;
		return true;
	}
}
