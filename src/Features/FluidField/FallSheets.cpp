#include "FallSheets.h"

#include "FallCapture.h"
#include "FallSites.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <queue>

namespace FluidFieldSim
{
	namespace
	{
		std::string Lowercase(std::string a_text)
		{
			std::transform(a_text.begin(), a_text.end(), a_text.begin(), [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
			return a_text;
		}

		/** @brief The nearest rock along a ray (the falls' own collision and anything not rock passed through): its distance and its face's normal, turned to face the ray. */
		bool RockHit(RE::hkpWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_direction, float a_reach, float& a_distance, RE::NiPoint3& a_normal)
		{
			const float scale = RE::bhkWorld::GetWorldScale();
			float from = 0.0f;
			for (int step = 0; step < 6 && from < a_reach; step++) {
				const RE::NiPoint3 start = a_from + a_direction * from;
				const RE::NiPoint3 end = a_from + a_direction * a_reach;
				RE::hkpWorldRayCastInput input;
				input.from = RE::hkVector4(start.x * scale, start.y * scale, start.z * scale, 0.0f);
				input.to = RE::hkVector4(end.x * scale, end.y * scale, end.z * scale, 0.0f);
				RE::hkpWorldRayCastOutput output;
				a_world->CastRay(input, output);
				if (!output.HasHit())
					return false;
				const float hit = from + (a_reach - from) * output.hitFraction;
				const auto layer = output.rootCollidable->GetCollisionLayer();
				const bool solid = layer == RE::COL_LAYER::kStatic || layer == RE::COL_LAYER::kAnimStatic || layer == RE::COL_LAYER::kTerrain || layer == RE::COL_LAYER::kGround;
				const auto* ref = RE::TESHavokUtilities::FindCollidableRef(*output.rootCollidable);
				const auto* model = ref && ref->GetBaseObject() ? ref->GetBaseObject()->As<RE::TESModel>() : nullptr;
				const std::string name = model && model->GetModel() ? Lowercase(model->GetModel()) : std::string();
				if (solid && name.find("fxwaterfall") == std::string::npos && name.find("fxrapids") == std::string::npos) {
					float normal[4];
					_mm_storeu_ps(normal, output.normal.quad);
					a_normal = RE::NiPoint3(normal[0], normal[1], normal[2]);
					const float length = a_normal.Length();
					a_normal = length > 1e-4f ? a_normal / length : RE::NiPoint3(0.0f, 0.0f, 1.0f);
					if (a_normal.Dot(a_direction) > 0.0f)
						a_normal = -a_normal;
					a_distance = hit;
					return true;
				}
				from = hit + 0.5f;
			}
			return false;
		}

		RE::NiPoint3 Normalized(const RE::NiPoint3& a_v)
		{
			const float length = a_v.Length();
			return length > 1e-6f ? a_v / length : RE::NiPoint3(0.0f, 0.0f, 0.0f);
		}

		/** @brief D62: the engine's own mesh builder (77465, 1.7.104): a BSTriShape with its renderer data from separate arrays; null arrays are left out of its layout. */
		RE::BSTriShape* BuildShape(uint32_t a_triangles, const uint16_t* a_indices, uint32_t a_vertices, RE::NiPoint3* a_positions, RE::NiPoint2* a_uvs, RE::NiPoint3* a_normals,
			RE::NiPoint3* a_tangents, RE::NiPoint3* a_bitangents)
		{
			using Build_t = RE::BSTriShape* (*)(uint32_t, const uint16_t*, uint32_t, RE::NiPoint3*, RE::NiPoint2*, void*, RE::NiPoint3*, RE::NiPoint3*, RE::NiPoint3*, void*, void*, void*, void*,
				void*, void*);
			static REL::Relocation<Build_t> build{ REL::ID(77465) };
			return build(a_triangles, a_indices, a_vertices, a_positions, a_uvs, nullptr, a_normals, a_tangents, a_bitangents, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
		}

		/** @brief A copy of a lit property for our shape: two-sided, set up for it as a NIF load would. */
		void GiveProperty(RE::BSTriShape* a_shape, RE::BSShaderProperty* a_source)
		{
			if (!a_source)
				return;
			RE::NiCloningProcess cloning;
			auto* copy = static_cast<RE::BSShaderProperty*>(a_source->CreateClone(cloning));
			if (!copy)
				return;
			copy->SetFlags(RE::BSShaderProperty::EShaderPropertyFlag8::kTwoSided, true);
			a_shape->GetGeometryRuntimeData().shaderProperty.reset(copy);
			copy->SetupGeometry(a_shape);
			copy->FinishSetupGeometry(a_shape);
		}
	}

	bool FallSheets::Route(const FallSites& a_sites, const Fall& a_fall, std::vector<Column>& a_columns, std::vector<RE::NiPoint3>* a_edge, std::vector<RE::NiPoint3>* a_seeds)
	{
		const RE::NiPoint3& a_flow = a_fall.flow;
		FallSites::SiteGround ground;
		if (!a_sites.GetSiteGround(a_fall.siteId, ground))
			return false;
		const auto w = static_cast<int32_t>(ground.width), h = static_cast<int32_t>(ground.height);
		const std::size_t count = static_cast<std::size_t>(w) * h;
		const auto& bed = *ground.bed;
		const auto& cover = *ground.cover;
		const auto index = [&](int32_t a_x, int32_t a_y) { return static_cast<std::size_t>(a_y) * w + a_x; };
		const auto inside = [&](int32_t a_x, int32_t a_y) { return a_x >= 0 && a_y >= 0 && a_x < w && a_y < h; };
		const auto known = [&](std::size_t a_i) { return bed[a_i] > FallCapture::NONE; };
		const float upper = a_fall.upper;
		// D66: per cell, the upper water (under a plane at its level, the ground under its surface) and the water below
		// it (a pool under it, a plane LEAST_DROP or more under it with the ground under its surface, or ground under
		// the lower level): its surface there
		std::vector<uint8_t> upperWater(count, 0);
		std::vector<float> lowerWater(count, FallSites::NO_SURFACE);
		for (int32_t y = 0; y < h; y++) {
			for (int32_t x = 0; x < w; x++) {
				const std::size_t i = index(x, y);
				if (!known(i))
					continue;
				const float px = ground.originX + static_cast<float>(x) * ground.cell, py = ground.originY + static_cast<float>(y) * ground.cell;
				for (const auto& water : a_fall.waters) {
					if (px < water[0] || px > water[2] || py < water[1] || py > water[3] || bed[i] >= water[4])
						continue;
					if (std::abs(water[4] - upper) <= LEVEL_SAME)
						upperWater[i] = 1;
					else if (water[4] < upper - LEAST_DROP)
						lowerWater[i] = std::max(lowerWater[i], water[4]);
				}
				if (cover[i] > FallSites::NO_SURFACE && cover[i] < upper - LEAST_DROP)
					lowerWater[i] = std::max(lowerWater[i], cover[i]);
				if (lowerWater[i] <= FallSites::NO_SURFACE && bed[i] < a_fall.lower)
					lowerWater[i] = a_fall.lower;
			}
		}
		const auto isUpper = [&](std::size_t a_i) { return upperWater[a_i] != 0; };
		const auto isLower = [&](std::size_t a_i) { return lowerWater[a_i] > FallSites::NO_SURFACE; };
		static constexpr int32_t DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

		// 1. The ground's hollows filled from the water below up, so every cell drains to it (priority flood)
		std::vector<float> filled(count, FLT_MAX);
		{
			using Entry = std::pair<float, std::size_t>;
			std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
			for (std::size_t i = 0; i < count; i++) {
				if (known(i) && isLower(i)) {
					filled[i] = std::min(bed[i], lowerWater[i]);
					open.emplace(filled[i], i);
				}
			}
			if (open.empty()) {
				logger::info("[Fluid Field]   route: no water below in the site's ground");
				return false;
			}
			while (!open.empty()) {
				const auto [level, i] = open.top();
				open.pop();
				if (level > filled[i])
					continue;
				const auto x = static_cast<int32_t>(i % w), y = static_cast<int32_t>(i / w);
				for (int k = 0; k < 8; k++) {
					const int32_t nx = x + DX[k], ny = y + DY[k];
					if (!inside(nx, ny))
						continue;
					const std::size_t j = index(nx, ny);
					if (!known(j) || isUpper(j))
						continue;
					const float next = std::max(bed[j], level + FILL_STEP);
					if (next < filled[j]) {
						filled[j] = next;
						open.emplace(next, j);
					}
				}
			}
		}
		const auto sample = [&](const std::vector<float>& a_field, float a_x, float a_y, float& a_value) {
			const float fx = (a_x - ground.originX) / ground.cell, fy = (a_y - ground.originY) / ground.cell;
			const auto ix = static_cast<int32_t>(std::floor(fx)), iy = static_cast<int32_t>(std::floor(fy));
			if (!inside(ix, iy) || !inside(ix + 1, iy + 1))
				return false;
			const float tx = fx - static_cast<float>(ix), ty = fy - static_cast<float>(iy);
			const float a = a_field[index(ix, iy)], b = a_field[index(ix + 1, iy)], c = a_field[index(ix, iy + 1)], d = a_field[index(ix + 1, iy + 1)];
			if (std::min({ a, b, c, d }) <= FallCapture::NONE || std::max({ a, b, c, d }) >= FLT_MAX * 0.5f)
				return false;
			a_value = (a * (1.0f - tx) + b * tx) * (1.0f - ty) + (c * (1.0f - tx) + d * tx) * ty;
			return true;
		};
		// D66: the ground's top round a point: the highest of its four cells (between them the bilinear one dips into
		// rounded rock, and the sheet fought it)
		const auto sampleTop = [&](float a_x, float a_y, float& a_value) {
			const float fx = (a_x - ground.originX) / ground.cell, fy = (a_y - ground.originY) / ground.cell;
			const auto ix = static_cast<int32_t>(std::floor(fx)), iy = static_cast<int32_t>(std::floor(fy));
			if (!inside(ix, iy) || !inside(ix + 1, iy + 1))
				return false;
			const float a = bed[index(ix, iy)], b = bed[index(ix + 1, iy)], c = bed[index(ix, iy + 1)], d = bed[index(ix + 1, iy + 1)];
			if (std::min({ a, b, c, d }) <= FallCapture::NONE)
				return false;
			a_value = std::max({ a, b, c, d });
			return true;
		};
		const auto cellOf = [&](float a_x, float a_y, int32_t& a_cx, int32_t& a_cy) {
			a_cx = static_cast<int32_t>(std::lround((a_x - ground.originX) / ground.cell));
			a_cy = static_cast<int32_t>(std::lround((a_y - ground.originY) / ground.cell));
			return inside(a_cx, a_cy);
		};
		// The downhill flow per cell over the filled ground: spread over the lower neighbours, weighted to the steeper
		// (multiple flow directions), then smoothed, so a column steered by it does not zigzag from cell to cell
		std::vector<RE::NiPoint3> flowField(count, RE::NiPoint3(0.0f, 0.0f, 0.0f));
		for (int32_t y = 0; y < h; y++) {
			for (int32_t x = 0; x < w; x++) {
				const std::size_t i = index(x, y);
				if (filled[i] >= FLT_MAX * 0.5f)
					continue;
				RE::NiPoint3 sum(0.0f, 0.0f, 0.0f);
				for (int k = 0; k < 8; k++) {
					const int32_t nx = x + DX[k], ny = y + DY[k];
					if (!inside(nx, ny) || filled[index(nx, ny)] >= FLT_MAX * 0.5f)
						continue;
					const float length = std::sqrt(static_cast<float>(DX[k] * DX[k] + DY[k] * DY[k]));
					const float drop = (filled[i] - filled[index(nx, ny)]) / length;
					if (drop > 0.0f)
						sum += RE::NiPoint3(static_cast<float>(DX[k]) / length, static_cast<float>(DY[k]) / length, 0.0f) * (drop * drop);
				}
				flowField[i] = Normalized(sum);
			}
		}
		for (int pass = 0; pass < 3; pass++) {
			std::vector<RE::NiPoint3> soft = flowField;
			for (int32_t y = 1; y + 1 < h; y++) {
				for (int32_t x = 1; x + 1 < w; x++) {
					RE::NiPoint3 sum(0.0f, 0.0f, 0.0f);
					for (int32_t dy = -1; dy <= 1; dy++) {
						for (int32_t dx = -1; dx <= 1; dx++)
							sum += flowField[index(x + dx, y + dy)] * ((dx == 0 ? 2.0f : 1.0f) * (dy == 0 ? 2.0f : 1.0f));
					}
					soft[index(x, y)] = Normalized(sum);
				}
			}
			flowField.swap(soft);
		}
		const auto downhill = [&](float a_x, float a_y, RE::NiPoint3& a_direction) {
			int32_t cx, cy;
			if (!cellOf(a_x, a_y, cx, cy))
				return false;
			a_direction = flowField[index(cx, cy)];
			return a_direction.Length() > 0.5f;
		};

		// 2. D66: the lip: the upper water's cells this fall spills from (FindFalls), ordered across the way it spills;
		// per strip across, the one furthest downstream (the brink), moved out to its cell's edge; the seeds spread
		// evenly along that line
		const RE::NiPoint3 across(-a_flow.y, a_flow.x, 0.0f);
		if (a_fall.lip.size() < 2)
			return false;
		float lo = FLT_MAX, hi = -FLT_MAX;
		for (const auto& p : a_fall.lip) {
			lo = std::min(lo, p.Dot(across));
			hi = std::max(hi, p.Dot(across));
		}
		std::map<int32_t, RE::NiPoint3> strips;
		for (const auto& p : a_fall.lip) {
			const auto key = static_cast<int32_t>(std::floor((p.Dot(across) - lo) / ground.cell));
			const auto it = strips.find(key);
			if (it == strips.end() || p.Dot(a_flow) > it->second.Dot(a_flow))
				strips[key] = p;
		}
		std::vector<RE::NiPoint3> brink;
		for (const auto& [key, p] : strips)
			brink.push_back(p + a_flow * (ground.cell * 0.5f));
		const std::size_t n = GUIDE_COLUMNS;
		std::vector<RE::NiPoint3> edge(n);
		for (std::size_t k = 0; k < n; k++) {
			const float s = lo + (hi - lo) * static_cast<float>(k) / static_cast<float>(n - 1);
			std::size_t j = 0;
			while (j + 2 < brink.size() && brink[j + 1].Dot(across) < s)
				j++;
			const std::size_t j1 = std::min(j + 1, brink.size() - 1);
			const float sa = brink[j].Dot(across), sb = brink[j1].Dot(across);
			const float t = sb - sa > 1e-3f ? std::clamp((s - sa) / (sb - sa), 0.0f, 1.0f) : 0.0f;
			edge[k] = brink[j] * (1.0f - t) + brink[j1] * t;
			edge[k].z = upper;
		}
		if (a_edge)
			*a_edge = a_fall.lip;
		if (a_seeds)
			*a_seeds = edge;
		// D66: the sheet's width down the fall: vanilla's, at each height, in proportion to its width at the top (the
		// lip's width is ours); the way down is the ground's
		const float lipWidth = hi - lo;
		const auto vanillaWidth = [&](int32_t a_band) {
			const auto bands = static_cast<int32_t>(a_fall.bandLo.size());
			for (int32_t d = 0; d < bands; d++) {
				for (const int32_t b : { a_band - d, a_band + d }) {
					if (b >= 0 && b < bands && a_fall.bandLo[b] <= a_fall.bandHi[b])
						return a_fall.bandHi[b] - a_fall.bandLo[b];
				}
			}
			return -1.0f;
		};
		const float vanillaTop = vanillaWidth(0);
		const auto widthAt = [&](float a_z) {
			const float width = vanillaWidth(static_cast<int32_t>(std::max((upper - a_z) / BAND, 0.0f)));
			if (width <= 0.0f || vanillaTop <= 0.0f)
				return lipWidth;
			return lipWidth * std::clamp(width / vanillaTop, 0.25f, 2.0f);
		};
		const float speed = std::clamp(a_fall.speed, SLOWEST_TOP * 0.25f, FASTEST_TOP);
		const float fast = std::clamp((speed - SLOWEST_TOP) / (FASTEST_TOP - SLOWEST_TOP), 0.0f, 1.0f);
		const float depth = WATER_DEPTH + (MOST_DEPTH - WATER_DEPTH) * fast;

		// 3. Each column traced from its seed: flat on the upper water to the edge, then over the ground with the edge's
		// speed, steered by the downhill flow (less when fast), thrown off drops on an arc, never under the ground plus
		// the water's depth, until the water below
		struct State
		{
			RE::NiPoint3 p, v;                                // Position; horizontal velocity
			float vz = 0.0f;                                  // Vertical speed while thrown
			bool air = false, done = false, onWater = false;  // D65: the seeds are on the lip already
			bool landed = false;                              // Ended in the water below
			float time = 0.0f;
		};
		std::vector<State> states(n);
		a_columns.assign(n, {});
		for (std::size_t k = 0; k < n; k++) {
			State& s = states[k];
			s.p = edge[k];
			RE::NiPoint3 dir;
			if (!downhill(s.p.x, s.p.y, dir))
				dir = a_flow;
			s.v = Normalized(dir + a_flow) * speed;
			// The top row: just back over the upper water, so the sheet overlaps it
			a_columns[k].points.push_back(s.p - Normalized(s.v) * OVERLAP);
			a_columns[k].times.push_back(0.0f);
			a_columns[k].onRock.push_back(0);
			a_columns[k].points.push_back(s.p);
			a_columns[k].times.push_back(OVERLAP / speed);
			a_columns[k].onRock.push_back(0);
			s.time = OVERLAP / speed;
		}
		for (uint32_t step = 0; step < ROUTE_STEPS; step++) {
			bool any = false;
			for (std::size_t k = 0; k < n; k++) {
				State& s = states[k];
				if (s.done)
					continue;
				any = true;
				const float horizontal = std::max(s.v.Length(), 40.0f);
				const float dt = ROUTE_STEP / horizontal;
				RE::NiPoint3 next = s.p + s.v * dt;
				float floor;
				if (!sampleTop(next.x, next.y, floor)) {
					s.done = true;
					continue;
				}
				// D64: over the upper river's bed it rides the upper water's surface (the pool map does not reach the
				// whole river: its bed lies just under the surface there); the brink is where the ground falls away
				// deeper than a riverbed under it
				float surface = floor + depth;
				// D66: rock standing out of the sheet by more than its thickness pokes through it (the sheet keeps its
				// own way: falling on); lower rock it covers
				if (floor > s.p.z - depth * 0.5f)
					surface = -FLT_MAX;
				if (s.onWater) {
					if (floor >= upper - RIVER_DEPTH) {
						surface = upper;
						next.z = upper;
					} else {
						s.onWater = false;
					}
				}
				if (s.onWater) {
					// Straight on at the edge's speed along the surface
				} else if (s.air) {
					s.vz -= GRAVITY * dt;
					next.z = s.p.z + s.vz * dt;
					if (next.z <= surface) {
						next.z = surface;
						s.air = false;
						s.vz = 0.0f;
						s.v *= 0.8f;  // The landing takes some of it
					}
				} else if (surface < s.p.z - ROUTE_STEP * 1.5f) {
					// The ground falls away steeper than the water can follow: it is thrown
					s.air = true;
					s.vz = 0.0f;
					next.z = s.p.z;
				} else {
					// Sliding: gravity along the ground's slope, less friction
					next.z = surface;
					const float slope = (s.p.z - surface) / ROUTE_STEP;
					s.v *= std::max(1.0f - SLIDE_FRICTION * dt, 0.0f);
					s.v += Normalized(s.v) * (GRAVITY * std::clamp(slope, -1.0f, 1.0f) * dt);
					if (s.v.Length() < 60.0f)
						s.v = Normalized(s.v) * 60.0f;
				}
				// Steered by the downhill flow; a fast column keeps more of its own way
				RE::NiPoint3 dir;
				if (!s.air && !s.onWater && downhill(next.x, next.y, dir)) {
					const float below = std::clamp((upper - STEER_BELOW - next.z) / STEER_BELOW + 1.0f, 0.0f, 1.0f);
					const float steer = STEER * below * (1.0f - 0.7f * std::clamp(s.v.Length() / FASTEST_TOP, 0.0f, 1.0f));
					const float keep = std::max(s.v.Length(), 40.0f);
					s.v = Normalized(Normalized(s.v) * (1.0f - steer) + dir * steer) * keep;
				}
				s.p = next;
				s.time += dt;
				// Into the water below
				int32_t cx, cy;
				if (cellOf(s.p.x, s.p.y, cx, cy) && isLower(index(cx, cy)) && (!s.air || s.p.z <= lowerWater[index(cx, cy)])) {
					s.p.z = std::max(lowerWater[index(cx, cy)] - POOL_TUCK, floor + 2.0f);
					s.done = true;
					s.landed = true;
				}
				// D64: at the vanilla pieces' foot it has landed; out of their extent it ends (vanilla's grouping says
				// where the fall goes)
				if (!s.done && !s.air && !s.onWater && s.p.z <= a_fall.lower + LANDED) {
					s.p.z = std::min(s.p.z, a_fall.lower) - POOL_TUCK;
					s.done = true;
				}
				if (!s.done && s.p.z <= a_fall.lower - POOL_TUCK) {
					s.p.z = std::max(a_fall.lower - POOL_TUCK, floor + 2.0f);
					s.done = true;
				}
				a_columns[k].points.push_back(s.p);
				a_columns[k].times.push_back(s.time);
				a_columns[k].onRock.push_back(s.air ? 0 : 1);
			}
			if (!any)
				break;
			// Held together: each column's heading drawn a little toward its neighbours'
			std::vector<RE::NiPoint3> headings(n);
			for (std::size_t k = 0; k < n; k++)
				headings[k] = Normalized(states[k].v);
			for (std::size_t k = 0; k < n; k++) {
				State& s = states[k];
				if (s.done)
					continue;
				RE::NiPoint3 sum(0.0f, 0.0f, 0.0f);
				for (const std::size_t j : { k - 1, k + 1 }) {
					if (j < n && !states[j].done)
						sum += headings[j];
				}
				if (sum.Length() < 1e-3f)
					continue;
				s.v = Normalized(headings[k] * 0.75f + Normalized(sum) * 0.25f) * s.v.Length();
			}
			// D66: each column drawn to its place across the sheet's width at its height (the sheet keeps its width, as
			// vanilla's does, instead of gathering into the ground's channels)
			float centre = 0.0f, active = 0.0f;
			for (std::size_t k = 0; k < n; k++) {
				if (states[k].done)
					continue;
				const float u = static_cast<float>(k) / static_cast<float>(n - 1) - 0.5f;
				centre += states[k].p.Dot(across) - u * widthAt(states[k].p.z);
				active += 1.0f;
			}
			if (active >= 2.0f) {
				centre /= active;
				for (std::size_t k = 0; k < n; k++) {
					State& s = states[k];
					if (s.done)
						continue;
					const float u = static_cast<float>(k) / static_cast<float>(n - 1) - 0.5f;
					s.p += across * ((centre + u * widthAt(s.p.z) - s.p.Dot(across)) * SPACING_PULL);
				}
			}
		}
		uint32_t reached = 0;
		for (const auto& s : states)
			reached += s.landed ? 1 : 0;
		logger::info("[Fluid Field]   route: {} lip cells, {:.0f} wide, {:.0f} units/s at the edge, {:.0f} thick; {} of {} columns reach the water below", a_fall.lip.size(), lipWidth, speed, depth,
			reached, n);
		return true;
	}

	void FallSheets::Drape(RE::hkpWorld* a_world, const FallSites& a_sites, const std::vector<RE::NiPoint3>& a_starts, const RE::NiPoint3& a_flow, const Fall& a_fall, std::vector<Column>& a_columns)
	{
		// D63, kept simple: the sheet starts where the upper water's mesh ends, falls on the arc of the water's speed
		// there into the water below, and is only mildly disturbed by the rock under it
		const std::size_t n = a_starts.size();
		const uint32_t rows = DRAPE_ROWS;
		const RE::NiPoint3 up(0.0f, 0.0f, 1.0f);
		std::vector<RE::NiPoint3> grid(n * rows);
		std::vector<uint8_t> lifted(grid.size(), 0);
		const auto at = [&](std::size_t a_k, uint32_t a_r) -> RE::NiPoint3& { return grid[a_r * n + a_k]; };
		const float speed = std::clamp(a_fall.speed, SLOWEST_TOP, FASTEST_TOP);
		const float fast = (speed - SLOWEST_TOP) / (FASTEST_TOP - SLOWEST_TOP);  // 0 slow and thin, 1 fast and thick
		const float lift = SLOW_LIFT + (FAST_LIFT - SLOW_LIFT) * fast;
		const int smoothing = SLOW_SMOOTHING + static_cast<int>(std::lround(static_cast<float>(FAST_SMOOTHING - SLOW_SMOOTHING) * fast));

		// The brink: where the upper water's mesh (its water object's footprint) ends, along the flow from each column's
		// start; without one, the lip itself
		std::vector<RE::NiPoint3> brinks(n);
		float bounds[4];
		for (std::size_t k = 0; k < n; k++) {
			RE::NiPoint3 brink = a_starts[k] + a_flow * UPSTREAM;
			if (a_sites.WaterObjectBounds(a_starts[k].x, a_starts[k].y, a_fall.upper, LEVEL_REACH, bounds)) {
				float exit = BRINK_REACH;
				if (a_flow.x > 1e-4f)
					exit = std::min(exit, (bounds[2] - a_starts[k].x) / a_flow.x);
				else if (a_flow.x < -1e-4f)
					exit = std::min(exit, (bounds[0] - a_starts[k].x) / a_flow.x);
				if (a_flow.y > 1e-4f)
					exit = std::min(exit, (bounds[3] - a_starts[k].y) / a_flow.y);
				else if (a_flow.y < -1e-4f)
					exit = std::min(exit, (bounds[1] - a_starts[k].y) / a_flow.y);
				brink = a_starts[k] + a_flow * std::max(exit, 0.0f);
			}
			brink.z = a_fall.upper;
			brinks[k] = brink;
			if (k == 0 || k == n / 2 || k + 1 == n)
				logger::info("[Fluid Field]   brink {}: start ({:.0f}, {:.0f}), {}, brink ({:.0f}, {:.0f}), {:.0f} past the start", k, a_starts[k].x, a_starts[k].y,
					a_sites.WaterObjectBounds(a_starts[k].x, a_starts[k].y, a_fall.upper, LEVEL_REACH, bounds) ?
						std::format("water object ({:.0f}..{:.0f}, {:.0f}..{:.0f})", bounds[0], bounds[2], bounds[1], bounds[3]) :
						std::string("no water object at the level"),
					brink.x, brink.y, (brink - a_starts[k]).Dot(a_flow));
		}

		// The arc: row 0 back over the upper water, row 1 on the brink, then the ballistic fall at the top's speed,
		// rows evenly spaced in height, down past the water below by the plunge depth
		const float drop = a_fall.upper - a_fall.lower;
		const float landing = std::sqrt(speed * speed + 2.0f * GRAVITY * std::max(drop, 0.0f));
		const float plunge = std::clamp(landing * PLUNGE_PER_SPEED, LEAST_PLUNGE, MOST_PLUNGE);
		const float total = drop + plunge;
		for (std::size_t k = 0; k < n; k++) {
			at(k, 0) = brinks[k] - a_flow * OVERLAP;
			at(k, 0).z = a_fall.upper;
			for (uint32_t r = 1; r < rows; r++) {
				const float fallen = total * static_cast<float>(r - 1) / static_cast<float>(rows - 2);
				const float time = std::sqrt(2.0f * fallen / GRAVITY);
				at(k, r) = brinks[k] + a_flow * (speed * time) - up * fallen;
			}
		}

		// Mildly disturbed by the rock under it: each vertex looks PROBE_LENGTH under itself (against its facing, in the
		// fall's downstream-up plane); rock found lifts it by a share of how far the rock reaches into that look. A
		// vertex already in rock is left (the rock pokes through there)
		const auto facing = [&](std::size_t a_k, uint32_t a_r) {
			const RE::NiPoint3 along = at(a_k, std::min(a_r + 1, rows - 1)) - at(a_k, a_r > 0 ? a_r - 1 : 0);
			const RE::NiPoint3 normal = Normalized(a_flow * (-along.z) + up * along.Dot(a_flow));
			return normal.Length() > 0.5f ? normal : Normalized(a_flow + up);
		};
		{
			std::vector<RE::NiPoint3> moved = grid;
			for (std::size_t k = 0; k < n; k++) {
				for (uint32_t r = 2; r < rows; r++) {
					const RE::NiPoint3 normal = facing(k, r);
					float distance;
					RE::NiPoint3 hit;
					if (!RockHit(a_world, at(k, r), -normal, PROBE_LENGTH, distance, hit))
						continue;
					moved[r * n + k] += normal * (lift * (PROBE_LENGTH - distance));
					lifted[r * n + k] = 1;
				}
			}
			grid.swap(moved);
		}
		// Smoothed, more for a fast, thick fall (small bumps disturb it less); the brink rows held
		for (int pass = 0; pass < smoothing; pass++) {
			std::vector<RE::NiPoint3> soft = grid;
			for (std::size_t k = 0; k < n; k++) {
				for (uint32_t r = 2; r < rows; r++) {
					RE::NiPoint3 sum = at(k, r) * 2.0f;
					float weight = 2.0f;
					if (k > 0) {
						sum += at(k - 1, r);
						weight += 1.0f;
					}
					if (k + 1 < n) {
						sum += at(k + 1, r);
						weight += 1.0f;
					}
					sum += at(k, r - 1);
					weight += 1.0f;
					if (r + 1 < rows) {
						sum += at(k, r + 1);
						weight += 1.0f;
					}
					soft[r * n + k] = sum / weight;
				}
			}
			grid.swap(soft);
		}

		// Into the ground for good: the rows after the last one outside the rock (rock crossed an odd number of times by
		// a ray from far above) collapse onto where the sheet went in. Rock it passes through and comes out of again
		// (a boulder) is left: it pokes through
		{
			uint32_t trimmed = 0;
			for (std::size_t k = 0; k < n; k++) {
				uint32_t last = rows - 1;
				while (last > 1 && RockCrossings(a_world, at(k, last) + up * 2000.0f, at(k, last)) % 2 == 1)
					last--;
				if (last + 1 >= rows)
					continue;
				RE::NiPoint3 entry = at(k, last + 1);
				const RE::NiPoint3 step = at(k, last + 1) - at(k, last);
				float distance;
				RE::NiPoint3 hit;
				if (RockHit(a_world, at(k, last), Normalized(step), step.Length(), distance, hit))
					entry = at(k, last) + Normalized(step) * distance;
				for (uint32_t r = last + 1; r < rows; r++)
					at(k, r) = entry;
				trimmed += rows - 1 - last;
			}
			logger::info("[Fluid Field]   sheet: {} rows of {} columns trimmed where it went into the ground", trimmed, n);
		}

		// Under the water below: never under its bed
		for (std::size_t k = 0; k < n; k++) {
			for (uint32_t r = 2; r < rows; r++) {
				RE::NiPoint3& p = at(k, r);
				if (p.z >= a_fall.lower)
					continue;
				float distance;
				RE::NiPoint3 hit;
				const RE::NiPoint3 start(p.x, p.y, a_fall.lower + 8.0f);
				if (RockHit(a_world, start, -up, 4000.0f, distance, hit))
					p.z = std::max(p.z, start.z - distance + BED_CLEARANCE);
			}
		}

		// The columns, with the water's travel time down them (its speed from the height it has fallen)
		a_columns.assign(n, {});
		for (std::size_t k = 0; k < n; k++) {
			float time = 0.0f;
			for (uint32_t r = 0; r < rows; r++) {
				if (r > 0) {
					const RE::NiPoint3 step = at(k, r) - at(k, r - 1);
					const float v = std::sqrt(speed * speed + 2.0f * GRAVITY * std::max(a_fall.upper - at(k, r).z, 0.0f));
					time += step.Length() / std::max(v, 1.0f);
				}
				a_columns[k].points.push_back(at(k, r));
				a_columns[k].times.push_back(time);
				a_columns[k].onRock.push_back(lifted[r * n + k]);
			}
		}
	}

	void FallSheets::UpperWaterMesh(float a_level, const RE::NiPoint3& a_near, float a_reach, std::vector<std::array<RE::NiPoint3, 3>>& a_triangles,
		std::vector<std::array<RE::NiPoint3, 2>>& a_edges)
	{
		a_triangles.clear();
		a_edges.clear();
		auto* waterSystem = RE::TESWaterSystem::GetSingleton();
		if (!waterSystem)
			return;
		RE::BSSpinLockGuard guard(waterSystem->lock);
		for (const auto& waterObject : waterSystem->waterObjects) {
			if (!waterObject || !waterObject->shape)
				continue;
			const auto& plane = waterObject->plane;
			const float height = std::abs(plane.normal.z) > 1e-4f ? plane.constant / plane.normal.z : NAN;
			auto* shape = waterObject->shape.get();
			auto* triShape = shape->AsTriShape();
			auto* renderer = shape->GetGeometryRuntimeData().rendererData;
			const auto& counts = triShape ? triShape->GetTrishapeRuntimeData() : RE::BSTriShape::TRISHAPE_RUNTIME_DATA{};
			// The shape's extent in the world, for the log
			const RE::NiBound& bound = shape->worldBound;
			const bool close = std::abs(bound.center.x - a_near.x) < bound.radius + a_reach && std::abs(bound.center.y - a_near.y) < bound.radius + a_reach;
			if (!close)
				continue;
			logger::info("[Fluid Field]   upper water: object at {:.0f} (want {:.0f}), shape '{}' {} vertices {} triangles, raw data {}, world bound ({:.0f}, {:.0f}, {:.0f}) r {:.0f}",
				height, a_level, shape->name.c_str(), counts.vertexCount, counts.triangleCount, renderer && renderer->rawVertexData ? "yes" : "no", bound.center.x, bound.center.y,
				bound.center.z, bound.radius);
			if (std::isnan(height) || std::abs(bound.center.z - a_level) > LEVEL_REACH * 2.0f || !renderer || !renderer->rawVertexData || !renderer->rawIndexData)
				continue;
			const uint32_t stride = renderer->vertexDesc.GetSize();
			const uint32_t positionOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
			const auto vertex = [&](uint16_t a_index) {
				float local[3];
				std::memcpy(local, renderer->rawVertexData + static_cast<std::size_t>(a_index) * stride + positionOffset, sizeof(local));
				return shape->world * RE::NiPoint3(local[0], local[1], local[2]);
			};
			std::map<std::pair<uint16_t, uint16_t>, int> uses;
			float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
			for (uint32_t i = 0; i < counts.triangleCount; i++) {
				const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(i) * 3;
				if (index[0] >= counts.vertexCount || index[1] >= counts.vertexCount || index[2] >= counts.vertexCount)
					continue;
				const std::array<RE::NiPoint3, 3> tri = { vertex(index[0]), vertex(index[1]), vertex(index[2]) };
				a_triangles.push_back(tri);
				for (const auto& p : tri) {
					lo[0] = std::min(lo[0], p.x), lo[1] = std::min(lo[1], p.y), lo[2] = std::min(lo[2], p.z);
					hi[0] = std::max(hi[0], p.x), hi[1] = std::max(hi[1], p.y), hi[2] = std::max(hi[2], p.z);
				}
				for (int k = 0; k < 3; k++) {
					const uint16_t a = index[k], b = index[(k + 1) % 3];
					uses[{ std::min(a, b), std::max(a, b) }]++;
				}
			}
			std::size_t border = 0;
			for (const auto& [edge, count] : uses) {
				if (count != 1)
					continue;
				a_edges.push_back({ vertex(edge.first), vertex(edge.second) });
				border++;
			}
			logger::info("[Fluid Field]     its mesh spans x {:.0f}..{:.0f}, y {:.0f}..{:.0f}, z {:.0f}..{:.0f}; {} border edges", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], border);
		}
	}

	int FallSheets::RockCrossings(RE::hkpWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
	{
		const RE::NiPoint3 direction = Normalized(a_to - a_from);
		const float reach = (a_to - a_from).Length();
		int crossings = 0;
		float from = 0.0f;
		for (int step = 0; step < 16 && from < reach; step++) {
			float distance;
			RE::NiPoint3 normal;
			if (!RockHit(a_world, a_from + direction * from, direction, reach - from, distance, normal))
				break;
			crossings++;
			from += distance + 0.5f;
		}
		return crossings;
	}

	std::unique_ptr<FallSheets::Sheet> FallSheets::Build(const FallSites& a_sites, const Fall& a_fall)
	{
		// Every piece's vertices in the world, with its texture coordinates and colours; the first piece
		// lends the sheet its vertex layout, material and place in the scene
		RE::BSGeometry* first = a_fall.pieces.empty() ? nullptr : a_fall.pieces.front().get();
		auto* parent = first ? first->parent : nullptr;
		if (!parent || !REL::Module::IsAE()) {
			logger::info("[Fluid Field]   sheet skipped: no parent, or not 1.7.104 (the engine's mesh builder)");
			return nullptr;
		}
		std::vector<RE::NiPoint3> lips;     // Each piece's top edge
		std::vector<RE::NiPoint3> heading;  // ...and the way the water leaves it, level, one per lip point
		std::vector<std::array<uint8_t, 4>> colors;
		std::vector<float> colorRun;  // 0 at a piece's top, 1 at its foot
		RE::NiPoint3 mean(0.0f, 0.0f, 0.0f);
		float topSpeed = 0.0f, topSamples = 0.0f;  // D63: the water's speed at the top, from the pieces' texture density there
		for (const auto& piece : a_fall.pieces) {
			auto* triShape = piece ? piece->AsTriShape() : nullptr;
			auto* renderer = piece ? piece->GetGeometryRuntimeData().rendererData : nullptr;
			if (!triShape || !renderer || !renderer->rawVertexData || !renderer->rawIndexData)
				continue;
			const uint32_t pieceStride = renderer->vertexDesc.GetSize();
			const uint32_t piecePosition = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
			const uint32_t pieceUv = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_TEXCOORD0);
			const bool pieceColor = renderer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_COLORS);
			const uint32_t pieceColorOffset = pieceColor ? renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_COLOR) : 0;
			if (pieceUv < piecePosition + 12)
				continue;
			const auto& counts = triShape->GetTrishapeRuntimeData();
			std::vector<RE::NiPoint3> positions(counts.vertexCount);
			std::vector<std::array<float, 2>> uvs(counts.vertexCount);
			float lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
			for (uint32_t v = 0; v < counts.vertexCount; v++) {
				const uint8_t* data = renderer->rawVertexData + static_cast<std::size_t>(v) * pieceStride;
				float local[3];
				std::memcpy(local, data + piecePosition, sizeof(local));
				positions[v] = piece->world * RE::NiPoint3(local[0], local[1], local[2]);
				const auto* uv = reinterpret_cast<const uint16_t*>(data + pieceUv);
				uvs[v] = { DirectX::PackedVector::XMConvertHalfToFloat(uv[0]), DirectX::PackedVector::XMConvertHalfToFloat(uv[1]) };
				for (int k = 0; k < 2; k++) {
					lo[k] = std::min(lo[k], uvs[v][k]);
					hi[k] = std::max(hi[k], uvs[v][k]);
				}
			}
			if (hi[1] <= lo[1])
				continue;
			for (uint32_t v = 0; v < counts.vertexCount; v++) {
				const float run = (uvs[v][1] - lo[1]) / (hi[1] - lo[1]);
				if (pieceColor) {
					std::array<uint8_t, 4> color;
					std::memcpy(color.data(), renderer->rawVertexData + static_cast<std::size_t>(v) * pieceStride + pieceColorOffset, 4);
					colors.push_back(color);
					colorRun.push_back(run);
				}
				if (run > 0.06f)
					continue;
				// A top-edge vertex: the water leaves it toward this piece's vertices a little further down, level
				RE::NiPoint3 toward(0.0f, 0.0f, 0.0f);
				float weight = 0.0f;
				for (uint32_t w = 0; w < counts.vertexCount; w++) {
					const float wRun = (uvs[w][1] - lo[1]) / (hi[1] - lo[1]);
					const float du = std::abs(uvs[w][0] - uvs[v][0]) / std::max(hi[0] - lo[0], 1e-4f);
					if (wRun < 0.1f || wRun > 0.3f || du > 0.1f)
						continue;
					toward += positions[w] - positions[v];
					weight += 1.0f;
					const float dv = uvs[w][1] - uvs[v][1];
					if (dv > 1e-3f && positions[v].z >= a_fall.upper - LEVEL_REACH) {
						topSpeed += (positions[w] - positions[v]).Length() / dv * SCROLL;
						topSamples += 1.0f;
					}
				}
				toward.z = 0.0f;
				toward = Normalized(toward);
				lips.push_back(positions[v]);
				heading.push_back(toward);
				mean += toward;
			}
		}
		// D66: the way it goes and the lip, A to B, are the upper water's spill (FindFalls); vanilla's lips are only drawn
		mean = a_fall.flow;
		const RE::NiPoint3 across(-mean.y, mean.x, 0.0f);
		float sA = FLT_MAX, sB = -FLT_MAX;
		for (const auto& p : a_fall.lip) {
			sA = std::min(sA, p.Dot(across));
			sB = std::max(sB, p.Dot(across));
		}
		const float width = sB - sA;
		if (width < 16.0f) {
			logger::info("[Fluid Field]   sheet skipped: lip only {:.0f} wide", width);
			return nullptr;
		}
		auto* tes = RE::TES::GetSingleton();
		auto* cell = tes ? (tes->interiorCell ? tes->interiorCell : tes->GetCell(first->world.translate)) : nullptr;
		auto* bhkWorld = cell ? cell->GetbhkWorld() : nullptr;
		auto* world = bhkWorld ? bhkWorld->GetWorld1() : nullptr;
		if (!world) {
			logger::info("[Fluid Field]   sheet skipped: no physics world");
			return nullptr;
		}
		std::vector<Column> columns;
		std::vector<RE::NiPoint3> debugEdge, debugSeeds;
		float thickness = WATER_DEPTH;
		// D66 debug: the upper water's planes' borders (white)
		std::vector<std::array<RE::NiPoint3, 2>> waterEdges;
		for (const auto& water : a_fall.waters) {
			if (std::abs(water[4] - a_fall.upper) > LEVEL_SAME)
				continue;
			const RE::NiPoint3 corners[4] = { { water[0], water[1], water[4] }, { water[2], water[1], water[4] }, { water[2], water[3], water[4] }, { water[0], water[3], water[4] } };
			for (int k = 0; k < 4; k++)
				waterEdges.push_back({ corners[k], corners[(k + 1) % 4] });
		}
		{
			const RE::BSReadLockGuard lock(bhkWorld->worldLock);
			Fall fall = a_fall;
			if (topSamples > 0.0f)
				fall.speed = topSpeed / topSamples;
			fall.speed = std::clamp(std::max(fall.speed, SLOWEST_TOP) * arcScale, SLOWEST_TOP * 0.25f, FASTEST_TOP);
			thickness = WATER_DEPTH + (MOST_DEPTH - WATER_DEPTH) * std::clamp((fall.speed - SLOWEST_TOP) / (FASTEST_TOP - SLOWEST_TOP), 0.0f, 1.0f);
			logger::info("[Fluid Field]   sheet: water at the top {:.0f} units/s, {:.0f} wide", fall.speed, width);
			// D64: routed over the site's ground; the drape only where its ground is not known
			if (!Route(a_sites, fall, columns, &debugEdge, &debugSeeds)) {
				// D64: no route, no sheet: vanilla's pieces stay (the drape went straight into the ground)
				logger::info("[Fluid Field]   sheet skipped: no route over the ground; vanilla's pieces stay");
				return nullptr;
			}
		}
		for (uint32_t k = 0; k < GUIDE_COLUMNS; k++) {
			auto& column = columns[k];
			column.u = (sA + width * static_cast<float>(k) / static_cast<float>(GUIDE_COLUMNS - 1)) / ACROSS_REPEAT;
		}
		// D64: each column resampled to ROWS rows by the march's step, the time all columns share (by its length it
		// stitched rows of short and long columns across the rock between them); a column that ended stays where it
		// ended; then filled across to COLUMNS
		std::size_t longest = 2;
		for (const auto& column : columns)
			longest = std::max(longest, column.points.size());
		const auto resample = [&](const Column& a_column, float a_f, RE::NiPoint3& a_point, float& a_time) {
			const float place = a_f * static_cast<float>(longest - 1);
			const auto i0 = std::min(static_cast<std::size_t>(place), a_column.points.size() - 1);
			const auto i1 = std::min(i0 + 1, a_column.points.size() - 1);
			const float t = std::clamp(place - static_cast<float>(i0), 0.0f, 1.0f);
			a_point = a_column.points[i0] + (a_column.points[i1] - a_column.points[i0]) * t;
			a_time = a_column.times[i0] + (a_column.times[i1] - a_column.times[i0]) * t;
		};
		const uint32_t guides = static_cast<uint32_t>(columns.size());
		std::vector<RE::NiPoint3> grid(static_cast<std::size_t>(ROWS) * COLUMNS);
		std::vector<float> times(grid.size()), us(grid.size());
		for (uint32_t r = 0; r < ROWS; r++) {
			const float f = static_cast<float>(r) / static_cast<float>(ROWS - 1);
			std::vector<RE::NiPoint3> guidePoints(guides);
			std::vector<float> guideTimes(guides);
			for (uint32_t k = 0; k < guides; k++)
				resample(columns[k], f, guidePoints[k], guideTimes[k]);
			for (uint32_t c = 0; c < COLUMNS; c++) {
				const float place = static_cast<float>(c) / static_cast<float>(COLUMNS - 1) * static_cast<float>(guides - 1);
				const auto k0 = std::min(static_cast<uint32_t>(place), guides - 2);
				const float t = place - static_cast<float>(k0);
				const std::size_t i = static_cast<std::size_t>(r) * COLUMNS + c;
				grid[i] = guidePoints[k0] * (1.0f - t) + guidePoints[k0 + 1] * t;
				times[i] = guideTimes[k0] * (1.0f - t) + guideTimes[k0 + 1] * t;
				us[i] = columns[k0].u * (1.0f - t) + columns[k0 + 1].u * t;
			}
		}
		// Softened across and along, so the columns do not show as strands
		for (int pass = 0; pass < 1; pass++) {
			std::vector<RE::NiPoint3> soft = grid;
			for (uint32_t r = 0; r < ROWS; r++) {
				for (uint32_t c = 0; c < COLUMNS; c++) {
					RE::NiPoint3 total(0.0f, 0.0f, 0.0f);
					float weight = 0.0f;
					for (int dr = -1; dr <= 1; dr++) {
						for (int dc = -1; dc <= 1; dc++) {
							const int nr = static_cast<int>(r) + dr, nc = static_cast<int>(c) + dc;
							if (nr < 0 || nc < 0 || nr >= static_cast<int>(ROWS) || nc >= static_cast<int>(COLUMNS))
								continue;
							const float k = (dr == 0 ? 2.0f : 1.0f) * (dc == 0 ? 2.0f : 1.0f);
							total += grid[static_cast<std::size_t>(nr) * COLUMNS + nc] * k;
							weight += k;
						}
					}
					// The lip row stays on the lip
					if (r > 0)
						soft[static_cast<std::size_t>(r) * COLUMNS + c] = total / weight;
				}
			}
			grid.swap(soft);
		}
		// D66: never into the ground: each vertex but the lip row's CLEARANCE over the captured ground's highest cell round it
		{
			FallSites::SiteGround ground;
			if (a_sites.GetSiteGround(a_fall.siteId, ground)) {
				for (std::size_t i = COLUMNS; i < grid.size(); i++) {
					const float fx = (grid[i].x - ground.originX) / ground.cell, fy = (grid[i].y - ground.originY) / ground.cell;
					const auto ix = static_cast<int32_t>(std::floor(fx)), iy = static_cast<int32_t>(std::floor(fy));
					if (ix < 0 || iy < 0 || ix + 1 >= static_cast<int32_t>(ground.width) || iy + 1 >= static_cast<int32_t>(ground.height))
						continue;
					float top = FallCapture::NONE;
					for (int32_t dy = 0; dy <= 1; dy++) {
						for (int32_t dx = 0; dx <= 1; dx++)
							top = std::max(top, (*ground.bed)[static_cast<std::size_t>(iy + dy) * ground.width + ix + dx]);
					}
					// Rock standing out of it by more than its thickness pokes through instead
					if (top > FallCapture::NONE && top + CLEARANCE - grid[i].z <= thickness * 0.5f)
						grid[i].z = std::max(grid[i].z, top + CLEARANCE);
				}
			}
		}

