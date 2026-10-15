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

	void FallCapture::ExtractLips(const std::vector<float>& a_levels, const std::vector<FallEdge>& a_falls)
	{
		lips.clear();
		edges.clear();
		fallsWithLips.assign(a_falls.size(), false);
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
			std::vector<std::vector<uint32_t>> segmentFalls;  // The meshes each lip segment lies by
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
				// Where vanilla's falling water goes over: the top edge of a mesh whose top is near this level
				std::vector<uint32_t> nearFalls;
				for (uint32_t f = 0; f < a_falls.size(); f++) {
					const auto& fall = a_falls[f];
					if (level < fall.top - FALL_NEAR_BELOW || level > fall.top + FALL_NEAR_ABOVE)
						continue;
					const bool close = std::any_of(fall.edge.begin(), fall.edge.end(), [&](const std::array<float, 2>& a_vertex) {
						const float dx = a_vertex[0] - mid[0];
						const float dy = a_vertex[1] - mid[1];
						return dx * dx + dy * dy <= FALL_NEAR * FALL_NEAR;
					});
					if (close)
						nearFalls.push_back(f);
				}
				if (nearFalls.empty())
					continue;
				lipSegments.push_back(segment);
				segmentFalls.push_back(std::move(nearFalls));
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
				std::vector<std::size_t> chainSegments{ start };
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
						chainSegments.push_back(found);
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
				if (lip.length < MIN_LIP)
					continue;
				// The meshes this lip replaces, and its pieces for the debug view
				for (const std::size_t i : chainSegments) {
					for (const uint32_t f : segmentFalls[i])
						fallsWithLips[f] = true;
					const auto& a = points[lipSegments[i].a];
					const auto& b = points[lipSegments[i].b];
					edges.push_back({ { a[0], a[1] }, { b[0], b[1] }, level, true });
				}
				lips.push_back(std::move(lip));
			}
		}
	}

	void FallCapture::TraceStreams(const std::vector<float>& a_levels)
	{
		streams.clear();
		std::fill(std::begin(streamEnds), std::end(streamEnds), 0u);
		if (!complete)
			return;
		constexpr float STEP_DISTANCE = 4.0f;  // Most a stream moves per step, world units
		constexpr float MAX_TIME = 8.0f;       // Seconds
		constexpr uint32_t MAX_STEPS = 4096;
		constexpr float LEVEL_GAP = 16.0f;  // A level this far below the lip's is a different one

		struct PathPoint
		{
			float position[3];
			float speed;
			float time;
			float onRock;
		};
		std::vector<PathPoint> path;
		std::vector<float> lengths;

		for (const auto& lip : lips) {
			if (lip.points.size() < 2)
				continue;
			// Samples every STREAM_SPACING along the lip, with the outward direction there
			const auto count = std::max<uint32_t>(2, static_cast<uint32_t>(std::lround(lip.length / STREAM_SPACING)) + 1);
			const float spacing = lip.length / static_cast<float>(count - 1);
			std::size_t segment = 0;
			float segmentStart = 0.0f;
			for (uint32_t sample = 0; sample < count; sample++) {
				const float distance = std::min(static_cast<float>(sample) * spacing, lip.length);
				float segmentLength = 0.0f;
				for (;;) {
					const auto& a = lip.points[segment].position;
					const auto& b = lip.points[segment + 1].position;
					segmentLength = std::hypot(b[0] - a[0], b[1] - a[1]);
					if (segmentStart + segmentLength >= distance || segment + 2 >= lip.points.size())
						break;
					segmentStart += segmentLength;
					segment++;
				}
				const auto& pa = lip.points[segment];
				const auto& pb = lip.points[segment + 1];
				const float t = segmentLength > 1e-3f ? std::clamp((distance - segmentStart) / segmentLength, 0.0f, 1.0f) : 0.0f;
				float normal[2] = { pa.normal[0] + (pb.normal[0] - pa.normal[0]) * t, pa.normal[1] + (pb.normal[1] - pa.normal[1]) * t };
				const float normalLength = std::hypot(normal[0], normal[1]);
				if (normalLength < 1e-3f)
					continue;
				normal[0] /= normalLength;
				normal[1] /= normalLength;
				float p[3] = { pa.position[0] + (pb.position[0] - pa.position[0]) * t, pa.position[1] + (pb.position[1] - pa.position[1]) * t, lip.level };
				float v[3] = { normal[0] * V0, normal[1] * V0, 0.0f };

				// In the water of a lower level: below its surface, where the ground is below it too
				const auto inPool = [&](float a_z, float a_ground, float& a_poolLevel) {
					for (const float level : a_levels) {
						if (level < lip.level - LEVEL_GAP && a_z <= level && a_ground < level) {
							a_poolLevel = level;
							return true;
						}
					}
					return false;
				};

				path.clear();
				path.push_back({ { p[0], p[1], p[2] }, V0, 0.0f, 0.0f });
				bool onRock = false;
				float time = 0.0f;
				StreamEnd end = StreamEnd::Outside;
				for (uint32_t step = 0; step < MAX_STEPS && time < MAX_TIME; step++) {
					const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
					const float dt = std::clamp(STEP_DISTANCE / std::max(speed, 1.0f), 1.0f / 600.0f, 1.0f / 30.0f);
					float vNext[3] = { v[0], v[1], v[2] - GRAVITY * dt };
					if (onRock) {
						// Down the slope: gravity's part along the rock, less friction
						float gradient[2] = { 0.0f, 0.0f };
						float hx0, hx1, hy0, hy1;
						if (Sample(p[0] - CELL, p[1], hx0) && Sample(p[0] + CELL, p[1], hx1) && Sample(p[0], p[1] - CELL, hy0) && Sample(p[0], p[1] + CELL, hy1)) {
							gradient[0] = (hx1 - hx0) / (2.0f * CELL);
							gradient[1] = (hy1 - hy0) / (2.0f * CELL);
						}
						const float g2 = gradient[0] * gradient[0] + gradient[1] * gradient[1];
						vNext[0] -= GRAVITY * gradient[0] / (1.0f + g2) * dt;
						vNext[1] -= GRAVITY * gradient[1] / (1.0f + g2) * dt;
						const float keep = std::max(0.0f, 1.0f - ROCK_FRICTION * dt);
						for (float& component : vNext)
							component *= keep;
					}
					float pNext[3];
					for (int k = 0; k < 3; k++)
						pNext[k] = p[k] + vNext[k] * dt;
					float ground;
					if (!Sample(pNext[0], pNext[1], ground))
						break;
					float poolLevel;
					if (inPool(pNext[2], ground, poolLevel)) {
						// Into the water below: the path ends at its surface
						const float fraction = p[2] > pNext[2] ? std::clamp((p[2] - poolLevel) / (p[2] - pNext[2]), 0.0f, 1.0f) : 1.0f;
						for (int k = 0; k < 2; k++)
							p[k] += (pNext[k] - p[k]) * fraction;
						p[2] = poolLevel;
						time += dt * fraction;
						path.push_back({ { p[0], p[1], p[2] }, speed, time, onRock ? 1.0f : 0.0f });
						end = StreamEnd::Pool;
						break;
					}
					if (pNext[2] <= ground + ROCK_OFFSET) {
						// On the rock: it runs over the surface, keeping the speed it had along it (no faster)
						pNext[2] = ground + ROCK_OFFSET;
						const float before = std::sqrt(vNext[0] * vNext[0] + vNext[1] * vNext[1] + vNext[2] * vNext[2]);
						for (int k = 0; k < 3; k++)
							vNext[k] = (pNext[k] - p[k]) / dt;
						const float after = std::sqrt(vNext[0] * vNext[0] + vNext[1] * vNext[1] + vNext[2] * vNext[2]);
						if (after > before && after > 1e-3f) {
							for (float& component : vNext)
								component *= before / after;
						}
						onRock = true;
					} else {
						// Above the rock: falling free (it leaves the rock where the rock drops away faster)
						onRock = false;
					}
					for (int k = 0; k < 3; k++) {
						p[k] = pNext[k];
						v[k] = vNext[k];
					}
					time += dt;
					const float newSpeed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
					path.push_back({ { p[0], p[1], p[2] }, newSpeed, time, onRock ? 1.0f : 0.0f });
					if (onRock && newSpeed < STALL_SPEED && time > 0.5f) {
						end = StreamEnd::Stalled;
						break;
					}
				}
				streamEnds[static_cast<int>(end)]++;
				if (path.size() < 2 || lip.level - path.back().position[2] < MIN_STREAM_DROP)
					continue;

				// Resampled to STREAM_ROWS points by length
				lengths.assign(path.size(), 0.0f);
				for (std::size_t i = 1; i < path.size(); i++) {
					const auto& a = path[i - 1].position;
					const auto& b = path[i].position;
					lengths[i] = lengths[i - 1] + std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]));
				}
				const float total = lengths.back();
				if (total < 1.0f)
					continue;
				const float across = 2.0f * static_cast<float>(sample) / static_cast<float>(count - 1) - 1.0f;
				std::size_t at = 0;
				for (uint32_t row = 0; row < STREAM_ROWS; row++) {
					const float along = static_cast<float>(row) / static_cast<float>(STREAM_ROWS - 1);
					const float target = along * total;
					while (at + 2 < path.size() && lengths[at + 1] < target)
						at++;
					const float span = lengths[at + 1] - lengths[at];
					const float f = span > 1e-4f ? std::clamp((target - lengths[at]) / span, 0.0f, 1.0f) : 0.0f;
					const auto& a = path[at];
					const auto& b = path[at + 1];
					StreamPoint point{};
					for (int k = 0; k < 3; k++)
						point.position[k] = a.position[k] + (b.position[k] - a.position[k]) * f;
					point.along = along;
					// Across the stream: along the lip, so the ribbons side by side make one sheet
					point.side[0] = -normal[1];
					point.side[1] = normal[0];
					point.speed = a.speed + (b.speed - a.speed) * f;
					// Overlapping its neighbours, spreading a little as it falls
					point.halfWidth = spacing * (0.65f + 0.35f * along);
					point.across = across;
					point.onRock = a.onRock + (b.onRock - a.onRock) * f;
					point.travelTime = a.time + (b.time - a.time) * f;
					streams.push_back(point);
				}
			}
		}
	}
}
