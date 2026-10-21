#include "FallVisibility.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <future>
#include <numbers>
#include <thread>

namespace FluidFieldSim
{
	namespace
	{
		/** @brief A bounding volume hierarchy over triangles, built by median splits; asked only whether a segment meets any. */
		class Bvh
		{
		public:
			explicit Bvh(std::vector<FallVisibility::Triangle> a_triangles) :
				triangles(std::move(a_triangles))
			{
				order.resize(triangles.size());
				for (uint32_t i = 0; i < order.size(); i++)
					order[i] = i;
				centres.resize(triangles.size());
				for (std::size_t i = 0; i < triangles.size(); i++)
					centres[i] = (triangles[i][0] + triangles[i][1] + triangles[i][2]) / 3.0f;
				if (!triangles.empty()) {
					nodes.reserve(triangles.size() / LEAF * 2 + 1);
					Build(0, static_cast<uint32_t>(triangles.size()));
				}
			}

			/** @brief Whether the segment from a_origin along a_direction (unit) to a_length meets a triangle. */
			bool Hits(const RE::NiPoint3& a_origin, const RE::NiPoint3& a_direction, float a_length) const
			{
				if (nodes.empty())
					return false;
				const float inverse[3] = { 1.0f / a_direction.x, 1.0f / a_direction.y, 1.0f / a_direction.z };
				uint32_t stack[64];
				int top = 0;
				stack[top++] = 0;
				while (top > 0) {
					const Node& node = nodes[stack[--top]];
					if (!BoxHit(node, a_origin, inverse, a_length))
						continue;
					if (node.count > 0) {
						for (uint32_t i = node.first; i < node.first + node.count; i++) {
							if (TriangleHit(triangles[order[i]], a_origin, a_direction, a_length))
								return true;
						}
					} else if (top + 2 <= 64) {
						stack[top++] = node.left;
						stack[top++] = node.right;
					}
				}
				return false;
			}

		private:
			static constexpr uint32_t LEAF = 4;
			struct Node
			{
				float lo[3], hi[3];
				uint32_t first = 0;            ///< Leaf: the first of its triangles in order
				uint32_t count = 0;            ///< Leaf: its triangles; 0 for an inner node
				uint32_t left = 0, right = 0;  ///< Inner node: its children
			};

			uint32_t Build(uint32_t a_begin, uint32_t a_end)
			{
				const auto index = static_cast<uint32_t>(nodes.size());
				nodes.push_back({});
				Node node{};
				for (int k = 0; k < 3; k++) {
					node.lo[k] = FLT_MAX;
					node.hi[k] = -FLT_MAX;
				}
				for (uint32_t i = a_begin; i < a_end; i++) {
					for (const auto& corner : triangles[order[i]]) {
						const float c[3] = { corner.x, corner.y, corner.z };
						for (int k = 0; k < 3; k++) {
							node.lo[k] = std::min(node.lo[k], c[k]);
							node.hi[k] = std::max(node.hi[k], c[k]);
						}
					}
				}
				if (a_end - a_begin <= LEAF) {
					node.first = a_begin;
					node.count = a_end - a_begin;
					nodes[index] = node;
					return index;
				}
				int axis = 0;
				float extent = -1.0f;
				for (int k = 0; k < 3; k++) {
					if (node.hi[k] - node.lo[k] > extent) {
						extent = node.hi[k] - node.lo[k];
						axis = k;
					}
				}
				const uint32_t middle = a_begin + (a_end - a_begin) / 2;
				const auto at = [&](uint32_t a_i) { const auto& c = centres[a_i]; return axis == 0 ? c.x : axis == 1 ? c.y : c.z; };
				std::nth_element(order.begin() + a_begin, order.begin() + middle, order.begin() + a_end, [&](uint32_t a_a, uint32_t a_b) { return at(a_a) < at(a_b); });
				node.count = 0;
				nodes[index] = node;
				const uint32_t left = Build(a_begin, middle);
				const uint32_t right = Build(middle, a_end);
				nodes[index].left = left;
				nodes[index].right = right;
				return index;
			}