		// The grid in the parent's model space, through the engine's own builder
		const RE::NiTransform inverse = parent->world.Invert();
		std::vector<RE::NiPoint3> positions(grid.size()), normals(grid.size()), tangents(grid.size()), bitangents(grid.size());
		std::vector<RE::NiPoint2> uvs(grid.size());
		std::vector<RE::NiPoint3> worldNormals(grid.size());
		const RE::NiPoint3 outward = mean + RE::NiPoint3(0.0f, 0.0f, 1.0f);
		for (uint32_t r = 0; r < ROWS; r++) {
			for (uint32_t c = 0; c < COLUMNS; c++) {
				const std::size_t i = static_cast<std::size_t>(r) * COLUMNS + c;
				const RE::NiPoint3 alongU = grid[static_cast<std::size_t>(r) * COLUMNS + std::min(c + 1, COLUMNS - 1)] - grid[static_cast<std::size_t>(r) * COLUMNS + (c > 0 ? c - 1 : 0)];
				const RE::NiPoint3 alongV = grid[static_cast<std::size_t>(std::min(r + 1, ROWS - 1)) * COLUMNS + c] - grid[static_cast<std::size_t>(r > 0 ? r - 1 : 0) * COLUMNS + c];
				RE::NiPoint3 normal = Normalized(alongU.Cross(alongV));
				if (normal.Dot(outward) < 0.0f)
					normal = -normal;
				positions[i] = inverse * grid[i];
				normals[i] = Normalized(inverse.rotate * normal);
				worldNormals[i] = normal;
				tangents[i] = Normalized(inverse.rotate * alongU);
				bitangents[i] = Normalized(inverse.rotate * alongV);
				// Along the flow, the texture runs with the water's travel time at vanilla's scroll
				uvs[i] = RE::NiPoint2(us[i], times[i] * SCROLL);
			}
		}
		// D64: no faces across a tear (an edge far longer than the columns' spacing at the lip: neighbours that went
		// round opposite sides of a rock) or under the captured ground
		const float spacing = std::max((grid[COLUMNS - 1] - grid[0]).Length() / static_cast<float>(COLUMNS - 1), 4.0f);
		const float tear = spacing * BREAK_LENGTH * static_cast<float>(COLUMNS) / static_cast<float>(GUIDE_COLUMNS);
		FallSites::SiteGround siteGround;
		const bool haveGround = a_sites.GetSiteGround(a_fall.siteId, siteGround);
		const auto underground = [&](const RE::NiPoint3& a_p) {
			if (!haveGround)
				return false;
			const auto x = static_cast<int32_t>(std::lround((a_p.x - siteGround.originX) / siteGround.cell));
			const auto y = static_cast<int32_t>(std::lround((a_p.y - siteGround.originY) / siteGround.cell));
			if (x < 0 || y < 0 || x >= static_cast<int32_t>(siteGround.width) || y >= static_cast<int32_t>(siteGround.height))
				return false;
			const float bed = (*siteGround.bed)[static_cast<std::size_t>(y) * siteGround.width + x];
			return bed > FallCapture::NONE && a_p.z < bed - 8.0f;
		};
		std::vector<uint16_t> indices;
		indices.reserve(static_cast<std::size_t>(ROWS - 1) * (COLUMNS - 1) * 6);
		uint32_t torn = 0;
		for (uint32_t r = 0; r + 1 < ROWS; r++) {
			for (uint32_t c = 0; c + 1 < COLUMNS; c++) {
				const auto a = static_cast<uint16_t>(r * COLUMNS + c), b = static_cast<uint16_t>(a + 1), d = static_cast<uint16_t>(a + COLUMNS), e = static_cast<uint16_t>(d + 1);
				// D64: one whole sheet: rock it crosses pokes through it (and drives the flow), nothing is cut out
				(void)tear;
				(void)underground;
				indices.insert(indices.end(), { a, d, b, b, d, e });
			}
		}
		(void)torn;
		if (indices.empty())
			return nullptr;
		RE::BSTriShape* shape = BuildShape(static_cast<uint32_t>(indices.size() / 3), indices.data(), static_cast<uint32_t>(grid.size()), positions.data(), uvs.data(), normals.data(),
			tangents.data(), bitangents.data());
		if (!shape) {
			logger::info("[Fluid Field]   sheet skipped: the engine's mesh builder returned nothing");
			return nullptr;
		}
		auto sheet = std::make_unique<Sheet>();
		sheet->shape.reset(shape);
		sheet->originals = a_fall.pieces;
		sheet->parent.reset(parent);
		shape->name = SHEET_NAME;
		GiveProperty(shape, first->GetGeometryRuntimeData().shaderProperty.get());
		parent->AttachChild(shape, true);
		RE::NiUpdateData update;
		shape->Update(update);
		for (const auto& piece : a_fall.pieces) {
			if (piece)
				piece->SetAppCulled(true);
		}
		for (const auto& column : columns) {
			sheet->columns.push_back(column.points);
			sheet->columnRock.push_back(column.onRock);
		}
		sheet->waterEdges.clear();
		{
			RE::NiPoint3 centre(0.0f, 0.0f, 0.0f);
			for (const auto& lip : lips)
				centre += lip;
			centre = centre / static_cast<float>(std::max<std::size_t>(lips.size(), 1));
			for (const auto& edge : waterEdges) {
				const RE::NiPoint3 mid = (edge[0] + edge[1]) * 0.5f;
				if (std::abs(mid.x - centre.x) < 6000.0f && std::abs(mid.y - centre.y) < 6000.0f)
					sheet->waterEdges.push_back(edge);
			}
		}
		// Debug views (D64): the grid, its normals, where it lies under the captured ground, the edge and the seeds
		sheet->grid = grid;
		sheet->normals = worldNormals;
		sheet->edge = std::move(debugEdge);
		sheet->seeds = std::move(debugSeeds);
		sheet->lips = lips;
		{
			FallSites::SiteGround ground;
			sheet->underground.assign(grid.size(), 0);
			if (a_sites.GetSiteGround(a_fall.siteId, ground)) {
				for (std::size_t i = 0; i < grid.size(); i++) {
					const auto x = static_cast<int32_t>(std::lround((grid[i].x - ground.originX) / ground.cell));
					const auto y = static_cast<int32_t>(std::lround((grid[i].y - ground.originY) / ground.cell));
					if (x < 0 || y < 0 || x >= static_cast<int32_t>(ground.width) || y >= static_cast<int32_t>(ground.height))
						continue;
					const float bed = (*ground.bed)[static_cast<std::size_t>(y) * ground.width + x];
					sheet->underground[i] = bed > FallCapture::NONE && grid[i].z < bed - 2.0f ? 1 : 0;
				}
			}
			const auto under = static_cast<std::size_t>(std::count(sheet->underground.begin(), sheet->underground.end(), static_cast<uint8_t>(1)));
			logger::info("[Fluid Field]   sheet: {} of {} vertices under the captured ground", under, grid.size());
		}
		// Debug: the columns and the grid, next to the log
		if (const auto directory = logger::log_directory()) {
			static int dumps = 0;
			std::ofstream file(*directory / std::format("FluidField_sheet{}.bin", dumps++), std::ios::binary);
			const uint32_t header[3] = { static_cast<uint32_t>(columns.size()), ROWS, COLUMNS };
			const float levels[2] = { a_fall.upper, a_fall.lower };
			file.write(reinterpret_cast<const char*>(header), sizeof(header));
			file.write(reinterpret_cast<const char*>(levels), sizeof(levels));
			for (const auto& column : columns) {
				const auto n = static_cast<uint32_t>(column.points.size());
				file.write(reinterpret_cast<const char*>(&n), sizeof(n));
				for (uint32_t i = 0; i < n; i++) {
					const float point[4] = { column.points[i].x, column.points[i].y, column.points[i].z, static_cast<float>(column.onRock[i]) };
					file.write(reinterpret_cast<const char*>(point), sizeof(point));
				}
			}
			file.write(reinterpret_cast<const char*>(grid.data()), static_cast<std::streamsize>(grid.size() * sizeof(RE::NiPoint3)));
		}
		return sheet;
	}

