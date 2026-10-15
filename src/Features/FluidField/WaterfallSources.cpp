#include "WaterfallSources.h"

#include "EngineGrid.h"
#include "WaterSurfaces.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

namespace FluidFieldSim
{
	namespace
	{
		/** @brief The lowest part of a body (this fraction of its height) is where it lands. */
		constexpr float PLUNGE_BAND = 0.15f;
		/** @brief The plunge band is at least this tall, world units. */
		constexpr float MIN_PLUNGE_BAND = 32.0f;
		/** @brief The water surface may lie this far below a body's lowest point and still catch it. */
		constexpr float MAX_DROP_TO_WATER = 400.0f;
		/** @brief The water surface may lie this far above a body's lowest point (the body ends underwater). */
		constexpr float MAX_DEPTH_BELOW_WATER = 400.0f;
		/** @brief Bodies landing closer than this are one fall. */
		constexpr float MERGE_DISTANCE = 400.0f;
		/** @brief Narrowest a fall's plunge counts as, world units. */
		constexpr float MIN_WIDTH = 96.0f;

		/** @brief Depth pushed in and out at the plunge, world units per second, at full strength. */
		constexpr float PULSE_DEPTH_RATE = 50.0f;
		/** @brief Speed the water spreads out from the plunge, world units per second. */
		constexpr float RADIAL_SPEED = 110.0f;
		/** @brief Speed the water runs on in the fall's direction, world units per second. */
		constexpr float OUTFLOW_SPEED = 60.0f;
		/** @brief Gravity, world units per second squared (9.81 m/s^2). */
		constexpr float GRAVITY = 687.0f;
		/** @brief Speed the water leaves a lip at, world units per second (about a river's). */
		constexpr float LIP_SPEED = 150.0f;
		/** @brief Seconds per step of the traced water, and most steps traced. */
		constexpr float TRACE_STEP = 0.03f;
		constexpr uint32_t MAX_TRACE_STEPS = 200;
		/** @brief Per-second slowing of water sliding on rock, and the slowest it slides. */
		constexpr float SLIDE_FRICTION = 0.6f;
		constexpr float MIN_SLIDE_SPEED = 80.0f;
		/** @brief Rock rising more than this above the water ahead is a wall: water does not climb it. */
		constexpr float MAX_CLIMB = 8.0f;
		/** @brief Water lying on rock sits this far above it, world units. */
		constexpr float ROCK_OFFSET = 6.0f;
		/** @brief Bed rays start this far above the arc and reach this far below it, world units. */
		constexpr float RAY_ABOVE = 12.0f;
		constexpr float RAY_BELOW = 2048.0f;

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

		/** @brief The highest solid surface below a point, by a downward Havok ray; false if none in reach. */
		bool CastDown(RE::hkpWorld* a_world, float a_x, float a_y, float a_fromZ, float a_toZ, float& a_hitZ)
		{
			const float scale = RE::bhkWorld::GetWorldScale();
			float fromZ = a_fromZ;
			for (int attempt = 0; attempt < 4 && fromZ > a_toZ; attempt++) {
				RE::hkpWorldRayCastInput input;
				input.from = RE::hkVector4(a_x * scale, a_y * scale, fromZ * scale, 0.0f);
				input.to = RE::hkVector4(a_x * scale, a_y * scale, a_toZ * scale, 0.0f);
				RE::hkpWorldRayCastOutput output;
				a_world->CastRay(input, output);
				if (!output.HasHit())
					return false;
				const float hitZ = fromZ + (a_toZ - fromZ) * output.hitFraction;
				if (output.rootCollidable && IsSolidLayer(output.rootCollidable->GetCollisionLayer())) {
					a_hitZ = hitZ;
					return true;
				}
				fromZ = hitZ - 1.0f;  // Through anything that is not rock or ground
			}
			return false;
		}

		/** @brief Foam made per second at the plunge. */
		constexpr float FOAM_RATE = 0.6f;

