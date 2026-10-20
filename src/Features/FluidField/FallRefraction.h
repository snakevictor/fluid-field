#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace FluidFieldSim
{
	/**
	 * @brief D91: the falls' lit pieces drawn as refractive, see-through water by a forward pass of our own after the
	 * world (water and blended geometry included), before the spray. Each piece is drawn from our own immutable
	 * buffers, built once from its CPU copy (the dense copies', D73), with the same per-vertex shift and wobble (t123)
	 * and the same flow bake (t124) as the lit passes, at the frame's latched scroll phase. The shader
	 * (FluidField/FallSurface.hlsl) ports the river water's look (Water.hlsl): what lies behind, refracted from a copy
	 * of the scene and absorbed by the water along the view ray, fresnel reflection, the sun's glint, and white water
	 * from the bake. Every piece of pipeline state it touches is saved and restored.
	 */
	class FallRefraction
	{
	public:
		/** @brief One falls piece to draw. */
		struct MeshDraw
		{
			RE::BSGeometry* geometry = nullptr;           ///< Its buffers are built from this geometry's CPU copy, once
			float world[12]{};                            ///< Its model to world transform, rows of a 3x4 (rotation times scale, translation)
			const void* ground = nullptr;                 ///< FallGroundCB's contents for it (b13)
			uint32_t groundSize = 0;                      ///< ...its size in bytes
			ID3D11ShaderResourceView* bake = nullptr;     ///< Its flow bake (t124), or null
			ID3D11ShaderResourceView* diffuse = nullptr;  ///< Its material's diffuse texture, or null
			ID3D11ShaderResourceView* normal = nullptr;   ///< Its material's normal map, or null
		};

		/** @brief What every piece of a frame shares. */
		struct DrawParams
		{
			ID3D11RenderTargetView* target = nullptr;
			ID3D11Texture2D* targetTexture = nullptr;         ///< Copied before drawing: what the falls refract
			ID3D11DepthStencilView* depth = nullptr;          ///< Read-only: tested, not written
			ID3D11ShaderResourceView* sceneDepth = nullptr;   ///< The scene's depth, for the water's thickness behind the surface
			ID3D11Buffer* camera = nullptr;                   ///< The main camera's per-frame constants (b12)
			ID3D11Buffer* shared = nullptr;                   ///< SharedData (b5)
			ID3D11Buffer* features = nullptr;                 ///< FeatureData (b6)
			ID3D11ShaderResourceView* vertexClip = nullptr;   ///< FallGround's VertexClip (t123)
			ID3D11ShaderResourceView* poolHeights = nullptr;  ///< FallGround's Heights (t120)
			ID3D11ShaderResourceView* environment = nullptr;  ///< A cubemap of the surroundings (Dynamic Cubemaps), or null for the sky's ambient
			float shallowColor[3]{ 0.25f, 0.33f, 0.3f };      ///< The water type's shallow colour, 0 to 1
			float deepColor[3]{ 0.05f, 0.09f, 0.1f };         ///< ...and deep colour
			float clarity = 160.0f;                           ///< World units of water seen through before its colour mostly hides what is behind
			float refraction = 1.0f;                          ///< How strongly the surface bends what is seen through it
			float fresnel = 0.02f;                            ///< Reflectance looking straight at the water
			float specularPower = 600.0f;                     ///< The sun's glint, tightness
			float specularStrength = 1.0f;                    ///< ...and brightness
			float streakFoam = 0.6f;                          ///< How much of vanilla's streak texture reads as white water
		};

		/** @brief Draws a_meshes; nothing when the shaders failed or nothing is bound. */
		void Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const DrawParams& a_params, const std::vector<MeshDraw>& a_meshes);
		/** @brief Drops the buffers of every piece not in a_keep (their geometry went away). */
		void Prune(const std::unordered_set<const RE::BSGeometry*>& a_keep);
		void Release();
		void ReleaseShaders();
		/** @brief False once its shaders failed to compile or the scene could not be copied: the engine's lit falls are then kept. */
		bool IsUsable() const { return !shaderFailed && !sceneCopyFailed; }

		/** @brief World units of water behind the surface counted at most (a curtain far in front of its rock is not that thick). */
		static constexpr float LONGEST_PATH = 600.0f;
		/** @brief World units of water the sheet itself is thick, looked at square on. */
		static constexpr float SHEET_THICKNESS = 8.0f;

	private:
		bool EnsureShaders();
		bool EnsureStates(ID3D11Device* a_device);
		bool EnsureSceneCopy(ID3D11Device* a_device, ID3D11Texture2D* a_source, ID3D11RenderTargetView* a_target);

		/** @brief A piece's own buffers: vertex data read by SV_VertexID (t125) and its indices. */
		struct MeshBuffers
		{
			winrt::com_ptr<ID3D11Buffer> vertices;
			winrt::com_ptr<ID3D11ShaderResourceView> verticesSRV;
			winrt::com_ptr<ID3D11Buffer> indices;
			uint32_t vertexCount = 0;
			uint32_t indexCount = 0;
		};
		/** @brief Builds a piece's buffers from its CPU copy; false where it has none. */
		bool BuildMesh(ID3D11Device* a_device, RE::BSGeometry* a_geometry, MeshBuffers& o_mesh) const;

		/** @brief Matches FallSurfaceVertex in FallSurface.hlsl. */
		struct Vertex
		{
			float position[3];
			float uv[2];
			float normal[3];
		};
		static_assert(sizeof(Vertex) == 32);

		/** @brief Matches FallSurfaceCB in FallSurface.hlsl. */
		struct alignas(16) Constants
		{
			float world[12];
			float shallowColor[3];
			float clarity;
			float deepColor[3];
			float refraction;
			float fresnel;
			float specularPower;
			float specularStrength;
			float streakFoam;
			uint32_t hasEnvironment;
			uint32_t hasDiffuse;
			uint32_t hasNormal;
			float maxPath;
			float sheetThickness;
			float pad[3];
		};
		static_assert(sizeof(Constants) == 128);

		std::unordered_map<const RE::BSGeometry*, MeshBuffers> meshes;
		std::unordered_set<const RE::BSGeometry*> failedMeshes;  ///< No CPU copy: not tried again

		winrt::com_ptr<ID3D11Texture2D> sceneCopy;
		winrt::com_ptr<ID3D11ShaderResourceView> sceneCopySRV;
		winrt::com_ptr<ID3D11Buffer> constants;
		winrt::com_ptr<ID3D11Buffer> groundConstants;
		uint32_t groundSize = 0;
		winrt::com_ptr<ID3D11VertexShader> vertexShader;
		winrt::com_ptr<ID3D11PixelShader> pixelShader;
		winrt::com_ptr<ID3D11BlendState> blend;
		winrt::com_ptr<ID3D11DepthStencilState> depthState;
		winrt::com_ptr<ID3D11RasterizerState> raster;
		winrt::com_ptr<ID3D11SamplerState> wrapSampler;
		winrt::com_ptr<ID3D11SamplerState> clampSampler;
		bool shaderFailed = false;
		bool sceneCopyFailed = false;
	};
}