	void FallSheets::Remove(Sheet& a_sheet)
	{
		if (a_sheet.shape) {
			if (a_sheet.parent)
				a_sheet.parent->DetachChild(a_sheet.shape.get());
			a_sheet.shape.reset();
		}
		for (const auto& piece : a_sheet.originals) {
			if (piece)
				piece->SetAppCulled(false);
		}
	}

	void FallSheets::DrawDebug(ImDrawList* a_drawList, DebugView a_view) const
	{
		if (a_view == DebugView::Off || !a_drawList)
			return;
		auto* camera = RE::PlayerCamera::GetSingleton();
		RE::NiCamera* niCamera = nullptr;
		if (camera && camera->cameraRoot) {
			for (auto& child : camera->cameraRoot->GetChildren()) {
				if (child && (niCamera = netimmerse_cast<RE::NiCamera*>(child.get())) != nullptr)
					break;
			}
		}
		if (!niCamera)
			return;
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		const auto project = [&](const RE::NiPoint3& a_p, ImVec2& a_screen) {
			float sx, sy, sz;
			if (!niCamera->WorldPtToScreenPt3(a_p, sx, sy, sz, 1e-5f) || sz <= 0.0f)
				return false;
			a_screen = ImVec2(sx * display.x, (1.0f - sy) * display.y);
			return true;
		};
		const bool all = a_view == DebugView::All;
		for (const auto& sheet : sheets) {
			const auto& grid = sheet->grid;
			if (grid.size() != static_cast<std::size_t>(ROWS) * COLUMNS)
				continue;
			// Normals: each face filled with its normal as a colour
			if (a_view == DebugView::Normals || all) {
				for (uint32_t r = 0; r + 1 < ROWS; r++) {
					for (uint32_t c = 0; c + 1 < COLUMNS; c++) {
						const std::size_t i = static_cast<std::size_t>(r) * COLUMNS + c;
						ImVec2 a, b, d, e;
						if (!project(grid[i], a) || !project(grid[i + 1], b) || !project(grid[i + COLUMNS], d) || !project(grid[i + COLUMNS + 1], e))
							continue;
						const RE::NiPoint3& n = sheet->normals[i];
						const ImU32 color = IM_COL32(static_cast<int>((n.x * 0.5f + 0.5f) * 255.0f), static_cast<int>((n.y * 0.5f + 0.5f) * 255.0f),
							static_cast<int>((n.z * 0.5f + 0.5f) * 255.0f), all ? 120 : 220);
						a_drawList->AddQuadFilled(a, b, e, d, color);
					}
				}
			}
			// Wireframe: the grid, red where it lies under the captured ground
			if (a_view == DebugView::Wireframe || all) {
				for (uint32_t r = 0; r < ROWS; r++) {
					for (uint32_t c = 0; c < COLUMNS; c++) {
						const std::size_t i = static_cast<std::size_t>(r) * COLUMNS + c;
						ImVec2 a, b;
						if (!project(grid[i], a))
							continue;
						const bool under = !sheet->underground.empty() && sheet->underground[i];
						const ImU32 color = under ? IM_COL32(255, 40, 40, 255) : IM_COL32(255, 230, 60, 200);
						if (c + 1 < COLUMNS && project(grid[i + 1], b))
							a_drawList->AddLine(a, b, color, 1.0f);
						if (r + 1 < ROWS && project(grid[i + COLUMNS], b))
							a_drawList->AddLine(a, b, color, 1.0f);
					}
				}
			}
			// Lip: the detected edge, the seeds, vanilla's lips, and the routes
			if (a_view == DebugView::Lip || all) {
				for (const auto& p : sheet->lips) {
					ImVec2 s;
					if (project(p, s))
						a_drawList->AddCircleFilled(s, 3.0f, IM_COL32(60, 230, 255, 255));
				}
				for (const auto& edge : sheet->waterEdges) {
					ImVec2 a, b;
					if (project(edge[0], a) && project(edge[1], b))
						a_drawList->AddLine(a, b, IM_COL32(255, 255, 255, 255), 3.0f);
				}
				for (const auto& p : sheet->edge) {
					ImVec2 s;
					if (project(p, s))
						a_drawList->AddCircleFilled(s, 4.0f, IM_COL32(255, 60, 255, 255));
				}
				for (std::size_t k = 0; k < sheet->seeds.size(); k++) {
					ImVec2 s, prev;
					if (!project(sheet->seeds[k], s))
						continue;
					a_drawList->AddCircleFilled(s, 6.0f, IM_COL32(60, 255, 90, 255));
					if (k > 0 && project(sheet->seeds[k - 1], prev))
						a_drawList->AddLine(prev, s, IM_COL32(60, 255, 90, 255), 3.0f);
				}
				for (std::size_t c = 0; c < sheet->columns.size(); c++) {
					const auto& points = sheet->columns[c];
					for (std::size_t i = 1; i < points.size(); i++) {
						ImVec2 a, b;
						if (project(points[i - 1], a) && project(points[i], b))
							a_drawList->AddLine(a, b, sheet->columnRock[c][i] ? IM_COL32(60, 140, 255, 255) : IM_COL32(255, 150, 40, 255), 2.0f);
					}
				}
			}
		}
	}