		std::string Lowercase(const char* a_text)
		{
			std::string result(a_text ? a_text : "");
			std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return result;
		}

		bool IsWaterfallBody(const char* a_model)
		{
			const std::string model = Lowercase(a_model);
			return model.find("effects\\") != std::string::npos && (model.find("waterfallbody") != std::string::npos || model.find("waterfallthin") != std::string::npos);
		}

		const char* ModelOf(const RE::TESBoundObject* a_base)
		{
			const auto* model = a_base ? a_base->As<RE::TESModel>() : nullptr;
			return model ? model->GetModel() : nullptr;
		}

		/** @brief A cheap smooth pulse in [-1, 1], different for each fall. */
		float Pulse(float a_time, float a_seed)
		{
			return 0.6f * std::sin(a_time * 2.3f + a_seed) + 0.4f * std::sin(a_time * 5.1f + a_seed * 1.7f);
		}
	}

	void WaterfallSources::Clear()
	{
		bodies.clear();
		falls.clear();
		updatesUntilScan = 0;
	}

	bool WaterfallSources::ReadBody(RE::TESObjectREFR& a_ref, Body& a_body)
	{
		auto* root = a_ref.Get3D();
		if (!root)
			return false;

		// The body's vertices in world space, where the meshes keep them on the CPU
		std::vector<RE::NiPoint3> points;
		RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
			auto* triShape = a_geometry->AsTriShape();
			auto* renderer = a_geometry->GetGeometryRuntimeData().rendererData;
			if (!triShape || !renderer || !renderer->rawVertexData)
				return RE::BSVisit::BSVisitControl::kContinue;
			const auto& counts = triShape->GetTrishapeRuntimeData();
			const uint32_t stride = renderer->vertexDesc.GetSize();
			const uint32_t positionOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
			if (stride < positionOffset + 12)
				return RE::BSVisit::BSVisitControl::kContinue;
			const auto& transform = a_geometry->world;
			for (uint32_t i = 0; i < counts.vertexCount; i++) {
				float local[3];
				std::memcpy(local, renderer->rawVertexData + static_cast<std::size_t>(i) * stride + positionOffset, sizeof(local));
				points.push_back(transform * RE::NiPoint3(local[0], local[1], local[2]));
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});

		// Without vertex data, the corners of the reference's bounds
		if (points.size() < 4) {
			points.clear();
			const auto boundMin = a_ref.GetBoundMin();
			const auto boundMax = a_ref.GetBoundMax();
			const auto& transform = root->world;
			for (int corner = 0; corner < 8; corner++) {
				const RE::NiPoint3 local((corner & 1) ? boundMax.x : boundMin.x, (corner & 2) ? boundMax.y : boundMin.y, (corner & 4) ? boundMax.z : boundMin.z);
				points.push_back(transform * local);
			}
		}

		float minZ = FLT_MAX;
		float maxZ = -FLT_MAX;
		for (const auto& p : points) {
			minZ = std::min(minZ, p.z);
			maxZ = std::max(maxZ, p.z);
		}
		const float band = std::max((maxZ - minZ) * PLUNGE_BAND, MIN_PLUNGE_BAND);

		// The plunge is the centre of the lowest band; the fall runs from the top band toward it
		RE::NiPoint3 low{}, high{};
		uint32_t lowCount = 0, highCount = 0;
		float lowMin[2] = { FLT_MAX, FLT_MAX }, lowMax[2] = { -FLT_MAX, -FLT_MAX };
		for (const auto& p : points) {
			if (p.z <= minZ + band) {
				low += p;
				lowCount++;
				lowMin[0] = std::min(lowMin[0], p.x);
				lowMin[1] = std::min(lowMin[1], p.y);
				lowMax[0] = std::max(lowMax[0], p.x);
				lowMax[1] = std::max(lowMax[1], p.y);
			}
			if (p.z >= maxZ - band) {
				high += p;
				highCount++;
			}
		}
		if (lowCount == 0 || highCount == 0)
			return false;
		low /= static_cast<float>(lowCount);
		high /= static_cast<float>(highCount);

