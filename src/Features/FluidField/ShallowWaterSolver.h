#pragma once

// Kept free of plugin headers (no PCH, globals or CommonLib), like WindSolver, so a standalone
// test can build and run it on a WARP device outside the game.

#include "WindSolver.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>
#include <span>

namespace FluidFieldSim
{
	/**
	 * @brief Water emitter, matching `Emitter` in WaterForcesCS.hlsl. Same layout as the wind
	 * `Emitter`, with its first padding float used as a depth rate.
	 */
	struct WaterEmitter
	{
		float position[2];  ///< Logical texel coordinates (texel centres at +0.5)
		float radius;       ///< Gaussian falloff radius in texels
		float depthRate;    ///< World units per second added to depth at the centre
		float velocity[2];  ///< Velocity the water near the emitter is pulled toward
		float pad1[2];      ///< Radial speed (water spreads out from the centre) and foam rate, for waterfalls (D22); zero otherwise
	};
	static_assert(sizeof(WaterEmitter) == 32);

	/** @brief Physical and blending parameters, matching `WaterCB` in WaterCommon.hlsli. */
	struct WaterParams
	{
		float gravity = 687.0f;     ///< 9.81 m/s^2 in Skyrim units (1 unit = 1.428 cm)
		float maxDepth = 300.0f;    ///< Depth used for transport is capped here
		float wetThreshold = 1.0f;  ///< Depth below which a cell is dry
		float current[2]{};         ///< Background current, world units per second
		float currentRelax = 0.5f;  ///< Per-second relaxation toward the current
		float levelRelax = 1.0f;    ///< Per-second relaxation toward the vanilla depth
		float edgeWidth = 16.0f;    ///< Texels of the absorbing border; set from the grid size each step
		float edgeRelax = 4.0f;     ///< Per-second pull toward calm vanilla water at the very edge (quadratic ramp inward)
		float damping = 0.2f;       ///< Fraction of velocity lost per second
		/// River currents from Unified Water's world flowmap (64 texels per cell, rows from the north);
		/// when set, they replace `current` as the velocity the water relaxes toward
		ID3D11ShaderResourceView* flowmap = nullptr;
		float flowmapCells[2]{};          ///< Flowmap size in cells
		std::int32_t flowmapOffset[2]{};  ///< Added to a cell's coordinates to get its flowmap tile
		float riverSpeed = 0.0f;          ///< World units per second at full flowmap strength
		float vorticity = 0.0f;           ///< Vorticity confinement strength (0: off)
		float foamStrength = 1.0f;        ///< Scales how much foam the flow makes (0: none)
		float foamMax = 1.5f;             ///< Most foam a cell holds (1 is fully white once drawn)
		/// Foam for cells coming into the grid: another level's flow output (foam in z), with its
		/// placement, so the near level takes over the far level's foam instead of starting clean
		ID3D11ShaderResourceView* seedFoam = nullptr;
		std::int32_t seedGridMin[2]{};
		std::uint32_t seedRes = 0;
		float seedCellSize = 0.0f;
		/// D44: where waterfalls plunge into the water, per texel 0 to 1 (FallSites::BuildPlungeMap); the water foams there
		ID3D11ShaderResourceView* plungeMap = nullptr;
		float plungeOrigin[2]{};
		float plungeInverseCell = 0.0f;
		std::uint32_t plungeSize = 0;
	};

	/** @brief Colour scales for the water debug view: a value of 1/scale maps to full intensity. */
	struct WaterDebugScales
	{
		float depth = 1.0f / 300.0f;
		float flow = 1.0f / 300.0f;
		float wave = 1.0f / 10.0f;
	};

	/**
	 * @brief Shallow water on the Fluid Field grid: depth and staggered velocity, advected
	 * semi-Lagrangian, with height and velocity updated from the surface slope
	 * (Bridson & Mueller-Fischer, "Fluid Simulation", SIGGRAPH 2007 course, heightfield chapter).
	 *
	 * The bed and the vanilla water surface come from a coarse world-aligned map uploaded by the
	 * caller. Owns every GPU resource it uses.
	 */
	class ShallowWaterSolver
	{
	public:
		static constexpr std::uint32_t MAX_EMITTERS = 96;  // Wakes, splashes and the falls' plunges (D35)
		static constexpr float NO_WATER = -1.0e8f;         ///< Map surface value meaning "no vanilla water here"

		using NameFn = WindSolver::NameFn;
		using CompileFn = WindSolver::CompileFn;

