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

		/** @brief Speed the water spreads out from the plunge, world units per second. */
		constexpr float RADIAL_SPEED = 110.0f;
		/** @brief Speed the water runs on in the fall's direction, world units per second. */
		constexpr float OUTFLOW_SPEED = 60.0f;

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

	void WaterfallSources::Update(const WaterSurfaces& a_water, float a_centreX, float a_centreY, float a_halfSize)
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
				const Body& body = it->second;
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
	}

	void WaterfallSources::AppendEmitters(std::vector<WaterEmitter>& a_emitters, const int32_t a_gridMin[2], float a_cellSize, uint32_t a_gridRes,
		float a_strength, uint32_t a_maxEmitters, bool a_foam) const
	{
		if (a_strength <= 0.0f || !(a_cellSize > 0.0f))
			return;

		for (std::size_t i = 0; i < falls.size() && a_emitters.size() < a_maxEmitters; i++) {
			const auto& fall = falls[i];
			const float texel[2] = { fall.plunge[0] / a_cellSize - static_cast<float>(a_gridMin[0]), fall.plunge[1] / a_cellSize - static_cast<float>(a_gridMin[1]) };
			if (texel[0] < 0.0f || texel[1] < 0.0f || texel[0] >= static_cast<float>(a_gridRes) || texel[1] >= static_cast<float>(a_gridRes))
				continue;
			// A churning plunge: water spreading out and running downstream
			a_emitters.push_back({ .position = { texel[0], texel[1] },
				.radius = std::max(0.5f * fall.width / a_cellSize, 1.5f),
				.depthRate = 0.0f,  // D55: the falls drive the water's flow and foam, never dent its surface
				.velocity = { fall.direction[0] * OUTFLOW_SPEED * a_strength, fall.direction[1] * OUTFLOW_SPEED * a_strength },
				.pad1 = { RADIAL_SPEED * a_strength, a_foam ? FOAM_RATE * a_strength : 0.0f } });
		}
	}
}
