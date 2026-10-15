#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace FluidFieldSim
{
	/**
	 * @brief The ground under one waterfall site, and the lips its water pours over (D25, stages S4
	 * and S5 of `plans/waterfall-replacement.md`).
	 *
	 * S4, capture: a heightfield over the site's falling water plus MARGIN, CELL units per texel, from
	 * downward Havok rays that start CEILING above the site's top level (so bridges and arches over the
	 * river do not become dams) and see only rock, ground and statics. Cast a few at a time within a
	 * time budget; holes are filled from their neighbours and spikes removed with a 3x3 median.
	 *
	 * S5, lips: for each level the water pours from (every level but the bottom), the iso-contour of the
	 * ground at LIP_DEPTH below the level (the rock just under the water at the crest) by marching
	 * squares, kept where the ground on its downhill side drops more than MIN_DROP within DROP_PROBE:
	 * the edge the water falls from. Gorge walls, boulders and banks above deep pools pass that test
	 * too, so a lip must also lie within FALL_NEAR of the top edge of one of vanilla's falling-water
	 * meshes whose top is near the level: the mesh says roughly where water goes over, the ground says
	 * exactly. Chained into
	 * polylines; runs shorter than MIN_LIP are dropped. Each point carries the outward (downhill)
	 * direction.
	 *
	 * S6, tracing: a stream every STREAM_SPACING along each lip leaves it at V0 (the speed of water
	 * through critical depth at a free overfall) along the outward direction, and falls under gravity
	 * against the captured ground. Where it meets rock it slides, accelerated down the slope and slowed
	 * by friction, and falls free again where the rock drops away faster than it would fall. It ends in
	 * water at a lower level, when it stalls, or when it leaves the capture. Each path is resampled to
	 * STREAM_ROWS points by length, for drawing as a ribbon.
	 */
	class FallCapture
	{
	public:
		static constexpr float CELL = 16.0f;
		static constexpr float MARGIN = 256.0f;
		static constexpr float CEILING = 128.0f;
		static constexpr float BELOW = 512.0f;  ///< Rays reach this far below the site's bottom level
		static constexpr float LIP_DEPTH = 48.0f;
		static constexpr float MIN_DROP = 96.0f;
		static constexpr float DROP_PROBE = 64.0f;
		static constexpr float MIN_LIP = 64.0f;
		static constexpr float FALL_NEAR = 64.0f;         ///< How far (across) a lip may lie from a falling-water mesh's top edge
		static constexpr float FALL_NEAR_ABOVE = 64.0f;   ///< How far above a falling-water mesh's top its level may lie
		static constexpr float FALL_NEAR_BELOW = 128.0f;  ///< How far below it (meshes often start under the surface)
		static constexpr float NONE = -1.0e7f;

		static constexpr float GRAVITY = 687.0f;  ///< 9.81 m/s2 in world units (1 unit is about 1.43 cm)
		static constexpr float V0 = 90.0f;        ///< Speed over the lip, units/s
		static constexpr float STREAM_SPACING = 16.0f;
		static constexpr uint32_t STREAM_ROWS = 32;
		static constexpr float MIN_STREAM_DROP = 32.0f;  ///< Shorter paths are not drawn
		static constexpr float ROCK_FRICTION = 1.5f;     ///< Speed lost on rock, per second, as a fraction
		static constexpr float ROCK_OFFSET = 3.0f;       ///< How far above the rock sliding water runs
		static constexpr float STALL_SPEED = 20.0f;

		/** @brief One point of a traced stream, laid out for the sheet shader (Waterfall.hlsl CurtainPoint). */
		struct StreamPoint
		{
			float position[3];
			float along;       ///< 0 at the lip to 1 at the end
			float side[2];     ///< Horizontal direction across the stream
			float halfWidth;   ///< World units
			float speed;       ///< Units per second
			float across;      ///< -1 at one end of its lip to 1 at the other
			float onRock;      ///< 1 where it slides on rock
			float travelTime;  ///< Seconds from the lip
			float pad;
		};
		static_assert(sizeof(StreamPoint) == 48);
		/** @brief How a stream ended. */
		enum class StreamEnd : uint8_t
		{
			Pool,     ///< In water at a lower level
			Stalled,  ///< Came to rest on a ledge
			Outside   ///< Left the capture, or ran out of steps
		};
		struct LipPoint
		{
			float position[3];  ///< On the contour, at the level's height
			float normal[2];    ///< Outward: the way the water falls
		};
		struct Lip
		{
			float level;
			std::vector<LipPoint> points;
			float length;
		};
		/** @brief For the debug view: a piece of a level's shoreline (the ground's contour at the level), or of a lip. */
		struct Edge
		{
			float a[2];
			float b[2];
			float z;
			bool lip;
		};

		/** @brief Lays out the capture over a world box; rays run from a_top + CEILING down to a_bottom - BELOW. */
		void Begin(const float a_min[2], const float a_max[2], float a_top, float a_bottom);
		/** @brief Casts rays until a_budgetMs is used; cleans up once all are cast. True when complete. */
		bool Cast(double a_budgetMs);
		bool IsComplete() const { return complete; }
		float GetProgress() const { return heights.empty() ? 0.0f : static_cast<float>(next) / static_cast<float>(heights.size()); }

		/** @brief One of vanilla's falling-water meshes: its top edge's vertices across, and its top's height. */
		struct FallEdge
		{
			std::vector<std::array<float, 2>> edge;
			float top;
		};

		/** @brief Extracts the lips at each of a_levels (once complete), near the falling water in a_falls. */
		void ExtractLips(const std::vector<float>& a_levels, const std::vector<FallEdge>& a_falls);

		const std::vector<Lip>& GetLips() const { return lips; }
		const std::vector<Edge>& GetEdges() const { return edges; }
		/**
		 * @brief Traces a stream from every STREAM_SPACING of each lip (once the lips are extracted).
		 * @param a_levels The site's levels, highest first: a stream ends in the first below its lip whose water is there.
		 */
		void TraceStreams(const std::vector<float>& a_levels);
		/** @brief STREAM_ROWS points per stream, stream after stream. */
		const std::vector<StreamPoint>& GetStreams() const { return streams; }
		uint32_t GetStreamCount() const { return static_cast<uint32_t>(streams.size() / STREAM_ROWS); }
		/** @brief How many streams ended each way, indexed by StreamEnd. */
		const uint32_t* GetStreamEnds() const { return streamEnds; }
		/** @brief Which falling-water meshes (indices into ExtractLips' a_falls) have a lip: the ones replaced. */
		const std::vector<bool>& GetFallsWithLips() const { return fallsWithLips; }

		/** @brief The ground's height at a world position, bilinear; false outside the capture or where unknown. */
		bool Sample(float a_x, float a_y, float& a_height) const;

		float GetOriginX() const { return originX; }
		float GetOriginY() const { return originY; }
		uint32_t GetWidth() const { return width; }
		uint32_t GetHeight() const { return height; }
		float At(uint32_t a_x, uint32_t a_y) const { return heights[static_cast<std::size_t>(a_y) * width + a_x]; }

	private:
		void CleanUp();

		float originX = 0.0f;
		float originY = 0.0f;
		uint32_t width = 0;
		uint32_t height = 0;
		float top = 0.0f;
		float bottom = 0.0f;
		std::vector<float> heights;
		std::size_t next = 0;
		bool complete = false;
		std::vector<Lip> lips;
		std::vector<Edge> edges;
		std::vector<StreamPoint> streams;
		uint32_t streamEnds[3]{};
		std::vector<bool> fallsWithLips;
	};
}
