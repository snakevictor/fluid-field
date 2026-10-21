#include "MeshCut.h"

#include "EngineGrid.h"
#include "Globals.h"
#include "WaterSurfaces.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace FluidFieldSim
{
	namespace
	{
		/** @brief Segment endpoints closer than this are the same point when chaining loops. */
		constexpr float CHAIN_EPSILON = 0.25f;
		/** @brief Deepest node nesting walked when looking for collision. */
		constexpr uint32_t MAX_DEPTH = 32;

		struct Segment
		{
			float ax, ay, bx, by;
		};

		int32_t FloorDiv(int32_t a_value, int32_t a_divisor)
		{
			return a_value >= 0 ? a_value / a_divisor : -((-a_value + a_divisor - 1) / a_divisor);
		}

		/** @brief Kinds of placed objects that can stand in water: architecture, rocks, boats, docks. */
		bool IsCandidateBase(const RE::TESBoundObject* a_base)
		{
			if (!a_base)
				return false;
			switch (a_base->GetFormType()) {
			case RE::FormType::Static:
			case RE::FormType::MovableStatic:
			case RE::FormType::Activator:
			case RE::FormType::Furniture:
			case RE::FormType::Door:
			case RE::FormType::Container:
				return true;
			default:
				return false;
			}
		}

		/**
		 * @brief D176: a waterfall or rapids effect (Fluid Field draws these as water): never solid. Cut at the water's surface,
		 * the falls' sheets became a row of dry cells along every foot, holes in the pool's foam there (SS 1245, 1246).
		 */
		bool IsFallingWater(const RE::TESBoundObject* a_base)
		{
			const auto* model = a_base ? a_base->As<RE::TESModel>() : nullptr;
			const char* path = model ? model->GetModel() : nullptr;
			if (!path)
				return false;
			std::string lower(path);
			std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
			return lower.find("effects\\") != std::string::npos && (lower.find("waterfall") != std::string::npos || lower.find("waterwall") != std::string::npos || lower.find("rapids") != std::string::npos);
		}

		/** @brief Has collision somewhere in its 3D: mist, clouds and markers have none and never block water. */
		bool HasCollision(RE::NiAVObject* a_object, uint32_t a_depth = 0)
		{
			if (!a_object || a_depth > MAX_DEPTH)
				return false;
			if (a_object->collisionObject)
				return true;
			if (auto* node = a_object->AsNode()) {
				for (const auto& child : node->GetChildren()) {
					if (HasCollision(child.get(), a_depth + 1))
						return true;
				}
			}
			return false;
		}

		/** @brief Where a triangle crosses z = a_height, if it does. */
		bool CutTriangle(const RE::NiPoint3& a_p0, const RE::NiPoint3& a_p1, const RE::NiPoint3& a_p2, float a_height, Segment& a_segment)
		{
			const RE::NiPoint3* points[3] = { &a_p0, &a_p1, &a_p2 };
			float out[4];
			int found = 0;
			for (int i = 0; i < 3 && found < 2; i++) {
				const auto& p = *points[i];
				const auto& q = *points[(i + 1) % 3];
				const float dp = p.z - a_height;
				const float dq = q.z - a_height;
				if ((dp < 0.0f) != (dq < 0.0f)) {
					const float t = dp / (dp - dq);
					out[found * 2] = p.x + t * (q.x - p.x);
					out[found * 2 + 1] = p.y + t * (q.y - p.y);
					found++;
				}
			}
			if (found != 2)
				return false;
			a_segment = { out[0], out[1], out[2], out[3] };
			return true;
		}

		uint64_t EndpointKey(float a_x, float a_y)
		{
			const auto qx = static_cast<int32_t>(std::lround(a_x / CHAIN_EPSILON));
			const auto qy = static_cast<int32_t>(std::lround(a_y / CHAIN_EPSILON));
			return (static_cast<uint64_t>(static_cast<uint32_t>(qx)) << 32) | static_cast<uint32_t>(qy);
		}

		/** @brief Chains segments sharing endpoints into polylines; returns the closed ones as point lists. */
		std::vector<std::vector<std::pair<float, float>>> ClosedLoops(const std::vector<Segment>& a_segments)
		{
			std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint8_t>>> ends;
			ends.reserve(a_segments.size() * 2);
			for (uint32_t i = 0; i < a_segments.size(); i++) {
				ends[EndpointKey(a_segments[i].ax, a_segments[i].ay)].emplace_back(i, 0);
				ends[EndpointKey(a_segments[i].bx, a_segments[i].by)].emplace_back(i, 1);
			}
			std::vector<bool> used(a_segments.size(), false);
			std::vector<std::vector<std::pair<float, float>>> loops;
			for (uint32_t start = 0; start < a_segments.size(); start++) {
				if (used[start])
					continue;
				used[start] = true;
				std::vector<std::pair<float, float>> line{ { a_segments[start].ax, a_segments[start].ay }, { a_segments[start].bx, a_segments[start].by } };
				const uint64_t firstKey = EndpointKey(line.front().first, line.front().second);
				bool closed = false;
				while (true) {
					const auto it = ends.find(EndpointKey(line.back().first, line.back().second));
					if (it == ends.end())
						break;
					bool advanced = false;
					for (const auto& [index, end] : it->second) {
						if (used[index])
							continue;
						used[index] = true;
						const auto& s = a_segments[index];
						line.emplace_back(end == 0 ? s.bx : s.ax, end == 0 ? s.by : s.ay);
						advanced = true;
						break;
					}
					if (!advanced)
						break;
					if (EndpointKey(line.back().first, line.back().second) == firstKey) {
						closed = true;
						break;
					}
				}
				if (closed && line.size() >= 4)
					loops.push_back(std::move(line));
			}
			return loops;
		}
	}

	uint64_t MeshCut::MakeKey(int32_t a_x, int32_t a_y)
	{
		return (static_cast<uint64_t>(static_cast<uint32_t>(a_x)) << 32) | static_cast<uint32_t>(a_y);
	}

	std::pair<int32_t, int32_t> MeshCut::SplitKey(uint64_t a_key)
	{
		return { static_cast<int32_t>(a_key >> 32), static_cast<int32_t>(static_cast<uint32_t>(a_key)) };
	}

	void MeshCut::Clear()
	{
		cut.clear();
		queue.clear();
		queued.clear();
		solids.clear();
		dirtyBlocks.clear();
		updatesUntilScan = 0;
		cleared = true;
		stats = {};
	}

	bool MeshCut::IsSolid(int32_t a_fineX, int32_t a_fineY, float& a_top) const
	{
		const auto it = solids.find(MakeKey(a_fineX, a_fineY));
		if (it == solids.end())
			return false;
		a_top = it->second.top;
		return true;
	}

	bool MeshCut::IsSolidAt(float a_worldX, float a_worldY) const
	{
		if (fineSpacing <= 0.0f)
			return false;
		return solids.contains(MakeKey(static_cast<int32_t>(std::round(a_worldX / fineSpacing)), static_cast<int32_t>(std::round(a_worldY / fineSpacing))));
	}

	std::vector<std::pair<int32_t, int32_t>> MeshCut::TakeDirtyBlocks()
	{
		std::vector<std::pair<int32_t, int32_t>> blocks;
		blocks.reserve(dirtyBlocks.size());
		for (const uint64_t key : dirtyBlocks) {
			const auto [bx, by] = SplitKey(key);
			blocks.emplace_back(bx * BLOCK, by * BLOCK);
		}
		dirtyBlocks.clear();
		cleared = false;
		return blocks;
	}

	std::vector<std::pair<int32_t, int32_t>> MeshCut::GetAllBlocks() const
	{
		std::unordered_set<uint64_t> keys;
		for (const auto& [key, solid] : solids) {
			const auto [fx, fy] = SplitKey(key);
			keys.insert(MakeKey(FloorDiv(fx, BLOCK), FloorDiv(fy, BLOCK)));
		}
		std::vector<std::pair<int32_t, int32_t>> blocks;
		blocks.reserve(keys.size());
		for (const uint64_t key : keys) {
			const auto [bx, by] = SplitKey(key);
			blocks.emplace_back(bx * BLOCK, by * BLOCK);
		}
		return blocks;
	}

	void MeshCut::MarkBlockDirty(uint64_t a_pointKey)
	{
		const auto [fx, fy] = SplitKey(a_pointKey);
		dirtyBlocks.insert(MakeKey(FloorDiv(fx, BLOCK), FloorDiv(fy, BLOCK)));
	}

	void MeshCut::AddPoint(uint64_t a_key, float a_top)
	{
		auto& solid = solids[a_key];
		if (solid.count == 0 || a_top > solid.top)
			solid.top = a_top;
		if (solid.count++ == 0)
			MarkBlockDirty(a_key);
	}

	void MeshCut::RemoveRecord(const CutRecord& a_record)
	{
		for (const uint64_t key : a_record.points) {
			const auto it = solids.find(key);
			if (it == solids.end())
				continue;
			if (--it->second.count == 0) {
				solids.erase(it);
				MarkBlockDirty(key);
			}
		}
	}

	void MeshCut::Scan(const WaterSurfaces& a_water)
	{
		auto* tes = RE::TES::GetSingleton();
		if (!tes)
			return;

		std::unordered_set<uint32_t> present;
		const auto visitCell = [&](RE::TESObjectCELL* a_cell, bool a_requireAttached) {
			if (!a_cell || (a_requireAttached && !a_cell->IsAttached()))
				return;
			a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted() || !IsCandidateBase(a_ref->GetBaseObject()) || IsFallingWater(a_ref->GetBaseObject()))
					return RE::BSContainer::ForEachResult::kContinue;
				auto* root = a_ref->Get3D();
				if (!root)
					return RE::BSContainer::ForEachResult::kContinue;
				const auto& bound = root->worldBound;
				if (!(bound.radius > 0.0f))
					return RE::BSContainer::ForEachResult::kContinue;

				// Water at the centre, or under one of four points around it for long walls and banks
				float surface = 0.0f;
				bool found = a_water.SampleHeight(bound.center.x, bound.center.y, surface) != WaterSurfaces::Source::None;
				const float reach = bound.radius * 0.7f;
				for (int i = 0; i < 4 && !found; i++) {
					const float dx = (i == 0) ? reach : (i == 1) ? -reach :
					                                               0.0f;
					const float dy = (i == 2) ? reach : (i == 3) ? -reach :
					                                               0.0f;
					found = a_water.SampleHeight(bound.center.x + dx, bound.center.y + dy, surface) != WaterSurfaces::Source::None;
				}
				if (!found || surface < bound.center.z - bound.radius || surface > bound.center.z + bound.radius)
					return RE::BSContainer::ForEachResult::kContinue;
				if (!HasCollision(root))
					return RE::BSContainer::ForEachResult::kContinue;

				const uint32_t formID = a_ref->GetFormID();
				present.insert(formID);
				if (!cut.contains(formID) && queued.insert(formID).second)
					queue.push_back({ .handle = a_ref->GetHandle(), .formID = formID, .surface = surface });
				return RE::BSContainer::ForEachResult::kContinue;
			});
		};

		if (tes->interiorCell) {
			visitCell(tes->interiorCell, true);
		} else if (auto* grid = tes->gridCells) {
			for (uint32_t gx = 0; gx < grid->length; gx++) {
				for (uint32_t gy = 0; gy < grid->length; gy++)
					visitCell(GetGridCell(grid, gx, gy), true);
			}
			// Persistent references (the Riverwood stone bridge among them) are listed in the
			// worldspace's persistent cell, not in the grid cells; only the ones with loaded 3D pass
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (auto* worldspace = player ? player->GetWorldspace() : nullptr)
				visitCell(worldspace->persistentCell, false);
		}

		// References that unloaded (or left the water) give their points back
		for (auto it = cut.begin(); it != cut.end();) {
			if (!present.contains(it->first)) {
				RemoveRecord(it->second);
				it = cut.erase(it);
			} else {
				++it;
			}
		}
	}

	void MeshCut::Cut(RE::TESObjectREFR& a_ref, float a_surface, bool a_log)
	{
		const auto start = std::chrono::steady_clock::now();
		auto* root = a_ref.Get3D();
		std::vector<Segment> segments;
		uint32_t shapes = 0;
		uint32_t skipped = 0;
		std::vector<RE::NiPoint3> world;

		RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
			shapes++;
			auto* triShape = a_geometry->AsTriShape();
			const auto& runtime = a_geometry->GetGeometryRuntimeData();
			auto* renderer = runtime.rendererData;
			// Only lit surfaces are solid: water, effect and sky meshes use other shaders
			const auto* shader = runtime.shaderProperty.get();
			if (!shader || shader->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get()) {
				skipped++;
				return RE::BSVisit::BSVisitControl::kContinue;
			}
			if (!triShape || a_geometry->GetType().get() != RE::BSGeometry::Type::kTriShape || !renderer || !renderer->rawVertexData || !renderer->rawIndexData ||
				renderer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_SKINNED)) {
				skipped++;
				return RE::BSVisit::BSVisitControl::kContinue;
			}
			const auto& counts = triShape->GetTrishapeRuntimeData();
			const uint32_t stride = renderer->vertexDesc.GetSize();
			const uint32_t positionOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
			if (stride < positionOffset + 12 || counts.vertexCount == 0 || counts.triangleCount == 0) {
				skipped++;
				return RE::BSVisit::BSVisitControl::kContinue;
			}

			// Vertex positions are 4 floats in SE/AE whether or not VF_FULLPREC is set (live check, 2026-10-14)
			const auto& transform = a_geometry->world;
			world.resize(counts.vertexCount);
			float minZ = FLT_MAX;
			float maxZ = -FLT_MAX;
			for (uint32_t i = 0; i < counts.vertexCount; i++) {
				float local[3];
				std::memcpy(local, renderer->rawVertexData + static_cast<std::size_t>(i) * stride + positionOffset, sizeof(local));
				world[i] = transform * RE::NiPoint3(local[0], local[1], local[2]);
				minZ = std::min(minZ, world[i].z);
				maxZ = std::max(maxZ, world[i].z);
			}
			if (!(minZ < a_surface && maxZ > a_surface))
				return RE::BSVisit::BSVisitControl::kContinue;

			const uint16_t* indices = renderer->rawIndexData;
			for (uint32_t t = 0; t < counts.triangleCount; t++) {
				const uint16_t i0 = indices[t * 3];
				const uint16_t i1 = indices[t * 3 + 1];
				const uint16_t i2 = indices[t * 3 + 2];
				if (i0 >= counts.vertexCount || i1 >= counts.vertexCount || i2 >= counts.vertexCount)
					continue;
				Segment segment;
				if (CutTriangle(world[i0], world[i1], world[i2], a_surface, segment))
					segments.push_back(segment);
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});

		// Rasterise: every segment is drawn as a line (watertight on a four-neighbour grid),
		// and the inside of each closed loop is filled
		const float spacing = fineSpacing;
		const float top = a_surface + SOLID_RISE;
		std::unordered_set<uint64_t> points;
		for (const auto& s : segments) {
			const float length = std::hypot(s.bx - s.ax, s.by - s.ay);
			const int steps = std::max(1, static_cast<int>(std::ceil(length / (spacing * 0.5f))));
			for (int i = 0; i <= steps; i++) {
				const float t = static_cast<float>(i) / static_cast<float>(steps);
				points.insert(MakeKey(static_cast<int32_t>(std::round((s.ax + t * (s.bx - s.ax)) / spacing)),
					static_cast<int32_t>(std::round((s.ay + t * (s.by - s.ay)) / spacing))));
			}
		}
		const auto loops = ClosedLoops(segments);
		std::vector<float> crossings;
		for (const auto& loop : loops) {
			float minY = FLT_MAX;
			float maxY = -FLT_MAX;
			for (const auto& [x, y] : loop) {
				minY = std::min(minY, y);
				maxY = std::max(maxY, y);
			}
			for (auto row = static_cast<int32_t>(std::ceil(minY / spacing)); static_cast<float>(row) * spacing <= maxY; row++) {
				const float y = static_cast<float>(row) * spacing;
				crossings.clear();
				for (std::size_t i = 0; i + 1 < loop.size(); i++) {
					const auto& [x0, y0] = loop[i];
					const auto& [x1, y1] = loop[i + 1];
					if ((y0 <= y) != (y1 <= y))
						crossings.push_back(x0 + (y - y0) / (y1 - y0) * (x1 - x0));
				}
				std::sort(crossings.begin(), crossings.end());
				for (std::size_t i = 0; i + 1 < crossings.size(); i += 2) {
					for (auto column = static_cast<int32_t>(std::ceil(crossings[i] / spacing)); static_cast<float>(column) * spacing <= crossings[i + 1]; column++)
						points.insert(MakeKey(column, row));
				}
			}
		}

		CutRecord record;
		record.points.assign(points.begin(), points.end());
		for (const uint64_t key : record.points)
			AddPoint(key, top);
		cut[a_ref.GetFormID()] = std::move(record);

		stats.shapes += shapes;
		stats.shapesWithoutData += skipped;
		stats.segments += static_cast<uint32_t>(segments.size());
		stats.closedLoops += static_cast<uint32_t>(loops.size());

		if (a_log) {
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			const auto* base = a_ref.GetBaseObject();
			const auto* model = base ? base->As<RE::TESModel>() : nullptr;
			logger::info("[Fluid Field] Mesh cut {:08X} '{}' at {:.0f}: {} shapes ({} skipped), {} segments, {} closed loops, {} solid points, {:.3f} ms",
				a_ref.GetFormID(), model ? model->GetModel() : "", a_surface, shapes, skipped, segments.size(), loops.size(), points.size(), ms);
		}
	}

	void MeshCut::Update(const WaterSurfaces& a_water, float a_fineSpacing, bool a_log)
	{
		const auto start = std::chrono::steady_clock::now();
		if (a_fineSpacing != fineSpacing) {
			Clear();
			fineSpacing = a_fineSpacing;
		}
		if (!(fineSpacing > 0.0f))
			return;

		if (updatesUntilScan == 0) {
			Scan(a_water);
			updatesUntilScan = SCAN_INTERVAL;
		} else {
			updatesUntilScan--;
		}

		const auto deadline = start + std::chrono::duration<double, std::milli>(TIME_BUDGET_MS);
		while (!queue.empty() && std::chrono::steady_clock::now() < deadline) {
			const Pending pending = queue.front();
			queue.pop_front();
			queued.erase(pending.formID);
			const auto ref = pending.handle.get();
			if (!ref || ref->GetFormID() != pending.formID || !ref->Get3D() || cut.contains(pending.formID))
				continue;
			Cut(*ref, pending.surface, a_log);
		}

		stats.references = static_cast<uint32_t>(cut.size());
		stats.solidPoints = static_cast<uint32_t>(solids.size());
		stats.pending = static_cast<uint32_t>(queue.size());
		stats.lastUpdateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}
}
