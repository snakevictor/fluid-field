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
		/**
		 * @brief D98: the look of the water a piece pours out of, from its water type (TESWaterForm::data) as Water.hlsl
		 * reads it: colours under the weather's water multiplier, the above-water fog that hides what lies behind, the
		 * fresnel, the reflection and the sun's glint.
		 */
		struct WaterLook
		{
			float shallowColor[3]{ 0.25f, 0.33f, 0.3f };    ///< ShallowColor, 0 to 1, times the weather's water multiplier
			float deepColor[3]{ 0.05f, 0.09f, 0.1f };       ///< DeepColor, the same
			float reflectionColor[3]{ 0.3f, 0.35f, 0.4f };  ///< ReflectionColor times the reflection amount (VarAmounts.y): the reflection without a cubemap
			float fogNear = 0.0f;                           ///< Above-water fog distance near (FogParam.z less FogParam.w)
			float fogFar = 600.0f;                          ///< ...and far (FogParam.z): water this deep is wholly its own colour
			float fogAmount = 1.0f;                         ///< ...and amount (FogNearColor.w): the curve from seen-through to coloured
			float fresnel = 0.02f;                          ///< Fresnel amount (FresnelRI.x): reflectance looking straight at the water
			float sunSpecularPower = 600.0f;                ///< Sun specular power (VarAmounts.x)
			float sunSpecularMagnitude = 1.0f;              ///< Sun specular magnitude (taken as DeepColor.w: the glint is 1 - exp(-magnitude))
			float reflectionMagnitude = 1.0f;               ///< Reflection magnitude (WaterParams.y): how much the normal map tilts the reflection
		};

		/** @brief One falls piece to draw. */
		struct MeshDraw
		{
			WaterLook look;                               ///< D98: the water it pours out of
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
			ID3D11ShaderResourceView* environment = nullptr;  ///< A cubemap of the surroundings (Dynamic Cubemaps, once it holds a capture), or null for each water's reflection colour
			float depthScale = 2.0f;                          ///< D98: how much deeper than its path through it the falling water reads against its water's fog distances
			float refraction = 1.0f;                          ///< How strongly the surface bends what is seen through it
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
		};
		static_assert(sizeof(Constants) == 144);

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
