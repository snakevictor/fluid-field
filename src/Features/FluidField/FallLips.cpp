#include "FallSites.h"

#include "FallStrands.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// D170: lip map for the shallow-water solver: where a falls column climbs a hidden lead-in out of its river, the river is
// drawn toward the crest (the drawdown before a drop).

namespace FluidFieldSim
{
	namespace
	{
		constexpr float LIP_SPEED = 200.0f;      ///< World units/s at the crest
		constexpr float LIP_REACH = 320.0f;      ///< World units upstream of a crest the river starts to speed up
		constexpr float LIP_PAST = 48.0f;        ///< World units past the crest still drawn on
		constexpr float LIP_SIDE = 64.0f;        ///< World units to either side of a lip point it reaches
		constexpr float LIP_WATER_BAND = 40.0f;  ///< A crest this near the water over it is that water's lip
		constexpr float LIP_DROP_FROM = 24.0f;   ///< World units of drop below the crest where the draw starts...
		constexpr float LIP_DROP_FULL = 160.0f;  ///< ...and is full, so a small step barely draws the river on

		float Smoothstep(float a_from, float a_to, float a_x)
		{
			const float t = std::clamp((a_x - a_from) / (a_to - a_from), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		}

		struct Lip
		{
			float x, y;      ///< The crest, world units
			float dx, dy;    ///< Horizontal way the falls start, unit
			float strength;  ///< 0 to 1, by the drop
		};
	}

	void FallSites::BuildLipMap(float a_originX, float a_originY, float a_cell, uint32_t a_size, std::vector<float>& a_field) const
	{
		a_field.assign(static_cast<std::size_t>(a_size) * a_size * 4, 0.0f);
		if (!(a_cell > 0.0f) || a_size == 0)
			return;
		std::vector<Lip> lips;
		std::vector<std::array<float, 5>> planes;
		for (const auto& [geometry, field] : bakeFields) {
			if (!field || !grounded.contains(geometry))
				continue;
			const auto w = static_cast<int32_t>(field->width), h = static_cast<int32_t>(field->height);
			const std::size_t count = static_cast<std::size_t>(w) * h;
			if (w < 2 || h < 3 || field->position.size() != count || field->covered.size() != count)
				continue;
			// D126: shell crests only, none inside rock or another sheet
			std::vector<uint8_t> shell;
			{
				const float uvMin[2] = { field->uvMin[0], field->uvMin[1] }, uvSize[2] = { field->uvSize[0], field->uvSize[1] };
				shell = ShellTexels(const_cast<RE::BSGeometry*>(geometry), field->width, field->height, uvMin, uvSize, nullptr);
			}
			float bottom = FLT_MAX, lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
			for (std::size_t i = 0; i < count; i++) {
				if (!field->covered[i])
					continue;
				const RE::NiPoint3& p = field->position[i];
				bottom = std::min(bottom, p.z);
				lo[0] = std::min(lo[0], p.x);
				lo[1] = std::min(lo[1], p.y);
				hi[0] = std::max(hi[0], p.x);
				hi[1] = std::max(hi[1], p.y);
			}
			if (bottom == FLT_MAX)
				continue;
			planes.clear();
			CollectWaterPlanes(lo[0], lo[1], hi[0], hi[1], planes);
			FallStrands::Input input;
			input.width = field->width;
			input.height = field->height;
			input.position = reinterpret_cast<const float*>(field->position.data());
			input.covered = field->covered.data();
			std::vector<int32_t> crest;
			std::vector<uint8_t> leadIn;
			FallStrands::FindCrests(input, crest, leadIn);
			const auto index = [&](int32_t a_x, int32_t a_y) { return static_cast<std::size_t>(a_y) * w + a_x; };
			for (int32_t x = 0; x < w; x++) {
				const int32_t c = crest[x];
				// A lip needs a lead-in before its crest (fed meshes and mid-fall starts have none)
				if (c < 1 || c + 2 >= h || !leadIn[index(x, c - 1)] || !field->covered[index(x, c + 2)])
					continue;
				const std::size_t i = index(x, c);
				if ((!field->rock.empty() && field->rock[i]) || (i < shell.size() && !shell[i]))
					continue;
				const RE::NiPoint3& p = field->position[i];
				// The crest must lie at a water surface
				float level = 0.0f;
				bool river = PoolLevelAt(p.x, p.y, level) && std::abs(p.z - level) <= LIP_WATER_BAND;
				for (const auto& plane : planes) {
					if (!river && p.x >= plane[0] && p.x <= plane[2] && p.y >= plane[1] && p.y <= plane[3] && std::abs(p.z - plane[4]) <= LIP_WATER_BAND)
						river = true;
				}
				if (!river)
					continue;
				const RE::NiPoint3 down = field->position[index(x, c + 2)] - p;
				const float horizontal = std::sqrt(down.x * down.x + down.y * down.y);
				const float strength = Smoothstep(LIP_DROP_FROM, LIP_DROP_FULL, p.z - bottom);
				if (horizontal < 1e-3f || strength <= 0.0f)
					continue;
				lips.push_back({ p.x, p.y, down.x / horizontal, down.y / horizontal, strength });
			}
		}
		const auto n = static_cast<int32_t>(a_size);
		const auto reach = static_cast<int32_t>(std::ceil((LIP_REACH + LIP_SIDE) / a_cell));
		for (const Lip& lip : lips) {
			const auto cx = static_cast<int32_t>(std::lround((lip.x - a_originX) / a_cell));
			const auto cy = static_cast<int32_t>(std::lround((lip.y - a_originY) / a_cell));
			if (cx < -reach || cy < -reach || cx >= n + reach || cy >= n + reach)
				continue;
			for (int32_t y = std::max(cy - reach, 0); y <= std::min(cy + reach, n - 1); y++) {
				for (int32_t x = std::max(cx - reach, 0); x <= std::min(cx + reach, n - 1); x++) {
					const float rx = a_originX + static_cast<float>(x) * a_cell - lip.x;
					const float ry = a_originY + static_cast<float>(y) * a_cell - lip.y;
					const float upstream = -(rx * lip.dx + ry * lip.dy);
					const float side = std::abs(rx * lip.dy - ry * lip.dx);
					if (upstream > LIP_REACH || upstream < -LIP_PAST || side > LIP_SIDE)
						continue;
					// Stronger nearer the drop (the water accelerates toward it), full past the crest
					const float toward = upstream > 0.0f ? 1.0f - upstream / LIP_REACH : 1.0f;
					const float weight = lip.strength * toward * toward * (1.0f - Smoothstep(0.5f * LIP_SIDE, LIP_SIDE, side));
					float* texel = &a_field[(static_cast<std::size_t>(y) * a_size + x) * 4];
					if (weight <= texel[2])
						continue;
					texel[0] = lip.dx * LIP_SPEED;
					texel[1] = lip.dy * LIP_SPEED;
					texel[2] = weight;
				}
			}
		}
		if (lips.size() != loggedLips) {
			loggedLips = lips.size();
			logger::info("[Fluid Field] Waterfall lips (D170): {} crest columns draw the river on", lips.size());
		}
	}
}