	void FallSheets::ToggleTestQuad(const FallSites& a_sites)
	{
		if (testQuad) {
			if (testQuadParent)
				testQuadParent->DetachChild(testQuad.get());
			testQuad.reset();
			testQuadParent.reset();
			logger::info("[Fluid Field] Test quad removed");
			return;
		}
		if (!REL::Module::IsAE()) {
			logger::warn("[Fluid Field] Test quad: the engine's mesh builder is only known on 1.7.104");
			return;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		auto* loaded = cell ? cell->GetRuntimeData().loadedData : nullptr;
		RE::NiNode* parent = loaded ? loaded->cell3D.get() : nullptr;
		if (!parent) {
			logger::warn("[Fluid Field] Test quad: no cell 3D");
			return;
		}
		// A 300 x 300 quad standing 300 units in front of the player, facing them, at head height
		const float yaw = player->GetAngleZ();
		const RE::NiPoint3 forward(std::sin(yaw), std::cos(yaw), 0.0f);
		const RE::NiPoint3 right(std::cos(yaw), -std::sin(yaw), 0.0f);
		const RE::NiPoint3 up(0.0f, 0.0f, 1.0f);
		const RE::NiPoint3 centre = player->GetPosition() + forward * 300.0f + up * 120.0f;
		// Positions in the parent's model space
		const RE::NiTransform inverse = parent->world.Invert();
		RE::NiPoint3 positions[4];
		const float half = 150.0f;
		const RE::NiPoint3 corners[4] = { centre - right * half + up * half, centre + right * half + up * half, centre - right * half - up * half, centre + right * half - up * half };
		for (int i = 0; i < 4; i++)
			positions[i] = inverse * corners[i];
		const RE::NiPoint3 toPlayer = Normalized(inverse.rotate * (-forward));
		const RE::NiPoint3 tangent = Normalized(inverse.rotate * right);
		const RE::NiPoint3 bitangent = Normalized(inverse.rotate * (-up));
		RE::NiPoint3 normals[4] = { toPlayer, toPlayer, toPlayer, toPlayer };
		RE::NiPoint3 tangents[4] = { tangent, tangent, tangent, tangent };
		RE::NiPoint3 bitangents[4] = { bitangent, bitangent, bitangent, bitangent };
		RE::NiPoint2 uvs[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f } };
		const uint16_t indices[6] = { 0, 2, 1, 1, 2, 3 };

		logger::info("[Fluid Field] Test quad: building with the engine's mesh builder");
		RE::BSTriShape* shape = BuildShape(2, indices, 4, positions, uvs, normals, tangents, bitangents);
		if (!shape) {
			logger::warn("[Fluid Field] Test quad: the builder returned nothing");
			return;
		}
		testQuad.reset(shape);
		RE::BSShaderProperty* source = nullptr;
		for (const auto& mesh : a_sites.GetLitMeshes()) {
			if (mesh.geometry && (source = mesh.geometry->GetGeometryRuntimeData().shaderProperty.get()) != nullptr)
				break;
		}
		GiveProperty(shape, source);
		shape->name = "FluidFieldTestQuad";
		parent->AttachChild(shape, true);
		testQuadParent.reset(parent);
		RE::NiUpdateData update;
		shape->Update(update);
		logger::info("[Fluid Field] Test quad: attached at ({:.0f}, {:.0f}, {:.0f}), hidden flag {}", centre.x, centre.y, centre.z, shape->GetFlags().any(RE::NiAVObject::Flag::kHidden));
	}

