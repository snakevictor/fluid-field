#pragma once

#include "ShallowWaterSolver.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace FluidFieldSim
{
	class WaterSurfaces;

	/**
	 * @brief Waterfalls that drive the simulated water where they land (D22).
	 *
	 * Vanilla's falling water stays as it is: its body meshes (FXWaterfallBody*, FXWaterfallThin*)
	 * are found among the loaded references, and each one's plunge (where its lowest vertices are),
	 * width and fall direction are read once from its geometry, with its bounds as a fallback. Bodies
	 * landing on water are merged into falls, and each fall becomes a water emitter: a pulsing push
	 * at the plunge, a current spreading out from it and running on downstream, and foam. Runs on the
	 * main thread.
	 */
	class WaterfallSources
	{
	public:
		/** @brief Streams across a fall and points down each (D23); every stream is traced on its own. */
		static constexpr uint32_t CURTAIN_COLUMNS = 12;
		static constexpr uint32_t CURTAIN_ROWS = 40;

		/** @brief One point of a stream, matching `CurtainPoint` in Waterfall.hlsl. */
		struct CurtainPoint
		{
			float position[3];  ///< World units
			float along;        ///< 0 at the lip to 1 at the end of the fall
			float side[2];      ///< Horizontal direction across the stream here (unit length)
			float halfWidth;    ///< Half the stream's width, world units
			float speed;        ///< How fast the water moves here, world units per second
			float across;       ///< -1 at one edge of the fall to 1 at the other (where its stream started)
			float onRock;       ///< 1 where the water lies on rock, 0 where it falls free
			float travelTime;   ///< Seconds the water takes to get here from the lip
			float pad;
		};
		static_assert(sizeof(CurtainPoint) == 48);

		/** @brief A waterfall landing on water. */
		struct Fall
		{
			float plunge[3]{};     ///< Where it meets the water, world units (z is the water surface)
			float width = 0.0f;    ///< Width of the falling water at the plunge, world units
			float direction[2]{};  ///< Horizontal direction it falls toward (unit length), or zero
		};

		/**
		 * @brief Rescans the loaded waterfalls around a point every few updates.
		 * @param a_replace Hide vanilla's bodies and build curtains for them (D23).
		 */
		void Update(const WaterSurfaces& a_water, float a_centreX, float a_centreY, float a_halfSize, bool a_replace);

		/** @brief The falls being replaced: CURTAIN_COLUMNS streams of CURTAIN_ROWS points each per fall, rows first. */
		const std::vector<CurtainPoint>& GetCurtains() const { return curtainPoints; }
		/** @brief Changes whenever the curtains do, for re-uploading. */
		uint64_t GetCurtainVersion() const { return curtainVersion; }
		/** @brief Shows vanilla's hidden bodies again. */
		void RestoreBodies();

		/**
		 * @brief This step's emitters for one water level, in its logical texel coordinates.
		 * @param a_time Seconds since the start, for the pulsing.
		 * @param a_strength Scales every effect (0: none).
		 */
		void AppendEmitters(std::vector<WaterEmitter>& a_emitters, const int32_t a_gridMin[2], float a_cellSize, uint32_t a_gridRes, float a_time, float a_strength,
			uint32_t a_maxEmitters, bool a_foam) const;

		void Clear();

		const std::vector<Fall>& GetFalls() const { return falls; }

	private:
		/** @brief A body's shape, read once while its 3D is loaded. */
		struct Body
		{
			float plunge[3]{};
			float width = 0.0f;
			float direction[2]{};
			float lowest = 0.0f;  ///< Lowest point of the body, world units
			float lip[3]{};       ///< Centre of the top edge, where the water leaves the lip
			float lipWidth = 0.0f;
			std::vector<CurtainPoint> curtain;  ///< Built once (D23); empty if it could not be
			RE::ObjectRefHandle handle;
			bool hidden = false;
		};

		/** @brief Updates between scans of the loaded references. */
		static constexpr uint32_t SCAN_INTERVAL = 60;
		/** @brief Most falls kept, nearest first. */
		static constexpr std::size_t MAX_FALLS = 6;

		static bool ReadBody(RE::TESObjectREFR& a_ref, Body& a_body);
		/** @brief The curtain's shape: ballistic columns from the lip that lie on the rock where they meet it. */
		static void BuildCurtain(Body& a_body);

		std::vector<CurtainPoint> curtainPoints;
		uint64_t curtainVersion = 0;

		std::unordered_map<uint32_t, Body> bodies;  ///< Keyed by form ID
		std::vector<Fall> falls;
		uint32_t updatesUntilScan = 0;
	};
}
