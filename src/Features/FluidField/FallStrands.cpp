#include "FallStrands.h"

#include <algorithm>
#include <cstddef>

namespace FluidFieldSim::FallStrands
{
	namespace
	{
		constexpr float CREST_RISE = 48.0f;  ///< D79: a column climbing more than this to its highest row starts there
	}

	uint32_t FindCrests(const Input& a_input, std::vector<int32_t>& a_crest, std::vector<uint8_t>& a_leadIn)
	{
		const auto w = static_cast<int32_t>(a_input.width), h = static_cast<int32_t>(a_input.height);
		a_crest.assign(static_cast<std::size_t>(std::max(w, 0)), -1);
		a_leadIn.assign(static_cast<std::size_t>(std::max(w, 0)) * std::max(h, 0), 0);
		if (!a_input.position || !a_input.covered)
			return 0;
		const auto index = [&](int32_t a_x, int32_t a_y) { return static_cast<std::size_t>(a_y) * w + a_x; };
		const auto height = [&](int32_t a_x, int32_t a_y) { return a_input.position[index(a_x, a_y) * 3 + 2]; };
		uint32_t texels = 0;
		for (int32_t x = 0; x < w; x++) {
			int32_t first = -1, top = -1;
			for (int32_t y = 0; y < h; y++) {
				if (!a_input.covered[index(x, y)])
					continue;
				first = first < 0 ? y : first;
				if (top < 0 || height(x, y) > height(x, top))
					top = y;
			}
			if (first < 0)
				continue;
			a_crest[x] = first;
			if (top > first && height(x, top) - height(x, first) > CREST_RISE) {
				a_crest[x] = top;
				for (int32_t y = first; y < top; y++) {
					if (a_input.covered[index(x, y)]) {
						a_leadIn[index(x, y)] = 1;
						texels++;
					}
				}
			}
		}
		return texels;
	}
}
