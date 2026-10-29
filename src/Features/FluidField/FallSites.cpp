#include "FallSites.h"

#include "FallStrands.h"

#include "EngineGrid.h"
#include "FallSurfaceRenderer.h"

#include "WaterSurfaces.h"
#include <DirectXPackedVector.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <future>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_set>

namespace FluidFieldSim
{
	namespace
	{
		// D185: inline vector maths; CommonLib's NiPoint3 operators are library calls the optimiser cannot inline (~17 s of a cold load).
		struct V3
		{
			float x, y, z;
			V3(float a_x, float a_y, float a_z) :
				x(a_x), y(a_y), z(a_z) {}
			V3(const RE::NiPoint3& a_p) :
				x(a_p.x), y(a_p.y), z(a_p.z) {}
			V3 operator-(const V3& a_o) const { return { x - a_o.x, y - a_o.y, z - a_o.z }; }
			V3 operator*(float a_s) const { return { x * a_s, y * a_s, z * a_s }; }
			float Dot(const V3& a_o) const { return x * a_o.x + y * a_o.y + z * a_o.z; }
			V3 Cross(const V3& a_o) const { return { y * a_o.z - z * a_o.y, z * a_o.x - x * a_o.z, x * a_o.y - y * a_o.x }; }
		};

		/** @brief D67: squared distance from a point to a triangle (Ericson, Real-Time Collision Detection 5.1.5). */
		float TriangleDistanceSquared(const RE::NiPoint3& a_point, const RE::NiPoint3& a_ta, const RE::NiPoint3& a_tb, const RE::NiPoint3& a_tc)
		{
			const V3 a_p(a_point), a_a(a_ta), a_b(a_tb), a_c(a_tc);
			const V3 ab = a_b - a_a, ac = a_c - a_a, ap = a_p - a_a;
			const auto squared = [](const V3& a_v) { return a_v.Dot(a_v); };
			const float d1 = ab.Dot(ap), d2 = ac.Dot(ap);
			if (d1 <= 0.0f && d2 <= 0.0f)
				return squared(ap);
			const V3 bp = a_p - a_b;
			const float d3 = ab.Dot(bp), d4 = ac.Dot(bp);
			if (d3 >= 0.0f && d4 <= d3)
				return squared(bp);
			const float vc = d1 * d4 - d3 * d2;
			if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
				return squared(ap - ab * (d1 / (d1 - d3)));
			const V3 cp = a_p - a_c;
			const float d5 = ab.Dot(cp), d6 = ac.Dot(cp);
			if (d6 >= 0.0f && d5 <= d6)
				return squared(cp);
			const float vb = d5 * d2 - d1 * d6;
			if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
				return squared(ap - ac * (d2 / (d2 - d6)));
			const float va = d3 * d6 - d5 * d4;
			if (va <= 0.0f && d4 - d3 >= 0.0f && d5 - d6 >= 0.0f)
				return squared(bp - (a_c - a_b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6))));
			const float denominator = 1.0f / (va + vb + vc);
			return squared(ap - ab * (vb * denominator) - ac * (vc * denominator));
		}

		/**
		 * @brief D103: the water's travel from its highest tenth to its lowest, level and unit, then tipped up (z 0.3).
		 * OrientSheet picks a sheet's outer side by it. a_positions in world space.
		 */
		RE::NiPoint3 SheetTravel(const std::vector<RE::NiPoint3>& a_positions)
		{
			RE::NiPoint3 travel(0.0f, 0.0f, 0.0f);
			if (a_positions.empty())
				return RE::NiPoint3(0.0f, 0.0f, 0.3f);
			std::vector<float> sorted;
			sorted.reserve(a_positions.size());
			for (const auto& p : a_positions)
				sorted.push_back(p.z);
			std::sort(sorted.begin(), sorted.end());
			const float high = sorted[sorted.size() * 9 / 10], low = sorted[sorted.size() / 10];
			RE::NiPoint3 head(0.0f, 0.0f, 0.0f), foot(0.0f, 0.0f, 0.0f);
			float heads = 0.0f, feet = 0.0f;
			for (const auto& p : a_positions) {
				if (p.z >= high) {
					head += p;
					heads += 1.0f;
				} else if (p.z <= low) {
					foot += p;
					feet += 1.0f;
				}
			}
			if (heads > 0.0f && feet > 0.0f) {
				travel = foot / feet - head / heads;
				travel.z = 0.0f;
				const float length = travel.Length();
				travel = length > 1e-3f ? travel / length : RE::NiPoint3(0.0f, 0.0f, 0.0f);
			}
			travel.z = 0.3f;
			return travel;
		}

		/** @brief D109: a geometry's authored per-vertex normals in world space (kept by the dense copies); empty if none. */
		std::vector<RE::NiPoint3> AuthoredNormals(RE::BSGeometry* a_geometry)
		{
			auto* triShape = a_geometry ? a_geometry->AsTriShape() : nullptr;
			auto* renderer = a_geometry ? a_geometry->GetGeometryRuntimeData().rendererData : nullptr;
			if (!triShape || !renderer || !renderer->rawVertexData || !renderer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_NORMAL))
				return {};
			auto& desc = renderer->vertexDesc;
			const uint32_t stride = desc.GetSize();
			const uint32_t offset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_NORMAL);
			std::vector<RE::NiPoint3> normals(triShape->GetTrishapeRuntimeData().vertexCount);
			for (uint32_t v = 0; v < normals.size(); v++) {
				const uint8_t* n = renderer->rawVertexData + static_cast<std::size_t>(v) * stride + offset;
				normals[v] = a_geometry->world.rotate * RE::NiPoint3(n[0] / 255.0f * 2.0f - 1.0f, n[1] / 255.0f * 2.0f - 1.0f, n[2] / 255.0f * 2.0f - 1.0f);
			}
			return normals;
		}

		/**
		 * @brief D103: each triangle's outer side, +1 (its winding) or -1, consistent over each connected sheet.
		 * Carried across shared edges (vertices welded by position, so texture seams join); D92's per-vertex test turned the
		 * up-facing lead-in of C-shaped sheets inward. Each sheet takes the side its lowest third faces a_travel with.
		 */
		std::vector<int8_t> OrientSheet(const std::vector<RE::NiPoint3>& a_positions, const std::vector<std::array<uint16_t, 3>>& a_triangles, const RE::NiPoint3& a_travel,
			const std::vector<RE::NiPoint3>* a_authored = nullptr)
		{
			std::vector<int8_t> side(a_triangles.size(), 0);
			std::map<std::tuple<int64_t, int64_t, int64_t>, uint32_t> welds;
			std::vector<uint32_t> weld(a_positions.size());
			for (std::size_t v = 0; v < a_positions.size(); v++) {
				const auto key = std::make_tuple(std::llround(a_positions[v].x * 8.0), std::llround(a_positions[v].y * 8.0), std::llround(a_positions[v].z * 8.0));
				weld[v] = welds.emplace(key, static_cast<uint32_t>(welds.size())).first->second;
			}
			// Each welded edge's triangles, and its direction along it (+1: from the lower weld index)
			std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, int8_t>>> edges;
			std::vector<RE::NiPoint3> face(a_triangles.size());
			float lowest = FLT_MAX, highest = -FLT_MAX;
			for (uint32_t t = 0; t < a_triangles.size(); t++) {
				const auto& tri = a_triangles[t];
				face[t] = (a_positions[tri[1]] - a_positions[tri[0]]).Cross(a_positions[tri[2]] - a_positions[tri[0]]);
				for (int k = 0; k < 3; k++) {
					lowest = std::min(lowest, a_positions[tri[k]].z);
					highest = std::max(highest, a_positions[tri[k]].z);
					const uint32_t a = weld[tri[k]], b = weld[tri[(k + 1) % 3]];
					if (a == b)
						continue;
					edges[static_cast<uint64_t>(std::min(a, b)) << 32 | std::max(a, b)].emplace_back(t, static_cast<int8_t>(a < b ? 1 : -1));
				}
			}
			std::vector<std::vector<std::pair<uint32_t, bool>>> next(a_triangles.size());  // neighbour, and whether it keeps the side
			for (const auto& [key, uses] : edges) {
				if (uses.size() != 2 || uses[0].first == uses[1].first)
					continue;                                         // Open edge, or more than two sheets meet: not carried across
				const bool keeps = uses[0].second != uses[1].second;  // Consistent windings run a shared edge opposite ways
				next[uses[0].first].emplace_back(uses[1].first, keeps);
				next[uses[1].first].emplace_back(uses[0].first, keeps);
			}
			const float footLine = lowest + (highest - lowest) / 3.0f;
			std::vector<uint32_t> component, queue;
			for (uint32_t seed = 0; seed < a_triangles.size(); seed++) {
				if (side[seed] != 0)
					continue;
				component.clear();
				queue.assign(1, seed);
				side[seed] = 1;
				while (!queue.empty()) {
					const uint32_t t = queue.back();
					queue.pop_back();
					component.push_back(t);
					for (const auto& [n, keeps] : next[t]) {
						if (side[n] == 0) {
							side[n] = static_cast<int8_t>(keeps ? side[t] : -side[t]);
							queue.push_back(n);
						}
					}
				}
				// D109: authored normals decide by majority (about three in four face out of the visible side)
				if (a_authored && a_authored->size() == a_positions.size()) {
					double vote = 0.0;
					for (const uint32_t t : component) {
						const auto& tri = a_triangles[t];
						const RE::NiPoint3 authored = (*a_authored)[tri[0]] + (*a_authored)[tri[1]] + (*a_authored)[tri[2]];
						vote += static_cast<double>(face[t].Dot(authored)) * side[t];
					}
					if (vote < 0.0) {
						for (const uint32_t t : component)
							side[t] = static_cast<int8_t>(-side[t]);
					}
					continue;
				}
				float atFoot = 0.0f, all = 0.0f;
				for (const uint32_t t : component) {
					const float facing = face[t].Dot(a_travel) * side[t];
					all += facing;
					const auto& tri = a_triangles[t];
					if ((a_positions[tri[0]].z + a_positions[tri[1]].z + a_positions[tri[2]].z) / 3.0f <= footLine)
						atFoot += facing;
				}
				if ((atFoot != 0.0f ? atFoot : all) < 0.0f) {
					for (const uint32_t t : component)
						side[t] = static_cast<int8_t>(-side[t]);
				}
			}
			return side;
		}

		/** @brief D103: distance along a ray (a_rayDirection unit) to a triangle, up to a_reach, else negative (Moller-Trumbore). */
		float RayTriangle(const RE::NiPoint3& a_rayFrom, const RE::NiPoint3& a_rayDirection, const RE::NiPoint3& a_ta, const RE::NiPoint3& a_tb, const RE::NiPoint3& a_tc, float a_reach)
		{
			const V3 a_from(a_rayFrom), a_direction(a_rayDirection), a_a(a_ta), a_b(a_tb), a_c(a_tc);
			const V3 e1 = a_b - a_a, e2 = a_c - a_a;
			const V3 p = a_direction.Cross(e2);
			const float det = e1.Dot(p);
			if (std::abs(det) < 1e-8f)
				return -1.0f;
			const float inverse = 1.0f / det;
			const V3 s = a_from - a_a;
			const float u = s.Dot(p) * inverse;
			if (u < 0.0f || u > 1.0f)
				return -1.0f;
			const V3 q = s.Cross(e1);
			const float v = a_direction.Dot(q) * inverse;
			if (v < 0.0f || u + v > 1.0f)
				return -1.0f;
			const float t = e2.Dot(q) * inverse;
			return t >= 0.0f && t <= a_reach ? t : -1.0f;
		}

		/** @brief A grid softened in place: a_passes of a 3x3 tent. */
		void BlurGrid(std::vector<float>& a_field, int32_t a_width, int32_t a_height, int a_passes)
		{
			std::vector<float> scratch(a_field.size());
			for (int pass = 0; pass < a_passes; pass++) {
				for (int32_t y = 0; y < a_height; y++) {
					for (int32_t x = 0; x < a_width; x++) {
						float total = 0.0f, weight = 0.0f;
						for (int32_t dy = -1; dy <= 1; dy++) {
							for (int32_t dx = -1; dx <= 1; dx++) {
								if (x + dx < 0 || y + dy < 0 || x + dx >= a_width || y + dy >= a_height)
									continue;
								const float k = (dx == 0 ? 2.0f : 1.0f) * (dy == 0 ? 2.0f : 1.0f);
								total += a_field[static_cast<std::size_t>(y + dy) * a_width + x + dx] * k;
								weight += k;
							}
						}
						scratch[static_cast<std::size_t>(y) * a_width + x] = total / weight;
					}
				}
				a_field.swap(scratch);
			}
		}

		std::string Lowercase(const char* a_text)
		{
			std::string result(a_text ? a_text : "");
			std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return result;
		}

		const char* ModelOf(const RE::TESBoundObject* a_base)
		{
			const auto* model = a_base ? a_base->As<RE::TESModel>() : nullptr;
			return model ? model->GetModel() : nullptr;
		}

		/** @brief Classifies an effect model path; false when it is not a waterfall or rapids effect. */
		bool Classify(const char* a_model, FallSites::FxClass& a_class)
		{
			const std::string model = Lowercase(a_model);
			if (model.find("effects\\") == std::string::npos)
				return false;
			// Ambient fish leaping in waterfalls are not water
			if (model.find("salmon") != std::string::npos)
				return false;
			const auto has = [&](const char* a_part) { return model.find(a_part) != std::string::npos; };
			if (has("waterfallbody") || has("waterfallthin") || has("waterfalltall") || has("waterwall"))
				a_class = FallSites::FxClass::Body;
			else if (has("waterfallskirt"))
				a_class = FallSites::FxClass::Skirt;
			else if (has("rapids"))
				a_class = FallSites::FxClass::Rapids;
			else if (has("splash") || has("churn"))
				a_class = FallSites::FxClass::Splash;
			else if ((has("waterfall") && has("mist")) || has("mistblast"))
				a_class = FallSites::FxClass::Mist;
			else
				return false;
			return true;
		}

		/** @brief Half-size of the box given to a reference with no triangle geometry (particle emitters), world units. */
		constexpr float POINT_EXTENT = 64.0f;

		/**
		 * @brief World bounds of a reference from its triangle vertices: effect meshes' own bounds include their particles.
		 * References with no triangles get a POINT_EXTENT box round their position.
		 */
		bool WorldBounds(RE::TESObjectREFR& a_ref, float a_min[3], float a_max[3])
		{
			auto* root = a_ref.Get3D();
			if (!root)
				return false;
			for (int k = 0; k < 3; k++) {
				a_min[k] = FLT_MAX;
				a_max[k] = -FLT_MAX;
			}
			bool any = false;
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
					const RE::NiPoint3 world = transform * RE::NiPoint3(local[0], local[1], local[2]);
					const float p[3] = { world.x, world.y, world.z };
					for (int k = 0; k < 3; k++) {
						a_min[k] = std::min(a_min[k], p[k]);
						a_max[k] = std::max(a_max[k], p[k]);
					}
					any = true;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			if (!any) {
				const auto position = a_ref.GetPosition();
				const float p[3] = { position.x, position.y, position.z };
				for (int k = 0; k < 3; k++) {
					a_min[k] = p[k] - POINT_EXTENT;
					a_max[k] = p[k] + POINT_EXTENT;
				}
			}
			return true;
		}

		/** @brief Gap between two boxes (0 where they overlap), the largest of the three axes. */
		float BoxGap(const float a_minA[3], const float a_maxA[3], const float a_minB[3], const float a_maxB[3])
		{
			float gap = 0.0f;
			for (int k = 0; k < 3; k++)
				gap = std::max(gap, std::max(a_minA[k] - a_maxB[k], a_minB[k] - a_maxA[k]));
			return gap;
		}

		const char* KindName(FallSites::Kind a_kind)
		{
			switch (a_kind) {
			case FallSites::Kind::Fall:
				return "fall";
			case FallSites::Kind::Cascade:
				return "cascade";
			case FallSites::Kind::Rapids:
				return "rapids";
			case FallSites::Kind::NoBottom:
				return "no bottom";
			default:
				return "failed";
			}
		}

		RE::NiCamera* PlayerCamera()
		{
			auto* playerCamera = RE::PlayerCamera::GetSingleton();
			if (!playerCamera || !playerCamera->cameraRoot)
				return nullptr;
			for (auto& child : playerCamera->cameraRoot->GetChildren()) {
				if (auto* camera = netimmerse_cast<RE::NiCamera*>(child.get()))
					return camera;
			}
			return nullptr;
		}
	}

	void FallSites::SetHideEffects(bool a_hide, uint32_t a_layerMask)
	{
		if (a_hide == hideEffects && a_layerMask == layerMask)
			return;
		hideEffects = a_hide;
		layerMask = a_layerMask;
		RefreshHiddenEffects();
	}

	FallSites::Layer FallSites::LayerOf(RE::BSGeometry* a_geometry)
	{
		auto* triShape = a_geometry->AsTriShape();
		if (!triShape || triShape->GetTrishapeRuntimeData().triangleCount == 0)
			return Layer::Particles;
		auto* property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
		auto* effect = property ? netimmerse_cast<RE::BSEffectShaderProperty*>(property) : nullptr;
		auto* material = effect ? static_cast<RE::BSEffectShaderMaterial*>(effect->GetBaseMaterial()) : nullptr;
		if (!material)
			return Layer::Inner;
		return Lowercase(material->sourceTexturePath.c_str()).find("whitewater") != std::string::npos ? Layer::Sheet : Layer::Streaks;
	}

	void FallSites::RefreshHiddenEffects()
	{
		std::unordered_map<const RE::NiAVObject*, RE::NiPointer<RE::NiAVObject>> wanted;
		if (hideEffects) {
			for (const auto& ref : refs) {
				auto* object = RE::TESForm::LookupByID<RE::TESObjectREFR>(ref.formID);
				auto* root = object ? object->Get3D() : nullptr;
				if (!root)
					continue;
				if (ref.cls != FxClass::Body) {
					// D81: effect layers and particles only; some models carry lit rock (FXrapidsRocks01's boulders)
					RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
						if (!netimmerse_cast<RE::BSLightingShaderProperty*>(a_geometry->GetGeometryRuntimeData().shaderProperty.get()))
							wanted.emplace(a_geometry, RE::NiPointer<RE::NiAVObject>(a_geometry));
						return RE::BSVisit::BSVisitControl::kContinue;
					});
					continue;
				}
				// Falling-water models: show only the layers in the mask
				RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
					// D73: an original drawn by its dense copy stays hidden; the copy takes its layer
					if (denseOf.contains(a_geometry))
						return RE::BSVisit::BSVisitControl::kContinue;
					if (!(layerMask & (1u << static_cast<uint32_t>(LayerOf(a_geometry)))))
						wanted.emplace(a_geometry, RE::NiPointer<RE::NiAVObject>(a_geometry));
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
		}
		for (auto& [object, pointer] : hiddenEffects) {
			// D91: a lit piece the refractive pass draws stays hidden
			const bool lit = std::any_of(litHidden.begin(), litHidden.end(), [&](const RE::NiPointer<RE::BSGeometry>& a_hidden) { return a_hidden.get() == object; });
			if (!wanted.contains(object) && pointer && !lit)
				pointer->SetAppCulled(false);
		}
		for (auto& [object, pointer] : wanted)
			pointer->SetAppCulled(true);
		hiddenEffects.swap(wanted);
	}

	void FallSites::Clear()
	{
		outwardNormals.clear();  // D109
		depthRaysShown.clear();
		depthQueue.clear();
		depthJob.reset();
		buryQueue.clear();  // D112
		buryDone = true;
		if (denseWork.valid())  // D172: wait for the dense worker; its results are dropped
			denseWork.wait();
		denseWork = {};
		denseJobs.clear();
		denseFailed.clear();
		if (heavyWork.valid())  // D172: wait for the seams' or bakes' linking worker (it cannot be stopped)
			heavyWork.wait();
		heavyWork = {};
		linkedDue = false;
		plungeResult.reset();
		poolResult.reset();
		plungesDue = false;
		if (cullJob)  // D172: cancel the worker and wait for it
			cullJob->cancel = true;
		if (cullWork.valid())
			cullWork.wait();
		cullWork = {};
		cullJob.reset();  // D113
		cullRequested = false;
		hideEffects = false;
		RefreshHiddenEffects();
		// D91: the pieces the refractive pass drew show again (the wish is kept for the next sites)
		for (const auto& geometry : litHidden) {
			if (geometry && !denseOf.contains(geometry.get()))
				geometry->SetAppCulled(false);
		}
		litHidden.clear();
		RemoveDense();
		RemoveUnified();
		grounded.clear();
		clipQueue.clear();
		bakeQueue.clear();
		bakeJob.reset();
		if (!flowBakes.empty())
			flowBakeVersion++;
		flowBakes.clear();
		bakeFields.clear();
		if (!plungePoints.empty() || !sprayEmitters.empty())
			emitterVersion++;
		plungePoints.clear();
		sprayEmitters.clear();
		vertexClipBase.clear();
		groundVersion++;
		refs.clear();
		sites.clear();
		refSignature = 0;
		waterSignature = 0;
		updatesUntilScan = 0;
		cellSummaries.clear();
		// Captures and their pools stay: turning waterfalls back on finds the same sites and rock at once
	}

	const FallCapture* FallSites::GetCapture(const Site& a_site) const
	{
		const auto it = a_site.captureKey ? captures.find(a_site.captureKey) : captures.end();
		return it != captures.end() ? &it->second : nullptr;
	}

	void FallSites::RasterSurface(const Site& a_site, float a_originX, float a_originY, float a_spacing, int32_t a_width, int32_t a_height, std::vector<float>& a_surface,
		std::vector<float>* a_lowest, std::vector<float>* a_highest) const
	{
		a_surface.assign(static_cast<std::size_t>(a_width) * a_height, NO_SURFACE);
		if (a_lowest)
			a_lowest->assign(a_surface.size(), FLT_MAX);
		if (a_highest)
			a_highest->assign(a_surface.size(), NO_SURFACE);
		const auto window = [&](int32_t a_x, int32_t a_y) { return static_cast<std::size_t>(a_y) * a_width + a_x; };
		const auto range = [&](float a_min, float a_max, float a_origin, int32_t a_count, int32_t& a_from, int32_t& a_to) {
			a_from = std::max(0, static_cast<int32_t>(std::ceil((a_min - a_origin) / a_spacing)));
			a_to = std::min(a_count - 1, static_cast<int32_t>(std::floor((a_max - a_origin) / a_spacing)));
		};
		// The surface: the highest triangle over each point. D53: lowest and highest per cell, curtains too (walked along each edge).
		const auto extremes = [&](const RE::NiPoint3& a, const RE::NiPoint3& b) {
			const float run = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
			const auto steps = static_cast<int32_t>(std::ceil(run / (a_spacing * 0.5f))) + 1;
			for (int32_t s = 0; s <= steps; s++) {
				const float f = static_cast<float>(s) / static_cast<float>(steps);
				const auto x = static_cast<int32_t>(std::lround((a.x + (b.x - a.x) * f - a_originX) / a_spacing));
				const auto y = static_cast<int32_t>(std::lround((a.y + (b.y - a.y) * f - a_originY) / a_spacing));
				if (x < 0 || y < 0 || x >= a_width || y >= a_height)
					continue;
				const float z = a.z + (b.z - a.z) * f;
				if (a_lowest)
					(*a_lowest)[window(x, y)] = std::min((*a_lowest)[window(x, y)], z);
				if (a_highest)
					(*a_highest)[window(x, y)] = std::max((*a_highest)[window(x, y)], z);
			}
		};
		const auto raster = [&](const RE::NiPoint3& a, const RE::NiPoint3& b, const RE::NiPoint3& c) {
			if (a_lowest || a_highest) {
				extremes(a, b);
				extremes(b, c);
				extremes(c, a);
			}
			const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
			if (std::abs(area) < 1e-3f)
				return;  // Edge-on from above: a vertical curtain adds nothing to the surface
			int32_t x0, x1, y0, y1;
			range(std::min({ a.x, b.x, c.x }), std::max({ a.x, b.x, c.x }), a_originX, a_width, x0, x1);
			range(std::min({ a.y, b.y, c.y }), std::max({ a.y, b.y, c.y }), a_originY, a_height, y0, y1);
			for (int32_t y = y0; y <= y1; y++) {
				for (int32_t x = x0; x <= x1; x++) {
					const float px = a_originX + static_cast<float>(x) * a_spacing;
					const float py = a_originY + static_cast<float>(y) * a_spacing;
					const float w0 = ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) / area;
					const float w1 = ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) / area;
					const float w2 = 1.0f - w0 - w1;
					if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f)
						continue;
					float& stored = a_surface[window(x, y)];
					const float z = w0 * a.z + w1 * b.z + w2 * c.z;
					stored = std::max(stored, z);
					if (a_lowest)
						(*a_lowest)[window(x, y)] = std::min((*a_lowest)[window(x, y)], z);
					if (a_highest)
						(*a_highest)[window(x, y)] = std::max((*a_highest)[window(x, y)], z);
				}
			}
		};
		for (const uint32_t member : a_site.members) {
			if (refs[member].cls != FxClass::Body)
				continue;
			auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refs[member].formID);
			auto* root = ref ? ref->Get3D() : nullptr;
			if (!root)
				continue;
			RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
				auto* triShape = a_geometry->AsTriShape();
				auto* renderer = a_geometry->GetGeometryRuntimeData().rendererData;
				if (!triShape || !renderer || !renderer->rawVertexData || !renderer->rawIndexData)
					return RE::BSVisit::BSVisitControl::kContinue;
				const auto& counts = triShape->GetTrishapeRuntimeData();
				const uint32_t stride = renderer->vertexDesc.GetSize();
				const uint32_t positionOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
				if (stride < positionOffset + 12)
					return RE::BSVisit::BSVisitControl::kContinue;
				const auto& transform = a_geometry->world;
				const auto vertex = [&](uint16_t a_index) {
					float local[3];
					std::memcpy(local, renderer->rawVertexData + static_cast<std::size_t>(a_index) * stride + positionOffset, sizeof(local));
					return transform * RE::NiPoint3(local[0], local[1], local[2]);
				};
				for (uint32_t t = 0; t < counts.triangleCount; t++) {
					const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(t) * 3;
					if (index[0] >= counts.vertexCount || index[1] >= counts.vertexCount || index[2] >= counts.vertexCount)
						continue;
					raster(vertex(index[0]), vertex(index[1]), vertex(index[2]));
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}
	}

	FallSites::PoolField FallSites::ComputePools(const Site& a_site, const FallCapture& a_capture) const
	{
		// D78: pool water over a complete capture (was D36's flow-solve cover); the pool cut, debug arrows and plunges read it
		PoolField pools;
		pools.originX = a_capture.GetOriginX();
		pools.originY = a_capture.GetOriginY();
		pools.width = a_capture.GetWidth();
		pools.height = a_capture.GetHeight();
		pools.top = a_site.top;
		const auto cw = static_cast<int32_t>(pools.width);
		const auto ch = static_cast<int32_t>(pools.height);
		const std::size_t cells = static_cast<std::size_t>(pools.width) * pools.height;
		std::vector<float> surface, lowest, highest;
		RasterSurface(a_site, pools.originX, pools.originY, FallCapture::CELL, cw, ch, surface, &lowest, &highest);
		std::vector<float> bed(cells, 0.0f);
		for (uint32_t y = 0; y < pools.height; y++) {
			for (uint32_t x = 0; x < pools.width; x++)
				bed[static_cast<std::size_t>(y) * pools.width + x] = a_capture.At(x, y);
		}
		// D40, D45: the site's planes miss pools (a tier between two falls) and are whole cells, so sample the engine's water per cell.
		// Pool water: rock under its level, and either the falls' surface at that level or a shallow bed (not a fall's face).
		pools.cover.assign(cells, NO_SURFACE);
		std::vector<float> levels(cells, NO_SURFACE);
		std::vector<std::size_t> open;
		// D53: the falls' lowest and highest within POOL_LIP_REACH. Where they drop below a pool's level with none above it, the cell
		// is a lip or the air under a curtain, which the top-down rock calls deep water.
		std::vector<float> lowNear(cells, FLT_MAX), highNear(cells, NO_SURFACE);
		{
			std::vector<float> lowRow(cells, FLT_MAX), highRow(cells, NO_SURFACE);
			for (int32_t y = 0; y < ch; y++) {
				for (int32_t x = 0; x < cw; x++) {
					const std::size_t i = static_cast<std::size_t>(y) * cw + x;
					for (int32_t d = std::max(0, x - POOL_LIP_REACH); d <= std::min(cw - 1, x + POOL_LIP_REACH); d++) {
						lowRow[i] = std::min(lowRow[i], lowest[static_cast<std::size_t>(y) * cw + d]);
						highRow[i] = std::max(highRow[i], highest[static_cast<std::size_t>(y) * cw + d]);
					}
				}
			}
			for (int32_t y = 0; y < ch; y++) {
				for (int32_t x = 0; x < cw; x++) {
					const std::size_t i = static_cast<std::size_t>(y) * cw + x;
					for (int32_t d = std::max(0, y - POOL_LIP_REACH); d <= std::min(ch - 1, y + POOL_LIP_REACH); d++) {
						lowNear[i] = std::min(lowNear[i], lowRow[static_cast<std::size_t>(d) * cw + x]);
						highNear[i] = std::max(highNear[i], highRow[static_cast<std::size_t>(d) * cw + x]);
					}
				}
			}
		}
		const auto dropsAway = [&](std::size_t a_i, float a_level) {
			return lowNear[a_i] < a_level - POOL_LIP_DROP && highNear[a_i] < a_level + POOL_BAND && a_level > a_site.bottom + POOL_BAND;
		};
		uint32_t lips = 0;
		for (int32_t y = 0; y < ch; y++) {
			for (int32_t x = 0; x < cw; x++) {
				const std::size_t i = static_cast<std::size_t>(y) * cw + x;
				float level = NO_SURFACE;
				if (waterSurfaces) {
					float height;
					if (waterSurfaces->SampleHeight(pools.originX + static_cast<float>(x) * FallCapture::CELL, pools.originY + static_cast<float>(y) * FallCapture::CELL, height) !=
						WaterSurfaces::Source::None)
						level = height;
				}
				levels[i] = level;
				const float rock = bed[i];
				const float mesh = surface[i];
				if (level <= NO_SURFACE || rock <= FallCapture::NONE || rock >= level - POOL_COVER)
					continue;
				const bool meshAtLevel = mesh > NO_SURFACE && mesh >= level - POOL_BAND;
				const bool bottomPool = level <= a_site.bottom + POOL_BAND;  // D50: the pool the falls end in
				const bool shallowBed = rock >= level - POOL_SHALLOW;
				if (dropsAway(i, level)) {
					lips++;
					continue;
				}
				if (meshAtLevel || shallowBed || bottomPool) {
					pools.cover[i] = level;
					open.push_back(i);
				}
			}
		}
		// D52: flood the basin at the same level over rock, stopped where falls run below the surface, so deep middle tiers are found
		while (!open.empty()) {
			const std::size_t i = open.back();
			open.pop_back();
			const auto x = static_cast<int32_t>(i % cw), y = static_cast<int32_t>(i / cw);
			const float level = pools.cover[i];
			static constexpr int32_t DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
			for (int k = 0; k < 4; k++) {
				const int32_t nx = x + DX[k], ny = y + DY[k];
				if (nx < 0 || ny < 0 || nx >= cw || ny >= ch)
					continue;
				const std::size_t j = static_cast<std::size_t>(ny) * cw + nx;
				if (pools.cover[j] > NO_SURFACE || std::abs(levels[j] - level) > 1.0f)
					continue;
				const float rock = bed[j];
				const float mesh = surface[j];
				if (rock <= FallCapture::NONE || rock >= level - POOL_COVER || (mesh > NO_SURFACE && mesh < level - POOL_BAND) || dropsAway(j, level))
					continue;
				pools.cover[j] = level;
				open.push_back(j);
			}
		}
		logger::info("[Fluid Field] Waterfall site #{} pools: {} cells, {} kept out as lips and curtains", a_site.id,
			std::count_if(pools.cover.begin(), pools.cover.end(), [](float a_cover) { return a_cover > NO_SURFACE; }), lips);
		return pools;
	}

	void FallSites::PollPools()
	{
		// D172: pools of complete captures, on a worker (up to 90 ms on the render thread); FinishHeavyWork takes them in
		std::vector<std::pair<const Site*, const FallCapture*>> todo;
		for (const auto& site : sites) {
			if (!site.captureKey || sitePools.contains(site.captureKey))
				continue;
			const FallCapture* capture = GetCapture(site);
			if (!capture || !capture->IsComplete())
				continue;
			todo.emplace_back(&site, capture);
		}
		if (todo.empty())
			return;
		poolResult = std::make_unique<std::vector<std::pair<uint64_t, PoolField>>>();
		heavyWork = std::async(std::launch::async, [this, todo, result = poolResult.get()]() {
			const auto start = std::chrono::steady_clock::now();
			for (const auto& [site, capture] : todo)
				result->emplace_back(site->captureKey, ComputePools(*site, *capture));
			loadTiming.AddWorker(LoadWork::Pools, start);
		});
	}

	void FallSites::AppendWaterEmitters(std::vector<WaterEmitter>& a_emitters, const int32_t a_gridMin[2], float a_cellSize, uint32_t a_gridRes,
		float a_strength, uint32_t a_maxEmitters, bool a_foam) const
	{
		if (a_strength <= 0.0f || !(a_cellSize > 0.0f) || plungePoints.empty() || a_emitters.size() >= a_maxEmitters)
			return;
		// Nearest the grid's centre first
		const float centre = 0.5f * static_cast<float>(a_gridRes);
		std::vector<std::pair<float, const FallPlunge*>> order;
		order.reserve(plungePoints.size());
		for (const auto& plunge : plungePoints) {
			const float tx = plunge.position[0] / a_cellSize - static_cast<float>(a_gridMin[0]);
			const float ty = plunge.position[1] / a_cellSize - static_cast<float>(a_gridMin[1]);
			if (tx < 0.0f || ty < 0.0f || tx >= static_cast<float>(a_gridRes) || ty >= static_cast<float>(a_gridRes))
				continue;
			order.emplace_back((tx - centre) * (tx - centre) + (ty - centre) * (ty - centre), &plunge);
		}
		std::sort(order.begin(), order.end(), [](const auto& a_a, const auto& a_b) { return a_a.first < a_b.first; });
		for (const auto& [distance, plunge] : order) {
			if (a_emitters.size() >= a_maxEmitters)
				break;
			const float pull = std::min(plunge->speed * PLUNGE_PULL, 260.0f) * a_strength;
			// D56: just downstream of the falls' cut, so the push never lowers the water there
			const float x = plunge->position[0] + plunge->direction[0] * PLUNGE_PUSH_OFFSET;
			const float y = plunge->position[1] + plunge->direction[1] * PLUNGE_PUSH_OFFSET;
			a_emitters.push_back({ .position = { x / a_cellSize - static_cast<float>(a_gridMin[0]), y / a_cellSize - static_cast<float>(a_gridMin[1]) },
				.radius = std::max(0.75f * PLUNGE_WATER_SPACING / a_cellSize, 1.5f),
				.depthRate = 0.0f,  // D55: no dent (it broke the foam along the cut); flow and foam only
				.velocity = { plunge->direction[0] * pull, plunge->direction[1] * pull },
				.pad1 = { PLUNGE_MARK, a_foam ? 4.0f * std::max(plunge->strength, 0.6f) * a_strength : 0.0f } });  // D56: no spreading out (it dented the water)
		}
	}

	void FallSites::CountCaptures(uint32_t& a_complete, uint32_t& a_total) const
	{
		a_complete = a_total = 0;
		for (const auto& [key, capture] : captures) {
			a_total++;
			a_complete += capture.IsComplete() ? 1 : 0;
		}
	}

	void FallSites::RefreshGrounded()
	{
		// D26: ground the falling-water meshes of every site with captured ground
		grounded.clear();
		std::unordered_set<const RE::BSGeometry*> litMeshes;
		litMeshList.clear();
		std::vector<RE::NiPointer<RE::BSGeometry>> toDensify;
		std::unordered_set<const RE::BSGeometry*> denseSeen;
		if (unifiedFalls && !unifiedLoaded)
			LoadUnifiedCaches();
		std::unordered_set<std::size_t> unifiedSeen;
		const auto queueLit = [&](RE::BSGeometry* a_geometry, const std::shared_ptr<BakeField>& a_baked) {
			litMeshes.insert(a_geometry);
			litMeshList.push_back({ RE::NiPointer<RE::BSGeometry>(a_geometry) });
			if (!vertexClipBase.contains(a_geometry) &&
				std::none_of(clipQueue.begin(), clipQueue.end(), [&](const RE::NiPointer<RE::BSGeometry>& a_queued) { return a_queued.get() == a_geometry; }))
				clipQueue.emplace_back(a_geometry);
			// Baked offline: taken as a finished bake (linked, plunged and shaded as any), never baked here
			if (a_baked) {
				if (!bakeFields.contains(a_geometry)) {
					bakeFields[a_geometry] = a_baked;
					bakesCached++;
					linkDue = true;
				}
				return;
			}
			if (!bakeFields.contains(a_geometry) && (!bakeJob || bakeJob->geometry.get() != a_geometry) &&
				std::none_of(bakeQueue.begin(), bakeQueue.end(), [&](const RE::NiPointer<RE::BSGeometry>& a_queued) { return a_queued.get() == a_geometry; }) &&
				std::none_of(bakesPending.begin(), bakesPending.end(), [&](const auto& a_pending) { return a_pending.first.get() == a_geometry; }))
				bakeQueue.emplace_back(a_geometry);
		};
		for (const auto& site : sites) {
			const FallCapture* capture = GetCapture(site);
			if (!capture || !capture->IsComplete())
				continue;
			// Unified falls: a site whose cache's members are all here draws as one sheet (the members' lit meshes hidden, not copied)
			std::size_t unifiedIndex = SIZE_MAX;
			for (std::size_t c = 0; unifiedFalls && c < unifiedCaches.size() && unifiedIndex == SIZE_MAX; c++) {
				const auto& wanted = unifiedCaches[c].members;
				if (std::all_of(wanted.begin(), wanted.end(), [&](RE::FormID a_id) {
						return std::any_of(site.members.begin(), site.members.end(), [&](uint32_t a_member) { return refs[a_member].formID == a_id && refs[a_member].cls == FxClass::Body; });
					}))
					unifiedIndex = c;
			}
			std::vector<RE::BSGeometry*> unifiedLit;
			RE::NiNode* unifiedRoot = nullptr;
			for (const uint32_t member : site.members) {
				if (refs[member].cls != FxClass::Body)
					continue;
				auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refs[member].formID);
				auto* root = ref ? ref->Get3D() : nullptr;
				if (!root)
					continue;
				if (unifiedIndex != SIZE_MAX && std::ranges::find(unifiedCaches[unifiedIndex].members, refs[member].formID) != unifiedCaches[unifiedIndex].members.end()) {
					if (!unifiedRoot)
						unifiedRoot = root->AsNode();
					RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
						// The sheet itself, and old dense copies (detached below: their originals go unseen)
						auto* triShape = a_geometry->AsTriShape();
						if (unifiedShapes.contains(a_geometry) || denseShapes.contains(a_geometry) || !triShape || triShape->GetTrishapeRuntimeData().triangleCount == 0)
							return RE::BSVisit::BSVisitControl::kContinue;
						if (netimmerse_cast<RE::BSLightingShaderProperty*>(a_geometry->GetGeometryRuntimeData().shaderProperty.get()))
							unifiedLit.push_back(a_geometry);
						else
							grounded.insert(a_geometry);  // the effect layers ground as before
						return RE::BSVisit::BSVisitControl::kContinue;
					});
					continue;
				}
				RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
					// D73: skip an original drawn by its dense copy (the copy is found as its sibling)
					if (denseOf.contains(a_geometry)) {
						denseSeen.insert(a_geometry);
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					if (DENSE && !denseShapes.contains(a_geometry) && !denseFailed.contains(a_geometry) && a_geometry->AsTriShape() &&
						a_geometry->AsTriShape()->GetTrishapeRuntimeData().triangleCount > 0 && netimmerse_cast<RE::BSLightingShaderProperty*>(a_geometry->GetGeometryRuntimeData().shaderProperty.get())) {
						// D172: copied on a worker (FinishDenseWork); not lit, clipped or baked until then
						toDensify.emplace_back(a_geometry);
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					// Water surfaces only: the particle spray (no triangles) would smear grounding and the streamlines
					auto* triShape = a_geometry->AsTriShape();
					if (triShape && triShape->GetTrishapeRuntimeData().triangleCount > 0)
						grounded.insert(a_geometry);
					// D56: test the lit surface's vertices against the rock, once
					if (triShape && triShape->GetTrishapeRuntimeData().triangleCount > 0 && netimmerse_cast<RE::BSLightingShaderProperty*>(a_geometry->GetGeometryRuntimeData().shaderProperty.get())) {
						litMeshes.insert(a_geometry);
						litMeshList.push_back({ RE::NiPointer<RE::BSGeometry>(a_geometry) });
						if (!vertexClipBase.contains(a_geometry) &&
							std::none_of(clipQueue.begin(), clipQueue.end(), [&](const RE::NiPointer<RE::BSGeometry>& a_queued) { return a_queued.get() == a_geometry; }))
							clipQueue.emplace_back(a_geometry);
						if (bakeOthers && !bakeFields.contains(a_geometry) && (!bakeJob || bakeJob->geometry.get() != a_geometry) &&
							std::none_of(bakeQueue.begin(), bakeQueue.end(), [&](const RE::NiPointer<RE::BSGeometry>& a_queued) { return a_queued.get() == a_geometry; }) &&
							std::none_of(bakesPending.begin(), bakesPending.end(), [&](const auto& a_pending) { return a_pending.first.get() == a_geometry; }))
							bakeQueue.emplace_back(a_geometry);
					}
					// Diagnostics: what each part of a falling-water mesh is, once per model and part
					auto* property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
					auto* effect = property ? netimmerse_cast<RE::BSEffectShaderProperty*>(property) : nullptr;
					auto* material = effect ? static_cast<RE::BSEffectShaderMaterial*>(effect->GetBaseMaterial()) : nullptr;
					const char* model = ModelOf(ref->GetBaseObject());
					const std::string key = std::format("{}|{}", model ? model : "?", a_geometry->name.c_str());
					if (loggedParts.insert(key).second) {
						// Alpha, textures and texture-coordinate range
						std::string extra;
						if (auto* alpha = a_geometry->GetGeometryRuntimeData().alphaProperty.get())
							extra += std::format(", alpha flags {:04X} (blend {}, test {}), threshold {}", alpha->alphaFlags,
								alpha->GetAlphaBlending(), alpha->GetAlphaTesting(), alpha->alphaThreshold);
						else
							extra += ", no alpha property";
						if (auto* lighting = property ? netimmerse_cast<RE::BSLightingShaderProperty*>(property) : nullptr) {
							auto* base = static_cast<RE::BSLightingShaderMaterialBase*>(lighting->GetBaseMaterial());
							if (base && base->textureSet)
								extra += std::format(", diffuse '{}', normal '{}'", base->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse),
									base->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kNormal));
							if (base)
								extra += std::format(", uv offset {:.3f},{:.3f} scale {:.3f},{:.3f}", base->texCoordOffset[0].x, base->texCoordOffset[0].y, base->texCoordScale[0].x,
									base->texCoordScale[0].y);
						}
						auto* renderer = a_geometry->GetGeometryRuntimeData().rendererData;
						if (triShape && renderer && renderer->rawVertexData && renderer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_UV)) {
							const uint32_t stride = renderer->vertexDesc.GetSize();
							const uint32_t uvOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_TEXCOORD0);
							float lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
							for (uint32_t v = 0; v < triShape->GetTrishapeRuntimeData().vertexCount; v++) {
								const auto* uv = reinterpret_cast<const uint16_t*>(renderer->rawVertexData + static_cast<std::size_t>(v) * stride + uvOffset);
								for (int k = 0; k < 2; k++) {
									const float value = DirectX::PackedVector::XMConvertHalfToFloat(uv[k]);
									lo[k] = std::min(lo[k], value);
									hi[k] = std::max(hi[k], value);
								}
							}
							extra += std::format(", vertex uv {:.2f}..{:.2f} x {:.2f}..{:.2f}", lo[0], hi[0], lo[1], hi[1]);
						}
						logger::info("[Fluid Field]   fall part {} '{}'{}", model ? model : "?", a_geometry->name.c_str(), extra);
						logger::info("[Fluid Field]   fall part {}: '{}' ({}, {}, layer {}), {} vertices, {} triangles, texture '{}', greyscale '{}', shader flags {:016X}, culled {}",
							model ? model : "?", a_geometry->name.c_str(), a_geometry->GetRTTI() ? a_geometry->GetRTTI()->GetName() : "?",
							property && property->GetRTTI() ? property->GetRTTI()->GetName() : "no shader", static_cast<int>(LayerOf(a_geometry)),
							triShape ? triShape->GetTrishapeRuntimeData().vertexCount : 0, triShape ? triShape->GetTrishapeRuntimeData().triangleCount : 0,
							material ? material->sourceTexturePath.c_str() : "", material ? material->greyscaleTexturePath.c_str() : "",
							property ? property->flags.underlying() : 0ull, a_geometry->GetAppCulled());
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
			// Unified falls: the site's sheet (attached once), lit, clipped and baked as one mesh; its members' lit meshes hidden
			if (unifiedIndex != SIZE_MAX && unifiedRoot && !unifiedLit.empty()) {
				const auto found = unifiedOf.find(unifiedIndex);
				RE::BSGeometry* sheet = found != unifiedOf.end() ? found->second.shape.get() : AttachUnified(unifiedIndex, unifiedRoot, unifiedLit.front());
				if (sheet) {
					unifiedSeen.insert(unifiedIndex);
					auto& unified = unifiedOf[unifiedIndex];
					for (auto* lit : unifiedLit) {
						lit->SetAppCulled(true);
						if (std::none_of(unified.hidden.begin(), unified.hidden.end(), [&](const RE::NiPointer<RE::BSGeometry>& a_hidden) { return a_hidden.get() == lit; }))
							unified.hidden.emplace_back(lit);
					}
					grounded.insert(sheet);
					queueLit(sheet, unifiedCaches[unifiedIndex].bake);
				}
			}
		}
		// Unified falls: sheets whose site went away (cell unloaded) detached, their members shown again
		for (auto it = unifiedOf.begin(); it != unifiedOf.end();) {
			if (unifiedSeen.contains(it->first)) {
				++it;
				continue;
			}
			if (it->second.parent && it->second.shape)
				it->second.parent->DetachChild(it->second.shape.get());
			for (auto& original : it->second.hidden) {
				if (original)
					original->SetAppCulled(false);
			}
			unifiedShapes.erase(it->second.shape.get());
			it = unifiedOf.erase(it);
		}
		// D73: drop copies whose original went away; new lit meshes get theirs, and grounding then uses the copies
		for (auto it = denseOf.begin(); it != denseOf.end();) {
			if (denseSeen.contains(it->first)) {
				++it;
				continue;
			}
			if (it->second.parent && it->second.shape)
				it->second.parent->DetachChild(it->second.shape.get());
			denseShapes.erase(it->second.shape.get());
			untrimmedIndices.erase(it->second.shape.get());  // D172
			it = denseOf.erase(it);
		}
		if (!toDensify.empty() && !refreshing && !denseWork.valid()) {
			// D172: copied here, made on a worker, attached by FinishDenseWork
			for (const auto& geometry : toDensify) {
				auto job = std::make_unique<DenseJob>();
				if (ReadDense(geometry.get(), *job))
					denseJobs.push_back(std::move(job));
				else
					denseFailed.insert(geometry.get());
			}
			if (!denseJobs.empty()) {
				std::vector<DenseJob*> running;
				for (auto& job : denseJobs)
					running.push_back(job.get());
				// D173: gather what can hide the copies' sheets (statics, copies made, landscape); the worker finds the outer shell per
				// batch
				float lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
				for (const auto* job : running) {
					for (int k = 0; k < 2; k++) {
						lo[k] = std::min(lo[k], job->lo[k]);
						hi[k] = std::max(hi[k], job->hi[k]);
					}
				}
				auto shell = std::make_shared<ShellInput>();
				const auto gatherStart = std::chrono::steady_clock::now();
				GatherOccluders(lo[0] - OCCLUDER_REACH, lo[1] - OCCLUDER_REACH, hi[0] + OCCLUDER_REACH, hi[1] + OCCLUDER_REACH, shell->occluders);
				shell->gatherMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gatherStart).count();
				auto* tes = RE::TES::GetSingleton();
				shell->interior = !tes || tes->interiorCell != nullptr || !landscape;
				if (!shell->interior)
					shell->land = landscape->Subset(lo[0] - FallVisibility::REACH, lo[1] - FallVisibility::REACH, hi[0] + FallVisibility::REACH, hi[1] + FallVisibility::REACH);
				denseStarted = std::chrono::steady_clock::now();
				denseWork = std::async(std::launch::async, [this, running, shell]() {
					const auto start = std::chrono::steady_clock::now();
					for (auto* job : running)
						ComputeDense(*job, shell.get());
					TrimHidden(running, *shell);
					loadTiming.AddWorker(LoadWork::Dense, start);
				});
			}
		}
		// Drop meshes no longer grounded (cell unloaded) so a reused address is not mistaken for them
		std::erase_if(vertexClipBase, [&](const auto& a_entry) { return !litMeshes.contains(a_entry.first); });
		std::erase_if(gapShifts, [&](const auto& a_entry) { return !litMeshes.contains(a_entry.first); });
		std::erase_if(vertexPushes, [&](const auto& a_entry) { return !litMeshes.contains(a_entry.first); });
		std::erase_if(outwardNormals, [&](const auto& a_entry) { return !litMeshes.contains(a_entry.first); });
		std::erase_if(depthRaysShown, [&](const auto& a_entry) { return !litMeshes.contains(a_entry.first); });
		if (std::erase_if(flowBakes, [&](const auto& a_entry) { return !litMeshes.contains(a_entry.first); }))
			flowBakeVersion++;
		// D78: plunges and spray go with their meshes
		if (std::erase_if(bakeFields, [&](const auto& a_entry) { return !litMeshes.contains(a_entry.first); }))
			plungesDue = true;  // D172: ComputePlunges on a worker (Update)
		groundVersion++;
		ApplyLitHidden();
	}

	void FallSites::SetLitHidden(bool a_hidden)
	{
		if (a_hidden == litHiddenWanted)
			return;
		litHiddenWanted = a_hidden;
		ApplyLitHidden();
	}

	void FallSites::ApplyLitHidden()
	{
		// Show what is no longer wanted hidden (or no longer lit); an original drawn by its dense copy (D73) stays hidden
		std::unordered_set<const RE::BSGeometry*> current;
		if (litHiddenWanted) {
			for (const auto& lit : litMeshList)
				current.insert(lit.geometry.get());
		}
		std::unordered_set<const RE::BSGeometry*> replaced;  // unified falls: the members' meshes a site sheet draws for
		for (const auto& [index, unified] : unifiedOf) {
			for (const auto& original : unified.hidden)
				replaced.insert(original.get());
		}
		std::erase_if(litHidden, [&](const RE::NiPointer<RE::BSGeometry>& a_geometry) {
			if (!a_geometry)
				return true;
			if (current.contains(a_geometry.get()))
				return false;
			// Not an original drawn by its copy or a site's sheet, nor a layer the debug hides (RefreshHiddenEffects)
			if (!denseOf.contains(a_geometry.get()) && !replaced.contains(a_geometry.get()) && !hiddenEffects.contains(a_geometry.get()))
				a_geometry->SetAppCulled(false);
			return true;
		});
		if (!litHiddenWanted)
			return;
		for (const auto& lit : litMeshList) {
			auto* geometry = lit.geometry.get();
			if (!geometry || std::any_of(litHidden.begin(), litHidden.end(), [&](const RE::NiPointer<RE::BSGeometry>& a_hidden) { return a_hidden.get() == geometry; }))
				continue;
			geometry->SetAppCulled(true);
			litHidden.emplace_back(geometry);
		}
	}

	float FallSites::RockAlong(RE::hkpWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_direction, float a_reach, int* a_crossings, float* a_last) const
	{
		// Passes through anything not rock (the falls' own collision is water), counting rock crossings
		const float scale = RE::bhkWorld::GetWorldScale();
		float first = -1.0f;
		float from = 0.0f;
		for (int step = 0; step < 8 && from < a_reach; step++) {
			const RE::NiPoint3 start = a_from + a_direction * from;
			const RE::NiPoint3 end = a_from + a_direction * a_reach;
			RE::hkpWorldRayCastInput input;
			input.from = RE::hkVector4(start.x * scale, start.y * scale, start.z * scale, 0.0f);
			input.to = RE::hkVector4(end.x * scale, end.y * scale, end.z * scale, 0.0f);
			RE::hkpWorldRayCastOutput output;
			a_world->CastRay(input, output);
			if (!output.HasHit())
				break;
			const float hit = from + (a_reach - from) * output.hitFraction;
			const auto layer = output.rootCollidable->GetCollisionLayer();
			const bool solid = layer == RE::COL_LAYER::kStatic || layer == RE::COL_LAYER::kAnimStatic || layer == RE::COL_LAYER::kTerrain || layer == RE::COL_LAYER::kGround;
			const auto* ref = RE::TESHavokUtilities::FindCollidableRef(*output.rootCollidable);
			const char* model = ref ? ModelOf(ref->GetBaseObject()) : nullptr;
			const std::string name = model ? Lowercase(model) : std::string();
			if (solid && name.find("fxwaterfall") == std::string::npos && name.find("fxrapids") == std::string::npos) {
				if (first < 0.0f)
					first = hit;
				if (a_crossings)
					(*a_crossings)++;
				if (a_last)
					*a_last = hit;
				if (!a_crossings)
					break;
			}
			from = hit + 0.5f;
		}
		return first;
	}

	void FallSites::DepthStep()
	{
		auto* tes = RE::TES::GetSingleton();
		if (!tes)
			return;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(CLIP_BUDGET_MS);
		while (std::chrono::steady_clock::now() < deadline) {
			if (!depthJob) {
				if (depthQueue.empty()) {
					// TEMPORARY D110 audit: depth behind each mesh's shown vertices, once all are done
					if (!depthAudited) {
						depthAudited = true;
						for (const auto& [geometry, base] : vertexClipBase) {
							auto* triShape = geometry ? const_cast<RE::BSGeometry*>(geometry)->AsTriShape() : nullptr;
							if (!triShape)
								continue;
							const uint32_t count = triShape->GetTrishapeRuntimeData().vertexCount;
							if (base + count > vertexClips.size())
								continue;
							std::vector<float> shown;
							for (uint32_t v = 0; v < count; v++) {
								if (VertexShown(base + v) && vertexClips[base + v][3] < 3.5f)
									shown.push_back(vertexClips[base + v][13]);
							}
							if (shown.empty())
								continue;
							std::sort(shown.begin(), shown.end());
							const auto at = [&](float a_q) { return shown[std::min(shown.size() - 1, static_cast<std::size_t>(a_q * static_cast<float>(shown.size())))]; };
							const auto atReach = std::count_if(shown.begin(), shown.end(), [](float a_d) { return a_d >= CLEAR_REACH - 1.0f; });
							logger::info("[Fluid Field]   depth audit '{}': {} of {} shown; depth 10% {:.0f}, 50% {:.0f}, 90% {:.0f}; {} at the reach", geometry->name.c_str(), shown.size(), count,
								at(0.1f), at(0.5f), at(0.9f), atReach);
						}
					}
					return;
				}
				// D109: a mesh waits for its consistent outward normals (CullStep)
				RE::NiPointer<RE::BSGeometry> geometry = depthQueue.back();
				// D110: and every cut (the lead-in's comes with the swell, ComputePushes); D112: the rock test (BuryStep); D113: the
				// culling (CullStep)
				if (geometry && vertexClipBase.contains(geometry.get()) &&
					(!outwardNormals.contains(geometry.get()) || !vertexPushes.contains(geometry.get()) || !buryDone || CullBusy()))
					return;
				depthQueue.pop_back();
				const auto base = geometry ? vertexClipBase.find(geometry.get()) : vertexClipBase.end();
				auto* triShape = geometry ? geometry->AsTriShape() : nullptr;
				auto* renderer = geometry ? geometry->GetGeometryRuntimeData().rendererData : nullptr;
				if (base == vertexClipBase.end() || !triShape || !renderer || !renderer->rawVertexData || !renderer->rawIndexData)
					continue;
				const auto& counts = triShape->GetTrishapeRuntimeData();
				if (base->second + counts.vertexCount > vertexClips.size())
					continue;
				auto& desc = renderer->vertexDesc;  // CommonLib's getters are not const
				const uint32_t stride = desc.GetSize();
				const uint32_t positionOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
				auto job = std::make_unique<DepthJob>();
				job->geometry = geometry;
				job->base = base->second;
				job->positions.resize(counts.vertexCount);
				job->normals.assign(counts.vertexCount, RE::NiPoint3(0.0f, 0.0f, 0.0f));
				job->ring.resize(counts.vertexCount);
				for (uint32_t v = 0; v < counts.vertexCount; v++) {
					float local[3];
					std::memcpy(local, renderer->rawVertexData + static_cast<std::size_t>(v) * stride + positionOffset, sizeof(local));
					job->positions[v] = geometry->world * RE::NiPoint3(local[0], local[1], local[2]);
				}
				// D106: authored normals (the game lights by them, out of the visible face); the winding is inconsistent, so used only
				// without them
				const bool authored = desc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_NORMAL);
				const uint32_t normalOffset = authored ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_NORMAL) : 0;
				for (uint32_t i = 0; i < counts.triangleCount; i++) {
					const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(i) * 3;
					if (index[0] >= counts.vertexCount || index[1] >= counts.vertexCount || index[2] >= counts.vertexCount)
						continue;
					const RE::NiPoint3 face = (job->positions[index[1]] - job->positions[index[0]]).Cross(job->positions[index[2]] - job->positions[index[0]]);
					for (int k = 0; k < 3; k++) {
						if (!authored)
							job->normals[index[k]] += face;
						job->ring[index[k]].push_back(index[(k + 1) % 3]);
						job->ring[index[k]].push_back(index[(k + 2) % 3]);
					}
				}
				if (authored) {
					for (uint32_t v = 0; v < counts.vertexCount; v++) {
						const uint8_t* n = renderer->rawVertexData + static_cast<std::size_t>(v) * stride + normalOffset;
						job->normals[v] = geometry->world.rotate * RE::NiPoint3(n[0] / 255.0f * 2.0f - 1.0f, n[1] / 255.0f * 2.0f - 1.0f, n[2] / 255.0f * 2.0f - 1.0f);
					}
				}
				for (auto& n : job->normals) {
					const float length = n.Length();
					n = length > 1e-6f ? n / length : RE::NiPoint3(0.0f, 0.0f, 1.0f);
				}
				// D109: the sheet's consistent outer side (ComputeSides); rays go against it, into what lies behind the sheet
				if (const auto outward = outwardNormals.find(geometry.get()); outward != outwardNormals.end() && outward->second.size() == job->normals.size()) {
					for (std::size_t v = 0; v < job->normals.size(); v++) {
						if (outward->second[v].Length() > 0.5f)
							job->normals[v] = outward->second[v];
					}
				}
				// D110: start where the vertex is drawn (moved by the seam gap and swell, ComposeShifts; not the wobble).
				// D112: those moves are in mesh space (b.xyz), so rotate and scale them into the world.
				for (uint32_t v = 0; v < counts.vertexCount; v++) {
					const auto& clip = vertexClips[job->base + v];
					job->positions[v] += geometry->world.rotate * (RE::NiPoint3(clip[4], clip[5], clip[6]) * geometry->world.scale);
				}
				// One shown vertex per DEPTH_CELL cell (D110: only the outer surface matters). D112: shown as the GPU draws it
				// (VertexShown: pool cut and rock too) and seen from outside (a.w's +4 marks a vertex inside another sheet's curl).
				std::unordered_set<int64_t> cells;
				for (uint32_t v = 0; v < counts.vertexCount; v++) {
					if (!VertexShown(job->base + v) || vertexClips[job->base + v][3] >= 3.5f)
						continue;
					// D111: nor the underside (the curl back under the crest, facing down): its ray went into the hollow behind the
					// curtain. Filled from the samples round it.
					if (job->normals[v].z < DEPTH_UNDERSIDE_Z)
						continue;
					const RE::NiPoint3& p = job->positions[v];
					const int64_t key = (static_cast<int64_t>(std::floor(p.x / DEPTH_CELL)) & 0x1FFFFF) << 42 | (static_cast<int64_t>(std::floor(p.y / DEPTH_CELL)) & 0x1FFFFF) << 21 |
					                    (static_cast<int64_t>(std::floor(p.z / DEPTH_CELL)) & 0x1FFFFF);
					if (cells.insert(key).second)
						job->samples.push_back(v);
				}
				job->plus.assign(counts.vertexCount, CLEAR_REACH);
				job->minus.assign(counts.vertexCount, CLEAR_REACH);
				job->known.assign(counts.vertexCount, 0);
				depthJob = std::move(job);
				continue;
			}
			auto* cell = tes->interiorCell ? tes->interiorCell : tes->GetCell(depthJob->geometry->world.translate);
			auto* bhkWorld = cell ? cell->GetbhkWorld() : nullptr;
			auto* world = bhkWorld ? bhkWorld->GetWorld1() : nullptr;
			if (!world)
				return;
			{
				const RE::BSReadLockGuard lock(bhkWorld->worldLock);
				for (int rays = 0; depthJob->next < depthJob->samples.size() && rays < 32; depthJob->next++, rays++) {
					const uint32_t v = depthJob->samples[depthJob->next];
					const RE::NiPoint3& p = depthJob->positions[v];
					const RE::NiPoint3& n = depthJob->normals[v];
					// From DEPTH_START off the visible face, back through the vertex, to the first rock or ground. D106: against the
					// authored normal (up on 75% of vertices; level on the curtains, where a miss rightly means open air or water behind).
					const RE::NiPoint3 from = p + n * DEPTH_START;
					const float hit = RockAlong(world, from, -n, CLEAR_REACH + DEPTH_START);
					depthJob->minus[v] = hit >= 0.0f ? std::clamp(hit - DEPTH_START, 0.0f, CLEAR_REACH) : CLEAR_REACH;
					depthRays++;
					depthHits += hit >= 0.0f ? 1 : 0;
					// D108: kept for the overlay (Falls Mesh Debug: Depth Rays). D111: from the vertex to the hit or the reach
					depthJob->rays.push_back({ p, p - n * (hit >= 0.0f ? std::max(hit - DEPTH_START, 0.0f) : CLEAR_REACH), RE::NiPoint3(hit >= 0.0f ? 1.0f : 0.0f, 0.0f, 0.0f) });
					// TEMPORARY D106 diagnostics: ray tilt (-n.z: 1 straight down); for a miss, a ray straight down
					depthJob->tiltSum += -n.z;                  // The ray's z: negative is down
					depthJob->downward += -n.z < 0.0f ? 1 : 0;  // Rays pointing down
					if (hit >= 0.0f) {
						depthJob->hits++;
						depthJob->hitDistances.push_back(hit - DEPTH_START);
					} else {
						depthJob->misses++;
						const float down = RockAlong(world, p + RE::NiPoint3(0.0f, 0.0f, DEPTH_START), RE::NiPoint3(0.0f, 0.0f, -1.0f), CLEAR_REACH + DEPTH_START);
						if (down >= 0.0f) {
							depthJob->missDownHits++;
							depthJob->missDown.push_back(down - DEPTH_START);
						}
					}
					depthJob->known[v] = 1;
				}
			}
			if (depthJob->next < depthJob->samples.size())
				continue;
			// Spread to the vertices between (each the mean of its neighbours, the samples held), then a light softening
			DepthJob& job = *depthJob;
			const std::size_t count = job.positions.size();
			for (auto* field : { &job.plus, &job.minus }) {
				std::vector<float> next = *field;
				for (int pass = 0; pass < DEPTH_FILL_PASSES; pass++) {
					for (std::size_t v = 0; v < count; v++) {
						if (job.known[v] || job.ring[v].empty())
							continue;
						float sum = 0.0f;
						for (const uint32_t n : job.ring[v])
							sum += (*field)[n];
						next[v] = sum / static_cast<float>(job.ring[v].size());
					}
					field->swap(next);
					next = *field;
				}
			}
			// a unified sheet: its depth behind from its offline bake (the open space behind every texel, along the normal against the
			// world's rock and land), at each vertex's uv: full resolution where the rays sampled one vertex in several
			if (unifiedShapes.contains(job.geometry.get())) {
				const auto baked = bakeFields.find(job.geometry.get());
				const UnifiedCache* cache = nullptr;
				for (const auto& [index, unified] : unifiedOf) {
					if (unified.shape.get() == job.geometry.get() && index < unifiedCaches.size())
						cache = &unifiedCaches[index];
				}
				if (baked != bakeFields.end() && baked->second && cache && cache->uvs.size() == count) {
					const BakeField& bake = *baked->second;
					const auto bw = static_cast<int32_t>(bake.width), bh = static_cast<int32_t>(bake.height);
					if (bake.clearBack.size() == static_cast<std::size_t>(bw) * bh) {
						for (std::size_t v = 0; v < count; v++) {
							const float gx = std::clamp((cache->uvs[v].x - bake.uvMin[0]) / std::max(bake.uvSize[0], 1e-6f) * static_cast<float>(bw) - 0.5f, 0.0f, static_cast<float>(bw - 1));
							const float gy = std::clamp((cache->uvs[v].y - bake.uvMin[1]) / std::max(bake.uvSize[1], 1e-6f) * static_cast<float>(bh) - 0.5f, 0.0f, static_cast<float>(bh - 1));
							const int32_t x0 = std::min(static_cast<int32_t>(gx), bw - 1), y0 = std::min(static_cast<int32_t>(gy), bh - 1);
							const int32_t x1 = std::min(x0 + 1, bw - 1), y1 = std::min(y0 + 1, bh - 1);
							const float fx = gx - static_cast<float>(x0), fy = gy - static_cast<float>(y0);
							const auto at = [&](int32_t a_x, int32_t a_y) { return bake.clearBack[static_cast<std::size_t>(a_y) * bw + a_x]; };
							job.minus[v] = (at(x0, y0) * (1.0f - fx) + at(x1, y0) * fx) * (1.0f - fy) + (at(x0, y1) * (1.0f - fx) + at(x1, y1) * fx) * fy;
						}
					}
				}
			}
			const auto base = vertexClipBase.find(job.geometry.get());
			if (base != vertexClipBase.end() && base->second == job.base && job.base + count <= vertexClips.size()) {
				uint32_t shallow = 0;
				for (std::size_t v = 0; v < count; v++) {
					vertexClips[job.base + v][13] = job.minus[v];  // (d.x is the wobble's scale, D188: ComputePushes)
					shallow += job.minus[v] < CLEAR_REACH * 0.5f ? 1 : 0;
				}
				vertexClipVersion++;
				depthRaysShown[job.geometry.get()] = std::move(job.rays);
				depthAudited = false;
				logger::info("[Fluid Field]   depth behind '{}': {} vertices sampled of {}, {} of {} rays hit rock or ground so far; {} vertices within {:.0f} units of it",
					job.geometry->name.c_str(), job.samples.size(), count, depthHits, depthRays, shallow, CLEAR_REACH * 0.5f);
				// TEMPORARY D106 diagnostics
				const auto median = [](std::vector<float> a_values) {
					if (a_values.empty())
						return -1.0f;
					std::nth_element(a_values.begin(), a_values.begin() + static_cast<std::ptrdiff_t>(a_values.size() / 2), a_values.end());
					return a_values[a_values.size() / 2];
				};
				const auto samples = std::max<std::size_t>(job.samples.size(), 1);
				logger::info("[Fluid Field]   depth rays '{}' at ({:.0f}, {:.0f}, {:.0f}): {} hit (median {:.0f} units), {} missed; rays point down on {} of {} (mean ray z {:.2f}, negative is down); of the misses, {} find ground straight down (median {:.0f} units)",
					job.geometry->name.c_str(), job.geometry->world.translate.x, job.geometry->world.translate.y, job.geometry->world.translate.z, job.hits, median(job.hitDistances), job.misses,
					job.downward, samples, job.tiltSum / static_cast<double>(samples), job.missDownHits, median(job.missDown));
			}
			depthJob.reset();
		}
	}

	void FallSites::ClipStep()
	{
		if (clipQueue.empty())
			return;
		auto* tes = RE::TES::GetSingleton();
		if (!tes)
			return;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(CLIP_BUDGET_MS);
		while (!clipQueue.empty() && std::chrono::steady_clock::now() < deadline) {
			RE::NiPointer<RE::BSGeometry> geometry = clipQueue.back();
			clipQueue.pop_back();
			auto* triShape = geometry ? geometry->AsTriShape() : nullptr;
			auto* renderer = geometry ? geometry->GetGeometryRuntimeData().rendererData : nullptr;
			if (!triShape || !renderer || !renderer->rawVertexData || !renderer->rawIndexData || !grounded.contains(geometry.get()))
				continue;
			auto* cell = tes->interiorCell ? tes->interiorCell : tes->GetCell(geometry->world.translate);
			auto* bhkWorld = cell ? cell->GetbhkWorld() : nullptr;
			auto* world = bhkWorld ? bhkWorld->GetWorld1() : nullptr;
			if (!world)
				continue;
			const auto& counts = triShape->GetTrishapeRuntimeData();
			const uint32_t stride = renderer->vertexDesc.GetSize();
			const uint32_t positionOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
			if (stride < positionOffset + 12)
				continue;
			std::vector<RE::NiPoint3> positions(counts.vertexCount);
			for (uint32_t v = 0; v < counts.vertexCount; v++) {
				float local[3];
				std::memcpy(local, renderer->rawVertexData + static_cast<std::size_t>(v) * stride + positionOffset, sizeof(local));
				positions[v] = geometry->world * RE::NiPoint3(local[0], local[1], local[2]);
			}
			// D68: the water this mesh falls into: planes under its top. A plane covers its whole bound, but below its height lies only
			// water or ground, and both hide the falls.
			float top = -FLT_MAX, bottom = FLT_MAX, lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
			for (const auto& position : positions) {
				top = std::max(top, position.z);
				bottom = std::min(bottom, position.z);
				lo[0] = std::min(lo[0], position.x);
				lo[1] = std::min(lo[1], position.y);
				hi[0] = std::max(hi[0], position.x);
				hi[1] = std::max(hi[1], position.y);
			}
			std::vector<std::array<float, 5>> planes;
			CollectWaterPlanes(lo[0], lo[1], hi[0], hi[1], planes);
			// D124: the water it pours out of, for its foam. D138: any plane near the mesh, each vertex taking the nearest above it
			// (SOURCE_DEPTH_MOST); chosen per mesh, the foam stopped dead along mesh seams.
			std::vector<std::array<float, 5>> sources;
			CollectWaterPlanes(lo[0] - SOURCE_REACH, lo[1] - SOURCE_REACH, hi[0] + SOURCE_REACH, hi[1] + SOURCE_REACH, sources);
			std::erase_if(planes, [&](const auto& a_plane) { return a_plane[4] >= top - 32.0f; });
			const auto base = static_cast<uint32_t>(vertexClips.size());
			for (uint32_t v = 0; v < counts.vertexCount; v++) {
				// D70: no per-vertex rock cut (it holed sheets where its parity misfired); rock hides water inside it anyway
				float clip = CLIP_MARGIN + CLIP_RANGE;
				float water = -FLT_MAX;
				for (const auto& plane : planes) {
					if (positions[v].x >= plane[0] && positions[v].x <= plane[2] && positions[v].y >= plane[1] && positions[v].y <= plane[3])
						water = std::max(water, plane[4]);
				}
				// D124: depth under the water it pours out of (none where it starts under another sheet, mid-fall). D174: on the upper
				// half, up to CREST_RISE under it: else a crest bulging over its river took the river for its pool (foam on the lip).
				const bool upper = positions[v].z > (top + bottom) * 0.5f;
				const float rise = upper ? CREST_RISE : 8.0f;
				float source = FLT_MAX;
				for (const auto& plane : sources) {
					if (positions[v].x >= plane[0] - SOURCE_REACH && positions[v].x <= plane[2] + SOURCE_REACH && positions[v].y >= plane[1] - SOURCE_REACH &&
						positions[v].y <= plane[3] + SOURCE_REACH && plane[4] >= positions[v].z - rise && plane[4] <= positions[v].z + SOURCE_DEPTH_MOST &&
						plane[4] >= (top + bottom) * 0.5f)  // D176: source over its middle (a short drop's foot took its pool)
						source = std::min(source, plane[4]);
				}
				// D174, D176: not the source. D139: height above the water it falls into, per vertex: the highest plane under it of all
				// planes near the mesh (chosen per mesh, the foam band stopped on a straight line). The cut below keeps the mesh's own
				// planes (D70).
				float under = -FLT_MAX;
				float over = FLT_MAX;
				for (const auto& plane : sources) {
					if (positions[v].x >= plane[0] && positions[v].x <= plane[2] && positions[v].y >= plane[1] && positions[v].y <= plane[3] && plane[4] != source) {
						if (plane[4] <= positions[v].z + 8.0f)
							under = std::max(under, plane[4]);
						else
							over = std::min(over, plane[4]);
					}
				}
				// D174: under every plane (below the pool's surface): the nearest above, negative; held at none, the waterline drew a foam
				// lattice
				if (under == -FLT_MAX && over < FLT_MAX)
					under = over;
				const float above = under > -FLT_MAX ? positions[v].z - under : NO_WATER;
				const float below = source < FLT_MAX ? std::max(source - positions[v].z, 0.0f) : NO_WATER;
				// D70: only near the mesh's foot: higher up, a plane whose bound covers the fall cut holes in the sheet
				if (water > -FLT_MAX && positions[v].z < bottom + WATER_CUT_BAND)
					clip = std::min(clip, positions[v].z - water + WATER_KEEP);
				// D71: w marks the mesh's foot, the only place the pools may cut it (the shader's pool cut)
				vertexClips.push_back({ clip, 1.0f, above, positions[v].z < bottom + WATER_CUT_BAND ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, CLEAR_REACH, 0.0f, below, NO_WATER });  // D126: d.w no other sheet near until CullStep's MeetDistance
				vertexCuts.push_back({ .water = clip });                                                                                                                                                   // D112: a.x's parts, kept apart
				clipTested++;
			}
			// D191: every vertex up to CREST_RISE above the mesh's highest source gets its depth under it, not only those within
			// SOURCE_DEPTH_MOST: otherwise the lip gate and source-pool test saw-toothed along the crest.
			{
				float meshSource = -FLT_MAX;
				for (uint32_t v = 0; v < counts.vertexCount; v++) {
					if (vertexClips[base + v][14] < NO_WATER)
						meshSource = std::max(meshSource, vertexClips[base + v][14] + positions[v].z);
				}
				if (meshSource > -FLT_MAX) {
					for (uint32_t v = 0; v < counts.vertexCount; v++) {
						auto& below = vertexClips[base + v][14];
						if (below >= NO_WATER && positions[v].z <= meshSource + CREST_RISE)
							below = std::max(meshSource - positions[v].z, 0.0f);
					}
				}
			}
			vertexClipBase[geometry.get()] = base;
			depthQueue.emplace_back(geometry);  // D106
			buryQueue.emplace_back(geometry);   // D112
			buryDone = false;
			vertexClipVersion++;
		}
		if (clipQueue.empty()) {
			logger::info("[Fluid Field] Waterfall rock cut: {} vertices of {} lit meshes tested", clipTested, vertexClipBase.size());
			// D172: on a worker (1.8 s in one frame for a cascade); Update holds everything else back meanwhile (Busy)
			heavyWork = std::async(std::launch::async, [this]() {
				const auto start = std::chrono::steady_clock::now();
				ComputeFades();
				logger::info("[Fluid Field]   seams took {:.1f} ms (worker)", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
				loadTiming.AddWorker(LoadWork::Seams, start);
			});
		}
	}

	namespace
	{
		/** @brief D62: the engine's own mesh builder (77465, 1.7.104): a BSTriShape from separate arrays; null arrays are left out. */
		RE::BSTriShape* BuildDenseShape(uint32_t a_triangles, const uint16_t* a_indices, uint32_t a_vertices, RE::NiPoint3* a_positions, RE::NiPoint2* a_uvs, RE::NiPoint3* a_normals,
			RE::NiPoint3* a_tangents, RE::NiPoint3* a_bitangents, RE::NiColorA* a_colors)
		{
			using Build_t = RE::BSTriShape* (*)(uint32_t, const uint16_t*, uint32_t, RE::NiPoint3*, RE::NiPoint2*, void*, RE::NiPoint3*, RE::NiPoint3*, RE::NiPoint3*, void*, void*, void*, void*,
				void*, void*);
			static REL::Relocation<Build_t> build{ REL::ID(77465) };
			return build(a_triangles, a_indices, a_vertices, a_positions, a_uvs, nullptr, a_normals, a_tangents, a_bitangents, a_colors, nullptr, nullptr, nullptr, nullptr, nullptr);
		}
	}

	bool FallSites::ReadDense(RE::BSGeometry* a_geometry, DenseJob& o_job) const
	{
		auto* triShape = a_geometry ? a_geometry->AsTriShape() : nullptr;
		auto* renderer = a_geometry ? a_geometry->GetGeometryRuntimeData().rendererData : nullptr;
		auto* parent = a_geometry ? a_geometry->parent : nullptr;
		auto* source = a_geometry ? a_geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
		if (!REL::Module::IsAE() || !triShape || !renderer || !renderer->rawVertexData || !renderer->rawIndexData || !parent || !source ||
			renderer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_SKINNED))
			return false;
		const auto& counts = triShape->GetTrishapeRuntimeData();
		auto& desc = renderer->vertexDesc;
		const uint32_t stride = desc.GetSize();
		const uint32_t positionOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
		const uint32_t uvOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_TEXCOORD0);
		const bool haveNormals = desc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_NORMAL);
		const uint32_t normalOffset = haveNormals ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_NORMAL) : 0;
		const bool haveColors = desc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_COLORS);
		const uint32_t colorOffset = haveColors ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_COLOR) : 0;
		if (uvOffset < positionOffset + 12 || counts.vertexCount == 0 || counts.triangleCount == 0)
			return false;
		// The original's vertices in its own space
		std::vector<RE::NiPoint3> positions(counts.vertexCount), normals(counts.vertexCount);
		std::vector<RE::NiPoint2> uvs(counts.vertexCount);
		std::vector<RE::NiColorA> colors(counts.vertexCount, RE::NiColorA(1.0f, 1.0f, 1.0f, 1.0f));
		for (uint32_t v = 0; v < counts.vertexCount; v++) {
			const uint8_t* data = renderer->rawVertexData + static_cast<std::size_t>(v) * stride;
			float local[3];
			std::memcpy(local, data + positionOffset, sizeof(local));
			positions[v] = RE::NiPoint3(local[0], local[1], local[2]);
			const auto* uv = reinterpret_cast<const uint16_t*>(data + uvOffset);
			uvs[v] = RE::NiPoint2(DirectX::PackedVector::XMConvertHalfToFloat(uv[0]), DirectX::PackedVector::XMConvertHalfToFloat(uv[1]));
			if (haveNormals) {
				const uint8_t* n = data + normalOffset;
				normals[v] = RE::NiPoint3(n[0] / 255.0f * 2.0f - 1.0f, n[1] / 255.0f * 2.0f - 1.0f, n[2] / 255.0f * 2.0f - 1.0f);
			}
			if (haveColors) {
				const uint8_t* c = data + colorOffset;
				colors[v] = RE::NiColorA(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f);
			}
		}
		std::vector<std::array<uint32_t, 3>> triangles;
		for (uint32_t i = 0; i < counts.triangleCount; i++) {
			const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(i) * 3;
			if (index[0] < counts.vertexCount && index[1] < counts.vertexCount && index[2] < counts.vertexCount)
				triangles.push_back({ index[0], index[1], index[2] });
		}
		const auto normalize = [](const RE::NiPoint3& a_v) {
			const float length = a_v.Length();
			return length > 1e-6f ? a_v / length : RE::NiPoint3(0.0f, 0.0f, 1.0f);
		};
		if (!haveNormals) {
			for (const auto& tri : triangles) {
				const RE::NiPoint3 face = (positions[tri[1]] - positions[tri[0]]).Cross(positions[tri[2]] - positions[tri[0]]);
				for (const uint32_t k : tri)
					normals[k] += face;
			}
		}
		for (auto& n : normals)
			n = normalize(n);
		// D172: what the worker needs, copied (the landscape's cells round the mesh too: the cache changes every frame)
		float lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
		for (const auto& position : positions) {
			const RE::NiPoint3 p = a_geometry->world * position;
			lo[0] = std::min(lo[0], p.x);
			lo[1] = std::min(lo[1], p.y);
			hi[0] = std::max(hi[0], p.x);
			hi[1] = std::max(hi[1], p.y);
		}
		for (int k = 0; k < 2; k++) {
			o_job.lo[k] = lo[k];
			o_job.hi[k] = hi[k];
		}
		auto* tes = RE::TES::GetSingleton();
		o_job.interior = !tes || tes->interiorCell != nullptr || !landscape;
		if (!o_job.interior)
			o_job.land = landscape->Subset(lo[0] - 512.0f, lo[1] - 512.0f, hi[0] + 512.0f, hi[1] + 512.0f);
		o_job.geometry.reset(a_geometry);
		o_job.world = a_geometry->world;
		o_job.vertexCount = counts.vertexCount;
		o_job.triangleCount = counts.triangleCount;
		o_job.haveColors = haveColors;
		o_job.positions = std::move(positions);
		o_job.normals = std::move(normals);
		o_job.uvs = std::move(uvs);
		o_job.colors = std::move(colors);
		o_job.triangles = std::move(triangles);
		return true;
	}

	void FallSites::ComputeDense(DenseJob& a_job, const ShellInput* a_solids)
	{
		// D172: on a worker (subdivision and landscape cut: 365 ms in one frame for a cascade)
		auto& positions = a_job.positions;
		auto& normals = a_job.normals;
		auto& uvs = a_job.uvs;
		auto& colors = a_job.colors;
		auto& triangles = a_job.triangles;
		const auto normalize = [](const RE::NiPoint3& a_v) {
			const float length = a_v.Length();
			return length > 1e-6f ? a_v / length : RE::NiPoint3(0.0f, 0.0f, 1.0f);
		};
		// The world scale, for the edge length
		const float scale = std::max(a_job.world.scale, 1e-3f);
		// D186: the solids round the sheet (statics' triangles binned in its own space; the landscape); edges split finer near them
		const RE::NiTransform toModel = a_job.world.Invert();
		std::vector<std::array<RE::NiPoint3, 3>> solids;
		std::unordered_map<int64_t, std::vector<uint32_t>> solidBins;
		const float bin = DENSE_NEAR_REACH * 2.0f / scale;
		const auto binKey = [](int64_t a_x, int64_t a_y, int64_t a_z) { return (a_x & 0x1FFFFF) << 42 | (a_y & 0x1FFFFF) << 21 | (a_z & 0x1FFFFF); };
		const auto cellOf = [&](float a_v) { return static_cast<int64_t>(std::floor(a_v / bin)); };
		if (a_solids) {
			float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
			for (const auto& p : positions) {
				const float c[3] = { p.x, p.y, p.z };
				for (int k = 0; k < 3; k++) {
					lo[k] = std::min(lo[k], c[k]);
					hi[k] = std::max(hi[k], c[k]);
				}
			}
			const float margin = DENSE_NEAR_REACH / scale;
			for (const auto& triangle : a_solids->occluders) {
				std::array<RE::NiPoint3, 3> local;
				float tlo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, thi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
				for (int k = 0; k < 3; k++) {
					local[k] = toModel * triangle[k];
					const float c[3] = { local[k].x, local[k].y, local[k].z };
					for (int a = 0; a < 3; a++) {
						tlo[a] = std::min(tlo[a], c[a]);
						thi[a] = std::max(thi[a], c[a]);
					}
				}
				if (thi[0] < lo[0] - margin || tlo[0] > hi[0] + margin || thi[1] < lo[1] - margin || tlo[1] > hi[1] + margin || thi[2] < lo[2] - margin || tlo[2] > hi[2] + margin)
					continue;
				const auto index = static_cast<uint32_t>(solids.size());
				solids.push_back(local);
				for (int64_t x = cellOf(tlo[0]); x <= cellOf(thi[0]); x++) {
					for (int64_t y = cellOf(tlo[1]); y <= cellOf(thi[1]); y++) {
						for (int64_t z = cellOf(tlo[2]); z <= cellOf(thi[2]); z++)
							solidBins[binKey(x, y, z)].push_back(index);
					}
				}
			}
		}
		const auto nearSolid = [&](const RE::NiPoint3& a_p) {
			// Not under or near the landscape (the sheets run along the ground most of their length): only statics count
			const RE::NiPoint3 world = a_job.world * a_p;
			float ground = 0.0f;
			if (!a_job.interior && a_job.land.SampleHeight(world.x, world.y, ground) && world.z < ground + DENSE_NEAR_REACH)
				return false;
			const float reach = DENSE_NEAR_REACH / scale;
			const int64_t cx = cellOf(a_p.x), cy = cellOf(a_p.y), cz = cellOf(a_p.z);
			for (int64_t x = cx - 1; x <= cx + 1; x++) {
				for (int64_t y = cy - 1; y <= cy + 1; y++) {
					for (int64_t z = cz - 1; z <= cz + 1; z++) {
						const auto it = solidBins.find(binKey(x, y, z));
						if (it == solidBins.end())
							continue;
						for (const uint32_t t : it->second) {
							if (TriangleDistanceSquared(a_p, solids[t][0], solids[t][1], solids[t][2]) <= reach * reach)
								return true;
						}
					}
				}
			}
			return false;
		};
		// Split until edges are short (or the budget is spent): midpoints on the PN triangle's curved edge, attributes averaged; split
		// vertices (texture seams) share the position's mean normal so curved edges meet. D91: only edges longer than DENSE_EDGE, each
		// pass (red-green: one, two or three split edges make two, three or four triangles), so triangles come out evenly sized.
		// D186: then smoothed, as PN's bulge is too slight on large, gently curved triangles and the lighting stepped in bands: Taubin's
		// two steps (in, then out, so it does not shrink) along each vertex's normal (texture coordinates stay true), open edges held, none
		// moved past DENSE_SHAPE_MOST from rest; then normals from the new faces. Run after every round of splitting (one pass at the end
		// left the larger triangles banded).
		std::vector<RE::NiPoint3> rest = positions;
		const auto smoothShape = [&](int a_passes) {
			std::map<std::tuple<int64_t, int64_t, int64_t>, uint32_t> groupAt;
			std::vector<uint32_t> groupOf(positions.size());
			for (std::size_t v = 0; v < positions.size(); v++) {
				const auto key = std::make_tuple(std::llround(positions[v].x * 20.0), std::llround(positions[v].y * 20.0), std::llround(positions[v].z * 20.0));
				groupOf[v] = groupAt.try_emplace(key, static_cast<uint32_t>(groupAt.size())).first->second;
			}
			const std::size_t groups = groupAt.size();
			std::vector<RE::NiPoint3> place(groups), along(groups, RE::NiPoint3(0.0f, 0.0f, 0.0f));
			for (std::size_t v = 0; v < positions.size(); v++) {
				place[groupOf[v]] = positions[v];
				along[groupOf[v]] += along[groupOf[v]].Dot(normals[v]) < 0.0f ? -normals[v] : normals[v];
			}
			for (auto& n : along)
				n = normalize(n);
			std::vector<std::vector<uint32_t>> neighbours(groups);
			std::map<std::pair<uint32_t, uint32_t>, int> edgeUses;
			for (const auto& tri : triangles) {
				for (int k = 0; k < 3; k++) {
					const uint32_t a = groupOf[tri[k]], b = groupOf[tri[(k + 1) % 3]];
					if (a == b)
						continue;
					neighbours[a].push_back(b);
					neighbours[b].push_back(a);
					edgeUses[std::make_pair(std::min(a, b), std::max(a, b))]++;
				}
			}
			std::vector<uint8_t> held(groups, 0);
			for (const auto& [edge, uses] : edgeUses) {
				if (uses == 1)
					held[edge.first] = held[edge.second] = 1;  // an open edge (texture seams are welded: no edge there)
			}
			std::vector<RE::NiPoint3> start(groups);
			for (std::size_t v = 0; v < positions.size(); v++)
				start[groupOf[v]] = rest[v];
			const float most = DENSE_SHAPE_MOST / scale;
			// D190: move the bend, don't remove it (Taubin averaging flattened every bump the size of the crease bands). The original's
			// faces are flat and its bend all in its creases: each vertex's bend (its neighbours' mean along its normal, over their spacing
			// squared) is spread DENSE_CURVE_SPREAD passes per pass onto the faces beside, then the shape solved to it (DENSE_CURVE_SOLVE
			// passes, open edges held).
			std::vector<float> bend(groups, 0.0f), spacing(groups, 1.0f), spread(groups, 0.0f);
			for (std::size_t g = 0; g < groups; g++) {
				if (neighbours[g].empty())
					continue;
				RE::NiPoint3 sum(0.0f, 0.0f, 0.0f);
				float squared = 0.0f;
				for (const uint32_t o : neighbours[g]) {
					sum += place[o];
					squared += (place[o] - place[g]).SqrLength();
				}
				const auto n = static_cast<float>(neighbours[g].size());
				spacing[g] = std::max(squared / n, 1e-6f);
				bend[g] = (sum / n - place[g]).Dot(along[g]) / spacing[g];
			}
			for (int pass = 0; pass < a_passes * DENSE_CURVE_SPREAD; pass++) {
				for (std::size_t g = 0; g < groups; g++) {
					spread[g] = bend[g];
					if (held[g] || neighbours[g].empty())
						continue;
					// Open edges' bend left out (neighbours on one side only); a neighbour's turned to this one's normal
					float total = bend[g], weight = 1.0f;
					for (const uint32_t o : neighbours[g]) {
						if (held[o])
							continue;
						total += along[o].Dot(along[g]) >= 0.0f ? bend[o] : -bend[o];
						weight += 1.0f;
					}
					spread[g] = total / weight;
				}
				bend.swap(spread);
			}
			std::vector<RE::NiPoint3> next(groups);
			for (int pass = 0; a_passes > 0 && pass < DENSE_CURVE_SOLVE; pass++) {
				for (std::size_t g = 0; g < groups; g++) {
					next[g] = place[g];
					if (held[g] || neighbours[g].empty())
						continue;
					RE::NiPoint3 sum(0.0f, 0.0f, 0.0f);
					for (const uint32_t o : neighbours[g])
						sum += place[o];
					// Toward the place whose neighbours' mean reaches its bend along its normal
					const float off = (sum / static_cast<float>(neighbours[g].size()) - place[g]).Dot(along[g]) - bend[g] * spacing[g];
					const float moved = (place[g] - start[g]).Dot(along[g]) + off * DENSE_CURVE_STEP;
					// D188: eased into its limit, not clamped (clamping lined the vertices up into a plateau with a fold)
					next[g] = start[g] + along[g] * (most * std::tanh(moved / most));
				}
				place.swap(next);
			}
			// D190: remove the solve's per-vertex dimples with DENSE_CURVE_TIDY Taubin pairs, too few to reach past a vertex's neighbours
			for (int pass = 0; a_passes > 0 && pass < DENSE_CURVE_TIDY * 2; pass++) {
				const float factor = pass % 2 == 0 ? 0.5f : -0.53f;
				for (std::size_t g = 0; g < groups; g++) {
					next[g] = place[g];
					if (held[g] || neighbours[g].empty())
						continue;
					RE::NiPoint3 sum(0.0f, 0.0f, 0.0f);
					for (const uint32_t o : neighbours[g])
						sum += place[o];
					next[g] = place[g] + along[g] * (factor * (sum / static_cast<float>(neighbours[g].size()) - place[g]).Dot(along[g]));
				}
				place.swap(next);
			}
			for (std::size_t v = 0; v < positions.size(); v++)
				positions[v] = place[groupOf[v]];
			// D188: relax the texture coordinates too (the original's mapping turns at its triangle edges, stepping the bake's grid); held
			// at open edges and texture seams (a position with several coordinates), never so far that a triangle flips
			{
				std::vector<RE::NiPoint2> mapped(groups);
				std::vector<uint8_t> seam(groups, 0), seen(groups, 0);
				for (std::size_t v = 0; v < uvs.size() && v < positions.size(); v++) {
					const uint32_t g = groupOf[v];
					if (seen[g] && (std::abs(mapped[g].x - uvs[v].x) > 1e-5f || std::abs(mapped[g].y - uvs[v].y) > 1e-5f))
						seam[g] = 1;
					mapped[g] = uvs[v];
					seen[g] = 1;
				}
				const auto signedArea = [&](const std::vector<RE::NiPoint2>& a_uv, const std::array<uint32_t, 3>& a_tri) {
					const RE::NiPoint2 &a = a_uv[groupOf[a_tri[0]]], &b = a_uv[groupOf[a_tri[1]]], &c = a_uv[groupOf[a_tri[2]]];
					return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
				};
				std::vector<RE::NiPoint2> moved(groups);
				for (int pass = 0; pass < DENSE_UV_PASSES; pass++) {
					for (std::size_t g = 0; g < groups; g++) {
						moved[g] = mapped[g];
						if (held[g] || seam[g] || neighbours[g].empty())
							continue;
						float sx = 0.0f, sy = 0.0f;
						for (const uint32_t o : neighbours[g]) {
							sx += mapped[o].x;
							sy += mapped[o].y;
						}
						const float n = static_cast<float>(neighbours[g].size());
						moved[g] = RE::NiPoint2(mapped[g].x + DENSE_UV_STEP * (sx / n - mapped[g].x), mapped[g].y + DENSE_UV_STEP * (sy / n - mapped[g].y));
					}
					// (a triangle whose winding in the texture would flip keeps its corners where they were)
					for (const auto& tri : triangles) {
						const float before = signedArea(mapped, tri), after = signedArea(moved, tri);
						if (before * after <= 0.0f || std::abs(after) < std::abs(before) * 0.25f) {
							for (const uint32_t k : tri)
								moved[groupOf[k]] = mapped[groupOf[k]];
						}
					}
					mapped.swap(moved);
				}
				for (std::size_t v = 0; v < uvs.size() && v < positions.size(); v++) {
					if (!seam[groupOf[v]])
						uvs[v] = mapped[groupOf[v]];
				}
			}
			// Its normals from the new shape's faces (by area), each vertex keeping its own side
			std::vector<RE::NiPoint3> faces(groups, RE::NiPoint3(0.0f, 0.0f, 0.0f));
			for (const auto& tri : triangles) {
				RE::NiPoint3 face = (positions[tri[1]] - positions[tri[0]]).Cross(positions[tri[2]] - positions[tri[0]]);
				for (const uint32_t k : tri) {
					const uint32_t g = groupOf[k];
					faces[g] += face.Dot(along[g]) < 0.0f ? -face : face;
				}
			}
			for (std::size_t v = 0; v < positions.size(); v++) {
				const RE::NiPoint3& face = faces[groupOf[v]];
				if (face.Length() > 1e-8f)
					normals[v] = face.Dot(normals[v]) >= 0.0f ? normalize(face) : -normalize(face);
			}
		};
		const auto edgeKey = [](uint32_t a_a, uint32_t a_b) { return std::make_pair(std::min(a_a, a_b), std::max(a_a, a_b)); };
		for (int level = 0; level < DENSE_LEVELS; level++) {
			std::set<std::pair<uint32_t, uint32_t>> marked;
			for (const auto& tri : triangles) {
				for (int k = 0; k < 3; k++) {
					const float length = (positions[tri[k]] - positions[tri[(k + 1) % 3]]).Length() * scale;
					if (length > DENSE_EDGE || (length > DENSE_EDGE_NEAR && positions.size() < DENSE_NEAR_BUDGET && nearSolid((positions[tri[k]] + positions[tri[(k + 1) % 3]]) * 0.5f)))
						marked.insert(edgeKey(tri[k], tri[(k + 1) % 3]));
				}
			}
			if (marked.empty() || positions.size() + marked.size() > 60000)
				break;
			std::map<std::tuple<int64_t, int64_t, int64_t>, RE::NiPoint3> welded;
			const auto weldKey = [](const RE::NiPoint3& a_p) { return std::make_tuple(std::llround(a_p.x * 20.0), std::llround(a_p.y * 20.0), std::llround(a_p.z * 20.0)); };
			for (std::size_t v = 0; v < positions.size(); v++)
				welded[weldKey(positions[v])] += normals[v];
			const auto smooth = [&](uint32_t a_v) { return normalize(welded[weldKey(positions[a_v])]); };
			std::map<std::pair<uint32_t, uint32_t>, uint32_t> middles;
			const auto middle = [&](uint32_t a_a, uint32_t a_b) {
				const auto key = std::make_pair(std::min(a_a, a_b), std::max(a_a, a_b));
				if (const auto it = middles.find(key); it != middles.end())
					return it->second;
				const RE::NiPoint3 p0 = positions[a_a], p1 = positions[a_b], n0 = smooth(a_a), n1 = smooth(a_b);
				const RE::NiPoint3 b1 = (p0 * 2.0f + p1 - n0 * (p1 - p0).Dot(n0)) / 3.0f;
				const RE::NiPoint3 b2 = (p1 * 2.0f + p0 - n1 * (p0 - p1).Dot(n1)) / 3.0f;
				positions.push_back((p0 + b1 * 3.0f + b2 * 3.0f + p1) / 8.0f);
				rest.push_back(positions.back());
				normals.push_back(normalize(normals[a_a] + normals[a_b]));
				uvs.push_back(RE::NiPoint2((uvs[a_a].x + uvs[a_b].x) * 0.5f, (uvs[a_a].y + uvs[a_b].y) * 0.5f));
				const RE::NiColorA& c0 = colors[a_a];
				const RE::NiColorA& c1 = colors[a_b];
				colors.push_back(RE::NiColorA((c0.red + c1.red) * 0.5f, (c0.green + c1.green) * 0.5f, (c0.blue + c1.blue) * 0.5f, (c0.alpha + c1.alpha) * 0.5f));
				const auto index = static_cast<uint32_t>(positions.size() - 1);
				middles[key] = index;
				return index;
			};
			std::vector<std::array<uint32_t, 3>> finer;
			finer.reserve(triangles.size() * 2);
			for (const auto& tri : triangles) {
				bool split[3];
				int splits = 0;
				for (int k = 0; k < 3; k++) {
					split[k] = marked.contains(edgeKey(tri[k], tri[(k + 1) % 3]));
					splits += split[k] ? 1 : 0;
				}
				if (splits == 0) {
					finer.push_back(tri);
				} else if (splits == 3) {
					const uint32_t ab = middle(tri[0], tri[1]), bc = middle(tri[1], tri[2]), ca = middle(tri[2], tri[0]);
					finer.push_back({ tri[0], ab, ca });
					finer.push_back({ ab, tri[1], bc });
					finer.push_back({ ca, bc, tri[2] });
					finer.push_back({ ab, bc, ca });
				} else if (splits == 1) {
					// Turned so the split edge is (a, b): two triangles, winding kept
					const int k = split[0] ? 0 : (split[1] ? 1 : 2);
					const uint32_t a = tri[k], b = tri[(k + 1) % 3], cc = tri[(k + 2) % 3];
					const uint32_t m = middle(a, b);
					finer.push_back({ a, m, cc });
					finer.push_back({ m, b, cc });
				} else {
					// Turned so the edge left whole is (c, a): the corner at b, and the quad split by its shorter diagonal
					const int k = !split[2] ? 0 : (!split[0] ? 1 : 2);
					const uint32_t a = tri[k], b = tri[(k + 1) % 3], cc = tri[(k + 2) % 3];
					const uint32_t mab = middle(a, b), mbc = middle(b, cc);
					finer.push_back({ mab, b, mbc });
					if ((positions[a] - positions[mbc]).Length() <= (positions[mab] - positions[cc]).Length()) {
						finer.push_back({ a, mab, mbc });
						finer.push_back({ a, mbc, cc });
					} else {
						finer.push_back({ a, mab, cc });
						finer.push_back({ mab, mbc, cc });
					}
				}
			}
			triangles.swap(finer);
			smoothShape(DENSE_SHAPE_LEVEL_PASSES);
		}
		if (positions.size() > 65535)
			return;
		smoothShape(DENSE_SHAPE_PASSES);
		// D186: smooth the normals over the copy (else hard lines in the light along the original's long edges and texture seams):
		// each position's vertices share one, softened DENSE_NORMAL_PASSES times (about 4 edges' reach each way)
		{
			std::map<std::tuple<int64_t, int64_t, int64_t>, uint32_t> groupAt;
			std::vector<uint32_t> groupOf(positions.size());
			for (std::size_t v = 0; v < positions.size(); v++) {
				const auto key = std::make_tuple(std::llround(positions[v].x * 20.0), std::llround(positions[v].y * 20.0), std::llround(positions[v].z * 20.0));
				groupOf[v] = groupAt.try_emplace(key, static_cast<uint32_t>(groupAt.size())).first->second;
			}
			std::vector<RE::NiPoint3> current(groupAt.size(), RE::NiPoint3(0.0f, 0.0f, 0.0f));
			for (std::size_t v = 0; v < positions.size(); v++)
				current[groupOf[v]] += normals[v];
			std::vector<std::vector<uint32_t>> neighbours(groupAt.size());
			for (const auto& tri : triangles) {
				for (int k = 0; k < 3; k++) {
					const uint32_t a = groupOf[tri[k]], b = groupOf[tri[(k + 1) % 3]];
					if (a != b) {
						neighbours[a].push_back(b);
						neighbours[b].push_back(a);
					}
				}
			}
			for (auto& n : current)
				n = normalize(n);
			std::vector<RE::NiPoint3> next(current.size());
			for (int pass = 0; pass < DENSE_NORMAL_PASSES; pass++) {
				for (std::size_t g = 0; g < current.size(); g++) {
					RE::NiPoint3 sum(0.0f, 0.0f, 0.0f);
					for (const uint32_t o : neighbours[g])
						sum += current[o];
					next[g] = neighbours[g].empty() ? current[g] : normalize(current[g] + sum / static_cast<float>(neighbours[g].size()));
				}
				current.swap(next);
			}
			for (std::size_t v = 0; v < positions.size(); v++) {
				const RE::NiPoint3& n = current[groupOf[v]];
				// (each vertex keeps its own side: a sheet's two faces may share a position with opposite normals)
				normals[v] = normals[v].Dot(n) >= 0.0f ? n : -n;
			}
		}
		// D172: cut triangles away under the landscape (from DENSE_TRIM_UNDER below it, so the cut stays hidden), splitting those crossing
		// it. All vertices are kept, new ones appended, so the bake reads the untrimmed triangles over the same vertices (BakeIndices,
		// D136: it needs the whole sheet). The landscape comes from Fluid Field's cache; nothing is cast for it (BuryStep).
		a_job.untrimmed = triangles;
		uint32_t& trimmedAway = a_job.trimmedAway;
		uint32_t& trimmedSplit = a_job.trimmedSplit;
		if (!a_job.interior) {
			const RE::NiTransform& world = a_job.world;
			// Over the landscape (world units, from DENSE_TRIM_UNDER below it); FLT_MAX where its height is not known
			const auto over = [&](const RE::NiPoint3& a_local) {
				const RE::NiPoint3 p = world * a_local;
				float ground = 0.0f;
				return a_job.land.SampleHeight(p.x, p.y, ground) ? p.z - (ground - DENSE_TRIM_UNDER) : FLT_MAX;
			};
			std::vector<float> height(positions.size());
			for (std::size_t v = 0; v < positions.size(); v++)
				height[v] = over(positions[v]);
			std::map<std::pair<uint32_t, uint32_t>, uint32_t> crossings;
			// Where an edge crosses the cut: bisected against the landscape itself (the ground bends within an edge), attributes
			// interpolated
			const auto crossing = [&](uint32_t a_over, uint32_t a_under) {
				const auto key = std::make_pair(a_over, a_under);
				if (const auto it = crossings.find(key); it != crossings.end())
					return it->second;
				float lo = 0.0f, hi = 1.0f;
				for (int step = 0; step < 12; step++) {
					const float mid = 0.5f * (lo + hi);
					if (over(positions[a_over] + (positions[a_under] - positions[a_over]) * mid) >= 0.0f)
						lo = mid;
					else
						hi = mid;
				}
				const float f = lo;
				positions.push_back(positions[a_over] + (positions[a_under] - positions[a_over]) * f);
				normals.push_back(normalize(normals[a_over] * (1.0f - f) + normals[a_under] * f));
				uvs.push_back(RE::NiPoint2(uvs[a_over].x + (uvs[a_under].x - uvs[a_over].x) * f, uvs[a_over].y + (uvs[a_under].y - uvs[a_over].y) * f));
				const RE::NiColorA& c0 = colors[a_over];
				const RE::NiColorA& c1 = colors[a_under];
				colors.push_back(RE::NiColorA(c0.red + (c1.red - c0.red) * f, c0.green + (c1.green - c0.green) * f, c0.blue + (c1.blue - c0.blue) * f, c0.alpha + (c1.alpha - c0.alpha) * f));
				height.push_back(0.0f);
				const auto index = static_cast<uint32_t>(positions.size() - 1);
				crossings[key] = index;
				return index;
			};
			std::vector<std::array<uint32_t, 3>> kept;
			kept.reserve(triangles.size());
			for (const auto& tri : triangles) {
				const bool up[3] = { height[tri[0]] >= 0.0f, height[tri[1]] >= 0.0f, height[tri[2]] >= 0.0f };
				const int ups = (up[0] ? 1 : 0) + (up[1] ? 1 : 0) + (up[2] ? 1 : 0);
				if (ups == 3) {
					kept.push_back(tri);
				} else if (ups == 0) {
					trimmedAway++;
				} else if (ups == 1) {
					// Turned so the one over it is a: what is left of the triangle is a triangle, winding kept
					const int k = up[0] ? 0 : (up[1] ? 1 : 2);
					const uint32_t a = tri[k], b = tri[(k + 1) % 3], c = tri[(k + 2) % 3];
					kept.push_back({ a, crossing(a, b), crossing(a, c) });
					trimmedSplit++;
				} else {
					// Turned so the one under it is c: what is left is a quad, two triangles
					const int k = !up[0] ? 2 : (!up[1] ? 0 : 1);
					const uint32_t a = tri[k], b = tri[(k + 1) % 3], c = tri[(k + 2) % 3];
					const uint32_t bc = crossing(b, c), ac = crossing(a, c);
					kept.push_back({ a, b, bc });
					kept.push_back({ a, bc, ac });
					trimmedSplit++;
				}
			}
			if (positions.size() > 65535)
				return;
			triangles.swap(kept);
		}
		// Tangents from the texture coordinates
		std::vector<RE::NiPoint3> tangents(positions.size(), RE::NiPoint3(0.0f, 0.0f, 0.0f)), bitangents(positions.size(), RE::NiPoint3(0.0f, 0.0f, 0.0f));
		for (const auto& tri : triangles) {
			const RE::NiPoint3 e1 = positions[tri[1]] - positions[tri[0]], e2 = positions[tri[2]] - positions[tri[0]];
			const float du1 = uvs[tri[1]].x - uvs[tri[0]].x, dv1 = uvs[tri[1]].y - uvs[tri[0]].y;
			const float du2 = uvs[tri[2]].x - uvs[tri[0]].x, dv2 = uvs[tri[2]].y - uvs[tri[0]].y;
			const float det = du1 * dv2 - du2 * dv1;
			if (std::abs(det) < 1e-10f)
				continue;
			const RE::NiPoint3 tu = (e1 * dv2 - e2 * dv1) / det, tv = (e2 * du1 - e1 * du2) / det;
			for (const uint32_t k : tri) {
				tangents[k] += tu;
				bitangents[k] += tv;
			}
		}
		for (std::size_t v = 0; v < positions.size(); v++) {
			const RE::NiPoint3& n = normals[v];
			RE::NiPoint3 tangent = tangents[v] - n * n.Dot(tangents[v]);
			tangent = tangent.Length() > 1e-6f ? normalize(tangent) : normalize(n.Cross(RE::NiPoint3(0.0f, 0.0f, 1.0f)).Length() > 1e-3f ? n.Cross(RE::NiPoint3(0.0f, 0.0f, 1.0f)) : RE::NiPoint3(1.0f, 0.0f, 0.0f));
			RE::NiPoint3 bitangent = n.Cross(tangent);
			if (bitangent.Dot(bitangents[v]) < 0.0f)
				bitangent = -bitangent;
			tangents[v] = tangent;
			bitangents[v] = bitangent;
		}
		// D186: smooth the tangents like the normals (taken from the original's texture coordinates, they turned per triangle and the
		// normal maps creased at every edge): shared per position, softened DENSE_NORMAL_PASSES times, kept across the normal
		{
			std::map<std::tuple<int64_t, int64_t, int64_t>, uint32_t> groupAt;
			std::vector<uint32_t> groupOf(positions.size());
			for (std::size_t v = 0; v < positions.size(); v++) {
				const auto key = std::make_tuple(std::llround(positions[v].x * 20.0), std::llround(positions[v].y * 20.0), std::llround(positions[v].z * 20.0));
				groupOf[v] = groupAt.try_emplace(key, static_cast<uint32_t>(groupAt.size())).first->second;
			}
			std::vector<RE::NiPoint3> current(groupAt.size(), RE::NiPoint3(0.0f, 0.0f, 0.0f)), across(groupAt.size(), RE::NiPoint3(0.0f, 0.0f, 0.0f));
			for (std::size_t v = 0; v < positions.size(); v++) {
				current[groupOf[v]] += tangents[v];
				across[groupOf[v]] += normals[v];
			}
			std::vector<std::vector<uint32_t>> neighbours(groupAt.size());
			for (const auto& tri : a_job.untrimmed.empty() ? triangles : a_job.untrimmed) {
				for (int k = 0; k < 3; k++) {
					const uint32_t a = groupOf[tri[k]], b = groupOf[tri[(k + 1) % 3]];
					if (a != b) {
						neighbours[a].push_back(b);
						neighbours[b].push_back(a);
					}
				}
			}
			const auto flatten = [&](std::size_t a_g, const RE::NiPoint3& a_t) {
				const RE::NiPoint3 n = normalize(across[a_g]);
				const RE::NiPoint3 t = a_t - n * n.Dot(a_t);
				return t.Length() > 1e-6f ? normalize(t) : current[a_g];
			};
			for (std::size_t g = 0; g < current.size(); g++)
				current[g] = flatten(g, current[g]);
			std::vector<RE::NiPoint3> next(current.size());
			for (int pass = 0; pass < DENSE_NORMAL_PASSES; pass++) {
				for (std::size_t g = 0; g < current.size(); g++) {
					RE::NiPoint3 sum(0.0f, 0.0f, 0.0f);
					for (const uint32_t o : neighbours[g])
						sum += current[o];
					next[g] = neighbours[g].empty() ? current[g] : flatten(g, current[g] + sum / static_cast<float>(neighbours[g].size()));
				}
				current.swap(next);
			}
			for (std::size_t v = 0; v < positions.size(); v++) {
				const RE::NiPoint3& n = normals[v];
				RE::NiPoint3 t = current[groupOf[v]] - n * n.Dot(current[groupOf[v]]);
				if (t.Length() < 1e-6f)
					continue;
				t = normalize(t);
				RE::NiPoint3 b = n.Cross(t);
				if (b.Dot(bitangents[v]) < 0.0f)
					b = -b;
				tangents[v] = t;
				bitangents[v] = b;
			}
		}
		std::vector<uint16_t> indices;
		indices.reserve(triangles.size() * 3);
		for (const auto& tri : triangles) {
			for (const uint32_t k : tri)
				indices.push_back(static_cast<uint16_t>(k));
		}
		a_job.tangents = std::move(tangents);
		a_job.bitangents = std::move(bitangents);
		a_job.indices = std::move(indices);
		a_job.ok = true;
	}

	bool FallSites::AttachDense(DenseJob& a_job)
	{
		// D172: the engine's shape, on the render thread, from the worker's geometry (if the original is still where it was)
		RE::BSGeometry* a_geometry = a_job.geometry.get();
		auto* parent = a_geometry ? a_geometry->parent : nullptr;
		auto* source = a_geometry ? a_geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
		if (!a_job.ok || !a_geometry || !parent || !source || denseOf.contains(a_geometry))
			return false;
		auto& positions = a_job.positions;
		auto& normals = a_job.normals;
		auto& uvs = a_job.uvs;
		auto& colors = a_job.colors;
		auto& tangents = a_job.tangents;
		auto& bitangents = a_job.bitangents;
		auto& triangles = a_job.triangles;
		auto& indices = a_job.indices;
		auto& untrimmed = a_job.untrimmed;
		const bool haveColors = a_job.haveColors;
		const uint32_t trimmedAway = a_job.trimmedAway, trimmedSplit = a_job.trimmedSplit;
		const struct
		{
			uint32_t vertexCount, triangleCount;
		} counts{ a_job.vertexCount, a_job.triangleCount };
		RE::BSTriShape* shape = BuildDenseShape(static_cast<uint32_t>(triangles.size()), indices.data(), static_cast<uint32_t>(positions.size()), positions.data(), uvs.data(), normals.data(),
			tangents.data(), bitangents.data(), haveColors ? colors.data() : nullptr);
		if (!shape)
			return false;
		RE::NiPointer<RE::BSTriShape> held(shape);
		shape->name = a_geometry->name;
		shape->local = a_geometry->local;
		RE::NiCloningProcess cloning;
		auto* copy = static_cast<RE::BSShaderProperty*>(source->CreateClone(cloning));
		if (!copy)
			return false;
		shape->GetGeometryRuntimeData().shaderProperty.reset(copy);
		shape->GetGeometryRuntimeData().alphaProperty = a_geometry->GetGeometryRuntimeData().alphaProperty;
		// D91 experiment: blended alpha before setup, so the copy goes to the blended pass from the start (D53-D56 switched live
		// geometry, which stayed registered as opaque and drew nothing)
		if (blendedCopies) {
			if (auto* blended = MakeBlendedAlpha(a_geometry)) {
				shape->GetGeometryRuntimeData().alphaProperty.reset(blended);
				logger::info("[Fluid Field]   dense copy of '{}' blended (alpha flags {:04X})", a_geometry->name.c_str(), blended->alphaFlags);
			} else {
				logger::info("[Fluid Field]   dense copy of '{}': no alpha property to borrow, stays opaque", a_geometry->name.c_str());
			}
		}
		copy->SetupGeometry(shape);
		copy->FinishSetupGeometry(shape);
		parent->AttachChild(shape, true);
		RE::NiUpdateData update;
		shape->Update(update);
		a_geometry->SetAppCulled(true);
		denseOf[a_geometry] = { held, RE::NiPointer<RE::BSGeometry>(a_geometry), RE::NiPointer<RE::NiNode>(parent) };
		denseShapes.insert(shape);
		// D172: the bake's whole sheet
		auto& whole = untrimmedIndices[shape];
		whole.clear();
		whole.reserve(untrimmed.size() * 3);
		for (const auto& tri : untrimmed) {
			for (const uint32_t k : tri)
				whole.push_back(static_cast<uint16_t>(k));
		}
		logger::info("[Fluid Field]   dense copy of '{}': {} to {} vertices, {} to {} triangles ({} under the landscape cut away, {} cut along it, {} seen from nowhere)",
			a_geometry->name.c_str(), counts.vertexCount, positions.size(), counts.triangleCount, triangles.size(), trimmedAway, trimmedSplit, a_job.hiddenAway);
		return true;
	}

	RE::NiAlphaProperty* FallSites::MakeBlendedAlpha(RE::BSGeometry* a_geometry) const
	{
		// The lit surface has no alpha property (D53); its model's effect layers carry 0x10ED. Search from its parent up to the model's
		// root
		RE::NiAlphaProperty* source = nullptr;
		RE::NiNode* node = a_geometry->parent;
		for (int up = 0; up < 3 && node && !source; up++, node = node->parent) {
			RE::BSVisit::TraverseScenegraphGeometries(node, [&](RE::BSGeometry* a_other) {
				auto* alpha = a_other != a_geometry ? a_other->GetGeometryRuntimeData().alphaProperty.get() : nullptr;
				if (alpha && alpha->GetAlphaBlending()) {
					source = alpha;
					return RE::BSVisit::BSVisitControl::kStop;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}
		if (!source)
			source = a_geometry->GetGeometryRuntimeData().alphaProperty.get();
		if (!source)
			return nullptr;
		// A clone, so the effect layers' own property is never changed
		RE::NiCloningProcess cloning;
		auto* clone = static_cast<RE::NiAlphaProperty*>(source->CreateClone(cloning));
		if (!clone)
			return nullptr;
		clone->alphaFlags = BLENDED_ALPHA_FLAGS;
		clone->alphaThreshold = 0;
		return clone;
	}

	void FallSites::LoadUnifiedCaches()
	{
		// Layout (little-endian), "UNFS" v2 (tools/unify/site_cache.py): member, vertex and triangle counts; per member its plugin
		// (length, name) and local form ID and position; per vertex position, normal (f32 x 3 each), uv (f32 x 2), flags (u8),
		// colour (u8 x 4: the members' water tint); the triangles (u16 x 3)
		unifiedLoaded = true;
		unifiedCaches.clear();
		const auto directory = Util::PathHelpers::GetDataPath() / "SKSE" / "Plugins" / "CommunityShaders" / "FluidField" / "Sites";
		std::error_code error;
		if (!std::filesystem::is_directory(directory, error))
			return;
		auto* handler = RE::TESDataHandler::GetSingleton();
		for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
			if (entry.path().extension() != ".unfs")
				continue;
			std::ifstream file(entry.path(), std::ios::binary);
			uint32_t header[5]{};
			if (!file.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] != 0x53464E55 || header[1] != 2 || header[3] == 0 || header[3] > 65535) {
				logger::warn("[Fluid Field] Unified falls: '{}' is not a site cache this version reads", entry.path().filename().string());
				continue;
			}
			UnifiedCache cache;
			cache.path = entry.path().filename().string();
			bool resolved = true;
			for (uint32_t m = 0; m < header[2]; m++) {
				uint8_t length = 0;
				file.read(reinterpret_cast<char*>(&length), 1);
				std::string plugin(length, '\0');
				file.read(plugin.data(), length);
				uint32_t local = 0;
				float at[3]{};
				file.read(reinterpret_cast<char*>(&local), 4);
				file.read(reinterpret_cast<char*>(at), sizeof(at));
				const RE::FormID id = handler ? handler->LookupFormID(local, plugin) : 0;
				resolved = resolved && id != 0;
				cache.members.push_back(id);
			}
			const uint32_t vertices = header[3], triangles = header[4];
			cache.positions.resize(vertices);
			cache.normals.resize(vertices);
			cache.uvs.resize(vertices);
			cache.flags.resize(vertices);
			cache.colors.resize(vertices);
			for (uint32_t v = 0; v < vertices; v++) {
				float data[8]{};
				uint8_t color[4]{};
				file.read(reinterpret_cast<char*>(data), sizeof(data));
				file.read(reinterpret_cast<char*>(&cache.flags[v]), 1);
				file.read(reinterpret_cast<char*>(color), sizeof(color));
				cache.colors[v] = RE::NiColorA(color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, color[3] / 255.0f);
				cache.positions[v] = { data[0], data[1], data[2] };
				cache.normals[v] = { data[3], data[4], data[5] };
				cache.uvs[v] = { data[6], data[7] };
			}
			cache.indices.resize(static_cast<std::size_t>(triangles) * 3);
			file.read(reinterpret_cast<char*>(cache.indices.data()), static_cast<std::streamsize>(cache.indices.size()) * sizeof(uint16_t));
			if (!file || !resolved) {
				logger::warn("[Fluid Field] Unified falls: '{}' {}", cache.path, !file ? "is cut short" : "names a reference this load order lacks");
				continue;
			}
			// Its flow bake, baked offline in the game's bake format (tools/unify/bake_site.py), beside it
			auto bakePath = entry.path();
			bakePath.replace_extension(".ffbk");
			cache.bake = std::filesystem::exists(bakePath, error) ? LoadBake(bakePath) : nullptr;
			cache.bakePath = bakePath;
			if (cache.bake)
				cache.bakeTime = std::filesystem::last_write_time(bakePath, error);
			logger::info("[Fluid Field] Unified falls: site cache '{}': {} members, {} vertices, {} triangles; flow bake {}", cache.path, cache.members.size(), vertices, triangles,
				cache.bake ? std::format("{}x{} texels, baked offline", cache.bake->width, cache.bake->height) : std::string("baked here"));
			unifiedCaches.push_back(std::move(cache));
		}
	}

	void FallSites::ReloadChangedBakes()
	{
		// Once a second, and only while no worker reads the bakes
		const auto now = std::chrono::steady_clock::now();
		if (now - bakeCheckedAt < std::chrono::seconds(1))
			return;
		bakeCheckedAt = now;
		for (std::size_t c = 0; c < unifiedCaches.size(); c++) {
			auto& cache = unifiedCaches[c];
			std::error_code error;
			const auto time = cache.bakePath.empty() ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(cache.bakePath, error);
			if (error || time == cache.bakeTime)
				continue;
			auto field = LoadBake(cache.bakePath);
			if (!field)
				continue;  // still being written: tried again next second
			cache.bake = std::move(field);
			cache.bakeTime = time;
			logger::info("[Fluid Field] Unified falls: '{}' bake reloaded ({}x{} texels)", cache.path, cache.bake->width, cache.bake->height);
			const auto found = unifiedOf.find(c);
			if (found == unifiedOf.end() || !found->second.shape)
				continue;
			RE::BSGeometry* sheet = found->second.shape.get();
			bakeFields[sheet] = cache.bake;
			linkDue = true;
			if (std::none_of(depthQueue.begin(), depthQueue.end(), [&](const RE::NiPointer<RE::BSGeometry>& a_queued) { return a_queued.get() == sheet; }))
				depthQueue.emplace_back(sheet);
		}
	}

	RE::BSTriShape* FallSites::AttachUnified(std::size_t a_cache, RE::NiNode* a_root, RE::BSGeometry* a_material)
	{
		// As AttachDense (the engine's builder, a cloned lit material), the sheet's world positions taken into a_root's space
		const UnifiedCache& cache = unifiedCaches[a_cache];
		auto* source = a_material ? a_material->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
		if (!a_root || !source)
			return nullptr;
		const RE::NiTransform toLocal = a_root->world.Invert();
		const std::size_t count = cache.positions.size();
		std::vector<RE::NiPoint3> positions(count), normals(count), tangents(count, RE::NiPoint3(0.0f, 0.0f, 0.0f)), bitangents(count, RE::NiPoint3(0.0f, 0.0f, 0.0f));
		std::vector<RE::NiPoint2> uvs = cache.uvs;
		std::vector<RE::NiColorA> colors = cache.colors;
		for (std::size_t v = 0; v < count; v++) {
			positions[v] = toLocal * cache.positions[v];
			RE::NiPoint3 n = toLocal.rotate * cache.normals[v];
			const float length = n.Length();
			normals[v] = length > 1e-6f ? n / length : RE::NiPoint3(0.0f, 0.0f, 1.0f);
		}
		// Tangents from the uv chart, kept across the normal
		for (std::size_t i = 0; i + 2 < cache.indices.size(); i += 3) {
			const uint16_t a = cache.indices[i], b = cache.indices[i + 1], c = cache.indices[i + 2];
			const RE::NiPoint3 e1 = positions[b] - positions[a], e2 = positions[c] - positions[a];
			const float du1 = uvs[b].x - uvs[a].x, dv1 = uvs[b].y - uvs[a].y, du2 = uvs[c].x - uvs[a].x, dv2 = uvs[c].y - uvs[a].y;
			const float det = du1 * dv2 - du2 * dv1;
			if (std::abs(det) < 1e-12f)
				continue;
			const RE::NiPoint3 tu = (e1 * dv2 - e2 * dv1) / det, tv = (e2 * du1 - e1 * du2) / det;
			for (const uint16_t k : { a, b, c }) {
				tangents[k] += tu;
				bitangents[k] += tv;
			}
		}
		for (std::size_t v = 0; v < count; v++) {
			const RE::NiPoint3& n = normals[v];
			RE::NiPoint3 t = tangents[v] - n * n.Dot(tangents[v]);
			if (t.Length() < 1e-6f)
				t = n.Cross(std::abs(n.z) < 0.9f ? RE::NiPoint3(0.0f, 0.0f, 1.0f) : RE::NiPoint3(1.0f, 0.0f, 0.0f));
			t = t / t.Length();
			RE::NiPoint3 bt = n.Cross(t);
			if (bt.Dot(bitangents[v]) < 0.0f)
				bt = -bt;
			tangents[v] = t;
			bitangents[v] = bt;
		}
		RE::BSTriShape* shape = BuildDenseShape(static_cast<uint32_t>(cache.indices.size() / 3), cache.indices.data(), static_cast<uint32_t>(count), positions.data(), uvs.data(), normals.data(),
			tangents.data(), bitangents.data(), colors.data());
		if (!shape)
			return nullptr;
		RE::NiPointer<RE::BSTriShape> held(shape);
		shape->name = std::format("FluidFieldUnified {}", cache.path).c_str();
		RE::NiCloningProcess cloning;
		auto* copy = static_cast<RE::BSShaderProperty*>(source->CreateClone(cloning));
		if (!copy)
			return nullptr;
		shape->GetGeometryRuntimeData().shaderProperty.reset(copy);
		shape->GetGeometryRuntimeData().alphaProperty = a_material->GetGeometryRuntimeData().alphaProperty;
		copy->SetupGeometry(shape);
		copy->FinishSetupGeometry(shape);
		a_root->AttachChild(shape, true);
		RE::NiUpdateData update;
		shape->Update(update);
		unifiedOf[a_cache] = { held, RE::NiPointer<RE::NiNode>(a_root), {} };
		unifiedShapes.insert(shape);
		logger::info("[Fluid Field] Unified falls: site '{}' drawn as one sheet ({} vertices, {} triangles)", cache.path, count, cache.indices.size() / 3);
		return shape;
	}

	void FallSites::RemoveUnified()
	{
		for (auto& [index, unified] : unifiedOf) {
			if (unified.parent && unified.shape)
				unified.parent->DetachChild(unified.shape.get());
			for (auto& original : unified.hidden) {
				if (original)
					original->SetAppCulled(false);
			}
		}
		unifiedOf.clear();
		unifiedShapes.clear();
	}

	void FallSites::SetUnifiedFalls(bool a_on)
	{
		if (a_on == unifiedFalls)
			return;
		unifiedFalls = a_on;
		RemoveUnified();
		unifiedLoaded = false;  // turned on again: the site caches read anew (they may have been rebuilt)
		RefreshGrounded();
	}

	void FallSites::RemoveDense()
	{
		for (auto& [original, copy] : denseOf) {
			if (copy.parent && copy.shape)
				copy.parent->DetachChild(copy.shape.get());
			if (copy.original)
				copy.original->SetAppCulled(false);
		}
		denseOf.clear();
		denseShapes.clear();
		untrimmedIndices.clear();  // D172
	}

	void FallSites::ComputeFades()
	{
		// Every tested lit mesh in the world: its vertices, triangles, open edges' vertices, extent, top and area
		struct Mesh
		{
			const RE::BSGeometry* geometry = nullptr;
			uint32_t base = 0;
			std::vector<std::array<uint16_t, 2>> openEdges;
			std::vector<RE::NiPoint3> positions;
			std::vector<std::array<uint16_t, 3>> triangles;
			std::vector<uint8_t> open;
			float lo[3]{ FLT_MAX, FLT_MAX, FLT_MAX }, hi[3]{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
			float top = -FLT_MAX, area = 0.0f;
			std::unordered_map<int64_t, std::vector<uint32_t>> bins;  ///< D91: triangles per SEAM_BIN cell their bound touches
		};
		const auto binKey = [](int64_t a_x, int64_t a_y, int64_t a_z) { return (a_x & 0x1FFFFF) << 42 | (a_y & 0x1FFFFF) << 21 | (a_z & 0x1FFFFF); };
		const auto cellOf = [](float a_v) { return static_cast<int64_t>(std::floor(a_v / SEAM_BIN)); };
		// The triangles of a mesh within a_reach of a point (a superset): those in the cells round it
		const auto nearby = [&](const auto& a_mesh, const RE::NiPoint3& a_p, float a_reach, const auto& a_visit) {
			const int64_t x0 = cellOf(a_p.x - a_reach), x1 = cellOf(a_p.x + a_reach), y0 = cellOf(a_p.y - a_reach), y1 = cellOf(a_p.y + a_reach), z0 = cellOf(a_p.z - a_reach),
						  z1 = cellOf(a_p.z + a_reach);
			for (int64_t x = x0; x <= x1; x++) {
				for (int64_t y = y0; y <= y1; y++) {
					for (int64_t z = z0; z <= z1; z++) {
						const auto it = a_mesh.bins.find(binKey(x, y, z));
						if (it == a_mesh.bins.end())
							continue;
						for (const uint32_t t : it->second) {
							if (a_visit(a_mesh.triangles[t]))
								return;
						}
					}
				}
			}
		};
		std::vector<Mesh> meshes;
		std::unordered_set<const RE::BSGeometry*> seen;
		for (const auto& lit : litMeshList) {
			auto* geometry = lit.geometry.get();
			const auto base = vertexClipBase.find(geometry);
			if (!geometry || base == vertexClipBase.end() || !seen.insert(geometry).second)
				continue;
			auto* triShape = geometry->AsTriShape();
			auto* renderer = geometry->GetGeometryRuntimeData().rendererData;
			if (!triShape || !renderer || !renderer->rawVertexData || !renderer->rawIndexData)
				continue;
			const auto& counts = triShape->GetTrishapeRuntimeData();
			const uint32_t stride = renderer->vertexDesc.GetSize();
			const uint32_t positionOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
			if (base->second + counts.vertexCount > vertexClips.size())
				continue;
			Mesh mesh;
			mesh.geometry = geometry;
			mesh.base = base->second;
			mesh.positions.resize(counts.vertexCount);
			for (uint32_t v = 0; v < counts.vertexCount; v++) {
				float local[3];
				std::memcpy(local, renderer->rawVertexData + static_cast<std::size_t>(v) * stride + positionOffset, sizeof(local));
				mesh.positions[v] = geometry->world * RE::NiPoint3(local[0], local[1], local[2]);
				const float c[3] = { mesh.positions[v].x, mesh.positions[v].y, mesh.positions[v].z };
				for (int k = 0; k < 3; k++) {
					mesh.lo[k] = std::min(mesh.lo[k], c[k]);
					mesh.hi[k] = std::max(mesh.hi[k], c[k]);
				}
				mesh.top = std::max(mesh.top, mesh.positions[v].z);
			}
			std::map<std::pair<uint16_t, uint16_t>, int> uses;
			for (uint32_t i = 0; i < counts.triangleCount; i++) {
				const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(i) * 3;
				if (index[0] >= counts.vertexCount || index[1] >= counts.vertexCount || index[2] >= counts.vertexCount)
					continue;
				mesh.triangles.push_back({ index[0], index[1], index[2] });
				mesh.area += 0.5f * (mesh.positions[index[1]] - mesh.positions[index[0]]).Cross(mesh.positions[index[2]] - mesh.positions[index[0]]).Length();
				for (int k = 0; k < 3; k++)
					uses[{ std::min(index[k], index[(k + 1) % 3]), std::max(index[k], index[(k + 1) % 3]) }]++;
			}
			for (uint32_t t = 0; t < mesh.triangles.size(); t++) {
				const auto& tri = mesh.triangles[t];
				const RE::NiPoint3 &a = mesh.positions[tri[0]], &b = mesh.positions[tri[1]], &cc = mesh.positions[tri[2]];
				for (int64_t x = cellOf(std::min({ a.x, b.x, cc.x })); x <= cellOf(std::max({ a.x, b.x, cc.x })); x++) {
					for (int64_t y = cellOf(std::min({ a.y, b.y, cc.y })); y <= cellOf(std::max({ a.y, b.y, cc.y })); y++) {
						for (int64_t z = cellOf(std::min({ a.z, b.z, cc.z })); z <= cellOf(std::max({ a.z, b.z, cc.z })); z++)
							mesh.bins[binKey(x, y, z)].push_back(t);
					}
				}
			}
			mesh.open.assign(counts.vertexCount, 0);
			for (const auto& [edge, count] : uses) {
				if (count != 1)
					continue;
				mesh.open[edge.first] = mesh.open[edge.second] = 1;
				mesh.openEdges.push_back({ edge.first, edge.second });
			}
			meshes.push_back(std::move(mesh));
		}
		// An open-edge vertex on a leading mesh (higher, or as high and larger) fades, so that mesh shows through instead of a hard seam
		const auto leads = [&](const Mesh& a_lead, const Mesh& a_mesh) {
			return a_lead.top > a_mesh.top + 8.0f || (std::abs(a_lead.top - a_mesh.top) <= 8.0f && a_lead.area > a_mesh.area);
		};
		uint32_t faded = 0, edges = 0, gaps = 0, smoothed = 0;
		for (const auto& mesh : meshes) {
			// Which vertices lie on a leading mesh. D91: binned, as asking every dense vertex against every near triangle took seconds.
			// D95: every vertex is asked: one wholly on a leading mesh is cut away, so a see-through sheet shows no second sheet inside it.
			std::vector<uint8_t> onLead(mesh.positions.size(), 0), asked(mesh.positions.size(), 1);
			for (std::size_t v = 0; v < mesh.positions.size(); v++) {
				if (!asked[v])
					continue;
				const RE::NiPoint3& p = mesh.positions[v];
				for (const auto& other : meshes) {
					if (&other == &mesh || !leads(other, mesh) || p.x < other.lo[0] - FADE_REACH || p.x > other.hi[0] + FADE_REACH || p.y < other.lo[1] - FADE_REACH ||
						p.y > other.hi[1] + FADE_REACH || p.z < other.lo[2] - FADE_REACH || p.z > other.hi[2] + FADE_REACH)
						continue;
					nearby(other, p, FADE_REACH, [&](const auto& tri) {
						if (TriangleDistanceSquared(p, other.positions[tri[0]], other.positions[tri[1]], other.positions[tri[2]]) < FADE_REACH * FADE_REACH)
							onLead[v] = 1;
						return onLead[v] != 0;
					});
					if (onLead[v])
						break;
				}
			}
			// D68: an open-edge vertex fades only where the overlap reaches past it (it and all its neighbours on the leading mesh);
			// where two meshes only meet edge to edge, fading opened a gap
			std::vector<uint8_t> ringOn(mesh.positions.size(), 1);
			for (const auto& tri : mesh.triangles) {
				for (int k = 0; k < 3; k++) {
					for (int j = 0; j < 3; j++) {
						if (!onLead[tri[j]])
							ringOn[tri[k]] = 0;
					}
				}
			}
			for (std::size_t v = 0; v < mesh.positions.size(); v++) {
				float fade = 1.0f;
				if (mesh.open[v]) {
					edges++;
					fade = onLead[v] && ringOn[v] ? 0.0f : 1.0f;
					faded += fade == 0.0f ? 1 : 0;
				}
				vertexClips[mesh.base + v][1] = fade;
			}
			// D69: an open-edge vertex off every other mesh but near another's open edge (air between them: dark seam lines) moves past the
			// gap's middle by GAP_OVERLAP; the other edge does the same. In mesh space, for both depth passes' vertex shaders.
			const RE::NiTransform inverse = mesh.geometry->world.Invert();
			auto& shifts = gapShifts[mesh.geometry];
			shifts.assign(mesh.positions.size(), RE::NiPoint3(0.0f, 0.0f, 0.0f));
			for (std::size_t v = 0; v < mesh.positions.size(); v++) {
				if (!mesh.open[v] || onLead[v])
					continue;
				const RE::NiPoint3& p = mesh.positions[v];
				float best = GAP_REACH * GAP_REACH;
				RE::NiPoint3 target;
				bool found = false;
				for (const auto& other : meshes) {
					if (&other == &mesh || p.x < other.lo[0] - GAP_REACH || p.x > other.hi[0] + GAP_REACH || p.y < other.lo[1] - GAP_REACH || p.y > other.hi[1] + GAP_REACH ||
						p.z < other.lo[2] - GAP_REACH || p.z > other.hi[2] + GAP_REACH)
						continue;
					// Not where this vertex already lies on the other mesh (an overlap, D67's)
					bool over = false;
					nearby(other, p, 2.0f, [&](const auto& tri) {
						over = TriangleDistanceSquared(p, other.positions[tri[0]], other.positions[tri[1]], other.positions[tri[2]]) < 4.0f;
						return over;
					});
					if (over)
						continue;
					for (const auto& edge : other.openEdges) {
						const RE::NiPoint3& a = other.positions[edge[0]];
						const RE::NiPoint3 ab = other.positions[edge[1]] - a;
						const float length = ab.Dot(ab);
						const float s = length > 1e-6f ? std::clamp((p - a).Dot(ab) / length, 0.0f, 1.0f) : 0.0f;
						const RE::NiPoint3 q = a + ab * s;
						const RE::NiPoint3 d = q - p;
						const float distance = d.Dot(d);
						if (distance < best && distance > 1.0f) {
							best = distance;
							target = q;
							found = true;
						}
					}
				}
				if (!found)
					continue;
				const RE::NiPoint3 across = target - p;
				const float gap = std::sqrt(best);
				const RE::NiPoint3 moved = p + across * ((0.5f * gap + GAP_OVERLAP) / gap);
				shifts[v] = inverse * moved - inverse * p;
				gaps++;
			}
			// D86: smooth the open edge's ragged outline (vanilla's teeth) as a curve: each vertex drawn toward its two edge neighbours'
			// middle, at most EDGE_SMOOTH_MOST, unless closing a gap (D69). D87: edges over a leading mesh too (their teeth showed).
			std::vector<std::array<int32_t, 2>> along(mesh.positions.size(), { -1, -1 });
			std::vector<uint8_t> ends(mesh.positions.size(), 0);
			for (const auto& edge : mesh.openEdges) {
				for (int k = 0; k < 2; k++) {
					auto& slot = along[edge[k]];
					const int32_t other = edge[1 - k];
					if (slot[0] < 0)
						slot[0] = other;
					else if (slot[1] < 0)
						slot[1] = other;
					else
						ends[edge[k]] = 1;  // Where three or more edge runs meet: kept
				}
			}
			std::vector<RE::NiPoint3> curve = mesh.positions, next = mesh.positions;
			for (int pass = 0; pass < EDGE_SMOOTH_PASSES; pass++) {
				for (std::size_t v = 0; v < curve.size(); v++) {
					const auto& [a, b] = along[v];
					next[v] = curve[v];
					if (a < 0 || b < 0 || ends[v])
						continue;
					const RE::NiPoint3 middle = (curve[a] + curve[b]) * 0.5f;
					RE::NiPoint3 moved = curve[v] + (middle - curve[v]) * 0.5f;
					const RE::NiPoint3 offset = moved - mesh.positions[v];
					const float distance = offset.Length();
					if (distance > EDGE_SMOOTH_MOST)
						moved = mesh.positions[v] + offset * (EDGE_SMOOTH_MOST / distance);
					next[v] = moved;
				}
				curve.swap(next);
			}
			for (std::size_t v = 0; v < mesh.positions.size(); v++) {
				if (!mesh.open[v] || shifts[v].Length() > 0.0f || (curve[v] - mesh.positions[v]).Length() < 0.01f)
					continue;
				shifts[v] = inverse * curve[v] - inverse * mesh.positions[v];
				smoothed++;
			}
			// D94: fade the edge's moves into the surface over EDGE_BLEND_REACH so the rows inside follow (moved alone, the edge row folded
			// the dense copy's triangles over each other). Harmonic: each inner vertex the mean of its neighbours, the edge held.
			{
				std::vector<std::vector<uint32_t>> ring(mesh.positions.size());
				for (const auto& tri : mesh.triangles) {
					for (int k = 0; k < 3; k++) {
						ring[tri[k]].push_back(tri[(k + 1) % 3]);
						ring[tri[k]].push_back(tri[(k + 2) % 3]);
					}
				}
				std::vector<float> fromEdge(mesh.positions.size(), FLT_MAX);
				using Entry = std::pair<float, uint32_t>;
				std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> frontier;
				for (uint32_t v = 0; v < mesh.positions.size(); v++) {
					if (mesh.open[v]) {
						fromEdge[v] = 0.0f;
						frontier.emplace(0.0f, v);
					}
				}
				while (!frontier.empty()) {
					const auto [distance, v] = frontier.top();
					frontier.pop();
					if (distance > fromEdge[v] || distance > EDGE_BLEND_REACH)
						continue;
					for (const uint32_t n : ring[v]) {
						const float blended = distance + (mesh.positions[n] - mesh.positions[v]).Length();
						if (blended < fromEdge[n]) {
							fromEdge[n] = blended;
							frontier.emplace(blended, n);
						}
					}
				}
				std::vector<uint32_t> inner;
				for (uint32_t v = 0; v < mesh.positions.size(); v++) {
					if (!mesh.open[v] && fromEdge[v] < EDGE_BLEND_REACH)
						inner.push_back(v);
				}
				std::vector<RE::NiPoint3> blended = shifts;
				for (int pass = 0; pass < EDGE_BLEND_PASSES; pass++) {
					for (const uint32_t v : inner) {
						RE::NiPoint3 sum(0.0f, 0.0f, 0.0f);
						for (const uint32_t n : ring[v])
							sum += fromEdge[n] < EDGE_BLEND_REACH ? shifts[n] : RE::NiPoint3(0.0f, 0.0f, 0.0f);
						const float taper = 1.0f - fromEdge[v] / EDGE_BLEND_REACH;
						blended[v] = ring[v].empty() ? shifts[v] : sum / static_cast<float>(ring[v].size()) * std::min(1.0f, taper * 1.5f);
					}
					for (const uint32_t v : inner)
						shifts[v] = blended[v];
				}
			}
		}
		ComposeShifts();
		logger::info("[Fluid Field] Waterfall seams: {} of {} open-edge vertices of {} lit meshes lie on a mesh leading them, and fade; {} move across a gap to the mesh beside them; {} edge vertices smoothed", faded,
			edges, meshes.size(), gaps, smoothed);
		// D112: the inside cut and the sides (D103) now the fades are known; again once the rock and lead-in cuts are in
		RequestCulling();
	}

	void FallSites::ComposeCut(std::size_t a_index)
	{
		static_assert(NO_CUT == CLIP_MARGIN + CLIP_RANGE, "D112: a part of VertexCut that hides nothing is ClipStep's uncut value");
		if (a_index >= vertexClips.size() || a_index >= vertexCuts.size())
			return;
		const VertexCut& cut = vertexCuts[a_index];
		vertexClips[a_index][0] = std::min({ cut.water, cut.inside, cut.lead, cut.submerged, cut.rock, cut.island });
	}

	bool FallSites::VertexInShell(std::size_t a_index) const
	{
		if (a_index >= vertexCuts.size())
			return true;
		const VertexCut& cut = vertexCuts[a_index];
		return !cut.buried && cut.inside > 0.0f && cut.lead > 0.0f && cut.submerged > 0.0f && cut.island > 0.0f;
	}

	float FallSites::LandHeightAt(RE::hkpWorld* a_world, float a_x, float a_y, float a_above, LandProbe* a_probe, float a_engineHeight) const
	{
		// D126: straight down, past every hit but the landscape's own collision: layer 17 (ground) or 13 (terrain) with no reference.
		// LEARNINGS: landscape rays hit kGround, never kTerrain.
		const float scale = RE::bhkWorld::GetWorldScale();
		const float bottom = a_above - LAND_RAY_DROP;
		float z = a_above;
		for (int step = 0; step < 32 && z > bottom; step++) {
			RE::hkpWorldRayCastInput input;
			input.from = RE::hkVector4(a_x * scale, a_y * scale, z * scale, 0.0f);
			input.to = RE::hkVector4(a_x * scale, a_y * scale, bottom * scale, 0.0f);
			RE::hkpWorldRayCastOutput output;
			a_world->CastRay(input, output);
			if (!output.HasHit() || !output.rootCollidable)
				return -FLT_MAX;
			const float hit = z + (bottom - z) * output.hitFraction;
			const auto layer = output.rootCollidable->GetCollisionLayer();
			const bool referenced = RE::TESHavokUtilities::FindCollidableRef(*output.rootCollidable) != nullptr;
			// D131: what the ray met, for the log's samples
			if (a_probe) {
				a_probe->steps = step + 1;
				if (step == 0) {
					a_probe->firstLayer = static_cast<uint32_t>(layer);
					a_probe->firstReferenced = referenced;
					a_probe->firstHeight = hit;
				}
			}
			if ((layer == RE::COL_LAYER::kGround || layer == RE::COL_LAYER::kTerrain) && !referenced) {
				// D138: only where the engine's landscape height agrees (TES::GetLandHeight): unreferenced layer-17 collision can be a
				// mesh's, and taken for the ground it buried a visible sheet (a hole). Skipped and counted.
				float engine = 0.0f;
				bool known = false;
				if (!std::isnan(a_engineHeight)) {
					// D172: read on the render thread (the worker's rays)
					engine = a_engineHeight;
					known = a_engineHeight > -FLT_MAX;
				} else if (auto* tes = RE::TES::GetSingleton()) {
					known = tes->GetLandHeight(RE::NiPoint3(a_x, a_y, hit + 64.0f), engine);
				}
				if (!known || std::abs(engine - hit) <= LAND_AGREE) {
					if (a_probe)
						a_probe->engine = engine;
					return hit;
				}
				if (a_probe)
					a_probe->disagreed++;
			}
			z = hit - 0.5f;
		}
		return -FLT_MAX;
	}

	void FallSites::StaticHits(RE::hkpWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_direction, float a_reach, std::vector<StaticHit>& a_hits) const
	{
		// D135: every static face (layers 1 and 2; the falls' own collision skipped) the ray meets, as distances from a_from; the
		// landscape is LandHeightAt's. D138: with the face's normal along the ray (negative: the ray meets its front).
		const float scale = RE::bhkWorld::GetWorldScale();
		float from = 0.0f;
		for (int step = 0; step < 16 && from < a_reach; step++) {
			const RE::NiPoint3 start = a_from + a_direction * from;
			const RE::NiPoint3 end = a_from + a_direction * a_reach;
			RE::hkpWorldRayCastInput input;
			input.from = RE::hkVector4(start.x * scale, start.y * scale, start.z * scale, 0.0f);
			input.to = RE::hkVector4(end.x * scale, end.y * scale, end.z * scale, 0.0f);
			RE::hkpWorldRayCastOutput output;
			a_world->CastRay(input, output);
			if (!output.HasHit() || !output.rootCollidable)
				break;
			const float hit = from + (a_reach - from) * output.hitFraction;
			const auto layer = output.rootCollidable->GetCollisionLayer();
			if (layer == RE::COL_LAYER::kStatic || layer == RE::COL_LAYER::kAnimStatic) {
				const auto* ref = RE::TESHavokUtilities::FindCollidableRef(*output.rootCollidable);
				const char* model = ref ? ModelOf(ref->GetBaseObject()) : nullptr;
				const std::string name = model ? Lowercase(model) : std::string();
				if (name.find("fxwaterfall") == std::string::npos && name.find("fxrapids") == std::string::npos) {
					float normal[4];
					_mm_storeu_ps(normal, output.normal.quad);
					a_hits.push_back({ hit, normal[0] * a_direction.x + normal[1] * a_direction.y + normal[2] * a_direction.z });
				}
			}
			from = hit + 0.5f;
		}
	}

	std::vector<uint8_t> FallSites::ShellTexels(RE::BSGeometry* a_geometry, uint32_t a_width, uint32_t a_height, const float a_uvMin[2], const float a_uvSize[2], uint64_t* a_hash) const
	{
		const auto base = a_geometry ? vertexClipBase.find(a_geometry) : vertexClipBase.end();
		auto* triShape = a_geometry ? a_geometry->AsTriShape() : nullptr;
		auto* renderer = a_geometry ? a_geometry->GetGeometryRuntimeData().rendererData : nullptr;
		if (base == vertexClipBase.end() || !triShape || !renderer || !renderer->rawVertexData || !renderer->rawIndexData || a_width == 0 || a_height == 0 ||
			a_uvSize[0] <= 0.0f || a_uvSize[1] <= 0.0f || !renderer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_UV))
			return {};
		const auto& counts = triShape->GetTrishapeRuntimeData();
		const IndexSpan bakeIndices = BakeIndices(a_geometry);  // D172: the dense copy's untrimmed triangles
		if (base->second + counts.vertexCount > vertexCuts.size())
			return {};
		const uint32_t stride = renderer->vertexDesc.GetSize();
		const uint32_t uvOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_TEXCOORD0);
		std::vector<std::array<float, 2>> uvs(counts.vertexCount);
		std::vector<float> kept(counts.vertexCount);
		uint64_t hash = 14695981039346656037ull;
		for (uint32_t v = 0; v < counts.vertexCount; v++) {
			const auto* uv = reinterpret_cast<const uint16_t*>(renderer->rawVertexData + static_cast<std::size_t>(v) * stride + uvOffset);
			uvs[v] = { DirectX::PackedVector::XMConvertHalfToFloat(uv[0]), DirectX::PackedVector::XMConvertHalfToFloat(uv[1]) };
			kept[v] = VertexInShell(base->second + v) ? 1.0f : 0.0f;
			hash = (hash ^ (kept[v] > 0.5f ? 0x9Bu : 0x5Au)) * 1099511628211ull;
		}
		if (a_hash)
			*a_hash = hash;
		// The kept flags interpolated as StartBake lays each triangle into the grid (same texel centres and weights)
		std::vector<uint8_t> mask(static_cast<std::size_t>(a_width) * a_height, 1);
		const auto gx = [&](float a_u) { return (a_u - a_uvMin[0]) / a_uvSize[0] * static_cast<float>(a_width) - 0.5f; };
		const auto gy = [&](float a_v) { return (a_v - a_uvMin[1]) / a_uvSize[1] * static_cast<float>(a_height) - 0.5f; };
		for (uint32_t i = 0; i < bakeIndices.count; i++) {
			const uint16_t* index = bakeIndices.data + static_cast<std::size_t>(i) * 3;
			if (index[0] >= counts.vertexCount || index[1] >= counts.vertexCount || index[2] >= counts.vertexCount)
				continue;
			const float ax = gx(uvs[index[0]][0]), ay = gy(uvs[index[0]][1]), bx = gx(uvs[index[1]][0]), by = gy(uvs[index[1]][1]), cx = gx(uvs[index[2]][0]), cy = gy(uvs[index[2]][1]);
			const float det = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
			if (std::abs(det) < 1e-6f)
				continue;
			const int32_t x0 = std::max(0, static_cast<int32_t>(std::floor(std::min({ ax, bx, cx }))));
			const int32_t x1 = std::min(static_cast<int32_t>(a_width) - 1, static_cast<int32_t>(std::ceil(std::max({ ax, bx, cx }))));
			const int32_t y0 = std::max(0, static_cast<int32_t>(std::floor(std::min({ ay, by, cy }))));
			const int32_t y1 = std::min(static_cast<int32_t>(a_height) - 1, static_cast<int32_t>(std::ceil(std::max({ ay, by, cy }))));
			for (int32_t y = y0; y <= y1; y++) {
				for (int32_t x = x0; x <= x1; x++) {
					const float px = static_cast<float>(x), py = static_cast<float>(y);
					const float w1 = ((px - ax) * (cy - ay) - (cx - ax) * (py - ay)) / det;
					const float w2 = ((bx - ax) * (py - ay) - (px - ax) * (by - ay)) / det;
					const float w0 = 1.0f - w1 - w2;
					if (w0 < -0.02f || w1 < -0.02f || w2 < -0.02f)
						continue;
					const float value = w0 * kept[index[0]] + w1 * kept[index[1]] + w2 * kept[index[2]];
					mask[static_cast<std::size_t>(y) * a_width + x] = value >= 0.5f ? 1 : 0;
				}
			}
		}
		return mask;
	}

	FallSites::IndexSpan FallSites::BakeIndices(RE::BSGeometry* a_geometry) const
	{
		if (const auto it = untrimmedIndices.find(a_geometry); it != untrimmedIndices.end())
			return { it->second.data(), static_cast<uint32_t>(it->second.size() / 3) };
		auto* triShape = a_geometry ? a_geometry->AsTriShape() : nullptr;
		auto* renderer = a_geometry ? a_geometry->GetGeometryRuntimeData().rendererData : nullptr;
		if (!triShape || !renderer || !renderer->rawIndexData)
			return {};
		return { renderer->rawIndexData, triShape->GetTrishapeRuntimeData().triangleCount };
	}

	bool FallSites::VertexShown(std::size_t a_index) const
	{
		if (a_index >= vertexClips.size())
			return false;
		const auto& clip = vertexClips[a_index];
		// a.x (the water below, the curl, the lead-in) and its seam fade (a.y: an edge on a leading mesh is dithered out, D67)
		if (clip[0] <= 0.0f || clip[1] < 0.5f)
			return false;
		if (a_index >= vertexCuts.size())
			return true;
		// The pools' cut at the foot (the shaders', per frame) and the rock (the depth test): neither is in a.x
		const VertexCut& cut = vertexCuts[a_index];
		return cut.pool > 0.0f && !cut.buried;
	}

	bool FallSites::PoolCutAt(float a_x, float a_y, float& a_level) const
	{
		// As BuildPoolMap fills the shaders' pool map (the source pool cut deeper, D50), at the nearest texel (the shaders read the same
		// lattice bilinear), less FallGround::POOL_CUT
		bool found = false;
		for (const auto& [key, pools] : sitePools) {
			const auto x = static_cast<int32_t>(std::lround((a_x - pools.originX) / FallCapture::CELL));
			const auto y = static_cast<int32_t>(std::lround((a_y - pools.originY) / FallCapture::CELL));
			if (x < 0 || y < 0 || x >= static_cast<int32_t>(pools.width) || y >= static_cast<int32_t>(pools.height))
				continue;
			const float cover = pools.cover[static_cast<std::size_t>(y) * pools.width + x];
			if (cover <= NO_SURFACE)
				continue;
			const float cut = (cover >= pools.top - POOL_BAND ? cover - TOP_POOL_CUT : cover) - POOL_CUT_DEPTH;
			a_level = found ? std::max(a_level, cut) : cut;
			found = true;
		}
		return found;
	}

	void FallSites::BuryStep()
	{
		if (buryDone)
			return;
		auto* tes = RE::TES::GetSingleton();
		if (!tes)
			return;
		// D172: the landscape height under each vertex from Fluid Field's landscape cache, no rays. Densify already trimmed the dense
		// copies (DENSE_TRIM_UNDER), so this marks the vertices left under it (the shell mask reads them, D126) and gives the culling the
		// ground (SUBMERGED_BED). The six-axis Havok rays inside statics (D135, D138) cut nothing yet cost 3.7 ms a frame for minutes.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(BURY_BUDGET_MS);
		while (std::chrono::steady_clock::now() < deadline) {
			// D112: all tested: redo everything that reads the rock
			if (buryQueue.empty()) {
				buryDone = true;
				logger::info("[Fluid Field] Waterfall rock test: {} of {} vertices under the landscape (from its height, no rays)", buriedCount, buryTested);
				RequestCulling();
				return;
			}
			RE::NiPointer<RE::BSGeometry> geometry = buryQueue.back();
			buryQueue.pop_back();
			const auto base = geometry ? vertexClipBase.find(geometry.get()) : vertexClipBase.end();
			auto* triShape = geometry ? geometry->AsTriShape() : nullptr;
			auto* renderer = geometry ? geometry->GetGeometryRuntimeData().rendererData : nullptr;
			if (base == vertexClipBase.end() || !triShape || !renderer || !renderer->rawVertexData)
				continue;
			const auto& counts = triShape->GetTrishapeRuntimeData();
			if (base->second + counts.vertexCount > vertexCuts.size())
				continue;
			const uint32_t stride = renderer->vertexDesc.GetSize();
			const uint32_t positionOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
			const bool exterior = tes->interiorCell == nullptr;
			for (uint32_t v = 0; v < counts.vertexCount; v++) {
				float local[3];
				std::memcpy(local, renderer->rawVertexData + static_cast<std::size_t>(v) * stride + positionOffset, sizeof(local));
				const RE::NiPoint3 p = geometry->world * RE::NiPoint3(local[0], local[1], local[2]);
				VertexCut& parts = vertexCuts[base->second + v];
				float ground = -FLT_MAX;
				if (exterior && landscape && !landscape->SampleHeight(p.x, p.y, ground))
					ground = -FLT_MAX;
				parts.ground = ground;
				// D131: in a.x (the GPU cuts it), from BURY_LAND under the landscape
				const float landClip = ground > -FLT_MAX ? std::clamp(p.z - (ground - BURY_LAND), -CLIP_RANGE, NO_CUT) : NO_CUT;
				parts.rock = landClip;
				parts.rockCause = static_cast<uint8_t>(landClip <= 0.0f ? 1 : 0);
				parts.votes = 0;
				parts.buried = landClip <= 0.0f ? 1 : 0;
				buriedCount += parts.buried;
				buryTested++;
			}
		}
	}
	FallSites::CullSheet FallSites::BuildCullSheet(RE::BSGeometry* a_geometry, uint32_t a_base)
	{
		const auto binKey = [](int64_t a_x, int64_t a_y, int64_t a_z) { return (a_x & 0x1FFFFF) << 42 | (a_y & 0x1FFFFF) << 21 | (a_z & 0x1FFFFF); };
		const auto cellOf = [](float a_v) { return static_cast<int64_t>(std::floor(a_v / SEAM_BIN)); };
		auto* geometry = a_geometry;
		auto* triShape = geometry ? geometry->AsTriShape() : nullptr;
		auto* renderer = geometry ? geometry->GetGeometryRuntimeData().rendererData : nullptr;
		if (!triShape || !renderer || !renderer->rawVertexData || !renderer->rawIndexData)
			return {};
		const auto& counts = triShape->GetTrishapeRuntimeData();
		if (a_base + counts.vertexCount > vertexClips.size() || a_base + counts.vertexCount > vertexCuts.size())
			return {};
		auto& desc = renderer->vertexDesc;  // CommonLib's getters are not const
		const uint32_t stride = desc.GetSize();
		const uint32_t positionOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
		const bool haveNormals = desc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_NORMAL);
		const uint32_t normalOffset = haveNormals ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_NORMAL) : 0;
		CullSheet sheet;
		sheet.geometry = geometry;
		sheet.base = a_base;
		sheet.positions.resize(counts.vertexCount);
		sheet.drawn.resize(counts.vertexCount);
		sheet.outward.assign(counts.vertexCount, RE::NiPoint3(0.0f, 0.0f, 0.0f));
		sheet.shown.assign(counts.vertexCount, RE::NiPoint3(0.0f, 0.0f, 0.0f));
		for (uint32_t v = 0; v < counts.vertexCount; v++) {
			const uint8_t* data = renderer->rawVertexData + static_cast<std::size_t>(v) * stride;
			float local[3];
			std::memcpy(local, data + positionOffset, sizeof(local));
			sheet.positions[v] = geometry->world * RE::NiPoint3(local[0], local[1], local[2]);
			// Where it is drawn: moved by the seam gap and swell (b.xyz, mesh space; not the wobble)
			const auto& clip = vertexClips[sheet.base + v];
			sheet.drawn[v] = sheet.positions[v] + geometry->world.rotate * (RE::NiPoint3(clip[4], clip[5], clip[6]) * geometry->world.scale);
			for (int k = 0; k < 3; k++) {
				const float c = k == 0 ? sheet.positions[v].x : (k == 1 ? sheet.positions[v].y : sheet.positions[v].z);
				sheet.lo[k] = std::min(sheet.lo[k], c);
				sheet.hi[k] = std::max(sheet.hi[k], c);
			}
			sheet.top = std::max(sheet.top, sheet.positions[v].z);
			// As FallRefraction::BuildMesh reads it: the see-through pass turns this normal by the flag
			if (haveNormals) {
				const uint8_t* n = data + normalOffset;
				sheet.shown[v] = geometry->world.rotate * RE::NiPoint3(n[0] / 255.0f * 2.0f - 1.0f, n[1] / 255.0f * 2.0f - 1.0f, n[2] / 255.0f * 2.0f - 1.0f);
			}
		}
		std::map<std::pair<uint16_t, uint16_t>, int> uses;
		for (uint32_t i = 0; i < counts.triangleCount; i++) {
			const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(i) * 3;
			if (index[0] >= counts.vertexCount || index[1] >= counts.vertexCount || index[2] >= counts.vertexCount)
				continue;
			sheet.triangles.push_back({ index[0], index[1], index[2] });
			for (int k = 0; k < 3; k++)
				uses[{ std::min(index[k], index[(k + 1) % 3]), std::max(index[k], index[(k + 1) % 3]) }]++;
		}
		sheet.open.assign(counts.vertexCount, 0);
		for (const auto& [edge, count] : uses) {
			if (count == 1)
				sheet.open[edge.first] = sheet.open[edge.second] = 1;
		}
		// D98, D103: its outer side, carried over the sheet (OrientSheet), as the lead-in's outer side faces against the travel
		sheet.side = OrientSheet(sheet.positions, sheet.triangles, SheetTravel(sheet.positions), haveNormals ? &sheet.shown : nullptr);
		for (uint32_t t = 0; t < sheet.triangles.size(); t++) {
			const auto& tri = sheet.triangles[t];
			const RE::NiPoint3 &a = sheet.positions[tri[0]], &b = sheet.positions[tri[1]], &c = sheet.positions[tri[2]];
			const RE::NiPoint3 face = (b - a).Cross(c - a);
			sheet.area += 0.5f * face.Length();
			for (const uint16_t k : tri) {
				sheet.outward[k] += face * static_cast<float>(sheet.side[t]);
				if (!haveNormals)
					sheet.shown[k] += face;
			}
			for (int64_t x = cellOf(std::min({ a.x, b.x, c.x })); x <= cellOf(std::max({ a.x, b.x, c.x })); x++) {
				for (int64_t y = cellOf(std::min({ a.y, b.y, c.y })); y <= cellOf(std::max({ a.y, b.y, c.y })); y++) {
					for (int64_t z = cellOf(std::min({ a.z, b.z, c.z })); z <= cellOf(std::max({ a.z, b.z, c.z })); z++)
						sheet.bins[binKey(x, y, z)].push_back(t);
				}
			}
		}
		for (auto& n : sheet.outward) {
			const float length = n.Length();
			n = length > 1e-6f ? n / length : RE::NiPoint3(0.0f, 0.0f, 0.0f);
		}
		// D131: the engine's water planes over it, for the submerged cut
		CollectWaterPlanes(sheet.lo[0], sheet.lo[1], sheet.hi[0], sheet.hi[1], sheet.planes);
		return sheet;
	}

	void FallSites::RunCullStages(CullJob& a_job)
	{
		// D172: stages 1-5 on a worker (3.3 ms a frame for tens of seconds on the render thread); meanwhile Update holds back
		// everything else that writes vertexClips, vertexCuts and the pools (CullWorking)
		CullJob& job = a_job;
		const auto begin = std::chrono::steady_clock::now();
		constexpr int DONE = 6;
		// Stage 0: the sheets (each one OrientSheet)
		if (job.stage == 0) {
			for (const auto& [geometry, base] : job.meshes) {
				if (job.cancel.load(std::memory_order_relaxed))
					return;
				auto sheet = BuildCullSheet(geometry.get(), base);
				if (sheet.geometry)
					job.sheets.push_back(std::move(sheet));
			}
			job.stage = 1;
			job.sheet = job.vertex = 0;
		}
		while (job.stage < DONE) {
			if (job.cancel.load(std::memory_order_relaxed))
				return;
			// Stages 1-5 over every sheet: the pools' cut, the inside cut (then a.x), cleanup (D138: a sheet a step), the sides, the meets.
			// Each stage reads only what earlier stages finished over all sheets.
			if (job.sheet >= job.sheets.size()) {
				job.stage++;
				job.sheet = job.vertex = 0;
				continue;
			}
			if (job.stage == 3) {
				CleanupSheet(job.sheets[job.sheet], job);
				job.sheet++;
				continue;
			}
			if (job.stage == 2 || job.stage == 4 || job.stage == 5) {
				// D185: these read only earlier stages and write only their own vertex, so all vertices run on several threads (37 s on
				// one)
				std::vector<std::pair<uint32_t, uint32_t>> work;
				for (uint32_t s = 0; s < job.sheets.size(); s++) {
					for (uint32_t v = 0; v < job.sheets[s].positions.size(); v++)
						work.emplace_back(s, v);
				}
				const int stage = job.stage;
				std::atomic<std::size_t> next = 0;
				const auto run = [&]() {
					for (;;) {
						if (job.cancel.load(std::memory_order_relaxed))
							return;
						const std::size_t from = next.fetch_add(CULL_CHUNK);
						if (from >= work.size())
							return;
						for (std::size_t w = from; w < std::min(work.size(), from + CULL_CHUNK); w++) {
							const auto [s, v] = work[w];
							if (stage == 2) {
								VolumeCut(job.sheets, s, v, job);
								ComposeCut(job.sheets[s].base + v);
							} else if (stage == 4) {
								SideFlags(job.sheets, s, v, job);
							} else {
								MeetDistance(job.sheets, s, v, job);
							}
						}
					}
				};
				const unsigned threads = std::clamp(std::thread::hardware_concurrency() > 2 ? std::thread::hardware_concurrency() - 2 : 1u, 1u, 12u);
				std::vector<std::future<void>> parts;
				for (unsigned t = 1; t < threads; t++)
					parts.push_back(std::async(std::launch::async, run));
				run();
				for (auto& part : parts)
					part.get();
				if (job.cancel.load(std::memory_order_relaxed))
					return;
				job.stage++;
				job.sheet = job.vertex = 0;
				continue;
			}
			const CullSheet& sheet = job.sheets[job.sheet];
			for (int n = 0; n < 32 && job.vertex < sheet.positions.size(); n++, job.vertex++) {
				const auto v = static_cast<uint32_t>(job.vertex);
				if (job.stage == 1) {
					PoolCut(sheet, v);
					// D126: in the river over it: under our pools' water (D40, D78) and near the landscape below (the bed, not the air
					// under a lip); the lead-in arch into the riverbed showed under the river where the bake missed it (D97)
					VertexCut& parts = vertexCuts[sheet.base + v];
					parts.submerged = NO_CUT;
					const RE::NiPoint3& at = sheet.positions[v];
					if (parts.ground > -FLT_MAX && at.z - parts.ground <= SUBMERGED_BED) {
						// D131: or the engine's water planes, each only where it stands at most SUBMERGED_COLUMN over the landscape: a
						// river over its bed, not a lake's or cell's plane high up the cliff (D103)
						float level = -FLT_MAX, pool = 0.0f;
						if (PoolLevelAt(at.x, at.y, pool))
							level = pool;
						for (const auto& plane : sheet.planes) {
							// D143: and at most SUBMERGED_OVER_TOP over this sheet's top: its own river, not the drop above's (it ate a
							// cascade's lower sheets)
							if (at.x >= plane[0] && at.x <= plane[2] && at.y >= plane[1] && at.y <= plane[3] && plane[4] - parts.ground <= SUBMERGED_COLUMN &&
								plane[4] <= sheet.top + SUBMERGED_OVER_TOP)
								level = std::max(level, plane[4]);
						}
						if (level > -FLT_MAX) {
							// D157: only SUBMERGED_DEPTH under it (at LEAD_CUT_DEPTH, 16, it ate 70k vertices of the sheets in a cascade's
							// pools)
							parts.submerged = std::clamp(at.z - (level - SUBMERGED_DEPTH), -CLIP_RANGE, NO_CUT);
							job.submerged += parts.submerged < 0.0f ? 1 : 0;
						}
					}
				} else if (job.stage == 2) {
					VolumeCut(job.sheets, job.sheet, v, job);
					ComposeCut(sheet.base + v);
				} else if (job.stage == 4) {
					SideFlags(job.sheets, job.sheet, v, job);
				} else {
					MeetDistance(job.sheets, job.sheet, v, job);
				}
			}
			if (job.vertex >= sheet.positions.size()) {
				job.sheet++;
				job.vertex = 0;
			}
		}
		job.busy += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
		job.complete = true;
	}

	void FallSites::CullStep()
	{
		// D185: wait (at most CULL_DEFER_MOST) for captures, dense copies and rock tests still coming: each would ask again (11 passes)
		if (cullRequested && !cullJob) {
			bool coming = denseWork.valid() || !clipQueue.empty() || !buryQueue.empty();
			for (const auto& [key, capture] : captures)
				coming = coming || (!capture.IsComplete() && capture.Castable()) || (capture.IsComplete() && !capture.reported);
			const auto now = std::chrono::steady_clock::now();
			if (!cullWaiting) {
				cullWaiting = true;
				cullAskedAt = now;
			}
			if (coming && std::chrono::duration<double>(now - cullAskedAt).count() < CULL_DEFER_MOST)
				return;
			cullWaiting = false;
		}
		if (cullRequested) {
			// Asked again: restart with what is known now (a half-done pass read older cuts). D172: stop a running worker first
			if (cullWork.valid()) {
				cullJob->cancel = true;
				if (cullWork.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
					return;
				cullWork.get();
				cullWork = {};
			}
			cullRequested = false;
			cullJob = std::make_unique<CullJob>();
			cullJob->started = std::chrono::steady_clock::now();
		}
		if (!cullJob)
			return;
		CullJob& job = *cullJob;
		if (cullWork.valid()) {
			if (cullWork.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
				return;
			cullWork.get();
			cullWork = {};
			if (!job.complete) {
				cullJob.reset();
				return;
			}
		}
		if (job.stage == 0) {
			// D172: gather the lit meshes here (a sheet took 40-58 ms on the render thread), then every stage on the worker
			for (const auto& lit : litMeshList) {
				auto* geometry = lit.geometry.get();
				const auto base = geometry ? vertexClipBase.find(geometry) : vertexClipBase.end();
				if (base == vertexClipBase.end() || std::any_of(job.meshes.begin(), job.meshes.end(), [&](const auto& a_mesh) { return a_mesh.first.get() == geometry; }))
					continue;
				job.meshes.emplace_back(lit.geometry, base->second);
			}
			CullJob* running = cullJob.get();
			cullWork = std::async(std::launch::async, [this, running]() {
				const auto start = std::chrono::steady_clock::now();
				RunCullStages(*running);
				loadTiming.AddWorker(LoadWork::Cull, start);
			});
			return;
		}
		if (!job.complete)
			return;
		// D109: the depth rays' way in (against the outer side), for meshes still loaded where they were
		for (const auto& sheet : job.sheets) {
			const auto base = vertexClipBase.find(sheet.geometry);
			if (base != vertexClipBase.end() && base->second == sheet.base)
				outwardNormals[sheet.geometry] = sheet.outward;
		}
		vertexClipVersion++;
		logger::info("[Fluid Field] Waterfall culling: {} lit meshes; {} of {} vertices tested lie inside another sheet; {} turned to face out; {} of {} tested have a sheet in front of their inner side; {} within {:.0f} units of another sheet ({:.1f} ms of work over {:.0f} ms)",
			job.sheets.size(), job.inside.load(), job.tested.load(), job.inward.load(), job.covered.load(), job.sideTested.load(), job.meeting.load(), MEET_REACH, job.busy,
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - job.started).count());
		const uint32_t submerged = job.submerged;
		logger::info("[Fluid Field]   {} vertices under the river's water by its bed, cut; cleanup: {} orphan pieces ({} triangles, {} vertices) cut, {} small enclosed holes ({} vertices) restored",
			submerged, job.islands, job.islandTriangles, job.islandVertices, job.holes, job.holeVertices);
		cullJob.reset();
		DumpUnifyInputs();
		// D126: the spray, plunges and lips come only from the shell (ComputePlunges reads VertexInShell)
		if (!bakeFields.empty())
			plungesDue = true;  // D172: ComputePlunges on a worker (Update)
	}
	void FallSites::CleanupSheet(const CullSheet& a_sheet, CullJob& a_job)
	{
		const uint32_t holesBefore = a_job.holes, holeVerticesBefore = a_job.holeVertices, orphansBefore = a_job.islands, orphanTrianglesBefore = a_job.islandTriangles;
		// D138: per-vertex cuts make small mistakes: kept vertices floating in rock or air, cut ones holing a plain sheet. On the sheet's
		// welded graph (texture seams join), after every cut: small kept pieces off the main body are cut; small cut pieces enclosed by
		// kept ones and cut only by rock or another sheet (not water, lead-in or pools) are restored. Whole pieces switch, so no cut line
		// moves.
		const std::size_t count = a_sheet.positions.size();
		if (count == 0)
			return;
		std::unordered_map<int64_t, uint32_t> welds;
		std::vector<uint32_t> node(count);
		for (std::size_t v = 0; v < count; v++) {
			const RE::NiPoint3& q = a_sheet.positions[v];
			const int64_t key = (std::llround(q.x * 8.0) & 0x1FFFFF) << 42 | (std::llround(q.y * 8.0) & 0x1FFFFF) << 21 | (std::llround(q.z * 8.0) & 0x1FFFFF);
			node[v] = welds.try_emplace(key, static_cast<uint32_t>(welds.size())).first->second;
		}
		const std::size_t nodes = welds.size();
		std::vector<std::vector<uint32_t>> next(nodes);
		for (const auto& tri : a_sheet.triangles) {
			for (int k = 0; k < 3; k++) {
				const uint32_t a = node[tri[k]], b = node[tri[(k + 1) % 3]];
				if (a == b)
					continue;
				next[a].push_back(b);
				next[b].push_back(a);
			}
		}
		// Each node's state from its vertices, the island cut undone first (redone here); cut if any copy is
		std::vector<uint8_t> kept(nodes, 1), solid(nodes, 1), open(nodes, 0);
		std::vector<std::vector<uint32_t>> copies(nodes);
		for (std::size_t v = 0; v < count; v++) {
			const std::size_t index = a_sheet.base + v;
			VertexCut& parts = vertexCuts[index];
			parts.island = NO_CUT;
			ComposeCut(index);
			const uint32_t n = node[v];
			copies[n].push_back(static_cast<uint32_t>(v));
			const bool shown = vertexClips[index][0] > 0.0f && parts.pool > 0.0f;
			kept[n] = kept[n] && shown;
			// Cut only by the rock or by another sheet: the water, the lead-in, the river and the pools cut where they mean to
			solid[n] = solid[n] && parts.water > 0.0f && parts.lead > 0.0f && parts.submerged > 0.0f && parts.pool > 0.0f;
			open[n] = open[n] || a_sheet.open[v];
		}
		// The pieces of one kind (kept, or cut by rock or a sheet), by a flood over the graph
		const auto pieces = [&](const auto& a_member, std::vector<int32_t>& a_label, std::vector<std::vector<uint32_t>>& a_pieces) {
			a_label.assign(nodes, -1);
			a_pieces.clear();
			std::vector<uint32_t> open2;
			for (uint32_t start = 0; start < nodes; start++) {
				if (a_label[start] >= 0 || !a_member(start))
					continue;
				const auto label = static_cast<int32_t>(a_pieces.size());
				a_pieces.emplace_back();
				open2.assign(1, start);
				a_label[start] = label;
				while (!open2.empty()) {
					const uint32_t n = open2.back();
					open2.pop_back();
					a_pieces.back().push_back(n);
					for (const uint32_t m : next[n]) {
						if (a_label[m] < 0 && a_member(m)) {
							a_label[m] = label;
							open2.push_back(m);
						}
					}
				}
			}
		};
		std::vector<int32_t> label;
		std::vector<std::vector<uint32_t>> found;
		// Holes first: a cut piece with no open edge, every neighbour kept, small, cut only by rock or a sheet
		uint32_t keptNodes = 0;
		for (std::size_t n = 0; n < nodes; n++)
			keptNodes += kept[n] ? 1 : 0;
		pieces([&](uint32_t a_n) { return !kept[a_n]; }, label, found);
		const auto holeMost = std::max<std::size_t>(HOLE_MOST_VERTICES, static_cast<std::size_t>(HOLE_MOST_FRACTION * static_cast<float>(keptNodes)));
		for (const auto& piece : found) {
			if (piece.size() > holeMost)
				continue;
			bool enclosed = true;
			for (const uint32_t n : piece) {
				if (open[n] || !solid[n]) {
					enclosed = false;
					break;
				}
				for (const uint32_t m : next[n]) {
					if (label[m] != label[piece.front()] && !kept[m]) {
						enclosed = false;
						break;
					}
				}
				if (!enclosed)
					break;
			}
			if (!enclosed)
				continue;
			for (const uint32_t n : piece) {
				kept[n] = 1;
				for (const uint32_t v : copies[n]) {
					VertexCut& parts = vertexCuts[a_sheet.base + v];
					parts.rock = NO_CUT;
					parts.inside = NO_CUT;
					parts.buried = 0;
					ComposeCut(a_sheet.base + v);
				}
			}
			a_job.holes++;
			a_job.holeVertices += static_cast<uint32_t>(piece.size());
			keptNodes += static_cast<uint32_t>(piece.size());
		}
		// D143: then the orphans, by triangle (drawn if any corner is kept: a lone kept vertex draws a fan of slivers). Kept triangles join
		// across an edge with a kept end; per authored part, the largest kept piece is its body. Another piece is cut if the cuts made it
		// (it has a cut corner) and it is small; the body too if it is a scrap of its part.
		std::vector<int32_t> partOf;
		std::vector<std::vector<uint32_t>> authored;
		pieces([](uint32_t) { return true; }, partOf, authored);
		const std::size_t triangles = a_sheet.triangles.size();
		std::vector<uint8_t> drawn(triangles, 0), touchesCut(triangles, 0);
		std::vector<std::array<uint32_t, 3>> corner(triangles);
		for (std::size_t t = 0; t < triangles; t++) {
			for (int k = 0; k < 3; k++) {
				corner[t][k] = node[a_sheet.triangles[t][k]];
				drawn[t] = drawn[t] || kept[corner[t][k]];
				touchesCut[t] = touchesCut[t] || !kept[corner[t][k]];
			}
		}
		std::vector<uint32_t> root(triangles);
		for (std::size_t t = 0; t < triangles; t++)
			root[t] = static_cast<uint32_t>(t);
		const auto find = [&](uint32_t a_t) {
			while (root[a_t] != a_t) {
				root[a_t] = root[root[a_t]];
				a_t = root[a_t];
			}
			return a_t;
		};
		{
			std::unordered_map<uint64_t, uint32_t> edgeOwner;
			for (std::size_t t = 0; t < triangles; t++) {
				if (!drawn[t])
					continue;
				for (int k = 0; k < 3; k++) {
					const uint32_t ea = corner[t][k], eb = corner[t][(k + 1) % 3];
					if (ea == eb || (!kept[ea] && !kept[eb]))
						continue;
					const uint64_t key = static_cast<uint64_t>(std::min(ea, eb)) << 32 | std::max(ea, eb);
					const auto [owner, added] = edgeOwner.try_emplace(key, static_cast<uint32_t>(t));
					if (!added)
						root[find(static_cast<uint32_t>(t))] = find(owner->second);
				}
			}
		}
		std::unordered_map<uint32_t, std::vector<uint32_t>> pieceTriangles;
		std::vector<std::size_t> partTriangles(authored.size(), 0), partAuthored(authored.size(), 0);
		for (std::size_t t = 0; t < triangles; t++) {
			partAuthored[partOf[corner[t][0]]]++;
			if (!drawn[t])
				continue;
			pieceTriangles[find(static_cast<uint32_t>(t))].push_back(static_cast<uint32_t>(t));
			partTriangles[partOf[corner[t][0]]]++;
		}
		std::vector<uint32_t> body(authored.size(), UINT32_MAX);
		for (const auto& [id, members] : pieceTriangles) {
			uint32_t& best = body[partOf[corner[members.front()][0]]];
			if (best == UINT32_MAX || members.size() > pieceTriangles[best].size())
				best = id;
		}
		std::vector<uint8_t> orphanNode(nodes, 0), bodyNode(nodes, 0);
		for (const auto& [id, members] : pieceTriangles) {
			const auto whole = partOf[corner[members.front()][0]];
			const auto most = std::max<std::size_t>(ORPHAN_MOST_TRIANGLES, static_cast<std::size_t>(ORPHAN_MOST_FRACTION * static_cast<float>(partTriangles[whole])));
			const bool scrap = members.size() <= BODY_SCRAP_TRIANGLES && static_cast<float>(members.size()) < BODY_SCRAP_FRACTION * static_cast<float>(partAuthored[whole]);
			bool split = false;
			for (const uint32_t t : members)
				split = split || touchesCut[t];
			const bool orphan = split && (id == body[whole] ? scrap : members.size() <= most);
			for (const uint32_t t : members) {
				for (const uint32_t n : corner[t]) {
					if (!kept[n])
						continue;
					(orphan ? orphanNode : bodyNode)[n] = 1;
				}
			}
			if (orphan) {
				a_job.islands++;
				a_job.islandTriangles += static_cast<uint32_t>(members.size());
			}
		}
		// A kept vertex is cut only if every piece it draws in is an orphan (a bow tie's middle vertex stays with the body)
		for (std::size_t n = 0; n < nodes; n++) {
			if (!orphanNode[n] || bodyNode[n])
				continue;
			kept[n] = 0;
			for (const uint32_t v : copies[n]) {
				vertexCuts[a_sheet.base + v].island = -CLIP_RANGE;
				ComposeCut(a_sheet.base + v);
			}
			a_job.islandVertices++;
		}
		// D138: log what cut this sheet's vertices and its two largest pieces cut by rock or a sheet alone, to trace a hole to its test
		uint32_t byLand = 0, byStatic = 0, byInside = 0, byLead = 0, byRiver = 0, byWater = 0, byPool = 0, byIsland = 0;
		for (std::size_t v = 0; v < count; v++) {
			const VertexCut& parts = vertexCuts[a_sheet.base + v];
			byLand += (parts.rockCause & 1) && parts.rock <= 0.0f ? 1 : 0;
			byStatic += (parts.rockCause & 2) && parts.rock <= 0.0f ? 1 : 0;
			byInside += parts.inside <= 0.0f ? 1 : 0;
			byLead += parts.lead <= 0.0f ? 1 : 0;
			byRiver += parts.submerged <= 0.0f ? 1 : 0;
			byWater += parts.water <= 0.0f ? 1 : 0;
			byPool += parts.pool <= 0.0f ? 1 : 0;
			byIsland += parts.island <= 0.0f ? 1 : 0;
		}
		logger::info("[Fluid Field]   cut '{}' ({} vertices): {} under the landscape, {} in statics, {} inside another sheet, {} lead-in, {} in the river, {} the water below, {} the pools, {} orphans; {} orphan pieces ({} triangles) cut, {} holes ({} vertices) restored",
			a_sheet.geometry->name.c_str(), count, byLand, byStatic, byInside, byLead, byRiver, byWater, byPool, byIsland, a_job.islands - orphansBefore,
			a_job.islandTriangles - orphanTrianglesBefore, a_job.holes - holesBefore, a_job.holeVertices - holeVerticesBefore);
		pieces([&](uint32_t a_n) { return !kept[a_n] && solid[a_n]; }, label, found);
		std::sort(found.begin(), found.end(), [](const auto& a_a, const auto& a_b) { return a_a.size() > a_b.size(); });
		for (std::size_t k = 0; k < found.size() && k < 2; k++) {
			RE::NiPoint3 centre(0.0f, 0.0f, 0.0f);
			uint32_t land = 0, statics = 0, inside = 0, votes = 0, edge = 0;
			for (const uint32_t n : found[k]) {
				const uint32_t v = copies[n].front();
				const VertexCut& parts = vertexCuts[a_sheet.base + v];
				centre += a_sheet.positions[v];
				land += (parts.rockCause & 1) && parts.rock <= 0.0f ? 1 : 0;
				statics += (parts.rockCause & 2) && parts.rock <= 0.0f ? 1 : 0;
				inside += parts.inside <= 0.0f ? 1 : 0;
				votes += parts.votes;
				edge += open[n] ? 1 : 0;
			}
			const float size = static_cast<float>(found[k].size());
			centre = centre / size;
			logger::info("[Fluid Field]     cut piece of {} vertices round ({:.0f}, {:.0f}, {:.0f}): {} by the landscape, {} by statics (mean {:.1f} of 6 axes inside), {} inside another sheet; {} on its open edge",
				found[k].size(), centre.x, centre.y, centre.z, land, statics, static_cast<float>(votes) / size, inside, edge);
		}
	}

	void FallSites::PoolCut(const CullSheet& a_sheet, uint32_t a_vertex)
	{
		VertexCut& parts = vertexCuts[a_sheet.base + a_vertex];
		parts.pool = NO_CUT;
		// D71: only its foot (a.w's first bit), as FallGround::ClipDistance
		if (std::fmod(vertexClips[a_sheet.base + a_vertex][3], 2.0f) < 0.5f)
			return;
		const RE::NiPoint3& p = a_sheet.drawn[a_vertex];
		float level = 0.0f;
		if (PoolCutAt(p.x, p.y, level))
			parts.pool = std::min(p.z - level, NO_CUT);
	}

	void FallSites::VolumeCut(const std::vector<CullSheet>& a_sheets, std::size_t a_sheet, uint32_t a_vertex, CullJob& a_job)
	{
		const auto binKey = [](int64_t a_x, int64_t a_y, int64_t a_z) { return (a_x & 0x1FFFFF) << 42 | (a_y & 0x1FFFFF) << 21 | (a_z & 0x1FFFFF); };
		const auto cellOf = [](float a_v) { return static_cast<int64_t>(std::floor(a_v / SEAM_BIN)); };
		const CullSheet& mesh = a_sheets[a_sheet];
		const RE::NiPoint3& p = mesh.positions[a_vertex];
		float deepest = NO_CUT;
		bool tested = false;
		static const RE::NiPoint3 axes[6] = { { 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f } };
		std::vector<int64_t> cells;
		for (std::size_t o = 0; o < a_sheets.size(); o++) {
			const CullSheet& other = a_sheets[o];
			// Only a sheet whose bound holds the vertex can enclose it
			if (o == a_sheet || p.x < other.lo[0] || p.x > other.hi[0] || p.y < other.lo[1] || p.y > other.hi[1] || p.z < other.lo[2] || p.z > other.hi[2])
				continue;
			tested = true;
			// Each axis: the first triangle of the other it meets. Leaving through its outer face votes inside; entering through it means
			// outside. No hit (open onto rock or water, or past it): neither.
			int leaving = 0;
			bool entering = false;
			for (int a = 0; a < 6; a++) {
				// Too few axes left to make the votes: outside
				if (leaving + (6 - a) < VOLUME_VOTES)
					break;
				const RE::NiPoint3& axis = axes[a];
				const RE::NiPoint3 to = p + axis * VOLUME_REACH;
				cells.clear();
				const int steps = static_cast<int>(std::ceil(VOLUME_REACH / (SEAM_BIN * 0.25f)));
				for (int i = 0; i <= steps; i++) {
					const RE::NiPoint3 q = p + (to - p) * (static_cast<float>(i) / static_cast<float>(steps));
					if (q.x < other.lo[0] - SEAM_BIN || q.x > other.hi[0] + SEAM_BIN || q.y < other.lo[1] - SEAM_BIN || q.y > other.hi[1] + SEAM_BIN || q.z < other.lo[2] - SEAM_BIN ||
						q.z > other.hi[2] + SEAM_BIN)
						break;  // Past the other's bound: nothing more of it along this axis
					const int64_t key = binKey(cellOf(q.x), cellOf(q.y), cellOf(q.z));
					if (cells.empty() || cells.back() != key)
						cells.push_back(key);
				}
				float nearest = VOLUME_REACH;
				int sign = 0;
				for (const int64_t key : cells) {
					const auto it = other.bins.find(key);
					if (it == other.bins.end())
						continue;
					for (const uint32_t t : it->second) {
						const auto& tri = other.triangles[t];
						const float along = RayTriangle(p, axis, other.positions[tri[0]], other.positions[tri[1]], other.positions[tri[2]], nearest);
						if (along < 1.0f)
							continue;  // Behind, past the nearest so far, or on the vertex itself (two sheets touching)
						nearest = along;
						const RE::NiPoint3 face = (other.positions[tri[1]] - other.positions[tri[0]]).Cross(other.positions[tri[2]] - other.positions[tri[0]]) * static_cast<float>(other.side[t]);
						sign = face.Dot(axis) > 0.0f ? 1 : -1;
					}
				}
				leaving += sign > 0 ? 1 : 0;
				if (sign < 0) {
					entering = true;
					break;
				}
			}
			if (entering || leaving < VOLUME_VOTES)
				continue;
			// Inside: cut from VOLUME_MARGIN within the other's surface, so the cut runs just past where the sheets cross and the seam
			// stays closed; held to -CLIP_RANGE like every cut (D58)
			const float reach = VOLUME_MARGIN + CLIP_RANGE;
			float best = reach * reach;
			const int64_t x0 = cellOf(p.x - reach), x1 = cellOf(p.x + reach), y0 = cellOf(p.y - reach), y1 = cellOf(p.y + reach), z0 = cellOf(p.z - reach), z1 = cellOf(p.z + reach);
			for (int64_t x = x0; x <= x1; x++) {
				for (int64_t y = y0; y <= y1; y++) {
					for (int64_t z = z0; z <= z1; z++) {
						const auto it = other.bins.find(binKey(x, y, z));
						if (it == other.bins.end())
							continue;
						for (const uint32_t t : it->second) {
							const auto& tri = other.triangles[t];
							best = std::min(best, TriangleDistanceSquared(p, other.positions[tri[0]], other.positions[tri[1]], other.positions[tri[2]]));
						}
					}
				}
			}
			deepest = std::min(deepest, std::max(VOLUME_MARGIN - std::sqrt(best), -CLIP_RANGE));
		}
		vertexCuts[mesh.base + a_vertex].inside = deepest;
		a_job.tested += tested ? 1 : 0;
		a_job.inside += deepest < 0.0f ? 1 : 0;
	}

	void FallSites::MeetDistance(const std::vector<CullSheet>& a_sheets, std::size_t a_sheet, uint32_t a_vertex, CullJob& a_job)
	{
		// D126: VertexClip d.w: distance from the drawn vertex to the nearest shown surface of any other sheet (after the inside cut),
		// up to MEET_REACH; NO_WATER beyond. The see-through pass foams where sheets meet. A triangle counts if any corner is shown.
		const auto binKey = [](int64_t a_x, int64_t a_y, int64_t a_z) { return (a_x & 0x1FFFFF) << 42 | (a_y & 0x1FFFFF) << 21 | (a_z & 0x1FFFFF); };
		const auto cellOf = [](float a_v) { return static_cast<int64_t>(std::floor(a_v / SEAM_BIN)); };
		const CullSheet& sheet = a_sheets[a_sheet];
		const RE::NiPoint3& p = sheet.drawn[a_vertex];
		// The bins hold rest positions; the drawn ones are moved by the seams and the swell, so look a little further
		const float reach = MEET_REACH + MEET_MOVE;
		float best = MEET_REACH * MEET_REACH;
		bool found = false;
		for (std::size_t o = 0; o < a_sheets.size(); o++) {
			const CullSheet& other = a_sheets[o];
			if (o == a_sheet || p.x < other.lo[0] - reach || p.x > other.hi[0] + reach || p.y < other.lo[1] - reach || p.y > other.hi[1] + reach || p.z < other.lo[2] - reach ||
				p.z > other.hi[2] + reach)
				continue;
			const int64_t x0 = cellOf(p.x - reach), x1 = cellOf(p.x + reach), y0 = cellOf(p.y - reach), y1 = cellOf(p.y + reach), z0 = cellOf(p.z - reach), z1 = cellOf(p.z + reach);
			for (int64_t x = x0; x <= x1; x++) {
				for (int64_t y = y0; y <= y1; y++) {
					for (int64_t z = z0; z <= z1; z++) {
						const auto it = other.bins.find(binKey(x, y, z));
						if (it == other.bins.end())
							continue;
						for (const uint32_t t : it->second) {
							const auto& tri = other.triangles[t];
							const RE::NiPoint3 &a = other.drawn[tri[0]], &b = other.drawn[tri[1]], &c = other.drawn[tri[2]];
							// D185: box test first; skip the exact distance when it cannot beat the best
							const float bx = std::max({ std::min({ a.x, b.x, c.x }) - p.x, p.x - std::max({ a.x, b.x, c.x }), 0.0f });
							const float by = std::max({ std::min({ a.y, b.y, c.y }) - p.y, p.y - std::max({ a.y, b.y, c.y }), 0.0f });
							const float bz = std::max({ std::min({ a.z, b.z, c.z }) - p.z, p.z - std::max({ a.z, b.z, c.z }), 0.0f });
							if (bx * bx + by * by + bz * bz >= best)
								continue;
							if (!VertexShown(other.base + tri[0]) && !VertexShown(other.base + tri[1]) && !VertexShown(other.base + tri[2]))
								continue;
							const float d = TriangleDistanceSquared(p, a, b, c);
							if (d < best) {
								best = d;
								found = true;
							}
						}
					}
				}
			}
		}
		vertexClips[sheet.base + a_vertex][15] = found ? std::sqrt(best) : NO_WATER;
		a_job.meeting += found ? 1 : 0;
	}

	void FallSites::SideFlags(const std::vector<CullSheet>& a_sheets, std::size_t a_sheet, uint32_t a_vertex, CullJob& a_job)
	{
		const auto binKey = [](int64_t a_x, int64_t a_y, int64_t a_z) { return (a_x & 0x1FFFFF) << 42 | (a_y & 0x1FFFFF) << 21 | (a_z & 0x1FFFFF); };
		const auto cellOf = [](float a_v) { return static_cast<int64_t>(std::floor(a_v / SEAM_BIN)); };
		const CullSheet& sheet = a_sheets[a_sheet];
		// D103: a vertex's inner side is hidden (D100's discard) only where another shown sheet lies behind it, outer side away from
		// the vertex. A sheet with air or rock behind still shows from inside (D100 hid it everywhere and the cliff showed through).
		const RE::NiPoint3& p = sheet.positions[a_vertex];
		const RE::NiPoint3 back = -sheet.outward[a_vertex];
		bool hidden = false;
		if (back.Length() > 0.5f) {
			a_job.sideTested++;
			const RE::NiPoint3 from = p + back * COVER_NEAR;
			const RE::NiPoint3 to = p + back * INSIDE_REACH;
			// The bins the ray passes, sampled at a quarter bin (a bin's triangles include every one whose bound touches it)
			std::vector<int64_t> cells;
			const int steps = static_cast<int>(std::ceil((INSIDE_REACH - COVER_NEAR) / (SEAM_BIN * 0.25f)));
			for (int i = 0; i <= steps; i++) {
				const RE::NiPoint3 q = from + (to - from) * (static_cast<float>(i) / static_cast<float>(steps));
				const int64_t key = binKey(cellOf(q.x), cellOf(q.y), cellOf(q.z));
				if (cells.empty() || cells.back() != key)
					cells.push_back(key);
			}
			const float lo[3] = { std::min(from.x, to.x), std::min(from.y, to.y), std::min(from.z, to.z) };
			const float hi[3] = { std::max(from.x, to.x), std::max(from.y, to.y), std::max(from.z, to.z) };
			float nearest = INSIDE_REACH - COVER_NEAR;
			bool hitShown = false;
			for (const auto& other : a_sheets) {
				if (lo[0] > other.hi[0] || hi[0] < other.lo[0] || lo[1] > other.hi[1] || hi[1] < other.lo[1] || lo[2] > other.hi[2] || hi[2] < other.lo[2])
					continue;
				for (const int64_t key : cells) {
					const auto it = other.bins.find(key);
					if (it == other.bins.end())
						continue;
					for (const uint32_t t : it->second) {
						const auto& tri = other.triangles[t];
						const float along = RayTriangle(from, back, other.positions[tri[0]], other.positions[tri[1]], other.positions[tri[2]], nearest);
						if (along < 0.0f)
							continue;
						nearest = along;
						// The first sheet behind decides: shown (D112: VertexShown, not only a.x; D113: not inside a third sheet), outer
						// side away
						const RE::NiPoint3 face = (other.positions[tri[1]] - other.positions[tri[0]]).Cross(other.positions[tri[2]] - other.positions[tri[0]]) * static_cast<float>(other.side[t]);
						bool shown = face.Dot(back) > 0.0f;
						for (const uint16_t k : tri)
							shown = shown && VertexShown(other.base + k);
						hitShown = shown;
					}
				}
			}
			hidden = hitShown;
		}
		const bool turned = sheet.shown[a_vertex].Dot(sheet.outward[a_vertex]) < 0.0f;
		a_job.inward += turned ? 1 : 0;
		a_job.covered += hidden ? 1 : 0;
		// a.w: 1 its foot (D71), +2 its normal faces in (D100), +4 its inner side is covered (D103)
		float& flags = vertexClips[sheet.base + a_vertex][3];
		flags = std::fmod(flags, 2.0f) + (turned ? 2.0f : 0.0f) + (hidden ? 4.0f : 0.0f);
	}

	std::unique_ptr<FallSites::BakeJob> FallSites::StartBake(RE::BSGeometry* a_geometry) const
	{
		auto* triShape = a_geometry ? a_geometry->AsTriShape() : nullptr;
		auto* renderer = a_geometry ? a_geometry->GetGeometryRuntimeData().rendererData : nullptr;
		if (!triShape || !renderer || !renderer->rawVertexData || !renderer->rawIndexData || !renderer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_UV))
			return nullptr;
		const auto& counts = triShape->GetTrishapeRuntimeData();
		const IndexSpan bakeIndices = BakeIndices(a_geometry);  // D172: the dense copy's untrimmed triangles
		const uint32_t stride = renderer->vertexDesc.GetSize();
		const uint32_t positionOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
		const uint32_t uvOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_TEXCOORD0);
		if (stride < positionOffset + 12 || counts.vertexCount == 0)
			return nullptr;
		std::vector<RE::NiPoint3> positions(counts.vertexCount);
		std::vector<std::array<float, 2>> uvs(counts.vertexCount);
		for (uint32_t v = 0; v < counts.vertexCount; v++) {
			const uint8_t* data = renderer->rawVertexData + static_cast<std::size_t>(v) * stride;
			float local[3];
			std::memcpy(local, data + positionOffset, sizeof(local));
			positions[v] = a_geometry->world * RE::NiPoint3(local[0], local[1], local[2]);
			const auto* uv = reinterpret_cast<const uint16_t*>(data + uvOffset);
			uvs[v] = { DirectX::PackedVector::XMConvertHalfToFloat(uv[0]), DirectX::PackedVector::XMConvertHalfToFloat(uv[1]) };
		}
		// The texture grid's extent, and its world units per texture coordinate along u and v, area weighted
		float lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
		for (const auto& uv : uvs) {
			for (int k = 0; k < 2; k++) {
				lo[k] = std::min(lo[k], uv[k]);
				hi[k] = std::max(hi[k], uv[k]);
			}
		}
		double sum[2]{}, area = 0.0;
		std::vector<std::array<uint32_t, 3>> triangles;
		for (uint32_t i = 0; i < bakeIndices.count; i++) {
			const uint16_t* index = bakeIndices.data + static_cast<std::size_t>(i) * 3;
			if (index[0] >= counts.vertexCount || index[1] >= counts.vertexCount || index[2] >= counts.vertexCount)
				continue;
			triangles.push_back({ index[0], index[1], index[2] });
			const RE::NiPoint3 e1 = positions[index[1]] - positions[index[0]];
			const RE::NiPoint3 e2 = positions[index[2]] - positions[index[0]];
			const float du1 = uvs[index[1]][0] - uvs[index[0]][0], dv1 = uvs[index[1]][1] - uvs[index[0]][1];
			const float du2 = uvs[index[2]][0] - uvs[index[0]][0], dv2 = uvs[index[2]][1] - uvs[index[0]][1];
			const float det = du1 * dv2 - du2 * dv1;
			const float faceArea = 0.5f * e1.Cross(e2).Length();
			if (std::abs(det) < 1e-8f || faceArea < 1e-3f)
				continue;
			sum[0] += static_cast<double>(((e1 * dv2 - e2 * dv1) / det).Length()) * faceArea;
			sum[1] += static_cast<double>(((e2 * du1 - e1 * du2) / det).Length()) * faceArea;
			area += faceArea;
		}
		if (area <= 0.0 || hi[0] <= lo[0] || hi[1] <= lo[1])
			return nullptr;
		auto job = std::make_unique<BakeJob>();
		job->geometry.reset(a_geometry);
		const float perU = static_cast<float>(sum[0] / area), perV = static_cast<float>(sum[1] / area);
		job->width = std::clamp(static_cast<uint32_t>(std::ceil((hi[0] - lo[0]) * perU / BAKE_TEXEL)), 4u, BAKE_MOST_SIDE);
		job->height = std::clamp(static_cast<uint32_t>(std::ceil((hi[1] - lo[1]) * perV / BAKE_TEXEL)), 4u, BAKE_MOST_SIDE);
		job->uvMin[0] = lo[0];
		job->uvMin[1] = lo[1];
		job->uvSize[0] = hi[0] - lo[0];
		job->uvSize[1] = hi[1] - lo[1];
		const std::size_t count = static_cast<std::size_t>(job->width) * job->height;
		job->position.assign(count, RE::NiPoint3(0.0f, 0.0f, 0.0f));
		job->normal.assign(count, RE::NiPoint3(0.0f, 0.0f, 1.0f));
		job->covered.assign(count, 0);
		job->rock.assign(count, 0);
		// Each triangle laid into the grid by its texture coordinates: the world position and facing at each texel centre
		for (const auto& tri : triangles) {
			const auto& a = uvs[tri[0]];
			const auto& b = uvs[tri[1]];
			const auto& c = uvs[tri[2]];
			const auto gx = [&](float a_u) { return (a_u - lo[0]) / job->uvSize[0] * static_cast<float>(job->width) - 0.5f; };
			const auto gy = [&](float a_v) { return (a_v - lo[1]) / job->uvSize[1] * static_cast<float>(job->height) - 0.5f; };
			const float ax = gx(a[0]), ay = gy(a[1]), bx = gx(b[0]), by = gy(b[1]), cx = gx(c[0]), cy = gy(c[1]);
			const float det = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
			if (std::abs(det) < 1e-6f)
				continue;
			RE::NiPoint3 face = (positions[tri[1]] - positions[tri[0]]).Cross(positions[tri[2]] - positions[tri[0]]);
			const float faceLength = face.Length();
			face = faceLength > 1e-6f ? face / faceLength : RE::NiPoint3(0.0f, 0.0f, 1.0f);
			const int32_t x0 = std::max(0, static_cast<int32_t>(std::floor(std::min({ ax, bx, cx }))));
			const int32_t x1 = std::min(static_cast<int32_t>(job->width) - 1, static_cast<int32_t>(std::ceil(std::max({ ax, bx, cx }))));
			const int32_t y0 = std::max(0, static_cast<int32_t>(std::floor(std::min({ ay, by, cy }))));
			const int32_t y1 = std::min(static_cast<int32_t>(job->height) - 1, static_cast<int32_t>(std::ceil(std::max({ ay, by, cy }))));
			for (int32_t y = y0; y <= y1; y++) {
				for (int32_t x = x0; x <= x1; x++) {
					const float px = static_cast<float>(x), py = static_cast<float>(y);
					const float w1 = ((px - ax) * (cy - ay) - (cx - ax) * (py - ay)) / det;
					const float w2 = ((bx - ax) * (py - ay) - (px - ax) * (by - ay)) / det;
					const float w0 = 1.0f - w1 - w2;
					if (w0 < -0.02f || w1 < -0.02f || w2 < -0.02f)
						continue;
					const std::size_t i = static_cast<std::size_t>(y) * job->width + x;
					job->covered[i] = 1;
					job->position[i] = positions[tri[0]] * w0 + positions[tri[1]] * w1 + positions[tri[2]] * w2;
					job->normal[i] = face;
				}
			}
		}
		return job;
	}

	void FallSites::GatherSolids(BakeJob& a_job) const
	{
		auto* tes = RE::TES::GetSingleton();
		if (!tes || a_job.position.empty())
			return;
		float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (std::size_t i = 0; i < a_job.position.size(); i++) {
			if (!a_job.covered[i])
				continue;
			const float c[3] = { a_job.position[i].x, a_job.position[i].y, a_job.position[i].z };
			for (int k = 0; k < 3; k++) {
				lo[k] = std::min(lo[k], c[k] - SOLID_REACH);
				hi[k] = std::max(hi[k], c[k] + SOLID_REACH);
			}
		}
		if (lo[0] > hi[0])
			return;
		uint32_t statics = 0;
		const auto visitCell = [&](RE::TESObjectCELL* a_cell, bool a_requireAttached) {
			if (!a_cell || (a_requireAttached && !a_cell->IsAttached()))
				return;
			a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
				if (!base || a_ref->IsDisabled() || a_ref->IsDeleted() || (base->GetFormType() != RE::FormType::Static && base->GetFormType() != RE::FormType::MovableStatic))
					return RE::BSContainer::ForEachResult::kContinue;
				auto* root = a_ref->Get3D();
				if (!root)
					return RE::BSContainer::ForEachResult::kContinue;
				const auto& bound = root->worldBound;
				if (bound.center.x + bound.radius < lo[0] || bound.center.x - bound.radius > hi[0] || bound.center.y + bound.radius < lo[1] || bound.center.y - bound.radius > hi[1] ||
					bound.center.z + bound.radius < lo[2] || bound.center.z - bound.radius > hi[2])
					return RE::BSContainer::ForEachResult::kContinue;
				// Not the falls themselves (their models carry rocks of their own, which do count)
				const std::string model = Lowercase(ModelOf(base));
				if (model.find("fxwaterfall") != std::string::npos)
					return RE::BSContainer::ForEachResult::kContinue;
				const auto ref = statics++;
				RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
					auto* triShape = a_geometry->AsTriShape();
					const auto& runtime = a_geometry->GetGeometryRuntimeData();
					auto* renderer = runtime.rendererData;
					if (!triShape || !renderer || !renderer->rawVertexData || !renderer->rawIndexData || !netimmerse_cast<RE::BSLightingShaderProperty*>(runtime.shaderProperty.get()) ||
						renderer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_SKINNED))
						return RE::BSVisit::BSVisitControl::kContinue;
					const auto& counts = triShape->GetTrishapeRuntimeData();
					const uint32_t stride = renderer->vertexDesc.GetSize();
					const uint32_t positionOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
					if (stride < positionOffset + 12)
						return RE::BSVisit::BSVisitControl::kContinue;
					std::vector<RE::NiPoint3> world(counts.vertexCount);
					for (uint32_t v = 0; v < counts.vertexCount; v++) {
						float local[3];
						std::memcpy(local, renderer->rawVertexData + static_cast<std::size_t>(v) * stride + positionOffset, sizeof(local));
						world[v] = a_geometry->world * RE::NiPoint3(local[0], local[1], local[2]);
					}
					for (uint32_t i = 0; i < counts.triangleCount; i++) {
						const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(i) * 3;
						if (index[0] >= counts.vertexCount || index[1] >= counts.vertexCount || index[2] >= counts.vertexCount)
							continue;
						const std::array<RE::NiPoint3, 3> tri = { world[index[0]], world[index[1]], world[index[2]] };
						if (std::max({ tri[0].x, tri[1].x, tri[2].x }) < lo[0] || std::min({ tri[0].x, tri[1].x, tri[2].x }) > hi[0] || std::max({ tri[0].y, tri[1].y, tri[2].y }) < lo[1] ||
							std::min({ tri[0].y, tri[1].y, tri[2].y }) > hi[1])
							continue;
						a_job.solids.push_back(tri);
						a_job.solidRef.push_back(ref);
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
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
		// Binned from above: each triangle in every bin its extent touches
		a_job.solidOrigin[0] = lo[0];
		a_job.solidOrigin[1] = lo[1];
		a_job.solidBins[0] = std::max(1, static_cast<int32_t>(std::ceil((hi[0] - lo[0]) / SOLID_BIN)));
		a_job.solidBins[1] = std::max(1, static_cast<int32_t>(std::ceil((hi[1] - lo[1]) / SOLID_BIN)));
		a_job.bins.assign(static_cast<std::size_t>(a_job.solidBins[0]) * a_job.solidBins[1], {});
		for (uint32_t t = 0; t < a_job.solids.size(); t++) {
			const auto& tri = a_job.solids[t];
			const int32_t x0 = std::clamp(static_cast<int32_t>((std::min({ tri[0].x, tri[1].x, tri[2].x }) - lo[0]) / SOLID_BIN), 0, a_job.solidBins[0] - 1);
			const int32_t x1 = std::clamp(static_cast<int32_t>((std::max({ tri[0].x, tri[1].x, tri[2].x }) - lo[0]) / SOLID_BIN), 0, a_job.solidBins[0] - 1);
			const int32_t y0 = std::clamp(static_cast<int32_t>((std::min({ tri[0].y, tri[1].y, tri[2].y }) - lo[1]) / SOLID_BIN), 0, a_job.solidBins[1] - 1);
			const int32_t y1 = std::clamp(static_cast<int32_t>((std::max({ tri[0].y, tri[1].y, tri[2].y }) - lo[1]) / SOLID_BIN), 0, a_job.solidBins[1] - 1);
			for (int32_t y = y0; y <= y1; y++) {
				for (int32_t x = x0; x <= x1; x++)
					a_job.bins[static_cast<std::size_t>(y) * a_job.solidBins[0] + x].push_back(t);
			}
		}
		logger::info("[Fluid Field]   bake: {} triangles of {} statics round the sheet", a_job.solids.size(), statics);
	}

	bool FallSites::InsideSolid(const BakeJob& a_job, const RE::NiPoint3& a_point, bool a_lying)
	{
		if (a_job.bins.empty())
			return false;
		const auto bx = static_cast<int32_t>(std::floor((a_point.x - a_job.solidOrigin[0]) / SOLID_BIN));
		const auto by = static_cast<int32_t>(std::floor((a_point.y - a_job.solidOrigin[1]) / SOLID_BIN));
		if (bx < 0 || by < 0 || bx >= a_job.solidBins[0] || by >= a_job.solidBins[1])
			return false;
		// Upward, per mesh: an odd count of its faces above means inside (an open buried underside is fine; an overhang counts twice)
		std::vector<std::pair<uint32_t, uint32_t>> crossings;
		float nearestAbove = FLT_MAX;
		for (const uint32_t t : a_job.bins[static_cast<std::size_t>(by) * a_job.solidBins[0] + bx]) {
			const auto& tri = a_job.solids[t];
			const float d = (tri[1].x - tri[0].x) * (tri[2].y - tri[0].y) - (tri[2].x - tri[0].x) * (tri[1].y - tri[0].y);
			if (std::abs(d) < 1e-6f)
				continue;
			const float w1 = ((a_point.x - tri[0].x) * (tri[2].y - tri[0].y) - (tri[2].x - tri[0].x) * (a_point.y - tri[0].y)) / d;
			const float w2 = ((tri[1].x - tri[0].x) * (a_point.y - tri[0].y) - (a_point.x - tri[0].x) * (tri[1].y - tri[0].y)) / d;
			if (w1 < 0.0f || w2 < 0.0f || w1 + w2 > 1.0f)
				continue;
			const float z = tri[0].z + (tri[1].z - tri[0].z) * w1 + (tri[2].z - tri[0].z) * w2;
			if (z <= a_point.z)
				continue;
			nearestAbove = std::min(nearestAbove, z - a_point.z);
			const uint32_t ref = a_job.solidRef[t];
			auto it = std::find_if(crossings.begin(), crossings.end(), [&](const auto& a_entry) { return a_entry.first == ref; });
			if (it == crossings.end())
				crossings.emplace_back(ref, 1u);
			else
				it->second++;
		}
		// D76: a lying (run, not curtain) texel with a rock's underside just above is under that rock (vanilla's sheet runs on beneath
		// boulders, an even crossing count); else the water drained through it instead of turning
		if (a_lying && nearestAbove < SOLID_COVER)
			return true;
		for (const auto& [ref, count] : crossings) {
			if (count % 2 == 0)
				continue;
			// Deeper inside it than a sheet lying on it dips (nearest face further than SOLID_MARGIN)
			bool deep = true;
			for (int32_t y = std::max(by - 1, 0); y <= std::min(by + 1, a_job.solidBins[1] - 1) && deep; y++) {
				for (int32_t x = std::max(bx - 1, 0); x <= std::min(bx + 1, a_job.solidBins[0] - 1) && deep; x++) {
					for (const uint32_t t : a_job.bins[static_cast<std::size_t>(y) * a_job.solidBins[0] + x]) {
						if (a_job.solidRef[t] != ref)
							continue;
						const auto& tri = a_job.solids[t];
						if (TriangleDistanceSquared(a_point, tri[0], tri[1], tri[2]) < SOLID_MARGIN * SOLID_MARGIN) {
							deep = false;
							break;
						}
					}
				}
			}
			if (deep)
				return true;
		}
		return false;
	}

	std::shared_ptr<FallSites::BakeField> FallSites::SolveBake(const BakeJob& a_job)
	{
		const uint32_t w = a_job.width, h = a_job.height;
		const auto iw = static_cast<int32_t>(w), ih = static_cast<int32_t>(h);
		const std::size_t count = static_cast<std::size_t>(w) * h;
		const auto index = [&](int32_t a_x, int32_t a_y) { return static_cast<std::size_t>(a_y) * w + a_x; };
		const auto inside = [&](int32_t a_x, int32_t a_y) { return a_x >= 0 && a_y >= 0 && a_x < iw && a_y < ih; };

		// D60: rock without specks: islands of BAKE_LEAST_ROCK texels or fewer are slivers of grazed collision, not boulders
		std::vector<uint8_t> rock = a_job.rock;
		// D68: plus rock from the statics' render meshes: a boulder on the ground failed the collision test (it wants rock on both sides,
		// and the ray from under it started in the ground). D82: both kept apart for the dump (render meshes alone moved the streaks too
		// much).
		std::vector<uint8_t> inMesh(count, 0);
		uint32_t meshRock = 0;
		for (std::size_t i = 0; i < count; i++) {
			if (!a_job.covered[i])
				continue;
			inMesh[i] = InsideSolid(a_job, a_job.position[i], std::abs(a_job.normal[i].z) > 0.5f) ? 1 : 0;
			if (!rock[i] && inMesh[i]) {
				rock[i] = 1;
				meshRock++;
			}
		}
		{
			std::vector<uint8_t> seen(count, 0);
			std::vector<std::size_t> island, open;
			for (std::size_t start = 0; start < count; start++) {
				if (!rock[start] || seen[start])
					continue;
				island.clear();
				open.assign(1, start);
				seen[start] = 1;
				while (!open.empty()) {
					const std::size_t i = open.back();
					open.pop_back();
					island.push_back(i);
					const auto x = static_cast<int32_t>(i % w), y = static_cast<int32_t>(i / w);
					static constexpr int32_t DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
					for (int k = 0; k < 4; k++) {
						const int32_t nx = x + DX[k], ny = y + DY[k];
						if (!inside(nx, ny) || seen[index(nx, ny)] || !rock[index(nx, ny)])
							continue;
						seen[index(nx, ny)] = 1;
						open.push_back(index(nx, ny));
					}
				}
				if (island.size() <= BAKE_LEAST_ROCK) {
					for (const std::size_t i : island)
						rock[i] = 0;
				}
			}
		}
		// D74 diagnostics: the sim's inputs beside the log, for the offline harness (scratchpad simbake.py)
		if (const auto directory = logger::log_directory()) {
			static std::atomic<int> dumps{ 0 };
			std::ofstream file(*directory / std::format("FluidField_bake{}.bin", dumps++), std::ios::binary);
			const uint32_t header[2] = { w, h };
			const float uv[4] = { a_job.uvMin[0], a_job.uvMin[1], a_job.uvSize[0], a_job.uvSize[1] };
			file.write(reinterpret_cast<const char*>(header), sizeof(header));
			file.write(reinterpret_cast<const char*>(uv), sizeof(uv));
			file.write(reinterpret_cast<const char*>(a_job.covered.data()), static_cast<std::streamsize>(count));
			file.write(reinterpret_cast<const char*>(rock.data()), static_cast<std::streamsize>(count));
			file.write(reinterpret_cast<const char*>(a_job.position.data()), static_cast<std::streamsize>(count * sizeof(RE::NiPoint3)));
			// D82: the rock tests apart: the collision's, the render meshes'
			file.write(reinterpret_cast<const char*>(a_job.rock.data()), static_cast<std::streamsize>(count));
			file.write(reinterpret_cast<const char*>(inMesh.data()), static_cast<std::streamsize>(count));
		}
		// D80: the water as a sheet of strands (FallStrands) from each column's crest (D79), parting round rock, never crossing;
		// the texture keeps vanilla's speed down the mesh, the strands only bend it
		FallStrands::Input input;
		input.width = w;
		input.height = h;
		input.texelV = a_job.uvSize[1] / static_cast<float>(h);
		input.position = reinterpret_cast<const float*>(a_job.position.data());
		input.covered = a_job.covered.data();
		input.rock = rock.data();
		auto strands = FallStrands::March(input);
		logger::info("[Fluid Field]   strands: {}x{} texels, {} strands from {} crest columns, {} reached the foot, {} wake texels, {} in hidden lead-ins, longest run {:.1f} s", w, h,
			strands.strands, strands.inflows, strands.reachedFoot, strands.openTexels, strands.leadTexels, strands.longestRun);
		const auto blur = [&](std::vector<float>& a_field, int a_passes) {
			std::vector<float> scratch(count);
			for (int pass = 0; pass < a_passes; pass++) {
				for (int32_t y = 0; y < ih; y++) {
					for (int32_t x = 0; x < iw; x++) {
						float total = 0.0f, weight = 0.0f;
						for (int32_t dy = -1; dy <= 1; dy++) {
							for (int32_t dx = -1; dx <= 1; dx++) {
								if (!inside(x + dx, y + dy))
									continue;
								const float k = (dx == 0 ? 2.0f : 1.0f) * (dy == 0 ? 2.0f : 1.0f);
								total += a_field[index(x + dx, y + dy)] * k;
								weight += k;
							}
						}
						scratch[index(x, y)] = total / weight;
					}
				}
				a_field.swap(scratch);
			}
		};
		// Foam on the rock's rim too (D60), where real water turns white anyway
		std::vector<float> foam = std::move(strands.foam);
		for (int32_t y = 0; y < ih; y++) {
			for (int32_t x = 0; x < iw; x++) {
				const std::size_t i = index(x, y);
				float rim = 0.0f;
				for (int32_t dy = -2; dy <= 2 && rim < 1.0f; dy++) {
					for (int32_t dx = -2; dx <= 2; dx++) {
						if (inside(x + dx, y + dy) && rock[index(x + dx, y + dy)] && a_job.covered[index(x + dx, y + dy)])
							rim = std::max(rim, 1.0f - 0.3f * static_cast<float>(std::max(std::abs(dx), std::abs(dy))));
					}
				}
				foam[i] = std::max(foam[i], rim);
			}
		}
		blur(foam, 2);  // D181: one pass showed low-res steps
		std::vector<float> aeration = std::move(strands.aeration);
		blur(aeration, 10);  // D179, D181, D186: fewer passes showed its patches' edges
		std::vector<float> turn = std::move(strands.turn);
		blur(turn, 1);
		std::vector<float> fallen = std::move(strands.fallen);
		blur(fallen, 4);  // D189: the shading's drop (a smooth ramp down the sheet)
		std::vector<float> calm = std::move(strands.calm);
		blur(calm, 4);

		auto field = std::make_shared<BakeField>();
		field->width = w;
		field->height = h;
		field->uvMin[0] = a_job.uvMin[0];
		field->uvMin[1] = a_job.uvMin[1];
		field->uvSize[0] = a_job.uvSize[0];
		field->uvSize[1] = a_job.uvSize[1];
		field->position = a_job.position;
		field->covered = a_job.covered;
		field->turn = std::move(turn);
		field->speed = std::move(strands.speed);
		field->wake = std::move(aeration);
		field->foam = std::move(foam);
		field->seed = std::move(strands.seed);
		field->along = std::move(strands.along);
		field->normal = a_job.normal;
		field->rock = rock;
		// D104: the clearances (untested texels, the hidden lead-in, count as open)
		field->clearFront = a_job.clearFront.size() == count ? a_job.clearFront : std::vector<float>(count, CLEAR_REACH);
		field->clearBack = a_job.clearBack.size() == count ? a_job.clearBack : std::vector<float>(count, CLEAR_REACH);
		// D82: the swell, softened to broad shapes (the vertices are 20 to 50 units apart)
		field->swell = std::move(strands.swell);
		blur(field->swell, PUSH_BLUR);
		// D85: the disturbance, softened alike
		field->disturbance = std::move(strands.disturbance);
		blur(field->disturbance, PUSH_BLUR);
		field->fallen = std::move(fallen);
		field->calm = std::move(calm);
		logger::info("[Fluid Field]   bake: {}x{} texels, {} more inside the statics' meshes", w, h, meshRock);
		if (!a_job.cache.empty() && !SaveBake(a_job.cache, *field))
			logger::warn("[Fluid Field]   bake cache: could not write {}", a_job.cache.string());
		return field;
	}

	std::string FallSites::CaptureCachePath(const FallCapture& a_capture, uint64_t a_key) const
	{
		const auto directory = logger::log_directory();
		if (!directory || a_capture.GetWidth() == 0)
			return {};
		const float lo[2] = { a_capture.GetOriginX(), a_capture.GetOriginY() };
		const float hi[2] = { lo[0] + a_capture.GetWidth() * FallCapture::CELL, lo[1] + a_capture.GetHeight() * FallCapture::CELL };
		std::vector<FallVisibility::Triangle> statics;
		GatherOccluders(lo[0], lo[1], hi[0], hi[1], statics);
		uint64_t hash = 14695981039346656037ull;
		const auto mix = [&](int64_t a_value) {
			const auto* bytes = reinterpret_cast<const uint8_t*>(&a_value);
			for (std::size_t i = 0; i < sizeof(a_value); i++)
				hash = (hash ^ bytes[i]) * 1099511628211ull;
		};
		mix(CAPTURE_CACHE_VERSION);
		mix(static_cast<int64_t>(a_key));
		mix(std::lround(lo[0]));
		mix(std::lround(lo[1]));
		mix(a_capture.GetWidth());
		mix(a_capture.GetHeight());
		// The statics' triangles summed in any order (the cells are visited from the player's side)
		uint64_t sum = 0, crossed = 0;
		for (const auto& triangle : statics) {
			uint64_t one = 14695981039346656037ull;
			for (const auto& corner : triangle) {
				for (const float c : { corner.x, corner.y, corner.z })
					one = (one ^ static_cast<uint64_t>(std::llround(c * 2.0f))) * 1099511628211ull;
			}
			sum += one;
			crossed ^= one * 0x9E3779B97F4A7C15ull;
		}
		mix(static_cast<int64_t>(sum));
		mix(static_cast<int64_t>(crossed));
		mix(static_cast<int64_t>(statics.size()));
		return (*directory / "FluidFieldBakes" / std::format("ground_{:016X}.bin", hash)).string();
	}

	std::filesystem::path FallSites::BakeCachePath(const BakeJob& a_job)
	{
		const auto directory = logger::log_directory();
		auto* geometry = a_job.geometry.get();
		if (!directory || !geometry)
			return {};
		RE::TESObjectREFR* ref = nullptr;
		for (RE::NiAVObject* object = geometry; object && !ref; object = object->parent)
			ref = object->GetUserData();
		if (!ref)
			return {};
		// FNV-1a over everything the bake depends on; static triangles summed in any order (cells are visited from the player's side)
		const auto fnv = [](uint64_t a_hash, const void* a_data, std::size_t a_size) {
			const auto* bytes = static_cast<const uint8_t*>(a_data);
			for (std::size_t i = 0; i < a_size; i++) {
				a_hash ^= bytes[i];
				a_hash *= 1099511628211ull;
			}
			return a_hash;
		};
		uint64_t hash = 14695981039346656037ull;
		const auto mix = [&](int64_t a_value) { hash = fnv(hash, &a_value, sizeof(a_value)); };
		const auto quantise = [](float a_value, float a_step) { return std::isfinite(a_value) ? static_cast<int64_t>(std::llround(a_value / a_step)) : INT64_MIN; };
		// The strand code's version too, so each build of it bakes afresh; other bake changes bump BAKE_CACHE_VERSION
		const std::string build = FallStrands::Version();
		hash = fnv(hash, build.data(), build.size());
		mix(BAKE_CACHE_VERSION);
		mix(ref->GetFormID());
		const std::string name = geometry->name.c_str();
		hash = fnv(hash, name.data(), name.size());
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++)
				mix(quantise(geometry->world.rotate.entry[r][c], 0.001f));
		}
		mix(quantise(geometry->world.translate.x, 0.1f));
		mix(quantise(geometry->world.translate.y, 0.1f));
		mix(quantise(geometry->world.translate.z, 0.1f));
		mix(quantise(geometry->world.scale, 0.001f));
		mix(a_job.width);
		mix(a_job.height);
		for (int k = 0; k < 2; k++) {
			mix(quantise(a_job.uvMin[k], 1e-4f));
			mix(quantise(a_job.uvSize[k], 1e-4f));
		}
		hash = fnv(hash, a_job.covered.data(), a_job.covered.size());
		uint64_t solidSum = 0, solidXor = 0;
		for (const auto& triangle : a_job.solids) {
			uint64_t one = 14695981039346656037ull;
			for (const auto& corner : triangle) {
				const int64_t q[3] = { quantise(corner.x, 0.5f), quantise(corner.y, 0.5f), quantise(corner.z, 0.5f) };
				one = fnv(one, q, sizeof(q));
			}
			solidSum += one;
			solidXor ^= one * 0x9E3779B97F4A7C15ull;
		}
		mix(static_cast<int64_t>(solidSum));
		mix(static_cast<int64_t>(solidXor));
		mix(static_cast<int64_t>(a_job.solids.size()));
		return *directory / "FluidFieldBakes" / std::format("{:08X}_{:016X}.bin", ref->GetFormID(), hash);
	}

	namespace
	{
		constexpr uint32_t BAKE_CACHE_MAGIC = 0x4B424646;  // "FFBK"
	}

	bool FallSites::SaveBake(const std::filesystem::path& a_path, const BakeField& a_field)
	{
		std::error_code error;
		std::filesystem::create_directories(a_path.parent_path(), error);
		auto partial = a_path;
		partial += ".part";
		{
			std::ofstream file(partial, std::ios::binary);
			if (!file)
				return false;
			const uint32_t header[4] = { BAKE_CACHE_MAGIC, BAKE_CACHE_VERSION, a_field.width, a_field.height };
			file.write(reinterpret_cast<const char*>(header), sizeof(header));
			file.write(reinterpret_cast<const char*>(a_field.uvMin), sizeof(a_field.uvMin));
			file.write(reinterpret_cast<const char*>(a_field.uvSize), sizeof(a_field.uvSize));
			const auto put = [&](const auto& a_vector) {
				const uint64_t size = a_vector.size();
				file.write(reinterpret_cast<const char*>(&size), sizeof(size));
				file.write(reinterpret_cast<const char*>(a_vector.data()), static_cast<std::streamsize>(size * sizeof(a_vector[0])));
			};
			put(a_field.position);
			put(a_field.covered);
			put(a_field.turn);
			put(a_field.speed);
			put(a_field.wake);
			put(a_field.foam);
			put(a_field.seed);
			put(a_field.along);
			put(a_field.normal);
			put(a_field.rock);
			put(a_field.swell);
			put(a_field.disturbance);
			put(a_field.clearFront);
			put(a_field.clearBack);
			put(a_field.fallen);  // D189
			put(a_field.calm);    // D190
			if (!file)
				return false;
		}
		std::filesystem::rename(partial, a_path, error);
		return !error;
	}

	std::shared_ptr<FallSites::BakeField> FallSites::LoadBake(const std::filesystem::path& a_path)
	{
		std::ifstream file(a_path, std::ios::binary);
		if (!file)
			return nullptr;
		uint32_t header[4]{};
		auto field = std::make_shared<BakeField>();
		file.read(reinterpret_cast<char*>(header), sizeof(header));
		file.read(reinterpret_cast<char*>(field->uvMin), sizeof(field->uvMin));
		file.read(reinterpret_cast<char*>(field->uvSize), sizeof(field->uvSize));
		if (!file || header[0] != BAKE_CACHE_MAGIC || header[1] != BAKE_CACHE_VERSION || header[2] == 0 || header[3] == 0 || header[2] > BAKE_MOST_SIDE || header[3] > BAKE_MOST_SIDE)
			return nullptr;
		field->width = header[2];
		field->height = header[3];
		const std::size_t count = static_cast<std::size_t>(field->width) * field->height;
		bool ok = true;
		const auto get = [&](auto& a_vector) {
			uint64_t size = 0;
			file.read(reinterpret_cast<char*>(&size), sizeof(size));
			if (!file || size != count) {
				ok = false;
				return;
			}
			a_vector.resize(count);
			file.read(reinterpret_cast<char*>(a_vector.data()), static_cast<std::streamsize>(count * sizeof(a_vector[0])));
			ok = ok && static_cast<bool>(file);
		};
		get(field->position);
		get(field->covered);
		get(field->turn);
		get(field->speed);
		get(field->wake);
		get(field->foam);
		get(field->seed);
		get(field->along);
		get(field->normal);
		get(field->rock);
		get(field->swell);
		get(field->disturbance);
		get(field->clearFront);
		get(field->clearBack);
		get(field->fallen);  // D189
		get(field->calm);    // D190
		// an offline site bake may add its flow direction (tools/unify/bake_site.py); the game's own bakes end here
		if (ok && file.peek() != std::char_traits<char>::eof()) {
			get(field->flow);
			if (!ok) {
				field->flow.clear();
				ok = true;
			}
		}
		// and then its streaks left (tools/unify/layers_to_ffbk.py)
		if (ok && !field->flow.empty() && file.peek() != std::char_traits<char>::eof()) {
			get(field->streak);
			if (!ok) {
				field->streak.clear();
				ok = true;
			}
		}
		// and its contact distance and that swept down the flow
		if (ok && !field->streak.empty() && file.peek() != std::char_traits<char>::eof()) {
			get(field->contact);
			get(field->contactTail);
			if (!ok) {
				field->contact.clear();
				field->contactTail.clear();
				ok = true;
			}
		}
		// and how hard its water heaves over hidden rock (its foam there)
		if (ok && !field->contactTail.empty() && file.peek() != std::char_traits<char>::eof()) {
			get(field->bulge);
			if (!ok) {
				field->bulge.clear();
				ok = true;
			}
		}
		return ok ? field : nullptr;
	}

	void FallSites::ComposeShifts()
	{
		float largest = 0.0f;
		uint32_t broken = 0, badGaps = 0, badPushes = 0;
		for (const auto& [geometry, base] : vertexClipBase) {
			const auto gap = gapShifts.find(geometry);
			const auto push = vertexPushes.find(geometry);
			const std::size_t gaps = gap != gapShifts.end() ? gap->second.size() : 0, pushes = push != vertexPushes.end() ? push->second.size() : 0;
			for (std::size_t v = 0; v < std::max(gaps, pushes) && base + v < vertexClips.size(); v++) {
				RE::NiPoint3 shift(0.0f, 0.0f, 0.0f);
				const auto bad = [&](const RE::NiPoint3& a_p) { return !std::isfinite(a_p.x) || !std::isfinite(a_p.y) || !std::isfinite(a_p.z) || a_p.Length() > MOST_SHIFT; };
				if (v < gaps) {
					badGaps += bad(gap->second[v]) ? 1 : 0;
					shift += gap->second[v];
				}
				if (v < pushes) {
					badPushes += bad(push->second[v]) ? 1 : 0;
					shift += push->second[v];
				}
				// D96: a move that is not finite or far past any push is a bug: dropped and counted
				if (!std::isfinite(shift.x) || !std::isfinite(shift.y) || !std::isfinite(shift.z) || shift.Length() > MOST_SHIFT) {
					shift = RE::NiPoint3(0.0f, 0.0f, 0.0f);
					broken++;
				}
				largest = std::max(largest, shift.Length());
				auto& value = vertexClips[base + v];
				value[4] = shift.x;
				value[5] = shift.y;
				value[6] = shift.z;
			}
		}
		// D186: normals of the pushed surface (seam moves and swell) for the see-through falls' light, which otherwise ignored the
		// swell: faces from the shifted vertices, by area, softened SHIFTED_NORMAL_PASSES times, each vertex keeping its own side
		for (const auto& [geometry, base] : vertexClipBase) {
			auto* triShape = geometry ? const_cast<RE::BSGeometry*>(geometry)->AsTriShape() : nullptr;
			auto* renderer = geometry ? const_cast<RE::BSGeometry*>(geometry)->GetGeometryRuntimeData().rendererData : nullptr;
			if (!triShape || !renderer || !renderer->rawVertexData || !renderer->rawIndexData || !renderer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_NORMAL))
				continue;
			const auto& counts = triShape->GetTrishapeRuntimeData();
			if (base + counts.vertexCount > vertexClips.size())
				continue;
			auto& desc = renderer->vertexDesc;
			const uint32_t stride = desc.GetSize();
			const uint32_t positionOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
			const uint32_t normalOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_NORMAL);
			std::vector<RE::NiPoint3> moved(counts.vertexCount), rest(counts.vertexCount);
			std::map<std::tuple<int64_t, int64_t, int64_t>, uint32_t> groupAt;
			std::vector<uint32_t> groupOf(counts.vertexCount);
			for (uint32_t v = 0; v < counts.vertexCount; v++) {
				const uint8_t* data = renderer->rawVertexData + static_cast<std::size_t>(v) * stride;
				float local[3];
				std::memcpy(local, data + positionOffset, sizeof(local));
				const auto& value = vertexClips[base + v];
				moved[v] = RE::NiPoint3(local[0] + value[4], local[1] + value[5], local[2] + value[6]);
				const uint8_t* n = data + normalOffset;
				rest[v] = RE::NiPoint3(n[0] / 255.0f * 2.0f - 1.0f, n[1] / 255.0f * 2.0f - 1.0f, n[2] / 255.0f * 2.0f - 1.0f);
				const auto key = std::make_tuple(std::llround(local[0] * 20.0), std::llround(local[1] * 20.0), std::llround(local[2] * 20.0));
				groupOf[v] = groupAt.try_emplace(key, static_cast<uint32_t>(groupAt.size())).first->second;
			}
			const std::size_t groups = groupAt.size();
			std::vector<RE::NiPoint3> sideOf(groups, RE::NiPoint3(0.0f, 0.0f, 0.0f)), current(groups, RE::NiPoint3(0.0f, 0.0f, 0.0f));
			for (uint32_t v = 0; v < counts.vertexCount; v++)
				sideOf[groupOf[v]] += sideOf[groupOf[v]].Dot(rest[v]) < 0.0f ? -rest[v] : rest[v];
			std::vector<std::vector<uint32_t>> neighbours(groups);
			for (uint32_t t = 0; t < counts.triangleCount; t++) {
				const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(t) * 3;
				if (index[0] >= counts.vertexCount || index[1] >= counts.vertexCount || index[2] >= counts.vertexCount)
					continue;
				const RE::NiPoint3 face = (moved[index[1]] - moved[index[0]]).Cross(moved[index[2]] - moved[index[0]]);
				for (int k = 0; k < 3; k++) {
					const uint32_t g = groupOf[index[k]], o = groupOf[index[(k + 1) % 3]];
					current[g] += face.Dot(sideOf[g]) < 0.0f ? -face : face;
					if (g != o) {
						neighbours[g].push_back(o);
						neighbours[o].push_back(g);
					}
				}
			}
			const auto unit = [](const RE::NiPoint3& a_v) { const float l = a_v.Length(); return l > 1e-8f ? a_v / l : a_v; };
			for (auto& n : current)
				n = unit(n);
			std::vector<RE::NiPoint3> next(groups);
			for (int pass = 0; pass < SHIFTED_NORMAL_PASSES; pass++) {
				for (std::size_t g = 0; g < groups; g++) {
					RE::NiPoint3 sum(0.0f, 0.0f, 0.0f);
					for (const uint32_t o : neighbours[g])
						sum += current[o];
					next[g] = neighbours[g].empty() ? current[g] : unit(current[g] + sum / static_cast<float>(neighbours[g].size()));
				}
				current.swap(next);
			}
			auto& normals = shiftedNormals[geometry];
			normals.resize(counts.vertexCount);
			for (uint32_t v = 0; v < counts.vertexCount; v++) {
				const RE::NiPoint3& n = current[groupOf[v]];
				normals[v] = n.Length() > 0.5f ? (n.Dot(rest[v]) >= 0.0f ? n : -n) : rest[v];
			}
		}
		std::erase_if(shiftedNormals, [&](const auto& a_entry) { return !vertexClipBase.contains(a_entry.first); });
		shiftedNormalsVersion++;
		vertexClipVersion++;
		logger::info("[Fluid Field]   vertex moves: largest {:.1f} units; {} broken (not finite or past {:.0f}) left still: {} from the seams, {} from the swell", largest, broken,
			MOST_SHIFT, badGaps, badPushes);
	}

	void FallSites::ComputePushes()
	{
		uint32_t meshes = 0, pushed = 0, wobbled = 0, flippedTotal = 0, leadCut = 0;
		float most = 0.0f, mostWobble = 0.0f;
		wobbleImpacts.clear();
		for (const auto& lit : litMeshList) {
			auto* geometry = lit.geometry.get();
			const auto field = bakeFields.find(geometry);
			auto* triShape = geometry ? geometry->AsTriShape() : nullptr;
			auto* renderer = geometry ? geometry->GetGeometryRuntimeData().rendererData : nullptr;
			if (field == bakeFields.end() || !field->second || field->second->swell.empty() || !vertexClipBase.contains(geometry) || !triShape || !renderer || !renderer->rawVertexData ||
				!renderer->rawIndexData || !renderer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_UV))
				continue;
			const BakeField& bake = *field->second;
			const auto& counts = triShape->GetTrishapeRuntimeData();
			const uint32_t stride = renderer->vertexDesc.GetSize();
			const uint32_t positionOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
			const uint32_t uvOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_TEXCOORD0);
			std::vector<RE::NiPoint3> positions(counts.vertexCount), normals(counts.vertexCount, RE::NiPoint3(0.0f, 0.0f, 0.0f));
			std::vector<std::array<float, 2>> uvs(counts.vertexCount);
			for (uint32_t v = 0; v < counts.vertexCount; v++) {
				const uint8_t* data = renderer->rawVertexData + static_cast<std::size_t>(v) * stride;
				float local[3];
				std::memcpy(local, data + positionOffset, sizeof(local));
				positions[v] = RE::NiPoint3(local[0], local[1], local[2]);
				const auto* uv = reinterpret_cast<const uint16_t*>(data + uvOffset);
				uvs[v] = { DirectX::PackedVector::XMConvertHalfToFloat(uv[0]), DirectX::PackedVector::XMConvertHalfToFloat(uv[1]) };
			}
			// Its normals (area weighted, by its winding: the face it shows) and its open edges
			std::map<std::pair<uint16_t, uint16_t>, int> uses;
			std::vector<std::array<uint16_t, 3>> triangles;
			for (uint32_t i = 0; i < counts.triangleCount; i++) {
				const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(i) * 3;
				if (index[0] >= counts.vertexCount || index[1] >= counts.vertexCount || index[2] >= counts.vertexCount)
					continue;
				triangles.push_back({ index[0], index[1], index[2] });
				const RE::NiPoint3 face = (positions[index[1]] - positions[index[0]]).Cross(positions[index[2]] - positions[index[0]]);
				for (int k = 0; k < 3; k++) {
					normals[index[k]] += face;
					uses[{ std::min(index[k], index[(k + 1) % 3]), std::max(index[k], index[(k + 1) % 3]) }]++;
				}
			}
			// D92: the outer face is the side the water travels toward. D103: carried over the sheet (OrientSheet), not per vertex: D92
			// turned a C-shaped sheet's lead-in into the curl. The shaders' outer-side flags are ComputeSides'.
			{
				std::vector<RE::NiPoint3> world(counts.vertexCount);
				for (uint32_t v = 0; v < counts.vertexCount; v++)
					world[v] = geometry->world * positions[v];
				const auto authored = AuthoredNormals(geometry);
				const auto side = OrientSheet(world, triangles, SheetTravel(world), &authored);
				std::vector<RE::NiPoint3> winding = normals;
				std::fill(normals.begin(), normals.end(), RE::NiPoint3(0.0f, 0.0f, 0.0f));
				for (std::size_t t = 0; t < triangles.size(); t++) {
					const auto& tri = triangles[t];
					const RE::NiPoint3 face = (positions[tri[1]] - positions[tri[0]]).Cross(positions[tri[2]] - positions[tri[0]]) * static_cast<float>(side[t]);
					for (int k = 0; k < 3; k++)
						normals[tri[k]] += face;
				}
				for (uint32_t v = 0; v < counts.vertexCount; v++)
					flippedTotal += normals[v].Dot(winding[v]) < 0.0f ? 1 : 0;
			}
			std::vector<uint8_t> open(counts.vertexCount, 0);
			for (const auto& [edge, count] : uses) {
				if (count == 1)
					open[edge.first] = open[edge.second] = 1;
			}
			// D89: no swell or wobble inside rock or ground (the bake's mask; they lifted the sheet out through boulders). D99: the mask
			// bilinear, inside from half: the nearest texel made a staircase of the boulder's outline.
			std::vector<uint8_t> inRock(counts.vertexCount, 0);
			if (bake.rock.size() == static_cast<std::size_t>(bake.width) * bake.height) {
				const auto rw = static_cast<int32_t>(bake.width), rh = static_cast<int32_t>(bake.height);
				for (uint32_t v = 0; v < counts.vertexCount; v++) {
					const float gx = std::clamp((uvs[v][0] - bake.uvMin[0]) / std::max(bake.uvSize[0], 1e-6f) * static_cast<float>(rw) - 0.5f, 0.0f, static_cast<float>(rw - 1));
					const float gy = std::clamp((uvs[v][1] - bake.uvMin[1]) / std::max(bake.uvSize[1], 1e-6f) * static_cast<float>(rh) - 0.5f, 0.0f, static_cast<float>(rh - 1));
					const int32_t x0 = std::min(static_cast<int32_t>(gx), rw - 1), y0 = std::min(static_cast<int32_t>(gy), rh - 1);
					const int32_t x1 = std::min(x0 + 1, rw - 1), y1 = std::min(y0 + 1, rh - 1);
					const float fx = gx - static_cast<float>(x0), fy = gy - static_cast<float>(y0);
					const auto at = [&](int32_t a_x, int32_t a_y) { return bake.rock[static_cast<std::size_t>(a_y) * rw + a_x] ? 1.0f : 0.0f; };
					inRock[v] = (at(x0, y0) * (1.0f - fx) + at(x1, y0) * fx) * (1.0f - fy) + (at(x0, y1) * (1.0f - fx) + at(x1, y1) * fx) * fy >= 0.5f;
				}
			}
			// The mesh's edges with their world lengths (D88's distances, D99's slope limit)
			std::vector<std::vector<std::pair<uint16_t, float>>> next(counts.vertexCount);
			for (const auto& triangle : triangles) {
				for (int k = 0; k < 3; k++) {
					const uint16_t a = triangle[k], b = triangle[(k + 1) % 3];
					const float length = (positions[a] - positions[b]).Length() * geometry->world.scale;
					next[a].emplace_back(b, length);
					next[b].emplace_back(a, length);
				}
			}
			// D99: the largest function under a_value rising at most a_slope world units per unit along the mesh, so no push stands up as a
			// spike over one triangle; zeros (open edges, rock) stay zero and the push ramps up from them. D186: a_fineSlope on edges under
			// DENSE_FINE_EDGE (near rock): small triangles carry a steeper rise, so the water piles up against rock, keeping the bake's
			// detail.
			const auto limitSlope = [&](std::vector<float>& a_value, float a_slope, float a_fineSlope) {
				using Entry = std::pair<float, uint16_t>;
				std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> frontier;
				for (uint32_t v = 0; v < counts.vertexCount; v++)
					frontier.emplace(a_value[v], static_cast<uint16_t>(v));
				while (!frontier.empty()) {
					const auto [value, v] = frontier.top();
					frontier.pop();
					if (value > a_value[v])
						continue;
					for (const auto& [n, length] : next[v]) {
						const float rise = (length < DENSE_FINE_EDGE ? a_fineSlope : a_slope) * length;
						if (value + rise < a_value[n]) {
							a_value[n] = value + rise;
							frontier.emplace(a_value[n], n);
						}
					}
				}
			};
			// D88: distance from the open edges along the mesh: the wobble fades toward them so the water stays pinned at its banks.
			// D89: and from rock, likewise
			std::vector<float> fromEdge(counts.vertexCount, FLT_MAX);
			{
				using Entry = std::pair<float, uint16_t>;
				std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> frontier;
				for (uint32_t v = 0; v < counts.vertexCount; v++) {
					if (open[v] || inRock[v]) {
						fromEdge[v] = 0.0f;
						frontier.emplace(0.0f, static_cast<uint16_t>(v));
					}
				}
				while (!frontier.empty()) {
					const auto [distance, v] = frontier.top();
					frontier.pop();
					if (distance > fromEdge[v] || distance > WOBBLE_EDGE_REACH)
						continue;
					for (const auto& [n, length] : next[v]) {
						if (distance + length < fromEdge[n]) {
							fromEdge[n] = distance + length;
							frontier.emplace(fromEdge[n], n);
						}
					}
				}
			}
			// D186: how far each vertex lies from rock, along the mesh, up to ROCK_REACH: the push rises off it by this (the bow wave)
			std::vector<float> fromRock(counts.vertexCount, FLT_MAX);
			{
				using Entry = std::pair<float, uint16_t>;
				std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> frontier;
				for (uint32_t v = 0; v < counts.vertexCount; v++) {
					if (inRock[v]) {
						fromRock[v] = 0.0f;
						frontier.emplace(0.0f, static_cast<uint16_t>(v));
					}
				}
				while (!frontier.empty()) {
					const auto [distance, v] = frontier.top();
					frontier.pop();
					if (distance > fromRock[v] || distance > ROCK_REACH)
						continue;
					for (const auto& [n, length] : next[v]) {
						if (distance + length < fromRock[n]) {
							fromRock[n] = distance + length;
							frontier.emplace(fromRock[n], n);
						}
					}
				}
			}
			// The swell at each vertex's texture coordinate (bilinear on the bake's grid), to a push along its normal
			const auto w = static_cast<int32_t>(bake.width), h = static_cast<int32_t>(bake.height);
			const auto swellAt = [&](float a_u, float a_v) {
				const float gx = std::clamp((a_u - bake.uvMin[0]) / std::max(bake.uvSize[0], 1e-6f) * static_cast<float>(w) - 0.5f, 0.0f, static_cast<float>(w - 1));
				const float gy = std::clamp((a_v - bake.uvMin[1]) / std::max(bake.uvSize[1], 1e-6f) * static_cast<float>(h) - 0.5f, 0.0f, static_cast<float>(h - 1));
				const int32_t x0 = std::min(static_cast<int32_t>(gx), w - 1), y0 = std::min(static_cast<int32_t>(gy), h - 1);
				const int32_t x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
				const float fx = gx - static_cast<float>(x0), fy = gy - static_cast<float>(y0);
				const auto at = [&](int32_t a_x, int32_t a_y) { return bake.swell[static_cast<std::size_t>(a_y) * w + a_x]; };
				return (at(x0, y0) * (1.0f - fx) + at(x1, y0) * fx) * (1.0f - fy) + (at(x0, y1) * (1.0f - fx) + at(x1, y1) * fx) * fy;
			};
			const float scale = std::max(geometry->world.scale, 1e-3f);
			auto& push = vertexPushes[geometry];
			push.assign(counts.vertexCount, RE::NiPoint3(0.0f, 0.0f, 0.0f));
			// D85: the wobble: per vertex its normal times its size (from how stirred the water is) and its place in the flow (the linked
			// bake's streak and run, as the texture reads), so the shaders move its waves down the flow with the texture
			const auto linked = flowBakes.find(geometry);
			const auto flowAt = [&](float a_u, float a_v, float& a_across, float& a_along) {
				const FlowBake& flow = linked->second;
				const auto fw = static_cast<int32_t>(flow.width), fh = static_cast<int32_t>(flow.height);
				const float gx = std::clamp((a_u - flow.uvMin[0]) / std::max(flow.uvSize[0], 1e-6f) * static_cast<float>(fw) - 0.5f, 0.0f, static_cast<float>(fw - 1));
				const float gy = std::clamp((a_v - flow.uvMin[1]) / std::max(flow.uvSize[1], 1e-6f) * static_cast<float>(fh) - 0.5f, 0.0f, static_cast<float>(fh - 1));
				const int32_t x0 = std::min(static_cast<int32_t>(gx), fw - 1), y0 = std::min(static_cast<int32_t>(gy), fh - 1);
				const int32_t x1 = std::min(x0 + 1, fw - 1), y1 = std::min(y0 + 1, fh - 1);
				const float fx = gx - static_cast<float>(x0), fy = gy - static_cast<float>(y0);
				const auto at = [&](int32_t a_x, int32_t a_y, int a_k) { return flow.texels[(static_cast<std::size_t>(a_y) * fw + a_x) * 4 + a_k]; };
				const auto mix = [&](int a_k) { return (at(x0, y0, a_k) * (1.0f - fx) + at(x1, y0, a_k) * fx) * (1.0f - fy) + (at(x0, y1, a_k) * (1.0f - fx) + at(x1, y1, a_k) * fx) * fy; };
				a_across = mix(0);
				a_along = mix(1);
			};
			// D191: how hard the water crashes there (0 to 1): the white part of a plunge, by its progress down the fall (plunge.w, written
			// just before; impacts come later, ComputePlunges), so crashing white water deforms more than glassy
			const auto crashAt = [&](float a_u, float a_v) {
				if (linked == flowBakes.end())
					return 0.0f;
				const FlowBake& flow = linked->second;
				const auto fw = static_cast<int32_t>(flow.width), fh = static_cast<int32_t>(flow.height);
				const std::size_t count = static_cast<std::size_t>(fw) * fh;
				if (flow.texels.size() != count * BAKE_TEXELS_PER)
					return 0.0f;
				const auto gx = std::clamp(static_cast<int32_t>(std::lround((a_u - flow.uvMin[0]) / std::max(flow.uvSize[0], 1e-6f) * static_cast<float>(fw) - 0.5f)), 0, fw - 1);
				const auto gy = std::clamp(static_cast<int32_t>(std::lround((a_v - flow.uvMin[1]) / std::max(flow.uvSize[1], 1e-6f) * static_cast<float>(fh) - 0.5f)), 0, fh - 1);
				const float* plunge = &flow.texels[(count * 3 + static_cast<std::size_t>(gy) * fw + gx) * 4];
				const float white = std::clamp((plunge[3] - WOBBLE_CRASH_FROM) / (WOBBLE_CRASH_FULL - WOBBLE_CRASH_FROM), 0.0f, 1.0f);
				const float crash = white * white * (3.0f - 2.0f * white);
				return std::isfinite(crash) ? crash : 0.0f;
			};
			const auto stirredAt = [&](float a_u, float a_v) {
				if (bake.disturbance.empty())
					return 0.0f;
				const float gx = std::clamp((a_u - bake.uvMin[0]) / std::max(bake.uvSize[0], 1e-6f) * static_cast<float>(w) - 0.5f, 0.0f, static_cast<float>(w - 1));
				const float gy = std::clamp((a_v - bake.uvMin[1]) / std::max(bake.uvSize[1], 1e-6f) * static_cast<float>(h) - 0.5f, 0.0f, static_cast<float>(h - 1));
				return bake.disturbance[static_cast<std::size_t>(std::lround(gy)) * w + std::lround(gx)];
			};
			const uint32_t base = vertexClipBase[geometry];
			// D95: cut the hidden lead-in (vanilla's sheet climbing under the upper water to its crest, D79), so a see-through sheet shows
			// no arch inside the river above it
			{
				FallStrands::Input input;
				input.width = bake.width;
				input.height = bake.height;
				input.position = reinterpret_cast<const float*>(bake.position.data());
				input.covered = bake.covered.data();
				std::vector<int32_t> crest;
				std::vector<uint8_t> leadIn;
				FallStrands::FindCrests(input, crest, leadIn);
				// D97: only under water (engine planes or our pools): a lead-in in the open is the sheet itself, and cutting it holed the
				// falls
				float lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
				for (uint32_t v = 0; v < counts.vertexCount; v++) {
					const RE::NiPoint3 world = geometry->world * positions[v];
					lo[0] = std::min(lo[0], world.x);
					lo[1] = std::min(lo[1], world.y);
					hi[0] = std::max(hi[0], world.x);
					hi[1] = std::max(hi[1], world.y);
				}
				std::vector<std::array<float, 5>> leadPlanes;
				CollectWaterPlanes(lo[0], lo[1], hi[0], hi[1], leadPlanes);
				// D99: the highest water over a point (our pools, engine planes), or none. D103: no plane above a_ceiling: a plane covers
				// its whole bound, so a river's or lake's plane higher up the cliff cut the visible top of a sheet standing in the open
				const auto waterOver = [&](const RE::NiPoint3& a_p, float a_ceiling) {
					float level = 0.0f, highest = -FLT_MAX;
					// D113: our pools have no ceiling: they are water confirmed over the rock (D40, D78), not a plane's bound, and the
					// river over a deeply submerged crest stands more than LEAD_WATER_OVER_CREST over it
					if (PoolLevelAt(a_p.x, a_p.y, level))
						highest = level;
					for (const auto& plane : leadPlanes) {
						if (a_p.x >= plane[0] && a_p.x <= plane[2] && a_p.y >= plane[1] && a_p.y <= plane[3] && plane[4] <= a_ceiling)
							highest = std::max(highest, plane[4]);
					}
					return highest;
				};
				for (uint32_t v = 0; v < counts.vertexCount && base + v < vertexClips.size() && base + v < vertexCuts.size(); v++) {
					const auto gx = std::clamp(static_cast<int32_t>(std::lround((uvs[v][0] - bake.uvMin[0]) / std::max(bake.uvSize[0], 1e-6f) * static_cast<float>(bake.width) - 0.5f)), 0,
						static_cast<int32_t>(bake.width) - 1);
					const auto gy = std::clamp(static_cast<int32_t>(std::lround((uvs[v][1] - bake.uvMin[1]) / std::max(bake.uvSize[1], 1e-6f) * static_cast<float>(bake.height) - 0.5f)), 0,
						static_cast<int32_t>(bake.height) - 1);
					// D112: redo the lead-in's part of the cut (ComposeCut makes a.x)
					vertexCuts[base + v].lead = NO_CUT;
					ComposeCut(base + v);
					if (!leadIn[static_cast<std::size_t>(gy) * bake.width + gx])
						continue;
					// D99: cut by signed height over the cut level, not a whole vertex at -CLIP_RANGE: the shaders interpolate linearly, so
					// a step between neighbours sawtoothed along the triangle edges; a height puts the cut on the waterline itself
					const RE::NiPoint3 world = geometry->world * positions[v];
					// D103: the water a lead-in hides under is the river feeding its crest, at about the crest's height
					const int32_t crestRow = crest[gx];
					const float crestHeight = crestRow >= 0 && bake.position.size() == static_cast<std::size_t>(bake.width) * bake.height ?
					                              bake.position[static_cast<std::size_t>(crestRow) * bake.width + gx].z :
					                              FLT_MAX;
					const float level = waterOver(world, crestHeight == FLT_MAX ? FLT_MAX : crestHeight + LEAD_WATER_OVER_CREST);
					if (level == -FLT_MAX)
						continue;
					const float height = std::max(world.z - (level - LEAD_CUT_DEPTH), -CLIP_RANGE);
					vertexCuts[base + v].lead = std::min(height, NO_CUT);
					ComposeCut(base + v);
					leadCut += height < 0.0f ? 1 : 0;
				}
			}
			// The wobble's size and the swell's push per vertex (world units along the normal): none on open edges, in rock or without
			// a normal. D99: each then held to a slope (limitSlope): spikes beside rock (D89, D84's pillow) and open edges made saw teeth.
			std::vector<float> wobbleSize(counts.vertexCount, 0.0f), swellPush(counts.vertexCount, 0.0f), wobbleWeight(counts.vertexCount, 0.0f);
			std::vector<float> bounce(counts.vertexCount, 0.0f);  // a unified sheet's vertical bob where aerated water faces up (crashing lips)
			const bool unified = unifiedShapes.contains(geometry);
			const auto aeratedAt = [&](float a_u, float a_v) {
				if (bake.wake.empty())
					return 0.0f;
				const float gx = std::clamp((a_u - bake.uvMin[0]) / std::max(bake.uvSize[0], 1e-6f) * static_cast<float>(w) - 0.5f, 0.0f, static_cast<float>(w - 1));
				const float gy = std::clamp((a_v - bake.uvMin[1]) / std::max(bake.uvSize[1], 1e-6f) * static_cast<float>(h) - 0.5f, 0.0f, static_cast<float>(h - 1));
				return bake.wake[static_cast<std::size_t>(std::lround(gy)) * w + std::lround(gx)];
			};
			for (uint32_t v = 0; v < counts.vertexCount; v++) {
				if (open[v] || inRock[v] || normals[v].Length() < 1e-6f)
					continue;
				if (linked != flowBakes.end()) {
					const float stirred = std::clamp(stirredAt(uvs[v][0], uvs[v][1]) / WOBBLE_STIRRED, 0.0f, 1.0f);
					const float inland = std::clamp(fromEdge[v] / WOBBLE_EDGE_REACH, 0.0f, 1.0f);
					// (D186: no churn toward rock: the flow coordinates jump where strands part round a boulder and spiked the fine mesh;
					// it fades toward rock instead, D89)
					wobbleSize[v] = (WOBBLE_CALM + (WOBBLE_MOST - WOBBLE_CALM) * stirred * stirred * (3.0f - 2.0f * stirred) + WOBBLE_CRASH * crashAt(uvs[v][0], uvs[v][1])) *
					                inland * inland * (3.0f - 2.0f * inland);
					wobbleWeight[v] = inland * inland * (3.0f - 2.0f * inland);
				}
				const float t = std::clamp((swellAt(uvs[v][0], uvs[v][1]) - PUSH_FROM) / (PUSH_FULL - PUSH_FROM), 0.0f, 1.0f);
				swellPush[v] = (unified ? UNIFIED_POOL_MOST : PUSH_MOST) * t * t * (3.0f - 2.0f * t);
				// a unified sheet: its baked turbulence (disturbance over TURB_SPAN) heaves the churned water on top of the rest, fading only
				// close to rock and the edges (the churn lies beside them)
				if (unified && linked != flowBakes.end()) {
					const float turb = std::clamp(stirredAt(uvs[v][0], uvs[v][1]) / TURB_SPAN, 0.0f, 1.0f);
					const float edgeAway = std::clamp(fromEdge[v] / (WOBBLE_EDGE_REACH * TURB_EDGE_SHARE), 0.0f, 1.0f);
					const float fade = edgeAway * edgeAway * (3.0f - 2.0f * edgeAway);
					const float churn = turb * turb * (3.0f - 2.0f * turb) * fade;
					wobbleSize[v] += TURB_WOBBLE * churn;
					wobbleWeight[v] = std::max(wobbleWeight[v], fade);
					swellPush[v] += TURB_PUSH * churn;
					// aerated water facing up or down (its world normal near vertical: lips and lobes, not plunging sheets) bobs vertically
					const RE::NiPoint3 worldNormal = geometry->world.rotate * normals[v];
					const float facing = std::abs(worldNormal.z) / std::max(worldNormal.Length(), 1e-6f);
					const float up = std::clamp((facing - BOUNCE_FACING[0]) / (BOUNCE_FACING[1] - BOUNCE_FACING[0]), 0.0f, 1.0f);
					const float aerated = std::clamp((aeratedAt(uvs[v][0], uvs[v][1]) - BOUNCE_AERATION[0]) / (BOUNCE_AERATION[1] - BOUNCE_AERATION[0]), 0.0f, 1.0f);
					bounce[v] = BOUNCE_MOST * aerated * aerated * (3.0f - 2.0f * aerated) * up * up * (3.0f - 2.0f * up) * fade;
					wobbleSize[v] *= UNIFIED_WOBBLE;
				}
			}
			// D186: none in a band along the rock, then rising off it by distance along the mesh (smooth, unlike the per-vertex rock mask,
			// which sawtoothed every contact on the fine mesh); then softened ROCK_FIELD_PASSES times, then held to their slopes
			for (uint32_t v = 0; v < counts.vertexCount; v++) {
				const float t = std::clamp((fromRock[v] - ROCK_CONTACT) / ROCK_RAMP, 0.0f, 1.0f);
				const float ramp = t * t * (3.0f - 2.0f * t);
				wobbleSize[v] *= ramp;
				bounce[v] *= ramp;
				wobbleWeight[v] *= ramp;
				// (a unified sheet's swell is its pooling: piled up against the rock, it bulges there rather than dipping into the corner)
				if (!unified)
					swellPush[v] *= ramp;
			}
			// D186: widely (water is smooth however stirred; it deforms by area, not locally): many passes, so it spreads tens of units
			for (int pass = 0; pass < ROCK_FIELD_PASSES; pass++) {
				for (auto* values : { &wobbleSize, &swellPush, &bounce }) {
					std::vector<float> softened = *values;
					for (uint32_t v = 0; v < counts.vertexCount; v++) {
						if (open[v] || inRock[v] || next[v].empty())
							continue;
						float sum = 0.0f;
						for (const auto& [n, length] : next[v])
							sum += (*values)[n];
						softened[v] = 0.5f * (*values)[v] + 0.5f * sum / static_cast<float>(next[v].size());
					}
					values->swap(softened);
				}
			}
			limitSlope(wobbleSize, WOBBLE_SLOPE, WOBBLE_SLOPE_FINE);
			limitSlope(swellPush, PUSH_SLOPE, PUSH_SLOPE_FINE);
			// D186: soften the wobble's coordinates too: where strands part round a boulder they jump between neighbours, which made
			// narrow, out-of-step ridges on the fine mesh
			std::vector<float> wobbleAcross(counts.vertexCount, 0.0f), wobbleAlong(counts.vertexCount, 0.0f);
			for (uint32_t v = 0; v < counts.vertexCount; v++)
				flowAt(uvs[v][0], uvs[v][1], wobbleAcross[v], wobbleAlong[v]);
			for (int pass = 0; pass < WOBBLE_COORD_PASSES; pass++) {
				for (auto* values : { &wobbleAcross, &wobbleAlong }) {
					std::vector<float> softened = *values;
					for (uint32_t v = 0; v < counts.vertexCount; v++) {
						if (next[v].empty())
							continue;
						float sum = 0.0f;
						for (const auto& [n, length] : next[v])
							sum += (*values)[n];
						softened[v] = 0.5f * (*values)[v] + 0.5f * sum / static_cast<float>(next[v].size());
					}
					values->swap(softened);
				}
			}
			// D188: world units per unit of the wobble's run coordinate at each vertex (from its neighbours), so the shaders can tilt
			// the light by the wobble's slope (d.x)
			std::vector<float> runScale(counts.vertexCount, 0.0f);
			for (uint32_t v = 0; v < counts.vertexCount; v++) {
				float distances = 0.0f, runs = 0.0f;
				for (const auto& [n, length] : next[v]) {
					const float run = std::abs(wobbleAlong[n] - wobbleAlong[v]);
					distances += length * run;
					runs += run * run;
				}
				runScale[v] = runs > 1e-10f ? distances / runs : 0.0f;
			}
			// D191: kept for the plunges' impacts (ApplyWobbleImpacts)
			auto& impact = wobbleImpacts[geometry];
			impact.size.assign(counts.vertexCount, 0.0f);
			impact.weight = std::move(wobbleWeight);
			impact.uv.resize(counts.vertexCount);
			impact.direction.assign(counts.vertexCount, RE::NiPoint3(0.0f, 0.0f, 0.0f));
			for (uint32_t v = 0; v < counts.vertexCount; v++) {
				const float length = normals[v].Length();
				impact.uv[v] = { uvs[v][0], uvs[v][1] };
				// the wobble along the normal, and the bounce straight up (world), as one vector per unit of size
				const float total = wobbleSize[v] + bounce[v];
				const RE::NiPoint3 upLocal = geometry->world.rotate.Transpose() * RE::NiPoint3(0.0f, 0.0f, 1.0f);
				const RE::NiPoint3 wobbleWay = total > 0.0f && length > 1e-6f ? (normals[v] * (wobbleSize[v] / length) + upLocal * bounce[v]) / (total * scale) : RE::NiPoint3();
				if (total > 0.0f && length > 1e-6f) {
					impact.size[v] = total;
					impact.direction[v] = wobbleWay;
				}
				if (base + v < vertexClips.size()) {
					auto& value = vertexClips[base + v];
					value[7] = value[8] = value[9] = value[10] = value[11] = 0.0f;
					value[12] = std::isfinite(runScale[v]) ? std::clamp(runScale[v], 0.0f, 4096.0f) : 0.0f;
					// D106: the depth behind (value[13]) comes from DepthStep's rays
					if (total > 0.0f && length > 1e-6f) {
						const float across = wobbleAcross[v], along = wobbleAlong[v];
						const RE::NiPoint3 wobble = wobbleWay * total;
						value[7] = across;
						wobbled++;
						mostWobble = std::max(mostWobble, total);
						value[8] = wobble.x;
						value[9] = wobble.y;
						value[10] = wobble.z;
						value[11] = along;
					}
				}
				if (swellPush[v] < 0.05f)
					continue;
				push[v] = normals[v] * (swellPush[v] / (length * scale));
				pushed++;
				most = std::max(most, swellPush[v]);
			}
			meshes++;
		}
		ComposeShifts();
		RequestCulling();  // D103: after the lead-in cuts, which the covering sheets' test reads
		logger::info("[Fluid Field] Waterfall swell: {} vertices of {} lit meshes pushed out, at most {:.1f} units; {} wobble, at most {:.1f} units; {} normals turned outward; {} hidden lead-in vertices cut",
			pushed, meshes, most, wobbled, mostWobble, flippedTotal, leadCut);
	}

	void FallSites::ProbeScrolls()
	{
		const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		const auto materialOf = [](RE::BSGeometry* a_geometry) -> RE::BSShaderMaterial* {
			auto* property = a_geometry ? netimmerse_cast<RE::BSLightingShaderProperty*>(a_geometry->GetGeometryRuntimeData().shaderProperty.get()) : nullptr;
			return property ? property->material : nullptr;
		};
		for (const auto& [key, copy] : denseOf) {
			auto& probe = scrollProbes[key];
			auto* original = materialOf(copy.original.get());
			auto* dense = materialOf(copy.shape.get());
			if (probe.logged || !original)
				continue;
			const float offset = original->texCoordOffset[0].y, denseOffset = dense ? dense->texCoordOffset[0].y : 0.0f;
			if (probe.start < 0.0) {
				probe.offset = offset;
				probe.copyOffset = denseOffset;
				probe.scale = original->texCoordScale[0].y;
				probe.start = probe.last = now;
				continue;
			}
			if (now - probe.last < 0.2)
				continue;
			float delta = offset - probe.offset, denseDelta = denseOffset - probe.copyOffset;
			delta -= std::round(delta);
			denseDelta -= std::round(denseDelta);
			probe.travelled += delta;
			probe.copyTravelled += denseDelta;
			probe.offset = offset;
			probe.copyOffset = denseOffset;
			probe.last = now;
			if (now - probe.start >= 6.0) {
				const auto seconds = static_cast<float>(now - probe.start);
				logger::info("[Fluid Field]   scroll probe: '{}' (its copy shares the material: {}): the hidden original scrolls {:.4f} a second (scale {:.3f}), the copy {:.4f}",
					copy.original ? copy.original->name.c_str() : "?", original == dense, -probe.travelled / seconds, probe.scale, -probe.copyTravelled / seconds);
				probe.logged = true;
			}
		}
	}

	void FallSites::LinkBakes()
	{
		// Every solved mesh: per column its first and last covered row, its extent and its top edge's height
		struct Node
		{
			const RE::BSGeometry* geometry = nullptr;
			const BakeField* field = nullptr;
			std::vector<int32_t> first, last;
			float lo[3]{ FLT_MAX, FLT_MAX, FLT_MAX }, hi[3]{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
			float top = -FLT_MAX;
			int32_t feeder = -1;
			std::vector<std::array<int32_t, 3>> links;  ///< Per top column fed: the column, and the feeder's texel (x, y)
			bool done = false;
			std::vector<float> across, along;
			std::vector<float> fallen;                                ///< D189: height fallen, carried on from its feeder like along
			float tangent = std::numeric_limits<float>::quiet_NaN();  ///< D190: world z where its fall turns steep (none: NaN)
			bool continues = false;                                   ///< D190: continues its feeder's fall (the tangent carried on)
			float foot = FLT_MAX;                                     ///< D190: the lowest its fall reaches, down the meshes it runs into
			float arriving = 0.0f;                                    ///< D190: the recent fall its water arrives with (for the log)
			bool unified = false;                                     ///< A site's unified sheet: across is its own flow uv
			std::vector<float> column;                                ///< A unified sheet's streak columns (its bake's seed, as u)
		};
		std::vector<Node> nodes;
		for (const auto& [geometry, field] : bakeFields) {
			if (!field || !grounded.contains(geometry))
				continue;
			Node node;
			node.geometry = geometry;
			node.field = field.get();
			node.unified = linkUnified.contains(geometry);
			const auto w = static_cast<int32_t>(field->width), h = static_cast<int32_t>(field->height);
			node.first.assign(w, -1);
			node.last.assign(w, -1);
			for (int32_t x = 0; x < w; x++) {
				for (int32_t y = 0; y < h; y++) {
					const std::size_t i = static_cast<std::size_t>(y) * w + x;
					if (!field->covered[i])
						continue;
					node.first[x] = node.first[x] < 0 ? y : node.first[x];
					node.last[x] = y;
					const float c[3] = { field->position[i].x, field->position[i].y, field->position[i].z };
					for (int k = 0; k < 3; k++) {
						node.lo[k] = std::min(node.lo[k], c[k]);
						node.hi[k] = std::max(node.hi[k], c[k]);
					}
				}
			}
			float sum = 0.0f, count = 0.0f;
			for (int32_t x = 0; x < w; x++) {
				if (node.first[x] >= 0) {
					sum += field->position[static_cast<std::size_t>(node.first[x]) * w + x].z;
					count += 1.0f;
				}
			}
			if (count == 0.0f)
				continue;
			node.top = sum / count;
			nodes.push_back(std::move(node));
		}
		// Each mesh's feeder: the higher mesh on whose lowest rows most of its top edge lies (a quarter of it at least)
		for (auto& node : nodes) {
			const BakeField& field = *node.field;
			const auto w = static_cast<int32_t>(field.width);
			const auto columns = static_cast<std::size_t>(std::count_if(node.first.begin(), node.first.end(), [](int32_t a_row) { return a_row >= 0; }));
			for (std::size_t a = 0; a < nodes.size(); a++) {
				const Node& above = nodes[a];
				if (&above == &node || above.top <= node.top + 16.0f)
					continue;
				bool apart = false;
				for (int k = 0; k < 3; k++)
					apart = apart || node.lo[k] > above.hi[k] + LINK_REACH || node.hi[k] < above.lo[k] - LINK_REACH;
				if (apart)
					continue;
				const BakeField& feeder = *above.field;
				const auto fw = static_cast<int32_t>(feeder.width);
				std::vector<std::array<int32_t, 3>> links;
				for (int32_t x = 0; x < w; x++) {
					if (node.first[x] < 0)
						continue;
					const RE::NiPoint3& p = field.position[static_cast<std::size_t>(node.first[x]) * w + x];
					float best = LINK_REACH * LINK_REACH;
					int32_t bx = -1, by = -1;
					for (int32_t fx = 0; fx < fw; fx++) {
						if (above.last[fx] < 0)
							continue;
						const int32_t rows = std::max(LINK_ROWS, (above.last[fx] - above.first[fx]) / 4);
						for (int32_t fy = std::max(above.first[fx], above.last[fx] - rows); fy <= above.last[fx]; fy++) {
							const std::size_t j = static_cast<std::size_t>(fy) * fw + fx;
							if (!feeder.covered[j])
								continue;
							const RE::NiPoint3 d = feeder.position[j] - p;
							const float distance = d.Dot(d);
							if (distance < best) {
								best = distance;
								bx = fx;
								by = fy;
							}
						}
					}
					if (bx >= 0)
						links.push_back({ x, bx, by });
				}
				if (links.size() >= std::max<std::size_t>(3, columns / 8) && links.size() > node.links.size()) {
					node.feeder = static_cast<int32_t>(a);
					node.links = std::move(links);
				}
			}
		}
		// D68 diagnostics: for a mesh nothing feeds, how near its top edge comes to any higher mesh, and where on it
		for (const auto& node : nodes) {
			if (node.feeder >= 0)
				continue;
			const BakeField& field = *node.field;
			const auto w = static_cast<int32_t>(field.width);
			float best = FLT_MAX, row = 0.0f;
			for (const auto& above : nodes) {
				if (&above == &node || above.top <= node.top + 16.0f)
					continue;
				const BakeField& feeder = *above.field;
				for (int32_t x = 0; x < w; x += 4) {
					if (node.first[x] < 0)
						continue;
					const RE::NiPoint3& p = field.position[static_cast<std::size_t>(node.first[x]) * w + x];
					for (std::size_t j = 0; j < feeder.position.size(); j += 3) {
						if (!feeder.covered[j])
							continue;
						const RE::NiPoint3 d = feeder.position[j] - p;
						const float distance = d.Dot(d);
						if (distance < best) {
							best = distance;
							row = static_cast<float>(j / feeder.width) / static_cast<float>(feeder.height);
						}
					}
				}
			}
			logger::info("[Fluid Field]   link: a mesh with its top at {:.0f} is fed by none: the nearest higher mesh is {:.0f} units from its top edge, {:.0f}% down it", node.top,
				best < FLT_MAX ? std::sqrt(best) : -1.0f, row * 100.0f);
		}
		// Each mesh swept down from its top edge once its feeder is (meshes in a loop, if any, start as tops)
		uint32_t linked = 0;
		const auto sweep = [&](Node& a_node) {
			const BakeField& field = *a_node.field;
			const auto w = static_cast<int32_t>(field.width), h = static_cast<int32_t>(field.height);
			const std::size_t count = static_cast<std::size_t>(w) * h;
			const float texelU = field.uvSize[0] / static_cast<float>(w);
			const auto u = [&](int32_t a_x) { return field.uvMin[0] + (static_cast<float>(a_x) + 0.5f) * texelU; };
			// Inflow at each column's top: from the feeder where it reaches; elsewhere u shifted by the fed columns' offset (D80) with
			// their mean run time; on a top, the mesh's own u and no time yet
			std::vector<float> across0(w, 0.0f), along0(w, 0.0f), fallen0(w, 0.0f);
			std::vector<uint8_t> fed(w, 0);
			double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, sumAlong = 0.0, sumFallen = 0.0;
			const Node* feeder = a_node.feeder >= 0 && nodes[a_node.feeder].done ? &nodes[a_node.feeder] : nullptr;
			if (feeder) {
				for (const auto& link : a_node.links) {
					const std::size_t j = static_cast<std::size_t>(link[2]) * feeder->field->width + link[1];
					across0[link[0]] = feeder->across[j];
					along0[link[0]] = feeder->along[j];
					fallen0[link[0]] = feeder->fallen.size() > j ? feeder->fallen[j] : 0.0f;
					sumFallen += fallen0[link[0]];
					fed[link[0]] = 1;
					sx += u(link[0]);
					sy += across0[link[0]];
					sxx += static_cast<double>(u(link[0])) * u(link[0]);
					sxy += static_cast<double>(u(link[0])) * across0[link[0]];
					sumAlong += along0[link[0]];
				}
				linked++;
			}
			const double n = feeder ? static_cast<double>(a_node.links.size()) : 0.0;
			double slope = 1.0, intercept = 0.0, meanAlong = 0.0, meanFallen = 0.0;
			if (n >= 1.0) {
				const double denominator = n * sxx - sx * sx;
				slope = n >= 2.0 && std::abs(denominator) > 1e-9 ? (n * sxy - sx * sy) / denominator : 1.0;
				// D80: keep the mesh's own texture scale and direction (a slope fitted to the feeder's foot mirrored, stretched or turned
				// it); the feeder gives only the shift
				const double fitted = slope;
				slope = 1.0;
				intercept = (sy - slope * sx) / n;
				logger::info("[Fluid Field]   link: a mesh fed at {} columns (its feeder's streaks would have fitted a slope of {:.2f})", a_node.links.size(), fitted);
				meanAlong = sumAlong / n;
				meanFallen = sumFallen / n;
			}
			for (int32_t x = 0; x < w; x++) {
				if (fed[x])
					continue;
				across0[x] = static_cast<float>(slope * u(x) + intercept);
				along0[x] = static_cast<float>(meanAlong);
				fallen0[x] = static_cast<float>(meanFallen);
			}
			// D80: each texel's streak and run from the lip column its strand came over (between columns, in proportion)
			const auto atLip = [&](const std::vector<float>& a_line, float a_x) {
				const float x = std::clamp(a_x, 0.0f, static_cast<float>(w - 1));
				const auto x0 = std::min(static_cast<int32_t>(x), w - 1);
				const int32_t x1 = std::min(x0 + 1, w - 1);
				const float f = x - static_cast<float>(x0);
				return a_line[x0] * (1.0f - f) + a_line[x1] * f;
			};
			a_node.across.assign(count, 0.0f);
			a_node.along.assign(count, 0.0f);
			a_node.fallen.assign(count, 0.0f);
			for (std::size_t i = 0; i < count; i++) {
				const float seed = field.seed.size() == count ? field.seed[i] : static_cast<float>(i % w);
				// A unified sheet: across is its own flow uv (even: the textures keep their scale), and its streaks' columns (the offline
				// bake's seed: straight down, parted by rock) go to the ribbons alone (FallGround's column), where uneven spacing does no harm
				a_node.across[i] = a_node.unified ? u(static_cast<int32_t>(i % w)) : atLip(across0, seed);
				if (a_node.unified) {
					a_node.column.resize(count);
					a_node.column[i] = atLip(across0, seed);
				}
				a_node.along[i] = atLip(along0, seed) + (field.along.size() == count ? field.along[i] : 0.0f);
				// D190: what it carried in clears over its calm runs here, as the strands' own fall does
				const float calm = field.calm.size() == count ? field.calm[i] : 0.0f;
				a_node.fallen[i] = atLip(fallen0, seed) * std::exp(-calm / FallStrands::CALM_RUN) + (field.fallen.size() == count ? field.fallen[i] : 0.0f);
			}
			// D190: where its water turns steep (its tangent, world z), where the falls' look starts (clear from crest to tangent, aerated
			// to midway, white below). The feeder's while the water is still falling, so one fall's meshes agree; its own after calm water.
			a_node.arriving = static_cast<float>(meanFallen);
			if (feeder && !std::isnan(feeder->tangent) && meanFallen > TANGENT_KEEP_FALLEN) {
				a_node.tangent = feeder->tangent;
				a_node.continues = true;
			} else if (field.normal.size() == count) {
				std::vector<float> turns;
				std::size_t columns = 0;
				for (int32_t x = 0; x < w; x++) {
					if (a_node.first[x] < 0)
						continue;
					columns++;
					for (int32_t y = a_node.first[x]; y <= a_node.last[x]; y++) {
						const std::size_t i = static_cast<std::size_t>(y) * w + x;
						if (field.covered[i] && std::abs(field.normal[i].z) < TANGENT_STEEP) {
							turns.push_back(field.position[i].z);
							break;
						}
					}
				}
				// A fall where a quarter of its columns turn steep at least, at their median
				if (!turns.empty() && turns.size() * 4 >= columns) {
					std::nth_element(turns.begin(), turns.begin() + turns.size() / 2, turns.end());
					a_node.tangent = turns[turns.size() / 2];
				}
			}
			// D190: a mesh with no fall of its own below a feeder's carries the fall on, however calm its inflow (else it read grey to its
			// foot)
			if (std::isnan(a_node.tangent) && feeder && !std::isnan(feeder->tangent)) {
				a_node.tangent = feeder->tangent;
				a_node.continues = true;
			}
			FlowBake bake;
			bake.width = field.width;
			bake.height = field.height;
			bake.uvMin[0] = field.uvMin[0];
			bake.uvMin[1] = field.uvMin[1];
			bake.uvSize[0] = field.uvSize[0];
			bake.uvSize[1] = field.uvSize[1];
			// D181: the flow coordinates' stretch per texel (world length per unit along over per unit across), from the texels' positions,
			// centred, softened; the foam is drawn round on it (screen derivatives were constant per triangle and jumped at every edge)
			std::vector<float> stretch(count, 1.0f);
			if (field.position.size() == count) {
				const auto rate = [&](std::size_t a_i, std::size_t a_j, const std::vector<float>& a_coord) {
					const float d = std::abs(a_coord[a_j] - a_coord[a_i]);
					return d > 1e-6f ? (field.position[a_j] - field.position[a_i]).Length() / d : 0.0f;
				};
				for (int32_t y = 0; y < static_cast<int32_t>(h); y++) {
					for (int32_t x = 0; x < w; x++) {
						const std::size_t i = static_cast<std::size_t>(y) * w + x;
						const std::size_t right = static_cast<std::size_t>(y) * w + std::min(x + 1, w - 1), left = static_cast<std::size_t>(y) * w + std::max(x - 1, 0);
						const std::size_t down = static_cast<std::size_t>(std::min(y + 1, static_cast<int32_t>(h) - 1)) * w + x, up = static_cast<std::size_t>(std::max(y - 1, 0)) * w + x;
						const float perAcross = rate(left, right, a_node.across), perAlong = rate(up, down, a_node.along);
						if (perAcross > 1e-4f && perAlong > 1e-4f)
							stretch[i] = std::clamp(perAlong / perAcross, 0.25f, 4.0f);
					}
				}
				std::vector<float> scratch(count);
				for (int pass = 0; pass < 4; pass++) {
					for (int32_t y = 0; y < static_cast<int32_t>(h); y++) {
						for (int32_t x = 0; x < w; x++) {
							float total = 0.0f, weight = 0.0f;
							for (int32_t dy = -1; dy <= 1; dy++) {
								for (int32_t dx = -1; dx <= 1; dx++) {
									const int32_t nx = x + dx, ny = y + dy;
									if (nx < 0 || ny < 0 || nx >= w || ny >= static_cast<int32_t>(h))
										continue;
									const float k = (dx == 0 ? 2.0f : 1.0f) * (dy == 0 ? 2.0f : 1.0f);
									total += stretch[static_cast<std::size_t>(ny) * w + nx] * k;
									weight += k;
								}
							}
							scratch[static_cast<std::size_t>(y) * w + x] = total / weight;
						}
					}
					stretch.swap(scratch);
				}
			}
			// D186: the flow's frame per texel: the world direction along runs with across held (the normal maps' frame, as they run on the
			// flow coordinates), from changes over FRAME_REACH texels each way (wide enough to hide the original's coarse edges), softened;
			// zero where unknown (the shader keeps the mesh's own tangent). Per triangle, it creased the water.
			std::vector<RE::NiPoint3> frame(count, RE::NiPoint3(0.0f, 0.0f, 0.0f));
			if (field.position.size() == count && field.covered.size() == count) {
				const int32_t hh = static_cast<int32_t>(h);
				const auto coveredAt = [&](int32_t a_x, int32_t a_y) { return a_x >= 0 && a_y >= 0 && a_x < w && a_y < hh && field.covered[static_cast<std::size_t>(a_y) * w + a_x]; };
				// How many covered texels in a row there are from one, toward a side, up to FRAME_REACH
				const auto reach = [&](int32_t a_x, int32_t a_y, int32_t a_dx, int32_t a_dy) {
					int32_t k = 0;
					while (k < FRAME_REACH && coveredAt(a_x + a_dx * (k + 1), a_y + a_dy * (k + 1)))
						k++;
					return k;
				};
				for (int32_t y = 0; y < hh; y++) {
					for (int32_t x = 0; x < w; x++) {
						const std::size_t i = static_cast<std::size_t>(y) * w + x;
						if (!field.covered[i])
							continue;
						// a unified sheet's baked flow direction (offline: the strands' own way, smoothed), where it has one
						if (field.flow.size() == count) {
							const float length = field.flow[i].Length();
							if (std::isfinite(length) && length > 1e-6f) {
								frame[i] = field.flow[i] / length;
								continue;
							}
						}
						const int32_t l = reach(x, y, -1, 0), r = reach(x, y, 1, 0), above = reach(x, y, 0, -1), d = reach(x, y, 0, 1);
						if (l + r == 0 || above + d == 0)
							continue;
						const std::size_t il = i - l, ir = i + r, iu = i - static_cast<std::size_t>(above) * w, id = i + static_cast<std::size_t>(d) * w;
						// Per texel across the grid (x) and down it (y): the positions' change, and the coordinates'
						const RE::NiPoint3 px = (field.position[ir] - field.position[il]) / static_cast<float>(l + r);
						const RE::NiPoint3 py = (field.position[id] - field.position[iu]) / static_cast<float>(above + d);
						const float ax = (a_node.across[ir] - a_node.across[il]) / static_cast<float>(l + r), ay = (a_node.across[id] - a_node.across[iu]) / static_cast<float>(above + d);
						const float bx = (a_node.along[ir] - a_node.along[il]) / static_cast<float>(l + r), by = (a_node.along[id] - a_node.along[iu]) / static_cast<float>(above + d);
						// The grid's step that moves along by one with across held: (dx, dy) solving ax dx + ay dy = 0, bx dx + by dy = 1
						const float det = ax * by - ay * bx;
						const RE::NiPoint3 way = std::abs(det) > 1e-8f ? px * (-ay / det) + py * (ax / det) : py;
						const float length = way.Length();
						if (std::isfinite(length) && length > 1e-6f)
							frame[i] = way / length;
					}
				}
				std::vector<RE::NiPoint3> scratch(count);
				for (int pass = 0; pass < FRAME_BLUR; pass++) {
					for (int32_t y = 0; y < hh; y++) {
						for (int32_t x = 0; x < w; x++) {
							const std::size_t i = static_cast<std::size_t>(y) * w + x;
							RE::NiPoint3 total(0.0f, 0.0f, 0.0f);
							for (int32_t dy = -1; dy <= 1; dy++) {
								for (int32_t dx = -1; dx <= 1; dx++) {
									const int32_t nx = x + dx, ny = y + dy;
									if (nx < 0 || ny < 0 || nx >= w || ny >= hh)
										continue;
									total += frame[static_cast<std::size_t>(ny) * w + nx] * ((dx == 0 ? 2.0f : 1.0f) * (dy == 0 ? 2.0f : 1.0f));
								}
							}
							const float length = total.Length();
							scratch[i] = frame[i].Length() > 0.0f && length > 1e-6f ? total / length : frame[i];
						}
					}
					frame.swap(scratch);
				}
			}
			// D179: four sections. 1: the flow. 2: for the shading (FallGround's BakeFieldsAt): speed over vanilla's, swell, stirring,
			// aeration. 3 (D181): x the coordinates' stretch; D186: yzw the flow's frame. 4 (D188): x where the water lands in a pool (how
			// hard), y the foam it leaves on its way, both filled by ComputePlunges (ApplyPlunges: where it meets water below, not another
			// sheet); D189: z the height fallen down steep faces from the top of the falls, for the shading's drop; D190: w its own drop's
			// progress.
			bake.texels.resize(count * BAKE_TEXELS_PER);
			const auto fieldAt = [&](const std::vector<float>& a_values, std::size_t a_i, float a_none) { return a_values.size() == count ? a_values[a_i] : a_none; };
			for (std::size_t i = 0; i < count; i++) {
				bake.texels[i * 4] = a_node.across[i];
				bake.texels[i * 4 + 1] = a_node.along[i];
				// a unified sheet's third channel holds its streak columns (its wake is the aeration, also in the fields' w)
				bake.texels[i * 4 + 2] = a_node.unified && a_node.column.size() == count ? a_node.column[i] : std::clamp(field.wake[i], 0.0f, 1.0f);
				bake.texels[i * 4 + 3] = std::clamp(field.foam[i], 0.0f, 1.0f);
				float* extra = &bake.texels[(count + i) * 4];
				extra[0] = fieldAt(field.speed, i, 1.0f);
				extra[1] = 1.0f - std::exp(-std::max(fieldAt(field.swell, i, 0.0f), 0.0f));  // D179: to 0-1 (raw values saturated)
				extra[2] = fieldAt(field.disturbance, i, 0.0f);
				extra[3] = std::clamp(fieldAt(field.wake, i, 0.0f), 0.0f, 1.0f);
				float* third = &bake.texels[(count * 2 + i) * 4];
				third[0] = a_node.unified && field.streak.size() == count ? field.streak[i] : stretch[i];  // (a unified sheet's stretch is the shader's own)
				third[1] = frame[i].x;                                                                     // D186: the flow's frame, world axes (zero: none)
				third[2] = frame[i].y;
				third[3] = frame[i].z;
				float* fourth = &bake.texels[(count * 3 + i) * 4];
				fourth[0] = fourth[1] = 0.0f;
				fourth[2] = a_node.fallen[i];
				fourth[3] = -1.0f;  // D190: the fall's progress, once all are linked (below)
				// a unified sheet's contact: its distance to rock, land or the pool along the sheet, and that swept down its flow (world
				// units; far where it has none)
				float* fifth = &bake.texels[(count * 4 + i) * 4];
				fifth[0] = a_node.unified && field.contact.size() == count ? field.contact[i] : CONTACT_NONE;
				fifth[1] = a_node.unified && field.contactTail.size() == count ? field.contactTail[i] : CONTACT_NONE;
				fifth[2] = a_node.unified && field.bulge.size() == count ? field.bulge[i] : 0.0f;
				fifth[3] = 0.0f;
			}
			flowBakes[a_node.geometry] = std::move(bake);
			a_node.done = true;
		};
		for (std::size_t pass = 0; pass <= nodes.size(); pass++) {
			bool any = false;
			for (auto& node : nodes) {
				if (node.done || (node.feeder >= 0 && !nodes[node.feeder].done && pass < nodes.size()))
					continue;
				sweep(node);
				any = true;
			}
			if (!any)
				break;
		}
		// D190: each fall's foot (the lowest its water reaches, carried up from the meshes it runs into) and each texel's progress from
		// tangent (0) to foot (1), times its height up to PLUNGE_WHITE_HEIGHT (a short step whitens only partly); above the tangent, the
		// height still to go over the fall's, negative down to -PROGRESS_ABOVE_MOST; -1 where the mesh never turns steep
		for (auto& node : nodes)
			node.foot = node.lo[2];
		for (std::size_t pass = 0; pass < nodes.size(); pass++) {
			bool changed = false;
			for (const auto& node : nodes) {
				if (node.continues && node.feeder >= 0 && node.foot < nodes[node.feeder].foot) {
					nodes[node.feeder].foot = node.foot;
					changed = true;
				}
			}
			if (!changed)
				break;
		}
		// D191: each mesh's progress first, then what its feeder's water carries in (below), then written
		std::vector<std::vector<float>> progressOf(nodes.size());
		for (std::size_t n = 0; n < nodes.size(); n++) {
			const Node& node = nodes[n];
			const auto bake = flowBakes.find(node.geometry);
			const BakeField& field = *node.field;
			const auto w = static_cast<int32_t>(field.width), h = static_cast<int32_t>(field.height);
			const std::size_t count = static_cast<std::size_t>(w) * h;
			if (bake == flowBakes.end() || bake->second.texels.size() != count * BAKE_TEXELS_PER || std::isnan(node.tangent))
				continue;
			const float span = std::max(node.tangent - node.foot, 1.0f);
			const float tall = std::min(span / PLUNGE_WHITE_HEIGHT, 1.0f);
			std::vector<float> progress(count, -1.0f);
			for (int32_t x = 0; x < w; x++) {
				// Covered texels by their height; the rest of the column as its nearest covered one (the texture filter reaches them)
				int32_t lastKnown = -1;
				float calmAbove = std::numeric_limits<float>::quiet_NaN();
				for (int32_t y = 0; y < h; y++) {
					const std::size_t i = static_cast<std::size_t>(y) * w + x;
					if (field.covered[i]) {
						const float p = (node.tangent - field.position[i].z) / span;
						// (clearing over this mesh's calm runs, like the height fallen: a shelf carrying a fall on does not run white)
						// D191: calm counted from the tangent: the calm river over the crest faded a whole fall's progress to near 0
						const float calm = field.calm.size() == count ? field.calm[i] : 0.0f;
						if (p <= 0.0f || std::isnan(calmAbove))
							calmAbove = calm;
						// (above the tangent held over -1, which the shading reads as a mesh that never turns steep: D191)
						progress[i] = p > 0.0f ? std::min(p, 1.0f) * tall * std::exp(-std::max(calm - calmAbove, 0.0f) / FallStrands::CALM_RUN) : std::max(p, -PROGRESS_ABOVE_MOST);
						if (lastKnown < 0) {
							for (int32_t k = 0; k < y; k++)
								progress[static_cast<std::size_t>(k) * w + x] = progress[i];
						}
						lastKnown = y;
					} else if (lastKnown >= 0) {
						progress[i] = progress[static_cast<std::size_t>(lastKnown) * w + x];
					}
				}
			}
			// D191: soften among the texels that have a progress (per column it stepped between columns and at fills, so the white came in
			// blocks); a column with none keeps -1
			{
				std::vector<uint8_t> known(count, 0);
				for (std::size_t i = 0; i < count; i++)
					known[i] = progress[i] > -0.99f ? 1 : 0;
				for (int pass = 0; pass < PROGRESS_SOFTEN_PASSES; pass++) {
					std::vector<float> softened = progress;
					for (int32_t y = 0; y < h; y++) {
						for (int32_t x = 0; x < w; x++) {
							const std::size_t i = static_cast<std::size_t>(y) * w + x;
							if (!known[i])
								continue;
							float sum = 0.0f, weight = 0.0f;
							for (int32_t dy = -1; dy <= 1; dy++) {
								for (int32_t dx = -1; dx <= 1; dx++) {
									const int32_t nx = x + dx, ny = y + dy;
									if (nx < 0 || ny < 0 || nx >= w || ny >= h)
										continue;
									const std::size_t neighbour = static_cast<std::size_t>(ny) * w + nx;
									if (!known[neighbour])
										continue;
									const float k = (dx == 0 ? 2.0f : 1.0f) * (dy == 0 ? 2.0f : 1.0f);
									sum += progress[neighbour] * k;
									weight += k;
								}
							}
							softened[i] = std::max(sum / weight, -PROGRESS_ABOVE_MOST);
						}
					}
					progress.swap(softened);
				}
			}
			progressOf[n] = std::move(progress);
		}
		// D191: a fed mesh's water starts as far down its fall as the feeder texel it comes from (the links), clearing over this
		// mesh's calm runs (else a fall pouring from the basin under another's plunge started glassy). Highest first, so chains carry on.
		std::vector<std::size_t> order(nodes.size());
		for (std::size_t n = 0; n < order.size(); n++)
			order[n] = n;
		std::sort(order.begin(), order.end(), [&](std::size_t a_a, std::size_t a_b) { return nodes[a_a].top > nodes[a_b].top; });
		uint32_t carriedColumns = 0;
		for (const std::size_t n : order) {
			const Node& node = nodes[n];
			if (node.feeder < 0 || progressOf[n].empty() || progressOf[static_cast<std::size_t>(node.feeder)].empty())
				continue;
			const BakeField& field = *node.field;
			const BakeField& feederField = *nodes[static_cast<std::size_t>(node.feeder)].field;
			const auto w = static_cast<int32_t>(field.width), h = static_cast<int32_t>(field.height);
			const auto fw = static_cast<int32_t>(feederField.width), fh = static_cast<int32_t>(feederField.height);
			const std::size_t count = static_cast<std::size_t>(w) * h;
			const auto& from = progressOf[static_cast<std::size_t>(node.feeder)];
			auto& progress = progressOf[n];
			for (const auto& [column, fx, fy] : node.links) {
				if (column < 0 || column >= w || fx < 0 || fy < 0 || fx >= fw || fy >= fh)
					continue;
				const float carried = from[static_cast<std::size_t>(fy) * fw + fx];
				if (!(carried > 0.0f))
					continue;
				carriedColumns++;
				for (int32_t y = 0; y < h; y++) {
					const std::size_t i = static_cast<std::size_t>(y) * w + column;
					if (progress[i] <= -0.99f)
						continue;
					const float calm = field.calm.size() == count ? field.calm[i] : 0.0f;
					progress[i] = std::max(progress[i], carried * std::exp(-calm / FallStrands::CALM_RUN));
				}
			}
		}
		// D191: a fall pouring out of the pool other falls land in takes in their white water, fading across the pool (POOL_CARRY_RUN)
		// and over its own calm runs. Matched by the pool's surface, as the falls' meshes reach on under the water.
		uint32_t poolLinks = 0;
		{
			const auto ends = [&](std::size_t a_n, RE::NiPoint3& o_top, RE::NiPoint3& o_foot) {
				const BakeField& field = *nodes[a_n].field;
				float hiZ = -FLT_MAX, loZ = FLT_MAX;
				for (std::size_t i = 0; i < field.position.size() && i < field.covered.size(); i++) {
					if (!field.covered[i])
						continue;
					if (field.position[i].z > hiZ) {
						hiZ = field.position[i].z;
						o_top = field.position[i];
					}
					if (field.position[i].z < loZ) {
						loZ = field.position[i].z;
						o_foot = field.position[i];
					}
				}
				return hiZ > -FLT_MAX;
			};
			std::vector<RE::NiPoint3> tops(nodes.size()), feet(nodes.size());
			std::vector<uint8_t> known(nodes.size(), 0);
			for (std::size_t n = 0; n < nodes.size(); n++)
				known[n] = ends(n, tops[n], feet[n]) ? 1 : 0;
			// The water's surface at a point: our pools, else the highest engine plane over it up to a_ceiling (basins between falls are
			// as often the engine's rivers)
			const auto waterAt = [&](const RE::NiPoint3& a_p, float a_ceiling, float& o_level) {
				if (PoolLevelAt(a_p.x, a_p.y, o_level) && o_level <= a_ceiling)
					return true;
				std::vector<std::array<float, 5>> planes;
				CollectWaterPlanes(a_p.x - 1.0f, a_p.y - 1.0f, a_p.x + 1.0f, a_p.y + 1.0f, planes);
				float best = -FLT_MAX;
				for (const auto& plane : planes) {
					if (a_p.x >= plane[0] && a_p.x <= plane[2] && a_p.y >= plane[1] && a_p.y <= plane[3] && plane[4] <= a_ceiling)
						best = std::max(best, plane[4]);
				}
				o_level = best;
				return best > -FLT_MAX;
			};
			for (const std::size_t n : order) {
				if (nodes[n].feeder >= 0 || progressOf[n].empty() || !known[n])
					continue;
				// (by its crest, node.top: its highest texel is the hidden lead-in under the water above; vanilla's crests stand up to
				// ~120 units over the water they pour out of)
				float pool = 0.0f;
				const RE::NiPoint3 crest(tops[n].x, tops[n].y, nodes[n].top);
				if (!waterAt(crest, crest.z + POOL_LINK_TOP, pool) || crest.z - pool > POOL_LINK_ABOVE || pool - crest.z > POOL_LINK_TOP) {
					logger::info("[Fluid Field]   falls: fall {} (in at {:.0f}, {:.0f}, crest {:.0f}) pours out of no water near its top{}", n, tops[n].x, tops[n].y, crest.z,
						pool != 0.0f ? std::format(" (the nearest under it at {:.0f})", pool) : "");
					continue;
				}
				float carried = 0.0f;
				std::size_t from = n;
				for (std::size_t m = 0; m < nodes.size(); m++) {
					float landing = 0.0f;
					if (m == n || progressOf[m].empty() || !known[m] || feet[m].z > pool + POOL_LINK_TOP || !waterAt(feet[m], pool + POOL_LINK_LEVEL, landing) ||
						std::abs(landing - pool) > POOL_LINK_LEVEL)
						continue;
					const float across = std::sqrt((feet[m].x - tops[n].x) * (feet[m].x - tops[n].x) + (feet[m].y - tops[n].y) * (feet[m].y - tops[n].y));
					if (across > POOL_LINK_REACH)
						continue;
					const float white = *std::max_element(progressOf[m].begin(), progressOf[m].end()) * std::exp(-across / POOL_CARRY_RUN);
					if (white > carried) {
						carried = white;
						from = m;
					}
				}
				if (!(carried > 0.01f)) {
					logger::info("[Fluid Field]   falls: fall {} pours out of water at {:.0f}, but no fall lands in it within reach", n, pool);
					continue;
				}
				const BakeField& field = *nodes[n].field;
				auto& progress = progressOf[n];
				const std::size_t count = progress.size();
				for (std::size_t i = 0; i < count; i++) {
					if (progress[i] <= -0.99f)
						continue;
					const float calm = field.calm.size() == count ? field.calm[i] : 0.0f;
					progress[i] = std::max(progress[i], carried * std::exp(-calm / FallStrands::CALM_RUN));
				}
				poolLinks++;
				logger::info("[Fluid Field]   falls: fall {} pours out of the pool fall {} lands in (its surface {:.0f}): {:.2f} of a fall's white water carried in", n, from, pool, carried);
			}
		}
		for (std::size_t n = 0; n < nodes.size(); n++) {
			if (progressOf[n].empty())
				continue;
			auto& texels = flowBakes[nodes[n].geometry].texels;
			const std::size_t count = progressOf[n].size();
			if (texels.size() != count * BAKE_TEXELS_PER)
				continue;
			for (std::size_t i = 0; i < count; i++)
				texels[(count * 3 + i) * 4 + 3] = progressOf[n][i];
		}
		logger::info("[Fluid Field]   falls: {} columns carry their feeder's white water in", carriedColumns);
		// D191: where each mesh's water comes in (highest covered texel) and goes out (lowest), logged to check the pool links
		std::vector<RE::NiPoint3> topPoint(nodes.size()), footPoint(nodes.size());
		for (std::size_t n = 0; n < nodes.size(); n++) {
			const BakeField& field = *nodes[n].field;
			float hiZ = -FLT_MAX, loZ = FLT_MAX;
			for (std::size_t i = 0; i < field.position.size() && i < field.covered.size(); i++) {
				if (!field.covered[i])
					continue;
				if (field.position[i].z > hiZ) {
					hiZ = field.position[i].z;
					topPoint[n] = field.position[i];
				}
				if (field.position[i].z < loZ) {
					loZ = field.position[i].z;
					footPoint[n] = field.position[i];
				}
			}
		}
		for (std::size_t n = 0; n < nodes.size(); n++) {
			const Node& node = nodes[n];
			logger::info("[Fluid Field]   fall {}: a mesh from {:.0f} to {:.0f}: tangent {:.0f}{}, foot {:.0f}, {} (water in with {:.0f} fallen); in at ({:.0f}, {:.0f}), out at ({:.0f}, {:.0f}, {:.0f})", n,
				node.top, node.lo[2], node.tangent, node.continues ? " (its feeder's)" : "", node.foot, node.feeder >= 0 ? "fed" : "a top", node.arriving, topPoint[n].x, topPoint[n].y,
				footPoint[n].x, footPoint[n].y, footPoint[n].z);
			if (node.feeder >= 0)
				continue;
			// The meshes whose foot lands within reach of where this one's water comes in, below it or a little above: candidates
			for (std::size_t m = 0; m < nodes.size(); m++) {
				if (m == n)
					continue;
				const float dx = footPoint[m].x - topPoint[n].x, dy = footPoint[m].y - topPoint[n].y, dz = footPoint[m].z - topPoint[n].z;
				const float across = std::sqrt(dx * dx + dy * dy);
				if (across < 1200.0f && dz > -400.0f && dz < 200.0f)
					logger::info("[Fluid Field]     fall {} may pour out of the pool fall {} lands in: {:.0f} units across, its foot {:.0f} units {}", n, m, across, std::abs(dz), dz < 0.0f ? "under this one's top" : "over it");
			}
		}
		linkedDue = true;  // D172: flowBakeVersion++ and the plunges on the render thread, once this worker is done (FinishHeavyWork)
		logger::info("[Fluid Field] Waterfall flow linked: {} meshes, {} fed by the mesh above them", nodes.size(), linked);
		const auto start = std::chrono::steady_clock::now();
		ComputePushes();
		logger::info("[Fluid Field]   swell and wobble took {:.1f} ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
	}

	void FallSites::BakeStep()
	{
		BakeWork();
		// D67: link once all are solved, so each mesh carries on from the one above. D83: checked every step, not only when a bake
		// lands: a cached bake lands a frame after its job, before the queue's stale entries are skipped, and nothing lands after them
		if (linkDue && bakesPending.empty() && bakeQueue.empty() && !bakeJob) {
			linkDue = false;
			logger::info("[Fluid Field] Waterfall flow bakes: {} meshes ({} read from the cache, {} solved), {} texels tested, {} inside rock", bakeFields.size(), bakesCached,
				bakesSolved, bakedTexels, bakedRockTexels);
			logger::info("[Fluid Field]   {} more texels took their tested neighbours' answer (D185)", bakedFilled);
			// D172: on a worker (1.2 s on one frame); its plunges after it, on the render thread
			linkUnified = unifiedShapes;  // read by the worker, so a copy
			heavyWork = std::async(std::launch::async, [this]() {
				const auto start = std::chrono::steady_clock::now();
				LinkBakes();
				logger::info("[Fluid Field]   linking the bakes took {:.1f} ms (worker)", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
				loadTiming.AddWorker(LoadWork::Link, start);
			});
		}
	}

	void FallSites::DumpUnifyInputs() const
	{
		// Layout (little-endian): "UNIF" v2. Per mesh file: vertex and triangle counts, world rotation (9), translation (3), scale,
		// then per vertex model position (3), uv (2), its 16 vertexClips floats; then the triangles (3 x uint16). Then the untrimmed
		// triangles (count, 3 x uint16 each); the vanilla source (vertex and triangle counts; if any, its transform (13), per vertex
		// model position (3) and uv (2), its triangles); the reference's form ID and model path (length, characters).
		// The solids file: the statics' triangles (9 floats each) round all meshes, then a landscape height grid; beside it
		// FluidField_unify_refs.tsv, what each reference round the falls gave.
		const auto directory = logger::log_directory();
		if (!directory || litMeshList.empty())
			return;
		constexpr uint32_t MAGIC = 0x46494E55;  // "UNIF"
		constexpr uint32_t VERSION = 2;         // v2: untrimmed triangles, the vanilla source and its model path after each mesh
		float lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
		uint32_t written = 0;
		for (const auto& lit : litMeshList) {
			auto* geometry = lit.geometry.get();
			auto* triShape = geometry ? geometry->AsTriShape() : nullptr;
			auto* renderer = geometry ? geometry->GetGeometryRuntimeData().rendererData : nullptr;
			const auto base = geometry ? vertexClipBase.find(geometry) : vertexClipBase.end();
			if (!triShape || !renderer || !renderer->rawVertexData || !renderer->rawIndexData || base == vertexClipBase.end() ||
				!renderer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_UV))
				continue;
			const auto& counts = triShape->GetTrishapeRuntimeData();
			if (base->second + counts.vertexCount > vertexClips.size())
				continue;
			const uint32_t stride = renderer->vertexDesc.GetSize();
			const uint32_t positionOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
			const uint32_t uvOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_TEXCOORD0);
			std::ofstream file(*directory / std::format("FluidField_unify_mesh{}.bin", written), std::ios::binary);
			if (!file)
				continue;
			const uint32_t header[4] = { MAGIC, VERSION, counts.vertexCount, counts.triangleCount };
			file.write(reinterpret_cast<const char*>(header), sizeof(header));
			const auto& world = geometry->world;
			float transform[13];
			for (int r = 0; r < 3; r++) {
				for (int c = 0; c < 3; c++)
					transform[r * 3 + c] = world.rotate.entry[r][c];
			}
			transform[9] = world.translate.x;
			transform[10] = world.translate.y;
			transform[11] = world.translate.z;
			transform[12] = world.scale;
			file.write(reinterpret_cast<const char*>(transform), sizeof(transform));
			for (uint32_t v = 0; v < counts.vertexCount; v++) {
				const uint8_t* data = renderer->rawVertexData + static_cast<std::size_t>(v) * stride;
				float vertex[5];
				std::memcpy(vertex, data + positionOffset, sizeof(float) * 3);
				const auto* uv = reinterpret_cast<const uint16_t*>(data + uvOffset);
				vertex[3] = DirectX::PackedVector::XMConvertHalfToFloat(uv[0]);
				vertex[4] = DirectX::PackedVector::XMConvertHalfToFloat(uv[1]);
				file.write(reinterpret_cast<const char*>(vertex), sizeof(vertex));
				file.write(reinterpret_cast<const char*>(vertexClips[base->second + v].data()), sizeof(float) * 16);
				const RE::NiPoint3 at = world * RE::NiPoint3(vertex[0], vertex[1], vertex[2]);
				lo[0] = std::min(lo[0], at.x);
				lo[1] = std::min(lo[1], at.y);
				hi[0] = std::max(hi[0], at.x);
				hi[1] = std::max(hi[1], at.y);
			}
			file.write(reinterpret_cast<const char*>(renderer->rawIndexData), static_cast<std::streamsize>(counts.triangleCount) * 3 * sizeof(uint16_t));
			// v2: the bake's untrimmed triangles (D172: the copy before the landscape cut)
			const auto untrimmed = untrimmedIndices.find(geometry);
			const uint32_t untrimmedCount = untrimmed != untrimmedIndices.end() ? static_cast<uint32_t>(untrimmed->second.size() / 3) : 0;
			file.write(reinterpret_cast<const char*>(&untrimmedCount), sizeof(untrimmedCount));
			if (untrimmedCount)
				file.write(reinterpret_cast<const char*>(untrimmed->second.data()), static_cast<std::streamsize>(untrimmed->second.size()) * sizeof(uint16_t));
			// v2: the vanilla mesh the copy was made from (counts, world transform, model position and uv per vertex, triangles)
			RE::BSGeometry* source = nullptr;
			for (const auto& [original, copy] : denseOf) {
				if (copy.shape.get() == triShape) {
					source = copy.original.get();
					break;
				}
			}
			auto* sourceShape = source ? source->AsTriShape() : nullptr;
			auto* sourceRenderer = source ? source->GetGeometryRuntimeData().rendererData : nullptr;
			const bool haveSource = sourceShape && sourceRenderer && sourceRenderer->rawVertexData && sourceRenderer->rawIndexData &&
			                        sourceRenderer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_UV);
			const auto& sourceCounts = haveSource ? sourceShape->GetTrishapeRuntimeData() : counts;
			const uint32_t sourceHeader[2] = { haveSource ? sourceCounts.vertexCount : 0u, haveSource ? static_cast<uint32_t>(sourceCounts.triangleCount) : 0u };
			file.write(reinterpret_cast<const char*>(sourceHeader), sizeof(sourceHeader));
			if (haveSource) {
				const auto& sourceWorld = source->world;
				float sourceTransform[13];
				for (int r = 0; r < 3; r++) {
					for (int c = 0; c < 3; c++)
						sourceTransform[r * 3 + c] = sourceWorld.rotate.entry[r][c];
				}
				sourceTransform[9] = sourceWorld.translate.x;
				sourceTransform[10] = sourceWorld.translate.y;
				sourceTransform[11] = sourceWorld.translate.z;
				sourceTransform[12] = sourceWorld.scale;
				file.write(reinterpret_cast<const char*>(sourceTransform), sizeof(sourceTransform));
				const uint32_t sourceStride = sourceRenderer->vertexDesc.GetSize();
				const uint32_t sourcePosition = sourceRenderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
				const uint32_t sourceUv = sourceRenderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_TEXCOORD0);
				for (uint32_t v = 0; v < sourceCounts.vertexCount; v++) {
					const uint8_t* data = sourceRenderer->rawVertexData + static_cast<std::size_t>(v) * sourceStride;
					float vertex[5];
					std::memcpy(vertex, data + sourcePosition, sizeof(float) * 3);
					const auto* uv = reinterpret_cast<const uint16_t*>(data + sourceUv);
					vertex[3] = DirectX::PackedVector::XMConvertHalfToFloat(uv[0]);
					vertex[4] = DirectX::PackedVector::XMConvertHalfToFloat(uv[1]);
					file.write(reinterpret_cast<const char*>(vertex), sizeof(vertex));
				}
				file.write(reinterpret_cast<const char*>(sourceRenderer->rawIndexData), static_cast<std::streamsize>(sourceCounts.triangleCount) * 3 * sizeof(uint16_t));
			}
			// v2: the reference's form ID and model path (u32 length, then the characters)
			RE::TESObjectREFR* ref = nullptr;
			for (RE::NiAVObject* object = source ? static_cast<RE::NiAVObject*>(source) : geometry; object && !ref; object = object->parent)
				ref = object->GetUserData();
			const uint32_t formID = ref ? ref->GetFormID() : 0;
			const std::string model = ref ? (ModelOf(ref->GetBaseObject()) ? ModelOf(ref->GetBaseObject()) : "") : "";
			const uint32_t modelLength = static_cast<uint32_t>(model.size());
			file.write(reinterpret_cast<const char*>(&formID), sizeof(formID));
			file.write(reinterpret_cast<const char*>(&modelLength), sizeof(modelLength));
			file.write(model.data(), modelLength);
			written++;
		}
		if (!written)
			return;
		constexpr float MARGIN = 256.0f;  // world units round the meshes
		constexpr float LAND_STEP = 32.0f;
		std::vector<FallVisibility::Triangle> statics;
		std::string report = "form\ttype\tmodel\tposition\tbound\ttaken\tskipped\n";
		GatherOccluders(lo[0] - MARGIN, lo[1] - MARGIN, hi[0] + MARGIN, hi[1] + MARGIN, statics, &report);
		if (std::ofstream refsFile(*directory / "FluidField_unify_refs.tsv"); refsFile)
			refsFile << report;
		std::ofstream file(*directory / "FluidField_unify_solids.bin", std::ios::binary);
		if (!file)
			return;
		const uint32_t header[3] = { MAGIC, VERSION, static_cast<uint32_t>(statics.size()) };
		file.write(reinterpret_cast<const char*>(header), sizeof(header));
		for (const auto& triangle : statics)
			file.write(reinterpret_cast<const char*>(triangle.data()), sizeof(RE::NiPoint3) * 3);
		// Landscape: origin, step, size, then heights row by row (-1e9 where none)
		const auto nx = static_cast<uint32_t>((hi[0] - lo[0] + 2.0f * MARGIN) / LAND_STEP) + 1;
		const auto ny = static_cast<uint32_t>((hi[1] - lo[1] + 2.0f * MARGIN) / LAND_STEP) + 1;
		const float grid[3] = { lo[0] - MARGIN, lo[1] - MARGIN, LAND_STEP };
		const uint32_t size[2] = { nx, ny };
		file.write(reinterpret_cast<const char*>(grid), sizeof(grid));
		file.write(reinterpret_cast<const char*>(size), sizeof(size));
		for (uint32_t y = 0; y < ny; y++) {
			for (uint32_t x = 0; x < nx; x++) {
				float height = -1e9f;
				if (landscape && !landscape->SampleHeight(grid[0] + x * LAND_STEP, grid[1] + y * LAND_STEP, height))
					height = -1e9f;
				file.write(reinterpret_cast<const char*>(&height), sizeof(height));
			}
		}
		logger::info("[Fluid Field] Unified falls inputs: {} meshes, {} static triangles, {}x{} landscape heights, next to the log", written, statics.size(), nx, ny);
	}

	void FallSites::GatherOccluders(float a_minX, float a_minY, float a_maxX, float a_maxY, std::vector<FallVisibility::Triangle>& o_triangles, std::string* o_report) const
	{
		// D173: like GatherSolids: the statics' render triangles over the area, not the falls' own models (their copies are added
		// below, this batch's by TrimHidden)
		auto* tes = RE::TES::GetSingleton();
		if (!tes)
			return;
		const auto inArea = [&](const RE::NiPoint3& a_p) { return a_p.x >= a_minX && a_p.x <= a_maxX && a_p.y >= a_minY && a_p.y <= a_maxY; };
		// the report's tally for the reference being visited: triangles taken, geometries skipped by reason
		std::size_t taken = 0, noShape = 0, noData = 0, skinned = 0, otherShader = 0;
		const auto addGeometry = [&](RE::BSGeometry* a_geometry) {
			auto* triShape = a_geometry ? a_geometry->AsTriShape() : nullptr;
			auto* renderer = a_geometry ? a_geometry->GetGeometryRuntimeData().rendererData : nullptr;
			if (!triShape || !renderer) {
				noShape++;
				return;
			}
			if (!renderer->rawVertexData || !renderer->rawIndexData) {
				noData++;
				return;
			}
			if (renderer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_SKINNED)) {
				skinned++;
				return;
			}
			const auto& counts = triShape->GetTrishapeRuntimeData();
			const uint32_t stride = renderer->vertexDesc.GetSize();
			const uint32_t positionOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
			if (stride < positionOffset + 12)
				return;
			std::vector<RE::NiPoint3> world(counts.vertexCount);
			for (uint32_t v = 0; v < counts.vertexCount; v++) {
				float local[3];
				std::memcpy(local, renderer->rawVertexData + static_cast<std::size_t>(v) * stride + positionOffset, sizeof(local));
				world[v] = a_geometry->world * RE::NiPoint3(local[0], local[1], local[2]);
			}
			for (uint32_t i = 0; i < counts.triangleCount; i++) {
				const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(i) * 3;
				if (index[0] >= counts.vertexCount || index[1] >= counts.vertexCount || index[2] >= counts.vertexCount)
					continue;
				const FallVisibility::Triangle tri = { world[index[0]], world[index[1]], world[index[2]] };
				// the report's: a triangle spanning the area with no corner in it counts too (a large boulder face)
				const bool spans = o_report && std::max({ tri[0].x, tri[1].x, tri[2].x }) >= a_minX && std::min({ tri[0].x, tri[1].x, tri[2].x }) <= a_maxX &&
				                   std::max({ tri[0].y, tri[1].y, tri[2].y }) >= a_minY && std::min({ tri[0].y, tri[1].y, tri[2].y }) <= a_maxY;
				if (inArea(tri[0]) || inArea(tri[1]) || inArea(tri[2]) || spans) {
					o_triangles.push_back(tri);
					taken++;
				}
			}
		};
		const auto visitCell = [&](RE::TESObjectCELL* a_cell, bool a_requireAttached) {
			if (!a_cell || (a_requireAttached && !a_cell->IsAttached()))
				return;
			a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
				const bool staticType = base && (base->GetFormType() == RE::FormType::Static || base->GetFormType() == RE::FormType::MovableStatic);
				if (!base || a_ref->IsDisabled() || a_ref->IsDeleted() || (!staticType && !o_report) || a_ref->Is(RE::FormType::ActorCharacter))
					return RE::BSContainer::ForEachResult::kContinue;
				auto* root = a_ref->Get3D();
				if (!root)
					return RE::BSContainer::ForEachResult::kContinue;
				const auto& bound = root->worldBound;
				if (bound.center.x + bound.radius < a_minX || bound.center.x - bound.radius > a_maxX || bound.center.y + bound.radius < a_minY || bound.center.y - bound.radius > a_maxY)
					return RE::BSContainer::ForEachResult::kContinue;
				const std::string model = Lowercase(ModelOf(base));
				if (model.find("fxwaterfall") != std::string::npos || model.find("fxrapids") != std::string::npos)
					return RE::BSContainer::ForEachResult::kContinue;
				taken = noShape = noData = skinned = otherShader = 0;
				std::size_t hidden = 0;
				RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
					if (o_report && a_geometry->GetAppCulled())  // markers' editor-only geometry, with every reference type taken
						hidden++;
					else if (netimmerse_cast<RE::BSLightingShaderProperty*>(a_geometry->GetGeometryRuntimeData().shaderProperty.get()))
						addGeometry(a_geometry);
					else
						otherShader++;
					return RE::BSVisit::BSVisitControl::kContinue;
				});
				if (o_report) {
					const auto& at = a_ref->GetPosition();
					*o_report += std::format("{:08X}\t{}\t{}\t{:.0f} {:.0f} {:.0f}\tradius {:.0f}\ttriangles {}\tskipped: no shape {}, no CPU data {}, skinned {}, other shader {}, hidden {}\n",
						a_ref->GetFormID(), static_cast<int>(base->GetFormType()), model, at.x, at.y, at.z, bound.radius, taken, noShape, noData, skinned, otherShader, hidden);
				}
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
		}
		// The dense copies made before (another batch's sheets hide this one's too)
		for (const auto& [original, copy] : denseOf)
			addGeometry(copy.shape.get());
	}

	void FallSites::TrimHidden(const std::vector<DenseJob*>& a_jobs, const ShellInput& a_shell)
	{
		// D173: the outer shell of the batch's sheets, the statics and the landscape: drop the triangles none of whose vertices can
		// be seen from outside (FallVisibility), keeping a SHELL_MARGIN round what is seen
		const auto start = std::chrono::steady_clock::now();
		std::vector<std::vector<RE::NiPoint3>> world(a_jobs.size());
		std::vector<FallVisibility::Sheet> sheets;
		for (std::size_t j = 0; j < a_jobs.size(); j++) {
			if (!a_jobs[j]->ok)
				continue;
			world[j].resize(a_jobs[j]->positions.size());
			for (std::size_t v = 0; v < world[j].size(); v++)
				world[j][v] = a_jobs[j]->world * a_jobs[j]->positions[v];
		}
		std::vector<std::size_t> sheetJob;
		for (std::size_t j = 0; j < a_jobs.size(); j++) {
			if (!a_jobs[j]->ok)
				continue;
			sheets.push_back({ &world[j], &a_jobs[j]->triangles });
			sheetJob.push_back(j);
		}
		uint64_t tested = 0, rays = 0;
		const auto visible = FallVisibility::Visible(sheets, a_shell.occluders, a_shell.interior ? nullptr : &a_shell.land, &tested, &rays);
		uint64_t dropped = 0, kept = 0;
		for (std::size_t s = 0; s < sheets.size(); s++) {
			DenseJob& job = *a_jobs[sheetJob[s]];
			// The margin: a vertex within SHELL_MARGIN of a seen one, along its sheet, is kept too (else two sheets' meeting edges
			// stopped short of each other and the ground showed between)
			const std::size_t count = world[sheetJob[s]].size();
			std::vector<std::vector<std::pair<uint32_t, float>>> links(count);
			for (const auto& tri : job.triangles) {
				for (int k = 0; k < 3; k++) {
					const uint32_t a = tri[k], b = tri[(k + 1) % 3];
					const float length = (world[sheetJob[s]][a] - world[sheetJob[s]][b]).Length();
					links[a].emplace_back(b, length);
					links[b].emplace_back(a, length);
				}
			}
			std::vector<float> reach(count, FLT_MAX);
			using Entry = std::pair<float, uint32_t>;
			std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
			for (uint32_t v = 0; v < count; v++) {
				if (visible[s][v] && !links[v].empty()) {
					reach[v] = 0.0f;
					open.emplace(0.0f, v);
				}
			}
			while (!open.empty()) {
				const auto [distance, v] = open.top();
				open.pop();
				if (distance > reach[v])
					continue;
				for (const auto& [w, length] : links[v]) {
					const float through = distance + length;
					if (through < reach[w] && through <= SHELL_MARGIN) {
						reach[w] = through;
						open.emplace(through, w);
					}
				}
			}
			// Cut along the margin's edge, not at whole triangles (their staircase drew the meeting foam as white wedges): kept where
			// SHELL_MARGIN - reach is not negative, a crossing triangle split (as Densify's landscape cut), new vertices at the end
			std::vector<float> keep(count);
			for (std::size_t v = 0; v < count; v++)
				keep[v] = std::clamp(SHELL_MARGIN - reach[v], -SHELL_MARGIN, SHELL_MARGIN);
			std::map<std::pair<uint32_t, uint32_t>, uint32_t> crossings;
			const auto crossing = [&](uint32_t a_in, uint32_t a_out) {
				const auto key = std::make_pair(a_in, a_out);
				if (const auto it = crossings.find(key); it != crossings.end())
					return it->second;
				const float f = std::clamp(keep[a_in] / std::max(keep[a_in] - keep[a_out], 1e-4f), 0.0f, 1.0f);
				const auto lerp3 = [f](const RE::NiPoint3& a_a, const RE::NiPoint3& a_b) { return a_a + (a_b - a_a) * f; };
				const auto unit = [](const RE::NiPoint3& a_v) { const float l = a_v.Length(); return l > 1e-6f ? a_v / l : RE::NiPoint3(0.0f, 0.0f, 1.0f); };
				job.positions.push_back(lerp3(job.positions[a_in], job.positions[a_out]));
				job.normals.push_back(unit(lerp3(job.normals[a_in], job.normals[a_out])));
				job.tangents.push_back(unit(lerp3(job.tangents[a_in], job.tangents[a_out])));
				job.bitangents.push_back(unit(lerp3(job.bitangents[a_in], job.bitangents[a_out])));
				job.uvs.push_back(RE::NiPoint2(job.uvs[a_in].x + (job.uvs[a_out].x - job.uvs[a_in].x) * f, job.uvs[a_in].y + (job.uvs[a_out].y - job.uvs[a_in].y) * f));
				const RE::NiColorA& c0 = job.colors[a_in];
				const RE::NiColorA& c1 = job.colors[a_out];
				job.colors.push_back(RE::NiColorA(c0.red + (c1.red - c0.red) * f, c0.green + (c1.green - c0.green) * f, c0.blue + (c1.blue - c0.blue) * f, c0.alpha + (c1.alpha - c0.alpha) * f));
				const auto index = static_cast<uint32_t>(job.positions.size() - 1);
				crossings[key] = index;
				return index;
			};
			std::vector<std::array<uint32_t, 3>> shell;
			shell.reserve(job.triangles.size());
			for (const auto& tri : job.triangles) {
				const bool in[3] = { keep[tri[0]] >= 0.0f, keep[tri[1]] >= 0.0f, keep[tri[2]] >= 0.0f };
				const int ins = (in[0] ? 1 : 0) + (in[1] ? 1 : 0) + (in[2] ? 1 : 0);
				if (ins == 3) {
					shell.push_back(tri);
				} else if (ins == 1) {
					const int k = in[0] ? 0 : (in[1] ? 1 : 2);
					const uint32_t a = tri[k], b = tri[(k + 1) % 3], c = tri[(k + 2) % 3];
					shell.push_back({ a, crossing(a, b), crossing(a, c) });
				} else if (ins == 2) {
					const int k = !in[0] ? 2 : (!in[1] ? 0 : 1);
					const uint32_t a = tri[k], b = tri[(k + 1) % 3], c = tri[(k + 2) % 3];
					const uint32_t bc = crossing(b, c), ac = crossing(a, c);
					shell.push_back({ a, b, bc });
					shell.push_back({ a, bc, ac });
				}
			}
			if (job.positions.size() > 65535) {
				// Too many for 16-bit indices: the whole-triangle cut instead (a staircase, but drawn)
				job.positions.resize(count);
				job.normals.resize(count);
				job.tangents.resize(count);
				job.bitangents.resize(count);
				job.uvs.resize(count);
				job.colors.resize(count);
				shell.clear();
				for (const auto& tri : job.triangles) {
					if (keep[tri[0]] >= 0.0f || keep[tri[1]] >= 0.0f || keep[tri[2]] >= 0.0f)
						shell.push_back(tri);
				}
			}
			job.hiddenAway = job.triangles.size() > shell.size() ? static_cast<uint32_t>(job.triangles.size() - shell.size()) : 0;
			dropped += job.hiddenAway;
			kept += shell.size();
			job.triangles.swap(shell);
			job.indices.clear();
			job.indices.reserve(job.triangles.size() * 3);
			for (const auto& tri : job.triangles) {
				for (const uint32_t k : tri)
					job.indices.push_back(static_cast<uint16_t>(k));
			}
		}
		logger::info("[Fluid Field] Waterfall outer shell: {} of {} triangles seen from nowhere cut away ({} vertices tested, {} rays, {} occluding triangles gathered in {:.0f} ms; {:.0f} ms)",
			dropped, dropped + kept, tested, rays, a_shell.occluders.size(), a_shell.gatherMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
	}

	void FallSites::FinishDenseWork()
	{
		if (!denseWork.valid() || denseWork.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return;
		denseWork.get();
		denseWork = {};
		const auto start = std::chrono::steady_clock::now();
		uint32_t built = 0;
		for (auto& job : denseJobs) {
			if (AttachDense(*job))
				built++;
			else if (job->geometry && !denseOf.contains(job->geometry.get()))
				denseFailed.insert(job->geometry.get());
		}
		logger::info("[Fluid Field] Waterfall dense copies: {} of {} lit meshes ({:.0f} ms on a worker, {:.0f} ms attaching)", built, denseJobs.size(),
			std::chrono::duration<double, std::milli>(start - denseStarted).count(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
		denseJobs.clear();
		// Ground again, with the copies (or the originals that could not have one) in their place
		RefreshGrounded();
		RefreshHiddenEffects();
	}

	void FallSites::FinishHeavyWork()
	{
		if (!heavyWork.valid() || heavyWork.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return;
		heavyWork.get();
		heavyWork = {};
		// D172: re-upload vertexClips whole: a worker's mid-way version bumps uploaded it half-written (read as foam)
		vertexClipVersion++;
		if (linkedDue) {
			linkedDue = false;
			flowBakeVersion++;
			plungesDue = true;  // D172: ComputePlunges on a worker (Update)
		}
		if (plungeResult) {
			ApplyPlunges(*plungeResult);
			plungeResult.reset();
		}
		if (poolResult) {
			for (auto& [key, pools] : *poolResult)
				sitePools[key] = std::move(pools);
			poolResult.reset();
			groundVersion++;
			// D78: the plunges stand on the pools' water
			if (!bakeFields.empty())
				plungesDue = true;
			// D112: the pools cut the meshes' feet, which the culling reads (once the rock test has run; its end reads them anyway)
			if (buryDone && !vertexClipBase.empty())
				RequestCulling();
		}
	}

	void FallSites::BakeWork()
	{
		LandBakes();
		BakeRays();
	}

	void FallSites::LandBakes()
	{
		// Finished solves
		for (auto it = bakesPending.begin(); it != bakesPending.end();) {
			if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
				++it;
				continue;
			}
			auto field = it->second.get();
			const bool kept = it->first && grounded.contains(it->first.get()) && field;
			if (kept) {
				bakeFields[it->first.get()] = std::move(field);
				linkDue = true;
			}
			it = bakesPending.erase(it);
		}
	}

	void FallSites::BakeRays()
	{
		// D185: the bakes' start (cache read or statics gathered) and rock tests touch nothing the workers have, so they run while a
		// worker does (waiting out every culling pass cost 37 s of a cold load)
		auto* tes = RE::TES::GetSingleton();
		if (!tes)
			return;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(BAKE_BUDGET_MS);
		while (std::chrono::steady_clock::now() < deadline) {
			if (!bakeJob) {
				if (bakeQueue.empty())
					return;
				// D83: the falls nearest the camera first
				if (auto* camera = PlayerCamera(); camera && bakeQueue.size() > 1) {
					const RE::NiPoint3 eye = camera->world.translate;
					std::sort(bakeQueue.begin(), bakeQueue.end(), [&](const auto& a_a, const auto& a_b) {
						const float da = a_a ? (a_a->worldBound.center - eye).Length() : FLT_MAX, db = a_b ? (a_b->worldBound.center - eye).Length() : FLT_MAX;
						return da > db;
					});
				}
				RE::NiPointer<RE::BSGeometry> geometry = bakeQueue.back();
				bakeQueue.pop_back();
				if (!geometry || !grounded.contains(geometry.get()))
					continue;
				bakeJob = StartBake(geometry.get());
				if (!bakeJob)
					continue;
				// D132: bake the whole mesh (D126 left culled texels out: every cull edge became a rim, and foam and wake filled sheets in
				// bands); the cut hides the culled parts, and ComputePlunges' shell mask their spray, plunges and lips
				GatherSolids(*bakeJob);
				// D82: baked before (same reference, placement, rock and ground): read back, no rock test or solve
				bakeJob->cache = BakeCachePath(*bakeJob);
				std::error_code error;
				const bool cached = !bakeJob->cache.empty() && std::filesystem::exists(bakeJob->cache, error);
				logger::info("[Fluid Field]   bake job '{}' ({}x{}): cache {} '{}'", geometry->name.c_str(), bakeJob->width, bakeJob->height, cached ? "hit" : "miss",
					bakeJob->cache.filename().string());
				if (cached) {
					auto field = LoadBake(bakeJob->cache);
					if (!field)
						logger::warn("[Fluid Field]   bake cache: could not read '{}'", bakeJob->cache.string());
					if (field) {
						std::promise<std::shared_ptr<BakeField>> ready;
						ready.set_value(std::move(field));
						bakesPending.emplace_back(bakeJob->geometry, ready.get_future());
						bakesCached++;
						bakeJob.reset();
						continue;
					}
				}
				// D83: the hidden lead-in no strand comes near is not tested (a third of the rays)
				{
					FallStrands::Input input;
					input.width = bakeJob->width;
					input.height = bakeJob->height;
					input.position = reinterpret_cast<const float*>(bakeJob->position.data());
					input.covered = bakeJob->covered.data();
					bakeJob->untested = FallStrands::HiddenLeadIn(input, LEAD_TEST_REACH);
				}
				bakesSolved++;
				continue;
			}
			auto* cell = tes->interiorCell ? tes->interiorCell : tes->GetCell(bakeJob->geometry->world.translate);
			auto* bhkWorld = cell ? cell->GetbhkWorld() : nullptr;
			auto* world = bhkWorld ? bhkWorld->GetWorld1() : nullptr;
			if (!world) {
				if (!bakeJob->worldMissing)
					logger::warn("[Fluid Field]   bake job '{}': no collision world at its cell; waiting", bakeJob->geometry->name.c_str());
				bakeJob->worldMissing = true;
				return;
			}
			{
				// Each texel inside rock? A ray from outside, in along the sheet's facing, crosses rock an odd number of times to reach a
				// point inside (D57), deeper than a sheet lying on its rock
				const RE::BSReadLockGuard lock(bhkWorld->worldLock);
				if (bakeJob->clearFront.size() != bakeJob->covered.size()) {
					bakeJob->clearFront.assign(bakeJob->covered.size(), CLEAR_REACH);
					bakeJob->clearBack.assign(bakeJob->covered.size(), CLEAR_REACH);
				}
				// D185: every other texel each way first (pass 0); the rest (pass 1) take their tested neighbours' answer where those agree
				// (all rock, or all open with clearances within CLEAR_AGREE), tested only where they differ: the rock's edges (testing all
				// cost 16 s)
				const auto w = static_cast<int32_t>(bakeJob->width);
				const auto testable = [&](std::size_t a_i) { return bakeJob->covered[a_i] && !(a_i < bakeJob->untested.size() && bakeJob->untested[a_i]); };
				int rays = 0;
				for (; rays < 64; bakeJob->next++) {
					if (bakeJob->next >= bakeJob->covered.size()) {
						if (bakeJob->pass == 0) {
							bakeJob->pass = 1;
							bakeJob->next = 0;
						}
						if (bakeJob->next >= bakeJob->covered.size())
							break;
					}
					const std::size_t i = bakeJob->next;
					if (!testable(i))
						continue;
					const auto x = static_cast<int32_t>(i % w), y = static_cast<int32_t>(i / w);
					const bool coarse = x % 2 == 0 && y % 2 == 0;
					if (coarse != (bakeJob->pass == 0))
						continue;
					if (!coarse) {
						// Its tested neighbours: the coarse texels round it (two along an even row or column, else four)
						int count = 0, rockCount = 0;
						float front[2] = { FLT_MAX, -FLT_MAX }, back[2] = { FLT_MAX, -FLT_MAX }, frontSum = 0.0f, backSum = 0.0f;
						for (const int32_t cy : { y - y % 2, y - y % 2 + 2 }) {
							for (const int32_t cx : { x - x % 2, x - x % 2 + 2 }) {
								if ((cx == x - x % 2 + 2 && x % 2 == 0) || (cy == y - y % 2 + 2 && y % 2 == 0))
									continue;  // (on an even row or column the texel lies between two, not four)
								if (cx < 0 || cy < 0 || cx >= w || cy >= static_cast<int32_t>(bakeJob->height))
									continue;
								const std::size_t c = static_cast<std::size_t>(cy) * w + cx;
								if (!testable(c))
									continue;
								count++;
								rockCount += bakeJob->rock[c];
								front[0] = std::min(front[0], bakeJob->clearFront[c]);
								front[1] = std::max(front[1], bakeJob->clearFront[c]);
								back[0] = std::min(back[0], bakeJob->clearBack[c]);
								back[1] = std::max(back[1], bakeJob->clearBack[c]);
								frontSum += bakeJob->clearFront[c];
								backSum += bakeJob->clearBack[c];
							}
						}
						const bool allRock = count >= 2 && rockCount == count;
						const bool allOpen = count >= 2 && rockCount == 0 && front[1] - front[0] <= CLEAR_AGREE && back[1] - back[0] <= CLEAR_AGREE;
						if (allRock || allOpen) {
							bakeJob->rock[i] = allRock ? 1 : 0;
							bakeJob->clearFront[i] = allRock ? 0.0f : frontSum / static_cast<float>(count);
							bakeJob->clearBack[i] = allRock ? 0.0f : backSum / static_cast<float>(count);
							bakedFilled++;
							bakedRockTexels += bakeJob->rock[i];
							continue;
						}
					}
					rays++;
					// From both sides: a curtain before a cliff has it on one side only (that ray starts inside and crosses its face once);
					// a boulder through the sheet is on both
					bool inside = true;
					for (const float side : { 1.0f, -1.0f }) {
						const RE::NiPoint3 out = bakeJob->normal[i] * side;
						int crossings = 0;
						float last = 0.0f;
						RockAlong(world, bakeJob->position[i] + out * CLIP_REACH, -out, CLIP_REACH - 0.5f, &crossings, &last);
						inside = inside && crossings % 2 == 1 && CLIP_REACH - 0.5f - last > BAKE_ROCK_MARGIN;
						if (!inside)
							break;
					}
					bakeJob->rock[i] = inside ? 1 : 0;
					// D104: how far rock lies off each side of the sheet (the water's depth behind it)
					if (!inside) {
						for (const float side : { 1.0f, -1.0f }) {
							// D105: from CLEAR_START out on the far side (in its water or air), through the sheet, to the first rock: rays
							// starting inside the rock the sheet lies on saw no face (Havok sees none from inside)
							const RE::NiPoint3 out = bakeJob->normal[i] * side;
							const float hit = RockAlong(world, bakeJob->position[i] - out * CLEAR_START, out, CLEAR_REACH + CLEAR_START);
							(side > 0.0f ? bakeJob->clearFront : bakeJob->clearBack)[i] = hit >= 0.0f ? std::clamp(hit - CLEAR_START, 0.0f, CLEAR_REACH) : CLEAR_REACH;
						}
					} else {
						bakeJob->clearFront[i] = bakeJob->clearBack[i] = 0.0f;
					}
					bakedTexels++;
					bakedRockTexels += bakeJob->rock[i];
				}
			}
			if (bakeJob->pass == 1 && bakeJob->next >= bakeJob->covered.size()) {
				auto job = std::shared_ptr<BakeJob>(std::move(bakeJob));
				logger::info("[Fluid Field]   bake job '{}': rock tested, solving", job->geometry->name.c_str());
				bakesPending.emplace_back(job->geometry, std::async(std::launch::async, [this, job]() {
					const auto start = std::chrono::steady_clock::now();
					auto field = SolveBake(*job);
					loadTiming.AddWorker(LoadWork::Solve, start);
					return field;
				}));
			}
		}
	}

	void FallSites::CaptureStep(bool a_canFinish)
	{
		// D185: handle a finished capture (log, cache to disk, reground) only when no worker holds what that touches; rays go on meanwhile
		for (const auto& site : sites) {
			if (!a_canFinish)
				break;
			auto it = site.captureKey ? captures.find(site.captureKey) : captures.end();
			if (it == captures.end() || !it->second.IsComplete() || it->second.reported)
				continue;
			auto& capture = it->second;
			capture.reported = true;
			if (!capture.cachePath.empty() && capture.GetProgress() >= 1.0f && !std::filesystem::exists(capture.cachePath)) {
				std::error_code error;
				std::filesystem::create_directories(std::filesystem::path(capture.cachePath).parent_path(), error);
				capture.Dump(capture.cachePath);
			}
			uint32_t holes = 0;
			for (uint32_t y = 0; y < capture.GetHeight(); y++) {
				for (uint32_t x = 0; x < capture.GetWidth(); x++)
					holes += capture.At(x, y) <= FallCapture::NONE ? 1 : 0;
			}
			logger::info("[Fluid Field] Waterfall site #{} ground captured: {}x{} at {:.0f}, {:.0f}, {} holes left", site.id, capture.GetWidth(), capture.GetHeight(),
				capture.GetOriginX(), capture.GetOriginY(), holes);
			logger::info("[Fluid Field]   rays: {}", capture.DescribeRays());
			// Diagnostics: the captured rock, next to the log
			if (const auto directory = logger::log_directory()) {
				const auto path = (*directory / std::format("FluidField_rock_site{}.bin", site.id)).string();
				if (capture.Dump(path))
					logger::info("[Fluid Field]   rock written to {}", path);
			}
			RefreshGrounded();
			return;
		}
		// D185: one capture at a time, the nearest the camera that can progress (one whose cell has no physics world waits without
		// holding up the rest)
		FallCapture* nearest = nullptr;
		float nearestDistance = FLT_MAX;
		auto* camera = PlayerCamera();
		const RE::NiPoint3 eye = camera ? camera->world.translate : RE::NiPoint3(0.0f, 0.0f, 0.0f);
		for (auto& [key, capture] : captures) {
			if (capture.IsComplete() || !capture.Castable())
				continue;
			const float dx = capture.GetOriginX() + 0.5f * capture.GetWidth() * FallCapture::CELL - eye.x;
			const float dy = capture.GetOriginY() + 0.5f * capture.GetHeight() * FallCapture::CELL - eye.y;
			const float distance = dx * dx + dy * dy;
			if (distance < nearestDistance) {
				nearestDistance = distance;
				nearest = &capture;
			}
		}
		if (nearest)
			nearest->Cast(CAPTURE_BUDGET_MS);
	}

	uint32_t FallSites::CountKind(Kind a_kind) const
	{
		return static_cast<uint32_t>(std::count_if(sites.begin(), sites.end(), [&](const Site& a_site) { return a_site.kind == a_kind; }));
	}

	bool FallSites::LoadIdle() const
	{
		// (a capture whose cell has no physics world loaded never makes progress: not pending)
		bool capturing = false;
		for (const auto& [key, capture] : captures)
			capturing = capturing || (!capture.IsComplete() && capture.Castable()) || (capture.IsComplete() && !capture.reported);
		return !capturing && !Busy() && !CullBusy() && !denseWork.valid() && bakeQueue.empty() && !bakeJob && bakesPending.empty() && clipQueue.empty() && buryQueue.empty() &&
		       buryDone && !plungesDue && !linkDue;
	}

	void FallSites::TrackLoad()
	{
		const auto now = std::chrono::steady_clock::now();
		if (!LoadIdle()) {
			if (!loadTiming.active) {
				loadTiming.active = true;
				loadTiming.firstActive = now;
			}
			loadTiming.lastActive = now;
			return;
		}
		if (!loadTiming.active || sites.empty() || std::chrono::duration<double>(now - loadTiming.lastActive).count() < SETTLE_SECONDS)
			return;
		// D185: settled: when the work ended (wall clock, for the timing script), and where its time went
		loadTiming.active = false;
		loadTiming.reports++;
		const auto endedAt = std::chrono::system_clock::now() - std::chrono::duration_cast<std::chrono::system_clock::duration>(now - loadTiming.lastActive);
		const auto startedAt = endedAt - std::chrono::duration_cast<std::chrono::system_clock::duration>(loadTiming.lastActive - loadTiming.firstActive);
		static constexpr const char* STAGES[] = { "scan", "capture", "clip", "bury", "cull", "depth", "bake", "pools", "finish" };
		static constexpr const char* WORKS[] = { "pools", "dense", "seams", "cull", "link", "plunges", "solve" };
		std::string stages, works;
		double mainTotal = 0.0;
		for (std::size_t k = 0; k < static_cast<std::size_t>(LoadStage::Count); k++) {
			mainTotal += loadTiming.mainMs[k];
			stages += std::format("{}{} {:.0f} ms/{} frames", k ? ", " : "", STAGES[k], loadTiming.mainMs[k], loadTiming.mainFrames[k]);
		}
		for (std::size_t k = 0; k < static_cast<std::size_t>(LoadWork::Count); k++)
			works += std::format("{}{} {:.0f} ms/{} runs", k ? ", " : "", WORKS[k], loadTiming.workerUs[k].load() / 1000.0, loadTiming.workerRuns[k].load());
		uint32_t complete = 0, total = 0;
		CountCaptures(complete, total);
		const auto clock = [](std::chrono::system_clock::time_point a_time) {
			return std::format("{:%H:%M:%S}", std::chrono::floor<std::chrono::milliseconds>(std::chrono::current_zone()->to_local(a_time)));
		};
		logger::info("[Fluid Field] Falls settled (D185) #{}: work {} to {} ({:.1f} s); {} sites, {} captures, {} baked meshes ({} from the cache, {} solved)", loadTiming.reports,
			clock(startedAt), clock(endedAt), std::chrono::duration<double>(loadTiming.lastActive - loadTiming.firstActive).count(), sites.size(), total, bakeFields.size(),
			bakesCached, bakesSolved);
		logger::info("[Fluid Field]   render thread {:.0f} ms: {}", mainTotal, stages);
		logger::info("[Fluid Field]   workers: {}", works);
	}

	void FallSites::Update(const WaterSurfaces& a_water)
	{
		TrackLoad();
		// D185: each step's render-thread time
		const auto timed = [this](LoadStage a_stage, auto&& a_step) {
			const auto start = std::chrono::steady_clock::now();
			a_step();
			loadTiming.mainMs[static_cast<std::size_t>(a_stage)] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			loadTiming.mainFrames[static_cast<std::size_t>(a_stage)]++;
		};
		// D172: the workers read the water planes (ComputePushes, ComputePlunges): a copy, taken only while none runs
		if (!Busy() && a_water.GetSignature() != waterCopySignature) {
			waterCopy = a_water;
			waterCopySignature = a_water.GetSignature();
		}
		waterSurfaces = &waterCopy;
		timed(LoadStage::Finish, [&]() {
			FinishHeavyWork();
			if (!Busy())
				FinishDenseWork();
			if (!Busy() && wobbleImpactsDue)
				ApplyWobbleImpacts();
			if (!Busy() && unifiedLoaded)
				ReloadChangedBakes();
		});
		if (!Busy() && plungesDue) {
			// D172: the plunges on a worker (up to 150 ms on the render thread, three to five times a load)
			plungesDue = false;
			plungeResult = std::make_unique<PlungeResult>();
			// D182: the landscape round every baked sheet, copied for the worker (no spray from texels under it)
			if (landscape && !RE::TES::GetSingleton()->interiorCell) {
				float lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
				for (const auto& [geometry, field] : bakeFields) {
					if (!field)
						continue;
					for (const auto& p : field->position) {
						lo[0] = std::min(lo[0], p.x);
						lo[1] = std::min(lo[1], p.y);
						hi[0] = std::max(hi[0], p.x);
						hi[1] = std::max(hi[1], p.y);
					}
				}
				if (lo[0] <= hi[0]) {
					plungeResult->land = landscape->Subset(lo[0] - 64.0f, lo[1] - 64.0f, hi[0] + 64.0f, hi[1] + 64.0f);
					plungeResult->haveLand = true;
				}
			}
			heavyWork = std::async(std::launch::async, [this, result = plungeResult.get()]() {
				const auto start = std::chrono::steady_clock::now();
				ComputePlunges(*result);
				loadTiming.AddWorker(LoadWork::Plunges, start);
			});
		}
		if (Busy()) {
			// D172: a worker has vertexClips, vertexCuts, the pools and the bakes: only poll it (the culling's step only polls its own
			// worker; it restarts once nothing else runs)
			if (CullWorking())
				timed(LoadStage::Cull, [&]() { CullStep(); });
			// D185: the captures' and the bakes' rays go on meanwhile (they touch nothing the workers have)
			timed(LoadStage::Capture, [&]() { CaptureStep(false); });
			timed(LoadStage::Bake, [&]() { BakeRays(); });
			ProbeScrolls();
			return;
		}
		// D172: captures (their end regrounds the meshes: RefreshGrounded) only while no worker reads them. A step that starts a
		// worker ends the update: nothing after it may touch the worker's data, and no second worker may start beside it.
		timed(LoadStage::Capture, [&]() { CaptureStep(true); });
		if (Busy())
			return;
		timed(LoadStage::Clip, [&]() { ClipStep(); });
		if (Busy())
			return;
		timed(LoadStage::Bury, [&]() { BuryStep(); });
		timed(LoadStage::Cull, [&]() { CullStep(); });  // D113
		if (Busy())
			return;
		timed(LoadStage::Depth, [&]() { DepthStep(); });
		timed(LoadStage::Bake, [&]() { BakeStep(); });
		if (Busy())
			return;
		timed(LoadStage::Pools, [&]() { PollPools(); });
		if (Busy())
			return;
		ProbeScrolls();
		if (updatesUntilScan > 0) {
			updatesUntilScan--;
			return;
		}
		updatesUntilScan = SCAN_INTERVAL;
		const auto scanStart = std::chrono::steady_clock::now();
		const auto scanned = [&]() {
			loadTiming.mainMs[static_cast<std::size_t>(LoadStage::Scan)] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - scanStart).count();
			loadTiming.mainFrames[static_cast<std::size_t>(LoadStage::Scan)]++;
		};

		// S1, inventory
		auto* tes = RE::TES::GetSingleton();
		if (!tes)
			return;
		std::vector<FxRef> found;
		// Only the loaded grid: its water is known there, so sites elsewhere cannot be paired
		float gridMin[2] = { FLT_MAX, FLT_MAX };
		float gridMax[2] = { -FLT_MAX, -FLT_MAX };
		if (!tes->interiorCell && tes->gridCells) {
			auto* grid = tes->gridCells;
			for (uint32_t gx = 0; gx < grid->length; gx++) {
				for (uint32_t gy = 0; gy < grid->length; gy++) {
					auto* cell = GetGridCell(grid, gx, gy);
					const auto* coordinates = cell && cell->IsAttached() ? cell->GetCoordinates() : nullptr;
					if (!coordinates)
						continue;
					gridMin[0] = std::min(gridMin[0], coordinates->cellX * 4096.0f);
					gridMin[1] = std::min(gridMin[1], coordinates->cellY * 4096.0f);
					gridMax[0] = std::max(gridMax[0], (coordinates->cellX + 1) * 4096.0f);
					gridMax[1] = std::max(gridMax[1], (coordinates->cellY + 1) * 4096.0f);
				}
			}
		}
		const auto inGrid = [&](const RE::NiPoint3& a_position) {
			return tes->interiorCell || (a_position.x >= gridMin[0] && a_position.x <= gridMax[0] && a_position.y >= gridMin[1] && a_position.y <= gridMax[1]);
		};
		const auto visitCell = [&](RE::TESObjectCELL* a_cell, bool a_requireAttached) {
			if (!a_cell || (a_requireAttached && !a_cell->IsAttached()))
				return;
			a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted() || !a_ref->Get3D() || !inGrid(a_ref->GetPosition()))
					return RE::BSContainer::ForEachResult::kContinue;
				FxClass cls;
				if (!Classify(ModelOf(a_ref->GetBaseObject()), cls))
					return RE::BSContainer::ForEachResult::kContinue;
				FxRef ref{ a_ref->GetFormID(), cls, {}, {} };
				if (WorldBounds(*a_ref, ref.min, ref.max))
					found.push_back(ref);
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
		std::sort(found.begin(), found.end(), [](const FxRef& a, const FxRef& b) { return a.formID < b.formID; });

		uint64_t signature = found.size();
		for (const auto& ref : found)
			signature = signature * 1099511628211ull ^ ref.formID;
		if (signature == refSignature && a_water.GetSignature() == waterSignature) {
			scanned();
			return;
		}
		refSignature = signature;
		waterSignature = a_water.GetSignature();
		refs.swap(found);
		Rebuild(a_water);
		Summarise();
		scanned();
	}

	void FallSites::Rebuild(const WaterSurfaces& a_water)
	{
		sites.clear();
		const std::size_t n = refs.size();

		// S2, clustering: union-find over references within CLUSTER_RADIUS of each other
		std::vector<std::size_t> parent(n);
		std::iota(parent.begin(), parent.end(), std::size_t{ 0 });
		const auto find = [&](std::size_t a_i) {
			while (parent[a_i] != a_i) {
				parent[a_i] = parent[parent[a_i]];
				a_i = parent[a_i];
			}
			return a_i;
		};
		// Rivers are lined with rapids and splash meshes in unbroken chains that would join a whole gorge into one site. So falls
		// cluster among themselves; other effects join the nearest fall within CLUSTER_RADIUS, and the rest form rapids sites at
		// RAPIDS_CLUSTER_RADIUS. Skirts are tall mist sheets, so only the falling water makes a fall.
		const auto isFall = [&](std::size_t a_i) { return refs[a_i].cls == FxClass::Body; };
		for (std::size_t i = 0; i < n; i++) {
			for (std::size_t j = i + 1; j < n; j++) {
				if (isFall(i) && isFall(j) && BoxGap(refs[i].min, refs[i].max, refs[j].min, refs[j].max) <= CLUSTER_RADIUS)
					parent[find(i)] = find(j);
			}
		}
		std::vector<bool> attached(n, false);
		for (std::size_t i = 0; i < n; i++) {
			if (isFall(i))
				continue;
			float best = CLUSTER_RADIUS;
			std::size_t bestFall = n;
			for (std::size_t j = 0; j < n; j++) {
				if (!isFall(j))
					continue;
				const float gap = BoxGap(refs[i].min, refs[i].max, refs[j].min, refs[j].max);
				if (gap <= best) {
					best = gap;
					bestFall = j;
				}
			}
			if (bestFall < n) {
				parent[find(i)] = find(bestFall);
				attached[i] = true;
			}
		}
		for (std::size_t i = 0; i < n; i++) {
			for (std::size_t j = i + 1; j < n; j++) {
				if (!isFall(i) && !isFall(j) && !attached[i] && !attached[j] && BoxGap(refs[i].min, refs[i].max, refs[j].min, refs[j].max) <= RAPIDS_CLUSTER_RADIUS)
					parent[find(i)] = find(j);
			}
		}
		std::unordered_map<std::size_t, std::size_t> siteOf;
		for (std::size_t i = 0; i < n; i++) {
			const std::size_t root = find(i);
			auto [it, added] = siteOf.try_emplace(root, sites.size());
			if (added) {
				Site site{};
				site.id = static_cast<uint32_t>(sites.size());
				for (int k = 0; k < 3; k++) {
					site.min[k] = FLT_MAX;
					site.max[k] = -FLT_MAX;
				}
				sites.push_back(site);
			}
			Site& site = sites[it->second];
			site.members.push_back(static_cast<uint32_t>(i));
			site.classCounts[static_cast<int>(refs[i].cls)]++;
			for (int k = 0; k < 3; k++) {
				site.min[k] = std::min(site.min[k], refs[i].min[k]);
				site.max[k] = std::max(site.max[k], refs[i].max[k]);
			}
		}

		// S3, level pairing and tiers, on the falling water's bounds (the site's include mist and splashes far from the drop)
		std::vector<WaterSurfaces::Plane> planes;
		for (auto& site : sites) {
			for (int k = 0; k < 3; k++) {
				site.fallMin[k] = FLT_MAX;
				site.fallMax[k] = -FLT_MAX;
			}
			for (const uint32_t member : site.members) {
				if (refs[member].cls != FxClass::Body)
					continue;
				for (int k = 0; k < 3; k++) {
					site.fallMin[k] = std::min(site.fallMin[k], refs[member].min[k]);
					site.fallMax[k] = std::max(site.fallMax[k], refs[member].max[k]);
				}
			}
			if (site.fallMin[0] > site.fallMax[0]) {
				for (int k = 0; k < 3; k++) {
					site.fallMin[k] = site.min[k];
					site.fallMax[k] = site.max[k];
				}
			}
			for (int k = 0; k < 3; k++)
				site.centroid[k] = 0.5f * (site.fallMin[k] + site.fallMax[k]);
			planes.clear();
			a_water.CollectPlanes(site.fallMin[0] - PAIR_MARGIN, site.fallMin[1] - PAIR_MARGIN, site.fallMax[0] + PAIR_MARGIN, site.fallMax[1] + PAIR_MARGIN, planes);
			// For the log: every plane within a wider search, and what pairing made of it
			site.candidates.clear();
			{
				std::vector<WaterSurfaces::Plane> wider;
				constexpr float WIDE = 1024.0f;
				a_water.CollectPlanes(site.fallMin[0] - WIDE, site.fallMin[1] - WIDE, site.fallMax[0] + WIDE, site.fallMax[1] + WIDE, wider);
				for (const auto& plane : wider) {
					const bool inMargin = plane.maxX >= site.fallMin[0] - PAIR_MARGIN && plane.minX <= site.fallMax[0] + PAIR_MARGIN &&
					                      plane.maxY >= site.fallMin[1] - PAIR_MARGIN && plane.minY <= site.fallMax[1] + PAIR_MARGIN;
					const char* verdict = !inMargin                                         ? "outside margin" :
					                      plane.height > site.fallMax[2] + NEAR_LEVEL_ABOVE ? "too high" :
					                      plane.height < site.fallMin[2] - NEAR_LEVEL       ? "too low" :
					                                                                          "paired";
					site.candidates += std::format(" {:.0f}{}[{}, x {:.0f}..{:.0f} y {:.0f}..{:.0f}]", plane.height,
						plane.source == WaterSurfaces::Source::CellWater ? "c" : "o", verdict, plane.minX, plane.maxX, plane.minY, plane.maxY);
				}
			}
			std::vector<float> heights;
			site.planes.clear();
			const float clipMin[2] = { site.fallMin[0] - PAIR_MARGIN, site.fallMin[1] - PAIR_MARGIN };
			const float clipMax[2] = { site.fallMax[0] + PAIR_MARGIN, site.fallMax[1] + PAIR_MARGIN };
			for (const auto& plane : planes) {
				// Planes may lie well under the falling water (it runs on underwater), but not much above its top
				if (plane.height < site.fallMin[2] - NEAR_LEVEL || plane.height > site.fallMax[2] + NEAR_LEVEL_ABOVE)
					continue;
				heights.push_back(plane.height);
				// Where this water is, trimmed to the area around the fall
				site.planes.push_back({ std::max(plane.minX, clipMin[0]), std::max(plane.minY, clipMin[1]), std::min(plane.maxX, clipMax[0]), std::min(plane.maxY, clipMax[1]), plane.height });
			}
			std::sort(heights.begin(), heights.end(), std::greater<float>());
			for (const float height : heights) {
				if (site.levels.empty() || site.levels.back() - height > LEVEL_TOLERANCE)
					site.levels.push_back(height);
			}

			const bool hasFallMeshes = site.classCounts[static_cast<int>(FxClass::Body)] > 0;
			if (site.levels.empty()) {
				site.kind = Kind::Failed;
				site.failure = "S3: no water plane";
				continue;
			}
			if (!hasFallMeshes) {
				// Rapids and splashes only: no drop to reconstruct, the water simulation and foam carry it
				site.top = site.levels.front();
				site.bottom = site.levels.back();
				site.kind = Kind::Rapids;
				continue;
			}
			if (site.levels.size() == 1) {
				const float fallMin = site.fallMin[2];
				const float fallMax = site.fallMax[2];
				const float level = site.levels.front();
				// A plane near the falling water's top is its top, and the fall ends on ground; a lower one is its bottom (falling water
				// often runs on well below the surface)
				if (level < fallMax - RAPIDS_THRESHOLD) {
					// The only plane is at the bottom: the falling water's top stands in for the top level
					site.bottom = level;
					site.top = std::max(fallMax, level);
					site.topFromMesh = true;
					site.levels.insert(site.levels.begin(), site.top);
				} else {
					site.top = level;
					site.bottom = fallMin;
					site.kind = Kind::NoBottom;
					continue;
				}
			} else {
				site.top = site.levels.front();
				site.bottom = site.levels.back();
			}
			const float drop = site.top - site.bottom;
			if (drop < RAPIDS_THRESHOLD || !hasFallMeshes)
				site.kind = Kind::Rapids;
			else
				site.kind = site.levels.size() > 2 ? Kind::Cascade : Kind::Fall;
		}

		// S4: a capture per site with a drop, kept across rebuilds while the site is the same
		std::unordered_map<uint64_t, FallCapture> kept;
		for (auto& site : sites) {
			if (site.kind != Kind::Fall && site.kind != Kind::Cascade && site.kind != Kind::NoBottom)
				continue;
			uint64_t key = 1469598103934665603ull;
			const auto mix = [&](float a_value) { key = (key ^ static_cast<uint64_t>(static_cast<int64_t>(std::lround(a_value / 16.0f)))) * 1099511628211ull; };
			for (int k = 0; k < 2; k++) {
				mix(site.fallMin[k]);
				mix(site.fallMax[k]);
			}
			for (const float level : site.levels)
				mix(level);
			mix(site.bottom);
			key = key ? key : 1;
			site.captureKey = key;
			if (kept.contains(key))
				continue;
			if (auto it = captures.find(key); it != captures.end()) {
				kept.emplace(key, std::move(it->second));
				continue;
			}
			auto& capture = kept[key];
			capture.Begin(site.fallMin, site.fallMax, site.top, site.bottom);
			// D185: cached on disk, keyed by its layout and the statics round it (what its rays see besides the landscape); read back, no
			// rays
			capture.cachePath = CaptureCachePath(capture, key);
			if (!capture.cachePath.empty() && capture.Load(capture.cachePath))
				logger::info("[Fluid Field] Waterfall site #{} ground read from the cache", site.id);
		}
		captures.swap(kept);
		// Pools of sites that are gone
		std::erase_if(sitePools, [&](const auto& a_entry) { return !captures.contains(a_entry.first); });
		RefreshGrounded();
		RefreshHiddenEffects();
	}

	void FallSites::BuildGroundMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_heights) const
	{
		a_heights.assign(static_cast<std::size_t>(a_size) * a_size, FallCapture::NONE);
		for (const auto& [key, capture] : captures) {
			if (!capture.IsComplete())
				continue;
			const float minX = capture.GetOriginX();
			const float minY = capture.GetOriginY();
			const float maxX = minX + static_cast<float>(capture.GetWidth() - 1) * FallCapture::CELL;
			const float maxY = minY + static_cast<float>(capture.GetHeight() - 1) * FallCapture::CELL;
			const auto x0 = std::max(0, static_cast<int32_t>(std::ceil((minX - a_originX) / a_cell)));
			const auto y0 = std::max(0, static_cast<int32_t>(std::ceil((minY - a_originY) / a_cell)));
			const auto x1 = std::min(static_cast<int32_t>(a_size) - 1, static_cast<int32_t>(std::floor((maxX - a_originX) / a_cell)));
			const auto y1 = std::min(static_cast<int32_t>(a_size) - 1, static_cast<int32_t>(std::floor((maxY - a_originY) / a_cell)));
			for (int32_t y = y0; y <= y1; y++) {
				for (int32_t x = x0; x <= x1; x++) {
					float height;
					if (!capture.Sample(a_originX + static_cast<float>(x) * a_cell, a_originY + static_cast<float>(y) * a_cell, height))
						continue;
					float& stored = a_heights[static_cast<std::size_t>(y) * a_size + x];
					stored = std::max(stored, height);
				}
			}
		}
	}

	void FallSites::CollectWaterPlanes(float a_minX, float a_minY, float a_maxX, float a_maxY, std::vector<std::array<float, 5>>& a_planes) const
	{
		a_planes.clear();
		if (!waterSurfaces)
			return;
		std::vector<WaterSurfaces::Plane> planes;
		waterSurfaces->CollectPlanes(a_minX, a_minY, a_maxX, a_maxY, planes);
		for (const auto& plane : planes) {
			if (plane.source != WaterSurfaces::Source::None)
				a_planes.push_back({ plane.minX, plane.minY, plane.maxX, plane.maxY, plane.height });
		}
	}

	bool FallSites::PoolLevelAt(float a_x, float a_y, float& a_level) const
	{
		bool found = false;
		for (const auto& [key, pools] : sitePools) {
			const auto x = static_cast<int32_t>(std::lround((a_x - pools.originX) / FallCapture::CELL));
			const auto y = static_cast<int32_t>(std::lround((a_y - pools.originY) / FallCapture::CELL));
			if (x < 0 || y < 0 || x >= static_cast<int32_t>(pools.width) || y >= static_cast<int32_t>(pools.height))
				continue;
			const float cover = pools.cover[static_cast<std::size_t>(y) * pools.width + x];
			if (cover <= NO_SURFACE)
				continue;
			a_level = found ? std::max(a_level, cover) : cover;
			found = true;
		}
		return found;
	}

	void FallSites::BuildPoolMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_heights) const
	{
		// D40: where each site's pools really have water (D78: ComputePools), on the same lattice as its capture
		a_heights.assign(static_cast<std::size_t>(a_size) * a_size, FallCapture::NONE);
		const auto n = static_cast<int32_t>(a_size);
		for (const auto& [key, pools] : sitePools) {
			const auto w = static_cast<int32_t>(pools.width);
			const auto h = static_cast<int32_t>(pools.height);
			const auto offsetX = static_cast<int32_t>(std::lround((pools.originX - a_originX) / a_cell));
			const auto offsetY = static_cast<int32_t>(std::lround((pools.originY - a_originY) / a_cell));
			for (int32_t y = std::max(0, -offsetY); y < h && y + offsetY < n; y++) {
				for (int32_t x = std::max(0, -offsetX); x < w && x + offsetX < n; x++) {
					const float cover = pools.cover[static_cast<std::size_t>(y) * w + x];
					if (cover <= NO_SURFACE)
						continue;
					// D50: the pool the falls come from is cut deeper: the lip dips under it where its water does not show
					const float cut = cover >= pools.top - POOL_BAND ? cover - TOP_POOL_CUT : cover;
					float& stored = a_heights[static_cast<std::size_t>(y + offsetY) * a_size + static_cast<std::size_t>(x + offsetX)];
					stored = std::max(stored, cut);
				}
			}
		}
	}

	void FallSites::BuildPlungeMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_field, float a_behind, float a_radiusScale) const
	{
		// D44, D47: where the falls meet the water below: every plunge point (D78: from the meshes' sims, ComputePlunges) foams the
		// water round it, harder plunges more; the simulated flow carries the foam downstream
		a_field.assign(static_cast<std::size_t>(a_size) * a_size, 0.0f);
		const auto n = static_cast<int32_t>(a_size);
		for (const auto& plunge : plungePoints) {
			// D98: a crossing reaches further (its foam made 50% stronger)
			const float radius = PLUNGE_FOAM_RADIUS * std::max(plunge.reach, 1.0f) * std::max(a_radiusScale, 0.05f);
			const auto reach = static_cast<int32_t>(std::ceil(radius / a_cell));
			// D176: moved a_behind world units against the falling water's way (behind the curtain's line)
			const float px = plunge.position[0] - plunge.direction[0] * a_behind;
			const float py = plunge.position[1] - plunge.direction[1] * a_behind;
			const auto cx = static_cast<int32_t>(std::lround((px - a_originX) / a_cell));
			const auto cy = static_cast<int32_t>(std::lround((py - a_originY) / a_cell));
			if (cx < -reach || cy < -reach || cx >= n + reach || cy >= n + reach)
				continue;
			const float strength = std::clamp(0.85f + 0.15f * plunge.strength, 0.0f, 1.0f);  // D52: the rim is solid for every fall
			for (int32_t y = cy - reach; y <= cy + reach; y++) {
				for (int32_t x = cx - reach; x <= cx + reach; x++) {
					if (x < 0 || y < 0 || x >= n || y >= n)
						continue;
					const float dx = a_originX + static_cast<float>(x) * a_cell - px;
					const float dy = a_originY + static_cast<float>(y) * a_cell - py;
					const float falloff = std::clamp(1.0f - std::sqrt(dx * dx + dy * dy) / radius, 0.0f, 1.0f);
					float& value = a_field[static_cast<std::size_t>(y) * a_size + x];
					value = std::max(value, strength * falloff);
				}
			}
		}
	}

	void FallSites::ApplyPlunges(PlungeResult& a_result)
	{
		// D188: the pool landings into the bakes' plunge section (x the impact, y its foam carried on), for the falls' foam
		bool plunged = false;
		for (const auto& [geometry, impact] : a_result.impacts) {
			const auto bake = flowBakes.find(geometry);
			const auto landing = a_result.landings.find(geometry);
			if (bake == flowBakes.end() || landing == a_result.landings.end())
				continue;
			const std::size_t count = static_cast<std::size_t>(bake->second.width) * bake->second.height;
			if (bake->second.texels.size() != count * BAKE_TEXELS_PER || impact.size() != count || landing->second.size() != count)
				continue;
			for (std::size_t i = 0; i < count; i++) {
				float* fourth = &bake->second.texels[(count * 3 + i) * 4];
				fourth[0] = impact[i];
				fourth[1] = landing->second[i];
			}
			plunged = true;
		}
		if (plunged) {
			flowBakeVersion++;
			wobbleImpactsDue = true;  // D191: the wobble takes them in once no worker holds vertexClips
		}
		plungePoints.swap(a_result.plungePoints);
		sprayEmitters.swap(a_result.sprayEmitters);
		emitterVersion++;
		groundVersion++;  // The plunge map
		logger::info("[Fluid Field] Waterfall plunges: {} texels meet the water below, {} plunge points ({} where a mesh crosses a water surface); spray emitters: {} at plunges, {} droplets-only on steep seams, {} striking rock, {} past the crests ({} under the landscape left out); crest columns {}, on meshes from a water {}, turning down {} ({} deep under a sheet), sheets meeting {} texels",
			a_result.meets, plungePoints.size(), a_result.lips, a_result.kinds[1], a_result.kinds[3], a_result.kinds[2], a_result.kinds[0], a_result.buried, a_result.crestColumns,
			a_result.lipColumns, a_result.dropColumns, a_result.submergedDrops, a_result.footColumns);
	}

	void FallSites::ApplyWobbleImpacts()
	{
		wobbleImpactsDue = false;
		uint32_t raised = 0;
		float most = 0.0f;
		for (const auto& [geometry, impact] : wobbleImpacts) {
			const auto bake = flowBakes.find(geometry);
			const auto base = vertexClipBase.find(geometry);
			if (bake == flowBakes.end() || base == vertexClipBase.end())
				continue;
			const FlowBake& flow = bake->second;
			const auto fw = static_cast<int32_t>(flow.width), fh = static_cast<int32_t>(flow.height);
			const std::size_t count = static_cast<std::size_t>(fw) * fh;
			const std::size_t vertices = impact.size.size();
			if (flow.texels.size() != count * BAKE_TEXELS_PER || base->second + vertices > vertexClips.size() || impact.weight.size() != vertices || impact.uv.size() != vertices ||
				impact.direction.size() != vertices)
				continue;
			// The impact (the plunge section's x) bilinear at a vertex's texture coordinates
			const auto impactAt = [&](float a_u, float a_v) {
				const float gx = std::clamp((a_u - flow.uvMin[0]) / std::max(flow.uvSize[0], 1e-6f) * static_cast<float>(fw) - 0.5f, 0.0f, static_cast<float>(fw - 1));
				const float gy = std::clamp((a_v - flow.uvMin[1]) / std::max(flow.uvSize[1], 1e-6f) * static_cast<float>(fh) - 0.5f, 0.0f, static_cast<float>(fh - 1));
				const int32_t x0 = std::min(static_cast<int32_t>(gx), fw - 1), y0 = std::min(static_cast<int32_t>(gy), fh - 1);
				const int32_t x1 = std::min(x0 + 1, fw - 1), y1 = std::min(y0 + 1, fh - 1);
				const float fx = gx - static_cast<float>(x0), fy = gy - static_cast<float>(y0);
				const auto at = [&](int32_t a_x, int32_t a_y) { return flow.texels[(count * 3 + static_cast<std::size_t>(a_y) * fw + a_x) * 4]; };
				return (at(x0, y0) * (1.0f - fx) + at(x1, y0) * fx) * (1.0f - fy) + (at(x0, y1) * (1.0f - fx) + at(x1, y1) * fx) * fy;
			};
			for (std::size_t v = 0; v < vertices; v++) {
				if (impact.size[v] <= 0.0f)
					continue;
				const float hit = std::clamp(impactAt(impact.uv[v][0], impact.uv[v][1]), 0.0f, 1.0f);
				const float extra = WOBBLE_IMPACT * hit * hit * (3.0f - 2.0f * hit) * impact.weight[v];
				const float size = impact.size[v] + (std::isfinite(extra) ? extra : 0.0f);
				auto& value = vertexClips[base->second + v];
				value[8] = impact.direction[v].x * size;
				value[9] = impact.direction[v].y * size;
				value[10] = impact.direction[v].z * size;
				raised += extra > 0.5f ? 1 : 0;
				most = std::max(most, size);
			}
		}
		vertexClipVersion++;
		logger::info("[Fluid Field] Waterfall wobble: {} vertices raised where the plunges land, at most {:.1f} units", raised, most);
	}

	void FallSites::ComputePlunges(PlungeResult& o_result) const
	{
		// D78: from each lit mesh's own sim (its BakeField): where it enters the water below (foot texels at or under it, coming from
		// above) and where its water strikes rock (the bake's foam). The water's way is the sim's; its speed from the height fallen.
		std::unordered_map<int64_t, FallPlunge> points;
		std::unordered_map<int64_t, FallEmitter> chosen;
		const auto offer = [&](const RE::NiPoint3& a_at, const RE::NiPoint3& a_up, const RE::NiPoint3& a_direction, float a_speed, float a_strength, FallEmitterKind a_kind, float a_spacing) {
			// D182: never from the ground: a texel under the landscape throws nothing (the bake reads the whole mesh, D136).
			// Texels under a water surface arrive already lifted onto it.
			const RE::NiPoint3& a_p = a_at;
			const RE::NiPoint3& a_n = a_up;
			float ground = 0.0f;
			if (o_result.haveLand && o_result.land.SampleHeight(a_p.x, a_p.y, ground) && a_p.z < ground - SPRAY_BURIED) {
				o_result.buried++;
				return;
			}
			const auto key = (static_cast<int64_t>(std::floor(a_p.x / a_spacing)) << 40) ^ (static_cast<int64_t>(std::floor(a_p.y / a_spacing)) << 18) ^
			                 static_cast<int64_t>(std::floor(a_p.z / a_spacing)) ^ (static_cast<int64_t>(a_kind) << 60);
			auto [it, added] = chosen.try_emplace(key);
			if (!added && it->second.strength >= a_strength)
				return;
			FallEmitter& emitter = it->second;
			emitter.position[0] = a_p.x;
			emitter.position[1] = a_p.y;
			emitter.position[2] = a_p.z;
			emitter.normal[0] = a_n.x;
			emitter.normal[1] = a_n.y;
			emitter.normal[2] = a_n.z;
			emitter.direction[0] = a_direction.x;
			emitter.direction[1] = a_direction.y;
			emitter.direction[2] = a_direction.z;
			emitter.speed = a_speed;
			emitter.strength = std::clamp(a_strength, 0.0f, 1.0f);
			emitter.kind = static_cast<float>(a_kind);
		};
		// D184: each mesh's shell (ShellTexels) and its shown water (shell texels not in rock) in MEET_CELL cells, so a texel can tell
		// whether another mesh's shown water lies within MEET_EMIT: where two sheets run into each other (the culling's meet line, D126)
		std::unordered_map<const RE::BSGeometry*, std::vector<uint8_t>> shells;
		struct ShownTexel
		{
			const RE::BSGeometry* geometry;
			RE::NiPoint3 p;
			RE::NiPoint3 normal;  // D189: its surface's (which way it faces), for how another sheet's water meets it
		};
		std::unordered_map<int64_t, std::vector<ShownTexel>> shownCells;
		// D184: and each mesh's highest shown texel per SURFACE_CELL column: no emitter deep under another sheet's water
		std::unordered_map<const RE::BSGeometry*, std::unordered_map<int64_t, float>> shownSurfaces;
		const auto surfaceKey = [](float a_x, float a_y) { return (static_cast<int64_t>(std::floor(a_x / SURFACE_CELL)) << 32) ^ static_cast<uint32_t>(static_cast<int32_t>(std::floor(a_y / SURFACE_CELL))); };
		const auto meetCell = [](int64_t a_x, int64_t a_y, int64_t a_z) { return (a_x & 0x1FFFFF) << 42 | (a_y & 0x1FFFFF) << 21 | (a_z & 0x1FFFFF); };
		const auto meetOf = [](float a_v) { return static_cast<int64_t>(std::floor(a_v / MEET_CELL)); };
		for (const auto& [geometry, field] : bakeFields) {
			if (!field || !grounded.contains(geometry))
				continue;
			std::vector<uint8_t> shell = field->covered;
			{
				const float uvMin[2] = { field->uvMin[0], field->uvMin[1] }, uvSize[2] = { field->uvSize[0], field->uvSize[1] };
				const auto inShell = ShellTexels(const_cast<RE::BSGeometry*>(geometry), field->width, field->height, uvMin, uvSize, nullptr);
				for (std::size_t i = 0; i < inShell.size() && i < shell.size(); i++)
					shell[i] = shell[i] && inShell[i] ? 1 : 0;
			}
			auto& surface = shownSurfaces[geometry];
			for (std::size_t i = 0; i < shell.size() && i < field->position.size(); i++) {
				if (shell[i] && (field->rock.empty() || !field->rock[i])) {
					const RE::NiPoint3& p = field->position[i];
					shownCells[meetCell(meetOf(p.x), meetOf(p.y), meetOf(p.z))].push_back({ geometry, p, i < field->normal.size() ? field->normal[i] : RE::NiPoint3(0.0f, 0.0f, 1.0f) });
					auto [it, added] = surface.try_emplace(surfaceKey(p.x, p.y), p.z);
					if (!added)
						it->second = std::max(it->second, p.z);
				}
			}
			shells[geometry] = std::move(shell);
		}
		const auto meetsOther = [&](const RE::BSGeometry* a_self, const RE::NiPoint3& a_p) {
			const int64_t cx = meetOf(a_p.x), cy = meetOf(a_p.y), cz = meetOf(a_p.z);
			for (int64_t x = cx - 1; x <= cx + 1; x++) {
				for (int64_t y = cy - 1; y <= cy + 1; y++) {
					for (int64_t z = cz - 1; z <= cz + 1; z++) {
						const auto it = shownCells.find(meetCell(x, y, z));
						if (it == shownCells.end())
							continue;
						for (const ShownTexel& other : it->second) {
							if (other.geometry != a_self && (other.p - a_p).SqrLength() <= MEET_EMIT * MEET_EMIT)
								return true;
						}
					}
				}
			}
			return false;
		};
		// D189: the nearest other sheet's surface normal round a_p (zero: none within MEET_EMIT)
		const auto otherNormal = [&](const RE::BSGeometry* a_self, const RE::NiPoint3& a_p) {
			const int64_t cx = meetOf(a_p.x), cy = meetOf(a_p.y), cz = meetOf(a_p.z);
			float nearest = MEET_EMIT * MEET_EMIT;
			RE::NiPoint3 normal(0.0f, 0.0f, 0.0f);
			for (int64_t x = cx - 1; x <= cx + 1; x++) {
				for (int64_t y = cy - 1; y <= cy + 1; y++) {
					for (int64_t z = cz - 1; z <= cz + 1; z++) {
						const auto it = shownCells.find(meetCell(x, y, z));
						if (it == shownCells.end())
							continue;
						for (const ShownTexel& other : it->second) {
							const float d = (other.p - a_p).SqrLength();
							if (other.geometry != a_self && d <= nearest) {
								nearest = d;
								normal = other.normal;
							}
						}
					}
				}
			}
			return normal;
		};
		const auto deepUnderSheet = [&](const RE::BSGeometry* a_self, const RE::NiPoint3& a_p) {
			for (const auto& [geometry, surface] : shownSurfaces) {
				if (geometry == a_self)
					continue;
				const auto it = surface.find(surfaceKey(a_p.x, a_p.y));
				if (it != surface.end() && it->second > a_p.z + EMIT_DEEP && it->second < a_p.z + OTHER_REACH)
					return true;
			}
			return false;
		};
		struct MeetPoint
		{
			RE::NiPoint3 at, way;
			float speed;
			float level;  // D189: how level its water's surface is there (its normal's up, either side)
			bool ends;    // D189: its sheet ends just downstream (its water lands here), not running on beside the other
		};
		std::vector<MeetPoint> meetPoints;  // D186: where sheets meet, over every mesh
		std::vector<std::array<float, 5>> planes;
		uint32_t meets = 0;
		for (const auto& [geometry, field] : bakeFields) {
			if (!field || !grounded.contains(geometry))
				continue;
			const auto w = static_cast<int32_t>(field->width), h = static_cast<int32_t>(field->height);
			const std::size_t count = static_cast<std::size_t>(w) * h;
			if (field->position.size() != count || field->turn.size() != count || field->normal.size() != count)
				continue;
			// D126: only the shell's texels (VertexInShell, as culled now) give spray, plunges or lips. D132: only as a gate on a texel's
			// own output; the mesh's shape (neighbours, crests, top, foot) is read whole, so a cull's edge is no rim or foot
			const std::vector<uint8_t>& shellCovered = shells[geometry];
			if (shellCovered.size() != count)
				continue;
			// D188: where its water lands in a pool, how hard (for the bake's plunge section: ApplyPlunges)
			auto& meshImpact = o_result.impacts[geometry];
			meshImpact.assign(count, 0.0f);
			const auto index = [&](int32_t a_x, int32_t a_y) { return static_cast<std::size_t>(a_y) * w + a_x; };
			const auto covered = [&](int32_t a_x, int32_t a_y) { return a_x >= 0 && a_y >= 0 && a_x < w && a_y < h && field->covered[index(a_x, a_y)]; };
			float top = -FLT_MAX, bottom = FLT_MAX, lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
			for (std::size_t i = 0; i < count; i++) {
				if (!field->covered[i])
					continue;
				const RE::NiPoint3& p = field->position[i];
				top = std::max(top, p.z);
				bottom = std::min(bottom, p.z);
				lo[0] = std::min(lo[0], p.x);
				lo[1] = std::min(lo[1], p.y);
				hi[0] = std::max(hi[0], p.x);
				hi[1] = std::max(hi[1], p.y);
			}
			if (top < bottom)
				continue;
			// The water this mesh falls into: the pools' surface (ComputePools), else the engine's highest plane under its top (as
			// ClipStep's cut); planes only at its foot, where a plane's bound reaching over the fall cannot pass for its water (D70)
			CollectWaterPlanes(lo[0], lo[1], hi[0], hi[1], planes);
			const std::vector<std::array<float, 5>> allPlanes = planes;  // D182: every surface over the mesh, for lifting spray
			std::erase_if(planes, [&](const auto& a_plane) { return a_plane[4] >= top - 32.0f; });
			const auto waterAt = [&](const RE::NiPoint3& a_p, float& a_level) {
				if (PoolLevelAt(a_p.x, a_p.y, a_level) && a_level < top - 32.0f)
					return true;
				a_level = -FLT_MAX;
				for (const auto& plane : planes) {
					if (a_p.x >= plane[0] && a_p.x <= plane[2] && a_p.y >= plane[1] && a_p.y <= plane[3])
						a_level = std::max(a_level, plane[4]);
				}
				return a_level > -FLT_MAX;
			};
			// The water's way in the world: down the mesh (texture v) turned across it by the sim (D75's arrows)
			const std::vector<float>& turn = field->turn;
			const auto flowAt = [&](int32_t a_x, int32_t a_y) {
				const auto side = [&](int32_t a_dx, int32_t a_dy) {
					const bool f = covered(a_x + a_dx, a_y + a_dy), b = covered(a_x - a_dx, a_y - a_dy);
					const RE::NiPoint3& here = field->position[index(a_x, a_y)];
					if (f && b)
						return (field->position[index(a_x + a_dx, a_y + a_dy)] - field->position[index(a_x - a_dx, a_y - a_dy)]) * 0.5f;
					if (f)
						return field->position[index(a_x + a_dx, a_y + a_dy)] - here;
					if (b)
						return here - field->position[index(a_x - a_dx, a_y - a_dy)];
					return RE::NiPoint3(0.0f, 0.0f, 0.0f);
				};
				RE::NiPoint3 direction = side(0, 1) + side(1, 0) * turn[index(a_x, a_y)];
				const float length = direction.Length();
				return length > 1e-4f ? direction / length : RE::NiPoint3(0.0f, 0.0f, -1.0f);
			};
			const auto speedAt = [&](float a_z) { return std::min(SIM_TERMINAL, std::sqrt(SIM_INFLOW * SIM_INFLOW + SIM_GRAVITY * std::max(top - a_z, 0.0f))); };
			const auto upward = [&](std::size_t a_i) {
				const RE::NiPoint3& n = field->normal[a_i];
				return n.z < 0.0f ? -n : n;
			};
			// D92: the sheet's outer face: the side its water travels toward (top rows to foot, out from the cliff), tipped up; the
			// winding (D91) pointed into the cliff on some meshes. And nothing from the hidden lead-in under the upper water.
			RE::NiPoint3 travel(0.0f, 0.0f, 0.0f);
			{
				RE::NiPoint3 head(0.0f, 0.0f, 0.0f), foot(0.0f, 0.0f, 0.0f);
				float heads = 0.0f, feet = 0.0f;
				const int32_t band = std::max(1, h / 10);
				for (int32_t y = 0; y < h; y++) {
					for (int32_t x = 0; x < w; x++) {
						if (!field->covered[index(x, y)])
							continue;
						if (y < band) {
							head += field->position[index(x, y)];
							heads += 1.0f;
						} else if (y >= h - band) {
							foot += field->position[index(x, y)];
							feet += 1.0f;
						}
					}
				}
				if (heads > 0.0f && feet > 0.0f) {
					travel = foot / feet - head / heads;
					travel.z = 0.0f;
					const float length = travel.Length();
					travel = length > 1e-3f ? travel / length : RE::NiPoint3(0.0f, 0.0f, 0.0f);
				}
				travel.z = 0.3f;
			}
			const auto outward = [&](std::size_t a_i) {
				const RE::NiPoint3& n = field->normal[a_i];
				// D181: toward the open side by the bake's clearances (D104) where they differ: the travel's side pointed into the cliff
				// on overhangs, sending spray into rock or water
				if (field->clearFront.size() == count && field->clearBack.size() == count) {
					const float front = field->clearFront[a_i], back = field->clearBack[a_i];
					if (std::abs(front - back) > 16.0f)
						return front >= back ? n : -n;
				}
				return n.Dot(travel) < 0.0f ? -n : n;
			};
			// D183: the water as drawn: pushed out by its swell (ComputePushes), a little proud, so spray and mist start on the visible
			// surface
			const auto drawnAt = [&](std::size_t a_i) {
				float push = 0.0f;
				if (field->swell.size() == count) {
					const float t = std::clamp((field->swell[a_i] - PUSH_FROM) / (PUSH_FULL - PUSH_FROM), 0.0f, 1.0f);
					push = PUSH_MOST * t * t * (3.0f - 2.0f * t);
				}
				return field->position[a_i] + outward(a_i) * (push + EMIT_PROUD);
			};
			// D184: under a river plane or a pool over it (not another sheet: a meet lies on the other's surface)
			const auto underWater = [&](const RE::NiPoint3& a_p) {
				for (const auto& plane : allPlanes) {
					if (a_p.x >= plane[0] && a_p.x <= plane[2] && a_p.y >= plane[1] && a_p.y <= plane[3] && plane[4] > a_p.z + EMIT_SUBMERGED)
						return true;
				}
				float pool;
				return PoolLevelAt(a_p.x, a_p.y, pool) && pool > a_p.z + EMIT_SUBMERGED;
			};
			// D183, D184: rock ahead along the sim's way (down the texture, turned across) within HEAD_ON_REACH: the water runs into it
			// head-on (splashes only there, not where it slides along a bank)
			const auto headOn = [&](int32_t a_x, int32_t a_y) {
				if (field->rock.empty())
					return false;
				const float across = turn[index(a_x, a_y)];
				const float length = std::sqrt(1.0f + across * across);
				for (int32_t k = 1; k <= HEAD_ON_REACH; k++) {
					const int32_t x = a_x + static_cast<int32_t>(std::lround(across / length * k)), y = a_y + static_cast<int32_t>(std::lround(k / length));
					if (x < 0 || y < 0 || x >= w || y >= h || (x == a_x && y == a_y))
						continue;
					if (field->rock[index(x, y)]) {
						// (and none beside it: a face it runs into, not a bank it runs along)
						for (const int32_t dx : { -2, -1, 1, 2 }) {
							if (a_x + dx >= 0 && a_x + dx < w && field->rock[index(a_x + dx, a_y)])
								return false;
						}
						return true;
					}
				}
				return false;
			};
			std::vector<int32_t> crestRows;
			std::vector<uint8_t> hiddenLead;
			{
				FallStrands::Input input;
				input.width = field->width;
				input.height = field->height;
				input.position = reinterpret_cast<const float*>(field->position.data());
				input.covered = field->covered.data();
				FallStrands::FindCrests(input, crestRows, hiddenLead);
			}
			// D184: crest spray only on meshes that pour out of a body of water (enough crests at a plane's or pool's level, as
			// BuildLipMap's lips, D170), not mid-fall meshes. Along the whole crest, each column from where its shown surface turns from
			// facing the sky to falling (less than LIP_TANGENT up), thrown out the way it falls. A column that runs into rock first throws
			// none.
			const auto fromWater = [&](const RE::NiPoint3& a_p) {
				float level;
				if (PoolLevelAt(a_p.x, a_p.y, level) && std::abs(a_p.z - level) <= LIP_BAND)
					return true;
				for (const auto& plane : allPlanes) {
					if (a_p.x >= plane[0] && a_p.x <= plane[2] && a_p.y >= plane[1] && a_p.y <= plane[3] && std::abs(a_p.z - plane[4]) <= LIP_BAND)
						return true;
				}
				return false;
			};
			uint32_t crests = 0, fromWaterCrests = 0;
			for (int32_t x = 0; x < w && x < static_cast<int32_t>(crestRows.size()); x++) {
				if (crestRows[x] < 0)
					continue;
				crests++;
				fromWaterCrests += fromWater(field->position[index(x, crestRows[x])]) ? 1 : 0;
			}
			o_result.crestColumns += crests;
			const bool source = fromWaterCrests > 0 && fromWaterCrests >= std::max<uint32_t>(SOURCE_LEAST_CRESTS, static_cast<uint32_t>(crests * SOURCE_SHARE));
			for (int32_t x = 0; source && x < w && x < static_cast<int32_t>(crestRows.size()); x++) {
				const int32_t crest = crestRows[x];
				if (crest < 0)
					continue;
				o_result.lipColumns++;
				bool started = false;  // (rock before shown water is lead-in tucked into the bed)
				for (int32_t y = crest; y < std::min(h, crest + 1 + DROP_SEARCH); y++) {
					const std::size_t i = index(x, y);
					const bool rock = !field->rock.empty() && field->rock[i];
					if (!covered(x, y) || (rock && started))
						break;
					if (rock || !shellCovered[i])
						continue;
					started = true;
					// (past the parts the culling hides by the crest blend: the first shown texel turned down)
					if (upward(i).z >= LIP_TANGENT)
						continue;
					const RE::NiPoint3 at = drawnAt(i);
					o_result.dropColumns++;
					if (deepUnderSheet(geometry, at)) {
						o_result.submergedDrops++;
						break;
					}
					offer(at, outward(i), flowAt(x, y), std::max(speedAt(field->position[i].z), CREST_THROW), DROP_STRENGTH, FallEmitterKind::Lip, DROP_SPACING);
					// D184: and weaker ones over the crest's band upstream, switching on and off with the flow. D186: upstream until the
					// water lies almost flat (CREST_FLAT), shown lead-in included, throwing less further from the drop
					for (int32_t k = CREST_BAND_STEP; k <= CREST_BAND_ROWS; k += CREST_BAND_STEP) {
						const int32_t yb = y - k;
						if (yb < 0 || !covered(x, yb))
							break;
						const std::size_t ib = index(x, yb);
						if (upward(ib).z >= CREST_FLAT)
							break;
						if ((!field->rock.empty() && field->rock[ib]) || !shellCovered[ib])
							continue;
						const RE::NiPoint3 band = drawnAt(ib);
						const float strength = CREST_BAND_STRENGTH * (1.0f - 0.4f * static_cast<float>(k) / static_cast<float>(CREST_BAND_ROWS));
						if (!deepUnderSheet(geometry, band))
							offer(band, outward(ib), flowAt(x, yb), std::max(speedAt(field->position[ib].z), CREST_THROW), strength, FallEmitterKind::Lip, DROP_SPACING);
					}
					break;
				}
			}
			for (int32_t y = 0; y < h; y++) {
				for (int32_t x = 0; x < w; x++) {
					const std::size_t i = index(x, y);
					// D184: where this sheet's shown water runs into another's: heavy mist
					if (shellCovered[i] && (field->rock.empty() || !field->rock[i]) && !hiddenLead[i] && meetsOther(geometry, field->position[i])) {
						o_result.footColumns++;
						const RE::NiPoint3 at = drawnAt(i);
						if (!underWater(at) && !deepUnderSheet(geometry, at)) {
							// D186: kept until every mesh is done: mist only where each seam's water lands (its line's lowest), droplets
							// along the rest. D189: it lands where its water pours onto the other's surface, not where both run on side by
							// side. Its way: a few texels down.
							const int32_t below = std::min(y + MEET_WAY_ROWS, h - 1), aboveRow = std::max(y - MEET_WAY_ROWS, 0);
							RE::NiPoint3 way = field->position[index(x, below)] - field->position[index(x, aboveRow)];
							const float wayLength = way.Length();
							const RE::NiPoint3 facing = otherNormal(geometry, field->position[i]);
							const bool ends = wayLength > 1e-3f && facing.SqrLength() > 0.25f && std::abs(way.Dot(facing)) / wayLength / facing.Length() >= MEET_POUR;
							meetPoints.push_back({ at, flowAt(x, y), speedAt(field->position[i].z), field->normal.size() > i ? std::abs(field->normal[i].z) : 1.0f, ends });
						}
					}
					if (!shellCovered[i] || (!field->rock.empty() && field->rock[i]))
						continue;
					const RE::NiPoint3& here = field->position[i];
					float level = -FLT_MAX;
					const bool foot = here.z < bottom + WATER_CUT_BAND;
					// D90: or one of our pools anywhere down the mesh (a cascade's middle pool); the engine's planes stay foot-only (D70)
					float ownPool = -FLT_MAX;
					const bool water = (foot && waterAt(here, level)) || (PoolLevelAt(here.x, here.y, ownPool) && ownPool < top - 32.0f && (level = ownPool, true));
					// D183: where the water strikes rock head-on (the bake's foam), on the drawn surface: splashes and mist (kind Strike)
					if (field->foam[i] >= STRIKE_FOAM && !hiddenLead[i] && headOn(x, y)) {
						const RE::NiPoint3 at = drawnAt(i);
						if (!underWater(at) && !deepUnderSheet(geometry, at))
							offer(at, RE::NiPoint3(0.0f, 0.0f, 1.0f), flowAt(x, y), speedAt(here.z), 0.4f + 0.6f * field->foam[i], FallEmitterKind::Strike, STRIKE_SPACING);
					}
					// Where it goes into the water below: at or under it, coming from above it (texture v runs downstream)
					if (!water || here.z > level + PLUNGE_BAND || !covered(x, y - 1))
						continue;
					const RE::NiPoint3& above = field->position[index(x, y - 1)];
					if (above.z <= level + PLUNGE_BAND)
						continue;
					const float s = std::clamp((above.z - level) / std::max(above.z - here.z, 1e-3f), 0.0f, 1.0f);
					RE::NiPoint3 p = above + (here - above) * s;
					p.z = level;
					const float fallen = top - p.z;
					// D46: or steep water reaching the level (a short mesh's head says little)
					const bool steepIn = upward(i).z < PLUNGE_STEEP;
					if (fallen <= PLUNGE_HEAD && !steepIn)
						continue;
					meets++;
					const float strength = std::clamp(std::max(fallen / (PLUNGE_HEAD * 4.0f), steepIn ? 0.6f : 0.0f), 0.25f, 1.0f);
					meshImpact[i] = std::max(meshImpact[i], strength);
					const RE::NiPoint3 direction = flowAt(x, y);
					const float speed = speedAt(p.z);
					const auto key = (static_cast<int64_t>(std::floor(p.x / PLUNGE_WATER_SPACING)) << 32) ^
					                 static_cast<uint32_t>(static_cast<int32_t>(std::floor(p.y / PLUNGE_WATER_SPACING)));
					auto [it, added] = points.try_emplace(key);
					if (!added && it->second.strength >= strength)
						continue;
					FallPlunge& plunge = it->second;
					plunge.position[0] = p.x;
					plunge.position[1] = p.y;
					plunge.position[2] = p.z;
					// Straight down a curtain the way across is noise: no pull then
					const float horizontal = std::sqrt(direction.x * direction.x + direction.y * direction.y);
					plunge.direction[0] = horizontal > 0.05f ? direction.x / horizontal : 0.0f;
					plunge.direction[1] = horizontal > 0.05f ? direction.y / horizontal : 0.0f;
					plunge.speed = speed;
					plunge.strength = strength;
				}
			}
		}
		// D188: each mesh's pool landings, softened. D189: widened first (a neighbour max: blurred alone, the one-texel waterline
		// faded to a tenth), then carried UP the sheet (carried down the flow, the white ran off past the foot where there is no sheet)
		std::vector<float> scratch;
		for (auto& [geometry, impact] : o_result.impacts) {
			const auto field = bakeFields.find(geometry);
			if (field == bakeFields.end() || !field->second || impact.size() != static_cast<std::size_t>(field->second->width) * field->second->height)
				continue;
			const auto w = static_cast<int32_t>(field->second->width), h = static_cast<int32_t>(field->second->height);
			const std::size_t count = impact.size();
			// D190: only where the sheet falls into the pool: one lying level on it is at its waterline all over
			if (field->second->normal.size() == count) {
				for (std::size_t i = 0; i < count; i++)
					impact[i] *= std::clamp((PLUNGE_LEVEL - std::abs(field->second->normal[i].z)) / PLUNGE_LEVEL_EASE, 0.0f, 1.0f);
			}
			scratch.resize(count);
			for (int pass = 0; pass < PLUNGE_WIDEN; pass++) {
				for (int32_t y = 0; y < h; y++) {
					for (int32_t x = 0; x < w; x++) {
						float most = 0.0f;
						for (int32_t dy = std::max(y - 1, 0); dy <= std::min(y + 1, h - 1); dy++) {
							for (int32_t dx = std::max(x - 1, 0); dx <= std::min(x + 1, w - 1); dx++)
								most = std::max(most, impact[static_cast<std::size_t>(dy) * w + dx]);
						}
						scratch[static_cast<std::size_t>(y) * w + x] = most;
					}
				}
				impact.swap(scratch);
			}
			auto& landing = o_result.landings[geometry];
			landing.assign(count, 0.0f);
			for (int32_t y = h - 1; y >= 0; y--) {
				for (int32_t x = 0; x < w; x++) {
					const std::size_t i = static_cast<std::size_t>(y) * w + x;
					landing[i] = field->second->covered[i] ? std::max(impact[i], y + 1 < h ? landing[i + w] * PLUNGE_CARRY : 0.0f) : 0.0f;
				}
			}
			for (int pass = 0; pass < 2; pass++) {
				for (auto* values : { &impact, &landing }) {
					for (int32_t y = 0; y < h; y++) {
						for (int32_t x = 0; x < w; x++) {
							float total = 0.0f, weight = 0.0f;
							for (int32_t dy = -1; dy <= 1; dy++) {
								for (int32_t dx = -1; dx <= 1; dx++) {
									if (x + dx < 0 || y + dy < 0 || x + dx >= w || y + dy >= h)
										continue;
									const float k = (dx == 0 ? 2.0f : 1.0f) * (dy == 0 ? 2.0f : 1.0f);
									total += (*values)[static_cast<std::size_t>(y + dy) * w + x + dx] * k;
									weight += k;
								}
							}
							scratch[static_cast<std::size_t>(y) * w + x] = total / weight;
						}
					}
					values->swap(scratch);
				}
			}
		}
		// D186: seams as lines (points joined across MEET_LINE_CELL cells); each line's lowest MEET_FOOT_BAND, where its water lands,
		// gets mist and droplets, higher up droplets only (judged point by point, mist hung in columns between the domes)
		{
			const auto lineCell = [](float a_v) { return static_cast<int64_t>(std::floor(a_v / MEET_LINE_CELL)); };
			const auto lineKey = [](int64_t a_x, int64_t a_y, int64_t a_z) { return (a_x & 0x1FFFFF) << 42 | (a_y & 0x1FFFFF) << 21 | (a_z & 0x1FFFFF); };
			std::unordered_map<int64_t, std::vector<uint32_t>> cells;
			for (uint32_t k = 0; k < meetPoints.size(); k++) {
				const RE::NiPoint3& p = meetPoints[k].at;
				cells[lineKey(lineCell(p.x), lineCell(p.y), lineCell(p.z))].push_back(k);
			}
			std::vector<uint32_t> parent(meetPoints.size());
			for (uint32_t k = 0; k < parent.size(); k++)
				parent[k] = k;
			const auto root = [&](uint32_t a_k) {
				while (parent[a_k] != a_k)
					a_k = parent[a_k] = parent[parent[a_k]];
				return a_k;
			};
			for (const auto& [key, members] : cells) {
				const RE::NiPoint3& p = meetPoints[members.front()].at;
				const int64_t cx = lineCell(p.x), cy = lineCell(p.y), cz = lineCell(p.z);
				for (int64_t dx = -1; dx <= 1; dx++) {
					for (int64_t dy = -1; dy <= 1; dy++) {
						for (int64_t dz = -1; dz <= 1; dz++) {
							const auto it = cells.find(lineKey(cx + dx, cy + dy, cz + dz));
							if (it == cells.end())
								continue;
							for (const uint32_t o : it->second)
								parent[root(o)] = root(members.front());
						}
					}
				}
				for (const uint32_t m : members)
					parent[root(m)] = root(members.front());
			}
			// D188: judged round each point (its seam's points within MEET_LOCAL). D189: by its water's own surface there, not the seam's
			// direction (overlapping curtains meet in a patch, not a line). Mist where the water is level enough to land on, or at the foot
			// of a steeper stretch (no meeting point lower nearby); droplets only down steep faces.
			for (uint32_t k = 0; k < meetPoints.size(); k++) {
				const MeetPoint& m = meetPoints[k];
				const int64_t cx = lineCell(m.at.x), cy = lineCell(m.at.y), cz = lineCell(m.at.z);
				bool lower = false;
				for (int64_t dx = -2; dx <= 2 && !lower; dx++) {
					for (int64_t dy = -2; dy <= 2 && !lower; dy++) {
						for (int64_t dz = -2; dz <= 2 && !lower; dz++) {
							const auto it = cells.find(lineKey(cx + dx, cy + dy, cz + dz));
							if (it == cells.end())
								continue;
							for (const uint32_t o2 : it->second) {
								const RE::NiPoint3 d = meetPoints[o2].at - m.at;
								if (d.SqrLength() <= MEET_LOCAL * MEET_LOCAL && d.z < -MEET_FOOT_DROP) {
									lower = true;
									break;
								}
							}
						}
					}
				}
				// D189: and only where its water lands (its sheet ends there): two sheets running on down side by side are no landing
				const bool landing = m.ends && (m.level >= MEET_LEVEL || (!lower && m.level >= MEET_FOOT_LEVEL));
				offer(m.at, RE::NiPoint3(0.0f, 0.0f, 1.0f), m.way, m.speed, MEET_STRENGTH, landing ? FallEmitterKind::Plunge : FallEmitterKind::MeetDrops, MEET_SPACING);
			}
			o_result.footColumns = static_cast<uint32_t>(meetPoints.size());
		}
		// D94: and wherever a mesh crosses a water surface (the river it leaves, a pool partway, the pool below): the water foams
		// and is stirred along the crossing line. Any engine plane over the texels, or our pools.
		uint32_t lips = 0;
		for (const auto& [geometry, field] : bakeFields) {
			if (!field || !grounded.contains(geometry))
				continue;
			const auto w = static_cast<int32_t>(field->width), h = static_cast<int32_t>(field->height);
			const std::size_t count = static_cast<std::size_t>(w) * h;
			if (field->position.size() != count)
				continue;
			// D126: the shell's texels only, as above (D132: as the gate on a crossing's own texel)
			std::vector<uint8_t> shellCovered = field->covered;
			{
				const float uvMin[2] = { field->uvMin[0], field->uvMin[1] }, uvSize[2] = { field->uvSize[0], field->uvSize[1] };
				const auto shell = ShellTexels(const_cast<RE::BSGeometry*>(geometry), field->width, field->height, uvMin, uvSize, nullptr);
				for (std::size_t i = 0; i < shell.size() && i < shellCovered.size(); i++)
					shellCovered[i] = shellCovered[i] && shell[i] ? 1 : 0;
			}
			float lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX }, meshTop = -FLT_MAX;
			for (std::size_t i = 0; i < count; i++) {
				if (!field->covered[i])
					continue;
				meshTop = std::max(meshTop, field->position[i].z);
				lo[0] = std::min(lo[0], field->position[i].x);
				lo[1] = std::min(lo[1], field->position[i].y);
				hi[0] = std::max(hi[0], field->position[i].x);
				hi[1] = std::max(hi[1], field->position[i].y);
			}
			std::vector<std::array<float, 5>> crossPlanes;
			CollectWaterPlanes(lo[0], lo[1], hi[0], hi[1], crossPlanes);
			const auto water = [&](std::size_t a_i) { return field->covered[a_i] && (field->rock.empty() || !field->rock[a_i]); };
			for (int32_t y = 0; y < h; y++) {
				for (int32_t x = 0; x < w; x++) {
					const std::size_t i = static_cast<std::size_t>(y) * w + x;
					if (!water(i) || !shellCovered[i])
						continue;
					const RE::NiPoint3& a = field->position[i];
					// The levels over this texel
					float levels[8];
					int levelCount = 0;
					float pool = 0.0f;
					if (PoolLevelAt(a.x, a.y, pool))
						levels[levelCount++] = pool;
					for (const auto& plane : crossPlanes) {
						if (levelCount < 8 && a.x >= plane[0] && a.x <= plane[2] && a.y >= plane[1] && a.y <= plane[3])
							levels[levelCount++] = plane[4];
					}
					// D141: not the water the fall pours out of (a level near the mesh's top): a river is glassy over its lip; the sheet
					// fades in instead
					levelCount = static_cast<int>(std::remove_if(levels, levels + levelCount, [&](float a_level) { return a_level >= meshTop - SOURCE_CROSSING_BAND; }) - levels);
					for (const auto [dx, dy] : { std::pair{ 1, 0 }, std::pair{ 0, 1 } }) {
						if (x + dx >= w || y + dy >= h)
							continue;
						const std::size_t j = static_cast<std::size_t>(y + dy) * w + x + dx;
						if (!water(j))
							continue;
						const RE::NiPoint3& b = field->position[j];
						for (int l = 0; l < levelCount; l++) {
							const float level = levels[l];
							if ((a.z - level) * (b.z - level) > 0.0f || std::abs(a.z - b.z) < 1e-3f)
								continue;
							const float s = std::clamp((level - a.z) / (b.z - a.z), 0.0f, 1.0f);
							RE::NiPoint3 p = a + (b - a) * s;
							// D100: set back a little toward the submerged texel, so the foam starts before the seam, not on it
							{
								RE::NiPoint3 toWater = (a.z < level ? a : b) - p;
								toWater.z = 0.0f;
								const float length = toWater.Length();
								if (length > 1e-3f)
									p += toWater * (CROSSING_BACK / length);
							}
							// The water's way there: down the mesh, level
							const std::size_t below = static_cast<std::size_t>(std::min(y + 1, h - 1)) * w + x, aboveRow = static_cast<std::size_t>(std::max(y - 1, 0)) * w + x;
							RE::NiPoint3 down = field->position[below] - field->position[aboveRow];
							down.z = 0.0f;
							const float horizontal = down.Length();
							const auto key = (static_cast<int64_t>(std::floor(p.x / PLUNGE_WATER_SPACING)) << 32) ^
							                 static_cast<uint32_t>(static_cast<int32_t>(std::floor(p.y / PLUNGE_WATER_SPACING))) ^ (int64_t{ 1 } << 62);
							auto [it, added] = points.try_emplace(key);
							if (!added)
								continue;
							FallPlunge& crossing = it->second;
							crossing.position[0] = p.x;
							crossing.position[1] = p.y;
							crossing.position[2] = level;
							crossing.direction[0] = horizontal > 1e-3f ? down.x / horizontal : 0.0f;
							crossing.direction[1] = horizontal > 1e-3f ? down.y / horizontal : 0.0f;
							crossing.speed = SIM_INFLOW;
							crossing.strength = CROSSING_STRENGTH;
							lips++;
						}
					}
				}
			}
		}
		if (false) {
			for (const auto& [geometry, field] : bakeFields) {
				if (!field || !grounded.contains(geometry))
					continue;
				const auto w = static_cast<int32_t>(field->width);
				FallStrands::Input input;
				input.width = field->width;
				input.height = field->height;
				input.position = reinterpret_cast<const float*>(field->position.data());
				input.covered = field->covered.data();
				std::vector<int32_t> crest;
				std::vector<uint8_t> leadIn;
				FallStrands::FindCrests(input, crest, leadIn);
				for (int32_t x = 0; x < w; x++) {
					const int32_t y = crest[x];
					if (y < 0 || y + 2 >= static_cast<int32_t>(field->height))
						continue;
					const std::size_t i = static_cast<std::size_t>(y) * w + x;
					if (!field->rock.empty() && field->rock[i])
						continue;
					const RE::NiPoint3& p = field->position[i];
					// D91: the pool it comes from, ours or the engine's (the lip lies at a river plane's surface, so D70's overreach is no
					// risk here)
					float level = 0.0f;
					bool upper = PoolLevelAt(p.x, p.y, level) && std::abs(p.z - level) <= LIP_BAND;
					if (!upper) {
						std::vector<std::array<float, 5>> lipPlanes;
						CollectWaterPlanes(p.x - 1.0f, p.y - 1.0f, p.x + 1.0f, p.y + 1.0f, lipPlanes);
						for (const auto& plane : lipPlanes) {
							if (p.x >= plane[0] && p.x <= plane[2] && p.y >= plane[1] && p.y <= plane[3] && std::abs(p.z - plane[4]) <= LIP_BAND) {
								level = plane[4];
								upper = true;
								break;
							}
						}
					}
					if (!upper)
						continue;
					const RE::NiPoint3 down = field->position[i + 2 * static_cast<std::size_t>(w)] - p;
					const float horizontal = std::sqrt(down.x * down.x + down.y * down.y);
					if (horizontal < 1e-3f)
						continue;
					const auto key = (static_cast<int64_t>(std::floor(p.x / PLUNGE_WATER_SPACING)) << 32) ^ static_cast<uint32_t>(static_cast<int32_t>(std::floor(p.y / PLUNGE_WATER_SPACING))) ^
					                 (int64_t{ 1 } << 62);
					auto [it, added] = points.try_emplace(key);
					if (!added)
						continue;
					FallPlunge& lip = it->second;
					// D92: upstream of the drop (at it, its foam was carried straight over the edge and never showed)
					lip.position[0] = p.x - down.x / horizontal * LIP_UPSTREAM;
					lip.position[1] = p.y - down.y / horizontal * LIP_UPSTREAM;
					lip.position[2] = level;
					lip.direction[0] = down.x / horizontal;
					lip.direction[1] = down.y / horizontal;
					lip.speed = SIM_INFLOW;
					lip.strength = LIP_STRENGTH;
					lips++;
				}
			}
		}
		o_result.plungePoints.clear();
		o_result.plungePoints.reserve(points.size());
		for (const auto& [key, plunge] : points)
			o_result.plungePoints.push_back(plunge);
		o_result.sprayEmitters.clear();
		o_result.sprayEmitters.reserve(chosen.size());
		for (const auto& [key, emitter] : chosen) {
			o_result.sprayEmitters.push_back(emitter);
			o_result.kinds[std::clamp(static_cast<int>(emitter.kind), 0, 3)]++;
		}
		o_result.meets = meets;
		o_result.lips = lips;
	}

	void FallSites::Summarise()
	{
		// One line per cell (by each site's centroid), logged when it changes
		struct Counts
		{
			uint32_t sites = 0, falls = 0, cascades = 0, rapids = 0, noBottom = 0, failed = 0;
			std::string failures;
		};
		std::unordered_map<uint64_t, Counts> perCell;
		std::unordered_map<uint64_t, std::pair<int32_t, int32_t>> coordinates;
		for (const auto& site : sites) {
			const auto cx = static_cast<int32_t>(std::floor(site.centroid[0] / 4096.0f));
			const auto cy = static_cast<int32_t>(std::floor(site.centroid[1] / 4096.0f));
			const uint64_t key = MakeCellKey(cx, cy);
			coordinates[key] = { cx, cy };
			auto& counts = perCell[key];
			counts.sites++;
			switch (site.kind) {
			case Kind::Fall:
				counts.falls++;
				break;
			case Kind::Cascade:
				counts.cascades++;
				break;
			case Kind::Rapids:
				counts.rapids++;
				break;
			case Kind::NoBottom:
				counts.noBottom++;
				break;
			default:
				counts.failed++;
				counts.failures += std::format(" #{}({})", site.id, site.failure ? site.failure : "?");
				break;
			}
		}
		for (const auto& [key, counts] : perCell) {
			const auto [cx, cy] = coordinates[key];
			std::string line = std::format("[Fluid Field] Waterfall sites in cell ({}, {}): {} sites, {} falls, {} cascades, {} rapids, {} no bottom, {} failed{}",
				cx, cy, counts.sites, counts.falls, counts.cascades, counts.rapids, counts.noBottom, counts.failed, counts.failures);
			auto& last = cellSummaries[key];
			if (last == line)
				continue;
			last = line;
			logger::info("{}", line);
			// Each site in the cell, for checking the stages against the world
			for (const auto& site : sites) {
				if (MakeCellKey(static_cast<int32_t>(std::floor(site.centroid[0] / 4096.0f)), static_cast<int32_t>(std::floor(site.centroid[1] / 4096.0f))) != key)
					continue;
				std::string levels;
				for (const float level : site.levels)
					levels += std::format(" {:.0f}", level);
				logger::info("[Fluid Field]   site #{} {}: {} refs (fall {}, skirt {}, mist {}, splash {}, rapids {}), x {:.0f}..{:.0f}, y {:.0f}..{:.0f}, z {:.0f}..{:.0f}, levels{}{}",
					site.id, KindName(site.kind), site.members.size(), site.classCounts[0], site.classCounts[1], site.classCounts[2], site.classCounts[3], site.classCounts[4],
					site.min[0], site.max[0], site.min[1], site.max[1], site.min[2], site.max[2], levels, site.topFromMesh ? " (top from mesh)" : "");
				logger::info("[Fluid Field]     falling water x {:.0f}..{:.0f}, y {:.0f}..{:.0f}, z {:.0f}..{:.0f}; planes near:{}", site.fallMin[0], site.fallMax[0], site.fallMin[1],
					site.fallMax[1], site.fallMin[2], site.fallMax[2], site.candidates);
			}
			if (consoleSummary) {
				if (auto* console = RE::ConsoleLog::GetSingleton())
					console->Print("%s", line.c_str());
			}
		}
	}

	void FallSites::DrawMeshDebug(ImDrawList* a_drawList, float a_range, int a_mode) const
	{
		auto* camera = PlayerCamera();
		if (!camera || !a_drawList || a_mode <= 0)
			return;
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		const RE::NiPoint3 eye = camera->world.translate;
		const auto project = [&](const RE::NiPoint3& a_p, ImVec2& a_screen) {
			float sx, sy, sz;
			if (!camera->WorldPtToScreenPt3(a_p, sx, sy, sz, 1e-5f) || sz <= 0.0f)
				return false;
			a_screen = ImVec2(sx * display.x, (1.0f - sy) * display.y);
			return true;
		};
		const auto nearCamera = [&](const RE::NiBound& a_bound) { return (a_bound.center - eye).Length() < a_range + a_bound.radius; };
		// D108: the depth rays as cast: green from the face through the vertex to what they hit, red to their reach for a miss
		if (a_mode & 4) {
			for (const auto& lit : litMeshList) {
				auto* geometry = lit.geometry.get();
				const auto it = geometry ? depthRaysShown.find(geometry) : depthRaysShown.end();
				if (it == depthRaysShown.end() || !nearCamera(geometry->worldBound))
					continue;
				for (const auto& ray : it->second) {
					ImVec2 a, b;
					if (!project(ray[0], a) || !project(ray[1], b))
						continue;
					const bool hit = ray[2].x > 0.5f;
					a_drawList->AddLine(a, b, hit ? IM_COL32(60, 230, 90, 170) : IM_COL32(240, 60, 60, 140), 1.0f);
					if (hit)
						a_drawList->AddCircleFilled(b, 2.0f, IM_COL32(60, 255, 90, 230));
				}
			}
		}
		// Wireframe: every grounded lit mesh's triangles, from its CPU copy
		if (a_mode & 1) {
			for (const auto& lit : litMeshList) {
				auto* geometry = lit.geometry.get();
				auto* triShape = geometry ? geometry->AsTriShape() : nullptr;
				auto* renderer = geometry ? geometry->GetGeometryRuntimeData().rendererData : nullptr;
				if (!triShape || !renderer || !renderer->rawVertexData || !renderer->rawIndexData || !nearCamera(geometry->worldBound))
					continue;
				const auto& counts = triShape->GetTrishapeRuntimeData();
				const uint32_t stride = renderer->vertexDesc.GetSize();
				const uint32_t positionOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
				std::vector<ImVec2> screen(counts.vertexCount);
				std::vector<uint8_t> shown(counts.vertexCount, 0);
				for (uint32_t v = 0; v < counts.vertexCount; v++) {
					float local[3];
					std::memcpy(local, renderer->rawVertexData + static_cast<std::size_t>(v) * stride + positionOffset, sizeof(local));
					shown[v] = project(geometry->world * RE::NiPoint3(local[0], local[1], local[2]), screen[v]) ? 1 : 0;
				}
				const ImU32 color = denseShapes.contains(geometry) ? IM_COL32(255, 220, 60, 150) : IM_COL32(255, 120, 40, 170);
				for (uint32_t i = 0; i < counts.triangleCount; i++) {
					const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(i) * 3;
					if (index[0] >= counts.vertexCount || index[1] >= counts.vertexCount || index[2] >= counts.vertexCount)
						continue;
					for (int k = 0; k < 3; k++) {
						const uint16_t a = index[k], b = index[(k + 1) % 3];
						if (a < b && shown[a] && shown[b])
							a_drawList->AddLine(screen[a], screen[b], color, 1.0f);
					}
				}
			}
		}
		// Flow: the sim's way and speed, an arrow every few texels; not in rock, under the water below, on a sheet's far side, or
		// behind rock from the camera (a few hundred rays a frame, cached until the camera moves)
		if (a_mode & 2) {
			static constexpr int32_t STRIDE = 5;
			if ((eye - arrowEye).Length() > 48.0f) {
				arrowSeen.clear();
				arrowEye = eye;
			}
			auto* tes = RE::TES::GetSingleton();
			auto* cell = tes ? (tes->interiorCell ? tes->interiorCell : tes->GetCell(eye)) : nullptr;
			auto* bhkWorld = cell ? cell->GetbhkWorld() : nullptr;
			auto* world = bhkWorld ? bhkWorld->GetWorld1() : nullptr;
			int rays = 0;
			for (const auto& [geometry, field] : bakeFields) {
				if (!field || !geometry || !nearCamera(geometry->worldBound))
					continue;
				const auto w = static_cast<int32_t>(field->width), h = static_cast<int32_t>(field->height);
				const auto at = [&](int32_t a_x, int32_t a_y) { return static_cast<std::size_t>(std::clamp(a_y, 0, h - 1)) * w + std::clamp(a_x, 0, w - 1); };
				for (int32_t y = STRIDE / 2; y < h; y += STRIDE) {
					for (int32_t x = STRIDE / 2; x < w; x += STRIDE) {
						const std::size_t i = at(x, y);
						if (!field->covered[i] || (!field->rock.empty() && field->rock[i]))
							continue;
						const RE::NiPoint3& here = field->position[i];
						float pool;
						if (PoolLevelAt(here.x, here.y, pool) && here.z < pool - 4.0f)
							continue;
						const uint64_t key = (reinterpret_cast<uint64_t>(geometry) << 20) ^ static_cast<uint64_t>(i);
						auto seen = arrowSeen.find(key);
						if (seen == arrowSeen.end()) {
							if (!world || rays >= ARROW_RAYS)
								continue;
							rays++;
							const RE::NiPoint3 toward = here - eye;
							const float distance = toward.Length();
							bool visible = true;
							if (distance > 16.0f) {
								const RE::BSReadLockGuard lock(bhkWorld->worldLock);
								const float hit = RockAlong(world, eye, toward / distance, distance - 12.0f);
								visible = hit < 0.0f;
							}
							seen = arrowSeen.emplace(key, visible ? 1 : 0).first;
						}
						if (!seen->second)
							continue;
						const RE::NiPoint3 tu = field->position[at(x + 1, y)] - field->position[at(x - 1, y)];
						const RE::NiPoint3 tv = field->position[at(x, y + 1)] - field->position[at(x, y - 1)];
						RE::NiPoint3 direction = tv + tu * field->turn[i];
						const float length = direction.Length();
						if (length < 1e-3f)
							continue;
						direction = direction / length;
						const float speed = std::clamp(field->speed[i], 0.0f, 4.0f);
						const float fast = std::clamp((speed - 0.5f) / 2.5f, 0.0f, 1.0f);
						const ImU32 color = field->foam[i] > 0.4f ? IM_COL32(255, 255, 255, 230) :
						                                            IM_COL32(static_cast<int>(40 + 215 * fast), static_cast<int>(120 * (1.0f - std::abs(fast - 0.5f) * 2.0f) + 60), static_cast<int>(255 * (1.0f - fast)), 230);
						const RE::NiPoint3 from = field->position[i] + field->normal[i] * 4.0f;
						const RE::NiPoint3 to = from + direction * (12.0f + 14.0f * speed);
						ImVec2 a, b;
						if (!project(from, a) || !project(to, b))
							continue;
						a_drawList->AddLine(a, b, color, 1.6f);
						const ImVec2 d(b.x - a.x, b.y - a.y);
						const float l = std::sqrt(d.x * d.x + d.y * d.y);
						if (l > 4.0f) {
							const ImVec2 u(d.x / l, d.y / l);
							const float head = std::min(5.0f, l * 0.4f);
							a_drawList->AddTriangleFilled(b, ImVec2(b.x - u.x * head - u.y * head * 0.5f, b.y - u.y * head + u.x * head * 0.5f),
								ImVec2(b.x - u.x * head + u.y * head * 0.5f, b.y - u.y * head - u.x * head * 0.5f), color);
						}
					}
				}
			}
		}
	}

	void FallSites::DrawDebug(ImDrawList* a_drawList, float a_range, bool a_sites, int a_view) const
	{
		auto* camera = PlayerCamera();
		if (!camera || !a_drawList)
			return;
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		const RE::NiPoint3 eye = camera->world.translate;

		const auto project = [&](float a_x, float a_y, float a_z, ImVec2& a_screen) {
			float sx, sy, sz;
			if (!camera->WorldPtToScreenPt3(RE::NiPoint3(a_x, a_y, a_z), sx, sy, sz, 1e-5f) || sz <= 0.0f)
				return false;
			a_screen = ImVec2(sx * display.x, (1.0f - sy) * display.y);
			return true;
		};
		// Lines are split into pieces so the parts in front of the camera still draw
		const auto line = [&](const float a[3], const float b[3], ImU32 a_color, float a_thickness) {
			constexpr int PIECES = 8;
			for (int i = 0; i < PIECES; i++) {
				const float t0 = static_cast<float>(i) / PIECES;
				const float t1 = static_cast<float>(i + 1) / PIECES;
				ImVec2 pa, pb;
				if (project(a[0] + (b[0] - a[0]) * t0, a[1] + (b[1] - a[1]) * t0, a[2] + (b[2] - a[2]) * t0, pa) &&
					project(a[0] + (b[0] - a[0]) * t1, a[1] + (b[1] - a[1]) * t1, a[2] + (b[2] - a[2]) * t1, pb))
					a_drawList->AddLine(pa, pb, a_color, a_thickness);
			}
		};
		const auto box = [&](const float a_min[3], const float a_max[3], ImU32 a_color, float a_thickness) {
			float c[8][3];
			for (int i = 0; i < 8; i++) {
				c[i][0] = (i & 1) ? a_max[0] : a_min[0];
				c[i][1] = (i & 2) ? a_max[1] : a_min[1];
				c[i][2] = (i & 4) ? a_max[2] : a_min[2];
			}
			static constexpr int EDGES[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
			for (const auto& e : EDGES)
				line(c[e[0]], c[e[1]], a_color, a_thickness);
		};
		const auto rectangle = [&](float a_minX, float a_minY, float a_maxX, float a_maxY, float a_z, ImU32 a_outline) {
			const float p[4][3] = { { a_minX, a_minY, a_z }, { a_maxX, a_minY, a_z }, { a_maxX, a_maxY, a_z }, { a_minX, a_maxY, a_z } };
			for (int i = 0; i < 4; i++)
				line(p[i], p[(i + 1) % 4], a_outline, 3.0f);
		};

		static constexpr std::array<ImU32, static_cast<int>(FxClass::Count)> CLASS_COLORS = {
			IM_COL32(80, 220, 255, 255),   // Body: cyan
			IM_COL32(60, 120, 255, 255),   // Skirt: blue
			IM_COL32(230, 230, 230, 255),  // Mist: white
			IM_COL32(255, 230, 60, 255),   // Splash: yellow
			IM_COL32(255, 150, 40, 255)    // Rapids: orange
		};
		const auto kindColor = [](Kind a_kind) -> ImU32 {
			switch (a_kind) {
			case Kind::Fall:
				return IM_COL32(80, 255, 120, 255);
			case Kind::Cascade:
				return IM_COL32(80, 255, 220, 255);
			case Kind::Rapids:
				return IM_COL32(255, 160, 60, 255);
			case Kind::NoBottom:
				return IM_COL32(255, 80, 255, 255);
			default:
				return IM_COL32(255, 60, 60, 255);
			}
		};

		// D36, D78: the spray emitters by kind, and the plunges' pushes on the water below (a line the way they pull it)
		if (a_view > 0) {
			const auto inView = [&](const float p[3]) {
				const float dx = p[0] - eye.x, dy = p[1] - eye.y, dz = p[2] - eye.z;
				return dx * dx + dy * dy + dz * dz < FALL_VIEW_RANGE * FALL_VIEW_RANGE;
			};
			static constexpr ImU32 KIND_COLORS[3] = { IM_COL32(60, 230, 255, 255), IM_COL32(60, 110, 255, 255), IM_COL32(255, 150, 40, 255) };
			for (const auto& emitter : sprayEmitters) {
				ImVec2 s;
				if (inView(emitter.position) && project(emitter.position[0], emitter.position[1], emitter.position[2], s))
					a_drawList->AddCircleFilled(s, 2.5f, KIND_COLORS[std::clamp(static_cast<int>(emitter.kind), 0, 2)]);
			}
			for (const auto& plunge : plungePoints) {
				if (!inView(plunge.position))
					continue;
				const float to[3] = { plunge.position[0] + plunge.direction[0] * PLUNGE_PUSH_OFFSET, plunge.position[1] + plunge.direction[1] * PLUNGE_PUSH_OFFSET, plunge.position[2] };
				ImVec2 a, b;
				if (project(plunge.position[0], plunge.position[1], plunge.position[2], a) && project(to[0], to[1], to[2], b))
					a_drawList->AddLine(a, b, IM_COL32(60, 255, 90, 255), 1.5f + 1.5f * plunge.strength);
			}
		}
		if (!a_sites)
			return;

		for (const auto& site : sites) {
			const float dx = site.centroid[0] - eye.x;
			const float dy = site.centroid[1] - eye.y;
			if (dx * dx + dy * dy > a_range * a_range)
				continue;
			for (const uint32_t member : site.members) {
				const auto& ref = refs[member];
				// Falling water strong; skirts, mist, splashes and rapids faint
				const bool strong = ref.cls == FxClass::Body;
				const ImU32 color = (CLASS_COLORS[static_cast<int>(ref.cls)] & 0x00FFFFFFu) | (strong ? 0xFF000000u : 0x60000000u);
				box(ref.min, ref.max, color, strong ? 2.0f : 1.0f);
			}
			// Levels: top green, bottom blue, tiers teal; each outlined where its water is (the planes at that height, trimmed to the
			// fall's area)
			for (std::size_t i = 0; i < site.levels.size(); i++) {
				const bool isTop = i == 0;
				const bool isBottom = i + 1 == site.levels.size();
				const ImU32 outline = isTop ? IM_COL32(80, 255, 120, 230) : isBottom ? IM_COL32(80, 140, 255, 230) :
				                                                                       IM_COL32(80, 230, 210, 230);
				bool drawn = false;
				for (const auto& plane : site.planes) {
					if (std::abs(plane.height - site.levels[i]) > LEVEL_TOLERANCE)
						continue;
					rectangle(plane.min[0], plane.min[1], plane.max[0], plane.max[1], plane.height, outline);
					drawn = true;
				}
				// A level from the meshes (no plane): a small mark at the falling water's top
				if (!drawn) {
					rectangle(site.centroid[0] - 64.0f, site.centroid[1] - 64.0f, site.centroid[0] + 64.0f, site.centroid[1] + 64.0f, site.levels[i], outline);
				}
			}
			const FallCapture* capture = GetCapture(site);
			ImVec2 label;
			if (project(site.centroid[0], site.centroid[1], site.top + 48.0f, label)) {
				label.x = std::clamp(label.x, 8.0f, display.x - 420.0f);
				label.y = std::clamp(label.y, 8.0f, display.y - 24.0f);
				std::string text = std::format("#{} {}  drop {:.0f}  levels {}{}  refs {}", site.id, KindName(site.kind), site.top - site.bottom,
					site.levels.size(), site.topFromMesh ? " (top from mesh)" : "", site.members.size());
				if (site.kind == Kind::Failed && site.failure)
					text += std::format("  [{}]", site.failure);
				if (capture)
					text += capture->IsComplete() ? std::string("  grounded") : std::format("  capturing {:.0f}%", capture->GetProgress() * 100.0f);
				a_drawList->AddText(ImVec2(label.x + 1, label.y + 1), IM_COL32(0, 0, 0, 220), text.c_str());
				a_drawList->AddText(label, kindColor(site.kind), text.c_str());
			}
		}
	}
}
