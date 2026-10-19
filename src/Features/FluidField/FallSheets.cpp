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
	}

	void FallSheets::March(RE::hkpWorld* a_world, const FallSites& a_sites, const std::vector<RE::NiPoint3>& a_starts, const std::vector<RE::NiPoint3>& a_headings, const Fall& a_fall,
		std::vector<Column>& a_columns)
	{
		// Every column a step at a time, together:
		// - on the upper water while over it (its surface, level, at the lip's speed), until the brink;
		// - through the air on a ballistic arc;
		// - sliding on the rock (gravity along it, less friction) while it holds the water, off it where it falls away;
		// - ending in the first pool it enters, at the water below, or where it stalls on level rock.
		// After each step every column's heading is drawn toward its neighbours', so the sheet holds together
		const std::size_t n = a_starts.size();
		struct State
		{
			RE::NiPoint3 p, velocity, normal{ 0.0f, 0.0f, 1.0f };
			bool onRock = false, onWater = true, done = false;
			float time = 0.0f;
		};
		std::vector<State> states(n);
		a_columns.assign(n, {});
		for (std::size_t k = 0; k < n; k++) {
			states[k].p = a_starts[k];
			states[k].velocity = a_headings[k] * LIP_SPEED;
			a_columns[k].points.assign(1, a_starts[k]);
			a_columns[k].times.assign(1, 0.0f);
			a_columns[k].onRock.assign(1, 0);
		}
		const RE::NiPoint3 gravity(0.0f, 0.0f, -GRAVITY);
		for (uint32_t step = 0; step < MOST_STEPS; step++) {
			bool any = false;
			for (std::size_t k = 0; k < n; k++) {
				State& s = states[k];
				if (s.done)
					continue;
				any = true;
				const float dt = STEP / std::max(s.velocity.Length(), 60.0f);
				float level = 0.0f;
				const bool overPool = a_sites.PoolLevelAt(s.p.x, s.p.y, level);
				if (s.onWater) {
					// Riding the upper water's surface until it is no longer under the sheet
					RE::NiPoint3 flat(s.velocity.x, s.velocity.y, 0.0f);
					const RE::NiPoint3 next = s.p + flat * dt;
					float nextLevel = 0.0f;
					if (a_sites.PoolLevelAt(next.x, next.y, nextLevel) && nextLevel >= a_fall.upper - LEVEL_REACH) {
						s.p = RE::NiPoint3(next.x, next.y, a_fall.upper + 2.0f);
					} else {
						s.onWater = false;
						s.p = next;
					}
				} else if (s.onRock) {
					s.velocity += (gravity - s.normal * gravity.Dot(s.normal)) * dt;
					s.velocity = s.velocity - s.normal * s.velocity.Dot(s.normal);
					s.velocity *= std::max(1.0f - FRICTION * dt, 0.0f);
					RE::NiPoint3 next = s.p + s.velocity * dt;
					float distance;
					RE::NiPoint3 wall;
					const RE::NiPoint3 ahead = Normalized(s.velocity);
					if (s.velocity.Length() > 1.0f && RockHit(a_world, s.p, ahead, (next - s.p).Length() + FLOW_DEPTH, distance, wall)) {
						next = s.p + ahead * std::max(distance - FLOW_DEPTH, 0.0f);
						s.velocity = s.velocity - wall * s.velocity.Dot(wall);
						s.normal = Normalized(s.normal + wall);
					}
					RE::NiPoint3 under;
					const RE::NiPoint3 probeFrom = next + s.normal * PROBE;
					const RE::NiPoint3 probeDown = -s.normal;
					if (RockHit(a_world, probeFrom, probeDown, PROBE * 2.0f, distance, under) && distance < PROBE + FLOW_DEPTH * 2.0f) {
						s.normal = under;
						s.p = probeFrom + probeDown * distance + s.normal * FLOW_DEPTH;
					} else {
						s.onRock = false;
						s.p = next;
					}
					if (s.normal.z > 0.9f && s.velocity.Length() < STALL_SPEED && step > 4)
						s.done = true;
				} else {
					const RE::NiPoint3 next = s.p + s.velocity * dt + gravity * (0.5f * dt * dt);
					const RE::NiPoint3 travel = next - s.p;
					float distance;
					RE::NiPoint3 hit;
					if (RockHit(a_world, s.p, Normalized(travel), travel.Length() + FLOW_DEPTH, distance, hit)) {
						s.p = s.p + Normalized(travel) * distance + hit * FLOW_DEPTH;
						s.velocity = (s.velocity - hit * s.velocity.Dot(hit)) * 0.7f;
						s.normal = hit;
						s.onRock = true;
					} else {
						s.p = next;
						s.velocity += gravity * dt;
					}
				}
				const float speed = s.velocity.Length();
				if (speed > MOST_SPEED)
					s.velocity *= MOST_SPEED / speed;
				s.time += dt;
				// Into a pool (any but the one it came from), or the water below: ends just under its surface
				float poolLevel = 0.0f;
				const bool inPool = !s.onWater && a_sites.PoolLevelAt(s.p.x, s.p.y, poolLevel) && poolLevel < a_fall.upper - LEAST_DROP * 0.5f && s.p.z <= poolLevel;
				// ...or crossing any of the site's water levels under the upper one (a tier's pool the pool map has
				// no water for)
				float crossed = a_fall.lower;
				for (const float tier : a_fall.levels) {
					if (s.p.z <= tier && a_columns[k].points.back().z > tier)
						crossed = std::max(crossed, tier);
				}
				if (inPool || s.p.z <= crossed) {
					s.p.z = (inPool ? poolLevel : crossed) - POOL_TUCK;
					s.done = true;
				}
				(void)overPool;
				a_columns[k].points.push_back(s.p);
				a_columns[k].times.push_back(s.time);
				a_columns[k].onRock.push_back(s.onRock ? 1 : 0);
			}
			if (!any)
				break;
			// The headings drawn toward the neighbours' (level part only: each keeps its own fall)
			std::vector<RE::NiPoint3> flat(n);
			for (std::size_t k = 0; k < n; k++)
				flat[k] = RE::NiPoint3(states[k].velocity.x, states[k].velocity.y, 0.0f);
			for (std::size_t k = 0; k < n; k++) {
				State& s = states[k];
				if (s.done || s.onWater)
					continue;
				RE::NiPoint3 sum(0.0f, 0.0f, 0.0f);
				int count = 0;
				for (const std::size_t j : { k - 1, k + 1 }) {
					if (j < n && !states[j].done) {
						sum += flat[j];
						count++;
					}
				}
				if (!count)
					continue;
				const RE::NiPoint3 own = flat[k];
				const RE::NiPoint3 blended = own + (sum / static_cast<float>(count) - own) * NEIGHBOUR_PULL;
				s.velocity.x = blended.x;
				s.velocity.y = blended.y;
			}
		}
	}

	std::unique_ptr<FallSheets::Sheet> FallSheets::Build(const FallSites& a_sites, const Fall& a_fall)
	{
		// Every piece's vertices in the world, with its texture coordinates and colours; the first piece
		// lends the sheet its vertex layout, material and place in the scene
		RE::BSGeometry* first = a_fall.pieces.empty() ? nullptr : a_fall.pieces.front().get();
		auto* firstRenderer = first ? first->GetGeometryRuntimeData().rendererData : nullptr;
		auto* parent = first ? first->parent : nullptr;
		if (!firstRenderer || !parent) {
			logger::info("[Fluid Field]   sheet skipped: no renderer data or parent");
			return nullptr;
		}
		RE::BSGraphics::VertexDesc desc = firstRenderer->vertexDesc;
		const uint32_t stride = desc.GetSize();
		const uint32_t positionOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
		const uint32_t uvOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_TEXCOORD0);
		// Full-precision positions (four floats, the last the tangent's x) when the texture coordinates follow 16 bytes on;
		// the flag read back false on the falls' own layout
		const bool fullPrecision = uvOffset >= positionOffset + 16;
		const bool hasNormal = desc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_NORMAL);
		const bool hasTangent = desc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_TANGENT);
		const bool hasColor = desc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_COLORS);
		const uint32_t normalOffset = hasNormal ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_NORMAL) : 0;
		const uint32_t tangentOffset = hasTangent ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_BINORMAL) : 0;
		const uint32_t colorOffset = hasColor ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_COLOR) : 0;
		if (!fullPrecision || stride < positionOffset + 16 || !desc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_UV)) {
			logger::info("[Fluid Field]   sheet skipped: vertex layout (stride {}, position {}, uv {})", stride, positionOffset, uvOffset);
			return nullptr;
		}

		std::vector<RE::NiPoint3> lips;     // Each piece's top edge
		std::vector<RE::NiPoint3> heading;  // ...and the way the water leaves it, level, one per lip point
		std::vector<std::array<uint8_t, 4>> colors;
		std::vector<float> colorRun;  // 0 at a piece's top, 1 at its foot
		RE::NiPoint3 facing(0.0f, 0.0f, 0.0f), mean(0.0f, 0.0f, 0.0f);
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
			for (uint32_t i = 0; i < counts.triangleCount; i++) {
				const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(i) * 3;
				if (index[0] < counts.vertexCount && index[1] < counts.vertexCount && index[2] < counts.vertexCount)
					facing += (positions[index[1]] - positions[index[0]]).Cross(positions[index[2]] - positions[index[0]]);
			}
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
			March(world, a_sites, starts, headings, a_fall, columns);
		}
		for (uint32_t k = 0; k < GUIDE_COLUMNS; k++) {
			auto& column = columns[k];
			column.u = (sA + width * static_cast<float>(k) / static_cast<float>(GUIDE_COLUMNS - 1)) / ACROSS_REPEAT;
			if (k % 8 == 0 || k + 1 == GUIDE_COLUMNS) {
				float run = 0.0f;
				for (std::size_t i = 1; i < column.points.size(); i++)
					run += (column.points[i] - column.points[i - 1]).Length();
				logger::info("[Fluid Field]   column {}: from ({:.0f}, {:.0f}, {:.0f}), {} points, {:.0f} units, {:.2f} s, ends at z {:.0f} (water {:.0f})", k, starts[k].x, starts[k].y, starts[k].z,
					column.points.size(), run, column.times.back(), column.points.back().z, a_fall.lower);
			}
		}
		// Vanilla's tint: its pieces' average colour at their tops and their feet, blended down the sheet
		std::array<float, 4> tintTop{ 255.0f, 255.0f, 255.0f, 255.0f }, tintBottom{ 255.0f, 255.0f, 255.0f, 255.0f };
		if (!colors.empty()) {
			tintTop = {};
			tintBottom = {};
			float weightTop = 0.0f, weightBottom = 0.0f;
			for (std::size_t v = 0; v < colors.size(); v++) {
				const float wt = 1.0f - colorRun[v], wb = colorRun[v];
				for (int c = 0; c < 4; c++) {
					tintTop[c] += colors[v][c] * wt;
					tintBottom[c] += colors[v][c] * wb;
				}
				weightTop += wt;
				weightBottom += wb;
			}
			for (int c = 0; c < 4; c++) {
				tintTop[c] /= std::max(weightTop, 1e-3f);
				tintBottom[c] /= std::max(weightBottom, 1e-3f);
			}
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
		for (int pass = 0; pass < 2; pass++) {
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

		// Into the piece's own vertex layout, in its model space
		auto sheet = std::make_unique<Sheet>();
		const auto& transform = first->world;
		const RE::NiMatrix3 toModel = transform.rotate.Transpose();
		const float toModelScale = transform.scale > 1e-6f ? 1.0f / transform.scale : 1.0f;
		const auto toLocal = [&](const RE::NiPoint3& a_world) { return (toModel * (a_world - transform.translate)) * toModelScale; };
		const auto toLocalDirection = [&](const RE::NiPoint3& a_world) { return Normalized(toModel * a_world); };
		logger::info("[Fluid Field]   sheet facing ({:.2f}, {:.2f}, {:.2f})", Normalized(facing).x, Normalized(facing).y, Normalized(facing).z);
		const bool flip = [&] {
			const RE::NiPoint3 a = grid[COLUMNS] - grid[0], b = grid[1] - grid[0];
			return b.Cross(a).Dot(facing) < 0.0f;
		}();
		sheet->rawVertices.assign(static_cast<std::size_t>(stride) * grid.size(), 0);
		RE::NiPoint3 boundMin(FLT_MAX, FLT_MAX, FLT_MAX), boundMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
		for (uint32_t r = 0; r < ROWS; r++) {
			for (uint32_t c = 0; c < COLUMNS; c++) {
				const std::size_t i = static_cast<std::size_t>(r) * COLUMNS + c;
				const RE::NiPoint3 alongU = grid[static_cast<std::size_t>(r) * COLUMNS + std::min(c + 1, COLUMNS - 1)] - grid[static_cast<std::size_t>(r) * COLUMNS + (c > 0 ? c - 1 : 0)];
				const RE::NiPoint3 alongV = grid[static_cast<std::size_t>(std::min(r + 1, ROWS - 1)) * COLUMNS + c] - grid[static_cast<std::size_t>(r > 0 ? r - 1 : 0) * COLUMNS + c];
				RE::NiPoint3 normal = Normalized(alongV.Cross(alongU));
				if (flip)
					normal = -normal;
				const RE::NiPoint3 local = toLocal(grid[i]);
				boundMin = { std::min(boundMin.x, local.x), std::min(boundMin.y, local.y), std::min(boundMin.z, local.z) };
				boundMax = { std::max(boundMax.x, local.x), std::max(boundMax.y, local.y), std::max(boundMax.z, local.z) };
				const RE::NiPoint3 n = toLocalDirection(normal), tangent = toLocalDirection(alongU), bitangent = toLocalDirection(alongV);
				uint8_t* data = sheet->rawVertices.data() + i * stride;
				const float position[4] = { local.x, local.y, local.z, tangent.x };
				std::memcpy(data + positionOffset, position, sizeof(position));
				// Along the flow, the texture runs with the water's travel time at vanilla's scroll
				const uint16_t uv[2] = { DirectX::PackedVector::XMConvertFloatToHalf(us[i]), DirectX::PackedVector::XMConvertFloatToHalf(times[i] * SCROLL) };
				std::memcpy(data + uvOffset, uv, sizeof(uv));
				const auto pack = [](float a_value) { return static_cast<uint8_t>(std::clamp((a_value * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f)); };
				if (hasNormal) {
					const uint8_t packed[4] = { pack(n.x), pack(n.y), pack(n.z), pack(tangent.y) };
					std::memcpy(data + normalOffset, packed, 4);
				}
				if (hasTangent) {
					const uint8_t packed[4] = { pack(bitangent.x), pack(bitangent.y), pack(bitangent.z), pack(tangent.z) };
					std::memcpy(data + tangentOffset, packed, 4);
				}
				if (hasColor) {
					const float f = static_cast<float>(r) / static_cast<float>(ROWS - 1);
					uint8_t color[4];
					for (int k = 0; k < 4; k++)
						color[k] = static_cast<uint8_t>(std::clamp(tintTop[k] * (1.0f - f) + tintBottom[k] * f, 0.0f, 255.0f));
					color[3] = 255;
					std::memcpy(data + colorOffset, color, 4);
				}
			}
		}
		for (uint32_t r = 0; r + 1 < ROWS; r++) {
			for (uint32_t c = 0; c + 1 < COLUMNS; c++) {
				const auto a = static_cast<uint16_t>(r * COLUMNS + c), b = static_cast<uint16_t>(a + 1), d = static_cast<uint16_t>(a + COLUMNS), e = static_cast<uint16_t>(d + 1);
				// Both faces for now (the facing is not settled yet: a sheet facing away drew nothing)
				sheet->rawIndices.insert(sheet->rawIndices.end(), { a, b, d, b, e, d });
				sheet->rawIndices.insert(sheet->rawIndices.end(), { a, d, b, b, d, e });
			}
		}

		// Our buffers, as the engine's renderer data, on a clone of the piece under the same parent
		auto* device = globals::d3d::device;
		const D3D11_BUFFER_DESC vertexDesc{ .ByteWidth = static_cast<UINT>(sheet->rawVertices.size()), .Usage = D3D11_USAGE_IMMUTABLE, .BindFlags = D3D11_BIND_VERTEX_BUFFER };
		const D3D11_BUFFER_DESC indexDesc{ .ByteWidth = static_cast<UINT>(sheet->rawIndices.size() * sizeof(uint16_t)), .Usage = D3D11_USAGE_IMMUTABLE, .BindFlags = D3D11_BIND_INDEX_BUFFER };
		const D3D11_SUBRESOURCE_DATA vertexData{ sheet->rawVertices.data(), 0, 0 };
		const D3D11_SUBRESOURCE_DATA indexData{ sheet->rawIndices.data(), 0, 0 };
		if (!device || FAILED(device->CreateBuffer(&vertexDesc, &vertexData, sheet->vertexBuffer.put())) || FAILED(device->CreateBuffer(&indexDesc, &indexData, sheet->indexBuffer.put())))
			return nullptr;
		Util::SetResourceName(sheet->vertexBuffer.get(), "FluidField::FallSheet VB");
		Util::SetResourceName(sheet->indexBuffer.get(), "FluidField::FallSheet IB");
		sheet->data.vertexBuffer = reinterpret_cast<RE::ID3D11Buffer*>(sheet->vertexBuffer.get());
		sheet->data.indexBuffer = reinterpret_cast<RE::ID3D11Buffer*>(sheet->indexBuffer.get());
		sheet->data.vertexDesc = desc;
		sheet->data.refCount = 1000;  // Never released by the engine: ours, given back before the clone goes
		sheet->data.rawVertexData = sheet->rawVertices.data();
		sheet->data.rawIndexData = sheet->rawIndices.data();

		RE::NiCloningProcess cloning;
		auto* cloneObject = first->CreateClone(cloning);
		auto* clone = cloneObject ? cloneObject->AsTriShape() : nullptr;
		if (!clone)
			return nullptr;
		sheet->clone.reset(clone);
		sheet->originals = a_fall.pieces;
		sheet->parent.reset(parent);
		clone->name = SHEET_NAME;
		clone->local = first->local;
		auto& runtime = clone->GetGeometryRuntimeData();
		sheet->previousData = runtime.rendererData;
		runtime.rendererData = &sheet->data;
		auto& shapeCounts = clone->GetTrishapeRuntimeData();
		shapeCounts.vertexCount = static_cast<uint16_t>(grid.size());
		shapeCounts.triangleCount = static_cast<uint16_t>(sheet->rawIndices.size() / 3);
		const RE::NiPoint3 centre = (boundMin + boundMax) * 0.5f;
		clone->GetModelData().modelBound = RE::NiBound{ centre, (boundMax - boundMin).Length() * 0.5f };
		parent->AttachChild(clone, true);
		RE::NiUpdateData update;
		clone->Update(update);
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
		if (a_sheet.clone) {
			if (a_sheet.parent)
				a_sheet.parent->DetachChild(a_sheet.clone.get());
			a_sheet.clone->GetGeometryRuntimeData().rendererData = a_sheet.previousData;
			a_sheet.clone.reset();
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

	void FallSheets::Clear()
	{
		for (auto& sheet : sheets)
			Remove(*sheet);
		sheets.clear();
		pending.clear();
		groundVersion = UINT64_MAX;
		readyStreams = UINT32_MAX;
	}

	void FallSheets::Update(const FallSites& a_sites, bool a_enabled)
	{
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
		logger::info("[Fluid Field] Waterfall sheet ({} pieces, water {:.0f} to {:.0f}): {} in {:.1f} ms", fall.pieces.size(), fall.upper, fall.lower, sheet ? "built" : "not built", ms);
		if (sheet)
			sheets.push_back(std::move(sheet));
	}
}
