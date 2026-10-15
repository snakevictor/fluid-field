#include "FallStream.h"

#include <algorithm>
#include <cmath>
#include <deque>

namespace FluidFieldSim
{
	FallStream::Result FallStream::Solve(const Input& a_input)
	{
		Result result;
		const auto w = static_cast<int32_t>(a_input.width);
		const auto h = static_cast<int32_t>(a_input.height);
		result.width = a_input.width;
		result.height = a_input.height;
		const std::size_t count = static_cast<std::size_t>(w) * h;
		if (w < 3 || h < 3 || a_input.surface.size() != count || a_input.kind.size() != count)
			return result;
		const float cell = a_input.cell;
		const auto index = [&](int32_t a_x, int32_t a_y) { return static_cast<std::size_t>(a_y) * w + a_x; };
		const auto inside = [&](int32_t a_x, int32_t a_y) { return a_x >= 0 && a_y >= 0 && a_x < w && a_y < h; };
		const auto open = [&](int32_t a_x, int32_t a_y) { return inside(a_x, a_y) && a_input.kind[index(a_x, a_y)] != Wall; };

		// The surface, smoothed over the water so its slope is not the mesh's facets
		std::vector<float> smooth(count, 0.0f);
		for (int32_t y = 0; y < h; y++) {
			for (int32_t x = 0; x < w; x++) {
				if (!open(x, y))
					continue;
				float sum = 0.0f, weight = 0.0f;
				for (int32_t dy = -SMOOTH_CELLS; dy <= SMOOTH_CELLS; dy++) {
					for (int32_t dx = -SMOOTH_CELLS; dx <= SMOOTH_CELLS; dx++) {
						if (!open(x + dx, y + dy))
							continue;
						sum += a_input.surface[index(x + dx, y + dy)];
						weight += 1.0f;
					}
				}
				smooth[index(x, y)] = sum / weight;
			}
		}

		// The drive: downhill, unit speed where the surface slopes, weaker where it is nearly flat
		std::vector<float> gx(count, 0.0f), gy(count, 0.0f);
		for (int32_t y = 0; y < h; y++) {
			for (int32_t x = 0; x < w; x++) {
				if (!open(x, y))
					continue;
				const float here = smooth[index(x, y)];
				const auto at = [&](int32_t a_x, int32_t a_y) { return open(a_x, a_y) ? smooth[index(a_x, a_y)] : here; };
				const float sx = (at(x + 1, y) - at(x - 1, y)) / (2.0f * cell);
				const float sy = (at(x, y + 1) - at(x, y - 1)) / (2.0f * cell);
				const float slope = std::sqrt(sx * sx + sy * sy);
				if (slope < 1e-5f)
					continue;
				const float strength = std::min(1.0f, slope / FLAT_SLOPE);
				gx[index(x, y)] = -sx / slope * strength;
				gy[index(x, y)] = -sy / slope * strength;
			}
		}

		// The pressure: no flux through rock, zero at the open boundaries; Gauss-Seidel with over-relaxation
		std::vector<float> p(count, 0.0f);
		static constexpr int32_t DX[4] = { 1, -1, 0, 0 };
		static constexpr int32_t DY[4] = { 0, 0, 1, -1 };
		for (result.iterations = 0; result.iterations < MAX_ITERATIONS; result.iterations++) {
			float largest = 0.0f;
			for (int32_t y = 0; y < h; y++) {
				for (int32_t x = 0; x < w; x++) {
					const std::size_t i = index(x, y);
					if (a_input.kind[i] != Water)
						continue;
					float sum = 0.0f, drive = 0.0f;
					int32_t neighbours = 0;
					for (int k = 0; k < 4; k++) {
						const int32_t nx = x + DX[k];
						const int32_t ny = y + DY[k];
						if (!open(nx, ny))
							continue;
						const std::size_t j = index(nx, ny);
						sum += p[j];
						drive += 0.5f * ((gx[i] + gx[j]) * static_cast<float>(DX[k]) + (gy[i] + gy[j]) * static_cast<float>(DY[k]));
						neighbours++;
					}
					if (neighbours == 0)
						continue;
					const float target = (sum - cell * drive) / static_cast<float>(neighbours);
					const float change = SOR * (target - p[i]);
					p[i] += change;
					largest = std::max(largest, std::abs(change));
				}
			}
			result.residual = largest;
			if (largest < TOLERANCE)
				break;
		}

		// Face fluxes: x faces (w + 1 per row) and y faces (h + 1 per column); zero through rock
		const auto flux = [&](int32_t a_x0, int32_t a_y0, int32_t a_x1, int32_t a_y1) {
			if (!open(a_x0, a_y0) || !open(a_x1, a_y1))
				return 0.0f;
			const std::size_t i = index(a_x0, a_y0);
			const std::size_t j = index(a_x1, a_y1);
			const float dx = static_cast<float>(a_x1 - a_x0);
			const float dy = static_cast<float>(a_y1 - a_y0);
			return 0.5f * ((gx[i] + gx[j]) * dx + (gy[i] + gy[j]) * dy) - (p[j] - p[i]) / cell;
		};
		std::vector<float> fx(static_cast<std::size_t>(w + 1) * h, 0.0f);  // Face between cells (a - 1, y) and (a, y), at [y * (w + 1) + a]
		std::vector<float> fy(static_cast<std::size_t>(h + 1) * w, 0.0f);  // Face between cells (x, b - 1) and (x, b), at [b * w + x]
		for (int32_t y = 0; y < h; y++) {
			for (int32_t a = 1; a < w; a++)
				fx[static_cast<std::size_t>(y) * (w + 1) + a] = flux(a - 1, y, a, y);
		}
		for (int32_t b = 1; b < h; b++) {
			for (int32_t x = 0; x < w; x++)
				fy[static_cast<std::size_t>(b) * w + x] = flux(x, b - 1, x, b);
		}

		// The stream function on the cell corners, integrated across the faces: along +y, psi gains the
		// x flux; along +x, it loses the y flux. Rock carries no flux, so psi is constant round a boulder
		const int32_t cw = w + 1;
		const int32_t ch = h + 1;
		std::vector<float> corner(static_cast<std::size_t>(cw) * ch, 0.0f);
		std::vector<uint8_t> seen(corner.size(), 0);
		std::deque<std::pair<int32_t, int32_t>> queue;
		for (int32_t sy = 0; sy < ch; sy++) {
			for (int32_t sx = 0; sx < cw; sx++) {
				if (seen[static_cast<std::size_t>(sy) * cw + sx])
					continue;
				seen[static_cast<std::size_t>(sy) * cw + sx] = 1;
				queue.emplace_back(sx, sy);
				while (!queue.empty()) {
					const auto [a, b] = queue.front();
					queue.pop_front();
					const float here = corner[static_cast<std::size_t>(b) * cw + a];
					const auto visit = [&](int32_t a_a, int32_t a_b, float a_value) {
						if (a_a < 0 || a_b < 0 || a_a >= cw || a_b >= ch)
							return;
						const std::size_t c = static_cast<std::size_t>(a_b) * cw + a_a;
						if (seen[c])
							return;
						seen[c] = 1;
						corner[c] = a_value;
						queue.emplace_back(a_a, a_b);
					};
					// Up: the vertical edge x = a between corners (a, b) and (a, b + 1) is x face a of row b
					if (b < h)
						visit(a, b + 1, here + fx[static_cast<std::size_t>(b) * (w + 1) + a] * cell);
					if (b > 0)
						visit(a, b - 1, here - fx[static_cast<std::size_t>(b - 1) * (w + 1) + a] * cell);
					// Right: the horizontal edge y = b between corners (a, b) and (a + 1, b) is y face b of column a
					if (a < w)
						visit(a + 1, b, here - fy[static_cast<std::size_t>(b) * w + a] * cell);
					if (a > 0)
						visit(a - 1, b, here + fy[static_cast<std::size_t>(b) * w + a - 1] * cell);
				}
			}
		}

		// Per cell: psi at its centre, speed, and where the water foams
		result.psi.assign(count, 0.0f);
		result.speed.assign(count, 0.0f);
		result.foam.assign(count, 0.0f);
		result.water.assign(count, 0);
		std::vector<uint8_t> nearRock(count, 0);
		for (int32_t y = 0; y < h; y++) {
			for (int32_t x = 0; x < w; x++) {
				if (open(x, y))
					continue;
				for (int32_t dy = -ROCK_REACH; dy <= ROCK_REACH; dy++) {
					for (int32_t dx = -ROCK_REACH; dx <= ROCK_REACH; dx++) {
						if (inside(x + dx, y + dy))
							nearRock[index(x + dx, y + dy)] = 1;
					}
				}
			}
		}
		for (int32_t y = 0; y < h; y++) {
			for (int32_t x = 0; x < w; x++) {
				const std::size_t i = index(x, y);
				const float c00 = corner[static_cast<std::size_t>(y) * cw + x];
				const float c10 = corner[static_cast<std::size_t>(y) * cw + x + 1];
				const float c01 = corner[static_cast<std::size_t>(y + 1) * cw + x];
				const float c11 = corner[static_cast<std::size_t>(y + 1) * cw + x + 1];
				result.psi[i] = 0.25f * (c00 + c10 + c01 + c11);
				if (!open(x, y))
					continue;
				result.water[i] = 1;
				const float u = 0.5f * (fx[static_cast<std::size_t>(y) * (w + 1) + x] + fx[static_cast<std::size_t>(y) * (w + 1) + x + 1]);
				const float v = 0.5f * (fy[static_cast<std::size_t>(y) * w + x] + fy[static_cast<std::size_t>(y + 1) * w + x]);
				const float speed = std::sqrt(u * u + v * v);
				result.speed[i] = speed;
				// Driven water that stalls beside rock piles up against it or eddies behind it; water
				// squeezed through a gap runs fast and white
				const float drive = std::sqrt(gx[i] * gx[i] + gy[i] * gy[i]);
				const float stall = nearRock[i] && drive > 0.5f ? std::clamp((STALL_SPEED - speed) / STALL_SPEED, 0.0f, 1.0f) : 0.0f;
				const float squeeze = std::clamp((speed - SQUEEZE_SPEED) * 2.0f, 0.0f, 1.0f);
				result.foam[i] = std::max(stall, squeeze);
			}
		}
		return result;
	}
}
