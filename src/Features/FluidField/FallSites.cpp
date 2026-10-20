#include "FallSites.h"

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
#include <map>
#include <numeric>
#include <tuple>
#include <unordered_set>

namespace FluidFieldSim
{
	namespace
	{
		/** @brief D67: the squared distance from a point to a triangle (Ericson, Real-Time Collision Detection 5.1.5). */
		float TriangleDistanceSquared(const RE::NiPoint3& a_p, const RE::NiPoint3& a_a, const RE::NiPoint3& a_b, const RE::NiPoint3& a_c)
		{
			const RE::NiPoint3 ab = a_b - a_a, ac = a_c - a_a, ap = a_p - a_a;
			const auto squared = [](const RE::NiPoint3& a_v) { return a_v.Dot(a_v); };
			const float d1 = ab.Dot(ap), d2 = ac.Dot(ap);
			if (d1 <= 0.0f && d2 <= 0.0f)
				return squared(ap);
			const RE::NiPoint3 bp = a_p - a_b;
			const float d3 = ab.Dot(bp), d4 = ac.Dot(bp);
			if (d3 >= 0.0f && d4 <= d3)
				return squared(bp);
			const float vc = d1 * d4 - d3 * d2;
			if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
				return squared(ap - ab * (d1 / (d1 - d3)));
			const RE::NiPoint3 cp = a_p - a_c;
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
		 * @brief World bounds of a reference from its triangle geometry's vertices. Effect meshes carry
		 * huge bounds for their particles (a skirt's is thousands of units tall), so their bound box is no
		 * use; references without triangles get a small box around their position.
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
					wanted.emplace(root, RE::NiPointer<RE::NiAVObject>(root));
					continue;
				}
				// The falling-water models carry layers of their own (and spray particles): show only the
				// layers in the mask
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
			if (!wanted.contains(object) && pointer)
				pointer->SetAppCulled(false);
		}
		for (auto& [object, pointer] : wanted)
			pointer->SetAppCulled(true);
		hiddenEffects.swap(wanted);
	}