		/** @brief Creates the state, output, map and obstacle textures. Releases any previous ones first. */
		void CreateResources(ID3D11Device* device, std::uint32_t resolution, std::uint32_t mapResolution, std::uint32_t obstacleResolution, const NameFn& name);
		bool CompileShaders(const CompileFn& compile);
		void ReleaseShaders();
		void ReleaseResources();

		bool HasResources() const { return resolution != 0; }
		bool HasShaders() const;
		std::uint32_t GetResolution() const { return resolution; }
		std::uint32_t GetMapResolution() const { return mapResolution; }
		std::uint32_t GetObstacleResolution() const { return obstacleResolution; }

		/**
		 * @brief Uploads the bed and vanilla surface map: mapResolution^2 pairs (bed, surface), stored
		 * toroidally (the map texel of lattice point (x, y) is (x mod res, y mod res)).
		 */
		void UploadMap(ID3D11DeviceContext* context, std::span<const float> bedAndSurface, float mapSpacing);
		bool HasMap() const { return mapUploaded; }

		/**
		 * @brief Writes one square block of fine bed detail: size x size texels of (height, fine x, fine y, kind),
		 * rows south to north, starting at fine lattice point (originX, originY). originX and originY must be
		 * multiples of size, and size must divide the obstacle resolution, so a block never wraps.
		 */
		void UploadObstacleBlock(ID3D11DeviceContext* context, std::int32_t originX, std::int32_t originY, std::uint32_t size, const float* texels, float spacing);
		/** @brief Erases all fine detail. Needed once after creation or a reset before blocks are uploaded. */
		void ClearObstacles(ID3D11DeviceContext* context);
		bool ObstaclesNeedClear() const { return obstaclesNeedClear; }

		/** @brief Forgets all state and the map; after the next upload, the first step starts every cell from vanilla water. */
		void Reset();

		/**
		 * @brief Runs one sub-step: forces, advection, height and velocity.
		 * @param grid Grid placement and sub-step length (velocityDecay and jacobiIterations are ignored).
		 * @param wrap Called as `wrap(passName, runPass)` around each pass, for profiling zones.
		 */
		template <class Wrap>
		void Step(ID3D11DeviceContext* context, const StepParams& grid, const WaterParams& params, std::span<const WaterEmitter> emitters, Wrap&& wrap)
		{
			BeginStep(context, grid, params, emitters);
			wrap("Water Forces", [&] { Forces(context); });
			wrap("Water Advect", [&] { Advect(context); });
			wrap("Water Height", [&] { Height(context); });
			wrap("Water Velocity", [&] { Velocity(context); });
			wrap("Water Foam", [&] { Foam(context); });
			EndStep(context);
		}

		/** @brief Writes the surface and flow outputs from the latest state. */
		void WriteOutputs(ID3D11DeviceContext* context);

		/** @brief Renders the north-up debug images: depth, flow, waves. */
		void RenderDebugViews(ID3D11DeviceContext* context, const WaterDebugScales& scales);

		/** @brief (slope x, slope y, height above vanilla, coverage) per texel, in storage order. */
		ID3D11ShaderResourceView* GetSurfaceSRV() const { return surface.srv.get(); }
		/** @brief Cell-centred velocity (xy) and foam (z, 0 to about 1.5) per texel, in storage order. */
		ID3D11ShaderResourceView* GetFlowSRV() const { return flow.srv.get(); }

		static constexpr std::array<const char*, 3> DEBUG_VIEW_NAMES = { "Water Depth", "Water Flow", "Waves" };
		ID3D11ShaderResourceView* GetDebugViewSRV(std::size_t index) const { return debugViews[index].srv.get(); }
		bool HasDebugViews() const { return hasDebugViews; }

	private:
		struct Layer
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
			winrt::com_ptr<ID3D11UnorderedAccessView> uav;
		};

		// Matches FluidFieldCB in Common.hlsli
		struct alignas(16) GridConstants
		{
			std::int32_t gridMin[2];
			std::int32_t prevGridMin[2];
			std::uint32_t gridRes;
			float cellSize;
			float deltaTime;
			float velocityDecay;
			std::uint32_t historyValid;
			std::uint32_t emitterCount;
			std::uint32_t pad[2];
		};
		static_assert(sizeof(GridConstants) == 48);

