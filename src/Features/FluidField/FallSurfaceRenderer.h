#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>
#include <vector>

namespace FluidFieldSim
{
	/** @brief One vertex of a fall surface (D30), matching SurfaceVertex in FallSurface.hlsl. */
	struct FallSurfaceVertex
	{
		float position[3];  ///< World units, on the water surface
		float across;       ///< Stream function: world units across the flow
		float travel;       ///< Seconds from where the water comes in
		float speed;        ///< 1 for free flow
		float foam;         ///< 0 to 1
		float edge;         ///< 0 at the water's edge to 1 inside
		float normal[3];
		float depth;  ///< Water above the rock, world units
		float uv[2];  ///< Vanilla's texture coordinates, where the flow's are not used
		float flow;   ///< 0 to 1: how much the flow's texture coordinates are used (none on steep water)
		float pad;
	};
	static_assert(sizeof(FallSurfaceVertex) == 64);

	/** @brief Where the falls throw spray and mist (D31), matching Emitter in FallSpray.hlsl. */
	struct FallEmitter
	{
		float position[3];   ///< On the surface, world units
		float strength;      ///< 0 to 1: how much it throws
		float direction[3];  ///< The water's motion there, unit
		float speed;         ///< World units per second (D35)
		float normal[3];     ///< The surface's normal there, up
		float kind;          ///< FallEmitterKind
	};
	static_assert(sizeof(FallEmitter) == 48);

	/** @brief What throws a spray emitter's water (D35), matching FallSpray.hlsl. */
	enum class FallEmitterKind : uint32_t
	{
		Lip = 0,     ///< Where the surface drops away: the water leaves it running on
		Plunge = 1,  ///< Where falling water meets the water below: thrown up and out
		Strike = 2   ///< Where the water strikes rock or another piece
	};

	/** @brief Where falling water meets the water below (D35), for its pushes on the simulated water. */
	struct FallPlunge
	{
		float position[3];   ///< On the water below, world units
		float direction[2];  ///< Horizontal way the falling water runs, unit (or zero)
		float speed;         ///< World units per second it arrives at
		float strength;      ///< 0 to 1
	};

	/**
	 * @brief Draws the falls' own water surface (D30) after the world: one merged mesh per site, made of
	 * vanilla's falling-water meshes subdivided, displaced by the flow in the vertex shader, with
	 * vanilla's falling-water textures laid along the streamlines and the travel time. Depth tested against the scene without writing,
	 * premultiplied-alpha blended. Every piece of pipeline state it touches is saved and restored.
	 */
	class FallSurfaceRenderer
	{
	public:
		/** @brief Uploads the mesh and the spray emitters when their version changed. */
		void Upload(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const std::vector<FallSurfaceVertex>& a_vertices, const std::vector<uint32_t>& a_indices,
			const std::vector<FallEmitter>& a_emitters, uint64_t a_version);

		struct DrawParams
		{
			ID3D11RenderTargetView* target = nullptr;
			ID3D11DepthStencilView* depth = nullptr;
			ID3D11ShaderResourceView* sceneDepth = nullptr;  ///< The depth copy, for soft contact with rock
			ID3D11Buffer* camera = nullptr;                  ///< The main camera's per-frame constants (b12), snapshotted during the deferred passes
			ID3D11Buffer* shared = nullptr;
			ID3D11Buffer* features = nullptr;
			ID3D11ShaderResourceView* diffuse = nullptr;  ///< Vanilla's falling-water textures
			ID3D11ShaderResourceView* normal = nullptr;
			float time = 0.0f;
			uint32_t debugMode = 0;
			bool drawSurface = true;  ///< false: only the spray and mist, over vanilla's own falling water (D32)
			uint32_t sprayPerEmitter = SPRAY_PER_EMITTER;
			uint32_t mistPerEmitter = MIST_PER_EMITTER;
			float spraySize = SPRAY_SIZE;
			float mistOpacity = 0.07f;
			float sprayOpacity = 0.35f;
			float fogDensity = 0.0f;  ///< Waterfall fog (D38): extinction per world unit over wholly white water; 0 off
			float fogHeight = 90.0f;
			float fogBottom = 0.0f;  ///< The fog's height band (D41)
			float fogTop = 0.0f;
		};
		void Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const DrawParams& a_params);

		/** @brief D38: vanilla's waterfall mist effects, as fog volumes: per volume its centre and strength, then its half extents. */
		void UploadMist(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const std::vector<std::array<float, 8>>& a_volumes, uint64_t a_version);

		void Release();
		void ReleaseShaders();
		bool HasMesh() const { return indexCount > 0; }

