#pragma once

#include "RiverCapture.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace FluidFieldSim
{
	/**
	 * @brief D91: draws the cached sites' sheets as see-through water in our own forward pass after the world (water and
	 * blended geometry included). Each piece uses immutable buffers built once from its CPU copy, with its vertex shift and
	 * wobble (t123), flow bake (t124) and latched scroll phase. D159: shaded by the river's own Water.hlsl (FLUID_FIELD_FALLS);
	 * FallSurface.hlsl only draws the depth passes. All pipeline state it touches is saved and restored.
	 */
	class FallRefraction
	{
	public:
		/** @brief D98: the look of the water a piece pours out of, from its TESWaterForm::data as Water.hlsl reads it. */
		struct WaterLook
		{
			float shallowColor[3]{ 0.25f, 0.33f, 0.3f };     ///< ShallowColor, 0 to 1, times the weather's water multiplier
			float deepColor[3]{ 0.05f, 0.09f, 0.1f };        ///< DeepColor, the same
			float reflectionColor[3]{ 0.3f, 0.35f, 0.4f };   ///< ReflectionColor times VarAmounts.y: the reflection without a cubemap
			float fogNear = 0.0f;                            ///< Above-water fog distance near (FogParam.z less FogParam.w)
			float fogFar = 600.0f;                           ///< ...and far (FogParam.z): water this deep is wholly its own colour
			float fogAmount = 1.0f;                          ///< ...and amount (FogNearColor.w): the curve from seen-through to coloured
			float fresnel = 0.02f;                           ///< Fresnel amount (FresnelRI.x): reflectance looking straight at the water
			float sunSpecularPower = 600.0f;                 ///< Sun specular power (VarAmounts.x)
			float sunSpecularMagnitude = 1.0f;               ///< Sun specular magnitude (taken as DeepColor.w: the glint is 1 - exp(-magnitude))
			float reflectionMagnitude = 1.0f;                ///< Reflection magnitude (WaterParams.y): how much the normal map tilts the reflection
			ID3D11ShaderResourceView* noise[3]{};            ///< D129: its normal maps (Water.hlsl's Normals01-03), or null
			float noiseScale[3]{ 1000.0f, 500.0f, 250.0f };  ///< D129: ...world units per repeat of each (NormalsScale)
			float noiseAmplitude[3]{};                       ///< D129: ...and how much each tilts the surface (NormalsAmplitude)
		};

		/** @brief One falls piece to draw. */
		struct MeshDraw
		{
			WaterLook look;                                      ///< D98: the water it pours out of
			RE::BSGeometry* geometry = nullptr;                  ///< Its buffers are built from this geometry's CPU copy, once
			const std::vector<RE::NiPoint3>* normals = nullptr;  ///< D186: its normals as its pushes leave it (model space), or null: the mesh's own
			uint64_t normalsVersion = 0;                         ///< D186: ...rebuilt when this changes
			float world[12]{};                                   ///< Its model to world transform, rows of a 3x4 (rotation times scale, translation)
			const void* ground = nullptr;                        ///< FallGroundCB's contents for it (b13)
			uint32_t groundSize = 0;                             ///< ...its size in bytes
			ID3D11ShaderResourceView* bake = nullptr;            ///< Its flow bake (t124), or null
			ID3D11ShaderResourceView* diffuse = nullptr;         ///< Its material's diffuse texture, or null
			ID3D11ShaderResourceView* normal = nullptr;          ///< Its material's normal map, or null
			const RiverCapture* river = nullptr;                 ///< D171: its water type's draw state this frame, or null: the frame's
			ID3D11Buffer* riverMaterial = nullptr;               ///< D171: its water type's PerMaterial (b1) for the river's shader, or null for the frame's
			ID3D11VertexShader* riverVertexShader = nullptr;     ///< D171: ...and that type's variant of it, or null for the frame's
			ID3D11PixelShader* riverPixelShader = nullptr;
		};

		/** @brief What every piece of a frame shares. */
		struct DrawParams
		{
			ID3D11RenderTargetView* target = nullptr;
			ID3D11RenderTargetView* motion = nullptr;         ///< The motion vectors: the river's shader writes the sheets' own (their wobble)
			ID3D11Texture2D* targetTexture = nullptr;         ///< Copied before drawing: what the falls refract
			ID3D11DepthStencilView* depth = nullptr;          ///< Read-only: tested, not written
			ID3D11ShaderResourceView* sceneDepth = nullptr;   ///< The scene's depth, for the water's thickness behind the surface
			ID3D11Buffer* camera = nullptr;                   ///< The main camera's per-frame constants (b12)
			ID3D11Buffer* shared = nullptr;                   ///< SharedData (b5)
			ID3D11Buffer* features = nullptr;                 ///< FeatureData (b6)
			ID3D11ShaderResourceView* vertexClip = nullptr;   ///< FallGround's VertexClip (t123)
			ID3D11ShaderResourceView* poolHeights = nullptr;  ///< FallGround's Heights (t120)
			ID3D11ShaderResourceView* environment = nullptr;  ///< Dynamic Cubemaps' capture, or null: each water's reflection colour
			float time = 0.0f;                                ///< D129: seconds, for the water normal maps' drift
			float skyFogNearColor[3]{};                       ///< D151: the sky's distance fog, as the water shader takes it
			float skyFogFarColor[3]{};
			float skyFogNear = 0.0f;
			float skyFogFar = 0.0f;  ///< 0: no fog
			float skyFogPower = 1.0f;
			float skyFogClamp = 0.0f;
			uint32_t frame = 0;  ///< D123: the rendered frame's number (only this frame's pre-water copy is used)
			/** D159: this frame's river draw state and the falls variant of its shaders (Water.hlsl, FLUID_FIELD_FALLS); all three needed */
			const RiverCapture* river = nullptr;
			ID3D11VertexShader* riverVertexShader = nullptr;
			ID3D11PixelShader* riverPixelShader = nullptr;
		};

		/** @brief Draws a_meshes with the river's shader; false (nothing drawn) without it, or when nothing is bound. */
		bool Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const DrawParams& a_params, const std::vector<MeshDraw>& a_meshes);
		/**
		 * @brief D123: copies the scene before the water draws, for this frame's Draw to refract when its target is the same
		 * texture (else the river seen through a sheet shows as a plane cut by it).
		 * @param a_frame The rendered frame's number; Draw uses the copy only within that frame.
		 */
		void CopyBeforeWater(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11Texture2D* a_source, ID3D11RenderTargetView* a_target, uint32_t a_frame);
		/** @brief Drops the buffers of every piece not in a_keep (their geometry went away). */
		void Prune(const std::unordered_set<const RE::BSGeometry*>& a_keep);
		void Release();
		void ReleaseShaders();
		/** @brief False once its shaders failed to compile or the scene could not be copied: vanilla's falls are then kept. */
		bool IsUsable() const { return !shaderFailed && !sceneCopyFailed; }

		/** @brief D102: most world units of water behind the surface counted (the sheet should read, not the gap behind it). */
		static constexpr float LONGEST_PATH = 32.0f;
		/** @brief World units of water the sheet itself is thick, looked at square on. */
		static constexpr float SHEET_THICKNESS = 8.0f;

	private:
		bool EnsureShaders();

		bool EnsureStates(ID3D11Device* a_device);
		bool EnsureSceneCopy(ID3D11Device* a_device, ID3D11Texture2D* a_source, ID3D11RenderTargetView* a_target);
		/** @brief D125: the front surface's depth pass shader (FallSurface.hlsl's PSDepth); null if it fails to compile. */
		ID3D11PixelShader* EnsureDepthShader();
		/** @brief D174: FallSurface.hlsl's PSBehindDepth, compiled once (null if it failed). */
		ID3D11PixelShader* EnsureBehindShader();
		/** @brief D125: a depth target like the scene's (a_sceneDepth's texture), its depth and shader views and the pass's state. */
		bool EnsureFrontDepth(ID3D11Device* a_device, ID3D11DepthStencilView* a_sceneDepth);

		/** @brief A piece's own buffers: vertex data read by SV_VertexID (t125) and its indices. */
		struct MeshBuffers
		{
			winrt::com_ptr<ID3D11Buffer> vertices;
			winrt::com_ptr<ID3D11ShaderResourceView> verticesSRV;
			winrt::com_ptr<ID3D11Buffer> indices;
			uint32_t vertexCount = 0;
			uint32_t indexCount = 0;
			uint64_t normalsVersion = 0;  ///< D186: the pushed normals it was built with (0: the mesh's own)
		};
		/** @brief Builds a piece's buffers from its CPU copy; false where it has none. */
		bool BuildMesh(ID3D11Device* a_device, RE::BSGeometry* a_geometry, MeshBuffers& o_mesh, const std::vector<RE::NiPoint3>* a_normals = nullptr) const;

		/** @brief Matches FallSurfaceVertex in FallSurface.hlsl. */
		struct Vertex
		{
			float position[3];
			float uv[2];
			float normal[3];
			float tangent[3];  ///< D174: the direction texture v runs (down the flow), model space, summed over its triangles
		};
		static_assert(sizeof(Vertex) == 44);

		/** @brief Matches FallSurfaceCB in FallSurface.hlsl. */
		struct alignas(16) Constants
		{
			float world[12];
			float shallowColor[3];
			float fogFar;
			float deepColor[3];
			float fogRange;
			float reflectionColor[3];
			float fogAmount;
			float fresnel;
			float sunSpecularPower;
			float sunSpecularStrength;
			float reflectionMagnitude;
			float refraction;
			float streakFoam;
			float depthScale;
			float maxPath;
			float sheetThickness;
			uint32_t hasEnvironment;
			uint32_t hasDiffuse;
			uint32_t hasNormal;
			uint32_t hasFront;  ///< D125: the front surface's depth is bound (t105)
			uint32_t padding[3];
			float noiseScale[3];  ///< D129: the water type's normal maps' world units per repeat
			float noiseTime;      ///< D129: seconds, for the maps' drift along the flow
			float noiseAmplitude[3];
			uint32_t hasNoise;
			float skyFogNearColor[3];  ///< D151: the sky's distance fog (Sky: its near and far colours, distances, power and clamp)
			float skyFogNear;
			float skyFogFarColor[3];
			float skyFogFar;
			float skyFogPower;
			float skyFogClamp;
			float skyFogPadding[2];
		};
		static_assert(sizeof(Constants) == 240);

		std::unordered_map<const RE::BSGeometry*, MeshBuffers> meshes;
		std::unordered_set<const RE::BSGeometry*> failedMeshes;  ///< No CPU copy: not tried again

		winrt::com_ptr<ID3D11Texture2D> sceneCopy;
		winrt::com_ptr<ID3D11ShaderResourceView> sceneCopySRV;
		winrt::com_ptr<ID3D11Buffer> constants;
		winrt::com_ptr<ID3D11Buffer> groundConstants;
		uint32_t groundSize = 0;
		winrt::com_ptr<ID3D11VertexShader> vertexShader;  ///< FallSurface.hlsl's VSMain, for the depth passes
		winrt::com_ptr<ID3D11BlendState> blend;
		winrt::com_ptr<ID3D11DepthStencilState> depthState;
		winrt::com_ptr<ID3D11RasterizerState> raster;
		winrt::com_ptr<ID3D11SamplerState> wrapSampler;
		winrt::com_ptr<ID3D11SamplerState> clampSampler;
		bool shaderFailed = false;
		bool sceneCopyFailed = false;
		// D125: the falls' front surface: its depth pass's shader, and a copy of the scene's depth it is drawn into
		winrt::com_ptr<ID3D11PixelShader> depthPixelShader;
		bool depthShaderFailed = false;
		winrt::com_ptr<ID3D11Texture2D> frontDepth;
		winrt::com_ptr<ID3D11DepthStencilView> frontDepthDSV;
		winrt::com_ptr<ID3D11ShaderResourceView> frontDepthSRV;
		winrt::com_ptr<ID3D11DepthStencilState> frontDepthState;
		bool frontDepthFailed = false;
		winrt::com_ptr<ID3D11Texture2D> behindDepth;  ///< D174: the second layer (the falls' surface nearest behind their front)
		winrt::com_ptr<ID3D11DepthStencilView> behindDepthDSV;
		winrt::com_ptr<ID3D11ShaderResourceView> behindDepthSRV;
		winrt::com_ptr<ID3D11PixelShader> behindPixelShader;
		bool behindShaderFailed = false;
		/** D174: per captured river state, the last Normals01 (t4) it had bound: the foam's bubbles (FOAM_MAP_SLOT). */
		std::unordered_map<const RiverCapture*, winrt::com_ptr<ID3D11ShaderResourceView>> riverFoamMaps;
		const ID3D11Texture2D* preWaterSource = nullptr;  ///< D123: what the pre-water copy was taken from, and in which frame
		uint32_t preWaterFrame = UINT32_MAX;
	};
}