	void FallSites::Clear()
	{
		hideEffects = false;
		RefreshHiddenEffects();
		RemoveDense();
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
		// Captures stay, and their pools: turning waterfalls back on finds the same sites, and their rock, at once
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
		// The falling water seen from above: the highest of its triangles over each point
		// D53: the lowest and highest of the falls over each cell, curtains too: along every edge, by the nearest cell
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

	void FallSites::ComputePools(const Site& a_site, const FallCapture& a_capture)
	{
		// D78: the pools' water over a site's capture, worked out once the capture is complete (it was the old top-down
		// flow solve's cover, D36, worked out as that solve started): the shader's pool cut, the debug arrows and the
		// plunges read it
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
		// D40, D45: the pools' water. The site's paired planes miss pools (a tier between two falls is in
		// none), and planes are whole cells, far wider than their water, so the engine's own water height
		// is sampled on every cell. A pool's water is where the rock lies under it and either the falls'
		// own surface lies at its level or the rock lies shallow under it (a river or pool bed, not the face
		// of a fall)
		pools.cover.assign(cells, NO_SURFACE);
		std::vector<float> levels(cells, NO_SURFACE);
		std::vector<std::size_t> open;
		// D53: the lowest and highest of the falls round each cell (POOL_LIP_REACH): where they drop
		// away below a pool's level with none coming in from above it, the cell is the pool's lip or
		// the air under a curtain falling from it, which the top-down rock calls deep water
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
				const bool bottomPool = level <= a_site.bottom + POOL_BAND;  // D50: the pool the falls end in: nothing lower to mistake it for
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
		// D52: from there through the basin: cells at the same level with the rock under it, where no
		// falls run below the surface (a falls' face under a pool's plane stops the flood), so a deep
		// middle tier's water is found too
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
		sitePools[a_site.captureKey] = std::move(pools);
	}

	void FallSites::PollPools()
	{
		bool changed = false;
		for (const auto& site : sites) {
			if (!site.captureKey || sitePools.contains(site.captureKey))
				continue;
			const FallCapture* capture = GetCapture(site);
			if (!capture || !capture->IsComplete())
				continue;
			ComputePools(site, *capture);
			changed = true;
		}
		if (!changed)
			return;
		groundVersion++;
		// D78: the plunges stand on the pools' water
		if (!bakeFields.empty())
			BuildPlunges();
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
			// D56: pushed downstream just past the line the falls are cut at, so the push never lowers the water there
			const float x = plunge->position[0] + plunge->direction[0] * PLUNGE_PUSH_OFFSET;
			const float y = plunge->position[1] + plunge->direction[1] * PLUNGE_PUSH_OFFSET;
			a_emitters.push_back({ .position = { x / a_cellSize - static_cast<float>(a_gridMin[0]), y / a_cellSize - static_cast<float>(a_gridMin[1]) },
				.radius = std::max(0.75f * PLUNGE_WATER_SPACING / a_cellSize, 1.5f),
				.depthRate = 0.0f,  // D55: no dent in the surface (it broke the foam along the falls' cut); the flow and foam only
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
		// D26: the falling-water meshes of every site with captured ground are grounded on it
		grounded.clear();
		std::unordered_set<const RE::BSGeometry*> litMeshes;
		litMeshList.clear();
		std::vector<RE::NiPointer<RE::BSGeometry>> toDensify;
		std::unordered_set<const RE::BSGeometry*> denseSeen;
		for (const auto& site : sites) {
			const FallCapture* capture = GetCapture(site);
			if (!capture || !capture->IsComplete())
				continue;
			for (const uint32_t member : site.members) {
				if (refs[member].cls != FxClass::Body)
					continue;
				auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refs[member].formID);
				auto* root = ref ? ref->Get3D() : nullptr;
				if (!root)
					continue;
				RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
					// D73: an original drawn by its dense copy is passed by (the copy is found as its sibling)
					if (denseOf.contains(a_geometry)) {
						denseSeen.insert(a_geometry);
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					if (DENSE && !denseShapes.contains(a_geometry) && a_geometry->AsTriShape() && a_geometry->AsTriShape()->GetTrishapeRuntimeData().triangleCount > 0 &&
						netimmerse_cast<RE::BSLightingShaderProperty*>(a_geometry->GetGeometryRuntimeData().shaderProperty.get()))
						toDensify.emplace_back(a_geometry);
					// Only the water's own surfaces: the models also carry particle spray (no triangles of
					// their own), which grounding and the streamlines would smear
					auto* triShape = a_geometry->AsTriShape();
					if (triShape && triShape->GetTrishapeRuntimeData().triangleCount > 0)
						grounded.insert(a_geometry);
					// D56: the lit surface's vertices are tested against the rock, once
					if (triShape && triShape->GetTrishapeRuntimeData().triangleCount > 0 && netimmerse_cast<RE::BSLightingShaderProperty*>(a_geometry->GetGeometryRuntimeData().shaderProperty.get())) {
						litMeshes.insert(a_geometry);
						litMeshList.push_back({ RE::NiPointer<RE::BSGeometry>(a_geometry) });
						if (!vertexClipBase.contains(a_geometry) &&
							std::none_of(clipQueue.begin(), clipQueue.end(), [&](const RE::NiPointer<RE::BSGeometry>& a_queued) { return a_queued.get() == a_geometry; }))
							clipQueue.emplace_back(a_geometry);
						if (!bakeFields.contains(a_geometry) && (!bakeJob || bakeJob->geometry.get() != a_geometry) &&
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
						// The lit surface's textures and the range of its texture coordinates
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
		}
		// D73: copies whose original went away are dropped; new lit meshes get theirs, and the grounded set is built
		// again with the copies in their place
		for (auto it = denseOf.begin(); it != denseOf.end();) {
			if (denseSeen.contains(it->first)) {
				++it;
				continue;
			}
			if (it->second.parent && it->second.shape)
				it->second.parent->DetachChild(it->second.shape.get());
			denseShapes.erase(it->second.shape.get());
			it = denseOf.erase(it);
		}
		if (!toDensify.empty() && !refreshing) {
			uint32_t built = 0;
			for (const auto& geometry : toDensify)
				built += Densify(geometry.get()) ? 1 : 0;
			logger::info("[Fluid Field] Waterfall dense copies: {} of {} lit meshes", built, toDensify.size());
			if (built > 0) {
				refreshing = true;
				RefreshGrounded();
				refreshing = false;
				RefreshHiddenEffects();
				return;
			}
		}
		// Meshes no longer grounded (their cell unloaded) lose their cut, so a reused address is not mistaken for them
		std::erase_if(vertexClipBase, [&](const auto& a_entry) { return !litMeshes.contains(a_entry.first); });
		if (std::erase_if(flowBakes, [&](const auto& a_entry) { return !litMeshes.contains(a_entry.first); }))
			flowBakeVersion++;
		// D78: the plunges and spray of meshes that went go with them
		if (std::erase_if(bakeFields, [&](const auto& a_entry) { return !litMeshes.contains(a_entry.first); }))
			BuildPlunges();
		groundVersion++;
	}

	float FallSites::RockAlong(RE::hkpWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_direction, float a_reach, int* a_crossings, float* a_last) const
	{
		// Through anything not rock (the falls' own collision is water), counting the rock crossed on the way
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
			// D68: the water this mesh falls into: planes under its top (water objects and cells' water). A plane
			// covers its whole bound, but under its height there is only water or ground, and both hide the falls
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
				const float above = water > -FLT_MAX ? positions[v].z - water : NO_WATER;
				// D70: only near the mesh's foot, where it goes into its water: higher up, a plane whose bound covers
				// the fall (a pool partway down, a whole cell) cut holes in the middle of the sheet
				if (water > -FLT_MAX && positions[v].z < bottom + WATER_CUT_BAND)
					clip = std::min(clip, above + WATER_KEEP);
				// D71: w marks the mesh's foot, the only place the pools may cut it (the shader's pool cut)
				vertexClips.push_back({ clip, 1.0f, above, positions[v].z < bottom + WATER_CUT_BAND ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f, 0.0f });
				clipTested++;
			}
			vertexClipBase[geometry.get()] = base;
			vertexClipVersion++;
		}
		if (clipQueue.empty()) {
			logger::info("[Fluid Field] Waterfall rock cut: {} vertices of {} lit meshes tested", clipTested, vertexClipBase.size());
			ComputeFades();
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

	bool FallSites::Densify(RE::BSGeometry* a_geometry)
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
		// The world scale, for the edge length
		const float scale = std::max(a_geometry->world.scale, 1e-3f);
		// Split until the edges are short (or the budget is spent): each edge's midpoint on the PN triangle's curved
		// edge, its normal, texture coordinate and colour averaged; split vertices (seams in the texture) share the
		// position's mean normal, so the curved edges meet
		for (int level = 0; level < DENSE_LEVELS; level++) {
			float longest = 0.0f;
			for (const auto& tri : triangles) {
				for (int k = 0; k < 3; k++)
					longest = std::max(longest, (positions[tri[k]] - positions[tri[(k + 1) % 3]]).Length() * scale);
			}
			if (longest <= DENSE_EDGE || positions.size() + triangles.size() * 3 / 2 > 60000)
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
			finer.reserve(triangles.size() * 4);
			for (const auto& tri : triangles) {
				const uint32_t ab = middle(tri[0], tri[1]), bc = middle(tri[1], tri[2]), ca = middle(tri[2], tri[0]);
				finer.push_back({ tri[0], ab, ca });
				finer.push_back({ ab, tri[1], bc });
				finer.push_back({ ca, bc, tri[2] });
				finer.push_back({ ab, bc, ca });
			}
			triangles.swap(finer);
		}
		if (positions.size() > 65535)
			return false;
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
		std::vector<uint16_t> indices;
		indices.reserve(triangles.size() * 3);
		for (const auto& tri : triangles) {
			for (const uint32_t k : tri)
				indices.push_back(static_cast<uint16_t>(k));
		}
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
		copy->SetupGeometry(shape);
		copy->FinishSetupGeometry(shape);
		parent->AttachChild(shape, true);
		RE::NiUpdateData update;
		shape->Update(update);
		a_geometry->SetAppCulled(true);
		denseOf[a_geometry] = { held, RE::NiPointer<RE::BSGeometry>(a_geometry), RE::NiPointer<RE::NiNode>(parent) };
		denseShapes.insert(shape);
		logger::info("[Fluid Field]   dense copy of '{}': {} to {} vertices, {} to {} triangles", a_geometry->name.c_str(), counts.vertexCount, positions.size(), counts.triangleCount,
			triangles.size());
		return true;
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
			mesh.open.assign(counts.vertexCount, 0);
			for (const auto& [edge, count] : uses) {
				if (count != 1)
					continue;
				mesh.open[edge.first] = mesh.open[edge.second] = 1;
				mesh.openEdges.push_back({ edge.first, edge.second });
			}
			meshes.push_back(std::move(mesh));
		}
		// An open edge's vertex on a mesh that leads this one (higher, or as high and larger) fades out, so that
		// mesh shows through the seam instead of a hard line where the two meet
		const auto leads = [&](const Mesh& a_lead, const Mesh& a_mesh) {
			return a_lead.top > a_mesh.top + 8.0f || (std::abs(a_lead.top - a_mesh.top) <= 8.0f && a_lead.area > a_mesh.area);
		};
		uint32_t faded = 0, edges = 0, gaps = 0;
		for (const auto& mesh : meshes) {
			// Which vertices lie on a mesh leading this one
			std::vector<uint8_t> onLead(mesh.positions.size(), 0);
			for (std::size_t v = 0; v < mesh.positions.size(); v++) {
				const RE::NiPoint3& p = mesh.positions[v];
				for (const auto& other : meshes) {
					if (&other == &mesh || !leads(other, mesh) || p.x < other.lo[0] - FADE_REACH || p.x > other.hi[0] + FADE_REACH || p.y < other.lo[1] - FADE_REACH ||
						p.y > other.hi[1] + FADE_REACH || p.z < other.lo[2] - FADE_REACH || p.z > other.hi[2] + FADE_REACH)
						continue;
					for (const auto& tri : other.triangles) {
						if (TriangleDistanceSquared(p, other.positions[tri[0]], other.positions[tri[1]], other.positions[tri[2]]) < FADE_REACH * FADE_REACH) {
							onLead[v] = 1;
							break;
						}
					}
					if (onLead[v])
						break;
				}
			}
			// D68: an open edge's vertex fades only where the overlap reaches past it (it and every neighbour on the
			// leading mesh): where two meshes only meet edge to edge, fading opened the gap between them
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
			// D69: an open edge's vertex that lies off every other mesh but near another mesh's open edge (two meshes
			// meeting edge to edge with air between: the dark lines along their seam) moves across the gap, past the
			// middle by GAP_OVERLAP; the other mesh's edge moves to meet it the same way. In the mesh's own space, for
			// both depth passes' vertex shaders
			const RE::NiTransform inverse = mesh.geometry->world.Invert();
			for (std::size_t v = 0; v < mesh.positions.size(); v++) {
				auto& value = vertexClips[mesh.base + v];
				value[4] = value[5] = value[6] = 0.0f;
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
					for (const auto& tri : other.triangles) {
						if (TriangleDistanceSquared(p, other.positions[tri[0]], other.positions[tri[1]], other.positions[tri[2]]) < 4.0f) {
							over = true;
							break;
						}
					}
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
				const RE::NiPoint3 local = inverse * moved - inverse * p;
				value[4] = local.x;
				value[5] = local.y;
				value[6] = local.z;
				gaps++;
			}
		}
		vertexClipVersion++;
		logger::info("[Fluid Field] Waterfall seams: {} of {} open-edge vertices of {} lit meshes lie on a mesh leading them, and fade; {} move across a gap to the mesh beside them", faded,
			edges, meshes.size(), gaps);
	}

	std::unique_ptr<FallSites::BakeJob> FallSites::StartBake(RE::BSGeometry* a_geometry)
	{
		auto* triShape = a_geometry ? a_geometry->AsTriShape() : nullptr;
		auto* renderer = a_geometry ? a_geometry->GetGeometryRuntimeData().rendererData : nullptr;
		if (!triShape || !renderer || !renderer->rawVertexData || !renderer->rawIndexData || !renderer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_UV))
			return nullptr;
		const auto& counts = triShape->GetTrishapeRuntimeData();
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
		for (uint32_t i = 0; i < counts.triangleCount; i++) {
			const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(i) * 3;
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
		// Upward from the point, per mesh: an odd number of its faces above means inside it (an open, buried
		// underside does not matter, an overhang above counts twice)
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
		// D76: a lying (run, not curtain) texel with a rock's underside just above it is under that rock: vanilla's sheet
		// runs on beneath a boulder resting on it (an even number of crossings: the boulder is wholly above), and the
		// water drained out through it instead of turning
		if (a_lying && nearestAbove < SOLID_COVER)
			return true;
		for (const auto& [ref, count] : crossings) {
			if (count % 2 == 0)
				continue;
			// Deeper inside it than a sheet lying on it dips (its nearest face further than BAKE_ROCK_MARGIN)
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

		// D60: rock, without its specks: islands of BAKE_LEAST_ROCK texels or fewer are slivers of the
		// collision the sheet grazes, not boulders; they only scattered the flow
		std::vector<uint8_t> rock = a_job.rock;
		// D68: and rock from the statics' render meshes: a boulder resting on the ground failed the collision test
		// (it wants rock on both sides of the sheet, and the ray from under the boulder started in the ground)
		uint32_t meshRock = 0;
		for (std::size_t i = 0; i < count; i++) {
			if (a_job.covered[i] && !rock[i] && InsideSolid(a_job, a_job.position[i], std::abs(a_job.normal[i].z) > 0.5f)) {
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
		// D79: each column's crest: vanilla's sheets start under the upper water and climb its hidden upstream face to the
		// lip (the top fall's from 260 units under its pool); the water only starts there. Rows before a crest the column
		// climbs more than SIM_CREST_RISE to are its lead-in: not simulated, they carry the water straight on
		std::vector<int32_t> crest(static_cast<std::size_t>(w), -1);
		std::vector<uint8_t> leadIn(count, 0);
		uint32_t leadTexels = 0;
		for (int32_t x = 0; x < iw; x++) {
			int32_t first = -1, top = -1;
			for (int32_t y = 0; y < ih; y++) {
				if (!a_job.covered[index(x, y)])
					continue;
				first = first < 0 ? y : first;
				if (top < 0 || a_job.position[index(x, y)].z > a_job.position[index(x, top)].z)
					top = y;
			}
			if (first < 0)
				continue;
			crest[static_cast<std::size_t>(x)] = first;
			if (top > first && a_job.position[index(x, top)].z - a_job.position[index(x, first)].z > SIM_CREST_RISE) {
				crest[static_cast<std::size_t>(x)] = top;
				for (int32_t y = first; y < top; y++) {
					leadIn[index(x, y)] = 1;
					leadTexels++;
				}
			}
		}
		const auto water = [&](int32_t a_x, int32_t a_y) { return inside(a_x, a_y) && a_job.covered[index(a_x, a_y)] && !rock[index(a_x, a_y)] && !leadIn[index(a_x, a_y)]; };

		// D74 diagnostics: the sim's inputs, next to the log, for the offline harness (scratchpad simbake.py)
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
		}
		// D74: the water on this mesh simulated on its texture grid with the mesh's real shape: per texel the world
		// across and down one texel (the grid's frame), gravity's pull along the surface there (fast down a cliff,
		// none on a pool's face), the water carried by its own speed (it keeps it over a crest, slams into a boulder
		// and is thrown round it), drag toward a sliding speed, and kept from piling up or vanishing (a pressure
		// projection), so it spreads where the way narrows or flattens. In at the mesh's top, out at its foot; rock
		// and the mesh's sides are walls. Run to its steady state; velocities in texels per second
		const auto at = [&](int32_t a_x, int32_t a_y) { return index(std::clamp(a_x, 0, iw - 1), std::clamp(a_y, 0, ih - 1)); };
		std::vector<RE::NiPoint3> tu(count), tv(count);
		std::vector<float> gu(count, 0.0f), gv(count, 0.0f), inv(count * 3, 0.0f);
		for (int32_t y = 0; y < ih; y++) {
			for (int32_t x = 0; x < iw; x++) {
				const std::size_t i = index(x, y);
				if (!a_job.covered[i])
					continue;
				const auto side = [&](int32_t a_dx, int32_t a_dy) {
					const std::size_t forward = at(x + a_dx, y + a_dy), back = at(x - a_dx, y - a_dy);
					const bool f = a_job.covered[forward] && forward != i, b = a_job.covered[back] && back != i;
					if (f && b)
						return (a_job.position[forward] - a_job.position[back]) * 0.5f;
					if (f)
						return a_job.position[forward] - a_job.position[i];
					if (b)
						return a_job.position[i] - a_job.position[back];
					return RE::NiPoint3(0.0f, 0.0f, 0.0f);
				};
				tu[i] = side(1, 0);
				tv[i] = side(0, 1);
				const float uu = tu[i].Dot(tu[i]), uv = tu[i].Dot(tv[i]), vv = tv[i].Dot(tv[i]);
				const float det = uu * vv - uv * uv;
				if (det < 1e-6f)
					continue;
				inv[i * 3] = vv / det;
				inv[i * 3 + 1] = -uv / det;
				inv[i * 3 + 2] = uu / det;
				const RE::NiPoint3 gravity(0.0f, 0.0f, -SIM_GRAVITY);
				const float du = gravity.Dot(tu[i]), dv = gravity.Dot(tv[i]);
				gu[i] = inv[i * 3] * du + inv[i * 3 + 1] * dv;
				gv[i] = inv[i * 3 + 1] * du + inv[i * 3 + 2] * dv;
			}
		}
		// In along the mesh's top band, out along its foot (any water at the edge of the low band, by texture v or by
		// height: the foot is often buried in rock or a pool); texture v runs downstream on every vanilla falls mesh
		float footZ = FLT_MAX, topZ = -FLT_MAX;
		int32_t rowTop = ih, rowBottom = -1;
		for (int32_t y = 0; y < ih; y++) {
			for (int32_t x = 0; x < iw; x++) {
				const std::size_t i = index(x, y);
				if (!a_job.covered[i])
					continue;
				footZ = std::min(footZ, a_job.position[i].z);
				topZ = std::max(topZ, a_job.position[i].z);
				rowTop = std::min(rowTop, y);
				rowBottom = std::max(rowBottom, y);
			}
		}
		const float band = std::max(64.0f, (topZ - footZ) * 0.2f);
		const int32_t rows = std::max(3, static_cast<int32_t>(static_cast<float>(rowBottom - rowTop) * SIM_BAND));
		std::vector<uint8_t> inflow(count, 0), outflow(count, 0);
		uint32_t inflows = 0, outflows = 0;
		for (int32_t x = 0; x < iw; x++) {
			// D79: in at the crest (the first covered row where the column does not climb)
			const int32_t first = crest[static_cast<std::size_t>(x)];
			const bool leads = first >= 0 && first > 0 && leadIn[index(x, std::max(first - 1, 0))];
			if (first >= 0 && (leads || first <= rowTop + rows) && water(x, first)) {
				inflow[index(x, first)] = 1;
				inflows++;
			}
		}
		for (int32_t y = 0; y < ih; y++) {
			for (int32_t x = 0; x < iw; x++) {
				const std::size_t i = index(x, y);
				if (!water(x, y) || inflow[i])
					continue;
				const bool low = y >= rowBottom - rows || a_job.position[i].z < footZ + band;
				const bool edge = !water(x + 1, y) || !water(x - 1, y) || !water(x, y + 1) || !water(x, y - 1);
				if (low && edge) {
					outflow[i] = 1;
					outflows++;
				}
			}
		}
		// The sim on a staggered grid: U across between columns (h x (w+1)), V down between rows ((h+1) x w), texels
		// per second; walls (rock, off the mesh) let nothing through; the faces over the inflow cells bring the water in
		const std::size_t uw = static_cast<std::size_t>(w) + 1;
		std::vector<float> U(static_cast<std::size_t>(h) * uw, 0.0f), V((static_cast<std::size_t>(h) + 1) * w, 0.0f);
		std::vector<uint8_t> uOpen(U.size(), 0), vOpen(V.size(), 0), vIn(V.size(), 0);
		std::vector<float> guF(U.size(), 0.0f), gvF(V.size(), 0.0f), luF(U.size(), 1.0f), lvF(V.size(), 1.0f), vInSpeed(V.size(), 0.0f);
		for (int32_t y = 0; y < ih; y++) {
			for (int32_t x = 1; x < iw; x++) {
				const std::size_t f = static_cast<std::size_t>(y) * uw + x;
				if (water(x - 1, y) && water(x, y)) {
					uOpen[f] = 1;
					guF[f] = 0.5f * (gu[index(x - 1, y)] + gu[index(x, y)]);
					luF[f] = 0.5f * (tu[index(x - 1, y)].Length() + tu[index(x, y)].Length());
				}
			}
		}
		for (int32_t y = 1; y < ih; y++) {
			for (int32_t x = 0; x < iw; x++) {
				const std::size_t f = static_cast<std::size_t>(y) * w + x;
				if (water(x, y - 1) && water(x, y)) {
					vOpen[f] = 1;
					gvF[f] = 0.5f * (gv[index(x, y - 1)] + gv[index(x, y)]);
					lvF[f] = 0.5f * (tv[index(x, y - 1)].Length() + tv[index(x, y)].Length());
				}
			}
		}
		for (int32_t y = 0; y < ih; y++) {
			for (int32_t x = 0; x < iw; x++) {
				if (!inflow[index(x, y)])
					continue;
				const std::size_t f = static_cast<std::size_t>(y) * w + x;
				vIn[f] = 1;
				lvF[f] = std::max(tv[index(x, y)].Length(), 1e-3f);
				vInSpeed[f] = SIM_INFLOW / lvF[f];
				V[f] = vInSpeed[f];
			}
		}
		for (std::size_t f = 0; f < V.size(); f++) {
			if (vOpen[f])
				V[f] = SIM_INFLOW / std::max(lvF[f], 1e-3f);
		}
		const auto sampleGrid = [](const std::vector<float>& a_grid, int32_t a_w, int32_t a_h, float a_x, float a_y) {
			const float cx = std::clamp(a_x, 0.0f, static_cast<float>(a_w - 1)), cy = std::clamp(a_y, 0.0f, static_cast<float>(a_h - 1));
			const auto x0 = std::min(static_cast<int32_t>(cx), a_w - 1), y0 = std::min(static_cast<int32_t>(cy), a_h - 1);
			const int32_t x1 = std::min(x0 + 1, a_w - 1), y1 = std::min(y0 + 1, a_h - 1);
			const float fx = cx - static_cast<float>(x0), fy = cy - static_cast<float>(y0);
			const auto at = [&](int32_t a_px, int32_t a_py) { return a_grid[static_cast<std::size_t>(a_py) * a_w + a_px]; };
			return (at(x0, y0) * (1.0f - fx) + at(x1, y0) * fx) * (1.0f - fy) + (at(x0, y1) * (1.0f - fx) + at(x1, y1) * fx) * fy;
		};
		const auto uw32 = static_cast<int32_t>(uw);
		// U at cell coordinates (px, py): U's face x sits at px = x - 0.5; V's face y at py = y - 0.5
		const auto sampleU = [&](float a_x, float a_y) { return sampleGrid(U, uw32, ih, a_x + 0.5f, a_y); };
		const auto sampleV = [&](float a_x, float a_y) { return sampleGrid(V, iw, ih + 1, a_x, a_y + 0.5f); };
		// The pressure's cells: water that is not an outflow (those hold 0)
		std::vector<int32_t> cell(count, -1);
		std::vector<std::size_t> cells;
		for (std::size_t i = 0; i < count; i++) {
			const auto x = static_cast<int32_t>(i % w), y = static_cast<int32_t>(i / w);
			if (water(x, y) && !outflow[i]) {
				cell[i] = static_cast<int32_t>(cells.size());
				cells.push_back(i);
			}
		}
		const std::size_t n = cells.size();
		std::vector<float> pressure(n, 0.0f), rhs(n), residual(n), direction(n), product(n), diagonal(n);
		static constexpr int32_t NX[4] = { 1, -1, 0, 0 }, NY[4] = { 0, 0, 1, -1 };
		for (std::size_t k = 0; k < n; k++) {
			const auto x = static_cast<int32_t>(cells[k] % w), y = static_cast<int32_t>(cells[k] / w);
			float d = 1e-4f;
			for (int j = 0; j < 4; j++)
				d += water(x + NX[j], y + NY[j]) ? 1.0f : 0.0f;
			diagonal[k] = d;
		}
		const auto apply = [&](const std::vector<float>& a_in, std::vector<float>& a_out) {
			for (std::size_t k = 0; k < n; k++) {
				const auto x = static_cast<int32_t>(cells[k] % w), y = static_cast<int32_t>(cells[k] / w);
				float sum = diagonal[k] * a_in[k];
				for (int j = 0; j < 4; j++) {
					const int32_t nx = x + NX[j], ny = y + NY[j];
					if (inside(nx, ny) && cell[index(nx, ny)] >= 0)
						sum -= a_in[static_cast<std::size_t>(cell[index(nx, ny)])];
				}
				a_out[k] = sum;
			}
		};
		const float drag = SIM_GRAVITY / (SIM_TERMINAL * SIM_TERMINAL);
		std::vector<float> nU(U.size()), nV(V.size());
		for (int step = 0; step < SIM_STEPS && n > 0; step++) {
			float fastest = 1.0f;
			for (const float value : U)
				fastest = std::max(fastest, std::abs(value));
			for (const float value : V)
				fastest = std::max(fastest, std::abs(value));
			const float dt = std::min(0.8f / fastest, 0.05f);
			// Carried by itself, pulled along the surface, slowed by the bed and the air
			for (int32_t y = 0; y < ih; y++) {
				for (int32_t x = 0; x <= iw; x++) {
					const std::size_t f = static_cast<std::size_t>(y) * uw + x;
					if (!uOpen[f]) {
						nU[f] = 0.0f;
						continue;
					}
					const float px = static_cast<float>(x) - 0.5f, py = static_cast<float>(y);
					const float a = U[f], b = sampleV(px, py);
					float value = sampleU(px - a * dt, py - b * dt) + guF[f] * dt;
					const float worldSpeed = std::sqrt(value * luF[f] * value * luF[f] + b * b * sampleGrid(lvF, iw, ih + 1, px, py + 0.5f) * sampleGrid(lvF, iw, ih + 1, px, py + 0.5f));
					nU[f] = value / (1.0f + (SIM_FRICTION + drag * worldSpeed) * dt);
				}
			}
			for (int32_t y = 0; y <= ih; y++) {
				for (int32_t x = 0; x < iw; x++) {
					const std::size_t f = static_cast<std::size_t>(y) * w + x;
					if (vIn[f]) {
						nV[f] = vInSpeed[f];
						continue;
					}
					if (!vOpen[f]) {
						nV[f] = 0.0f;
						continue;
					}
					const float px = static_cast<float>(x), py = static_cast<float>(y) - 0.5f;
					const float a = sampleU(px, py), b = V[f];
					float value = sampleV(px - a * dt, py - b * dt) + gvF[f] * dt;
					const float lu = sampleGrid(luF, uw32, ih, px + 0.5f, py);
					const float worldSpeed = std::sqrt(value * lvF[f] * value * lvF[f] + a * a * lu * lu);
					nV[f] = value / (1.0f + (SIM_FRICTION + drag * worldSpeed) * dt);
				}
			}
			U.swap(nU);
			V.swap(nV);
			// Kept from piling up or vanishing: solve for the pressure (conjugate gradients, from the last step's)
			for (std::size_t k = 0; k < n; k++) {
				const auto x = static_cast<std::size_t>(cells[k] % w), y = static_cast<std::size_t>(cells[k] / w);
				const float divergence = U[y * uw + x + 1] - U[y * uw + x] + V[(y + 1) * w + x] - V[y * w + x];
				rhs[k] = -divergence;
			}
			apply(pressure, product);
			float rr = 0.0f;
			for (std::size_t k = 0; k < n; k++) {
				residual[k] = rhs[k] - product[k];
				direction[k] = residual[k] / diagonal[k];
				rr += residual[k] * direction[k];
			}
			for (int iteration = 0; iteration < SIM_CG && rr > 1e-8f; iteration++) {
				apply(direction, product);
				float pAp = 0.0f;
				for (std::size_t k = 0; k < n; k++)
					pAp += direction[k] * product[k];
				if (std::abs(pAp) < 1e-12f)
					break;
				const float alpha = rr / pAp;
				float next = 0.0f;
				for (std::size_t k = 0; k < n; k++) {
					pressure[k] += alpha * direction[k];
					residual[k] -= alpha * product[k];
					next += residual[k] * residual[k] / diagonal[k];
				}
				const float beta = next / rr;
				for (std::size_t k = 0; k < n; k++)
					direction[k] = residual[k] / diagonal[k] + beta * direction[k];
				rr = next;
			}
			const auto pAt = [&](int32_t a_x, int32_t a_y) {
				const int32_t c = cell[index(a_x, a_y)];
				return c >= 0 ? pressure[static_cast<std::size_t>(c)] : 0.0f;
			};
			for (int32_t y = 0; y < ih; y++) {
				for (int32_t x = 1; x < iw; x++) {
					const std::size_t f = static_cast<std::size_t>(y) * uw + x;
					if (uOpen[f])
						U[f] -= pAt(x, y) - pAt(x - 1, y);
				}
			}
			for (int32_t y = 1; y < ih; y++) {
				for (int32_t x = 0; x < iw; x++) {
					const std::size_t f = static_cast<std::size_t>(y) * w + x;
					if (vOpen[f])
						V[f] -= pAt(x, y) - pAt(x, y - 1);
				}
			}
		}
		// The streaks' turn from the sim's direction (across per down; across or back up, as far round as the sweep
		// can take, SIM_MOST_TURN); dead pockets (barely moving, closed in by rock) keep vanilla's way down. The speed
		// from how far the water has fallen, to its sliding speed (a sheet thickens where it narrows, it does not race),
		// against vanilla's scroll (D67); foam where it strikes rock
		const float texelV = a_job.uvSize[1] / static_cast<float>(h);
		static constexpr float VANILLA_SCROLL = 0.15f;  // Texture coordinates per second (D56)
		std::vector<float> uc(count, 0.0f), vc(count, 0.0f), flowSpeed(count, 0.0f);
		std::vector<float> moving;
		for (int32_t y = 0; y < ih; y++) {
			for (int32_t x = 0; x < iw; x++) {
				const std::size_t i = index(x, y);
				if (!water(x, y))
					continue;
				uc[i] = 0.5f * (U[static_cast<std::size_t>(y) * uw + x] + U[static_cast<std::size_t>(y) * uw + x + 1]);
				vc[i] = 0.5f * (V[static_cast<std::size_t>(y) * w + x] + V[(static_cast<std::size_t>(y) + 1) * w + x]);
				flowSpeed[i] = (tu[i] * uc[i] + tv[i] * vc[i]).Length();
				moving.push_back(flowSpeed[i]);
			}
		}
		float median = 1.0f;
		if (!moving.empty()) {
			std::nth_element(moving.begin(), moving.begin() + static_cast<std::ptrdiff_t>(moving.size() / 2), moving.end());
			median = std::max(moving[moving.size() / 2], 1.0f);
		}
		std::vector<float> turn(count, 0.0f), foam(count, 0.0f), run(count, 0.0f), speed(count, 1.0f);
		std::vector<int32_t> from(count, -1);
		std::vector<std::size_t> frontier;
		float fastestWorld = 0.0f;
		uint32_t dead = 0;
		for (std::size_t i = 0; i < count; i++) {
			const auto x = static_cast<int32_t>(i % w), y = static_cast<int32_t>(i / w);
			if (!water(x, y))
				continue;
			const float a = uc[i], b = vc[i];
			const float length = std::sqrt(a * a + b * b);
			float bend = b > 0.18f * length ? std::clamp(a / b, -SIM_MOST_TURN, SIM_MOST_TURN) : (a >= 0.0f ? SIM_MOST_TURN : -SIM_MOST_TURN);
			const float alive = std::clamp(flowSpeed[i] / (median * SIM_DEAD), 0.0f, 1.0f);
			dead += alive < 0.5f ? 1 : 0;
			turn[i] = bend * alive;
			const float world = std::min(SIM_TERMINAL, std::sqrt(SIM_INFLOW * SIM_INFLOW + SIM_GRAVITY * std::max(topZ - a_job.position[i].z, 0.0f)));
			fastestWorld = std::max(fastestWorld, world);
			const float vanilla = VANILLA_SCROLL * std::max(tv[i].Length(), 1e-3f) / std::max(texelV, 1e-6f);
			// D76: at most 1.6 (vanilla's own UVs already stretch down the drops; 4x stretched the texture into blocks)
			speed[i] = std::clamp(world / std::max(vanilla, 1.0f), BAKE_LEAST_SPEED, BAKE_MOST_DOWN);
			float strike = 0.0f;
			for (int k = 0; k < 4; k++) {
				const int32_t nx = x + NX[k], ny = y + NY[k];
				if (inside(nx, ny) && a_job.covered[index(nx, ny)] && rock[index(nx, ny)] && a * NX[k] + b * NY[k] > 0.0f)
					strike = std::max(strike, (tu[i] * (a * static_cast<float>(NX[k])) + tv[i] * (b * static_cast<float>(NY[k]))).Length());
			}
			foam[i] = std::clamp(strike / 300.0f, 0.0f, 1.0f);
			from[i] = static_cast<int32_t>(i);
			frontier.push_back(i);
		}
		// D76: water coming over the lip goes straight on: the turn eases in over the first SIM_LIP_ROWS rows of each
		// column (on the lip's curl the sim's frame twists, and its flow there scattered sideways)
		for (int32_t x = 0; x < iw; x++) {
			// D79: from the crest; the lead-in before it goes straight
			const int32_t first = crest[static_cast<std::size_t>(x)];
			if (first < 0)
				continue;
			for (int32_t y = 0; y < first; y++) {
				if (leadIn[index(x, y)] && from[index(x, y)] < 0) {
					turn[index(x, y)] = 0.0f;
					speed[index(x, y)] = 1.0f;
					from[index(x, y)] = static_cast<int32_t>(index(x, y));
					frontier.push_back(index(x, y));
				}
			}
			for (int32_t y = first; y < std::min(ih, first + SIM_LIP_ROWS); y++) {
				const float ease = static_cast<float>(y - first) / static_cast<float>(SIM_LIP_ROWS);
				turn[index(x, y)] *= ease * ease * (3.0f - 2.0f * ease);
			}
		}
		for (std::size_t head = 0; head < frontier.size(); head++) {
			const std::size_t i = frontier[head];
			const auto x = static_cast<int32_t>(i % w), y = static_cast<int32_t>(i / w);
			for (int k = 0; k < 4; k++) {
				const int32_t nx = x + NX[k], ny = y + NY[k];
				if (!inside(nx, ny) || from[index(nx, ny)] >= 0)
					continue;
				from[index(nx, ny)] = from[i];
				turn[index(nx, ny)] = turn[i];
				speed[index(nx, ny)] = speed[i];
				frontier.push_back(index(nx, ny));
			}
		}
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
		blur(turn, 2);
		blur(speed, 2);
		logger::info("[Fluid Field]   sim: {}x{} texels, {} in and {} out, fastest {:.0f} units/s, {} texels in dead pockets, {} in hidden lead-ins", w, h, inflows, outflows, fastestWorld, dead,
			leadTexels);

		// The masks (D60), where the warp would misbehave and real water turns white anyway: the rim of each
		// rock, the water piling against it and squeezing past it (the solve's foam), and its wake, carried
		// down the streaks from the rock and fading over BAKE_WAKE_TEXELS
		std::vector<float> wake(count, 0.0f);
		for (int32_t y = 0; y < ih; y++) {
			for (int32_t x = 0; x < iw; x++) {
				const std::size_t i = index(x, y);
				if (rock[i] && a_job.covered[i]) {
					wake[i] = 1.0f;
					continue;
				}
				if (y == 0)
					continue;
				const float back = static_cast<float>(x) - turn[i];
				const int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(back)), 0, iw - 1);
				const int32_t x1 = std::min(x0 + 1, iw - 1);
				const float f = std::clamp(back - static_cast<float>(x0), 0.0f, 1.0f);
				wake[i] = (wake[index(x0, y - 1)] * (1.0f - f) + wake[index(x1, y - 1)] * f) * std::exp(-1.0f / BAKE_WAKE_TEXELS);
			}
		}
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
		blur(foam, 1);
		blur(wake, 1);

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
		field->speed = std::move(speed);
		field->wake = std::move(wake);
		field->foam = std::move(foam);
		field->normal = a_job.normal;
		field->rock = rock;
		logger::info("[Fluid Field]   bake: {}x{} texels, {} more inside the statics' meshes", w, h, meshRock);
		return field;
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
		};
		std::vector<Node> nodes;
		for (const auto& [geometry, field] : bakeFields) {
			if (!field || !grounded.contains(geometry))
				continue;
			Node node;
			node.geometry = geometry;
			node.field = field.get();
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
			const float texelU = field.uvSize[0] / static_cast<float>(w), texelV = field.uvSize[1] / static_cast<float>(h);
			const auto u = [&](int32_t a_x) { return field.uvMin[0] + (static_cast<float>(a_x) + 0.5f) * texelU; };
			// The water coming in at each column's top: from the feeder where it reaches; elsewhere its streaks
			// carried on in a straight line over u (fitted to the fed columns) and its mean run time; on a top,
			// the mesh's own u, and no time yet
			std::vector<float> across0(w, 0.0f), along0(w, 0.0f);
			std::vector<uint8_t> fed(w, 0);
			double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, sumAlong = 0.0;
			const Node* feeder = a_node.feeder >= 0 && nodes[a_node.feeder].done ? &nodes[a_node.feeder] : nullptr;
			if (feeder) {
				for (const auto& link : a_node.links) {
					const std::size_t j = static_cast<std::size_t>(link[2]) * feeder->field->width + link[1];
					across0[link[0]] = feeder->across[j];
					along0[link[0]] = feeder->along[j];
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
			double slope = 1.0, intercept = 0.0, meanAlong = 0.0;
			if (n >= 1.0) {
				const double denominator = n * sxx - sx * sx;
				slope = n >= 2.0 && std::abs(denominator) > 1e-9 ? (n * sxy - sx * sy) / denominator : 1.0;
				intercept = (sy - slope * sx) / n;
				meanAlong = sumAlong / n;
			}
			for (int32_t x = 0; x < w; x++) {
				if (fed[x])
					continue;
				across0[x] = static_cast<float>(slope * u(x) + intercept);
				along0[x] = static_cast<float>(meanAlong);
			}
			// The solved turn, softened (D68: more, so it cannot whorl)
			std::vector<float> turn = field.turn;
			BlurGrid(turn, w, h, 6);
			BlurGrid(turn, w, h, 3);
			field.swept = turn;
			// Down the sheet a row at a time (semi-Lagrangian, as D60's warp): each texel's streak and run time are
			// those of the texel above it on its way down, its run time grown by the row's length over its speed
			a_node.across.assign(count, 0.0f);
			a_node.along.assign(count, 0.0f);
			for (int32_t y = 0; y < h; y++) {
				for (int32_t x = 0; x < w; x++) {
					const std::size_t i = static_cast<std::size_t>(y) * w + x;
					const int32_t first = a_node.first[x];
					if (first < 0 || y <= first || y == 0) {
						a_node.across[i] = across0[x];
						a_node.along[i] = along0[x] - static_cast<float>(std::max(first - y, 0)) * texelV;
						continue;
					}
					const float back = static_cast<float>(x) - turn[i];
					const int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(back)), 0, w - 1);
					const int32_t x1 = std::min(x0 + 1, w - 1);
					const float f = std::clamp(back - static_cast<float>(x0), 0.0f, 1.0f);
					const std::size_t a = static_cast<std::size_t>(y - 1) * w + x0, b = static_cast<std::size_t>(y - 1) * w + x1;
					a_node.across[i] = a_node.across[a] * (1.0f - f) + a_node.across[b] * f;
					a_node.along[i] = a_node.along[a] * (1.0f - f) + a_node.along[b] * f + texelV / std::max(field.speed[i], BAKE_LEAST_SPEED);
				}
			}
			// The streaks' bend (D60): partly collapsed (they run partly under the rock), softened, and eased back
			// where they squeeze or spread too far
			std::vector<float> offset(count);
			// D71: and never drifting more than BAKE_MOST_DRIFT texels across (long sinuous drifts on flat runs
			// looked like jelly)
			const float texelAcross = field.uvSize[0] / static_cast<float>(w);
			for (std::size_t i = 0; i < count; i++)
				offset[i] = std::clamp((a_node.across[i] - across0[i % w]) * BAKE_WARP_KEEP, -BAKE_MOST_DRIFT * texelAcross, BAKE_MOST_DRIFT * texelAcross);
			BlurGrid(offset, w, h, BAKE_WARP_BLUR);
			std::vector<float> trust(count, 1.0f);
			for (int32_t y = 0; y < h; y++) {
				for (int32_t x = 1; x + 1 < w; x++) {
					const float straight = across0[x + 1] - across0[x - 1];
					if (std::abs(straight) < 1e-6f)
						continue;
					const std::size_t i = static_cast<std::size_t>(y) * w + x;
					const float spacing = (straight + offset[i + 1] - offset[i - 1]) / straight;
					trust[i] = std::clamp(std::min((spacing - BAKE_LEAST_SPACING * 0.75f) / (BAKE_LEAST_SPACING * 0.25f), (BAKE_MOST_SPACING * 1.25f - spacing) / (BAKE_MOST_SPACING * 0.25f)), 0.0f, 1.0f);
				}
			}
			BlurGrid(trust, w, h, 2);
			for (std::size_t i = 0; i < count; i++)
				a_node.across[i] = across0[i % w] + offset[i] * trust[i];
			// D70: streaks never cross or fold (where the turn flipped over a short way the streak coordinate folded
			// over itself: the whorls): along each row, at least BAKE_LEAST_SPACING of the straight spacing apart
			for (int32_t y = 0; y < h; y++) {
				for (int32_t x = 1; x < w; x++) {
					const std::size_t i = static_cast<std::size_t>(y) * w + x;
					const float straight = across0[x] - across0[x - 1];
					const float least = straight * BAKE_LEAST_SPACING;
					if (straight > 0.0f)
						a_node.across[i] = std::max(a_node.across[i], a_node.across[i - 1] + least);
					else if (straight < 0.0f)
						a_node.across[i] = std::min(a_node.across[i], a_node.across[i - 1] + least);
				}
			}
			FlowBake bake;
			bake.width = field.width;
			bake.height = field.height;
			bake.uvMin[0] = field.uvMin[0];
			bake.uvMin[1] = field.uvMin[1];
			bake.uvSize[0] = field.uvSize[0];
			bake.uvSize[1] = field.uvSize[1];
			bake.texels.resize(count * 4);
			for (std::size_t i = 0; i < count; i++) {
				bake.texels[i * 4] = a_node.across[i];
				bake.texels[i * 4 + 1] = a_node.along[i];
				bake.texels[i * 4 + 2] = std::clamp(field.wake[i], 0.0f, 1.0f);
				bake.texels[i * 4 + 3] = std::clamp(field.foam[i], 0.0f, 1.0f);
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
		flowBakeVersion++;
		logger::info("[Fluid Field] Waterfall flow linked: {} meshes, {} fed by the mesh above them", nodes.size(), linked);
		BuildPlunges();
	}

	void FallSites::BakeStep()
	{
		// Finished solves
		for (auto it = bakesPending.begin(); it != bakesPending.end();) {
			if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
				++it;
				continue;
			}
			auto field = it->second.get();
			if (it->first && grounded.contains(it->first.get()) && field)
				bakeFields[it->first.get()] = std::move(field);
			it = bakesPending.erase(it);
			// D67: linked once all are solved, so each mesh carries on from the one above it
			if (bakesPending.empty() && bakeQueue.empty() && !bakeJob) {
				logger::info("[Fluid Field] Waterfall flow bakes: {} meshes, {} texels, {} inside rock", bakeFields.size(), bakedTexels, bakedRockTexels);
				LinkBakes();
			}
		}
		auto* tes = RE::TES::GetSingleton();
		if (!tes)
			return;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(BAKE_BUDGET_MS);
		while (std::chrono::steady_clock::now() < deadline) {
			if (!bakeJob) {
				if (bakeQueue.empty())
					return;
				RE::NiPointer<RE::BSGeometry> geometry = bakeQueue.back();
				bakeQueue.pop_back();
				if (!geometry || !grounded.contains(geometry.get()))
					continue;
				bakeJob = StartBake(geometry.get());
				if (bakeJob)
					GatherSolids(*bakeJob);
				continue;
			}
			auto* cell = tes->interiorCell ? tes->interiorCell : tes->GetCell(bakeJob->geometry->world.translate);
			auto* bhkWorld = cell ? cell->GetbhkWorld() : nullptr;
			auto* world = bhkWorld ? bhkWorld->GetWorld1() : nullptr;
			if (!world)
				return;
			{
				// Each texel inside rock? A ray from outside it, in along the sheet's facing, crosses rock an odd
				// number of times to reach a point inside (D57), deeper than a sheet lying on its rock
				const RE::BSReadLockGuard lock(bhkWorld->worldLock);
				int rays = 0;
				for (; bakeJob->next < bakeJob->covered.size() && rays < 64; bakeJob->next++) {
					const std::size_t i = bakeJob->next;
					if (!bakeJob->covered[i])
						continue;
					rays++;
					// From both sides of the sheet: a curtain before a cliff has the cliff on one side only (a ray
					// from that side starts inside the cliff and crosses its face once); a boulder through the
					// sheet is on both
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
					bakedTexels++;
					bakedRockTexels += bakeJob->rock[i];
				}
			}
			if (bakeJob->next >= bakeJob->covered.size()) {
				auto job = std::shared_ptr<BakeJob>(std::move(bakeJob));
				bakesPending.emplace_back(job->geometry, std::async(std::launch::async, [job]() { return SolveBake(*job); }));
			}
		}
	}

	void FallSites::CaptureStep()
	{
		// One capture at a time, nearest the start of the list: sites are few and each takes seconds
		for (const auto& site : sites) {
			if (!site.captureKey)
				continue;
			auto it = captures.find(site.captureKey);
			if (it == captures.end() || it->second.IsComplete())
				continue;
			auto& capture = it->second;
			if (!capture.Cast(CAPTURE_BUDGET_MS))
				return;
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
	}

	uint32_t FallSites::CountKind(Kind a_kind) const
	{
		return static_cast<uint32_t>(std::count_if(sites.begin(), sites.end(), [&](const Site& a_site) { return a_site.kind == a_kind; }));
	}

	void FallSites::Update(const WaterSurfaces& a_water)
	{
		waterSurfaces = &a_water;
		CaptureStep();
		ClipStep();
		BakeStep();
		PollPools();
		if (updatesUntilScan > 0) {
			updatesUntilScan--;
			return;
		}
		updatesUntilScan = SCAN_INTERVAL;

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
		if (signature == refSignature && a_water.GetSignature() == waterSignature)
			return;
		refSignature = signature;
		waterSignature = a_water.GetSignature();
		refs.swap(found);
		Rebuild(a_water);
		Summarise();
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
		// Rivers are lined with rapids and splash meshes in an unbroken chain, which would link a whole
		// gorge into one site. So falls (falling water and skirts) cluster among themselves; the other
		// effects join the nearest fall within CLUSTER_RADIUS, and those left over form rapids sites
		// among themselves at a tighter radius.
		// Skirts are tall mist sheets, so only the falling water itself makes a fall
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

		// S3, level pairing and tiers, on the falling water's own bounds (the whole site's include
		// mist and splashes well away from the drop)
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
				// Below, planes may lie well under the falling water (it runs on underwater); above, a fall's
				// top cannot be much higher than its falling water's top
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
				// A plane near the top of the falling water is its top, and the fall ends on ground; one lower
				// down is its bottom (falling water often runs on well below the surface)
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

	void FallSites::BuildPlungeMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_field) const
	{
		// D44, D47: where the falls meet the water below, along their whole line into it: every plunge point (D78: from
		// the meshes' own sims, BuildPlunges) foams the water round it, harder plunges more, and the simulated flow
		// carries the foam off downstream
		a_field.assign(static_cast<std::size_t>(a_size) * a_size, 0.0f);
		const auto reach = static_cast<int32_t>(std::ceil(PLUNGE_FOAM_RADIUS / a_cell));
		const auto n = static_cast<int32_t>(a_size);
		for (const auto& plunge : plungePoints) {
			const auto cx = static_cast<int32_t>(std::lround((plunge.position[0] - a_originX) / a_cell));
			const auto cy = static_cast<int32_t>(std::lround((plunge.position[1] - a_originY) / a_cell));
			if (cx < -reach || cy < -reach || cx >= n + reach || cy >= n + reach)
				continue;
			const float strength = std::clamp(0.85f + 0.15f * plunge.strength, 0.0f, 1.0f);  // D52: the rim is solid for every fall
			for (int32_t y = cy - reach; y <= cy + reach; y++) {
				for (int32_t x = cx - reach; x <= cx + reach; x++) {
					if (x < 0 || y < 0 || x >= n || y >= n)
						continue;
					const float dx = a_originX + static_cast<float>(x) * a_cell - plunge.position[0];
					const float dy = a_originY + static_cast<float>(y) * a_cell - plunge.position[1];
					const float falloff = std::clamp(1.0f - std::sqrt(dx * dx + dy * dy) / PLUNGE_FOAM_RADIUS, 0.0f, 1.0f);
					float& value = a_field[static_cast<std::size_t>(y) * a_size + x];
					value = std::max(value, strength * falloff);
				}
			}
		}
	}

	void FallSites::BuildPlunges()
	{
		// D78: from each lit mesh's own sim (its BakeField): where it goes into the water below (texels on its foot at or
		// under that water, from above it), and where its water strikes rock (the bake's foam). The water's way there is
		// the sim's (down the mesh, turned), its speed the height it has fallen down the mesh (as SolveBake's)
		std::unordered_map<int64_t, FallPlunge> points;
		std::unordered_map<int64_t, FallEmitter> chosen;
		const auto offer = [&](const RE::NiPoint3& a_p, const RE::NiPoint3& a_n, const RE::NiPoint3& a_direction, float a_speed, float a_strength, FallEmitterKind a_kind, float a_spacing) {
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
		std::vector<std::array<float, 5>> planes;
		uint32_t meets = 0;
		for (const auto& [geometry, field] : bakeFields) {
			if (!field || !grounded.contains(geometry))
				continue;
			const auto w = static_cast<int32_t>(field->width), h = static_cast<int32_t>(field->height);
			const std::size_t count = static_cast<std::size_t>(w) * h;
			if (field->position.size() != count || field->turn.size() != count || field->normal.size() != count)
				continue;
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
			// The water this mesh falls into: the pools' surface (ComputePools), else the engine's highest plane under the
			// mesh's top (as ClipStep's cut); looked for only on its foot, where a plane's bound reaching over the fall
			// cannot be mistaken for its water (D70)
			CollectWaterPlanes(lo[0], lo[1], hi[0], hi[1], planes);
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
			const std::vector<float>& turn = field->swept.size() == count ? field->swept : field->turn;
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
			for (int32_t y = 0; y < h; y++) {
				for (int32_t x = 0; x < w; x++) {
					const std::size_t i = index(x, y);
					if (!field->covered[i] || (!field->rock.empty() && field->rock[i]))
						continue;
					const RE::NiPoint3& here = field->position[i];
					float level = -FLT_MAX;
					const bool foot = here.z < bottom + WATER_CUT_BAND;
					const bool water = foot && waterAt(here, level);
					// Where the water strikes rock (the bake's foam), above the water below
					if (field->foam[i] >= OBSTACLE_FOAM && !(water && here.z < level - POOL_COVER)) {
						float pool;
						if (!PoolLevelAt(here.x, here.y, pool) || here.z >= pool - POOL_COVER)
							offer(here, upward(i), flowAt(x, y), speedAt(here.z), 0.4f + 0.6f * field->foam[i], FallEmitterKind::Strike, OBSTACLE_SPACING);
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
					const RE::NiPoint3 direction = flowAt(x, y);
					const float speed = speedAt(p.z);
					offer(p, RE::NiPoint3(0.0f, 0.0f, 1.0f), direction, speed, strength, FallEmitterKind::Plunge, PLUNGE_SPACING);
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
		plungePoints.clear();
		plungePoints.reserve(points.size());
		for (const auto& [key, plunge] : points)
			plungePoints.push_back(plunge);
		sprayEmitters.clear();
		sprayEmitters.reserve(chosen.size());
		uint32_t kinds[3]{};
		for (const auto& [key, emitter] : chosen) {
			sprayEmitters.push_back(emitter);
			kinds[std::clamp(static_cast<int>(emitter.kind), 0, 2)]++;
		}
		emitterVersion++;
		groundVersion++;  // The plunge map
		logger::info("[Fluid Field] Waterfall plunges: {} texels meet the water below, {} plunge points; spray emitters: {} at plunges, {} striking rock", meets, plungePoints.size(),
			kinds[1], kinds[2]);
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
		// Flow: the sim's way and speed on each mesh, an arrow every few texels; not inside rock, under the water
		// below, on a sheet's far side, or behind rock from the camera (rays, a few hundred a frame, cached until the
		// camera moves)
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
						RE::NiPoint3 direction = tv + tu * (field->swept.size() == field->turn.size() ? field->swept[i] : field->turn[i]);
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
			// Levels: the top green, the bottom blue, tiers between teal; translucent over the footprint
			// Each level is drawn where its water is: the planes at that height, trimmed to the fall's area
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
