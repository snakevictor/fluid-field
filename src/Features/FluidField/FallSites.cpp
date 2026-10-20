#include "FallSites.h"

#include "EngineGrid.h"
#include "FallSheets.h"
#include "FallSurfaceRenderer.h"

#include "WaterSurfaces.h"
#include <DirectXPackedVector.h>

#include <algorithm>
#include <array>
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
					// D64: our own sheets live under vanilla's pieces' parent; the layer mask is for vanilla's
					if (a_geometry->name == FallSheets::SHEET_NAME)
						return RE::BSVisit::BSVisitControl::kContinue;
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

	void FallSites::SetTranslucent(bool a_translucent)
	{
		if (a_translucent == translucent)
			return;
		translucent = a_translucent;
		RefreshGrounded();
	}

	void FallSites::RestoreAlpha()
	{
		for (auto& [alpha, original] : blendedAlpha) {
			if (original.first)
				original.first->alphaFlags = original.second;
		}
		blendedAlpha.clear();
		for (auto& geometry : borrowedAlpha) {
			if (geometry)
				geometry->GetGeometryRuntimeData().alphaProperty = RE::NiPointer<RE::NiAlphaProperty>();
		}
		borrowedAlpha.clear();
		translucentGeometry.clear();
	}

	void FallSites::Clear()
	{
		hideEffects = false;
		RefreshHiddenEffects();
		RemoveDense();
		RestoreAlpha();
		grounded.clear();
		clipQueue.clear();
		bakeQueue.clear();
		bakeJob.reset();
		if (!flowBakes.empty())
			flowBakeVersion++;
		flowBakes.clear();
		bakeFields.clear();
		chainRoot.clear();
		vertexClipBase.clear();
		groundVersion++;
		refs.clear();
		sites.clear();
		refSignature = 0;
		waterSignature = 0;
		updatesUntilScan = 0;
		cellSummaries.clear();
		// Captures stay: turning waterfalls back on finds the same sites, and their rock, at once
	}

	const FallCapture* FallSites::GetCapture(const Site& a_site) const
	{
		const auto it = a_site.captureKey ? captures.find(a_site.captureKey) : captures.end();
		return it != captures.end() ? &it->second : nullptr;
	}

	void FallSites::RasterSurface(const Site& a_site, float a_originX, float a_originY, float a_spacing, int32_t a_width, int32_t a_height, std::vector<float>& a_surface, bool a_planes,
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
		// The site's pools
		for (const auto& plane : a_planes ? a_site.planes : std::vector<Site::Plane>{}) {
			int32_t x0, x1, y0, y1;
			range(plane.min[0], plane.max[0], a_originX, a_width, x0, x1);
			range(plane.min[1], plane.max[1], a_originY, a_height, y0, y1);
			for (int32_t y = y0; y <= y1; y++) {
				for (int32_t x = x0; x <= x1; x++) {
					float& stored = a_surface[window(x, y)];
					stored = std::max(stored, plane.height);
				}
			}
		}
	}

	void FallSites::StartStream(const Site& a_site, const FallCapture& a_capture)
	{
		FallStream::Input input;
		input.width = a_capture.GetWidth();
		input.height = a_capture.GetHeight();
		input.cell = FallCapture::CELL;
		std::vector<float> lowest, highest;
		RasterSurface(a_site, a_capture.GetOriginX(), a_capture.GetOriginY(), FallCapture::CELL, static_cast<int32_t>(input.width), static_cast<int32_t>(input.height), input.surface, false, &lowest,
			&highest);
		input.kind.assign(input.surface.size(), FallStream::Wall);
		input.bed.assign(input.surface.size(), 0.0f);
		for (uint32_t y = 0; y < input.height; y++) {
			for (uint32_t x = 0; x < input.width; x++)
				input.bed[static_cast<std::size_t>(y) * input.width + x] = a_capture.At(x, y);
		}
		// D40, D45: the pools' water. The site's paired planes miss pools (a tier between two falls is in
		// none), and planes are whole cells, far wider than their water, so the engine's own water height
		// is sampled on every cell. A pool's water is where the rock lies under it and either the falls'
		// own surface lies at its level or the rock lies shallow under it (a river or pool bed, not the face
		// of a fall)
		input.cover.assign(input.surface.size(), NO_SURFACE);
		{
			const auto cw = static_cast<int32_t>(input.width);
			const auto ch = static_cast<int32_t>(input.height);
			std::vector<float> levels(input.surface.size(), NO_SURFACE);
			std::vector<std::size_t> open;
			// D53: the lowest and highest of the falls round each cell (POOL_LIP_REACH): where they drop
			// away below a pool's level with none coming in from above it, the cell is the pool's lip or
			// the air under a curtain falling from it, which the top-down rock calls deep water
			std::vector<float> lowNear(input.surface.size(), FLT_MAX), highNear(input.surface.size(), NO_SURFACE);
			{
				std::vector<float> lowRow(input.surface.size(), FLT_MAX), highRow(input.surface.size(), NO_SURFACE);
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
						if (waterSurfaces->SampleHeight(a_capture.GetOriginX() + static_cast<float>(x) * FallCapture::CELL, a_capture.GetOriginY() + static_cast<float>(y) * FallCapture::CELL, height) !=
							WaterSurfaces::Source::None)
							level = height;
					}
					levels[i] = level;
					const float bed = input.bed[i];
					const float mesh = input.surface[i];
					if (level <= NO_SURFACE || bed <= FallCapture::NONE || bed >= level - POOL_COVER)
						continue;
					const bool meshAtLevel = mesh > NO_SURFACE && mesh >= level - POOL_BAND;
					const bool bottomPool = level <= a_site.bottom + POOL_BAND;  // D50: the pool the falls end in: nothing lower to mistake it for
					const bool shallowBed = bed >= level - POOL_SHALLOW;
					if (dropsAway(i, level)) {
						lips++;
						continue;
					}
					if (meshAtLevel || shallowBed || bottomPool) {
						input.cover[i] = level;
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
				const float level = input.cover[i];
				static constexpr int32_t DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
				for (int k = 0; k < 4; k++) {
					const int32_t nx = x + DX[k], ny = y + DY[k];
					if (nx < 0 || ny < 0 || nx >= cw || ny >= ch)
						continue;
					const std::size_t j = static_cast<std::size_t>(ny) * cw + nx;
					if (input.cover[j] > NO_SURFACE || std::abs(levels[j] - level) > 1.0f)
						continue;
					const float bed = input.bed[j];
					const float mesh = input.surface[j];
					if (bed <= FallCapture::NONE || bed >= level - POOL_COVER || (mesh > NO_SURFACE && mesh < level - POOL_BAND) || dropsAway(j, level))
						continue;
					input.cover[j] = level;
					open.push_back(j);
				}
			}
			for (std::size_t i = 0; i < input.surface.size(); i++)
				input.surface[i] = std::max(input.surface[i], input.cover[i]);
			logger::info("[Fluid Field] Waterfall site #{} pools: {} cells, {} kept out as lips and curtains", a_site.id,
				std::count_if(input.cover.begin(), input.cover.end(), [](float a_cover) { return a_cover > NO_SURFACE; }), lips);
		}
		uint32_t water = 0, inflow = 0, outflow = 0;
		for (uint32_t y = 0; y < input.height; y++) {
			for (uint32_t x = 0; x < input.width; x++) {
				const std::size_t i = static_cast<std::size_t>(y) * input.width + x;
				const float bed = a_capture.At(x, y);
				const float surface = input.surface[i];
				// Water only where its surface stands above the rock: rock standing through it is a wall
				if (bed <= FallCapture::NONE || surface <= NO_SURFACE || surface < bed + STREAM_MIN_DEPTH)
					continue;
				input.kind[i] = FallStream::Water;
			}
		}
		// The open boundaries are a rim only: water near the top or bottom that borders no water (the
		// edge of where it comes in or leaves). Inside, even the pools are solved
		std::vector<uint8_t> kind = input.kind;
		const auto w = static_cast<int32_t>(input.width);
		const auto h = static_cast<int32_t>(input.height);
		for (int32_t y = 0; y < h; y++) {
			for (int32_t x = 0; x < w; x++) {
				const std::size_t i = static_cast<std::size_t>(y) * w + x;
				if (input.kind[i] != FallStream::Water)
					continue;
				bool rim = false;
				for (int32_t dy = -1; dy <= 1 && !rim; dy++) {
					for (int32_t dx = -1; dx <= 1 && !rim; dx++) {
						const int32_t nx = x + dx;
						const int32_t ny = y + dy;
						rim = nx < 0 || ny < 0 || nx >= w || ny >= h || input.kind[static_cast<std::size_t>(ny) * w + nx] == FallStream::Wall;
					}
				}
				const float surface = input.surface[i];
				if (rim && surface >= a_site.top - STREAM_INFLOW_BAND) {
					kind[i] = FallStream::Inflow;
					inflow++;
				} else if (rim && surface <= a_site.bottom + STREAM_OUTFLOW_BAND) {
					kind[i] = FallStream::Outflow;
					outflow++;
				} else {
					water++;
				}
			}
		}
		// D36: water under a pool's surface (vanilla's meshes dipping into the water above or below) is the
		// pool's: under the top it is where the falls' water comes from, under the bottom where it goes.
		// The flow then starts where the falls leave the water above, and ends where they meet the water below
		{
			const std::vector<float>& cover = input.cover;
			for (std::size_t i = 0; i < kind.size(); i++) {
				if (kind[i] != FallStream::Water || cover[i] <= NO_SURFACE || cover[i] < input.surface[i] - POOL_COVER)
					continue;
				if (cover[i] >= a_site.top - STREAM_INFLOW_BAND) {
					kind[i] = FallStream::Inflow;
					inflow++;
					water--;
				} else if (cover[i] <= a_site.bottom + STREAM_OUTFLOW_BAND) {
					kind[i] = FallStream::Outflow;
					outflow++;
					water--;
				}
			}
		}
		input.kind.swap(kind);
		logger::info("[Fluid Field] Waterfall site #{} flow: solving {}x{}, {} water, {} inflow, {} outflow cells", a_site.id, input.width, input.height, water, inflow, outflow);
		auto& stream = streams[a_site.captureKey];
		stream.originX = a_capture.GetOriginX();
		stream.originY = a_capture.GetOriginY();
		stream.siteId = a_site.id;
		stream.ready = false;
		stream.meshReady = false;
		GatherSources(a_site, stream.sources);
		stream.levels = a_site.levels;
		stream.top = a_site.top;
		stream.pending = std::async(std::launch::async, [input = std::move(input)]() { return FallStream::Solve(input); });
	}

	void FallSites::PollStreams()
	{
		// Retired solves finish on their own; let them go once done
		std::erase_if(retired, [](std::future<FallStream::Result>& a_future) { return a_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
		std::erase_if(retiredMeshes, [](std::future<SurfaceMesh>& a_future) { return a_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
		bool changed = false;
		for (const auto& site : sites) {
			if (!site.captureKey)
				continue;
			const FallCapture* capture = GetCapture(site);
			if (!capture || !capture->IsComplete())
				continue;
			auto it = streams.find(site.captureKey);
			if (it == streams.end()) {
				StartStream(site, *capture);
				continue;
			}
			auto& stream = it->second;
			// The merged surface, once its build is done
			if (stream.ready && !stream.meshReady && stream.meshPending.valid() && stream.meshPending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
				SurfaceMesh mesh = stream.meshPending.get();
				stream.vertices.swap(mesh.vertices);
				stream.indices.swap(mesh.indices);
				stream.emitters.swap(mesh.emitters);
				stream.seamFoam.swap(mesh.seamFoam);
				stream.plunges.swap(mesh.plunges);
				stream.seamPoints.swap(mesh.seamPoints);
				stream.waterLines.swap(mesh.waterLines);
				stream.meshReady = true;
				changed = true;
				uint32_t kinds[3]{};
				for (const auto& emitter : stream.emitters)
					kinds[std::clamp(static_cast<int>(emitter.kind), 0, 2)]++;
				logger::info("[Fluid Field] Waterfall site #{} surface: {} source triangles ({} under another piece dropped), {} vertices, {} triangles, {} seam points ({} impacts); emitters: {} at drops, {} at plunges and impacts, {} striking; {} plunge pushes",
					stream.siteId, stream.sources.size(), mesh.culled, stream.vertices.size(), stream.indices.size() / 3, mesh.seams, mesh.impacts, kinds[0], kinds[1], kinds[2], stream.plunges.size());
				continue;
			}
			if (stream.ready || !stream.pending.valid() || stream.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
				continue;
			stream.result = stream.pending.get();
			stream.ready = true;
			stream.meshPending = std::async(std::launch::async, [flow = stream.result, originX = stream.originX, originY = stream.originY, sources = stream.sources, levels = stream.levels]() {
				return BuildMergedSurface(flow, originX, originY, sources, levels);
			});
			changed = true;
			float speed = 0.0f, foam = 0.0f;
			uint32_t water = 0;
			for (std::size_t i = 0; i < stream.result.water.size(); i++) {
				if (!stream.result.water[i])
					continue;
				speed += stream.result.speed[i];
				foam += stream.result.foam[i] > 0.5f ? 1.0f : 0.0f;
				water++;
			}
			logger::info("[Fluid Field] Waterfall site #{} flow: solved in {} sweeps (last change {:.3f}), mean speed {:.2f}, {:.0f} foam cells of {}", stream.siteId,
				stream.result.iterations, stream.result.residual, water ? speed / static_cast<float>(water) : 0.0f, foam, water);
		}
		if (changed) {
			groundVersion++;
			RefreshSurfaces();
			RefreshHiddenEffects();
		}
	}

	void FallSites::GatherSources(const Site& a_site, std::vector<SourceTriangle>& a_sources) const
	{
		a_sources.clear();
		uint32_t piece = 0;
		for (const uint32_t member : a_site.members) {
			if (refs[member].cls != FxClass::Body)
				continue;
			auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refs[member].formID);
			auto* root = ref ? ref->Get3D() : nullptr;
			if (!root)
				continue;
			// The water's own surface: the lit one where the model has it, else its largest effect layer
			std::vector<RE::BSGeometry*> lit;
			RE::BSGeometry* largest = nullptr;
			uint32_t largestCount = 0;
			RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
				auto* triShape = a_geometry->AsTriShape();
				auto* renderer = a_geometry->GetGeometryRuntimeData().rendererData;
				if (!triShape || !renderer || !renderer->rawVertexData || !renderer->rawIndexData || triShape->GetTrishapeRuntimeData().triangleCount == 0)
					return RE::BSVisit::BSVisitControl::kContinue;
				auto* property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
				if (property && netimmerse_cast<RE::BSLightingShaderProperty*>(property)) {
					lit.push_back(a_geometry);
				} else if (property && netimmerse_cast<RE::BSEffectShaderProperty*>(property) && triShape->GetTrishapeRuntimeData().triangleCount > largestCount) {
					largest = a_geometry;
					largestCount = triShape->GetTrishapeRuntimeData().triangleCount;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			if (lit.empty() && largest)
				lit.push_back(largest);
			for (auto* geometry : lit) {
				auto* triShape = geometry->AsTriShape();
				auto* renderer = geometry->GetGeometryRuntimeData().rendererData;
				const auto& counts = triShape->GetTrishapeRuntimeData();
				const uint32_t stride = renderer->vertexDesc.GetSize();
				const uint32_t positionOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
				const bool hasUV = renderer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_UV);
				const uint32_t uvOffset = hasUV ? renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_TEXCOORD0) : 0;
				if (stride < positionOffset + 12)
					continue;
				const auto& transform = geometry->world;
				std::vector<RE::NiPoint3> positions(counts.vertexCount);
				std::vector<std::array<float, 2>> uvs(counts.vertexCount, { 0.0f, 0.0f });
				for (uint32_t v = 0; v < counts.vertexCount; v++) {
					const uint8_t* data = renderer->rawVertexData + static_cast<std::size_t>(v) * stride;
					float local[3];
					std::memcpy(local, data + positionOffset, sizeof(local));
					positions[v] = transform * RE::NiPoint3(local[0], local[1], local[2]);
					if (hasUV) {
						const auto* uv = reinterpret_cast<const uint16_t*>(data + uvOffset);
						uvs[v] = { DirectX::PackedVector::XMConvertHalfToFloat(uv[0]), DirectX::PackedVector::XMConvertHalfToFloat(uv[1]) };
					}
				}
				// Smooth normals: each vertex's faces, area weighted
				std::vector<RE::NiPoint3> normals(counts.vertexCount, RE::NiPoint3(0.0f, 0.0f, 0.0f));
				for (uint32_t t = 0; t < counts.triangleCount; t++) {
					const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(t) * 3;
					if (index[0] >= counts.vertexCount || index[1] >= counts.vertexCount || index[2] >= counts.vertexCount)
						continue;
					const RE::NiPoint3 face = (positions[index[1]] - positions[index[0]]).Cross(positions[index[2]] - positions[index[0]]);
					for (int k = 0; k < 3; k++)
						normals[index[k]] += face;
				}
				for (auto& normal : normals) {
					const float length = normal.Length();
					// Upward, so every piece agrees which side is the top
					normal = length > 1e-6f ? normal / length : RE::NiPoint3(0.0f, 0.0f, 1.0f);
					if (normal.z < 0.0f)
						normal = -normal;
				}
				// D35: its texel density, world units per texture repeat along u and v, area weighted
				{
					double sum[2]{}, area = 0.0;
					for (uint32_t t = 0; t < counts.triangleCount; t++) {
						const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(t) * 3;
						if (index[0] >= counts.vertexCount || index[1] >= counts.vertexCount || index[2] >= counts.vertexCount)
							continue;
						const RE::NiPoint3 e1 = positions[index[1]] - positions[index[0]];
						const RE::NiPoint3 e2 = positions[index[2]] - positions[index[0]];
						const float du1 = uvs[index[1]][0] - uvs[index[0]][0], dv1 = uvs[index[1]][1] - uvs[index[0]][1];
						const float du2 = uvs[index[2]][0] - uvs[index[0]][0], dv2 = uvs[index[2]][1] - uvs[index[0]][1];
						const float det = du1 * dv2 - du2 * dv1;
						const float faceArea = 0.5f * e1.Cross(e2).Length();
						if (std::abs(det) < 1e-8f || faceArea < 1e-3f)
							continue;
						const RE::NiPoint3 tu = (e1 * dv2 - e2 * dv1) / det;
						const RE::NiPoint3 tv = (e2 * du1 - e1 * du2) / det;
						sum[0] += static_cast<double>(tu.Length()) * faceArea;
						sum[1] += static_cast<double>(tv.Length()) * faceArea;
						area += faceArea;
					}
					if (area > 0.0)
						textureScales[geometry] = { static_cast<float>(sum[0] / area), static_cast<float>(sum[1] / area) };
				}
				for (uint32_t t = 0; t < counts.triangleCount; t++) {
					const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(t) * 3;
					if (index[0] >= counts.vertexCount || index[1] >= counts.vertexCount || index[2] >= counts.vertexCount)
						continue;
					SourceTriangle triangle{};
					for (int k = 0; k < 3; k++) {
						const auto& p = positions[index[k]];
						const auto& n = normals[index[k]];
						triangle.position[k][0] = p.x;
						triangle.position[k][1] = p.y;
						triangle.position[k][2] = p.z;
						triangle.normal[k][0] = n.x;
						triangle.normal[k][1] = n.y;
						triangle.normal[k][2] = n.z;
						triangle.uv[k][0] = uvs[index[k]][0];
						triangle.uv[k][1] = uvs[index[k]][1];
					}
					triangle.piece = piece;
					a_sources.push_back(triangle);
				}
				piece++;
			}
		}
	}

	FallSites::SurfaceMesh FallSites::BuildMergedSurface(const FallStream::Result& a_flow, float a_originX, float a_originY, const std::vector<SourceTriangle>& a_sources, const std::vector<float>& a_levels)
	{
		// The top of the falls: the drop below it counts into the travel time, so the texture runs down steep water too
		float top = -FLT_MAX;
		for (const auto& triangle : a_sources) {
			for (int k = 0; k < 3; k++)
				top = std::max(top, triangle.position[k][2]);
		}
		SurfaceMesh mesh;
		const auto w = static_cast<int32_t>(a_flow.width);
		const auto h = static_cast<int32_t>(a_flow.height);
		const float cell = FallCapture::CELL;

		// The flow carried a few cells past the water's edge (each dry cell takes its wet neighbours'
		// mean), so vanilla's pieces reaching beyond the solved water keep one texture field
		FallStream::Result dilated = a_flow;
		std::vector<uint8_t> known = a_flow.water;
		for (int pass = 0; pass < FLOW_DILATE_CELLS; pass++) {
			std::vector<uint8_t> next = known;
			for (int32_t y = 0; y < h; y++) {
				for (int32_t x = 0; x < w; x++) {
					const std::size_t i = static_cast<std::size_t>(y) * w + x;
					if (known[i])
						continue;
					float sums[9]{};
					int count = 0;
					for (int32_t dy = -1; dy <= 1; dy++) {
						for (int32_t dx = -1; dx <= 1; dx++) {
							const int32_t nx = x + dx, ny = y + dy;
							if (nx < 0 || ny < 0 || nx >= w || ny >= h)
								continue;
							const std::size_t j = static_cast<std::size_t>(ny) * w + nx;
							if (!known[j])
								continue;
							sums[0] += dilated.psi[j];
							sums[1] += dilated.travel[j];
							sums[2] += dilated.speed[j];
							sums[3] += dilated.foam[j];
							sums[4] += dilated.depth[j];
							sums[5] += dilated.vx[j];
							sums[6] += dilated.vy[j];
							sums[7] += dilated.head[j];
							sums[8] += dilated.rockFoam[j];
							count++;
						}
					}
					if (!count)
						continue;
					const float k = 1.0f / static_cast<float>(count);
					dilated.psi[i] = sums[0] * k;
					dilated.travel[i] = sums[1] * k;
					dilated.speed[i] = sums[2] * k;
					dilated.foam[i] = sums[3] * k;
					dilated.depth[i] = sums[4] * k;
					dilated.vx[i] = sums[5] * k;
					dilated.vy[i] = sums[6] * k;
					dilated.head[i] = sums[7] * k;
					dilated.rockFoam[i] = sums[8] * k;
					next[i] = 1;
				}
			}
			known.swap(next);
		}
		dilated.water = known;
		const FallStream::Result& flowField = dilated;

		// The flow under a point, bilinear over its water cells; the weight is how much water is around
		struct Flow
		{
			float across, travel, speed, foam, edge, depth, water, vx, vy, head, rockFoam;
		};
		const auto flowAt = [&](float a_x, float a_y) {
			Flow flow{};
			const float fx = (a_x - a_originX) / cell;
			const float fy = (a_y - a_originY) / cell;
			const auto ix = static_cast<int32_t>(std::floor(fx));
			const auto iy = static_cast<int32_t>(std::floor(fy));
			const float tx = fx - static_cast<float>(ix);
			const float ty = fy - static_cast<float>(iy);
			float weight = 0.0f;
			for (int32_t dy = 0; dy <= 1; dy++) {
				for (int32_t dx = 0; dx <= 1; dx++) {
					const int32_t x = ix + dx;
					const int32_t y = iy + dy;
					if (x < 0 || y < 0 || x >= w || y >= h)
						continue;
					const std::size_t i = static_cast<std::size_t>(y) * w + x;
					const float k = (dx ? tx : 1.0f - tx) * (dy ? ty : 1.0f - ty);
					flow.water += flowField.water[i] ? k : 0.0f;
					if (!flowField.water[i])
						continue;
					flow.across += flowField.psi[i] * k;
					flow.travel += flowField.travel[i] * k;
					flow.speed += flowField.speed[i] * k;
					flow.foam += flowField.foam[i] * k;
					flow.edge += flowField.edge[i] * k;
					flow.depth += flowField.depth[i] * k;
					flow.vx += flowField.vx[i] * k;
					flow.vy += flowField.vy[i] * k;
					flow.head += flowField.head[i] * k;
					flow.rockFoam += flowField.rockFoam[i] * k;
					weight += k;
				}
			}
			if (weight > 1e-4f) {
				const float inverse = 1.0f / weight;
				flow.across *= inverse;
				flow.travel *= inverse;
				flow.speed *= inverse;
				flow.foam *= inverse;
				flow.edge *= inverse;
				flow.depth *= inverse;
				flow.vx *= inverse;
				flow.vy *= inverse;
				flow.head *= inverse;
				flow.rockFoam *= inverse;
			}
			return flow;
		};

		// Where two pieces lie on each other, only the upper one stays: the doubled layers, and their
		// seams, go. Steep parts (curtains) are left as they are
		const auto faceUp = [](const SourceTriangle& a_t) {
			const float ax = a_t.position[1][0] - a_t.position[0][0], ay = a_t.position[1][1] - a_t.position[0][1], az = a_t.position[1][2] - a_t.position[0][2];
			const float bx = a_t.position[2][0] - a_t.position[0][0], by = a_t.position[2][1] - a_t.position[0][1], bz = a_t.position[2][2] - a_t.position[0][2];
			const float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
			const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
			return length > 1e-6f ? std::abs(nz) / length : 0.0f;
		};
		const auto heightAt = [](const SourceTriangle& a_t, float a_x, float a_y, float& a_z) {
			const auto& a = a_t.position[0];
			const auto& b = a_t.position[1];
			const auto& c = a_t.position[2];
			const float area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
			if (std::abs(area) < 1e-3f)
				return false;
			const float w0 = ((b[0] - a_x) * (c[1] - a_y) - (c[0] - a_x) * (b[1] - a_y)) / area;
			const float w1 = ((c[0] - a_x) * (a[1] - a_y) - (a[0] - a_x) * (c[1] - a_y)) / area;
			const float w2 = 1.0f - w0 - w1;
			if (w0 < -1e-3f || w1 < -1e-3f || w2 < -1e-3f)
				return false;
			a_z = w0 * a[2] + w1 * b[2] + w2 * c[2];
			return true;
		};
		std::unordered_map<int64_t, std::vector<uint32_t>> buckets;
		const auto bucketKey = [](int32_t a_x, int32_t a_y) { return (static_cast<int64_t>(a_x) << 32) ^ static_cast<uint32_t>(a_y); };
		for (uint32_t t = 0; t < a_sources.size(); t++) {
			const auto& triangle = a_sources[t];
			if (faceUp(triangle) < MERGE_FACE_UP)
				continue;
			float lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
			for (int k = 0; k < 3; k++) {
				for (int a = 0; a < 2; a++) {
					lo[a] = std::min(lo[a], triangle.position[k][a]);
					hi[a] = std::max(hi[a], triangle.position[k][a]);
				}
			}
			for (auto by = static_cast<int32_t>(std::floor(lo[1] / MERGE_BUCKET)); by <= static_cast<int32_t>(std::floor(hi[1] / MERGE_BUCKET)); by++) {
				for (auto bx = static_cast<int32_t>(std::floor(lo[0] / MERGE_BUCKET)); bx <= static_cast<int32_t>(std::floor(hi[0] / MERGE_BUCKET)); bx++)
					buckets[bucketKey(bx, by)].push_back(t);
			}
		}
		std::vector<uint8_t> kept(a_sources.size(), 1);
		for (uint32_t t = 0; t < a_sources.size(); t++) {
			const auto& triangle = a_sources[t];
			if (faceUp(triangle) < MERGE_FACE_UP)
				continue;
			// Covered at its centre and at its corners pulled a little inward
			bool covered = true;
			for (int probe = 0; probe < 4 && covered; probe++) {
				float px = 0.0f, py = 0.0f, pz = 0.0f;
				for (int k = 0; k < 3; k++) {
					const float weight = probe == 3 ? 1.0f / 3.0f : (k == probe ? 0.6f : 0.2f);
					px += triangle.position[k][0] * weight;
					py += triangle.position[k][1] * weight;
					pz += triangle.position[k][2] * weight;
				}
				bool found = false;
				const auto it = buckets.find(bucketKey(static_cast<int32_t>(std::floor(px / MERGE_BUCKET)), static_cast<int32_t>(std::floor(py / MERGE_BUCKET))));
				if (it != buckets.end()) {
					for (const uint32_t other : it->second) {
						if (a_sources[other].piece == triangle.piece)
							continue;
						float oz;
						if (!heightAt(a_sources[other], px, py, oz))
							continue;
						const float above = oz - pz;
						if (above < -MERGE_BELOW || above > MERGE_ABOVE)
							continue;
						if (a_sources[other].piece < triangle.piece) {
							found = true;
							break;
						}
					}
				}
				covered = found;
			}
			if (covered)
				kept[t] = 0;
		}

		// Each piece subdivided evenly, so it has no cracks of its own; vertices welded where pieces meet
		std::vector<uint32_t> splits;
		for (const auto& triangle : a_sources) {
			if (splits.size() <= triangle.piece)
				splits.resize(triangle.piece + 1, 1);
			float longest = 0.0f;
			for (int k = 0; k < 3; k++) {
				const auto& a = triangle.position[k];
				const auto& b = triangle.position[(k + 1) % 3];
				longest = std::max(longest, std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2])));
			}
			splits[triangle.piece] = std::max(splits[triangle.piece], std::clamp(static_cast<uint32_t>(std::ceil(longest / SURFACE_EDGE)), 1u, SURFACE_MAX_SPLIT));
		}
		std::unordered_map<int64_t, uint32_t> welded;
		const auto weldKey = [](const float a_p[3]) {
			const auto qx = static_cast<int64_t>(std::lround(a_p[0] / SURFACE_WELD)) & 0x1FFFFF;
			const auto qy = static_cast<int64_t>(std::lround(a_p[1] / SURFACE_WELD)) & 0x1FFFFF;
			const auto qz = static_cast<int64_t>(std::lround(a_p[2] / SURFACE_WELD)) & 0x1FFFFF;
			return (qx << 42) | (qy << 21) | qz;
		};
		const auto vertexFor = [&](const SourceTriangle& a_t, float a_u, float a_v) {
			const float w0 = 1.0f - a_u - a_v;
			FallSurfaceVertex vertex{};
			for (int k = 0; k < 3; k++) {
				vertex.position[k] = a_t.position[0][k] * w0 + a_t.position[1][k] * a_u + a_t.position[2][k] * a_v;
				vertex.normal[k] = a_t.normal[0][k] * w0 + a_t.normal[1][k] * a_u + a_t.normal[2][k] * a_v;
			}
			const auto key = weldKey(vertex.position);
			if (const auto it = welded.find(key); it != welded.end())
				return it->second;
			const float length = std::sqrt(vertex.normal[0] * vertex.normal[0] + vertex.normal[1] * vertex.normal[1] + vertex.normal[2] * vertex.normal[2]);
			for (float& component : vertex.normal)
				component = length > 1e-6f ? component / length : 0.0f;
			vertex.uv[0] = a_t.uv[0][0] * w0 + a_t.uv[1][0] * a_u + a_t.uv[2][0] * a_v;
			vertex.uv[1] = a_t.uv[0][1] * w0 + a_t.uv[1][1] * a_u + a_t.uv[2][1] * a_v;
			const Flow flow = flowAt(vertex.position[0], vertex.position[1]);
			vertex.across = flow.across;
			vertex.travel = flow.travel + (top - vertex.position[2]) / FALL_SPEED;
			vertex.speed = flow.speed;
			vertex.foam = flow.foam;
			// Where the falls meet a body of water, above and below, the join is hidden in foam
			for (const float level : a_levels)
				vertex.foam = std::max(vertex.foam, std::clamp(1.0f - std::abs(vertex.position[2] - level) / MEET_FOAM_HEIGHT, 0.0f, 1.0f));
			vertex.edge = 1.0f;
			vertex.depth = flow.depth;
			// Off the flow (beyond the solved water) vanilla's own texture stays; steep water keeps the flow's,
			// its drop counted in the travel time
			vertex.flow = std::clamp((flow.water - 0.1f) / 0.4f, 0.0f, 1.0f);
			const auto index = static_cast<uint32_t>(mesh.vertices.size());
			mesh.vertices.push_back(vertex);
			welded.emplace(key, index);
			return index;
		};
		for (uint32_t t = 0; t < a_sources.size(); t++) {
			if (!kept[t])
				continue;
			const auto& triangle = a_sources[t];
			const uint32_t n = splits[triangle.piece];
			const float step = 1.0f / static_cast<float>(n);
			std::vector<uint32_t> grid((n + 1) * (n + 2) / 2);
			const auto at = [&](uint32_t a_i, uint32_t a_j) -> uint32_t& { return grid[a_i * (n + 1) - a_i * (a_i - 1) / 2 + a_j]; };
			for (uint32_t i = 0; i <= n; i++) {
				for (uint32_t j = 0; j + i <= n; j++)
					at(i, j) = vertexFor(triangle, static_cast<float>(i) * step, static_cast<float>(j) * step);
			}
			for (uint32_t i = 0; i < n; i++) {
				for (uint32_t j = 0; j + i < n; j++) {
					const uint32_t a = at(i, j), b = at(i + 1, j), c = at(i, j + 1);
					if (a != b && b != c && a != c)
						mesh.indices.insert(mesh.indices.end(), { a, b, c });
					if (i + j + 1 < n) {
						const uint32_t d = at(i + 1, j + 1);
						if (b != d && d != c && b != c)
							mesh.indices.insert(mesh.indices.end(), { b, d, c });
					}
				}
			}
		}
		mesh.culled = static_cast<uint32_t>(std::count(kept.begin(), kept.end(), static_cast<uint8_t>(0)));

		// Spray and mist (D35) where the falls throw it, with the water's own motion there: at the drops
		// (where the surface falls away downstream), at the plunges (where falling water meets the water
		// below), and where the water strikes something (rock in its way, vanilla's pieces meeting)
		constexpr float KEPT_GRAVITY = 687.0f * 0.6f;
		const auto motionAt = [&](const float a_p[3], const float a_n[3], float a_direction[3]) {
			const Flow flow = flowAt(a_p[0], a_p[1]);
			const float vx = flow.vx * FallStream::BASE_SPEED, vy = flow.vy * FallStream::BASE_SPEED;
			const float solved = std::sqrt(vx * vx + vy * vy);
			const float nz = std::max(a_n[2], 0.15f);
			float lifted[3] = { vx, vy, -(a_n[0] * vx + a_n[1] * vy) / nz };
			float down[3] = { a_n[0] * a_n[2], a_n[1] * a_n[2], -1.0f + a_n[2] * a_n[2] };
			const auto normalize = [](float a_v[3]) {
				const float length = std::sqrt(a_v[0] * a_v[0] + a_v[1] * a_v[1] + a_v[2] * a_v[2]);
				for (int k = 0; k < 3; k++)
					a_v[k] = length > 1e-5f ? a_v[k] / length : 0.0f;
				return length;
			};
			const bool moving = flow.water > 0.1f && normalize(lifted) > 1e-3f;
			const bool sloped = normalize(down) > 1e-3f;
			const float steep = 1.0f - std::clamp((a_n[2] - 0.35f) / 0.25f, 0.0f, 1.0f);
			const float agree = sloped && moving ? std::clamp((lifted[0] * down[0] + lifted[1] * down[1] + lifted[2] * down[2]) * 4.0f, 0.0f, 1.0f) : 0.0f;
			for (int k = 0; k < 3; k++)
				a_direction[k] = moving ? lifted[k] + (down[k] - lifted[k]) * steep * agree : down[k];
			normalize(a_direction);
			const float speed = moving ? solved + (std::max(solved, FallStream::BASE_SPEED) - solved) * steep * agree : FallStream::BASE_SPEED * 0.5f;
			const float drop = std::max(flow.head - a_p[2], 0.0f);
			return std::sqrt(speed * speed + 2.0f * KEPT_GRAVITY * drop);
		};
		// D37: vanilla's meshes reach on into rock, the ground and the pools, where nothing of them shows:
		// nothing is thrown from there
		const auto buried = [&](const float a_p[3], bool a_pools) {
			const auto x = static_cast<int32_t>(std::lround((a_p[0] - a_originX) / cell));
			const auto y = static_cast<int32_t>(std::lround((a_p[1] - a_originY) / cell));
			if (x < 0 || y < 0 || x >= w || y >= h)
				return false;
			const std::size_t i = static_cast<std::size_t>(y) * w + x;
			if (a_flow.bed.size() == static_cast<std::size_t>(w) * h && a_flow.bed[i] > FallCapture::NONE && a_p[2] < a_flow.bed[i] - BURIED_MARGIN)
				return true;
			return a_pools && a_flow.cover.size() == static_cast<std::size_t>(w) * h && a_p[2] < a_flow.cover[i] - POOL_COVER;
		};
		std::unordered_map<int64_t, FallEmitter> chosen;
		const auto offer = [&](const float a_p[3], const float a_n[3], float a_strength, FallEmitterKind a_kind, float a_spacing) {
			// Plunges stand at the pool's own level: only the rock hides them
			if (buried(a_p, a_kind != FallEmitterKind::Plunge))
				return;
			const auto key = (static_cast<int64_t>(std::floor(a_p[0] / a_spacing)) << 40) ^ (static_cast<int64_t>(std::floor(a_p[1] / a_spacing)) << 18) ^
			                 static_cast<int64_t>(std::floor(a_p[2] / a_spacing)) ^ (static_cast<int64_t>(a_kind) << 60);
			auto [it, added] = chosen.try_emplace(key);
			if (!added && it->second.strength >= a_strength)
				return;
			FallEmitter& emitter = it->second;
			for (int k = 0; k < 3; k++) {
				emitter.position[k] = a_p[k];
				emitter.normal[k] = a_n[k];
			}
			emitter.strength = std::clamp(a_strength, 0.0f, 1.0f);
			emitter.speed = motionAt(a_p, a_n, emitter.direction);
			emitter.kind = static_cast<float>(a_kind);
		};
		float bottomLevel = FLT_MAX, topLevel = -FLT_MAX;
		for (const float level : a_levels) {
			bottomLevel = std::min(bottomLevel, level);
			topLevel = std::max(topLevel, level);
		}

		// The drops: water cells whose surface falls away a cell or two downstream, but not just before them
		{
			const auto levelAt = [&](float a_x, float a_y, float& a_level) {
				const auto ix = static_cast<int32_t>(std::floor(a_x));
				const auto iy = static_cast<int32_t>(std::floor(a_y));
				const float tx = a_x - static_cast<float>(ix), ty = a_y - static_cast<float>(iy);
				float sum = 0.0f, weight = 0.0f;
				for (int32_t dy = 0; dy <= 1; dy++) {
					for (int32_t dx = 0; dx <= 1; dx++) {
						const int32_t x = ix + dx, y = iy + dy;
						if (x < 0 || y < 0 || x >= w || y >= h || !a_flow.water[static_cast<std::size_t>(y) * w + x])
							continue;
						const float k = (dx ? tx : 1.0f - tx) * (dy ? ty : 1.0f - ty);
						sum += a_flow.level[static_cast<std::size_t>(y) * w + x] * k;
						weight += k;
					}
				}
				if (weight < 0.2f)
					return false;
				a_level = sum / weight;
				return true;
			};
			for (int32_t y = 0; y < h; y++) {
				for (int32_t x = 0; x < w; x++) {
					const std::size_t i = static_cast<std::size_t>(y) * w + x;
					if (!a_flow.water[i] || a_flow.speed[i] < 0.05f)
						continue;
					const float dx = a_flow.vx[i] / a_flow.speed[i], dy = a_flow.vy[i] / a_flow.speed[i];
					const float here = a_flow.level[i];
					float down, up;
					if (!levelAt(static_cast<float>(x) + dx * LIP_CELLS, static_cast<float>(y) + dy * LIP_CELLS, down))
						continue;
					if (!levelAt(static_cast<float>(x) - dx * LIP_CELLS, static_cast<float>(y) - dy * LIP_CELLS, up))
						up = here;
					const float drop = here - down;
					if (drop < LIP_DROP || up - here > drop * 0.5f)
						continue;
					const float p[3] = { a_originX + static_cast<float>(x) * cell, a_originY + static_cast<float>(y) * cell, here };
					const float n[3] = { 0.0f, 0.0f, 1.0f };
					offer(p, n, std::clamp(drop / (LIP_DROP * 3.0f), 0.5f, 1.0f), FallEmitterKind::Lip, EMITTER_SPACING);  // D53: where the water above goes over
				}
			}
		}

		// D53: rock in the water's way, as the solve sees it: water piling against rock or squeezed past it
		// (a boulder standing out of a run, where no piece of the falls dips into it) throws spray too
		if (a_flow.rockFoam.size() == static_cast<std::size_t>(w) * h) {
			for (int32_t y = 0; y < h; y++) {
				for (int32_t x = 0; x < w; x++) {
					const std::size_t i = static_cast<std::size_t>(y) * w + x;
					if (!a_flow.water[i] || a_flow.rockFoam[i] < OBSTACLE_FOAM)
						continue;
					const float p[3] = { a_originX + static_cast<float>(x) * cell, a_originY + static_cast<float>(y) * cell, a_flow.level[i] };
					const float n[3] = { 0.0f, 0.0f, 1.0f };
					offer(p, n, std::clamp(0.4f + 0.6f * a_flow.rockFoam[i], 0.0f, 1.0f), FallEmitterKind::Strike, OBSTACLE_SPACING);
				}
			}
		}

		// The plunges and the rock in the way: on the surface's own vertices
		std::unordered_map<int64_t, FallPlunge> plunges;
		for (const auto& vertex : mesh.vertices) {
			const Flow flow = flowAt(vertex.position[0], vertex.position[1]);
			// D45: the pool's own level under the vertex (the engine's water), not only the site's paired levels
			float nearestLevel = FLT_MAX;
			{
				const auto cx = static_cast<int32_t>(std::lround((vertex.position[0] - a_originX) / cell));
				const auto cy = static_cast<int32_t>(std::lround((vertex.position[1] - a_originY) / cell));
				for (int32_t dy = -1; dy <= 1; dy++) {
					for (int32_t dx = -1; dx <= 1; dx++) {
						const int32_t x = cx + dx, y = cy + dy;
						if (x < 0 || y < 0 || x >= w || y >= h || a_flow.cover.size() != static_cast<std::size_t>(w) * h)
							continue;
						const float level = a_flow.cover[static_cast<std::size_t>(y) * w + x];
						if (level > NO_SURFACE && std::abs(vertex.position[2] - level) < std::abs(vertex.position[2] - nearestLevel))
							nearestLevel = level;
					}
				}
			}
			for (const float level : a_levels) {
				if (level < topLevel - 1.0f && std::abs(vertex.position[2] - level) < std::abs(vertex.position[2] - nearestLevel))
					nearestLevel = level;
			}
			const bool atLevel = std::abs(vertex.position[2] - nearestLevel) < PLUNGE_BAND;
			const float fallen = flow.head - vertex.position[2];
			// D46: or steep water reaching the level (where the solve is slow, its head says little)
			const bool steepIn = vertex.normal[2] < PLUNGE_STEEP;
			if (atLevel && (fallen > PLUNGE_HEAD || steepIn)) {
				const float strength = std::clamp(std::max(fallen / (PLUNGE_HEAD * 4.0f), steepIn ? 0.6f : 0.0f), 0.25f, 1.0f);
				offer(vertex.position, vertex.normal, strength, FallEmitterKind::Plunge, PLUNGE_SPACING);
				const auto key = (static_cast<int64_t>(std::floor(vertex.position[0] / PLUNGE_WATER_SPACING)) << 32) ^
				                 static_cast<uint32_t>(static_cast<int32_t>(std::floor(vertex.position[1] / PLUNGE_WATER_SPACING)));
				auto [it, added] = plunges.try_emplace(key);
				if (added || it->second.strength < strength) {
					FallPlunge& plunge = it->second;
					for (int k = 0; k < 3; k++)
						plunge.position[k] = vertex.position[k];
					plunge.position[2] = nearestLevel;
					float direction[3];
					plunge.speed = motionAt(vertex.position, vertex.normal, direction);
					const float horizontal = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1]);
					plunge.direction[0] = horizontal > 1e-3f ? direction[0] / horizontal : 0.0f;
					plunge.direction[1] = horizontal > 1e-3f ? direction[1] / horizontal : 0.0f;
					plunge.strength = strength;
				}
			}
		}

		// D49: impacts, where falling water lands: a flatter vertex with steep water just upstream of it,
		// standing well above it. Each throws spray up and out by how far the water fell, whitens the falls
		// round it and makes fog, as vanilla's foam cards there pretended to
		std::vector<std::array<float, 6>> impacts;  // Position, strength, unused x2
		{
			std::vector<float> cellSteep(static_cast<std::size_t>(w) * h, 1.0f);
			std::vector<float> cellTop(static_cast<std::size_t>(w) * h, -FLT_MAX);
			const auto cellOf = [&](const float a_p[3], int32_t& a_x, int32_t& a_y) {
				a_x = static_cast<int32_t>(std::floor((a_p[0] - a_originX) / cell));
				a_y = static_cast<int32_t>(std::floor((a_p[1] - a_originY) / cell));
				return a_x >= 0 && a_y >= 0 && a_x < w && a_y < h;
			};
			for (const auto& vertex : mesh.vertices) {
				int32_t x, y;
				if (!cellOf(vertex.position, x, y) || buried(vertex.position, true))
					continue;
				const std::size_t i = static_cast<std::size_t>(y) * w + x;
				cellSteep[i] = std::min(cellSteep[i], vertex.normal[2]);
				cellTop[i] = std::max(cellTop[i], vertex.position[2]);
			}
			std::unordered_map<int64_t, std::array<float, 6>> best;
			for (const auto& vertex : mesh.vertices) {
				if (vertex.normal[2] < IMPACT_FLAT)
					continue;
				int32_t x, y;
				if (!cellOf(vertex.position, x, y) || buried(vertex.position, true))
					continue;
				const Flow flow = flowAt(vertex.position[0], vertex.position[1]);
				const float speed = std::sqrt(flow.vx * flow.vx + flow.vy * flow.vy);
				// Upstream: against the flow, or every way round where the flow says little
				float drop = 0.0f;
				for (int32_t dy = -IMPACT_CELLS; dy <= IMPACT_CELLS; dy++) {
					for (int32_t dx = -IMPACT_CELLS; dx <= IMPACT_CELLS; dx++) {
						const int32_t nx = x + dx, ny = y + dy;
						if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= w || ny >= h)
							continue;
						if (speed > 0.05f && (static_cast<float>(dx) * flow.vx + static_cast<float>(dy) * flow.vy) > 0.0f)
							continue;  // Downstream
						const std::size_t j = static_cast<std::size_t>(ny) * w + nx;
						if (cellSteep[j] < IMPACT_STEEP)
							drop = std::max(drop, cellTop[j] - vertex.position[2]);
					}
				}
				if (drop < IMPACT_DROP)
					continue;
				const float strength = std::clamp(drop / (IMPACT_DROP * 4.0f), 0.35f, 1.0f);
				const auto key = (static_cast<int64_t>(std::floor(vertex.position[0] / IMPACT_SPACING)) << 32) ^
				                 static_cast<uint32_t>(static_cast<int32_t>(std::floor(vertex.position[1] / IMPACT_SPACING)));
				auto [it, added] = best.try_emplace(key);
				if (added || it->second[3] < strength)
					it->second = { vertex.position[0], vertex.position[1], vertex.position[2], strength, 0.0f, 0.0f };
			}
			for (const auto& [key, impact] : best)
				impacts.push_back(impact);  // D52: foam only; the spray comes from rock hits and seams
			mesh.impacts = static_cast<uint32_t>(impacts.size());
		}
		for (const auto& [key, plunge] : plunges)
			mesh.plunges.push_back(plunge);

		// D52: rock hits, where the falls strike something that is not water: an edge of vanilla's falls
		// that runs from above the captured rock into it. Spray thrown by the water's speed there, foam round it
		std::vector<std::array<float, 6>> rockHits;
		{
			const auto bedAt = [&](float a_x, float a_y, float& a_bed) {
				const auto x = static_cast<int32_t>(std::lround((a_x - a_originX) / cell));
				const auto y = static_cast<int32_t>(std::lround((a_y - a_originY) / cell));
				if (x < 0 || y < 0 || x >= w || y >= h || a_flow.bed.size() != static_cast<std::size_t>(w) * h)
					return false;
				a_bed = a_flow.bed[static_cast<std::size_t>(y) * w + x];
				return a_bed > FallCapture::NONE;
			};
			std::unordered_map<int64_t, std::array<float, 6>> best;
			for (const auto& triangle : a_sources) {
				for (int k = 0; k < 3; k++) {
					const float* a = triangle.position[k];
					const float* b = triangle.position[(k + 1) % 3];
					float bedA, bedB;
					if (!bedAt(a[0], a[1], bedA) || !bedAt(b[0], b[1], bedB))
						continue;
					const float above = a[2] - bedA, belowB = b[2] - bedB;
					// One end on or above the rock, the other inside it: the crossing is where the water hits
					const bool aOut = above > -ROCK_HIT_MARGIN, bOut = belowB > -ROCK_HIT_MARGIN;
					if (aOut == bOut || std::min(above, belowB) > -ROCK_HIT_DEPTH)
						continue;
					const float s = std::clamp(above / (above - belowB), 0.0f, 1.0f);
					float p[3];
					for (int c = 0; c < 3; c++)
						p[c] = a[c] + (b[c] - a[c]) * s;
					if (buried(p, true))
						continue;
					const Flow flow = flowAt(p[0], p[1]);
					const float fallen = std::max(flow.head - p[2], 0.0f);
					const float strength = std::clamp(0.4f + fallen / 200.0f, 0.4f, 1.0f);
					const auto key = (static_cast<int64_t>(std::floor(p[0] / ROCK_HIT_SPACING)) << 32) ^
					                 static_cast<uint32_t>(static_cast<int32_t>(std::floor(p[1] / ROCK_HIT_SPACING))) ^ (static_cast<int64_t>(std::floor(p[2] / ROCK_HIT_SPACING)) << 48);
					auto [it, added] = best.try_emplace(key);
					if (added || it->second[3] < strength)
						it->second = { p[0], p[1], p[2], strength, triangle.normal[k][0] + triangle.normal[(k + 1) % 3][0], triangle.normal[k][1] + triangle.normal[(k + 1) % 3][1] };
				}
			}
			for (const auto& [key, hit] : best) {
				rockHits.push_back(hit);
				const float n[3] = { 0.0f, 0.0f, 1.0f };
				offer(hit.data(), n, hit[3], FallEmitterKind::Plunge, ROCK_HIT_SPACING);
			}
		}

		// Where vanilla's pieces cross each water level, for the debug view (D36)
		for (const auto& triangle : a_sources) {
			for (const float level : a_levels) {
				float points[2][3];
				int count = 0;
				for (int k = 0; k < 3 && count < 2; k++) {
					const float* a = triangle.position[k];
					const float* b = triangle.position[(k + 1) % 3];
					if ((a[2] - level) * (b[2] - level) >= 0.0f)
						continue;
					const float s = (level - a[2]) / (b[2] - a[2]);
					for (int c = 0; c < 3; c++)
						points[count][c] = a[c] + (b[c] - a[c]) * s;
					count++;
				}
				if (count == 2)
					mesh.waterLines.push_back({ points[0][0], points[0][1], points[0][2], points[1][0], points[1][1], points[1][2] });
			}
		}

		// The seams (D34): where one of vanilla's pieces ends against another, the join shows on their own
		// meshes. Points along each piece's open edges (used by one of its triangles) that lie close to
		// another piece foam on the flow's map and throw spray and mist
		{
			const auto quantize = [](const float a_p[3]) {
				const auto qx = static_cast<int64_t>(std::lround(a_p[0] * 2.0f)) & 0x1FFFFF;
				const auto qy = static_cast<int64_t>(std::lround(a_p[1] * 2.0f)) & 0x1FFFFF;
				const auto qz = static_cast<int64_t>(std::lround(a_p[2] * 2.0f)) & 0x1FFFFF;
				return (qx << 42) | (qy << 21) | qz;
			};
			struct EdgeUse
			{
				uint32_t triangle;
				int corner;
				uint32_t uses;
			};
			std::map<std::tuple<uint32_t, int64_t, int64_t>, EdgeUse> edges;
			for (uint32_t t = 0; t < a_sources.size(); t++) {
				const auto& triangle = a_sources[t];
				for (int k = 0; k < 3; k++) {
					int64_t a = quantize(triangle.position[k]), b = quantize(triangle.position[(k + 1) % 3]);
					if (a > b)
						std::swap(a, b);
					auto [it, added] = edges.try_emplace({ triangle.piece, a, b }, EdgeUse{ t, k, 0 });
					it->second.uses++;
				}
			}
			// Every piece's triangles, by area, reach included
			std::unordered_map<int64_t, std::vector<uint32_t>> nearby;
			for (uint32_t t = 0; t < a_sources.size(); t++) {
				const auto& triangle = a_sources[t];
				float lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
				for (int k = 0; k < 3; k++) {
					for (int a = 0; a < 2; a++) {
						lo[a] = std::min(lo[a], triangle.position[k][a] - SEAM_REACH);
						hi[a] = std::max(hi[a], triangle.position[k][a] + SEAM_REACH);
					}
				}
				for (auto by = static_cast<int32_t>(std::floor(lo[1] / MERGE_BUCKET)); by <= static_cast<int32_t>(std::floor(hi[1] / MERGE_BUCKET)); by++) {
					for (auto bx = static_cast<int32_t>(std::floor(lo[0] / MERGE_BUCKET)); bx <= static_cast<int32_t>(std::floor(hi[0] / MERGE_BUCKET)); bx++)
						nearby[bucketKey(bx, by)].push_back(t);
				}
			}
			// Squared distance from a point to a triangle (closest point by region)
			const auto distance2 = [](const float a_p[3], const SourceTriangle& a_t) {
				const auto sub3 = [](const float* a, const float* b, float* r) { r[0] = a[0] - b[0]; r[1] = a[1] - b[1]; r[2] = a[2] - b[2]; };
				const auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
				const float* a = a_t.position[0];
				const float* b = a_t.position[1];
				const float* c = a_t.position[2];
				float ab[3], ac[3], ap[3], bp[3], cp[3], closest[3];
				sub3(b, a, ab);
				sub3(c, a, ac);
				sub3(a_p, a, ap);
				const auto at = [&](float a_v, float a_w) {
					for (int k = 0; k < 3; k++)
						closest[k] = a[k] + ab[k] * a_v + ac[k] * a_w;
				};
				const float d1 = dot3(ab, ap), d2 = dot3(ac, ap);
				sub3(a_p, b, bp);
				const float d3 = dot3(ab, bp), d4 = dot3(ac, bp);
				sub3(a_p, c, cp);
				const float d5 = dot3(ab, cp), d6 = dot3(ac, cp);
				const float va = d3 * d6 - d5 * d4, vb = d5 * d2 - d1 * d6, vc = d1 * d4 - d3 * d2;
				if (d1 <= 0.0f && d2 <= 0.0f)
					at(0.0f, 0.0f);
				else if (d3 >= 0.0f && d4 <= d3)
					at(1.0f, 0.0f);
				else if (d6 >= 0.0f && d5 <= d6)
					at(0.0f, 1.0f);
				else if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
					at(d1 / (d1 - d3), 0.0f);
				else if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
					at(0.0f, d2 / (d2 - d6));
				else if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
					const float k = (d4 - d3) / ((d4 - d3) + (d5 - d6));
					at(1.0f - k, k);
				} else {
					const float denominator = 1.0f / (va + vb + vc);
					at(vb * denominator, vc * denominator);
				}
				float d[3];
				sub3(a_p, closest, d);
				return dot3(d, d);
			};
			std::vector<std::array<float, 6>> seams;  // Position, normal
			std::vector<uint8_t> seamKinds;           // 0 an open edge against another piece, 1 two pieces crossing
			for (const auto& [key, use] : edges) {
				if (use.uses != 1)
					continue;
				const auto& triangle = a_sources[use.triangle];
				const float* from = triangle.position[use.corner];
				const float* to = triangle.position[(use.corner + 1) % 3];
				const float length = std::sqrt((to[0] - from[0]) * (to[0] - from[0]) + (to[1] - from[1]) * (to[1] - from[1]) + (to[2] - from[2]) * (to[2] - from[2]));
				const auto steps = std::max(1, static_cast<int>(std::ceil(length / SEAM_STEP)));
				for (int s = 0; s <= steps; s++) {
					const float k = static_cast<float>(s) / static_cast<float>(steps);
					const float p[3] = { from[0] + (to[0] - from[0]) * k, from[1] + (to[1] - from[1]) * k, from[2] + (to[2] - from[2]) * k };
					const auto it = nearby.find(bucketKey(static_cast<int32_t>(std::floor(p[0] / MERGE_BUCKET)), static_cast<int32_t>(std::floor(p[1] / MERGE_BUCKET))));
					if (it == nearby.end())
						continue;
					bool seam = false;
					for (const uint32_t other : it->second) {
						if (a_sources[other].piece != triangle.piece && distance2(p, a_sources[other]) < SEAM_REACH * SEAM_REACH) {
							seam = true;
							break;
						}
					}
					if (!seam)
						continue;
					if (buried(p, true))
						continue;
					const float* n0 = triangle.normal[use.corner];
					const float* n1 = triangle.normal[(use.corner + 1) % 3];
					seams.push_back({ p[0], p[1], p[2], n0[0] + n1[0], n0[1] + n1[1], n0[2] + n1[2] });
					seamKinds.push_back(0);
				}
			}
			// ...and where two pieces cut through each other: the line where their triangles intersect
			{
				const auto segmentHits = [](const float* a_from, const float* a_to, const SourceTriangle& a_t, float a_hit[3]) {
					const float* p0 = a_t.position[0];
					float e1[3], e2[3], d[3], s[3], q[3], pv[3];
					for (int k = 0; k < 3; k++) {
						e1[k] = a_t.position[1][k] - p0[k];
						e2[k] = a_t.position[2][k] - p0[k];
						d[k] = a_to[k] - a_from[k];
						s[k] = a_from[k] - p0[k];
					}
					pv[0] = d[1] * e2[2] - d[2] * e2[1];
					pv[1] = d[2] * e2[0] - d[0] * e2[2];
					pv[2] = d[0] * e2[1] - d[1] * e2[0];
					const float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
					if (std::abs(det) < 1e-6f)
						return false;
					const float inverse = 1.0f / det;
					const float u = (s[0] * pv[0] + s[1] * pv[1] + s[2] * pv[2]) * inverse;
					if (u < 0.0f || u > 1.0f)
						return false;
					q[0] = s[1] * e1[2] - s[2] * e1[1];
					q[1] = s[2] * e1[0] - s[0] * e1[2];
					q[2] = s[0] * e1[1] - s[1] * e1[0];
					const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inverse;
					if (v < 0.0f || u + v > 1.0f)
						return false;
					const float along = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inverse;
					if (along < 0.0f || along > 1.0f)
						return false;
					for (int k = 0; k < 3; k++)
						a_hit[k] = a_from[k] + d[k] * along;
					return true;
				};
				std::vector<uint32_t> seen(a_sources.size(), UINT32_MAX);
				for (uint32_t t = 0; t < a_sources.size(); t++) {
					const auto& first = a_sources[t];
					float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
					for (int k = 0; k < 3; k++) {
						for (int c = 0; c < 3; c++) {
							lo[c] = std::min(lo[c], first.position[k][c]);
							hi[c] = std::max(hi[c], first.position[k][c]);
						}
					}
					for (auto by = static_cast<int32_t>(std::floor(lo[1] / MERGE_BUCKET)); by <= static_cast<int32_t>(std::floor(hi[1] / MERGE_BUCKET)); by++) {
						for (auto bx = static_cast<int32_t>(std::floor(lo[0] / MERGE_BUCKET)); bx <= static_cast<int32_t>(std::floor(hi[0] / MERGE_BUCKET)); bx++) {
							const auto it = nearby.find(bucketKey(bx, by));
							if (it == nearby.end())
								continue;
							for (const uint32_t other : it->second) {
								if (other <= t || seen[other] == t || a_sources[other].piece == first.piece)
									continue;
								seen[other] = t;
								const auto& second = a_sources[other];
								bool apart = false;
								for (int c = 0; c < 3 && !apart; c++) {
									const float olo = std::min({ second.position[0][c], second.position[1][c], second.position[2][c] });
									const float ohi = std::max({ second.position[0][c], second.position[1][c], second.position[2][c] });
									apart = ohi < lo[c] || olo > hi[c];
								}
								if (apart)
									continue;
								// Pieces lying along each other (layers of the same water) show no seam where they cross
								const auto faceNormal = [](const SourceTriangle& a_t, float a_n[3]) {
									const float ax = a_t.position[1][0] - a_t.position[0][0], ay = a_t.position[1][1] - a_t.position[0][1], az = a_t.position[1][2] - a_t.position[0][2];
									const float bx = a_t.position[2][0] - a_t.position[0][0], by = a_t.position[2][1] - a_t.position[0][1], bz = a_t.position[2][2] - a_t.position[0][2];
									a_n[0] = ay * bz - az * by;
									a_n[1] = az * bx - ax * bz;
									a_n[2] = ax * by - ay * bx;
									const float length = std::sqrt(a_n[0] * a_n[0] + a_n[1] * a_n[1] + a_n[2] * a_n[2]);
									for (int c = 0; c < 3; c++)
										a_n[c] = length > 1e-6f ? a_n[c] / length : 0.0f;
								};
								float na[3], nb[3];
								faceNormal(first, na);
								faceNormal(second, nb);
								if (std::abs(na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2]) > SEAM_CROSSING_ANGLE)
									continue;
								float hits[6][3];
								int count = 0;
								for (int k = 0; k < 3; k++) {
									if (segmentHits(first.position[k], first.position[(k + 1) % 3], second, hits[count]))
										count++;
									if (segmentHits(second.position[k], second.position[(k + 1) % 3], first, hits[count]))
										count++;
								}
								if (count == 0)
									continue;
								// The intersection runs between the two hits farthest apart
								int from = 0, to = 0;
								float farthest = -1.0f;
								for (int i = 0; i < count; i++) {
									for (int j = i; j < count; j++) {
										const float dx = hits[i][0] - hits[j][0], dy = hits[i][1] - hits[j][1], dz = hits[i][2] - hits[j][2];
										const float distance = dx * dx + dy * dy + dz * dz;
										if (distance > farthest) {
											farthest = distance;
											from = i;
											to = j;
										}
									}
								}
								const auto steps = std::max(1, static_cast<int>(std::ceil(std::sqrt(farthest) / SEAM_STEP)));
								for (int s = 0; s <= steps; s++) {
									const float k = static_cast<float>(s) / static_cast<float>(steps);
									std::array<float, 6> seam{};
									for (int c = 0; c < 3; c++) {
										seam[c] = hits[from][c] + (hits[to][c] - hits[from][c]) * k;
										seam[3 + c] = first.normal[0][c] + first.normal[1][c] + first.normal[2][c];
									}
									if (buried(seam.data(), true))
										continue;
									seams.push_back(seam);
									seamKinds.push_back(1);
								}
							}
						}
					}
				}
			}
			for (const auto& impact : impacts) {
				seams.push_back({ impact[0], impact[1], impact[2], 0.0f, 0.0f, 1.0f });
				seamKinds.push_back(2);
			}
			for (const auto& hit : rockHits) {
				seams.push_back({ hit[0], hit[1], hit[2], 0.0f, 0.0f, 1.0f });
				seamKinds.push_back(2);  // Foam round it; it threw its own spray
			}
			mesh.seams = static_cast<uint32_t>(seams.size());
			for (std::size_t i = 0; i < seams.size(); i++)
				mesh.seamPoints.push_back({ seams[i][0], seams[i][1], seams[i][2], static_cast<float>(seamKinds[i]) });

			// Foam on the flow's map, round every seam point
			mesh.seamFoam.assign(static_cast<std::size_t>(w) * h, 0.0f);
			const auto reach = static_cast<int32_t>(std::ceil(SEAM_FOAM_RADIUS / cell));
			for (const auto& seam : seams) {
				const auto cx = static_cast<int32_t>(std::floor((seam[0] - a_originX) / cell));
				const auto cy = static_cast<int32_t>(std::floor((seam[1] - a_originY) / cell));
				for (int32_t y = cy - reach; y <= cy + reach; y++) {
					for (int32_t x = cx - reach; x <= cx + reach; x++) {
						if (x < 0 || y < 0 || x >= w || y >= h)
							continue;
						const float ox = a_originX + (static_cast<float>(x) + 0.5f) * cell - seam[0];
						const float oy = a_originY + (static_cast<float>(y) + 0.5f) * cell - seam[1];
						const float falloff = std::clamp(1.0f - std::sqrt(ox * ox + oy * oy) / SEAM_FOAM_RADIUS, 0.0f, 1.0f);
						const float foam = std::min(falloff * 1.8f, 1.0f);  // Solid along the seam (D47)
						float& value = mesh.seamFoam[static_cast<std::size_t>(y) * w + x];
						value = std::max(value, foam);
					}
				}
			}

			// Spray and mist along the seams, where the water strikes the other piece (less where two cross)
			for (std::size_t i = 0; i < seams.size(); i++) {
				if (seamKinds[i] == 2)
					continue;  // An impact: it threw its own
				const auto& seam = seams[i];
				float normal[3] = { seam[3], seam[4], seam[5] };
				const float n = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
				for (float& component : normal)
					component = n > 1e-6f ? component / n : 0.0f;
				if (n <= 1e-6f)
					normal[2] = 1.0f;
				const float p[3] = { seam[0], seam[1], seam[2] };
				offer(p, normal, seamKinds[i] == 0 ? 1.0f : 0.7f, FallEmitterKind::Strike, EMITTER_SPACING);  // D52: (b) the falls' pieces clashing
			}
		}
		mesh.emitters.reserve(chosen.size());
		for (const auto& [key, emitter] : chosen)
			mesh.emitters.push_back(emitter);
		return mesh;
	}

	void FallSites::RefreshSurfaces()
	{
		surfaceVertices.clear();
		surfaceEmitters.clear();
		surfacePlunges.clear();
		for (const auto& [key, stream] : streams) {
			if (!stream.meshReady || stream.vertices.empty())
				continue;
			surfaceVertices.insert(surfaceVertices.end(), stream.vertices.begin(), stream.vertices.end());
			surfaceEmitters.insert(surfaceEmitters.end(), stream.emitters.begin(), stream.emitters.end());
			surfacePlunges.insert(surfacePlunges.end(), stream.plunges.begin(), stream.plunges.end());
		}
		surfaceVersion++;
	}

	void FallSites::AppendWaterEmitters(std::vector<WaterEmitter>& a_emitters, const int32_t a_gridMin[2], float a_cellSize, uint32_t a_gridRes,
		float a_strength, uint32_t a_maxEmitters, bool a_foam) const
	{
		if (a_strength <= 0.0f || !(a_cellSize > 0.0f) || surfacePlunges.empty() || a_emitters.size() >= a_maxEmitters)
			return;
		// Nearest the grid's centre first
		const float centre = 0.5f * static_cast<float>(a_gridRes);
		std::vector<std::pair<float, const FallPlunge*>> order;
		order.reserve(surfacePlunges.size());
		for (const auto& plunge : surfacePlunges) {
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

	bool FallSites::GetTextureScale(const RE::BSGeometry* a_geometry, float a_scale[2]) const
	{
		const auto it = textureScales.find(a_geometry);
		if (it == textureScales.end())
			return false;
		a_scale[0] = it->second[0];
		a_scale[1] = it->second[1];
		return true;
	}

	void FallSites::BuildStreamMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_field) const
	{
		a_field.assign(static_cast<std::size_t>(a_size) * a_size * 4, 0.0f);
		const auto n = static_cast<int32_t>(a_size);
		for (const auto& [key, stream] : streams) {
			if (!stream.ready)
				continue;
			const auto& result = stream.result;
			const auto w = static_cast<int32_t>(result.width);
			const auto h = static_cast<int32_t>(result.height);
			// Both grids sit on the same world lattice, a_cell (FallCapture::CELL) apart
			const auto offsetX = static_cast<int32_t>(std::lround((stream.originX - a_originX) / a_cell));
			const auto offsetY = static_cast<int32_t>(std::lround((stream.originY - a_originY) / a_cell));
			for (int32_t y = std::max(0, -offsetY); y < h && y + offsetY < n; y++) {
				for (int32_t x = std::max(0, -offsetX); x < w && x + offsetX < n; x++) {
					const std::size_t from = static_cast<std::size_t>(y) * w + x;
					const std::size_t to = (static_cast<std::size_t>(y + offsetY) * a_size + static_cast<std::size_t>(x + offsetX)) * 4;
					if (!result.water[from] || a_field[to + 2] >= 2.0f)
						continue;
					a_field[to + 0] = result.vx[from];
					a_field[to + 1] = result.vy[from];
					const float seam = stream.seamFoam.size() == result.water.size() ? stream.seamFoam[from] : 0.0f;
					a_field[to + 2] = 2.0f + std::clamp(std::max(result.foam[from], seam), 0.0f, 1.0f);  // 2 and up: water
					a_field[to + 3] = result.head.size() == result.water.size() ? result.head[from] : result.level[from];
				}
			}
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
		RestoreAlpha();
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
				// D54: the lit surface has no alpha property of its own; its model's layers blend SrcAlpha /
				// InvSrcAlpha, and lend it theirs
				RE::NiAlphaProperty* lend = nullptr;
				RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
					auto* alpha = a_geometry->GetGeometryRuntimeData().alphaProperty.get();
					if (alpha && alpha->GetAlphaBlending() && !alpha->GetAlphaTesting() && alpha->GetSrcBlendMode() == RE::NiAlphaProperty::AlphaFunction::kSrcAlpha &&
						alpha->GetDestBlendMode() == RE::NiAlphaProperty::AlphaFunction::kInvSrcAlpha) {
						lend = alpha;
						return RE::BSVisit::BSVisitControl::kStop;
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
				RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
					// D61: our own sheets (FallSheets) are drawn as they are
					if (a_geometry->name == FallSheets::SHEET_NAME)
						return RE::BSVisit::BSVisitControl::kContinue;
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
						litMeshList.push_back({ RE::NiPointer<RE::BSGeometry>(a_geometry), site.bottom, site.id, site.levels });
						if (!vertexClipBase.contains(a_geometry) &&
							std::none_of(clipQueue.begin(), clipQueue.end(), [&](const RE::NiPointer<RE::BSGeometry>& a_queued) { return a_queued.get() == a_geometry; }))
							clipQueue.emplace_back(a_geometry);
						if (!bakeFields.contains(a_geometry) && (!bakeJob || bakeJob->geometry.get() != a_geometry) &&
							std::none_of(bakeQueue.begin(), bakeQueue.end(), [&](const RE::NiPointer<RE::BSGeometry>& a_queued) { return a_queued.get() == a_geometry; }) &&
							std::none_of(bakesPending.begin(), bakesPending.end(), [&](const auto& a_pending) { return a_pending.first.get() == a_geometry; }))
							bakeQueue.emplace_back(a_geometry);
					}
					// D53: the lit surface drawn blended, so it can be translucent in patches, as falling water is.
					// Its own alpha property is switched (the model's, shared by every copy of it) and restored after
					auto* lit = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
					auto* alphaProperty = a_geometry->GetGeometryRuntimeData().alphaProperty.get();
					if (translucent && triShape && lit && netimmerse_cast<RE::BSLightingShaderProperty*>(lit) && alphaProperty) {
						blendedAlpha.try_emplace(alphaProperty, RE::NiPointer<RE::NiAlphaProperty>(alphaProperty), alphaProperty->alphaFlags);
						alphaProperty->SetAlphaBlending(true);
						alphaProperty->SetSrcBlendMode(RE::NiAlphaProperty::AlphaFunction::kSrcAlpha);
						alphaProperty->SetDestBlendMode(RE::NiAlphaProperty::AlphaFunction::kInvSrcAlpha);
						translucentGeometry.insert(a_geometry);
					} else if (translucent && triShape && lit && netimmerse_cast<RE::BSLightingShaderProperty*>(lit) && !alphaProperty && lend) {
						a_geometry->GetGeometryRuntimeData().alphaProperty = RE::NiPointer<RE::NiAlphaProperty>(lend);
						borrowedAlpha.emplace_back(a_geometry);
						translucentGeometry.insert(a_geometry);
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
							extra += std::format(", alpha flags {:04X} (blend {}, test {}), threshold {}", blendedAlpha.contains(alpha) ? blendedAlpha.at(alpha).second : alpha->alphaFlags,
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
		std::erase_if(bakeFields, [&](const auto& a_entry) { return !litMeshes.contains(a_entry.first); });
		std::erase_if(chainRoot, [&](const auto& a_entry) { return !litMeshes.contains(a_entry.first) || !litMeshes.contains(a_entry.second); });
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

	float FallSites::RockClip(RE::hkpWorld* a_world, const RE::NiPoint3& a_point, const RE::NiPoint3& a_normal, bool& a_inside) const
	{
		// D57: inside rock where rays from outside it (straight down onto the point, down and in, and in along
		// its face) all cross rock an odd number of times on the way. D56 asked which way the rock's face
		// turned at the first hit; Havok's normals never said "from behind", so nothing was ever inside. All
		// three must agree: under an overhang the ray from above crosses it, the one along the face does not
		RE::NiPoint3 out = a_normal;
		if (out.z < 0.0f)
			out = -out;
		const RE::NiPoint3 directions[3] = { { 0.0f, 0.0f, 1.0f }, RE::NiPoint3(out.x, out.y, out.z + 1.0f), out };
		int inside = 0, tried = 0;
		float depth = CLIP_RANGE;
		for (int k = 0; k < 3; k++) {
			RE::NiPoint3 direction = directions[k];
			const float length = direction.Length();
			if (length < 1e-4f)
				continue;
			direction = direction / length;
			// From outside, toward the point: the last crossing is the rock's face nearest the point
			int crossings = 0;
			float last = 0.0f;
			RockAlong(a_world, a_point + direction * CLIP_REACH, -direction, CLIP_REACH - 0.5f, &crossings, &last);
			tried++;
			if (crossings % 2 == 1) {
				inside++;
				if (k == 0)
					depth = std::min(CLIP_REACH - last, CLIP_RANGE);
			}
		}
		a_inside = tried > 0 && inside == tried;
		// The cut sits just short of a buried vertex (2 against 38): Bethesda's triangles are large, and a deep
		// value pulled the cut far across them, out of the rock, notching black gaps along it
		(void)depth;
		return a_inside ? -CLIP_INSIDE : CLIP_MARGIN + CLIP_RANGE;
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
			std::vector<RE::NiPoint3> normals(counts.vertexCount, RE::NiPoint3(0.0f, 0.0f, 0.0f));
			for (uint32_t i = 0; i < counts.triangleCount; i++) {
				const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(i) * 3;
				if (index[0] >= counts.vertexCount || index[1] >= counts.vertexCount || index[2] >= counts.vertexCount)
					continue;
				const RE::NiPoint3 face = (positions[index[1]] - positions[index[0]]).Cross(positions[index[2]] - positions[index[0]]);
				for (int k = 0; k < 3; k++)
					normals[index[k]] += face;
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
			{
				const RE::BSReadLockGuard lock(bhkWorld->worldLock);
				for (uint32_t v = 0; v < counts.vertexCount; v++) {
					const float length = normals[v].Length();
					bool inside = false;
					float clip = ROCK_CUT ? RockClip(world, positions[v], length > 1e-6f ? normals[v] / length : RE::NiPoint3(0.0f, 0.0f, 1.0f), inside) : CLIP_MARGIN + CLIP_RANGE;
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
					clipInside += inside ? 1 : 0;
				}
			}
			vertexClipBase[geometry.get()] = base;
			vertexClipVersion++;
		}
		if (clipQueue.empty()) {
			logger::info("[Fluid Field] Waterfall rock cut: {} vertices of {} lit meshes tested, {} inside rock", clipTested, vertexClipBase.size(), clipInside);
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

	bool FallSites::InsideSolid(const BakeJob& a_job, const RE::NiPoint3& a_point)
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
			const uint32_t ref = a_job.solidRef[t];
			auto it = std::find_if(crossings.begin(), crossings.end(), [&](const auto& a_entry) { return a_entry.first == ref; });
			if (it == crossings.end())
				crossings.emplace_back(ref, 1u);
			else
				it->second++;
		}
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

	bool FallSites::SiteFlowAt(float a_x, float a_y, float& a_vx, float& a_vy) const
	{
		for (const auto& [key, stream] : streams) {
			const auto& result = stream.result;
			if (!stream.ready || result.vx.size() != static_cast<std::size_t>(result.width) * result.height || result.water.size() != result.vx.size())
				continue;
			const auto x = static_cast<int32_t>(std::lround((a_x - stream.originX) / FallCapture::CELL));
			const auto y = static_cast<int32_t>(std::lround((a_y - stream.originY) / FallCapture::CELL));
			if (x < 0 || y < 0 || x >= static_cast<int32_t>(result.width) || y >= static_cast<int32_t>(result.height))
				continue;
			const std::size_t i = static_cast<std::size_t>(y) * result.width + x;
			if (!result.water[i])
				continue;
			a_vx = result.vx[i];
			a_vy = result.vy[i];
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
			if (a_job.covered[i] && !rock[i] && InsideSolid(a_job, a_job.position[i])) {
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
		const auto water = [&](int32_t a_x, int32_t a_y) { return inside(a_x, a_y) && a_job.covered[index(a_x, a_y)] && !rock[index(a_x, a_y)]; };

		// The grid as a sheet tilted down +v, solved (D28) round the rock: in at each column's first covered
		// texel, out at its last (the jagged sides are neither: D59 made sources and sinks of them)
		FallStream::Input input;
		input.width = w;
		input.height = h;
		input.cell = BAKE_TEXEL;
		input.surface.resize(count);
		input.bed.resize(count);
		input.kind.assign(count, FallStream::Wall);
		for (int32_t y = 0; y < ih; y++) {
			for (int32_t x = 0; x < iw; x++) {
				input.surface[index(x, y)] = -static_cast<float>(y) * BAKE_TEXEL;
				input.bed[index(x, y)] = input.surface[index(x, y)] - 100.0f;
			}
		}
		// D71: water leaves only near the mesh's foot: a column ending higher up ends against rock (vanilla cut its
		// sheet round a boulder or the bank), where it drained away and never turned; there it is a wall, and the
		// solve turns the water round it toward the foot
		float footZ = FLT_MAX, topZ = -FLT_MAX;
		for (std::size_t i = 0; i < count; i++) {
			if (a_job.covered[i]) {
				footZ = std::min(footZ, a_job.position[i].z);
				topZ = std::max(topZ, a_job.position[i].z);
			}
		}
		const float footBand = std::max(64.0f, (topZ - footZ) * 0.2f);
		bool anyOut = false;
		for (int32_t x = 0; x < iw && !anyOut; x++) {
			for (int32_t y = ih - 1; y >= 0; y--) {
				if (a_job.covered[index(x, y)]) {
					anyOut = a_job.position[index(x, y)].z < footZ + footBand;
					break;
				}
			}
		}
		for (int32_t x = 0; x < iw; x++) {
			int32_t first = -1, last = -1;
			for (int32_t y = 0; y < ih; y++) {
				if (!a_job.covered[index(x, y)])
					continue;
				first = first < 0 ? y : first;
				last = y;
			}
			const bool outHere = !anyOut || (last >= 0 && a_job.position[index(x, last)].z < footZ + footBand);
			for (int32_t y = std::max(first, 0); first >= 0 && y <= last; y++) {
				if (!water(x, y))
					continue;
				input.kind[index(x, y)] = y == first ? FallStream::Inflow : (y == last && outHere) ? FallStream::Outflow :
				                                                                                     FallStream::Water;
			}
		}
		const FallStream::Result solved = FallStream::Solve(input);
		const bool have = solved.vx.size() == count;

		// The turn per texel (across over down), clamped (never back, at most BAKE_MOST_TURN), and filled
		// into rock and off the mesh from the nearest water, so nothing jumps at a rock's edge
		std::vector<float> turn(count, 0.0f), foam(count, 0.0f), run(count, 0.0f), speed(count, 1.0f);
		std::vector<int32_t> from(count, -1);
		std::vector<std::size_t> frontier;
		for (std::size_t i = 0; i < count; i++) {
			const auto x = static_cast<int32_t>(i % w), y = static_cast<int32_t>(i / w);
			if (!have || !water(x, y) || !solved.water[i])
				continue;
			const float down = std::clamp(solved.vy[i], BAKE_LEAST_DOWN, BAKE_MOST_DOWN);
			turn[i] = std::clamp(solved.vx[i] / down, -BAKE_NEAR_TURN, BAKE_NEAR_TURN);
			// D67: and how fast it runs, against vanilla's (1)
			speed[i] = std::clamp(std::sqrt(solved.vx[i] * solved.vx[i] + solved.vy[i] * solved.vy[i]), BAKE_LEAST_SPEED, BAKE_MOST_DOWN);
			foam[i] = solved.rockFoam[i];
			run[i] = std::min(solved.travel[i], 60.0f);
			from[i] = static_cast<int32_t>(i);
			frontier.push_back(i);
		}
		for (std::size_t head = 0; head < frontier.size(); head++) {
			const std::size_t i = frontier[head];
			const auto x = static_cast<int32_t>(i % w), y = static_cast<int32_t>(i / w);
			static constexpr int32_t DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
			for (int k = 0; k < 4; k++) {
				const int32_t nx = x + DX[k], ny = y + DY[k];
				if (!inside(nx, ny) || from[index(nx, ny)] >= 0)
					continue;
				from[index(nx, ny)] = from[i];
				turn[index(nx, ny)] = turn[i];
				speed[index(nx, ny)] = speed[i];
				run[index(nx, ny)] = run[i];
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
		// Only round the rock: away from it the solve's sideways drift is the grid's ragged sides, not the
		// water's, and bent the whole sheet (D60's first run); full within BAKE_BEND_NEAR texels of rock, none past BAKE_BEND_FAR
		{
			std::vector<int32_t> steps(count, INT32_MAX);
			std::vector<std::size_t> open;
			for (std::size_t i = 0; i < count; i++) {
				if (rock[i] && a_job.covered[i]) {
					steps[i] = 0;
					open.push_back(i);
				}
			}
			for (std::size_t head = 0; head < open.size(); head++) {
				const std::size_t i = open[head];
				const auto x = static_cast<int32_t>(i % w), y = static_cast<int32_t>(i / w);
				static constexpr int32_t DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
				for (int k = 0; k < 4; k++) {
					const int32_t nx = x + DX[k], ny = y + DY[k];
					if (!inside(nx, ny) || steps[index(nx, ny)] <= steps[i] + 1)
						continue;
					steps[index(nx, ny)] = steps[i] + 1;
					open.push_back(index(nx, ny));
				}
			}
			for (std::size_t i = 0; i < count; i++) {
				const float closeness = std::clamp((BAKE_BEND_FAR - static_cast<float>(steps[i])) / (BAKE_BEND_FAR - BAKE_BEND_NEAR), 0.0f, 1.0f);
				const float bend = closeness * closeness * (3.0f - 2.0f * closeness);
				// D71: up to 45 degrees right beside rock, easing to BAKE_MOST_TURN away from it
				const float most = BAKE_MOST_TURN + (BAKE_NEAR_TURN - BAKE_MOST_TURN) * bend;
				turn[i] = std::clamp(turn[i], -most, most) * bend;
				speed[i] = 1.0f + (speed[i] - 1.0f) * bend;
			}
		}
		blur(turn, BAKE_TURN_BLUR);
		blur(speed, BAKE_TURN_BLUR);

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
		// D72: every mesh's texels in one grid of MEET_CELL world units (every other texel), with the way its water
		// goes in the world (down +v, turned by its solve), so meshes that meet can pull each other's flow
		struct Probe
		{
			uint32_t node;
			RE::NiPoint3 position, direction;
		};
		std::unordered_map<uint64_t, std::vector<Probe>> probes;
		const auto meetKey = [](float a_x, float a_y, float a_z) {
			const auto cx = static_cast<int64_t>(std::floor(a_x / MEET_CELL)) & 0x1FFFFF, cy = static_cast<int64_t>(std::floor(a_y / MEET_CELL)) & 0x1FFFFF,
					   cz = static_cast<int64_t>(std::floor(a_z / MEET_CELL)) & 0x1FFFFF;
			return static_cast<uint64_t>(cx | (cy << 21) | (cz << 42));
		};
		const auto worldFlow = [](const BakeField& a_field, const std::vector<float>& a_turn, int32_t a_x, int32_t a_y, RE::NiPoint3& a_tu, RE::NiPoint3& a_tv) {
			const auto w = static_cast<int32_t>(a_field.width), h = static_cast<int32_t>(a_field.height);
			const auto at = [&](int32_t a_px, int32_t a_py) { return static_cast<std::size_t>(std::clamp(a_py, 0, h - 1)) * w + std::clamp(a_px, 0, w - 1); };
			a_tu = a_field.position[at(a_x + 1, a_y)] - a_field.position[at(a_x - 1, a_y)];
			a_tv = a_field.position[at(a_x, a_y + 1)] - a_field.position[at(a_x, a_y - 1)];
			const RE::NiPoint3 direction = a_tv + a_tu * a_turn[at(a_x, a_y)];
			const float length = direction.Length();
			return length > 1e-4f ? direction / length : RE::NiPoint3(0.0f, 0.0f, 0.0f);
		};
		for (uint32_t n = 0; n < nodes.size(); n++) {
			const BakeField& field = *nodes[n].field;
			const auto w = static_cast<int32_t>(field.width), h = static_cast<int32_t>(field.height);
			for (int32_t y = 1; y + 1 < h; y += 2) {
				for (int32_t x = 1; x + 1 < w; x += 2) {
					const std::size_t i = static_cast<std::size_t>(y) * w + x;
					if (!field.covered[i])
						continue;
					RE::NiPoint3 tu, tv;
					const RE::NiPoint3 direction = worldFlow(field, field.turn, x, y, tu, tv);
					if (direction.Length() < 0.5f)
						continue;
					const auto& p = field.position[i];
					probes[meetKey(p.x, p.y, p.z)].push_back({ n, p, direction });
				}
			}
		}
		uint32_t met = 0;
		// Each mesh swept down from its top edge once its feeder is (meshes in a loop, if any, start as tops)
		uint32_t linked = 0, flatTexels = 0;
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
			// D68: where the sheet lies flat (water landed on the ground or a pool), the flow turns to the site's main
			// flow there (the solved flow seen from above, put in the sheet's texture axes), as far as FLAT_MOST_TURN:
			// water falling onto a slope runs off along it, not on down the mesh's +v
			std::vector<float> turn = field.turn;
			{
				const auto at = [&](int32_t a_x, int32_t a_y) { return static_cast<std::size_t>(std::clamp(a_y, 0, h - 1)) * w + std::clamp(a_x, 0, w - 1); };
				for (int32_t y = 0; y < h; y++) {
					for (int32_t x = 0; x < w; x++) {
						const std::size_t i = at(x, y);
						if (!field.covered[i])
							continue;
						const float flat = std::clamp((std::abs(field.normal[i].z) - 0.75f) / 0.15f, 0.0f, 1.0f);
						float vx, vy;
						// D70: only near the mesh's foot, where the water lands (on a crest it swirled the arch)
						const float foot = std::clamp((a_node.lo[2] + 2.0f * WATER_CUT_BAND - field.position[i].z) / WATER_CUT_BAND, 0.0f, 1.0f);
						if (flat * foot <= 0.0f || !SiteFlowAt(field.position[i].x, field.position[i].y, vx, vy) || vx * vx + vy * vy < 0.0025f)
							continue;
						const RE::NiPoint3 tu = field.position[at(x + 1, y)] - field.position[at(x - 1, y)];
						const RE::NiPoint3 tv = field.position[at(x, y + 1)] - field.position[at(x, y - 1)];
						const float det = tu.x * tv.y - tv.x * tu.y;
						if (std::abs(det) < 1e-3f)
							continue;
						const float a = (vx * tv.y - tv.x * vy) / det, b = (tu.x * vy - vx * tu.y) / det;
						const float wanted = b > 1e-3f ? std::clamp(a / b, -FLAT_MOST_TURN, FLAT_MOST_TURN) : (a >= 0.0f ? FLAT_MOST_TURN : -FLAT_MOST_TURN);
						// Only where the main flow leaves the mesh's own way down by more than 20 degrees: elsewhere the
						// mesh already runs with it, and a small turn everywhere only sheared the streaks into whorls
						const float apart = std::clamp((std::abs(wanted - turn[i]) - 0.36f) / 0.36f, 0.0f, 1.0f);
						const float weight = flat * flat * (3.0f - 2.0f * flat) * apart * foot;
						turn[i] += (wanted - turn[i]) * weight;
						flatTexels += weight > 0.5f ? 1 : 0;
					}
				}
				BlurGrid(turn, w, h, 6);
			}
			// D72: where another mesh's water runs within MEET_REACH, this mesh's flow is drawn to the way that water
			// goes (in this mesh's texture axes; not where it runs back against this one): flows that meet merge
			{
				const auto self = static_cast<uint32_t>(&a_node - nodes.data());
				std::vector<float> pulled = turn;
				for (int32_t y = 1; y + 1 < h; y++) {
					for (int32_t x = 1; x + 1 < w; x++) {
						const std::size_t i = static_cast<std::size_t>(y) * w + x;
						if (!field.covered[i])
							continue;
						const RE::NiPoint3& p = field.position[i];
						RE::NiPoint3 sum(0.0f, 0.0f, 0.0f);
						float weight = 0.0f;
						for (int dz = -1; dz <= 1; dz++) {
							for (int dy = -1; dy <= 1; dy++) {
								for (int dx = -1; dx <= 1; dx++) {
									const auto found = probes.find(meetKey(p.x + dx * MEET_CELL, p.y + dy * MEET_CELL, p.z + dz * MEET_CELL));
									if (found == probes.end())
										continue;
									for (const auto& probe : found->second) {
										if (probe.node == self)
											continue;
										const float distance = (probe.position - p).Length();
										if (distance >= MEET_REACH)
											continue;
										const float k = 1.0f - distance / MEET_REACH;
										sum += probe.direction * k;
										weight = std::max(weight, k);
									}
								}
							}
						}
						if (weight <= 0.0f || sum.Length() < 1e-3f)
							continue;
						RE::NiPoint3 tu, tv;
						worldFlow(field, turn, x, y, tu, tv);
						const RE::NiPoint3 direction = sum / sum.Length();
						// The direction in this sheet's axes: solve [tu.tu tu.tv; tu.tv tv.tv] [a b] = [d.tu d.tv]
						const float uu = tu.Dot(tu), uv = tu.Dot(tv), vv = tv.Dot(tv);
						const float det = uu * vv - uv * uv;
						if (std::abs(det) < 1e-6f)
							continue;
						const float du = direction.Dot(tu), dv = direction.Dot(tv);
						const float a = (du * vv - dv * uv) / det, b = (dv * uu - du * uv) / det;
						if (b < 0.2f * std::abs(a) || b <= 1e-4f)
							continue;
						const float wanted = std::clamp(a / b, -BAKE_NEAR_TURN, BAKE_NEAR_TURN);
						pulled[i] += (wanted - pulled[i]) * weight * MEET_PULL;
						met++;
					}
				}
				BlurGrid(pulled, w, h, 3);
				turn.swap(pulled);
			}
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
		chainRoot.clear();
		for (const auto& node : nodes) {
			const Node* root = &node;
			for (std::size_t guard = 0; root->feeder >= 0 && guard < nodes.size(); guard++)
				root = &nodes[root->feeder];
			chainRoot[node.geometry] = root->geometry;
		}
		flowBakeVersion++;
		linkedStreams = CountReadyStreams();
		logger::info("[Fluid Field] Waterfall flow linked: {} meshes, {} fed by the mesh above them; {} flat texels join the main flow; {} texels drawn by a mesh beside them ({} sites' flows solved)",
			nodes.size(), linked, flatTexels, met, linkedStreams);
	}

	void FallSites::BakeStep()
	{
		// D68: the flat parts follow the sites' flows, so the bakes are linked again as more of them are solved
		if (!bakeFields.empty() && bakesPending.empty() && bakeQueue.empty() && !bakeJob && CountReadyStreams() != linkedStreams)
			LinkBakes();
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
		PollStreams();
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
		// Flows of sites that are gone; a solve still running finishes in the background
		for (auto it = streams.begin(); it != streams.end();) {
			if (captures.contains(it->first)) {
				++it;
				continue;
			}
			if (it->second.pending.valid())
				retired.push_back(std::move(it->second.pending));
			if (it->second.meshPending.valid())
				retiredMeshes.push_back(std::move(it->second.meshPending));
			it = streams.erase(it);
		}
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

	bool FallSites::WaterObjectBounds(float a_x, float a_y, float a_level, float a_reach, float a_bounds[4]) const
	{
		if (!waterSurfaces)
			return false;
		std::vector<WaterSurfaces::Plane> planes;
		waterSurfaces->CollectPlanes(a_x - 1.0f, a_y - 1.0f, a_x + 1.0f, a_y + 1.0f, planes);
		bool found = false;
		float best = FLT_MAX;
		for (const auto& plane : planes) {
			if (plane.source != WaterSurfaces::Source::WaterObject || std::abs(plane.height - a_level) > a_reach)
				continue;
			if (a_x < plane.minX || a_x > plane.maxX || a_y < plane.minY || a_y > plane.maxY)
				continue;
			// The smallest that holds the point: the water there, not a wide one around it
			const float area = (plane.maxX - plane.minX) * (plane.maxY - plane.minY);
			if (area >= best)
				continue;
			best = area;
			a_bounds[0] = plane.minX;
			a_bounds[1] = plane.minY;
			a_bounds[2] = plane.maxX;
			a_bounds[3] = plane.maxY;
			found = true;
		}
		return found;
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

	bool FallSites::GetSiteGround(uint32_t a_siteId, SiteGround& a_ground) const
	{
		for (const auto& [key, stream] : streams) {
			const std::size_t count = static_cast<std::size_t>(stream.result.width) * stream.result.height;
			if (stream.siteId != a_siteId || !stream.ready || count == 0 || stream.result.bed.size() != count || stream.result.cover.size() != count)
				continue;
			a_ground = { stream.originX, stream.originY, FallCapture::CELL, stream.result.width, stream.result.height, &stream.result.bed, &stream.result.cover };
			return true;
		}
		return false;
	}

	bool FallSites::PoolLevelAt(float a_x, float a_y, float& a_level) const
	{
		bool found = false;
		for (const auto& [key, stream] : streams) {
			if (!stream.ready || stream.result.cover.size() != static_cast<std::size_t>(stream.result.width) * stream.result.height)
				continue;
			const auto x = static_cast<int32_t>(std::lround((a_x - stream.originX) / FallCapture::CELL));
			const auto y = static_cast<int32_t>(std::lround((a_y - stream.originY) / FallCapture::CELL));
			if (x < 0 || y < 0 || x >= static_cast<int32_t>(stream.result.width) || y >= static_cast<int32_t>(stream.result.height))
				continue;
			const float cover = stream.result.cover[static_cast<std::size_t>(y) * stream.result.width + x];
			if (cover <= NO_SURFACE)
				continue;
			a_level = found ? std::max(a_level, cover) : cover;
			found = true;
		}
		return found;
	}

	uint32_t FallSites::CountReadyStreams() const
	{
		return static_cast<uint32_t>(std::count_if(streams.begin(), streams.end(), [](const auto& a_entry) { return a_entry.second.ready; }));
	}

	void FallSites::BuildPoolMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_heights) const
	{
		// D40: where each site's pools really have water (its flow's cover), on the same lattice as its flow
		a_heights.assign(static_cast<std::size_t>(a_size) * a_size, FallCapture::NONE);
		const auto n = static_cast<int32_t>(a_size);
		for (const auto& [key, stream] : streams) {
			if (!stream.ready || stream.result.cover.size() != stream.result.water.size())
				continue;
			const auto w = static_cast<int32_t>(stream.result.width);
			const auto h = static_cast<int32_t>(stream.result.height);
			const auto offsetX = static_cast<int32_t>(std::lround((stream.originX - a_originX) / a_cell));
			const auto offsetY = static_cast<int32_t>(std::lround((stream.originY - a_originY) / a_cell));
			for (int32_t y = std::max(0, -offsetY); y < h && y + offsetY < n; y++) {
				for (int32_t x = std::max(0, -offsetX); x < w && x + offsetX < n; x++) {
					const float cover = stream.result.cover[static_cast<std::size_t>(y) * w + x];
					if (cover <= NO_SURFACE)
						continue;
					// D50: the pool the falls come from is cut deeper: the lip dips under it where its water does not show
					const float cut = cover >= stream.top - POOL_BAND ? cover - TOP_POOL_CUT : cover;
					float& stored = a_heights[static_cast<std::size_t>(y + offsetY) * a_size + static_cast<std::size_t>(x + offsetX)];
					stored = std::max(stored, cut);
				}
			}
		}
	}

	void FallSites::BuildPlungeMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_field) const
	{
		// D44, D47: where the falls meet the water below, along their whole intersection with it: every
		// vertex of the falls' surface lying at the pools' surface foams the water round it (steeper and
		// faster water more), and the simulated flow carries the foam off downstream
		a_field.assign(static_cast<std::size_t>(a_size) * a_size, 0.0f);
		std::vector<float> pools;
		BuildPoolMap(a_originX, a_originY, a_cell, a_size, pools);
		const auto reach = static_cast<int32_t>(std::ceil(PLUNGE_FOAM_RADIUS / a_cell));
		const auto n = static_cast<int32_t>(a_size);
		for (const auto& vertex : surfaceVertices) {
			const auto cx = static_cast<int32_t>(std::lround((vertex.position[0] - a_originX) / a_cell));
			const auto cy = static_cast<int32_t>(std::lround((vertex.position[1] - a_originY) / a_cell));
			if (cx < 1 || cy < 1 || cx >= n - 1 || cy >= n - 1)
				continue;
			// The pool's level round the vertex (its own cell may be the falls' own, uncovered)
			float level = FallCapture::NONE;
			for (int32_t dy = -1; dy <= 1; dy++) {
				for (int32_t dx = -1; dx <= 1; dx++)
					level = std::max(level, pools[static_cast<std::size_t>(cy + dy) * a_size + (cx + dx)]);
			}
			if (level <= FallCapture::NONE || std::abs(vertex.position[2] - level) > PLUNGE_FOAM_BAND)
				continue;
			const float steep = std::clamp((1.0f - vertex.normal[2]) * 2.5f, 0.0f, 1.0f);
			const float strength = std::clamp(0.85f + 0.15f * std::max(steep, std::clamp(vertex.speed, 0.0f, 1.0f)), 0.0f, 1.0f);  // D52: the rim is solid for every fall
			for (int32_t y = cy - reach; y <= cy + reach; y++) {
				for (int32_t x = cx - reach; x <= cx + reach; x++) {
					if (x < 0 || y < 0 || x >= n || y >= n)
						continue;
					const float dx = a_originX + static_cast<float>(x) * a_cell - vertex.position[0];
					const float dy = a_originY + static_cast<float>(y) * a_cell - vertex.position[1];
					const float falloff = std::clamp(1.0f - std::sqrt(dx * dx + dy * dy) / PLUNGE_FOAM_RADIUS, 0.0f, 1.0f);
					float& value = a_field[static_cast<std::size_t>(y) * a_size + x];
					value = std::max(value, strength * falloff);
				}
			}
		}
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

		// D36: vanilla's fall meshes, and where they meet each other, the water and the rock
		if (a_view > 0) {
			// A short segment, only when both ends are in front of the camera
			const auto segment = [&](const float a[3], const float b[3], ImU32 a_color, float a_thickness) {
				ImVec2 pa, pb;
				if (project(a[0], a[1], a[2], pa) && project(b[0], b[1], b[2], pb))
					a_drawList->AddLine(pa, pb, a_color, a_thickness);
			};
			const auto mark = [&](const float p[3], ImU32 a_color, float a_size) {
				ImVec2 s;
				if (!project(p[0], p[1], p[2], s))
					return;
				a_drawList->AddLine(ImVec2(s.x - a_size, s.y - a_size), ImVec2(s.x + a_size, s.y + a_size), a_color, 2.0f);
				a_drawList->AddLine(ImVec2(s.x - a_size, s.y + a_size), ImVec2(s.x + a_size, s.y - a_size), a_color, 2.0f);
			};
			const auto inView = [&](const float p[3]) {
				const float dx = p[0] - eye.x, dy = p[1] - eye.y, dz = p[2] - eye.z;
				return dx * dx + dy * dy + dz * dz < FALL_VIEW_RANGE * FALL_VIEW_RANGE;
			};
			for (const auto& site : sites) {
				const auto it = site.captureKey ? streams.find(site.captureKey) : streams.end();
				if (it == streams.end())
					continue;
				const auto& stream = it->second;
				const FallCapture* capture = GetCapture(site);
				const auto insideRock = [&](const float p[3]) {
					float ground;
					return capture && capture->IsComplete() && capture->Sample(p[0], p[1], ground) && ground > p[2] + 4.0f;
				};
				if (a_view == 1) {
					for (const auto& triangle : stream.sources) {
						if (!inView(triangle.position[0]))
							continue;
						// One colour per piece, round the hue circle by the golden angle
						const float hue = std::fmod(static_cast<float>(triangle.piece) * 0.618034f, 1.0f);
						float r, g, b;
						ImGui::ColorConvertHSVtoRGB(hue, 0.65f, 1.0f, r, g, b);
						const ImU32 pieceColor = IM_COL32(static_cast<int>(r * 255.0f), static_cast<int>(g * 255.0f), static_cast<int>(b * 255.0f), 200);
						for (int k = 0; k < 3; k++) {
							const float* a = triangle.position[k];
							const float* b2 = triangle.position[(k + 1) % 3];
							segment(a, b2, insideRock(a) && insideRock(b2) ? IM_COL32(255, 40, 40, 230) : pieceColor, 1.0f);
						}
					}
				} else {
					for (const auto& point : stream.seamPoints) {
						if (inView(point.data()))
							mark(point.data(), point[3] < 0.5f ? IM_COL32(255, 230, 40, 255) : IM_COL32(255, 60, 255, 255), 3.0f);
					}
					for (const auto& waterLine : stream.waterLines) {
						if (inView(waterLine.data()))
							segment(waterLine.data(), waterLine.data() + 3, IM_COL32(60, 255, 90, 255), 2.5f);
					}
					for (const auto& triangle : stream.sources) {
						for (int k = 0; k < 3; k++) {
							if (inView(triangle.position[k]) && insideRock(triangle.position[k]))
								mark(triangle.position[k], IM_COL32(255, 40, 40, 200), 2.0f);
						}
					}
					static constexpr ImU32 KIND_COLORS[3] = { IM_COL32(60, 230, 255, 255), IM_COL32(60, 110, 255, 255), IM_COL32(255, 150, 40, 255) };
					for (const auto& emitter : stream.emitters) {
						if (inView(emitter.position)) {
							ImVec2 s;
							if (project(emitter.position[0], emitter.position[1], emitter.position[2], s))
								a_drawList->AddCircleFilled(s, 2.5f, KIND_COLORS[std::clamp(static_cast<int>(emitter.kind), 0, 2)]);
						}
					}
				}
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
