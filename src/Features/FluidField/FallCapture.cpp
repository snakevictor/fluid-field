#include "FallCapture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <unordered_map>

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
		lips.clear();
		edges.clear();
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

	void FallCapture::ExtractLips(const std::vector<float>& a_levels)
	{
		lips.clear();
		edges.clear();
		if (!complete || width < 2 || height < 2)
			return;
		const auto w = static_cast<int32_t>(width);
		const auto h = static_cast<int32_t>(height);

		// The downhill direction at a point, from the ground's gradient
		const auto downhill = [&](float a_x, float a_y, float a_normal[2]) {
			float hx0, hx1, hy0, hy1;
			if (!Sample(a_x - CELL, a_y, hx0) || !Sample(a_x + CELL, a_y, hx1) || !Sample(a_x, a_y - CELL, hy0) || !Sample(a_x, a_y + CELL, hy1))
				return false;
			const float gx = hx0 - hx1;
			const float gy = hy0 - hy1;
			const float length = std::sqrt(gx * gx + gy * gy);
			if (length < 1e-3f)
				return false;
			a_normal[0] = gx / length;
			a_normal[1] = gy / length;
			return true;
		};

		/*
		 * Marching squares over the texel grid at an iso height. Each crossing lies on a grid edge, named
		 * by its id (horizontal edges 2i, vertical 2i + 1, i the texel at its start), so the segments of
		 * neighbouring squares share their ends exactly.
		 */
		struct Segment
		{
			uint32_t a, b;
		};
		const auto contour = [&](float a_iso, std::vector<Segment>& a_segments, std::unordered_map<uint32_t, std::array<float, 2>>& a_points) {
			const auto crossing = [&](int32_t a_x, int32_t a_y, bool a_vertical) {
				const uint32_t id = (static_cast<uint32_t>(a_y) * width + static_cast<uint32_t>(a_x)) * 2 + (a_vertical ? 1 : 0);
				if (!a_points.contains(id)) {
					const float h0 = At(a_x, a_y);
					const float h1 = a_vertical ? At(a_x, a_y + 1) : At(a_x + 1, a_y);
					const float t = std::clamp((a_iso - h0) / (h1 - h0), 0.0f, 1.0f);
					const float x = originX + (static_cast<float>(a_x) + (a_vertical ? 0.0f : t)) * CELL;
					const float y = originY + (static_cast<float>(a_y) + (a_vertical ? t : 0.0f)) * CELL;
					a_points[id] = { x, y };
				}
				return id;
			};
			for (int32_t y = 0; y + 1 < h; y++) {
				for (int32_t x = 0; x + 1 < w; x++) {
					const float c[4] = { At(x, y), At(x + 1, y), At(x + 1, y + 1), At(x, y + 1) };
					if (c[0] <= NONE || c[1] <= NONE || c[2] <= NONE || c[3] <= NONE)
						continue;
					const int mask = (c[0] > a_iso ? 1 : 0) | (c[1] > a_iso ? 2 : 0) | (c[2] > a_iso ? 4 : 0) | (c[3] > a_iso ? 8 : 0);
					if (mask == 0 || mask == 15)
						continue;
					// Square edges: 0 bottom (y), 1 right (x + 1), 2 top (y + 1), 3 left (x)
					const auto edge = [&](int a_edge) {
						switch (a_edge) {
						case 0:
							return crossing(x, y, false);
						case 1:
							return crossing(x + 1, y, true);
						case 2:
							return crossing(x, y + 1, false);
						default:
							return crossing(x, y, true);
						}
					};
					static constexpr int8_t TABLE[16][4] = {
						{ -1, -1, -1, -1 }, { 3, 0, -1, -1 }, { 0, 1, -1, -1 }, { 3, 1, -1, -1 },
						{ 1, 2, -1, -1 }, { 3, 0, 1, 2 }, { 0, 2, -1, -1 }, { 3, 2, -1, -1 },
						{ 2, 3, -1, -1 }, { 2, 0, -1, -1 }, { 0, 1, 2, 3 }, { 2, 1, -1, -1 },
						{ 1, 3, -1, -1 }, { 1, 0, -1, -1 }, { 0, 3, -1, -1 }, { -1, -1, -1, -1 }
					};
					auto entry = TABLE[mask];
					// Saddles: the centre decides which corners connect
					int8_t saddle[4];
					if (mask == 5 || mask == 10) {
						const bool centreAbove = 0.25f * (c[0] + c[1] + c[2] + c[3]) > a_iso;
						if (centreAbove) {
							// The high corners join through the centre: cut off the low ones instead
							const int8_t joined[2][4] = { { 0, 1, 2, 3 }, { 3, 0, 1, 2 } };
							std::copy(joined[mask == 5 ? 0 : 1], joined[mask == 5 ? 0 : 1] + 4, saddle);
							entry = saddle;
						}
					}
					for (int s = 0; s < 4 && entry[s] >= 0; s += 2)
						a_segments.push_back({ edge(entry[s]), edge(entry[s + 1]) });
				}
			}
		};

		std::vector<Segment> segments;
		std::unordered_map<uint32_t, std::array<float, 2>> points;
		for (std::size_t levelIndex = 0; levelIndex < a_levels.size(); levelIndex++) {
			const float level = a_levels[levelIndex];

			// The shoreline: where this level's water meets the ground
			segments.clear();
			points.clear();
			contour(level, segments, points);
			for (const auto& segment : segments) {
				const auto& a = points[segment.a];
				const auto& b = points[segment.b];
				edges.push_back({ { a[0], a[1] }, { b[0], b[1] }, level, false });
			}

			// The bottom level pours over nothing
			if (levelIndex + 1 == a_levels.size())
				continue;

			// Lips: the contour just under the water, where the ground beyond it falls away
			segments.clear();
			points.clear();
			contour(level - LIP_DEPTH, segments, points);
			std::vector<Segment> lipSegments;
			for (const auto& segment : segments) {
				const auto& a = points[segment.a];
				const auto& b = points[segment.b];
				const float mid[2] = { 0.5f * (a[0] + b[0]), 0.5f * (a[1] + b[1]) };
				float normal[2];
				float beyond;
				if (!downhill(mid[0], mid[1], normal) || !Sample(mid[0] + normal[0] * DROP_PROBE, mid[1] + normal[1] * DROP_PROBE, beyond))
					continue;
				if (level - beyond <= MIN_DROP)
					continue;
				lipSegments.push_back(segment);
				edges.push_back({ { a[0], a[1] }, { b[0], b[1] }, level, true });
			}

			// Chain the lip segments into polylines through their shared ends
			std::unordered_map<uint32_t, std::vector<std::size_t>> byEnd;
			for (std::size_t i = 0; i < lipSegments.size(); i++) {
				byEnd[lipSegments[i].a].push_back(i);
				byEnd[lipSegments[i].b].push_back(i);
			}
			std::vector<bool> used(lipSegments.size(), false);
			for (std::size_t start = 0; start < lipSegments.size(); start++) {
				if (used[start])
					continue;
				used[start] = true;
				std::deque<uint32_t> chain{ lipSegments[start].a, lipSegments[start].b };
				const auto extend = [&](bool a_front) {
					for (;;) {
						const uint32_t end = a_front ? chain.front() : chain.back();
						std::size_t found = lipSegments.size();
						for (const std::size_t candidate : byEnd[end]) {
							if (!used[candidate]) {
								found = candidate;
								break;
							}
						}
						if (found == lipSegments.size())
							return;
						used[found] = true;
						const uint32_t other = lipSegments[found].a == end ? lipSegments[found].b : lipSegments[found].a;
						if (a_front)
							chain.push_front(other);
						else
							chain.push_back(other);
					}
				};
				extend(false);
				extend(true);

				Lip lip{ level, {}, 0.0f };
				for (const uint32_t id : chain) {
					const auto& p = points[id];
					LipPoint point{ { p[0], p[1], level }, { 0.0f, 0.0f } };
					downhill(p[0], p[1], point.normal);
					if (!lip.points.empty()) {
						const auto& last = lip.points.back().position;
						lip.length += std::hypot(p[0] - last[0], p[1] - last[1]);
					}
					lip.points.push_back(point);
				}
				if (lip.length >= MIN_LIP)
					lips.push_back(std::move(lip));
			}
		}
	}
}
