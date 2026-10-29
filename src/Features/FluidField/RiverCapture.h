#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>

namespace FluidFieldSim
{
	/**
	 * @brief D159: the river's draw state, captured once a frame, so the see-through falls are shaded by the engine's own
	 * water pixel shader (Water.hlsl with FLUID_FIELD_FALLS) exactly as the river is.
	 *
	 * One capture per water type, so each fall gets its own type's fog and glint. Taken in State::Draw at the type's first
	 * refracting-surface draw: constant buffers are copied (the engine's are shared and rewritten by every draw); views and
	 * samplers are referenced until the falls are drawn, so engine textures are not held across frames.
	 */
	class RiverCapture
	{
	public:
		static constexpr UINT VIEW_COUNT = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;  ///< t0-t127
		static constexpr UINT SAMPLER_COUNT = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;      ///< s0-s15
		static constexpr UINT PIXEL_BUFFER_COUNT = 5;                                     ///< b0 PerTechnique, b1 PerMaterial, b2 PerGeometry, b3 Light Limit Fix's, b4 Permutation
		static constexpr UINT VERTEX_BUFFER_COUNT = 2;                                    ///< b0 PerTechnique, b1 PerMaterial (normal map scroll and scale, vanilla fog)

		/** @brief Whether a draw with this shader and these (CS-modified) descriptors is the river's refracting surface. */
		static bool IsRiverDraw(const RE::BSShader* a_shader, uint32_t a_pixelDescriptor);

		/** @brief Captures the draw now bound, once per frame (a_frame: State's frame count). False if it could not. */
		bool Capture(ID3D11Device* a_device, ID3D11DeviceContext* a_context, RE::BSShader* a_shader, uint32_t a_vertexDescriptor, uint32_t a_pixelDescriptor, uint32_t a_frame);
		/** @brief Whether a draw of frame a_frame was captured (and its references are still held). */
		bool IsCaptured(uint32_t a_frame) const { return captured && capturedFrame == a_frame; }
		/** @brief Whether this frame's one capture attempt was made (taken or failed). */
		bool WasTried(uint32_t a_frame) const { return triedFrame == a_frame; }
		/** @brief Drops the references to the engine's views and samplers (our buffer copies are kept for reuse). */
		void ReleaseViews();
		void Release();

		RE::BSShader* GetShader() const { return shader; }
		/** @brief D171: the river piece (its geometry) the capture was taken at, set by the caller. */
		const void* GetGeometry() const { return geometry; }
		void SetGeometry(const void* a_geometry) { geometry = a_geometry; }
		/** @brief D171: our copy of its PerMaterial (b1: colours, fog, glint), kept past the frame, or null. */
		ID3D11Buffer* GetMaterialBuffer() const { return shader && pixelBound[1] ? pixelBuffers[1].get() : nullptr; }
		uint32_t GetVertexDescriptor() const { return vertexDescriptor; }
		uint32_t GetPixelDescriptor() const { return pixelDescriptor; }
		/** @brief t0-t127 as the river had them (entries may be null). */
		void GetViews(ID3D11ShaderResourceView* (&o_views)[VIEW_COUNT]) const;
		void GetSamplers(ID3D11SamplerState* (&o_samplers)[SAMPLER_COUNT]) const;
		/** @brief Our copies of the river's pixel shader b0-b4 (null where it had none bound). */
		void GetPixelBuffers(ID3D11Buffer* (&o_buffers)[PIXEL_BUFFER_COUNT]) const;
		void GetVertexBuffers(ID3D11Buffer* (&o_buffers)[VERTEX_BUFFER_COUNT]) const;

	private:
		/** @brief Copies a_source into o_copy (resized as needed); o_bound false if a_source is null. False on failure. */
		static bool CopyBuffer(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11Buffer* a_source, winrt::com_ptr<ID3D11Buffer>& o_copy, bool& o_bound);

		std::array<winrt::com_ptr<ID3D11ShaderResourceView>, VIEW_COUNT> views;
		std::array<winrt::com_ptr<ID3D11SamplerState>, SAMPLER_COUNT> samplers;
		std::array<winrt::com_ptr<ID3D11Buffer>, PIXEL_BUFFER_COUNT> pixelBuffers;
		std::array<bool, PIXEL_BUFFER_COUNT> pixelBound{};
		std::array<winrt::com_ptr<ID3D11Buffer>, VERTEX_BUFFER_COUNT> vertexBuffers;
		std::array<bool, VERTEX_BUFFER_COUNT> vertexBound{};
		RE::BSShader* shader = nullptr;
		const void* geometry = nullptr;
		uint32_t vertexDescriptor = 0;
		uint32_t pixelDescriptor = 0;
		uint32_t capturedFrame = UINT32_MAX;
		uint32_t triedFrame = UINT32_MAX;
		bool captured = false;
	};
}
