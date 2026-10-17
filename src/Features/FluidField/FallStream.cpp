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

		// The integration above follows one path to each corner, so wherever the flux is not exactly
		// divergence-free (an unconverged solve, the open boundaries) psi differs by path and shows as
		// zigzags. Relax it toward the least-squares fit of every edge's difference, starting from there
		const auto edgeDifference = [&](int32_t a_a, int32_t a_b, int k) {
			// psi(neighbour) - psi(here) across the edge in direction k: 0 up, 1 down, 2 right, 3 left
			switch (k) {
			case 0:
				return fx[static_cast<std::size_t>(a_b) * (w + 1) + a_a] * cell;
			case 1:
				return -fx[static_cast<std::size_t>(a_b - 1) * (w + 1) + a_a] * cell;
			case 2:
				return -fy[static_cast<std::size_t>(a_b) * w + a_a] * cell;
			default:
				return fy[static_cast<std::size_t>(a_b) * w + a_a - 1] * cell;
			}
		};
		for (uint32_t sweep = 0; sweep < PSI_SWEEPS; sweep++) {
			for (int32_t b = 0; b < ch; b++) {
				for (int32_t a = 0; a < cw; a++) {
					float sum = 0.0f;
					int32_t neighbours = 0;
					static constexpr int32_t CA[4] = { 0, 0, 1, -1 };
					static constexpr int32_t CB[4] = { 1, -1, 0, 0 };
					for (int k = 0; k < 4; k++) {
						const int32_t na = a + CA[k];
						const int32_t nb = b + CB[k];
						if (na < 0 || nb < 0 || na >= cw || nb >= ch)
							continue;
						sum += corner[static_cast<std::size_t>(nb) * cw + na] - edgeDifference(a, b, k);
						neighbours++;
					}
					float& value = corner[static_cast<std::size_t>(b) * cw + a];
					value += SOR * (sum / static_cast<float>(neighbours) - value);
				}
			}
		}

		// Per cell: psi at its centre, speed, and where the water foams
		result.psi.assign(count, 0.0f);
		result.speed.assign(count, 0.0f);
		result.foam.assign(count, 0.0f);
		result.rockFoam.assign(count, 0.0f);
		result.bed = a_input.bed;
		result.cover = a_input.cover;
		result.water.assign(count, 0);
		result.vx.assign(count, 0.0f);
		result.vy.assign(count, 0.0f);
		result.level.assign(count, 0.0f);
		result.depth.assign(count, 0.0f);
		result.edge.assign(count, 0.0f);
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
				result.rockFoam[i] = result.foam[i];
				result.vx[i] = u;
				result.vy[i] = v;
			}
		}

		// The course (D34): which way the water goes, from where it comes in to where it leaves. A
		// potential, 0 on the inflow and 1 on the outflow, harmonic over the water with the rock as walls,
		// has no hollows: its slope always leads from one to the other, round rock. The solved velocity
		// keeps its sideways part (parting round boulders, swirls) but never runs back along the course
		{
			bool inflow = false, outflow = false;
			for (std::size_t i = 0; i < count; i++) {
				inflow |= a_input.kind[i] == Inflow;
				outflow |= a_input.kind[i] == Outflow;
			}
			if (inflow && outflow) {
				std::vector<float> course(count, 0.5f);
				for (std::size_t i = 0; i < count; i++) {
					if (a_input.kind[i] == Inflow)
						course[i] = 0.0f;
					else if (a_input.kind[i] == Outflow)
						course[i] = 1.0f;
				}
				for (uint32_t sweep = 0; sweep < COURSE_SWEEPS; sweep++) {
					float change = 0.0f;
					for (int32_t y = 0; y < h; y++) {
						for (int32_t x = 0; x < w; x++) {
							const std::size_t i = index(x, y);
							if (!open(x, y) || a_input.kind[i] == Inflow || a_input.kind[i] == Outflow)
								continue;
							float sum = 0.0f;
							int32_t neighbours = 0;
							for (int k = 0; k < 4; k++) {
								if (!open(x + DX[k], y + DY[k]))
									continue;
								sum += course[index(x + DX[k], y + DY[k])];
								neighbours++;
							}
							if (!neighbours)
								continue;
							const float delta = SOR * (sum / static_cast<float>(neighbours) - course[i]);
							course[i] += delta;
							change = std::max(change, std::abs(delta));
						}
					}
					if (change < 1e-6f)
						break;
				}
				for (int32_t y = 0; y < h; y++) {
					for (int32_t x = 0; x < w; x++) {
						const std::size_t i = index(x, y);
						if (!result.water[i])
							continue;
						// Its slope, one-sided beside rock
						const auto at = [&](int32_t a_x, int32_t a_y) { return open(a_x, a_y) ? course[index(a_x, a_y)] : course[i]; };
						const float sx = 0.5f * (at(x + 1, y) - at(x - 1, y)) * ((open(x + 1, y) && open(x - 1, y)) ? 1.0f : 2.0f);
						const float sy = 0.5f * (at(x, y + 1) - at(x, y - 1)) * ((open(x, y + 1) && open(x, y - 1)) ? 1.0f : 2.0f);
						const float slope = std::sqrt(sx * sx + sy * sy);
						if (slope < 1e-7f)
							continue;
						const float dx = sx / slope, dy = sy / slope;
						const float along = result.vx[i] * dx + result.vy[i] * dy;
						// D37: near the start the course and gravity rule: the water runs straight on at free speed at
						// least, and turns aside less, so the falls' start never flows back however the solve churns
						const float start = 1.0f - std::clamp(course[i] / COURSE_START, 0.0f, 1.0f);
						const float side = 1.0f - 0.85f * start;
						const float sideX = (result.vx[i] - dx * along) * side, sideY = (result.vy[i] - dy * along) * side;
						const float forward = std::max(along, COURSE_FORWARD + (1.0f - COURSE_FORWARD) * start);
						result.vx[i] = sideX + dx * forward;
						result.vy[i] = sideY + dy * forward;
						result.speed[i] = std::sqrt(result.vx[i] * result.vx[i] + result.vy[i] * result.vy[i]);
					}
				}
			}
		}

		// D53: and never up the surface: where it slopes, the water runs down it at least COURSE_FORWARD of
		// free flow whatever the course and the solve say, so geometry the course reads wrongly cannot send
		// it back up a face
		for (std::size_t i = 0; i < count; i++) {
			if (!result.water[i])
				continue;
			const float slope = std::sqrt(gx[i] * gx[i] + gy[i] * gy[i]);
			if (slope < DOWNHILL_FROM)
				continue;
			const float dx = gx[i] / slope, dy = gy[i] / slope;
			const float along = result.vx[i] * dx + result.vy[i] * dy;
			const float least = COURSE_FORWARD * slope;
			if (along >= least)
				continue;
			result.vx[i] += dx * (least - along);
			result.vy[i] += dy * (least - along);
			result.speed[i] = std::sqrt(result.vx[i] * result.vx[i] + result.vy[i] * result.vy[i]);
		}

		// The surface for the mesh: the raw one smoothed over a cell, so the facets of vanilla's meshes
		// do not show, and the depth over the rock
		for (int32_t y = 0; y < h; y++) {
			for (int32_t x = 0; x < w; x++) {
				const std::size_t i = index(x, y);
				if (!open(x, y))
					continue;
				float sum = 0.0f, weight = 0.0f;
				for (int32_t dy = -1; dy <= 1; dy++) {
					for (int32_t dx = -1; dx <= 1; dx++) {
						if (!open(x + dx, y + dy))
							continue;
						const float k = (dx == 0 ? 2.0f : 1.0f) * (dy == 0 ? 2.0f : 1.0f);
						sum += a_input.surface[index(x + dx, y + dy)] * k;
						weight += k;
					}
				}
				result.level[i] = sum / weight;
				result.depth[i] = a_input.bed.size() == count ? std::max(result.level[i] - a_input.bed[i], 0.0f) : 0.0f;
			}
		}

		// The edge: how far each water cell is from rock or a bank, by breadth-first steps; and how far
		// from where the falls meet still water (where they come in and leave), which foams
		{
			const auto distanceFrom = [&](auto&& a_isSource, int32_t a_reach, std::vector<int32_t>& a_steps) {
				a_steps.assign(count, a_reach + 1);
				std::deque<std::pair<int32_t, int32_t>> frontier;
				for (int32_t y = 0; y < h; y++) {
					for (int32_t x = 0; x < w; x++) {
						if (a_isSource(x, y)) {
							a_steps[index(x, y)] = 0;
							frontier.emplace_back(x, y);
						}
					}
				}
				while (!frontier.empty()) {
					const auto [x, y] = frontier.front();
					frontier.pop_front();
					const int32_t next = a_steps[index(x, y)] + 1;
					if (next > a_reach)
						continue;
					for (int k = 0; k < 4; k++) {
						const int32_t nx = x + DX[k];
						const int32_t ny = y + DY[k];
						if (!inside(nx, ny) || a_steps[index(nx, ny)] <= next)
							continue;
						a_steps[index(nx, ny)] = next;
						frontier.emplace_back(nx, ny);
					}
				}
			};
			std::vector<int32_t> steps;
			distanceFrom([&](int32_t a_x, int32_t a_y) { return !open(a_x, a_y); }, EDGE_CELLS, steps);
			for (std::size_t i = 0; i < count; i++)
				result.edge[i] = result.water[i] ? std::clamp((static_cast<float>(steps[i]) - 0.5f) / static_cast<float>(EDGE_CELLS), 0.0f, 1.0f) : 0.0f;
			distanceFrom([&](int32_t a_x, int32_t a_y) { const auto kind = a_input.kind[index(a_x, a_y)]; return kind == Inflow || kind == Outflow; }, MEET_CELLS, steps);
			for (std::size_t i = 0; i < count; i++) {
				if (result.water[i])
					result.foam[i] = std::max(result.foam[i], std::clamp(1.0f - static_cast<float>(steps[i]) / static_cast<float>(MEET_CELLS + 1), 0.0f, 1.0f));
			}
		}

		// The travel time: each cell is reached from the point one cell upstream along the flow (fast
		// sweeping in four orders, so information crosses the grid in any direction)
		{
			constexpr float UNREACHED = 1.0e9f;
			result.travel.assign(count, UNREACHED);
			for (std::size_t i = 0; i < count; i++) {
				if (a_input.kind[i] == Inflow)
					result.travel[i] = 0.0f;
			}
			const auto sample = [&](float a_x, float a_y, float& a_value) {
				const auto ix = static_cast<int32_t>(std::floor(a_x));
				const auto iy = static_cast<int32_t>(std::floor(a_y));
				const float tx = a_x - static_cast<float>(ix);
				const float ty = a_y - static_cast<float>(iy);
				float sum = 0.0f, weight = 0.0f;
				for (int32_t dy = 0; dy <= 1; dy++) {
					for (int32_t dx = 0; dx <= 1; dx++) {
						if (!open(ix + dx, iy + dy))
							continue;
						const float value = result.travel[index(ix + dx, iy + dy)];
						if (value >= UNREACHED)
							continue;
						const float k = (dx ? tx : 1.0f - tx) * (dy ? ty : 1.0f - ty);
						sum += value * k;
						weight += k;
					}
				}
				if (weight < 1e-3f)
					return false;
				a_value = sum / weight;
				return true;
			};
			for (uint32_t round = 0; round < TRAVEL_ROUNDS; round++) {
				for (int order = 0; order < 4; order++) {
					const bool flipX = (order & 1) != 0;
					const bool flipY = (order & 2) != 0;
					for (int32_t yy = 0; yy < h; yy++) {
						const int32_t y = flipY ? h - 1 - yy : yy;
						for (int32_t xx = 0; xx < w; xx++) {
							const int32_t x = flipX ? w - 1 - xx : xx;
							const std::size_t i = index(x, y);
							if (!result.water[i] || a_input.kind[i] == Inflow)
								continue;
							const float speed = result.speed[i];
							if (speed < 1e-4f)
								continue;
							float upstream;
							if (!sample(static_cast<float>(x) - result.vx[i] / speed, static_cast<float>(y) - result.vy[i] / speed, upstream))
								continue;
							result.travel[i] = std::min(result.travel[i], upstream + cell / (BASE_SPEED * std::max(speed, SLOWEST)));
						}
					}
				}
			}
			// Water nothing reaches (cut off by rock from where it comes in) takes its neighbours' time
			for (int pass = 0; pass < 8; pass++) {
				for (int32_t y = 0; y < h; y++) {
					for (int32_t x = 0; x < w; x++) {
						const std::size_t i = index(x, y);
						if (!result.water[i] || result.travel[i] < UNREACHED)
							continue;
						for (int k = 0; k < 4; k++) {
							if (open(x + DX[k], y + DY[k]) && result.travel[index(x + DX[k], y + DY[k])] < UNREACHED) {
								result.travel[i] = result.travel[index(x + DX[k], y + DY[k])];
								break;
							}
						}
					}
				}
			}
			for (float& value : result.travel) {
				if (value >= UNREACHED)
					value = 0.0f;
			}
		}

		// The head (D33): the highest water surface upstream, less HEAD_LOSS per unit run, so water
		// that has dropped carries the speed of its fall (the shaders turn the drop below it into speed)
		{
			result.head = result.level;
			const auto sample = [&](float a_x, float a_y, float& a_value) {
				const auto ix = static_cast<int32_t>(std::floor(a_x));
				const auto iy = static_cast<int32_t>(std::floor(a_y));
				const float tx = a_x - static_cast<float>(ix);
				const float ty = a_y - static_cast<float>(iy);
				float sum = 0.0f, weight = 0.0f;
				for (int32_t dy = 0; dy <= 1; dy++) {
					for (int32_t dx = 0; dx <= 1; dx++) {
						if (!open(ix + dx, iy + dy) || !result.water[index(ix + dx, iy + dy)])
							continue;
						const float k = (dx ? tx : 1.0f - tx) * (dy ? ty : 1.0f - ty);
						// D55: water that has come through a pool starts again from its level: a fall's speed does not
						// carry past the pool it plunges into (the falls over the pool keep theirs)
						const std::size_t j = index(ix + dx, iy + dy);
						const bool pool = a_input.cover.size() == count && a_input.cover[j] >= result.level[j] - POOL_HEAD_BAND;
						sum += (pool ? result.level[j] : result.head[j]) * k;
						weight += k;
					}
				}
				if (weight < 1e-3f)
					return false;
				a_value = sum / weight;
				return true;
			};
			for (uint32_t round = 0; round < TRAVEL_ROUNDS; round++) {
				for (int order = 0; order < 4; order++) {
					const bool flipX = (order & 1) != 0;
					const bool flipY = (order & 2) != 0;
					for (int32_t yy = 0; yy < h; yy++) {
						const int32_t y = flipY ? h - 1 - yy : yy;
						for (int32_t xx = 0; xx < w; xx++) {
							const int32_t x = flipX ? w - 1 - xx : xx;
							const std::size_t i = index(x, y);
							const float speed = result.speed[i];
							if (!result.water[i] || speed < 1e-4f)
								continue;
							float upstream;
							if (sample(static_cast<float>(x) - result.vx[i] / speed, static_cast<float>(y) - result.vy[i] / speed, upstream))
								result.head[i] = std::clamp(upstream - HEAD_LOSS * cell, result.head[i], result.level[i] + HEAD_MOST);
						}
					}
				}
			}
		}
		return result;
	}
}
