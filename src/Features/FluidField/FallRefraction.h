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
	 * @brief D159: draws the cached sites' sheets as see-through water after the world, shaded by the river's own
	 * Water.hlsl (FLUID_FIELD_FALLS); FallSurface.hlsl only draws the depth passes. Restores all state it touches.
	 */
	class FallRefraction
	{
	public:
		/** @brief D98: the water a piece pours out of, from its TESWaterForm::data as Water.hlsl reads it. */
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
			ID3D11ShaderResourceView* noise[3]{};            ///< D129: its normal maps (Normals01-03), or null
			float noiseScale[3]{ 1000.0f, 500.0f, 250.0f };  ///< World units per repeat of each (NormalsScale)
			float noiseAmplitude[3]{};                       ///< How much each tilts the surface (NormalsAmplitude)
		};

		/** @brief One falls piece to draw. */
		struct MeshDraw
		{
			WaterLook look;                                      ///< The water it pours out of
			RE::BSGeometry* geometry = nullptr;                  ///< Buffers built once from its CPU copy
			const std::vector<RE::NiPoint3>* normals = nullptr;  ///< D186: its normals as its pushes leave it (model space), or null: the mesh's own
			uint64_t normalsVersion = 0;                         ///< Buffers rebuilt when this changes
			float world[12]{};                                   ///< Model to world, rows of a 3x4
			const void* ground = nullptr;                        ///< FallGroundCB's contents for it (b13)
			uint32_t groundSize = 0;                             ///< Bytes
			ID3D11ShaderResourceView* bake = nullptr;            ///< Its flow bake (t124), or null
			ID3D11ShaderResourceView* diffuse = nullptr;         ///< Its material's diffuse texture, or null
			ID3D11ShaderResourceView* normal = nullptr;          ///< Its material's normal map, or null
			const RiverCapture* river = nullptr;                 ///< D171: its water type's draw state this frame, or null for the frame's
			ID3D11Buffer* riverMaterial = nullptr;               ///< Its water type's PerMaterial (b1), or null for the frame's
			ID3D11VertexShader* riverVertexShader = nullptr;     ///< Its water type's shader variant, or null for the frame's
			ID3D11PixelShader* riverPixelShader = nullptr;
		};

		/** @brief What every piece of a frame shares. */
		struct DrawParams
		{
			ID3D11RenderTargetView* target = nullptr;
			ID3D11RenderTargetView* motion = nullptr;         ///< Motion vectors; the river's shader writes the sheets' wobble
			ID3D11Texture2D* targetTexture = nullptr;         ///< Copied before drawing, for the falls to refract
			ID3D11DepthStencilView* depth = nullptr;          ///< Read-only: tested, not written
			ID3D11ShaderResourceView* sceneDepth = nullptr;   ///< For the water's thickness behind the surface
			ID3D11Buffer* camera = nullptr;                   ///< The main camera's per-frame constants (b12)
			ID3D11Buffer* shared = nullptr;                   ///< SharedData (b5)
			ID3D11Buffer* features = nullptr;                 ///< FeatureData (b6)
			ID3D11ShaderResourceView* vertexClip = nullptr;   ///< FallGround's VertexClip (t123)
			ID3D11ShaderResourceView* poolHeights = nullptr;  ///< FallGround's Heights (t120)
			ID3D11ShaderResourceView* environment = nullptr;  ///< Dynamic Cubemaps' capture, or null for each water's reflection colour
			float time = 0.0f;                                ///< D129: seconds, for the water normal maps' drift
			float skyFogNearColor[3]{};                       ///< D151: the sky's distance fog, as Water.hlsl takes it
			float skyFogFarColor[3]{};
			float skyFogNear = 0.0f;
			float skyFogFar = 0.0f;  ///< 0: no fog
			float skyFogPower = 1.0f;
			float skyFogClamp = 0.0f;
			uint32_t frame = 0;  ///< D123: only this frame's pre-water copy is used
			/** D159: this frame's river draw state and the falls variant of its shaders; all three needed */
			const RiverCapture* river = nullptr;
			ID3D11VertexShader* riverVertexShader = nullptr;
			ID3D11PixelShader* riverPixelShader = nullptr;
		};

		/** @brief Draws a_meshes with the river's shader; false (nothing drawn) without it, or when nothing is bound. */
		bool Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const DrawParams& a_params, const std::vector<MeshDraw>& a_meshes);
		/** @brief D123: copies the scene before the water draws, so this frame's Draw shows no river behind a sheet. */
		void CopyBeforeWater(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11Texture2D* a_source, ID3D11RenderTargetView* a_target, uint32_t a_frame);
		/** @brief Drops the buffers of every piece not in a_keep (their geometry went away). */
		void Prune(const std::unordered_set<const RE::BSGeometry*>& a_keep);
		void Release();
		void ReleaseShaders();
		/** @brief False once its shaders or the scene copy failed. */
		bool IsUsable() const { return !shaderFailed && !sceneCopyFailed; }

		/** @brief D102: most world units of water counted behind the surface, so the sheet reads, not the gap behind. */
		static constexpr float LONGEST_PATH = 32.0f;
		/** @brief World units of water the sheet itself is thick, looked at square on. */
		static constexpr float SHEET_THICKNESS = 8.0f;

	private:
		bool EnsureShaders();

		bool EnsureStates(ID3D11Device* a_device);
		bool EnsureSceneCopy(ID3D11Device* a_device, ID3D11Texture2D* a_source, ID3D11RenderTargetView* a_target);
		/** @brief D125: FallSurface.hlsl's PSDepth, compiled once (null if it failed). */
		ID3D11PixelShader* EnsureDepthShader();
		/** @brief D174: FallSurface.hlsl's PSBehindDepth, compiled once (null if it failed). */
		ID3D11PixelShader* EnsureBehindShader();
		/** @brief D125: a depth target matching a_sceneDepth, with its views and the pass's state. */
		bool EnsureFrontDepth(ID3D11Device* a_device, ID3D11DepthStencilView* a_sceneDepth);

		/** @brief A piece's vertices (read by SV_VertexID, t125) and indices. */
		struct MeshBuffers
		{
			winrt::com_ptr<ID3D11Buffer> vertices;
			winrt::com_ptr<ID3D11ShaderResourceView> verticesSRV;
			winrt::com_ptr<ID3D11Buffer> indices;
			uint32_t vertexCount = 0;
			uint32_t indexCount = 0;
			uint64_t normalsVersion = 0;  ///< D186: pushed normals it was built with (0: the mesh's own)
		};
		/** @brief Builds a piece's buffers from its CPU copy; false where it has none. */
		bool BuildMesh(ID3D11Device* a_device, RE::BSGeometry* a_geometry, MeshBuffers& o_mesh, const std::vector<RE::NiPoint3>* a_normals = nullptr) const;

		/** @brief Matches FallSurfaceVertex in FallSurface.hlsl. */
		struct Vertex
		{
			float position[3];
			float uv[2];
			float normal[3];
			float tangent[3];  ///< D174: texture v's direction (down the flow), model space
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
			uint32_t hasFront;  ///< D125: front depth bound (t105)
			uint32_t padding[3];
			float noiseScale[3];  ///< D129: world units per normal map repeat
			float noiseTime;      ///< Seconds, for the maps' drift
			float noiseAmplitude[3];
			uint32_t hasNoise;
			float skyFogNearColor[3];  ///< D151: the sky's distance fog
			float skyFogNear;
			float skyFogFarColor[3];
			float skyFogFar;
			float skyFogPower;
			float skyFogClamp;
			float skyFogPadding[2];
		};
		static_assert(sizeof(Constants) == 240);

		std::unordered_map<const RE::BSGeometry*, MeshBuffers> meshes;
		std::unordered_set<const RE::BSGeometry*> failedMeshes;                ///< No CPU copy: not tried again
		std::vector<std::pair<const MeshDraw*, const MeshBuffers*>> drawable;  ///< Draw's pieces this frame; kept to reuse its memory

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
		// D125: the front surface's depth pass, drawn into a copy of the scene's depth
		winrt::com_ptr<ID3D11PixelShader> depthPixelShader;
		bool depthShaderFailed = false;
		winrt::com_ptr<ID3D11Texture2D> frontDepth;
		winrt::com_ptr<ID3D11DepthStencilView> frontDepthDSV;
		winrt::com_ptr<ID3D11ShaderResourceView> frontDepthSRV;
		winrt::com_ptr<ID3D11DepthStencilState> frontDepthState;
		bool frontDepthFailed = false;
		winrt::com_ptr<ID3D11Texture2D> behindDepth;  ///< D174: second layer, the surface nearest behind the front
		winrt::com_ptr<ID3D11DepthStencilView> behindDepthDSV;
		winrt::com_ptr<ID3D11ShaderResourceView> behindDepthSRV;
		winrt::com_ptr<ID3D11PixelShader> behindPixelShader;
		bool behindShaderFailed = false;
		/** D174: per captured river state, the last Normals01 (t4) bound, for the foam's bubbles. */
		std::unordered_map<const RiverCapture*, winrt::com_ptr<ID3D11ShaderResourceView>> riverFoamMaps;
		const ID3D11Texture2D* preWaterSource = nullptr;  ///< D123: the pre-water copy's source and frame
		uint32_t preWaterFrame = UINT32_MAX;
	};
}
