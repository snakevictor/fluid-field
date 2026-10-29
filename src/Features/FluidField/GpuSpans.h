#pragma once

#include <array>
#include <cstdint>
#include <winrt/base.h>

#include <d3d11.h>

namespace FluidFieldSim
{
	/**
	 * @brief D192: GPU timestamps for a few spans per frame; the built-in profiler cannot nest passes. Read a few frames
	 * late without stalling. A span past SLOW_MS is logged at once (the flush survives a following hang); each span's worst
	 * over LOG_SECONDS is logged if any passed NOTE_MS.
	 */
	class GpuSpans
	{
	public:
		enum Span : uint32_t
		{
			EngineWater,      ///< Deferred end to world end: the engine's water, including falls drawn by Water.hlsl
			RefractiveFalls,  ///< DrawRefractiveFalls: falls drawn by our own pass
			Simulation,       ///< Prepass: wind and water solvers, falls uploads
			Count
		};

		/** @brief GPU ms past which a span is logged at once. */
		static constexpr float SLOW_MS = 50.0f;
		/** @brief GPU ms past which the periodic summary is logged. */
		static constexpr float NOTE_MS = 8.0f;
		static constexpr float LOG_SECONDS = 10.0f;

		/** @brief Creates the queries (named for RenderDoc). */
		void Initialize(ID3D11Device* a_device);
		/** @brief Whether the queries exist. */
		bool IsReady() const { return ready; }
		/** @brief Releases the queries. */
		void Release();
		/** @brief Marks a span's start on the GPU timeline (ignored if it is already open this frame). */
		void Begin(ID3D11DeviceContext* a_context, Span a_span);
		/** @brief Marks a span's end; a span without its start this frame is ignored. */
		void End(ID3D11DeviceContext* a_context, Span a_span);
		/** @brief Once per frame: closes this frame's queries, reads finished frames and logs slow spans. */
		void EndFrame(ID3D11DeviceContext* a_context);

	private:
		static constexpr uint32_t FRAMES = 4;
		struct Frame
		{
			winrt::com_ptr<ID3D11Query> disjoint;
			std::array<winrt::com_ptr<ID3D11Query>, Count> begin, end;
			std::array<bool, Count> begun{}, ended{};
			bool open = false, inFlight = false;
			uint64_t number = 0;
		};
		std::array<Frame, FRAMES> frames;
		uint32_t current = 0;
		uint64_t frameNumber = 0;
		bool ready = false;
		std::array<float, Count> worst{};
		std::array<float, Count> last{};
		LARGE_INTEGER lastLog{};

		void Open(ID3D11DeviceContext* a_context);
		void Collect(ID3D11DeviceContext* a_context);
	};
}