		static constexpr float U_SCALE = 192.0f;      ///< World units across the flow per repeat of the texture
		static constexpr float V_SCALE = 384.0f;      ///< World units along the flow (at free speed) per repeat
		static constexpr float WAVE_HEIGHT = 10.0f;   ///< Standing waves where fast water runs shallow over rock, world units
		static constexpr float WAVE_LENGTH = 72.0f;   ///< World units along the flow
		static constexpr float RIPPLE_HEIGHT = 3.0f;  ///< Ripples carried by the flow, world units
		static constexpr float RIPPLE_LENGTH = 40.0f;
		static constexpr float BULGE_HEIGHT = 10.0f;   ///< The water mounds over rock just under it, world units
		static constexpr float SHALLOW_DEPTH = 40.0f;  ///< Water shallower than this over rock waves and mounds
		static constexpr float CONTACT_WIDTH = 2.0f;   ///< World units over which the water fades where it meets rock in front of it
		static constexpr float BRIGHTNESS = 0.32f;
		static constexpr float VANILLA_SCROLL = 0.6f;
		static constexpr uint32_t SPRAY_PER_EMITTER = 12;  ///< Spray particles alive per emitter at once
		static constexpr uint32_t MIST_PER_EMITTER = 3;    ///< Mist puffs alive per emitter at once
		static constexpr uint32_t MAX_PARTICLES = 2000000;
		static constexpr float SPRAY_STRETCH = 0.012f;  ///< Seconds of motion each droplet is drawn smeared over
		static constexpr float SPRAY_LIFE = 0.9f;       ///< Seconds
		static constexpr float SPRAY_SPEED = 260.0f;    ///< World units per second thrown up and out at full strength
		static constexpr float SPRAY_SIZE = 10.0f;      ///< World units across
		static constexpr float MIST_LIFE = 3.0f;
		static constexpr float MIST_SIZE = 100.0f;
		static constexpr float MIST_RISE = 40.0f;          ///< World units per second
		static constexpr float PARTICLE_GRAVITY = 687.0f;  ///< Repeats per second vanilla's texture scrolls along v, where the flow's is not used      ///< Light at this point of the frame, as the ground fog and the old curtains needed

	private:
		bool EnsureShaders();
		bool EnsureStates(ID3D11Device* a_device);
		void DrawParticles(ID3D11DeviceContext* a_context, uint32_t a_perEmitter);
		void DrawFog(ID3D11DeviceContext* a_context);

		struct alignas(16) Constants
		{
			float time;
			float uScale;
			float vScale;
			float baseSpeed;
			float waveHeight;
			float waveLength;
			float rippleHeight;
			float rippleLength;
			float bulgeHeight;
			float shallowDepth;
			float contactWidth;
			float brightness;
			uint32_t debugMode;
			float vanillaScroll;
			uint32_t emitterCount;
			uint32_t sprayPerEmitter;
			uint32_t mistPerEmitter;
			float sprayLife;
			float spraySpeed;
			float spraySize;
			float mistLife;
			float mistSize;
			float mistRise;
			float gravity;
			float mistOpacity;
			float sprayStretch;
			float sprayOpacity;
			float pad2;
			float fogDensity;
			float fogHeight;
			uint32_t fogSteps;
			uint32_t mistVolumeCount;
			float fogBottom;
			float fogTop;
			float fogPad[2];
		};

		winrt::com_ptr<ID3D11Buffer> vertices;
		winrt::com_ptr<ID3D11ShaderResourceView> verticesSRV;
		winrt::com_ptr<ID3D11Buffer> indices;
		uint32_t vertexCapacity = 0;
		uint32_t indexCapacity = 0;
		uint32_t indexCount = 0;
		uint64_t uploadedVersion = UINT64_MAX;

		winrt::com_ptr<ID3D11VertexShader> fogVertexShader;
		winrt::com_ptr<ID3D11PixelShader> fogPixelShader;
		winrt::com_ptr<ID3D11Buffer> mist;
		winrt::com_ptr<ID3D11ShaderResourceView> mistSRV;
		uint32_t mistCount = 0;
		uint64_t mistVersion = UINT64_MAX;
		static constexpr uint32_t MAX_MIST_VOLUMES = 64;
		static constexpr uint32_t FOG_STEPS = 96;  ///< Over FallFog.hlsl's FOG_RANGE: about 25 units a step, finer than the fog is thick
		winrt::com_ptr<ID3D11Buffer> emitters;
		winrt::com_ptr<ID3D11ShaderResourceView> emittersSRV;
		uint32_t emitterCapacity = 0;
		uint32_t emitterCount = 0;

		winrt::com_ptr<ID3D11Buffer> constants;
		winrt::com_ptr<ID3D11VertexShader> vertexShader;
		winrt::com_ptr<ID3D11PixelShader> pixelShader;
		winrt::com_ptr<ID3D11VertexShader> sprayVertexShader;
		winrt::com_ptr<ID3D11PixelShader> sprayPixelShader;
		winrt::com_ptr<ID3D11BlendState> blend;
		winrt::com_ptr<ID3D11DepthStencilState> depthState;
		winrt::com_ptr<ID3D11RasterizerState> raster;
		winrt::com_ptr<ID3D11SamplerState> sampler;
		bool shaderFailed = false;
	};
}