	void FallSheets::Clear()
	{
		for (auto& sheet : sheets)
			Remove(*sheet);
		sheets.clear();
		pending.clear();
		groundVersion = UINT64_MAX;
		readyStreams = UINT32_MAX;
		if (testQuad && testQuadParent)
			testQuadParent->DetachChild(testQuad.get());
		testQuad.reset();
		testQuadParent.reset();
	}

	void FallSheets::FindFalls(const FallSites& a_sites)
	{
		// Each vanilla piece: its vertices, its top edge (v runs downstream on every vanilla falls mesh) and its water's speed
		struct Piece
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			float top = -FLT_MAX;
			float speed = 0.0f;
			std::vector<RE::NiPoint3> points, lips;
		};
		std::map<uint32_t, std::vector<Piece>> sites;
		for (const auto& mesh : a_sites.GetLitMeshes()) {
			auto* triShape = mesh.geometry ? mesh.geometry->AsTriShape() : nullptr;
			auto* renderer = mesh.geometry ? mesh.geometry->GetGeometryRuntimeData().rendererData : nullptr;
			if (!triShape || !renderer || !renderer->rawVertexData)
				continue;
			const uint32_t stride = renderer->vertexDesc.GetSize();
			const uint32_t positionOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
			const uint32_t uvOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_TEXCOORD0);
			const bool haveUv = uvOffset >= positionOffset + 12;
			const uint32_t vertices = triShape->GetTrishapeRuntimeData().vertexCount;
			Piece piece;
			piece.geometry = mesh.geometry;
			std::vector<float> vs(vertices, 0.0f);
			float vLo = FLT_MAX, vHi = -FLT_MAX;
			for (uint32_t v = 0; v < vertices; v++) {
				const uint8_t* data = renderer->rawVertexData + static_cast<std::size_t>(v) * stride;
				float local[3];
				std::memcpy(local, data + positionOffset, sizeof(local));
				const RE::NiPoint3 world = mesh.geometry->world * RE::NiPoint3(local[0], local[1], local[2]);
				piece.points.push_back(world);
				piece.top = std::max(piece.top, world.z);
				if (haveUv) {
					vs[v] = DirectX::PackedVector::XMConvertHalfToFloat(reinterpret_cast<const uint16_t*>(data + uvOffset)[1]);
					vLo = std::min(vLo, vs[v]);
					vHi = std::max(vHi, vs[v]);
				}
			}
			for (uint32_t v = 0; v < vertices; v++) {
				const bool topEdge = haveUv && vHi > vLo ? (vs[v] - vLo) / (vHi - vLo) <= 0.06f : piece.points[v].z >= piece.top - 8.0f;
				if (topEdge)
					piece.lips.push_back(piece.points[v]);
			}
			float scale[2];
			if (a_sites.GetTextureScale(mesh.geometry.get(), scale))
				piece.speed = std::clamp(scale[1] * SCROLL, SLOWEST_TOP, FASTEST_TOP);
			sites[mesh.siteId].push_back(std::move(piece));
		}
		static constexpr int32_t DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
		for (auto& [siteId, pieces] : sites) {
			FallSites::SiteGround ground;
			if (!a_sites.GetSiteGround(siteId, ground)) {
				logger::info("[Fluid Field]   site {}: its ground is not known yet", siteId);
				continue;
			}
			const auto w = static_cast<int32_t>(ground.width), h = static_cast<int32_t>(ground.height);
			const std::size_t count = static_cast<std::size_t>(w) * h;
			const auto& bed = *ground.bed;
			const auto index = [&](int32_t a_x, int32_t a_y) { return static_cast<std::size_t>(a_y) * w + a_x; };
			const auto inside = [&](int32_t a_x, int32_t a_y) { return a_x >= 0 && a_y >= 0 && a_x < w && a_y < h; };
			const auto known = [&](std::size_t a_i) { return bed[a_i] > FallCapture::NONE; };
			const auto worldX = [&](int32_t a_x) { return ground.originX + static_cast<float>(a_x) * ground.cell; };
			const auto worldY = [&](int32_t a_y) { return ground.originY + static_cast<float>(a_y) * ground.cell; };
			std::vector<std::array<float, 5>> waters;
			a_sites.CollectWaterPlanes(worldX(0), worldY(0), worldX(w - 1), worldY(h - 1), waters);
			// The site's water levels, highest first; planes this near in height are one water
			std::vector<float> levels;
			for (const auto& water : waters) {
				if (std::none_of(levels.begin(), levels.end(), [&](float a_level) { return std::abs(a_level - water[4]) <= LEVEL_SAME; }))
					levels.push_back(water[4]);
			}
			std::sort(levels.begin(), levels.end(), std::greater<float>());
			float lowestBed = FLT_MAX;
			for (std::size_t i = 0; i < count; i++) {
				if (known(i))
					lowestBed = std::min(lowestBed, bed[i]);
			}
			const auto under = [&](int32_t a_x, int32_t a_y, float a_lo, float a_hi) {
				// Under a plane between these heights, with the ground under its surface
				const std::size_t i = index(a_x, a_y);
				for (const auto& water : waters) {
					if (water[4] >= a_lo && water[4] <= a_hi && bed[i] < water[4] && worldX(a_x) >= water[0] && worldX(a_x) <= water[2] && worldY(a_y) >= water[1] && worldY(a_y) <= water[3])
						return true;
				}
				return false;
			};
			std::vector<Fall> falls;
			for (const float level : levels) {
				// The water at this level: cells under one of its planes whose ground lies under its surface
				std::vector<uint8_t> wet(count, 0);
				for (int32_t y = 0; y < h; y++) {
					for (int32_t x = 0; x < w; x++) {
						if (known(index(x, y)) && under(x, y, level - LEVEL_SAME, level + LEVEL_SAME))
							wet[index(x, y)] = 1;
					}
				}
				// Where it spills: a wet cell beside dry ground under its surface, the ground SPILL_OUT further on
				// LEAST_DROP under it (a drop, not a bank) and under no water only a little lower (a step in a river)
				std::vector<RE::NiPoint3> spills, outs;
				const auto reach = static_cast<int32_t>(std::ceil(SPILL_OUT / ground.cell));
				for (int32_t y = 0; y < h; y++) {
					for (int32_t x = 0; x < w; x++) {
						if (!wet[index(x, y)])
							continue;
						RE::NiPoint3 out(0.0f, 0.0f, 0.0f);
						for (int k = 0; k < 4; k++) {
							const int32_t nx = x + DX[k], ny = y + DY[k], fx = x + DX[k] * reach, fy = y + DY[k] * reach;
							if (!inside(nx, ny) || !inside(fx, fy) || wet[index(nx, ny)] || !known(index(nx, ny)) || !known(index(fx, fy)))
								continue;
							if (bed[index(nx, ny)] >= level - EDGE_DROP || bed[index(fx, fy)] >= level - LEAST_DROP || under(nx, ny, level - LEAST_DROP, level - LEVEL_SAME))
								continue;
							out += RE::NiPoint3(static_cast<float>(DX[k]), static_cast<float>(DY[k]), 0.0f);
						}
						if (out.Length() < 0.5f)
							continue;
						spills.emplace_back(worldX(x), worldY(y), level);
						outs.push_back(Normalized(out));
					}
				}
				if (spills.empty())
					continue;
				// Stretches: spills within SPLIT_ROCK of each other are one (a rock shorter than that stands in one
				// sheet); a longer stretch with no spill (a big rock) splits it
				std::vector<std::size_t> group(spills.size());
				for (std::size_t i = 0; i < spills.size(); i++)
					group[i] = i;
				const std::function<std::size_t(std::size_t)> root = [&](std::size_t a_i) { return group[a_i] == a_i ? a_i : group[a_i] = root(group[a_i]); };
				for (std::size_t i = 0; i < spills.size(); i++) {
					for (std::size_t j = i + 1; j < spills.size(); j++) {
						const float dx = spills[i].x - spills[j].x, dy = spills[i].y - spills[j].y;
						if (dx * dx + dy * dy <= SPLIT_ROCK * SPLIT_ROCK)
							group[root(i)] = root(j);
					}
				}
				std::map<std::size_t, std::vector<std::size_t>> stretches;
				for (std::size_t i = 0; i < spills.size(); i++)
					stretches[root(i)].push_back(i);
				for (const auto& [key, members] : stretches) {
					// Vanilla's lip near it, at this level: the trigger
					bool trigger = false;
					for (const auto& piece : pieces) {
						if (std::abs(piece.top - level) > LEVEL_REACH)
							continue;
						for (const auto& lip : piece.lips) {
							for (const std::size_t m : members) {
								const float dx = lip.x - spills[m].x, dy = lip.y - spills[m].y;
								if (dx * dx + dy * dy <= TRIGGER_REACH * TRIGGER_REACH) {
									trigger = true;
									break;
								}
							}
							if (trigger)
								break;
						}
						if (trigger)
							break;
					}
					Fall fall;
					fall.upper = level;
					fall.siteId = siteId;
					fall.waters = waters;
					RE::NiPoint3 flow(0.0f, 0.0f, 0.0f);
					for (const std::size_t m : members) {
						fall.lip.push_back(spills[m]);
						flow += outs[m];
					}
					fall.flow = Normalized(flow);
					if (fall.flow.Length() < 0.5f)
						continue;
					const RE::NiPoint3 across(-fall.flow.y, fall.flow.x, 0.0f);
					float lo = FLT_MAX, hi = -FLT_MAX;
					for (const auto& p : fall.lip) {
						lo = std::min(lo, p.Dot(across));
						hi = std::max(hi, p.Dot(across));
					}
					logger::info("[Fluid Field]   site {}: water at {:.0f} spills over {} cells, {:.0f} wide, toward ({:.2f}, {:.2f}) at ({:.0f}, {:.0f}): {}", siteId, level, members.size(),
						hi - lo, fall.flow.x, fall.flow.y, spills[members.front()].x, spills[members.front()].y, trigger ? "a fall (vanilla's lip is there)" : "no vanilla lip near: not a fall");
					if (!trigger || hi - lo < 32.0f)
						continue;
					fall.lower = lowestBed;
					for (const float other : levels) {
						if (other < level - LEAST_DROP && other > fall.lower)
							fall.lower = other;
					}
					falls.push_back(std::move(fall));
				}
			}
			// Each vanilla piece to the fall it is a segment of: the one whose upper water is the lowest at or over its
			// top, with its lip near the piece's top edge; others stay vanilla's
			std::vector<int32_t> owner(pieces.size(), -1);
			for (std::size_t p = 0; p < pieces.size(); p++) {
				const auto& tops = pieces[p].lips.empty() ? pieces[p].points : pieces[p].lips;
				float bestDistance = FLT_MAX;
				for (std::size_t f = 0; f < falls.size(); f++) {
					if (falls[f].upper < pieces[p].top - LEVEL_REACH)
						continue;
					float distance = FLT_MAX;
					for (const auto& a : tops) {
						for (const auto& b : falls[f].lip)
							distance = std::min(distance, (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
					}
					distance = std::sqrt(distance);
					if (distance > PIECE_REACH)
						continue;
					const bool lower = owner[p] >= 0 && falls[f].upper < falls[owner[p]].upper - LEVEL_SAME;
					const bool same = owner[p] >= 0 && std::abs(falls[f].upper - falls[owner[p]].upper) <= LEVEL_SAME;
					if (owner[p] < 0 || lower || (same && distance < bestDistance)) {
						owner[p] = static_cast<int32_t>(f);
						bestDistance = distance;
					}
				}
			}
			for (std::size_t p = 0; p < pieces.size(); p++) {
				if (owner[p] < 0)
					continue;
				Fall& fall = falls[owner[p]];
				fall.pieces.push_back(pieces[p].geometry);
				if (pieces[p].speed > 0.0f && std::abs(pieces[p].top - fall.upper) <= LEVEL_REACH)
					fall.speed = fall.speed == 150.0f ? pieces[p].speed : 0.5f * (fall.speed + pieces[p].speed);
				// Vanilla's width per band of height under the upper water
				const RE::NiPoint3 across(-fall.flow.y, fall.flow.x, 0.0f);
				for (const auto& point : pieces[p].points) {
					const auto band = static_cast<std::size_t>(std::max((fall.upper - point.z) / BAND, 0.0f));
					if (band >= fall.bandLo.size()) {
						fall.bandLo.resize(band + 1, FLT_MAX);
						fall.bandHi.resize(band + 1, -FLT_MAX);
					}
					fall.bandLo[band] = std::min(fall.bandLo[band], point.Dot(across));
					fall.bandHi[band] = std::max(fall.bandHi[band], point.Dot(across));
				}
			}
			for (auto& fall : falls) {
				logger::info("[Fluid Field]   site {}: fall from {:.0f} to {:.0f}, {} lip cells, {} vanilla pieces", siteId, fall.upper, fall.lower, fall.lip.size(), fall.pieces.size());
				if (!fall.pieces.empty())
					pending.push_back(std::move(fall));
			}
		}
	}

	void FallSheets::Update(const FallSites& a_sites, bool a_enabled, float a_arcScale)
	{
		if (a_arcScale != arcScale) {
			arcScale = a_arcScale;
			groundVersion = UINT64_MAX;
		}
		if (!a_enabled) {
			if (!sheets.empty() || !pending.empty() || groundVersion != UINT64_MAX)
				Clear();
			return;
		}
		if (a_sites.GetGroundVersion() != groundVersion || a_sites.CountReadyStreams() != readyStreams) {
			readyStreams = a_sites.CountReadyStreams();
			for (auto& sheet : sheets)
				Remove(*sheet);
			sheets.clear();
			pending.clear();
			groundVersion = a_sites.GetGroundVersion();
			FindFalls(a_sites);
			logger::info("[Fluid Field] Waterfall sheets: {} falls from {} pieces", pending.size(), a_sites.GetLitMeshes().size());
		}
		// One new sheet a frame
		if (pending.empty())
			return;
		Fall fall = std::move(pending.back());
		pending.pop_back();
		const auto start = std::chrono::steady_clock::now();
		auto sheet = Build(a_sites, fall);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		logger::info("[Fluid Field] Waterfall sheet ({} pieces, water {:.0f} to {:.0f}, {:.0f} units/s at the top): {} in {:.1f} ms", fall.pieces.size(), fall.upper, fall.lower, fall.speed,
			sheet ? "built" : "not built", ms);
		if (sheet)
			sheets.push_back(std::move(sheet));
	}
}
