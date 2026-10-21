#include "RiverCapture.h"

#include "ShaderCache.h"

namespace FluidFieldSim
{
	bool RiverCapture::IsRiverDraw(const RE::BSShader* a_shader, uint32_t a_pixelDescriptor)
	{
		if (!a_shader || a_shader->shaderType.get() != RE::BSShader::Type::Water)
			return false;
		// Technique 0: SPECULAR with no specular lights (the lit surface; 1-7 are the point lights' passes, 8+ underwater,
		// LOD, stencil and simple); and the Refractions flag, which the falls' composite (the scene behind) needs
		const uint32_t technique = (a_pixelDescriptor >> 11) & 0xF;
		const auto refractions = static_cast<uint32_t>(SIE::ShaderCache::WaterShaderFlags::Refractions);
		const auto falls = static_cast<uint32_t>(SIE::ShaderCache::WaterShaderFlags::FluidFieldFalls);
		return technique == 0 && (a_pixelDescriptor & refractions) != 0 && (a_pixelDescriptor & falls) == 0;
	}

	bool RiverCapture::CopyBuffer(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11Buffer* a_source, winrt::com_ptr<ID3D11Buffer>& o_copy, bool& o_bound)
	{
		o_bound = false;
		if (!a_source)
			return true;  // Nothing bound there: bound as nothing
		D3D11_BUFFER_DESC source{};
		a_source->GetDesc(&source);
		if (o_copy) {
			D3D11_BUFFER_DESC existing{};
			o_copy->GetDesc(&existing);
			if (existing.ByteWidth != source.ByteWidth)
				o_copy = nullptr;
		}
		if (!o_copy) {
			D3D11_BUFFER_DESC desc{ .ByteWidth = source.ByteWidth, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_CONSTANT_BUFFER, .CPUAccessFlags = 0, .MiscFlags = 0, .StructureByteStride = 0 };
			if (FAILED(a_device->CreateBuffer(&desc, nullptr, o_copy.put())))
				return false;
			Util::SetResourceName(o_copy.get(), "FluidField::RiverCaptureBuffer");
		}
		a_context->CopyResource(o_copy.get(), a_source);
		o_bound = true;
		return true;
	}

	bool RiverCapture::Capture(ID3D11Device* a_device, ID3D11DeviceContext* a_context, RE::BSShader* a_shader, uint32_t a_vertexDescriptor, uint32_t a_pixelDescriptor, uint32_t a_frame)
	{
		triedFrame = a_frame;
		ReleaseViews();
		if (!a_device || !a_context || !a_shader)
			return false;

		// The constant buffers' contents: the engine's buffers are shared by every draw of their size and rewritten by the next
		ID3D11Buffer* pixel[PIXEL_BUFFER_COUNT]{};
		ID3D11Buffer* vertex[VERTEX_BUFFER_COUNT]{};
		a_context->PSGetConstantBuffers(0, PIXEL_BUFFER_COUNT, pixel);
		a_context->VSGetConstantBuffers(0, VERTEX_BUFFER_COUNT, vertex);
		bool ok = true;
		for (UINT i = 0; i < PIXEL_BUFFER_COUNT; i++)
			ok = CopyBuffer(a_device, a_context, pixel[i], pixelBuffers[i], pixelBound[i]) && ok;
		for (UINT i = 0; i < VERTEX_BUFFER_COUNT; i++)
			ok = CopyBuffer(a_device, a_context, vertex[i], vertexBuffers[i], vertexBound[i]) && ok;
		for (auto* buffer : pixel) {
			if (buffer)
				buffer->Release();
		}
		for (auto* buffer : vertex) {
			if (buffer)
				buffer->Release();
		}
		if (!ok || !pixelBound[0] || !pixelBound[1] || !vertexBound[1])
			return false;

		// Every view and sampler, referenced until the falls are drawn (or the next frame's water)
		a_context->PSGetShaderResources(0, VIEW_COUNT, reinterpret_cast<ID3D11ShaderResourceView**>(views.data()));
		a_context->PSGetSamplers(0, SAMPLER_COUNT, reinterpret_cast<ID3D11SamplerState**>(samplers.data()));

		shader = a_shader;
		vertexDescriptor = a_vertexDescriptor;
		pixelDescriptor = a_pixelDescriptor;
		capturedFrame = a_frame;
		captured = true;
		return true;
	}

	void RiverCapture::ReleaseViews()
	{
		for (auto& view : views)
			view = nullptr;
		for (auto& sampler : samplers)
			sampler = nullptr;
		captured = false;
	}

	void RiverCapture::Release()
	{
		ReleaseViews();
		for (auto& buffer : pixelBuffers)
			buffer = nullptr;
		for (auto& buffer : vertexBuffers)
			buffer = nullptr;
		pixelBound = {};
		vertexBound = {};
		shader = nullptr;
		capturedFrame = UINT32_MAX;
		triedFrame = UINT32_MAX;
	}

	void RiverCapture::GetViews(ID3D11ShaderResourceView* (&o_views)[VIEW_COUNT]) const
	{
		for (UINT i = 0; i < VIEW_COUNT; i++)
			o_views[i] = views[i].get();
	}

	void RiverCapture::GetSamplers(ID3D11SamplerState* (&o_samplers)[SAMPLER_COUNT]) const
	{
		for (UINT i = 0; i < SAMPLER_COUNT; i++)
			o_samplers[i] = samplers[i].get();
	}

	void RiverCapture::GetPixelBuffers(ID3D11Buffer* (&o_buffers)[PIXEL_BUFFER_COUNT]) const
	{
		for (UINT i = 0; i < PIXEL_BUFFER_COUNT; i++)
			o_buffers[i] = pixelBound[i] ? pixelBuffers[i].get() : nullptr;
	}

	void RiverCapture::GetVertexBuffers(ID3D11Buffer* (&o_buffers)[VERTEX_BUFFER_COUNT]) const
	{
		for (UINT i = 0; i < VERTEX_BUFFER_COUNT; i++)
			o_buffers[i] = vertexBound[i] ? vertexBuffers[i].get() : nullptr;
	}
}
