#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <vector>

namespace FluidFieldSim
{
	/** @brief One vertex of a site's merged falls surface (D30), on which FallSites finds plunges, impacts and seams. */
	struct FallSurfaceVertex
	{
		float position[3];  ///< World units, on the water surface
		float speed;        ///< 1 for free flow
		float normal[3];
	};

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
	 * @brief Draws the falls' spray after the world (D31): stateless GPU particles thrown from the
	 * emitters. Depth tested against the scene
	 * without writing, premultiplied-alpha blended. Every piece of pipeline state it touches is saved and
	 * restored. (D30's own merged falls surface was drawn here too, until D32 returned to vanilla's meshes.)
	 */
	class FallSurfaceRenderer
	{
	public:
		/** @brief Uploads the spray emitters when their version changed. */
		void Upload(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const std::vector<FallEmitter>& a_emitters, uint64_t a_version);

		struct DrawParams
		{
			ID3D11RenderTargetView* target = nullptr;
			ID3D11DepthStencilView* depth = nullptr;
			ID3D11ShaderResourceView* sceneDepth = nullptr;  ///< The depth copy, for soft contact with rock
			ID3D11Buffer* camera = nullptr;                  ///< The main camera's per-frame constants (b12), snapshotted during the deferred passes
			ID3D11Buffer* shared = nullptr;
			ID3D11Buffer* features = nullptr;
			float time = 0.0f;
			uint32_t sprayPerEmitter = 40;
			uint32_t mistPerEmitter = 0;
			float spraySize = 1.4f;
			float sprayOpacity = 0.2f;
			float mistOpacity = 0.06f;
		};
		void Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const DrawParams& a_params);

		void Release();
		void ReleaseShaders();

		static constexpr float BRIGHTNESS = 0.32f;
		static constexpr uint32_t MAX_PARTICLES = 2000000;
		static constexpr float SPRAY_STRETCH = 0.005f;  ///< Seconds of motion each droplet is drawn smeared over
		static constexpr float SPRAY_LIFE = 0.9f;       ///< Seconds
		static constexpr float SPRAY_SPEED = 150.0f;    ///< World units per second thrown up and out at full strength
		static constexpr float MIST_LIFE = 3.0f;
		static constexpr float MIST_SIZE = 100.0f;
		static constexpr float MIST_RISE = 40.0f;           ///< World units per second
		static constexpr float PARTICLE_GRAVITY = 1100.0f;  ///< D50: droplets turn down sooner than free fall (they read as too floaty)

	private:
		bool EnsureShaders();
		bool EnsureStates(ID3D11Device* a_device);
		void DrawParticles(ID3D11DeviceContext* a_context, uint32_t a_perEmitter);

		// Matches FallEffectsCB in FluidField/FallEffects.hlsli
		struct alignas(16) Constants
		{
			float time;
			float brightness;
			uint32_t emitterCount;
			uint32_t sprayPerEmitter;
			uint32_t mistPerEmitter;
			float sprayLife;
			float spraySpeed;
			float spraySize;
			float sprayStretch;
			float sprayOpacity;
			float mistLife;
			float mistSize;
			float mistRise;
			float mistOpacity;
			float gravity;
			float pad;
		};
		static_assert(sizeof(Constants) == 64);

		winrt::com_ptr<ID3D11Buffer> emitters;
		winrt::com_ptr<ID3D11ShaderResourceView> emittersSRV;
		uint32_t emitterCapacity = 0;
		uint32_t emitterCount = 0;
		uint64_t uploadedVersion = UINT64_MAX;

		winrt::com_ptr<ID3D11Buffer> constants;
		winrt::com_ptr<ID3D11VertexShader> sprayVertexShader;
		winrt::com_ptr<ID3D11PixelShader> sprayPixelShader;
		winrt::com_ptr<ID3D11BlendState> blend;
		winrt::com_ptr<ID3D11DepthStencilState> depthState;
		winrt::com_ptr<ID3D11RasterizerState> raster;
		bool shaderFailed = false;
	};
}
