#include "FallBed.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace FluidFieldSim
{
	namespace
	{
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
	}

	void FallBed::Clear()
	{
		areas.clear();
		samples.clear();
		pending.clear();
		texture = nullptr;
		srv = nullptr;
		heights.clear();
		resolution = 0;
		filled = false;
	}

	void FallBed::SetAreas(const std::vector<Area>& a_areas)
	{
		const auto same = [](const Area& a, const Area& b) {
			return a.min[0] == b.min[0] && a.min[1] == b.min[1] && a.max[0] == b.max[0] && a.max[1] == b.max[1];
		};
		if (a_areas.size() == areas.size() && std::equal(a_areas.begin(), a_areas.end(), areas.begin(), same))
			return;
		areas = a_areas;
		areasChanged = true;
		pending.clear();
		for (const auto& area : areas) {
			const auto x0 = static_cast<int32_t>(std::floor(area.min[0] / LATTICE));
			const auto y0 = static_cast<int32_t>(std::floor(area.min[1] / LATTICE));
			const auto x1 = static_cast<int32_t>(std::ceil(area.max[0] / LATTICE));
			const auto y1 = static_cast<int32_t>(std::ceil(area.max[1] / LATTICE));
			for (int32_t y = y0; y <= y1; y++) {
				for (int32_t x = x0; x <= x1; x++) {
					if (!samples.contains(Key(x, y)))
						pending.push_back({ x, y, area.top, area.bottom });
				}
			}
		}
	}

	void FallBed::CastPending(double a_budgetMs)
	{
		if (pending.empty())
			return;
		auto tes = RE::TES::GetSingleton();
		const auto& first = pending.back();
		auto* cell = tes ? (tes->interiorCell ? tes->interiorCell : tes->GetCell(RE::NiPoint3(first.x * LATTICE, first.y * LATTICE, first.bottom))) : nullptr;
		auto* bhkWorld = cell ? cell->GetbhkWorld() : nullptr;
		auto* world = bhkWorld ? bhkWorld->GetWorld1() : nullptr;
		if (!world)
			return;
		const RE::BSReadLockGuard lock(bhkWorld->worldLock);
		const float scale = RE::bhkWorld::GetWorldScale();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(a_budgetMs);

		while (!pending.empty() && std::chrono::steady_clock::now() < deadline) {
			const Pending point = pending.back();
			pending.pop_back();
			const float x = static_cast<float>(point.x) * LATTICE;
			const float y = static_cast<float>(point.y) * LATTICE;
			// The highest rock or ground below the top of the fall, through anything else
			float height = NONE;
			float fromZ = point.top;
			for (int attempt = 0; attempt < 4 && fromZ > point.bottom; attempt++) {
				RE::hkpWorldRayCastInput input;
				input.from = RE::hkVector4(x * scale, y * scale, fromZ * scale, 0.0f);
				input.to = RE::hkVector4(x * scale, y * scale, point.bottom * scale, 0.0f);
				RE::hkpWorldRayCastOutput output;
				world->CastRay(input, output);
				if (!output.HasHit())
					break;
				const float hitZ = fromZ + (point.bottom - fromZ) * output.hitFraction;
				if (output.rootCollidable && IsSolidLayer(output.rootCollidable->GetCollisionLayer())) {
					height = hitZ;
					break;
				}
				fromZ = hitZ - 1.0f;
			}
			samples[Key(point.x, point.y)] = height;
			dirtyMin[0] = std::min(dirtyMin[0], x - LATTICE);
			dirtyMin[1] = std::min(dirtyMin[1], y - LATTICE);
			dirtyMax[0] = std::max(dirtyMax[0], x + LATTICE);
			dirtyMax[1] = std::max(dirtyMax[1], y + LATTICE);
		}
	}

	std::size_t FallBed::CountHits() const
	{
		return static_cast<std::size_t>(std::count_if(samples.begin(), samples.end(), [](const auto& a_sample) { return a_sample.second > NONE; }));
	}

	bool FallBed::InAnyArea(float a_x, float a_y) const
	{
		for (const auto& area : areas) {
			if (a_x >= area.min[0] && a_x <= area.max[0] && a_y >= area.min[1] && a_y <= area.max[1])
				return true;
		}
		return false;
	}

	bool FallBed::Sample(float a_x, float a_y, float& a_height) const
	{
		const float fx = a_x / LATTICE;
		const float fy = a_y / LATTICE;
		const auto ix = static_cast<int32_t>(std::floor(fx));
		const auto iy = static_cast<int32_t>(std::floor(fy));
		const float tx = fx - static_cast<float>(ix);
		const float ty = fy - static_cast<float>(iy);
		float h[4];
		const int32_t offsets[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
		for (int i = 0; i < 4; i++) {
			const auto it = samples.find(Key(ix + offsets[i][0], iy + offsets[i][1]));
			if (it == samples.end() || it->second <= NONE)
				return false;
			h[i] = it->second;
		}
		a_height = (h[0] * (1.0f - tx) + h[1] * tx) * (1.0f - ty) + (h[2] * (1.0f - tx) + h[3] * tx) * ty;
		return true;
	}

	bool FallBed::Upload(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const int32_t a_gridMin[2], uint32_t a_resolution, float a_cellSize)
	{
		if (a_resolution != resolution || a_cellSize != cellSize || !texture) {
			texture = nullptr;
			srv = nullptr;
			D3D11_TEXTURE2D_DESC desc{ .Width = a_resolution, .Height = a_resolution, .MipLevels = 1, .ArraySize = 1, .Format = DXGI_FORMAT_R32_FLOAT, .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = 0 };
			if (FAILED(a_device->CreateTexture2D(&desc, nullptr, texture.put())) || FAILED(a_device->CreateShaderResourceView(texture.get(), nullptr, srv.put()))) {
				texture = nullptr;
				srv = nullptr;
				return false;
			}
			Util::SetResourceName(texture.get(), "FluidField::FallBed");
			Util::SetResourceName(srv.get(), "FluidField::FallBed SRV");
			resolution = a_resolution;
			cellSize = a_cellSize;
			heights.assign(static_cast<std::size_t>(resolution) * resolution, NONE);
			filled = false;
			areasChanged = true;
		}

		const auto n = static_cast<int32_t>(resolution);
		const int32_t mask = n - 1;
		bool changed = false;
		const auto fillCell = [&](int32_t a_cellX, int32_t a_cellY) {
			const float x = (static_cast<float>(a_cellX) + 0.5f) * cellSize;
			const float y = (static_cast<float>(a_cellY) + 0.5f) * cellSize;
			float value = NONE;
			if (InAnyArea(x, y) && !Sample(x, y, value))
				value = NONE;
			float& stored = heights[static_cast<std::size_t>(a_cellY & mask) * n + (a_cellX & mask)];
			if (stored != value) {
				stored = value;
				changed = true;
			}
		};

		// Everything when the areas changed; otherwise the cells new to the grid, and those under new rays
		if (areasChanged || !filled) {
			for (int32_t y = 0; y < n; y++) {
				for (int32_t x = 0; x < n; x++)
					fillCell(a_gridMin[0] + x, a_gridMin[1] + y);
			}
			areasChanged = false;
		} else {
			for (int32_t y = 0; y < n; y++) {
				const int32_t cellY = a_gridMin[1] + y;
				const bool rowWasInGrid = cellY >= filledMin[1] && cellY < filledMin[1] + n;
				for (int32_t x = 0; x < n; x++) {
					const int32_t cellX = a_gridMin[0] + x;
					if (rowWasInGrid && cellX >= filledMin[0] && cellX < filledMin[0] + n) {
						x = filledMin[0] + n - a_gridMin[0] - 1;
						continue;
					}
					fillCell(cellX, cellY);
				}
			}
			if (dirtyMin[0] <= dirtyMax[0]) {
				const auto x0 = std::max(static_cast<int32_t>(std::floor(dirtyMin[0] / cellSize)), a_gridMin[0]);
				const auto y0 = std::max(static_cast<int32_t>(std::floor(dirtyMin[1] / cellSize)), a_gridMin[1]);
				const auto x1 = std::min(static_cast<int32_t>(std::ceil(dirtyMax[0] / cellSize)), a_gridMin[0] + n - 1);
				const auto y1 = std::min(static_cast<int32_t>(std::ceil(dirtyMax[1] / cellSize)), a_gridMin[1] + n - 1);
				for (int32_t y = y0; y <= y1; y++) {
					for (int32_t x = x0; x <= x1; x++)
						fillCell(x, y);
				}
			}
		}
		dirtyMin[0] = dirtyMin[1] = FLT_MAX;
		dirtyMax[0] = dirtyMax[1] = -FLT_MAX;
		filledMin[0] = a_gridMin[0];
		filledMin[1] = a_gridMin[1];
		filled = true;

		if (changed)
			a_context->UpdateSubresource(texture.get(), 0, nullptr, heights.data(), static_cast<UINT>(resolution * sizeof(float)), 0);
		return true;
	}
}