			static bool BoxHit(const Node& a_node, const RE::NiPoint3& a_origin, const float (&a_inverse)[3], float a_length)
			{
				const float o[3] = { a_origin.x, a_origin.y, a_origin.z };
				float t0 = 0.0f, t1 = a_length;
				for (int k = 0; k < 3; k++) {
					float enter = (a_node.lo[k] - o[k]) * a_inverse[k];
					float leave = (a_node.hi[k] - o[k]) * a_inverse[k];
					if (enter > leave)
						std::swap(enter, leave);
					t0 = std::max(t0, enter);
					t1 = std::min(t1, leave);
					if (t0 > t1)
						return false;
				}
				return true;
			}

			/** @brief Möller-Trumbore, both faces, within the segment. */
			static bool TriangleHit(const FallVisibility::Triangle& a_triangle, const RE::NiPoint3& a_origin, const RE::NiPoint3& a_direction, float a_length)
			{
				const RE::NiPoint3 e1 = a_triangle[1] - a_triangle[0];
				const RE::NiPoint3 e2 = a_triangle[2] - a_triangle[0];
				const RE::NiPoint3 p = a_direction.Cross(e2);
				const float det = e1.Dot(p);
				if (std::abs(det) < 1e-8f)
					return false;
				const float inverse = 1.0f / det;
				const RE::NiPoint3 s = a_origin - a_triangle[0];
				const float u = s.Dot(p) * inverse;
				if (u < 0.0f || u > 1.0f)
					return false;
				const RE::NiPoint3 q = s.Cross(e1);
				const float v = a_direction.Dot(q) * inverse;
				if (v < 0.0f || u + v > 1.0f)
					return false;
				const float t = e2.Dot(q) * inverse;
				return t > 0.0f && t < a_length;
			}

			std::vector<FallVisibility::Triangle> triangles;
			std::vector<RE::NiPoint3> centres;
			std::vector<uint32_t> order;
			std::vector<Node> nodes;
		};

		/** @brief Whether a segment dips more than the margin under the landscape (where its height is known). */
		bool UnderLand(const LandscapeCache* a_land, const RE::NiPoint3& a_origin, const RE::NiPoint3& a_direction, float a_length)
		{
			if (!a_land)
				return false;
			for (float t = FallVisibility::LAND_STEP * 0.5f; t < a_length; t += FallVisibility::LAND_STEP) {
				const RE::NiPoint3 p = a_origin + a_direction * t;
				float ground = 0.0f;
				if (a_land->SampleHeight(p.x, p.y, ground) && p.z < ground - FallVisibility::LAND_MARGIN)
					return true;
			}
			return false;
		}

		/** @brief How far a ray from a_origin along a_direction runs before it leaves the box a_lo..a_hi. */
		float ToBoxEdge(const RE::NiPoint3& a_origin, const RE::NiPoint3& a_direction, const float (&a_lo)[3], const float (&a_hi)[3])
		{
			const float o[3] = { a_origin.x, a_origin.y, a_origin.z };
			const float d[3] = { a_direction.x, a_direction.y, a_direction.z };
			float exit = FLT_MAX;
			for (int k = 0; k < 3; k++) {
				if (std::abs(d[k]) < 1e-6f)
					continue;
				exit = std::min(exit, ((d[k] > 0.0f ? a_hi[k] : a_lo[k]) - o[k]) / d[k]);
			}
			return std::max(exit, 0.0f);
		}
	}

