#pragma once

// Kept free of plugin headers (no PCH, globals or CommonLib) so the solver test
// in tests/FluidField can build and run it on a WARP device outside the game.

#include <d3d11.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>
#include <functional>
#include <span>

namespace FluidFieldSim
{
	/** @brief A wind impulse, matching `Emitter` in AddForcesCS.hlsl. */
	struct Emitter
	{
		float position[2];  ///< Logical texel coordinates (texel centres at +0.5)
		float radius;       ///< Gaussian falloff radius in texels
		float pad0;         ///< Unused by the wind; the fog pass reads it as the actor's speed
		float velocity[2];  ///< World units per second, added once
		float pad1[2];
	};
	static_assert(sizeof(Emitter) == 32);

	/** @brief Inputs for the fog layer (D21), matching FogCB in FogCS.hlsl. */
	struct FogParams
	{
		float drift[2]{};              ///< Weather wind carrying the fog, world units per second
		float refillRate = 0.25f;      ///< Per-second rate at which displaced fog settles back
		float stirRate = 0.5f;         ///< Per-second rate at which fully stirred air thins the fog
		float stirSpeed = 120.0f;      ///< Wind speed counted as fully stirred, world units per second
		float displaceSpeed = 150.0f;  ///< Speed at which an actor's body shoves the fog outward, world units per second
		float maxFog = 2.0f;           ///< Most fog a cell can be packed to, as a multiple of the resting amount
		float pad = 0.0f;
	};
	static_assert(sizeof(FogParams) == 32);

	/** @brief Inputs for one simulation step. */
	struct StepParams
	{
		std::int32_t gridMin[2]{};   ///< World cell of logical texel (0, 0)
		float cellSize = 16.0f;      ///< World units per texel
		float deltaTime = 0.0f;      ///< Seconds to simulate
		float velocityDecay = 1.0f;  ///< Multiplier applied after advection
		std::uint32_t jacobiIterations = 30;
		std::span<const Emitter> emitters;
		const FogParams* fog = nullptr;  ///< Steps the fog layer too when set
	};

	/** @brief Colour scales for the debug view: a value of 1/scale maps to full intensity. */
	struct DebugViewScales
	{
		float velocity = 1.0f / 500.0f;
		float divergence = 1.0f;
		float pressure = 1.0f / 1000.0f;
	};

	/**
	 * @brief 2D stable-fluids wind layer on a toroidally addressed, scrolling grid.
	 *
	 * Owns every GPU resource it uses. Each step runs add forces, advect, divergence,
	 * a Jacobi pressure solve and projection, as separate compute dispatches.
	 */
	class WindSolver
	{
	public:
		static constexpr std::uint32_t MAX_EMITTERS = 16;

		/** @brief Names a D3D11 object for RenderDoc. */
		using NameFn = std::function<void(ID3D11DeviceChild*, const char*)>;
		/** @brief Compiles a compute shader from a file under the FluidField shader folder; returns an owned reference or null. */
		using CompileFn = std::function<ID3D11ComputeShader*(const wchar_t* fileName)>;

		/** @brief Creates the layer textures and buffers. Releases any previous ones first. */
		void CreateResources(ID3D11Device* device, std::uint32_t resolution, const NameFn& name);
		/** @brief Compiles every pass. Returns false if any pass failed, in which case the solver stays inactive. */
		bool CompileShaders(const CompileFn& compile);
		void ReleaseShaders();
		void ReleaseResources();

		bool HasResources() const { return resolution != 0; }
		bool HasShaders() const;
		std::uint32_t GetResolution() const { return resolution; }

		/** @brief Clears every layer; the next step starts from rest whatever the scroll. */
		void Reset(ID3D11DeviceContext* context);

		/**
		 * @brief Runs one simulation step.
		 * @param wrap Called as `wrap(passName, runPass)` around each pass, for profiling zones.
		 */
		template <class Wrap>
		void Step(ID3D11DeviceContext* context, const StepParams& params, Wrap&& wrap)
		{
			BeginStep(context, params);
			wrap("Add Forces", [&] { AddForces(context); });
			wrap("Advect", [&] { Advect(context); });
			wrap("Divergence", [&] { Divergence(context); });
			wrap("Pressure Solve", [&] { SolvePressure(context, params.jacobiIterations); });
			wrap("Project", [&] { Project(context); });
			if (params.fog)
				wrap("Fog", [&] { Fog(context, *params.fog); });
			EndStep(context);
		}

