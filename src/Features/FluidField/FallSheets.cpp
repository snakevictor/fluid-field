#include "FallSheets.h"

#include "FallSites.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <map>

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
					if (dv > 1e-3f) {
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
		mean.z = 0.0f;
		mean = Normalized(mean);
		if (lips.size() < 2 || mean.Length() < 0.5f) {
			logger::info("[Fluid Field]   sheet skipped: {} lip points", lips.size());
			return nullptr;
		}

		// The lip line, A to B: the lip points spread across the mean way the water goes. Each column starts
		// on the upper water's surface, just upstream of the furthest-out lip point near it across
		const RE::NiPoint3 across(-mean.y, mean.x, 0.0f);
		float sA = FLT_MAX, sB = -FLT_MAX;
		for (const auto& lip : lips) {
			sA = std::min(sA, lip.Dot(across));
			sB = std::max(sB, lip.Dot(across));
		}
		const float width = sB - sA;
		if (width < 16.0f) {
			logger::info("[Fluid Field]   sheet skipped: lip only {:.0f} wide", width);
			return nullptr;
		}
		std::vector<RE::NiPoint3> starts(GUIDE_COLUMNS);
		std::vector<RE::NiPoint3> headings(GUIDE_COLUMNS);
		std::vector<uint8_t> found(GUIDE_COLUMNS, 0);
		for (uint32_t k = 0; k < GUIDE_COLUMNS; k++) {
			const float s = sA + width * static_cast<float>(k) / static_cast<float>(GUIDE_COLUMNS - 1);
			const float band = width / static_cast<float>(GUIDE_COLUMNS - 1) * 1.5f;
			int32_t best = -1;
			for (std::size_t i = 0; i < lips.size(); i++) {
				if (std::abs(lips[i].Dot(across) - s) > band)
					continue;
				if (best < 0 || lips[i].Dot(mean) > lips[best].Dot(mean))
					best = static_cast<int32_t>(i);
			}
			if (best < 0)
				continue;
			found[k] = 1;
			// On the line across at s, at the lip point's distance along the flow
			starts[k] = across * s + mean * lips[best].Dot(mean);
			starts[k].z = a_fall.upper + 2.0f;
			starts[k] -= mean * UPSTREAM;
			headings[k] = heading[best].Length() > 0.5f ? Normalized(heading[best] + mean) : mean;
		}
		// Gaps across (between pieces) take their neighbours' lip
		for (uint32_t k = 0; k < GUIDE_COLUMNS; k++) {
			if (found[k])
				continue;
			int32_t left = -1, right = -1;
			for (int32_t j = static_cast<int32_t>(k) - 1; j >= 0 && left < 0; j--)
				left = found[j] ? j : -1;
			for (uint32_t j = k + 1; j < GUIDE_COLUMNS && right < 0; j++)
				right = found[j] ? static_cast<int32_t>(j) : -1;
			if (left < 0 && right < 0)
				continue;
			const int32_t a = left >= 0 ? left : right, b = right >= 0 ? right : left;
			const float t = a == b ? 0.0f : (static_cast<float>(k) - static_cast<float>(a)) / static_cast<float>(b - a);
			const float s = sA + width * static_cast<float>(k) / static_cast<float>(GUIDE_COLUMNS - 1);
			const float along = starts[a].Dot(mean) * (1.0f - t) + starts[b].Dot(mean) * t;
			starts[k] = across * s + mean * along;
			starts[k].z = a_fall.upper + 2.0f;
			headings[k] = Normalized(headings[a] * (1.0f - t) + headings[b] * t);
		}

		// The lip line smoothed across (vanilla's lips zigzag): each start's distance along the flow, averaged over its neighbours
		{
			std::vector<float> along(GUIDE_COLUMNS);
			for (uint32_t k = 0; k < GUIDE_COLUMNS; k++)
				along[k] = starts[k].Dot(mean);
			for (uint32_t k = 0; k < GUIDE_COLUMNS; k++) {
				float sum = 0.0f, count = 0.0f;
				for (int32_t j = static_cast<int32_t>(k) - 3; j <= static_cast<int32_t>(k) + 3; j++) {
					if (j >= 0 && j < static_cast<int32_t>(GUIDE_COLUMNS)) {
						sum += along[j];
						count += 1.0f;
					}
				}
				starts[k] += mean * (sum / count - along[k]);
			}
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
		{
			const RE::BSReadLockGuard lock(bhkWorld->worldLock);
			Fall fall = a_fall;
			if (topSamples > 0.0f)
				fall.speed = topSpeed / topSamples;
			fall.speed = std::clamp(std::max(fall.speed, SLOWEST_TOP) * arcScale, SLOWEST_TOP * 0.25f, FASTEST_TOP);
			logger::info("[Fluid Field]   sheet: water at the top {:.0f} units/s, {:.0f} wide", fall.speed, width);
			Drape(world, a_sites, starts, mean, fall, columns);
		}
		for (uint32_t k = 0; k < GUIDE_COLUMNS; k++) {
			auto& column = columns[k];
			column.u = (sA + width * static_cast<float>(k) / static_cast<float>(GUIDE_COLUMNS - 1)) / ACROSS_REPEAT;
		}
		// Each column resampled to ROWS rows by distance along it; then filled across to COLUMNS
		const auto resample = [](const Column& a_column, float a_f, RE::NiPoint3& a_point, float& a_time) {
			float total = 0.0f;
			for (std::size_t i = 1; i < a_column.points.size(); i++)
				total += (a_column.points[i] - a_column.points[i - 1]).Length();
			float target = a_f * total, run = 0.0f;
			for (std::size_t i = 1; i < a_column.points.size(); i++) {
				const float length = (a_column.points[i] - a_column.points[i - 1]).Length();
				if (run + length >= target || i + 1 == a_column.points.size()) {
					const float t = length > 1e-4f ? std::clamp((target - run) / length, 0.0f, 1.0f) : 0.0f;
					a_point = a_column.points[i - 1] + (a_column.points[i] - a_column.points[i - 1]) * t;
					a_time = a_column.times[i - 1] + (a_column.times[i] - a_column.times[i - 1]) * t;
					return;
				}
				run += length;
			}
			a_point = a_column.points.back();
			a_time = a_column.times.back();
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

		// The grid in the parent's model space, through the engine's own builder
		const RE::NiTransform inverse = parent->world.Invert();
		std::vector<RE::NiPoint3> positions(grid.size()), normals(grid.size()), tangents(grid.size()), bitangents(grid.size());
		std::vector<RE::NiPoint2> uvs(grid.size());
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
				tangents[i] = Normalized(inverse.rotate * alongU);
				bitangents[i] = Normalized(inverse.rotate * alongV);
				// Along the flow, the texture runs with the water's travel time at vanilla's scroll
				uvs[i] = RE::NiPoint2(us[i], times[i] * SCROLL);
			}
		}
		std::vector<uint16_t> indices;
		indices.reserve(static_cast<std::size_t>(ROWS - 1) * (COLUMNS - 1) * 6);
		for (uint32_t r = 0; r + 1 < ROWS; r++) {
			for (uint32_t c = 0; c + 1 < COLUMNS; c++) {
				const auto a = static_cast<uint16_t>(r * COLUMNS + c), b = static_cast<uint16_t>(a + 1), d = static_cast<uint16_t>(a + COLUMNS), e = static_cast<uint16_t>(d + 1);
				indices.insert(indices.end(), { a, d, b, b, d, e });
			}
		}
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

	void FallSheets::DrawDebug(ImDrawList* a_drawList) const
	{
		auto* camera = RE::PlayerCamera::GetSingleton();
		RE::NiCamera* niCamera = nullptr;
		if (camera && camera->cameraRoot) {
			for (auto& child : camera->cameraRoot->GetChildren()) {
				if (child && (niCamera = netimmerse_cast<RE::NiCamera*>(child.get())) != nullptr)
					break;
			}
		}
		if (!niCamera || !a_drawList)
			return;
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		const auto project = [&](const RE::NiPoint3& a_p, ImVec2& a_screen) {
			float sx, sy, sz;
			if (!niCamera->WorldPtToScreenPt3(a_p, sx, sy, sz, 1e-5f) || sz <= 0.0f)
				return false;
			a_screen = ImVec2(sx * display.x, (1.0f - sy) * display.y);
			return true;
		};
		for (const auto& sheet : sheets) {
			for (std::size_t c = 0; c < sheet->columns.size(); c++) {
				const auto& points = sheet->columns[c];
				for (std::size_t i = 1; i < points.size(); i++) {
					ImVec2 a, b;
					if (project(points[i - 1], a) && project(points[i], b))
						a_drawList->AddLine(a, b, sheet->columnRock[c][i] ? IM_COL32(60, 140, 255, 255) : IM_COL32(255, 230, 60, 255), 2.0f);
				}
				ImVec2 a, b;
				if (c > 0 && project(sheet->columns[c - 1].front(), a) && project(points.front(), b))
					a_drawList->AddLine(a, b, IM_COL32(60, 255, 90, 255), 3.0f);
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
			// Rebuilt whole whenever the grounded pieces change: one fall per site and upper water level
			for (auto& sheet : sheets)
				Remove(*sheet);
			sheets.clear();
			pending.clear();
			groundVersion = a_sites.GetGroundVersion();
			std::map<std::pair<uint32_t, int32_t>, Fall> falls;
			for (const auto& mesh : a_sites.GetLitMeshes()) {
				auto* triShape = mesh.geometry ? mesh.geometry->AsTriShape() : nullptr;
				auto* renderer = mesh.geometry ? mesh.geometry->GetGeometryRuntimeData().rendererData : nullptr;
				if (!triShape || !renderer || !renderer->rawVertexData)
					continue;
				// The piece's lip height: its highest vertices' (the top of the drop)
				const uint32_t stride = renderer->vertexDesc.GetSize();
				const uint32_t positionOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
				float top = -FLT_MAX;
				for (uint32_t v = 0; v < triShape->GetTrishapeRuntimeData().vertexCount; v++) {
					float local[3];
					std::memcpy(local, renderer->rawVertexData + static_cast<std::size_t>(v) * stride + positionOffset, sizeof(local));
					top = std::max(top, (mesh.geometry->world * RE::NiPoint3(local[0], local[1], local[2])).z);
				}
				// Its upper water: the lowest level the lip stands under by at most LEVEL_REACH, or the lip itself;
				// its lower water: the highest level LEAST_DROP or more under that
				float upper = top;
				for (const float level : mesh.levels) {
					if (level >= top - LEVEL_REACH && level <= top + LEVEL_REACH)
						upper = level;
				}
				float lower = mesh.bottom;
				for (const float level : mesh.levels) {
					if (level <= upper - LEAST_DROP && level > lower)
						lower = level;
				}
				if (lower > upper - LEAST_DROP)
					continue;
				auto& fall = falls[{ mesh.siteId, static_cast<int32_t>(std::lround(upper / 16.0f)) }];
				// D63: the water's speed at the top, from this piece's texture density down the flow and vanilla's scroll
				float scale[2];
				if (a_sites.GetTextureScale(mesh.geometry.get(), scale)) {
					const float speed = std::clamp(scale[1] * SCROLL, SLOWEST_TOP, FASTEST_TOP);
					fall.speed = fall.pieces.empty() ? speed : 0.5f * (fall.speed + speed);
				}
				fall.pieces.push_back(mesh.geometry);
				fall.upper = upper;
				fall.lower = lower;
			}
			// Each fall's water levels below: its site's other falls' upper waters, and its own lower
			for (auto& [key, fall] : falls) {
				for (const auto& [other, otherFall] : falls) {
					if (other.first == key.first && otherFall.upper < fall.upper - LEAST_DROP && otherFall.upper > fall.lower)
						fall.levels.push_back(otherFall.upper);
				}
			}
			for (auto& [key, fall] : falls)
				pending.push_back(std::move(fall));
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
