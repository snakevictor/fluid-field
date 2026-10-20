#include "FallStrands.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace FluidFieldSim::FallStrands
{
	namespace
	{
		constexpr float GRAVITY = 686.0f;        ///< World units per second squared (Skyrim's)
		constexpr float INFLOW = 250.0f;         ///< World units per second the water comes over a crest at
		constexpr float LIP_THICKNESS = 6.0f;    ///< World units of water coming over a crest (8.6 cm)
		constexpr float TERMINAL = 900.0f;       ///< World units per second a falling sheet tends to (air drag)
		constexpr float LEAST_SPEED = 150.0f;    ///< World units per second: on the flat the water behind pushes it on
		constexpr float FRICTION = 1.5f;         ///< Per second, the bed's friction, by how much the surface lies
		constexpr float CREST_RISE = 48.0f;      ///< D79: a column climbing more than this to its highest row starts there
		constexpr float BAND = 0.15f;            ///< Share of the mesh's rows its foot band is (the water leaves there)
		constexpr float VANILLA_SCROLL = 0.15f;  ///< Texture coordinates per second (D56)
		constexpr float LEAST_RATIO = 0.5f;      ///< The debug arrows' speed (vanilla's 1) at least...
		constexpr float MOST_RATIO = 1.6f;       ///< ...and at most this
		constexpr uint32_t MOST_STRANDS = 128;   ///< Strands per mesh at most (one per crest column below that)
		constexpr int32_t LOOK = 5;              ///< Rows ahead a strand sees rock coming (the pillow in front of it)
		constexpr float CLEAR = 1.2f;            ///< Texels a strand steers to pass rock by
		constexpr float LEAST_CLEAR = 0.5f;      ///< Texels: nearer than this it is pushed out
		constexpr float MOST_ESCAPE = 14.0f;     ///< Texels: rock wider than this either way lies under the sheet (it runs over)
		constexpr float MOST_TURN = 1.7320508f;  ///< The most a strand turns from straight down (60 degrees)
		constexpr float ACROSS_PULL = 0.35f;     ///< Share of gravity across the surface the strands feel (vanilla's UVs already run the right way; in full it rolled them off every dome)
		constexpr float HEAL = 0.6f;             ///< Seconds a strand turned aside takes to come back to its column
		constexpr float LEAST_SPACING = 0.5f;    ///< Strands press together to this share of their spacing at most (the streaks squeeze 2x)
		constexpr float OPEN_SPACING = 3.0f;     ///< A gap this many spacings wide (and 3 texels) is open: the wake behind rock
		constexpr float AERATION_FADE = 3.0f;    ///< Seconds aeration from strikes and pile-ups fades over
		constexpr float UNITS_PER_METRE = 70.0f;
		constexpr float NOWHERE = 1e6f;
		constexpr int SOFTEN = 3;              ///< Passes softening the streaks' bend on the texture grid
		constexpr float LEAST_STRETCH = 0.6f;  ///< The texture across stays within this much of vanilla's scale...
		constexpr float MOST_STRETCH = 1.6f;   ///< ...and this (pressed together, the strands squeezed it 3-6x into ripples)
		constexpr float MOST_SHEAR = 0.5f;     ///< Texels the streaks shift across per row at most
		constexpr int RELAX = 300;             ///< Passes easing the bend into those bounds
		constexpr float EDGE_FADE = 10.0f;     ///< Texels in from the mesh's sides over which the bend fades in (strands pressed along its ragged outline squeezed the texture into ripples)

		float Smoothstep(float a_from, float a_to, float a_x)
		{
			const float t = std::clamp((a_x - a_from) / (a_to - a_from), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		}

		/** @brief Squared distances to the nearest marked cell along a line (Felzenszwalb and Huttenlocher's lower envelope). */
		void DistanceLine(const float* a_in, float* a_out, int32_t a_n, std::vector<int32_t>& a_v, std::vector<float>& a_z)
		{
			int32_t k = 0;
			a_v[0] = 0;
			a_z[0] = -std::numeric_limits<float>::infinity();
			a_z[1] = std::numeric_limits<float>::infinity();
			for (int32_t q = 1; q < a_n; q++) {
				float s = 0.0f;
				while (true) {
					const int32_t p = a_v[k];
					s = ((a_in[q] + static_cast<float>(q * q)) - (a_in[p] + static_cast<float>(p * p))) / static_cast<float>(2 * q - 2 * p);
					if (s > a_z[k] || k == 0)
						break;
					k--;
				}
				if (s <= a_z[k]) {
					a_v[k] = q;
					a_z[k + 1] = std::numeric_limits<float>::infinity();
					continue;
				}
				k++;
				a_v[k] = q;
				a_z[k] = s;
				a_z[k + 1] = std::numeric_limits<float>::infinity();
			}
			k = 0;
			for (int32_t q = 0; q < a_n; q++) {
				while (a_z[k + 1] < static_cast<float>(q))
					k++;
				const auto d = static_cast<float>(q - a_v[k]);
				a_out[q] = d * d + a_in[a_v[k]];
			}
		}

		/** @brief Euclidean distance (texels) from each cell to the nearest marked one; NOWHERE with none. */
		std::vector<float> Distance(const std::vector<uint8_t>& a_mark, int32_t a_w, int32_t a_h)
		{
			constexpr float BIG = 1e12f;
			std::vector<float> grid(a_mark.size());
			for (std::size_t i = 0; i < a_mark.size(); i++)
				grid[i] = a_mark[i] ? 0.0f : BIG;
			const int32_t n = std::max(a_w, a_h);
			std::vector<float> in(n), out(n), z(n + 1);
			std::vector<int32_t> v(n);
			for (int32_t x = 0; x < a_w; x++) {
				for (int32_t y = 0; y < a_h; y++)
					in[y] = grid[static_cast<std::size_t>(y) * a_w + x];
				DistanceLine(in.data(), out.data(), a_h, v, z);
				for (int32_t y = 0; y < a_h; y++)
					grid[static_cast<std::size_t>(y) * a_w + x] = out[y];
			}
			for (int32_t y = 0; y < a_h; y++) {
				std::copy_n(grid.begin() + static_cast<std::ptrdiff_t>(y) * a_w, a_w, in.begin());
				DistanceLine(in.data(), out.data(), a_w, v, z);
				std::copy_n(out.begin(), a_w, grid.begin() + static_cast<std::ptrdiff_t>(y) * a_w);
			}
			for (float& value : grid)
				value = value >= BIG * 0.5f ? NOWHERE : std::sqrt(value);
			return grid;
		}

		struct Vec
		{
			float x = 0.0f, y = 0.0f, z = 0.0f;
			Vec operator+(const Vec& a_o) const { return { x + a_o.x, y + a_o.y, z + a_o.z }; }
			Vec operator-(const Vec& a_o) const { return { x - a_o.x, y - a_o.y, z - a_o.z }; }
			Vec operator*(float a_s) const { return { x * a_s, y * a_s, z * a_s }; }
			float Dot(const Vec& a_o) const { return x * a_o.x + y * a_o.y + z * a_o.z; }
			Vec Cross(const Vec& a_o) const { return { y * a_o.z - z * a_o.y, z * a_o.x - x * a_o.z, x * a_o.y - y * a_o.x }; }
			float Length() const { return std::sqrt(Dot(*this)); }
		};

		/** @brief A strand's state at a row. */
		struct Node
		{
			float x = 0.0f;       ///< Across, texels
			float across = 0.0f;  ///< Across speed, world units per second
			float down = 0.0f;    ///< Along speed, world units per second
			float time = 0.0f;    ///< Seconds since its crest
			float along = 0.0f;   ///< Texture v run since its crest, at the texture's speed
			float aeration = 0.0f;
			float fallen = 0.0f;  ///< World units fallen where steep (a sliding sheet does not break up by falling)
			float squeeze = 1.0f;
			float strike = 0.0f;  ///< World units per second of across speed lost against rock this row
			float spacing = 1.0f;
			int side = 0;  ///< Which way it passes the rock ahead (-1 left, 1 right)
			bool alive = false;
		};
	}

	Output March(const Input& a_input)
	{
		Output result;
		const auto w = static_cast<int32_t>(a_input.width), h = static_cast<int32_t>(a_input.height);
		const std::size_t count = static_cast<std::size_t>(w) * h;
		result.seed.assign(count, 0.0f);
		result.along.assign(count, 0.0f);
		result.aeration.assign(count, 0.0f);
		result.foam.assign(count, 0.0f);
		result.turn.assign(count, 0.0f);
		result.speed.assign(count, 1.0f);
		result.crest.assign(static_cast<std::size_t>(std::max(w, 0)), -1);
		result.leadIn.assign(count, 0);
		if (w < 2 || h < 2 || !a_input.position || !a_input.covered || !a_input.rock)
			return result;
		const auto index = [&](int32_t a_x, int32_t a_y) { return static_cast<std::size_t>(a_y) * w + a_x; };
		const auto inside = [&](int32_t a_x, int32_t a_y) { return a_x >= 0 && a_y >= 0 && a_x < w && a_y < h; };
		const auto* covered = a_input.covered;
		const auto point = [&](std::size_t a_i) { return Vec{ a_input.position[a_i * 3], a_input.position[a_i * 3 + 1], a_input.position[a_i * 3 + 2] }; };
		const float texelV = std::max(a_input.texelV, 1e-6f);

		// The mesh's frame per texel (world across and down one texel), filled off the mesh from the nearest texel on it
		std::vector<Vec> tu(count), tv(count);
		for (int32_t y = 0; y < h; y++) {
			for (int32_t x = 0; x < w; x++) {
				const std::size_t i = index(x, y);
				if (!covered[i])
					continue;
				const auto side = [&](int32_t a_dx, int32_t a_dy) {
					const bool f = inside(x + a_dx, y + a_dy) && covered[index(x + a_dx, y + a_dy)];
					const bool b = inside(x - a_dx, y - a_dy) && covered[index(x - a_dx, y - a_dy)];
					if (f && b)
						return (point(index(x + a_dx, y + a_dy)) - point(index(x - a_dx, y - a_dy))) * 0.5f;
					if (f)
						return point(index(x + a_dx, y + a_dy)) - point(i);
					if (b)
						return point(i) - point(index(x - a_dx, y - a_dy));
					return Vec{};
				};
				tu[i] = side(1, 0);
				tv[i] = side(0, 1);
			}
		}
		std::vector<int32_t> source(count, -1);
		{
			std::vector<std::size_t> frontier;
			for (std::size_t i = 0; i < count; i++) {
				if (covered[i]) {
					source[i] = static_cast<int32_t>(i);
					frontier.push_back(i);
				}
			}
			if (frontier.empty())
				return result;
			for (std::size_t head = 0; head < frontier.size(); head++) {
				const std::size_t i = frontier[head];
				const auto x = static_cast<int32_t>(i % w), y = static_cast<int32_t>(i / w);
				static constexpr int32_t DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
				for (int k = 0; k < 4; k++) {
					if (!inside(x + DX[k], y + DY[k]) || source[index(x + DX[k], y + DY[k])] >= 0)
						continue;
					source[index(x + DX[k], y + DY[k])] = source[i];
					frontier.push_back(index(x + DX[k], y + DY[k]));
				}
			}
		}
		// Per texel: world units per row (down) and per texel across, gravity down and across the surface, how much
		// the surface lies, its height, and vanilla's speed there (its texture's scroll)
		std::vector<float> perRow(count), perColumn(count), pullDown(count), pullAcross(count), lying(count), height(count), vanilla(count);
		for (std::size_t i = 0; i < count; i++) {
			const auto s = static_cast<std::size_t>(source[i]);
			const Vec u = tu[s], v = tv[s];
			const float lv = std::max(v.Length(), 1e-3f);
			const Vec ev = v * (1.0f / lv);
			const Vec perpendicular = u - ev * u.Dot(ev);
			const float lu = std::max(perpendicular.Length(), 1e-3f);
			const Vec ea = perpendicular * (1.0f / lu);
			const Vec n = u.Cross(v);
			const float nl = n.Length();
			perRow[i] = lv;
			perColumn[i] = lu;
			pullDown[i] = -GRAVITY * ev.z;
			pullAcross[i] = -GRAVITY * ea.z;
			lying[i] = nl > 1e-6f ? std::abs(n.z) / nl : 1.0f;
			height[i] = point(s).z;
			vanilla[i] = VANILLA_SCROLL * lv / texelV;
		}

		// D79: each column's crest (vanilla's sheets climb a hidden face under the upper water to their lip); the
		// mesh's extent, and per column its first and last texel
		std::vector<int32_t> first(w, -1), last(w, -1);
		float footZ = std::numeric_limits<float>::max(), topZ = -std::numeric_limits<float>::max();
		int32_t rowTop = h, rowBottom = -1;
		for (int32_t x = 0; x < w; x++) {
			int32_t top = -1;
			for (int32_t y = 0; y < h; y++) {
				const std::size_t i = index(x, y);
				if (!covered[i])
					continue;
				first[x] = first[x] < 0 ? y : first[x];
				last[x] = y;
				if (top < 0 || height[i] > height[index(x, top)])
					top = y;
				footZ = std::min(footZ, height[i]);
				topZ = std::max(topZ, height[i]);
				rowTop = std::min(rowTop, y);
				rowBottom = std::max(rowBottom, y);
			}
			if (first[x] < 0)
				continue;
			result.crest[x] = first[x];
			if (top > first[x] && height[index(x, top)] - height[index(x, first[x])] > CREST_RISE) {
				result.crest[x] = top;
				for (int32_t y = first[x]; y < top; y++) {
					if (covered[index(x, y)]) {
						result.leadIn[index(x, y)] = 1;
						result.leadTexels++;
					}
				}
			}
		}
		const float band = std::max(64.0f, (topZ - footZ) * 0.2f);
		const int32_t rows = std::max(3, static_cast<int32_t>(static_cast<float>(rowBottom - rowTop) * BAND));

		// Rock, its one-texel slits closed (strands squeezing through them flipped sides); off the mesh, the foot
		// band is where the water leaves (into the pool), anything else a wall
		std::vector<uint8_t> rock(count, 0), grown(count, 0), solid(count, 0), exit(count, 0);
		for (int32_t y = 0; y < h; y++) {
			for (int32_t x = 0; x < w; x++) {
				for (int32_t dy = -1; dy <= 1 && !grown[index(x, y)]; dy++) {
					for (int32_t dx = -1; dx <= 1; dx++) {
						if (inside(x + dx, y + dy) && a_input.rock[index(x + dx, y + dy)] && covered[index(x + dx, y + dy)]) {
							grown[index(x, y)] = 1;
							break;
						}
					}
				}
			}
		}
		for (int32_t y = 0; y < h; y++) {
			for (int32_t x = 0; x < w; x++) {
				const std::size_t i = index(x, y);
				bool all = grown[i] != 0;
				for (int32_t dy = -1; dy <= 1 && all; dy++) {
					for (int32_t dx = -1; dx <= 1; dx++)
						all = all && (!inside(x + dx, y + dy) || grown[index(x + dx, y + dy)]);
				}
				rock[i] = covered[i] && (a_input.rock[i] || all) ? 1 : 0;
				if (!covered[i]) {
					const bool foot = last[x] >= 0 && (last[x] >= rowBottom - rows || height[index(x, last[x])] < footZ + band);
					exit[i] = (y > last[x] && foot && last[x] >= 0) || y >= rowBottom - rows ? 1 : 0;
				}
				solid[i] = rock[i] || (!covered[i] && !exit[i]) ? 1 : 0;
			}
		}
		// Distance to the solid (texels, positive in the water), softened; per row the nearest clear texel each way
		std::vector<float> clearance(count);
		{
			std::vector<uint8_t> free(count);
			for (std::size_t i = 0; i < count; i++)
				free[i] = solid[i] ? 0 : 1;
			const auto toSolid = Distance(solid, w, h), toFree = Distance(free, w, h);
			for (std::size_t i = 0; i < count; i++)
				clearance[i] = solid[i] ? 0.5f - toFree[i] : toSolid[i] - 0.5f;
			static constexpr float KERNEL[3] = { 0.212f, 0.576f, 0.212f };
			std::vector<float> scratch(count);
			for (int pass = 0; pass < 2; pass++) {
				for (int32_t y = 0; y < h; y++) {
					for (int32_t x = 0; x < w; x++) {
						float sum = 0.0f;
						for (int k = -1; k <= 1; k++) {
							const int32_t px = pass == 0 ? std::clamp(x + k, 0, w - 1) : x, py = pass == 1 ? std::clamp(y + k, 0, h - 1) : y;
							sum += KERNEL[k + 1] * clearance[index(px, py)];
						}
						scratch[index(x, y)] = sum;
					}
				}
				clearance.swap(scratch);
			}
		}
		const auto scan = [&](float a_clear, std::vector<float>& a_left, std::vector<float>& a_right) {
			a_left.assign(count, -NOWHERE);
			a_right.assign(count, NOWHERE);
			for (int32_t y = 0; y < h; y++) {
				float seen = -NOWHERE;
				for (int32_t x = 0; x < w; x++) {
					seen = clearance[index(x, y)] >= a_clear ? static_cast<float>(x) : seen;
					a_left[index(x, y)] = seen;
				}
				seen = NOWHERE;
				for (int32_t x = w - 1; x >= 0; x--) {
					seen = clearance[index(x, y)] >= a_clear ? static_cast<float>(x) : seen;
					a_right[index(x, y)] = seen;
				}
			}
		};
		std::vector<float> leftClear, rightClear, leftOut, rightOut;
		scan(CLEAR, leftClear, rightClear);
		scan(LEAST_CLEAR, leftOut, rightOut);
		const auto sample = [&](const std::vector<float>& a_field, float a_x, float a_y) {
			const float cx = std::clamp(std::isfinite(a_x) ? a_x : 0.0f, 0.0f, static_cast<float>(w) - 1.001f);
			const float cy = std::clamp(std::isfinite(a_y) ? a_y : 0.0f, 0.0f, static_cast<float>(h) - 1.001f);
			const auto x0 = static_cast<int32_t>(cx), y0 = static_cast<int32_t>(cy);
			const float fx = cx - static_cast<float>(x0), fy = cy - static_cast<float>(y0);
			return (a_field[index(x0, y0)] * (1.0f - fx) + a_field[index(x0 + 1, y0)] * fx) * (1.0f - fy) +
			       (a_field[index(x0, y0 + 1)] * (1.0f - fx) + a_field[index(x0 + 1, y0 + 1)] * fx) * fy;
		};
		const auto nearest = [&](const std::vector<float>& a_field, float a_x, int32_t a_y) {
			const auto x = static_cast<int32_t>(std::lround(std::clamp(std::isfinite(a_x) ? a_x : 0.0f, 0.0f, static_cast<float>(w - 1))));
			return a_field[index(x, std::clamp(a_y, 0, h - 1))];
		};

		// The strands: one per crest column (at most MOST_STRANDS), spread evenly in world width along the crests, so each
		// carries the same water; every column's crest feeds (columns starting low on a dome's side included: fed from the
		// top band only, they stayed bare)
		// (a crest inside rock: from the first water below it, the water coming round the rock)
		std::vector<int32_t> columns;
		for (int32_t x = 0; x < w; x++) {
			int32_t c = result.crest[x];
			while (c >= 0 && c < h && covered[index(x, c)] && rock[index(x, c)])
				c++;
			if (c >= 0 && c < h && covered[index(x, c)]) {
				result.crest[x] = c;
				columns.push_back(x);
			}
		}
		result.inflows = static_cast<uint32_t>(columns.size());
		const auto strands = static_cast<int32_t>(std::min<std::size_t>(columns.size(), MOST_STRANDS));
		std::vector<float> lip(strands), spacing(strands, 1.0f);
		std::vector<int32_t> start(strands);
		float width = 0.0f;
		if (strands >= 2) {
			std::vector<float> run(columns.size() + 1, 0.0f);
			for (std::size_t c = 0; c < columns.size(); c++)
				run[c + 1] = run[c] + perColumn[index(columns[c], result.crest[columns[c]])];
			width = run.back();
			for (int32_t k = 0; k < strands; k++) {
				const float target = (static_cast<float>(k) + 0.5f) / static_cast<float>(strands) * width;
				const auto c = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(std::upper_bound(run.begin(), run.end(), target) - run.begin() - 1, 0, static_cast<std::ptrdiff_t>(columns.size()) - 1));
				const float f = (target - run[c]) / std::max(run[c + 1] - run[c], 1e-3f);
				lip[k] = static_cast<float>(columns[c]) - 0.5f + f;
				start[k] = result.crest[columns[c]];
			}
			// Each strand's own spacing (texels), as at its crest; across a rock at the crest, at most 3 typical spacings
			std::vector<float> gaps(strands);
			for (int32_t k = 0; k < strands; k++)
				gaps[k] = k == 0 ? lip[1] - lip[0] : (k == strands - 1 ? lip[k] - lip[k - 1] : 0.5f * (lip[k + 1] - lip[k - 1]));
			std::vector<float> sorted = gaps;
			std::nth_element(sorted.begin(), sorted.begin() + strands / 2, sorted.end());
			const float typical = std::max(sorted[strands / 2], 0.05f);
			for (int32_t k = 0; k < strands; k++)
				spacing[k] = std::clamp(gaps[k], 0.05f, 3.0f * typical);
		}
		result.strands = static_cast<uint32_t>(std::max(strands, 0));
		if (strands < 2) {
			for (std::size_t i = 0; i < count; i++)
				result.seed[i] = static_cast<float>(i % w);
			return result;
		}
		// Where a sheet breaks up falling (Horeni; Castillo): about 6 q^0.32 metres for its discharge per metre of width
		const float discharge = LIP_THICKNESS * INFLOW / (UNITS_PER_METRE * UNITS_PER_METRE);
		const float breakup = 6.0f * std::pow(discharge, 0.32f) * UNITS_PER_METRE;
		const float heal = 2.0f / HEAL;
		const float drag = GRAVITY / (TERMINAL * TERMINAL);
		const auto strandCount = static_cast<std::size_t>(strands);
		result.nodeX.assign(strandCount * h, 0.0f);
		result.nodeAlive.assign(strandCount * h, 0);
		result.nodeThickness.assign(strandCount * h, 0.0f);
		std::vector<Node> nodes(strandCount * h);
		std::vector<Node> now(strandCount);
		std::vector<uint8_t> ended(strandCount, 0);
		const float strandDischarge = LIP_THICKNESS * INFLOW * width / static_cast<float>(strands);
		const int32_t firstRow = *std::min_element(start.begin(), start.end());

		// One row down: carried by its speeds, pulled by gravity along and across the surface, slowed by the bed (where
		// it lies) and the air, drawn back to its column; steered round the rock it sees coming
		const auto advance = [&](Node& a_node, int32_t a_k, int32_t a_row) {
			const float mid = static_cast<float>(a_row) - 0.5f;
			const float lv = sample(perRow, a_node.x, mid), lu = sample(perColumn, a_node.x, mid);
			const float flat = sample(lying, a_node.x, mid);
			const float floor = std::max(LEAST_SPEED, LEAST_RATIO * sample(vanilla, a_node.x, mid));
			const float down = std::max(a_node.down, floor);
			const float dt = lv / down;
			float nextDown = std::max(floor, down + dt * (sample(pullDown, a_node.x, mid) - FRICTION * flat * down - drag * down * down));
			const float pull = ACROSS_PULL * sample(pullAcross, a_node.x, mid) + heal * heal * (lip[a_k] - a_node.x) * lu - 2.0f * heal * a_node.across;
			float nextAcross = (a_node.across + dt * pull) / (1.0f + dt * FRICTION * flat);
			float x = a_node.x + 0.5f * (a_node.across + nextAcross) * dt / lu;
			// The rock ahead: the first row within LOOK where its way comes nearer than CLEAR; it turns toward the
			// nearer way round (keeping its side near the middle), a share of the way each row (all at once zig-zagged)
			const float slope = nextAcross / nextDown * lv / lu;
			int32_t ahead = 0;
			for (int32_t k = 1; k <= LOOK && !ahead; k++) {
				if (sample(clearance, x + slope * static_cast<float>(k), static_cast<float>(a_row + k)) < CLEAR)
					ahead = k;
			}
			if (ahead) {
				const float hitX = x + slope * static_cast<float>(ahead);
				const float left = nearest(leftClear, hitX, a_row + ahead), right = nearest(rightClear, hitX, a_row + ahead);
				const float toLeft = hitX - left, toRight = right - hitX;
				if (std::min(toLeft, toRight) <= MOST_ESCAPE) {
					int choose = toLeft < toRight ? -1 : 1;
					if (a_node.side != 0 && std::abs(toLeft - toRight) < 1.5f)
						choose = a_node.side;
					const float target = choose < 0 ? left - 0.5f : right + 0.5f;
					const float wanted = (target - x) * lu * nextDown / (static_cast<float>(ahead) * lv);
					const float steered = nextAcross + (wanted - nextAcross) / static_cast<float>(ahead);
					nextAcross = choose > 0 ? std::max(nextAcross, steered) : std::min(nextAcross, steered);
					a_node.side = choose;
				}
			} else {
				a_node.side = 0;
			}
			// In the rock after all: out to the nearer side (its side if it chose one); the speed into the rock is lost
			// (the strike: foam, spray). Over rock wider than MOST_ESCAPE it runs on over it, roughened
			float strike = 0.0f;
			if (sample(clearance, x, static_cast<float>(a_row)) < LEAST_CLEAR) {
				const float left = nearest(leftOut, x, a_row), right = nearest(rightOut, x, a_row);
				int prefer = a_node.side != 0 ? a_node.side : (x - left < right - x ? -1 : 1);
				prefer = prefer < 0 && left < -NOWHERE * 0.1f ? 1 : (prefer > 0 && right > NOWHERE * 0.1f ? -1 : prefer);
				if (std::min(x - left, right - x) <= MOST_ESCAPE) {
					x = prefer < 0 ? left : right;
					strike = prefer > 0 ? std::max(-nextAcross, 0.0f) : std::max(nextAcross, 0.0f);
					nextAcross = prefer > 0 ? std::max(nextAcross, 0.0f) : std::min(nextAcross, 0.0f);
				} else {
					strike = 0.05f * nextDown;
				}
			}
			nextAcross = std::clamp(nextAcross, -MOST_TURN * nextDown, MOST_TURN * nextDown);
			// Falling (the drop where the surface is steep) breaks the sheet up; strikes and pressing together aerate it
			const float drop = std::max(sample(height, a_node.x, static_cast<float>(a_row - 1)) - sample(height, x, static_cast<float>(a_row)), 0.0f);
			a_node.fallen += drop * std::clamp((0.8f - flat) / 0.6f, 0.0f, 1.0f);
			const float stirred = a_node.aeration * std::exp(-dt / AERATION_FADE) + dt * 1.5f * std::max(a_node.squeeze - 1.5f, 0.0f) + 0.6f * strike / std::max(nextDown, 1.0f);
			a_node.aeration = std::clamp(std::max(Smoothstep(0.3f * breakup, breakup, a_node.fallen), stirred), 0.0f, 1.0f);
			// The texture keeps vanilla's speed down the mesh (its UVs carry the artist's speeding up down the drops;
			// retimed by the physics it stretched and squeezed): the strands only bend it
			a_node.along += texelV;
			a_node.time += dt;
			a_node.x = x;
			a_node.across = nextAcross;
			a_node.down = nextDown;
			a_node.strike = strike;
		};

		std::vector<int32_t> order;
		order.reserve(strandCount);
		for (int32_t y = firstRow; y < h; y++) {
			for (int32_t k = 0; k < strands; k++) {
				Node& node = now[k];
				if (node.alive && !ended[k] && start[k] < y) {
					advance(node, k, y);
					const auto ex = static_cast<int32_t>(std::lround(std::clamp(node.x, 0.0f, static_cast<float>(w - 1))));
					if (exit[index(ex, y)] || y == h - 1) {
						node.alive = false;
						ended[k] = 1;
						result.reachedFoot++;
					}
				} else if (start[k] == y) {
					node = Node{};
					node.x = lip[k];
					node.down = INFLOW;
					node.along = static_cast<float>(y - rowTop) * texelV;  // From the mesh's top: one run for all, never back up
					node.spacing = spacing[k];
					node.alive = true;
				}
			}
			// Across the row: never closer than LEAST_SPACING of their spacing (pressed together they share their
			// across speed: the pile-up), and back out of any rock that pushed them into it
			order.clear();
			for (int32_t k = 0; k < strands; k++) {
				if (now[k].alive)
					order.push_back(k);
			}
			for (int iteration = 0; iteration < 4; iteration++) {
				for (std::size_t o = 1; o < order.size(); o++) {
					Node& a = now[order[o - 1]];
					Node& b = now[order[o]];
					const float need = 0.5f * LEAST_SPACING * (spacing[order[o - 1]] + spacing[order[o]]) - (b.x - a.x);
					if (need <= 0.0f)
						continue;
					a.x -= 0.5f * need;
					b.x += 0.5f * need;
					a.across = b.across = 0.5f * (a.across + b.across);
				}
				if (iteration == 3)
					break;
				for (const int32_t k : order) {
					Node& node = now[k];
					if (sample(clearance, node.x, static_cast<float>(y)) >= LEAST_CLEAR * 0.5f)
						continue;
					const float left = nearest(leftOut, node.x, y), right = nearest(rightOut, node.x, y);
					if (std::min(node.x - left, right - node.x) <= MOST_ESCAPE)
						node.x = node.x - left < right - node.x ? left : right;
				}
			}
			// Each strand's share of the width: from its neighbours, not across an open gap (the wake); pressed
			// together it thickens
			for (std::size_t o = 0; o < order.size(); o++) {
				Node& node = now[order[o]];
				const float own = spacing[order[o]];
				const float open = std::max(3.0f, OPEN_SPACING * own);
				const float gapLeft = o > 0 ? node.x - now[order[o - 1]].x : NOWHERE;
				const float gapRight = o + 1 < order.size() ? now[order[o + 1]].x - node.x : NOWHERE;
				const bool openLeft = gapLeft > open, openRight = gapRight > open;
				float share = openLeft && openRight ? own : (openLeft ? gapRight : (openRight ? gapLeft : 0.5f * (gapLeft + gapRight)));
				share = std::max(share, 0.05f);
				node.spacing = share;
				node.squeeze = own / share;
			}
			for (int32_t k = 0; k < strands; k++) {
				const std::size_t n = static_cast<std::size_t>(k) * h + y;
				nodes[n] = now[k];
				result.nodeAlive[n] = now[k].alive ? 1 : 0;
				result.nodeX[n] = now[k].x;
				if (now[k].alive) {
					const float lu = sample(perColumn, now[k].x, static_cast<float>(y));
					result.nodeThickness[n] = strandDischarge / (std::max(now[k].down, 1.0f) * now[k].spacing * lu);
					result.nodes++;
					result.longestRun = std::max(result.longestRun, now[k].time);
				}
			}
		}

		// Onto the texture grid, a row at a time: each texel between the two strands either side of it (beyond the
		// outermost, carried straight on from it); rows no strand reaches carry on straight down
		for (int32_t y = 0; y < h; y++) {
			order.clear();
			for (int32_t k = 0; k < strands; k++) {
				if (y >= firstRow && result.nodeAlive[static_cast<std::size_t>(k) * h + y])
					order.push_back(k);
			}
			std::stable_sort(order.begin(), order.end(), [&](int32_t a_a, int32_t a_b) { return nodes[static_cast<std::size_t>(a_a) * h + y].x < nodes[static_cast<std::size_t>(a_b) * h + y].x; });
			for (int32_t x = 0; x < w; x++) {
				const std::size_t i = index(x, y);
				const int32_t crest = result.crest[x];
				if (result.leadIn[i] || order.empty()) {
					if (y == 0 || y < firstRow || result.leadIn[i] || (crest >= 0 && y <= crest)) {
						result.seed[i] = static_cast<float>(x);
						result.along[i] = static_cast<float>(y - rowTop) * texelV;
					} else {
						result.seed[i] = result.seed[i - w];
						result.along[i] = result.along[i - w] + texelV;
						result.aeration[i] = result.aeration[i - w];
					}
					continue;
				}
				const auto px = static_cast<float>(x);
				const auto at = [&](std::size_t a_o) -> const Node& { return nodes[static_cast<std::size_t>(order[a_o]) * h + y]; };
				std::size_t right = 0;
				while (right < order.size() && at(right).x < px)
					right++;
				const auto fill = [&](const Node& a_node, float a_weight, float a_lip) {
					result.seed[i] += a_lip * a_weight;
					result.along[i] += a_node.along * a_weight;
					result.aeration[i] += a_node.aeration * a_weight;
					result.foam[i] += std::clamp(1.5f * a_node.strike / std::max(a_node.down, 1.0f) + 0.5f * std::max(a_node.squeeze - 1.5f, 0.0f), 0.0f, 1.0f) * a_weight;
					result.turn[i] += a_node.across / std::max(a_node.down, 1.0f) * sample(perRow, a_node.x, static_cast<float>(y)) / sample(perColumn, a_node.x, static_cast<float>(y)) * a_weight;
					result.speed[i] += std::clamp(a_node.down / std::max(sample(vanilla, a_node.x, static_cast<float>(y)), 1.0f), LEAST_RATIO, MOST_RATIO) * a_weight;
				};
				result.speed[i] = 0.0f;
				if (right == 0 || right == order.size()) {
					const std::size_t o = right == 0 ? 0 : order.size() - 1;
					fill(at(o), 1.0f, lip[order[o]] + (px - at(o).x));
					continue;
				}
				const Node& a = at(right - 1);
				const Node& b = at(right);
				const float gap = b.x - a.x;
				// An open gap (the wake behind rock): each side's streaks carry straight on from its edge strand, at their
				// own spacing (spread over the gap they stretched 4x), meeting in a foamed seam where the two flows join
				if (gap > std::max(3.0f, OPEN_SPACING * 0.5f * (a.spacing + b.spacing))) {
					const bool nearA = px - a.x < b.x - px;
					fill(nearA ? a : b, 1.0f, lip[order[nearA ? right - 1 : right]] + (px - (nearA ? a.x : b.x)));
					const float seam = std::abs(px - 0.5f * (a.x + b.x));
					result.foam[i] = std::max(result.foam[i], std::clamp(1.5f - seam * 0.5f, 0.0f, 1.0f));
					result.aeration[i] = std::max(result.aeration[i], 0.5f);
					if (covered[i] && !rock[i])
						result.openTexels++;
					continue;
				}
				const float f = gap > 1e-4f ? std::clamp((px - a.x) / gap, 0.0f, 1.0f) : 0.0f;
				fill(a, 1.0f - f, lip[order[right - 1]]);
				fill(b, f, lip[order[right]]);
			}
		}
		// Softened (the offset from straight down): a strand's step from texel to texel stepped the texture's scale
		// and mip there, a blocky pattern at the bake's resolution
		{
			std::vector<float> offset(count), scratch(count);
			std::vector<uint8_t> off(count);
			for (std::size_t i = 0; i < count; i++)
				off[i] = covered[i] ? 0 : 1;
			const auto toSide = Distance(off, w, h);
			for (std::size_t i = 0; i < count; i++)
				offset[i] = (result.seed[i] - static_cast<float>(i % w)) * Smoothstep(1.5f, EDGE_FADE, toSide[i]);
			// Eased into the bounds: each pair of neighbours out of them moves halfway back, both ways, many times over
			for (int pass = 0; pass < RELAX; pass++) {
				for (int32_t y = 0; y < h; y++) {
					for (int32_t x = 1; x < w; x++) {
						float& a = offset[index(x - 1, y)];
						float& b = offset[index(x, y)];
						const float step = b - a, bounded = std::clamp(step, LEAST_STRETCH - 1.0f, MOST_STRETCH - 1.0f);
						a += 0.5f * (step - bounded);
						b -= 0.5f * (step - bounded);
					}
				}
				for (int32_t y = 1; y < h; y++) {
					for (int32_t x = 0; x < w; x++) {
						if (result.leadIn[index(x, y)] != result.leadIn[index(x, y - 1)])
							continue;
						float& a = offset[index(x, y - 1)];
						float& b = offset[index(x, y)];
						const float step = b - a, bounded = std::clamp(step, -MOST_SHEAR, MOST_SHEAR);
						a += 0.5f * (step - bounded);
						b -= 0.5f * (step - bounded);
					}
				}
			}
			for (int pass = 0; pass < SOFTEN; pass++) {
				for (int32_t y = 0; y < h; y++) {
					for (int32_t x = 0; x < w; x++) {
						float total = 0.0f, weight = 0.0f;
						for (int32_t dy = -1; dy <= 1; dy++) {
							for (int32_t dx = -1; dx <= 1; dx++) {
								if (!inside(x + dx, y + dy) || result.leadIn[index(x + dx, y + dy)] != result.leadIn[index(x, y)])
									continue;
								const float k = (dx == 0 ? 2.0f : 1.0f) * (dy == 0 ? 2.0f : 1.0f);
								total += offset[index(x + dx, y + dy)] * k;
								weight += k;
							}
						}
						scratch[index(x, y)] = total / weight;
					}
				}
				offset.swap(scratch);
			}
			for (std::size_t i = 0; i < count; i++)
				result.seed[i] = static_cast<float>(i % w) + offset[i];
		}
		return result;
	}
}