		/** @brief Runs one step with no profiling. */
		void Step(ID3D11DeviceContext* context, const StepParams& params)
		{
			Step(context, params, [](const char*, auto&& runPass) { runPass(); });
		}

		/** @brief Renders the north-up debug images of every layer. */
		void RenderDebugViews(ID3D11DeviceContext* context, const DebugViewScales& scales);

		ID3D11Texture2D* GetVelocityTexture() const { return velocity[current].texture.get(); }
		ID3D11Texture2D* GetDivergenceTexture() const { return divergence.texture.get(); }
		ID3D11Texture2D* GetPressureTexture() const { return pressure[pressureResult].texture.get(); }

		ID3D11ShaderResourceView* GetVelocitySRV() const { return velocity[current].srv.get(); }
		/** @brief Fog density multiplier (D21): 1 is the weather's fog, 0 clear air. Toroidal like the velocity. */
		ID3D11ShaderResourceView* GetFogSRV() const { return fog[currentFog].srv.get(); }

		/** @brief Debug images, in the order velocity, divergence, pressure. */
		static constexpr std::array<const char*, 3> DEBUG_VIEW_NAMES = { "Velocity", "Divergence", "Pressure" };
		ID3D11ShaderResourceView* GetDebugViewSRV(std::size_t index) const { return debugViews[index].srv.get(); }

	private:
		struct Layer
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
			winrt::com_ptr<ID3D11UnorderedAccessView> uav;
		};

		// Matches FluidFieldCB in Common.hlsli
		struct alignas(16) Constants
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
		static_assert(sizeof(Constants) == 48);

		// Matches DebugViewCB in DebugViewCS.hlsl
		struct alignas(16) DebugConstants
		{
			float velocityScale;
			float divergenceScale;
			float pressureScale;
			float pad;
		};

		static Layer CreateLayer(ID3D11Device* device, std::uint32_t resolution, DXGI_FORMAT format, const NameFn& name, const char* label);

		void BeginStep(ID3D11DeviceContext* context, const StepParams& params);
		void EndStep(ID3D11DeviceContext* context);
		void AddForces(ID3D11DeviceContext* context);
		void Advect(ID3D11DeviceContext* context);
		void Divergence(ID3D11DeviceContext* context);
		void SolvePressure(ID3D11DeviceContext* context, std::uint32_t iterations);
		void Project(ID3D11DeviceContext* context);
		void Fog(ID3D11DeviceContext* context, const FogParams& params);

		/** @brief Binds one SRV set and one UAV, then dispatches over the whole grid. */
		void Dispatch(ID3D11DeviceContext* context, ID3D11ComputeShader* shader, std::span<ID3D11ShaderResourceView* const> srvs, ID3D11UnorderedAccessView* uav);

		std::uint32_t resolution = 0;

		std::array<Layer, 2> velocity;  // Ping-pong, `current` holds the latest result
		std::array<Layer, 2> pressure;  // Ping-pong within the Jacobi solve
		Layer divergence;
		std::array<Layer, 3> debugViews;
		std::array<Layer, 2> fog;  // Ping-pong, `currentFog` holds the latest result
		std::uint32_t currentFog = 0;
		std::uint32_t current = 0;
		std::uint32_t pressureResult = 0;

		winrt::com_ptr<ID3D11Buffer> constants;
		winrt::com_ptr<ID3D11Buffer> debugConstants;
		winrt::com_ptr<ID3D11Buffer> fogConstants;
		winrt::com_ptr<ID3D11Buffer> emitterBuffer;
		winrt::com_ptr<ID3D11ShaderResourceView> emitterSRV;

		winrt::com_ptr<ID3D11ComputeShader> addForcesCS;
		winrt::com_ptr<ID3D11ComputeShader> advectCS;
		winrt::com_ptr<ID3D11ComputeShader> divergenceCS;
		winrt::com_ptr<ID3D11ComputeShader> jacobiCS;
		winrt::com_ptr<ID3D11ComputeShader> projectCS;
		winrt::com_ptr<ID3D11ComputeShader> debugViewCS;
		winrt::com_ptr<ID3D11ComputeShader> fogCS;

		// Scroll and reset state carried between steps
		std::int32_t prevGridMin[2]{};
		bool historyValid = false;
	};
}