	std::vector<std::vector<uint8_t>> FallVisibility::Visible(const std::vector<Sheet>& a_sheets, const std::vector<Triangle>& a_others, const LandscapeCache* a_land, uint64_t* o_tested,
		uint64_t* o_rays)
	{
		// Everything that can hide a vertex: every sheet's drawn triangles and the statics'; the box the rays must leave
		std::vector<Triangle> all = a_others;
		float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (const auto& sheet : a_sheets) {
			for (const auto& tri : *sheet.triangles) {
				all.push_back({ (*sheet.positions)[tri[0]], (*sheet.positions)[tri[1]], (*sheet.positions)[tri[2]] });
				for (const uint32_t k : tri) {
					const RE::NiPoint3& p = (*sheet.positions)[k];
					const float c[3] = { p.x, p.y, p.z };
					for (int a = 0; a < 3; a++) {
						lo[a] = std::min(lo[a], c[a]);
						hi[a] = std::max(hi[a], c[a]);
					}
				}
			}
		}
		std::vector<std::vector<uint8_t>> visible(a_sheets.size());
		for (std::size_t s = 0; s < a_sheets.size(); s++)
			visible[s].assign(a_sheets[s].positions->size(), 1);
		if (lo[0] > hi[0])
			return visible;
		for (int a = 0; a < 3; a++) {
			lo[a] -= REACH;
			hi[a] += REACH;
		}
		const Bvh bvh(std::move(all));

		// The directions: a Fibonacci lattice over the sphere
		std::vector<RE::NiPoint3> directions(DIRECTIONS);
		const float golden = std::numbers::pi_v<float> * (3.0f - std::sqrt(5.0f));
		for (int i = 0; i < DIRECTIONS; i++) {
			const float z = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(DIRECTIONS);
			const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
			directions[i] = RE::NiPoint3(std::cos(golden * static_cast<float>(i)) * r, std::sin(golden * static_cast<float>(i)) * r, z);
		}
		// Upward ones first: most vertices that can be seen are seen from above, so they stop early
		std::stable_sort(directions.begin(), directions.end(), [](const RE::NiPoint3& a_a, const RE::NiPoint3& a_b) { return a_a.z > a_b.z; });

		// The vertices some drawn triangle uses, over every sheet
		std::vector<std::pair<uint32_t, uint32_t>> work;
		for (std::size_t s = 0; s < a_sheets.size(); s++) {
			std::vector<uint8_t> used(a_sheets[s].positions->size(), 0);
			for (const auto& tri : *a_sheets[s].triangles) {
				for (const uint32_t k : tri)
					used[k] = 1;
			}
			for (uint32_t v = 0; v < used.size(); v++) {
				if (used[v])
					work.emplace_back(static_cast<uint32_t>(s), v);
			}
		}
		std::atomic<uint64_t> rays = 0;
		const auto run = [&](std::size_t a_begin, std::size_t a_end) {
			uint64_t cast = 0;
			for (std::size_t w = a_begin; w < a_end; w++) {
				const auto [s, v] = work[w];
				const RE::NiPoint3& p = (*a_sheets[s].positions)[v];
				bool seen = false;
				for (const auto& direction : directions) {
					const RE::NiPoint3 origin = p + direction * START;
					const float length = ToBoxEdge(origin, direction, lo, hi);
					cast++;
					if (!bvh.Hits(origin, direction, length) && !UnderLand(a_land, origin, direction, length)) {
						seen = true;
						break;
					}
				}
				visible[s][v] = seen ? 1 : 0;
			}
			rays += cast;
		};
		const unsigned threads = std::clamp(std::thread::hardware_concurrency() > 2 ? std::thread::hardware_concurrency() - 2 : 1u, 1u, 12u);
		std::vector<std::future<void>> parts;
		const std::size_t chunk = (work.size() + threads - 1) / threads;
		for (unsigned t = 0; t < threads; t++) {
			const std::size_t begin = t * chunk, end = std::min(work.size(), begin + chunk);
			if (begin < end)
				parts.push_back(std::async(std::launch::async, run, begin, end));
		}
		for (auto& part : parts)
			part.get();
		if (o_tested)
			*o_tested = work.size();
		if (o_rays)
			*o_rays = rays;
		return visible;
	}
}
