#include "GpuSpans.h"

#include "Utils/D3D.h"

namespace FluidFieldSim
{
	namespace
	{
		constexpr const char* SPAN_NAMES[GpuSpans::Count] = { "engine water and falls", "refractive falls", "falls spray", "simulation" };
	}

	void GpuSpans::Initialize(ID3D11Device* a_device)
	{
		Release();
		if (!a_device)
			return;
		for (uint32_t f = 0; f < FRAMES; f++) {
			auto& frame = frames[f];
			D3D11_QUERY_DESC disjoint{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			if (FAILED(a_device->CreateQuery(&disjoint, frame.disjoint.put())))
				return;
			Util::SetResourceName(frame.disjoint.get(), "FluidField::GpuSpans Disjoint");
			for (uint32_t s = 0; s < Count; s++) {
				D3D11_QUERY_DESC stamp{ D3D11_QUERY_TIMESTAMP, 0 };
				if (FAILED(a_device->CreateQuery(&stamp, frame.begin[s].put())) || FAILED(a_device->CreateQuery(&stamp, frame.end[s].put())))
					return;
				Util::SetResourceName(frame.begin[s].get(), "FluidField::GpuSpans Begin");
				Util::SetResourceName(frame.end[s].get(), "FluidField::GpuSpans End");
			}
		}
		QueryPerformanceCounter(&lastLog);
		ready = true;
	}

	void GpuSpans::Release()
	{
		ready = false;
		for (auto& frame : frames) {
			frame.disjoint = nullptr;
			for (auto& q : frame.begin)
				q = nullptr;
			for (auto& q : frame.end)
				q = nullptr;
			frame.open = frame.inFlight = false;
		}
	}

	void GpuSpans::Open(ID3D11DeviceContext* a_context)
	{
		auto& frame = frames[current];
		if (frame.open)
			return;
		// A frame still in flight from FRAMES ago (the GPU far behind): its results are dropped rather than waited for
		frame.inFlight = false;
		frame.begun.fill(false);
		frame.ended.fill(false);
		frame.number = frameNumber;
		a_context->Begin(frame.disjoint.get());
		frame.open = true;
	}

	void GpuSpans::Begin(ID3D11DeviceContext* a_context, Span a_span)
	{
		if (!ready || !a_context || a_span >= Count)
			return;
		Open(a_context);
		auto& frame = frames[current];
		if (frame.begun[a_span])
			return;
		a_context->End(frame.begin[a_span].get());
		frame.begun[a_span] = true;
	}

	void GpuSpans::End(ID3D11DeviceContext* a_context, Span a_span)
	{
		if (!ready || !a_context || a_span >= Count)
			return;
		auto& frame = frames[current];
		if (!frame.open || !frame.begun[a_span] || frame.ended[a_span])
			return;
		a_context->End(frame.end[a_span].get());
		frame.ended[a_span] = true;
	}

	void GpuSpans::EndFrame(ID3D11DeviceContext* a_context)
	{
		if (!ready || !a_context)
			return;
		auto& frame = frames[current];
		if (frame.open) {
			a_context->End(frame.disjoint.get());
			frame.open = false;
			frame.inFlight = true;
		}
		frameNumber++;
		current = (current + 1) % FRAMES;
		Collect(a_context);
	}

	void GpuSpans::Collect(ID3D11DeviceContext* a_context)
	{
		for (auto& frame : frames) {
			if (!frame.inFlight)
				continue;
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
			if (a_context->GetData(frame.disjoint.get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				continue;
			frame.inFlight = false;
			if (disjoint.Disjoint || disjoint.Frequency == 0)
				continue;
			for (uint32_t s = 0; s < Count; s++) {
				if (!frame.begun[s] || !frame.ended[s])
					continue;
				UINT64 begin = 0, end = 0;
				if (a_context->GetData(frame.begin[s].get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
					a_context->GetData(frame.end[s].get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || end < begin)
					continue;
				const float ms = static_cast<float>(static_cast<double>(end - begin) * 1000.0 / static_cast<double>(disjoint.Frequency));
				last[s] = ms;
				worst[s] = std::max(worst[s], ms);
				if (ms > SLOW_MS)
					logger::warn("[Fluid Field] GPU: {} took {:.1f} ms in frame {}", SPAN_NAMES[s], ms, frame.number);
			}
		}
		LARGE_INTEGER now, frequency;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&frequency);
		if (static_cast<double>(now.QuadPart - lastLog.QuadPart) / static_cast<double>(frequency.QuadPart) < LOG_SECONDS)
			return;
		lastLog = now;
		if (*std::max_element(worst.begin(), worst.end()) > NOTE_MS) {
			logger::info("[Fluid Field] GPU, worst over {:.0f} s: {} {:.1f} ms, {} {:.1f} ms, {} {:.1f} ms, {} {:.1f} ms", LOG_SECONDS, SPAN_NAMES[0], worst[0], SPAN_NAMES[1], worst[1],
				SPAN_NAMES[2], worst[2], SPAN_NAMES[3], worst[3]);
		}
		worst.fill(0.0f);
	}
}
