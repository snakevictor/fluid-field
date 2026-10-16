#include "FallSites.h"

#include "EngineGrid.h"
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
#include <numeric>

namespace FluidFieldSim
{
	namespace
	{
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
		// D30: the falling-water models of sites whose own surface draws
		std::unordered_set<uint32_t> replaced;
		if (replaceSurface) {
			for (const auto& site : sites) {
				const auto it = site.captureKey ? streams.find(site.captureKey) : streams.end();
				if (it == streams.end() || !it->second.meshReady || it->second.vertices.empty())
					continue;
				for (const uint32_t member : site.members) {
					if (refs[member].cls == FxClass::Body)
						replaced.insert(refs[member].formID);
				}
			}
		}
		for (const auto& ref : refs) {
			if (!replaced.contains(ref.formID))
				continue;
			auto* object = RE::TESForm::LookupByID<RE::TESObjectREFR>(ref.formID);
			if (auto* root = object ? object->Get3D() : nullptr)
				wanted.emplace(root, RE::NiPointer<RE::NiAVObject>(root));
		}
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
				if (replaced.contains(ref.formID))
					continue;
				// The falling-water models carry layers of their own (and spray particles): show only the
				// layers in the mask
				RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
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
		grounded.clear();
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

	void FallSites::RasterSurface(const Site& a_site, float a_originX, float a_originY, float a_spacing, int32_t a_width, int32_t a_height, std::vector<float>& a_surface) const
	{
		a_surface.assign(static_cast<std::size_t>(a_width) * a_height, NO_SURFACE);
		const auto window = [&](int32_t a_x, int32_t a_y) { return static_cast<std::size_t>(a_y) * a_width + a_x; };
		const auto range = [&](float a_min, float a_max, float a_origin, int32_t a_count, int32_t& a_from, int32_t& a_to) {
			a_from = std::max(0, static_cast<int32_t>(std::ceil((a_min - a_origin) / a_spacing)));
			a_to = std::min(a_count - 1, static_cast<int32_t>(std::floor((a_max - a_origin) / a_spacing)));
		};
		// The falling water seen from above: the highest of its triangles over each point
		const auto raster = [&](const RE::NiPoint3& a, const RE::NiPoint3& b, const RE::NiPoint3& c) {
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
					stored = std::max(stored, w0 * a.z + w1 * b.z + w2 * c.z);
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
		for (const auto& plane : a_site.planes) {
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
		RasterSurface(a_site, a_capture.GetOriginX(), a_capture.GetOriginY(), FallCapture::CELL, static_cast<int32_t>(input.width), static_cast<int32_t>(input.height), input.surface);
		input.kind.assign(input.surface.size(), FallStream::Wall);
		input.bed.assign(input.surface.size(), 0.0f);
		for (uint32_t y = 0; y < input.height; y++) {
			for (uint32_t x = 0; x < input.width; x++)
				input.bed[static_cast<std::size_t>(y) * input.width + x] = a_capture.At(x, y);
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
				stream.meshReady = true;
				changed = true;
				logger::info("[Fluid Field] Waterfall site #{} surface: {} source triangles ({} under another piece dropped), {} vertices, {} triangles", stream.siteId,
					stream.sources.size(), mesh.culled, stream.vertices.size(), stream.indices.size() / 3);
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
					float sums[5]{};
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
			float across, travel, speed, foam, edge, depth, water;
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
		return mesh;
	}

	void FallSites::RefreshSurfaces()
	{
		surfaceVertices.clear();
		surfaceIndices.clear();
		for (const auto& [key, stream] : streams) {
			if (!stream.meshReady || stream.vertices.empty())
				continue;
			const auto base = static_cast<uint32_t>(surfaceVertices.size());
			surfaceVertices.insert(surfaceVertices.end(), stream.vertices.begin(), stream.vertices.end());
			for (const uint32_t i : stream.indices)
				surfaceIndices.push_back(base + i);
		}
		surfaceVersion++;
	}

	ID3D11ShaderResourceView* FallSites::GetSurfaceDiffuse() const
	{
		return surfaceDiffuse && surfaceDiffuse->rendererTexture ? surfaceDiffuse->rendererTexture->resourceView : nullptr;
	}

	ID3D11ShaderResourceView* FallSites::GetSurfaceNormal() const
	{
		return surfaceNormal && surfaceNormal->rendererTexture ? surfaceNormal->rendererTexture->resourceView : nullptr;
	}

	void FallSites::SetReplaceSurface(bool a_replace)
	{
		if (a_replace == replaceSurface)
			return;
		replaceSurface = a_replace;
		RefreshHiddenEffects();
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
					if (!result.water[from] || a_field[to + 3] > 0.0f)
						continue;
					a_field[to + 0] = result.psi[from];
					a_field[to + 1] = result.speed[from];
					a_field[to + 2] = result.foam[from];
					a_field[to + 3] = 1.0f;
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
					// Only the water's own surfaces: the models also carry particle spray (no triangles of
					// their own), which grounding and the streamlines would smear
					auto* triShape = a_geometry->AsTriShape();
					if (triShape && triShape->GetTrishapeRuntimeData().triangleCount > 0)
						grounded.insert(a_geometry);
					// Diagnostics: what each part of a falling-water mesh is, once per model and part
					auto* property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
					// D30: vanilla's falling-water textures, from the lit surface
					if (auto* lit = property && !surfaceDiffuse ? netimmerse_cast<RE::BSLightingShaderProperty*>(property) : nullptr) {
						if (auto* base = static_cast<RE::BSLightingShaderMaterialBase*>(lit->GetBaseMaterial()); base && base->diffuseTexture) {
							surfaceDiffuse = base->diffuseTexture;
							surfaceNormal = base->normalTexture;
						}
					}
					auto* effect = property ? netimmerse_cast<RE::BSEffectShaderProperty*>(property) : nullptr;
					auto* material = effect ? static_cast<RE::BSEffectShaderMaterial*>(effect->GetBaseMaterial()) : nullptr;
					const char* model = ModelOf(ref->GetBaseObject());
					const std::string key = std::format("{}|{}", model ? model : "?", a_geometry->name.c_str());
					if (loggedParts.insert(key).second) {
						// The lit surface's textures and the range of its texture coordinates
						std::string extra;
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
		groundVersion++;
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
		CaptureStep();
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

	void FallSites::DrawDebug(ImDrawList* a_drawList, float a_range) const
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