		// The lip's width: the top band's spread across the fall's direction
		{
			float fx = low.x - high.x;
			float fy = low.y - high.y;
			const float fl = std::sqrt(fx * fx + fy * fy);
			fx = fl > 1.0f ? fx / fl : 1.0f;
			fy = fl > 1.0f ? fy / fl : 0.0f;
			float across[2] = { FLT_MAX, -FLT_MAX };
			for (const auto& p : points) {
				if (p.z < maxZ - band)
					continue;
				const float side = (p.x - high.x) * -fy + (p.y - high.y) * fx;
				across[0] = std::min(across[0], side);
				across[1] = std::max(across[1], side);
			}
			a_body.lip[0] = high.x + -fy * 0.5f * (across[0] + across[1]);
			a_body.lip[1] = high.y + fx * 0.5f * (across[0] + across[1]);
			a_body.lip[2] = high.z;
			a_body.lipWidth = std::max(across[1] - across[0], MIN_WIDTH);
		}

		a_body.plunge[0] = low.x;
		a_body.plunge[1] = low.y;
		a_body.plunge[2] = low.z;
		a_body.lowest = minZ;
		a_body.width = std::max(std::max(lowMax[0] - lowMin[0], lowMax[1] - lowMin[1]), MIN_WIDTH);
		const float dx = low.x - high.x;
		const float dy = low.y - high.y;
		const float length = std::sqrt(dx * dx + dy * dy);
		a_body.direction[0] = length > 1.0f ? dx / length : 0.0f;
		a_body.direction[1] = length > 1.0f ? dy / length : 0.0f;
		return true;
	}

	void WaterfallSources::BuildCurtain(Body& a_body)
	{
		a_body.curtain.clear();
		const float drop = a_body.lip[2] - a_body.plunge[2];
		if (drop < 32.0f)
			return;
		auto tes = RE::TES::GetSingleton();
		auto* cell = tes ? (tes->interiorCell ? tes->interiorCell : tes->GetCell(RE::NiPoint3(a_body.plunge[0], a_body.plunge[1], a_body.plunge[2]))) : nullptr;
		auto* bhkWorld = cell ? cell->GetbhkWorld() : nullptr;
		auto* world = bhkWorld ? bhkWorld->GetWorld1() : nullptr;
		if (!world)
			return;
		const RE::BSReadLockGuard lock(bhkWorld->worldLock);

		const float dx = a_body.plunge[0] - a_body.lip[0];
		const float dy = a_body.plunge[1] - a_body.lip[1];
		const float reach = std::sqrt(dx * dx + dy * dy);
		const float dir[2] = { reach > 1.0f ? dx / reach : a_body.direction[0], reach > 1.0f ? dy / reach : a_body.direction[1] };
		const float side[2] = { -dir[1], dir[0] };
		// Where the water ends: the plunge (the water surface or the bottom of vanilla's body)
		const float endZ = a_body.plunge[2];
		const float maxTravel = reach * 1.5f + drop;

		struct PathPoint
		{
			float x, y, z, speed, onRock, time;
		};
		std::vector<PathPoint> path;
		a_body.curtain.resize(static_cast<std::size_t>(CURTAIN_COLUMNS) * CURTAIN_ROWS);
		for (uint32_t c = 0; c < CURTAIN_COLUMNS; c++) {
			const float across = static_cast<float>(c) / static_cast<float>(CURTAIN_COLUMNS - 1) * 2.0f - 1.0f;
			// Each column is traced as water: it leaves the lip at about river speed and falls; on rock
			// it lies on the surface and slides, sped up by the slope and slowed a little by friction;
			// where the rock drops away it falls free again. Ends where it reaches the water below.
			float x = a_body.lip[0] + side[0] * across * 0.5f * a_body.lipWidth;
			float y = a_body.lip[1] + side[1] * across * 0.5f * a_body.lipWidth;
			float z = a_body.lip[2];
			float horizontal = LIP_SPEED;
			float vz = 0.0f;
			float travelled = 0.0f;
			float bed = z - RAY_BELOW;
			bool haveBed = CastDown(world, x, y, z + RAY_ABOVE, z - RAY_BELOW, bed);
			path.clear();
			float time = 0.0f;
			path.push_back({ x, y, z, horizontal, 0.0f, 0.0f });
			for (uint32_t step = 0; step < MAX_TRACE_STEPS && z > endZ && travelled < maxTravel; step++) {
				const float previousBed = bed;
				const bool hadBed = haveBed;
				const float nextX = x + dir[0] * horizontal * TRACE_STEP;
				const float nextY = y + dir[1] * horizontal * TRACE_STEP;
				float nextBed = 0.0f;
				const bool nextHasBed = CastDown(world, nextX, nextY, z + RAY_ABOVE, z - RAY_BELOW, nextBed);
				// Water does not climb: rock rising ahead is a wall, and the column ends against it
				if (nextHasBed && nextBed > z + MAX_CLIMB)
					break;
				x = nextX;
				y = nextY;
				bed = nextBed;
				haveBed = nextHasBed;
				travelled += horizontal * TRACE_STEP;
				time += TRACE_STEP;
				vz -= GRAVITY * TRACE_STEP;
				// Never upward
				float nextZ = std::min(z + vz * TRACE_STEP, z);
				float onRock = 0.0f;
				if (haveBed && nextZ < bed + ROCK_OFFSET) {
					// On the rock: follow it, sliding faster where it is steep
					nextZ = std::min(bed + ROCK_OFFSET, z);
					const float run = std::max(horizontal * TRACE_STEP, 1.0f);
					const float fall = hadBed ? std::max(previousBed - bed, 0.0f) : 0.0f;
					const float slope = fall / run;
					const float sine = slope / std::sqrt(1.0f + slope * slope);
					horizontal = std::max(horizontal + (GRAVITY * sine - SLIDE_FRICTION * horizontal) * TRACE_STEP, MIN_SLIDE_SPEED);
					vz = -horizontal * slope;
					onRock = 1.0f;
				}
				z = nextZ;
				path.push_back({ x, y, z, std::sqrt(horizontal * horizontal + vz * vz), onRock, time });
			}

			// Resample the path into evenly spaced rows by distance along it
			std::vector<float> distance(path.size(), 0.0f);
			for (std::size_t i = 1; i < path.size(); i++) {
				const float ex = path[i].x - path[i - 1].x;
				const float ey = path[i].y - path[i - 1].y;
				const float ez = path[i].z - path[i - 1].z;
				distance[i] = distance[i - 1] + std::sqrt(ex * ex + ey * ey + ez * ez);
			}
			const float total = std::max(distance.back(), 1.0f);
			std::size_t segment = 1;
			for (uint32_t r = 0; r < CURTAIN_ROWS; r++) {
				const float wanted = total * static_cast<float>(r) / static_cast<float>(CURTAIN_ROWS - 1);
				while (segment + 1 < path.size() && distance[segment] < wanted)
					segment++;
				const auto& a = path[std::min(segment - 1, path.size() - 1)];
				const auto& b = path[std::min(segment, path.size() - 1)];
				const float span = distance[std::min(segment, path.size() - 1)] - distance[segment - 1];
				const float t = span > 1e-3f ? std::clamp((wanted - distance[segment - 1]) / span, 0.0f, 1.0f) : 0.0f;
				auto& point = a_body.curtain[static_cast<std::size_t>(r) * CURTAIN_COLUMNS + c];
				point.position[0] = a.x + (b.x - a.x) * t;
				point.position[1] = a.y + (b.y - a.y) * t;
				point.position[2] = a.z + (b.z - a.z) * t;
				point.along = static_cast<float>(r) / static_cast<float>(CURTAIN_ROWS - 1);
				point.speed = a.speed + (b.speed - a.speed) * t;
				point.across = across;
				point.onRock = a.onRock + (b.onRock - a.onRock) * t;
				point.travelTime = a.time + (b.time - a.time) * t;
			}
		}
	}

	void WaterfallSources::RestoreBodies()
	{
		bool changed = false;
		for (auto& [formID, body] : bodies) {
			if (!body.hidden)
				continue;
			if (const auto ref = body.handle.get(); ref && ref->Get3D())
				ref->Get3D()->SetAppCulled(false);
			body.hidden = false;
			changed = true;
		}
		if (!curtainPoints.empty() || changed) {
			curtainPoints.clear();
			curtainVersion++;
		}
	}

	void WaterfallSources::Update(const WaterSurfaces& a_water, float a_centreX, float a_centreY, float a_halfSize, bool a_replace)
	{
		if (updatesUntilScan > 0) {
			updatesUntilScan--;
			return;
		}
		updatesUntilScan = SCAN_INTERVAL;

		auto* tes = RE::TES::GetSingleton();
		if (!tes)
			return;

		struct Landing
		{
			Fall fall;
			float distance;
		};
		std::vector<Landing> landings;
		std::vector<uint32_t> seenBodies;
		const auto visitCell = [&](RE::TESObjectCELL* a_cell, bool a_requireAttached) {
			if (!a_cell || (a_requireAttached && !a_cell->IsAttached()))
				return;
			a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted() || !a_ref->Get3D())
					return RE::BSContainer::ForEachResult::kContinue;
				if (!IsWaterfallBody(ModelOf(a_ref->GetBaseObject())))
					return RE::BSContainer::ForEachResult::kContinue;
				const auto position = a_ref->GetPosition();
				if (std::abs(position.x - a_centreX) > a_halfSize + 2048.0f || std::abs(position.y - a_centreY) > a_halfSize + 2048.0f)
					return RE::BSContainer::ForEachResult::kContinue;

				auto [it, added] = bodies.try_emplace(a_ref->GetFormID());
				if (added && !ReadBody(*a_ref, it->second)) {
					bodies.erase(it);
					return RE::BSContainer::ForEachResult::kContinue;
				}
				Body& body = it->second;
				body.handle = a_ref->GetHandle();
				seenBodies.push_back(a_ref->GetFormID());
				if (std::abs(body.plunge[0] - a_centreX) > a_halfSize || std::abs(body.plunge[1] - a_centreY) > a_halfSize)
					return RE::BSContainer::ForEachResult::kContinue;

				// It must land on water: the surface close below its lowest point, or above it
				float surface = 0.0f;
				if (a_water.SampleHeight(body.plunge[0], body.plunge[1], surface) == WaterSurfaces::Source::None ||
					surface < body.lowest - MAX_DROP_TO_WATER || surface > body.lowest + MAX_DEPTH_BELOW_WATER)
					return RE::BSContainer::ForEachResult::kContinue;

				Landing landing{};
				landing.fall.plunge[0] = body.plunge[0];
				landing.fall.plunge[1] = body.plunge[1];
				landing.fall.plunge[2] = surface;
				landing.fall.width = body.width;
				landing.fall.direction[0] = body.direction[0];
				landing.fall.direction[1] = body.direction[1];
				const float dx = body.plunge[0] - a_centreX;
				const float dy = body.plunge[1] - a_centreY;
				landing.distance = std::sqrt(dx * dx + dy * dy);
				landings.push_back(landing);
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
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (auto* worldspace = player ? player->GetWorldspace() : nullptr)
				visitCell(worldspace->persistentCell, false);
		}

		// Bodies landing close together (a wide fall is several meshes) are one fall
		std::sort(landings.begin(), landings.end(), [](const Landing& a, const Landing& b) { return a.distance < b.distance; });
		falls.clear();
		std::vector<float> weights;
		for (const auto& landing : landings) {
			bool merged = false;
			for (std::size_t i = 0; i < falls.size(); i++) {
				auto& fall = falls[i];
				const float dx = landing.fall.plunge[0] - fall.plunge[0];
				const float dy = landing.fall.plunge[1] - fall.plunge[1];
				const float distance = std::sqrt(dx * dx + dy * dy);
				if (distance > MERGE_DISTANCE)
					continue;
				// Weighted by width; the merged fall spans both
				const float w = weights[i];
				const float lw = landing.fall.width;
				for (int k = 0; k < 2; k++) {
					fall.plunge[k] = (fall.plunge[k] * w + landing.fall.plunge[k] * lw) / (w + lw);
					fall.direction[k] = fall.direction[k] * w + landing.fall.direction[k] * lw;
				}
				const float length = std::sqrt(fall.direction[0] * fall.direction[0] + fall.direction[1] * fall.direction[1]);
				if (length > 1e-3f) {
					fall.direction[0] /= length;
					fall.direction[1] /= length;
				}
				fall.width = std::max(fall.width, distance + 0.5f * (fall.width + lw));
				weights[i] = w + lw;
				merged = true;
				break;
			}
			if (!merged && falls.size() < MAX_FALLS) {
				falls.push_back(landing.fall);
				weights.push_back(landing.fall.width);
			}
		}

		// Forget bodies that unloaded
		for (auto it = bodies.begin(); it != bodies.end();) {
			auto* form = RE::TESForm::LookupByID(it->first);
			auto* ref = form ? form->AsReference() : nullptr;
			if (!ref || !ref->Get3D())
				it = bodies.erase(it);
			else
				++it;
		}

		// Replace the bodies in the area with curtains (D23): build each once, hide vanilla's
		if (!a_replace) {
			RestoreBodies();
			return;
		}
		std::vector<CurtainPoint> points;
		for (const uint32_t formID : seenBodies) {
			auto it = bodies.find(formID);
			if (it == bodies.end())
				continue;
			Body& body = it->second;
			if (body.curtain.empty())
				BuildCurtain(body);
			if (body.curtain.empty())
				continue;
			points.insert(points.end(), body.curtain.begin(), body.curtain.end());
			if (const auto ref = body.handle.get(); ref && ref->Get3D()) {
				// Hidden again after every reload of its 3D
				ref->Get3D()->SetAppCulled(true);
				body.hidden = true;
			}
		}
		if (points.size() != curtainPoints.size() || std::memcmp(points.data(), curtainPoints.data(), points.size() * sizeof(CurtainPoint)) != 0) {
			curtainPoints.swap(points);
			curtainVersion++;
		}
	}

	void WaterfallSources::AppendEmitters(std::vector<WaterEmitter>& a_emitters, const int32_t a_gridMin[2], float a_cellSize, uint32_t a_gridRes, float a_time,
		float a_strength, uint32_t a_maxEmitters) const
	{
		if (a_strength <= 0.0f || !(a_cellSize > 0.0f))
			return;
		for (std::size_t i = 0; i < falls.size() && a_emitters.size() < a_maxEmitters; i++) {
			const auto& fall = falls[i];
			const float texel[2] = { fall.plunge[0] / a_cellSize - static_cast<float>(a_gridMin[0]), fall.plunge[1] / a_cellSize - static_cast<float>(a_gridMin[1]) };
			if (texel[0] < 0.0f || texel[1] < 0.0f || texel[0] >= static_cast<float>(a_gridRes) || texel[1] >= static_cast<float>(a_gridRes))
				continue;
			// A heaving, churning plunge: water pushed in and out, spreading out and running downstream
			const float pulse = Pulse(a_time, static_cast<float>(i) * 2.17f + fall.plunge[0] * 0.001f);
			a_emitters.push_back({ .position = { texel[0], texel[1] },
				.radius = std::max(0.5f * fall.width / a_cellSize, 1.5f),
				.depthRate = PULSE_DEPTH_RATE * pulse * a_strength,
				.velocity = { fall.direction[0] * OUTFLOW_SPEED * a_strength, fall.direction[1] * OUTFLOW_SPEED * a_strength },
				.pad1 = { RADIAL_SPEED * a_strength, FOAM_RATE * a_strength } });
		}
	}
}