		// Matches WaterCB in WaterCommon.hlsli
		struct alignas(16) WaterConstants
		{
			float gravity;
			float maxDepth;
			float maxSpeed;
			float wetThreshold;
			float current[2];
			float currentRelax;
			float levelRelax;
			std::uint32_t mapRes;
			float mapSpacing;
			float edgeWidth;
			float edgeRelax;
			std::uint32_t obstacleRes;
			float obstacleSpacing;
			float riverSpeed;
			std::uint32_t flowmapEnabled;
			float flowmapCells[2];
			std::int32_t flowmapOffset[2];
			float vorticity;
			float simTime;
			float foamStrength;
			float foamMax;
			std::int32_t seedGridMin[2];
			std::uint32_t seedRes;
			float seedCellSize;
			float plungeOrigin[2];
			float plungeInverseCell;
			std::uint32_t plungeSize;
		};
		static_assert(sizeof(WaterConstants) == 128);

		// Matches WaterDebugCB in WaterDebugViewCS.hlsl
		struct alignas(16) DebugConstants
		{
			float depthScale;
			float flowScale;
			float waveScale;
			float pad;
		};

		static Layer CreateLayer(ID3D11Device* device, std::uint32_t resolution, DXGI_FORMAT format, const NameFn& name, const char* label, bool unordered = true);
		static void UpdateBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer, const void* data, std::size_t size);

		void BeginStep(ID3D11DeviceContext* context, const StepParams& grid, const WaterParams& params, std::span<const WaterEmitter> emitters);
		void EndStep(ID3D11DeviceContext* context);
		void Forces(ID3D11DeviceContext* context);
		void Advect(ID3D11DeviceContext* context);
		void Height(ID3D11DeviceContext* context);
		void Velocity(ID3D11DeviceContext* context);
		void Foam(ID3D11DeviceContext* context);

		/** @brief Binds SRVs t0..t3 (plus the map at t4) and up to three UAVs, then dispatches over the whole grid. */
		void Dispatch(ID3D11DeviceContext* context, ID3D11ComputeShader* shader, std::span<ID3D11ShaderResourceView* const> srvs, std::span<ID3D11UnorderedAccessView* const> uavs);

		std::uint32_t resolution = 0;
		ID3D11ShaderResourceView* seedFoamSRV = nullptr;
		ID3D11ShaderResourceView* plungeSRV = nullptr;  ///< This step's plunge map (not owned)  ///< This step's seed foam (not owned)
		std::uint32_t mapResolution = 0;
		float mapSpacing = 64.0f;
		bool mapUploaded = false;
		std::uint32_t obstacleResolution = 0;
		float obstacleSpacing = 16.0f;
		bool obstaclesNeedClear = true;

		std::array<Layer, 2> depth;     // Ping-pong, `current` holds the latest
		std::array<Layer, 2> velocity;  // Ping-pong, `current` holds the latest
		std::array<Layer, 2> cellMap;   // Ping-pong: the forces pass compares the new bed with the previous one
		std::array<Layer, 2> foam;      // Ping-pong, `currentFoam` holds the latest
		Layer surface;
		Layer flow;
		Layer map;
		Layer obstacles;
		std::array<Layer, 3> debugViews;
		std::uint32_t currentDepth = 0;
		std::uint32_t currentVelocity = 0;
		std::uint32_t currentCellMap = 0;
		std::uint32_t currentFoam = 0;
		bool hasDebugViews = false;

		winrt::com_ptr<ID3D11Buffer> gridConstants;
		winrt::com_ptr<ID3D11Buffer> waterConstants;
		winrt::com_ptr<ID3D11Buffer> debugConstants;
		winrt::com_ptr<ID3D11Buffer> emitterBuffer;
		winrt::com_ptr<ID3D11ShaderResourceView> emitterSRV;

		winrt::com_ptr<ID3D11ComputeShader> forcesCS;
		winrt::com_ptr<ID3D11ComputeShader> advectCS;
		winrt::com_ptr<ID3D11ComputeShader> heightCS;
		winrt::com_ptr<ID3D11ComputeShader> velocityCS;
		winrt::com_ptr<ID3D11ComputeShader> outputCS;
		winrt::com_ptr<ID3D11ComputeShader> foamCS;
		winrt::com_ptr<ID3D11ComputeShader> debugViewCS;

		std::int32_t prevGridMin[2]{};
		float simTime = 0.0f;  ///< Seconds simulated since the last reset
		bool historyValid = false;
		bool stepped = false;  ///< A step ran since the last reset, so b0/b1 hold a valid placement
	};
}
