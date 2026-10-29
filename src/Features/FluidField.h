#pragma once

#include "FluidField/AirSources.h"
#include "FluidField/BedRaycasts.h"
#include "FluidField/FallRefraction.h"
#include "FluidField/FallSites.h"
#include "FluidField/GpuSpans.h"
#include "FluidField/GroundHeightMap.h"
#include "FluidField/LandscapeCache.h"
#include "FluidField/MeshCut.h"
#include "FluidField/RiverCapture.h"
#include "FluidField/ShallowWaterSolver.h"
#include "FluidField/VanillaFoam.h"
#include "FluidField/WakeSources.h"
#include "FluidField/WaterMapBuilder.h"
#include "FluidField/WaterSurfaces.h"
#include "FluidField/WindSolver.h"

/**
 * @brief Player-centred fluid simulation: shallow water, waterfalls and ground fog.
 *
 * A stable-fluids wind layer on a toroidal grid that follows the player, a riverbed and water depth map, and a
 * shallow-water layer. Owns every GPU resource; cleared after each loading screen; while disabled it creates and
 * dispatches nothing.
 */
struct FluidField : Feature
{
	virtual inline std::string GetName() override { return "Fluid Field"; }
	virtual std::string GetDisplayName() override { return T("feature.fluid_field.name", "Fluid Field"); }
	virtual inline std::string GetShortName() override { return "FluidField"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kWater; }

	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.fluid_field.description", "A real-time fluid simulation around the player: shallow water, waterfalls and ground fog."),
			{ T("feature.fluid_field.key_feature_1", "Shallow water that flows around obstacles and carries wakes"),
				T("feature.fluid_field.key_feature_2", "Ground fog that moving actors push aside") } };
	}

	/** @brief Allowed grid resolutions. The shaders wrap with a bitmask, so each must be a power of two. */
	static constexpr std::array<uint, 5> GRID_RESOLUTIONS = { 64, 128, 256, 512, 1024 };
	static constexpr float MIN_GRID_SIZE = 1024.0f;
	static constexpr float MAX_GRID_SIZE = 16384.0f;
	static constexpr uint MIN_JACOBI_ITERATIONS = 1;
	static constexpr uint MAX_JACOBI_ITERATIONS = 100;
	/** @brief Longest step simulated in one frame, so a hitch does not fling the wind across the grid. */
	static constexpr float MAX_STEP_SECONDS = 1.0f / 20.0f;
	static constexpr uint MIN_RAYS_PER_FRAME = 1;
	static constexpr uint MAX_RAYS_PER_FRAME = 256;
	static constexpr float MIN_RAY_SPACING = 32.0f;
	static constexpr float MAX_RAY_SPACING = 256.0f;

	struct Settings
	{
		bool Enabled = false;
		uint GridResolution = 256;           ///< Texels per side
		float GridSize = 4096.0f;            ///< World units per side
		uint JacobiIterations = 30;          ///< Pressure solve iterations per step
		float WindDamping = 0.3f;            ///< Fraction of wind speed lost per second, as an exponential rate
		bool RaycastBed = true;              ///< Find rocks, banks and riverbed meshes under water with Havok rays
		uint RaysPerFrame = 32;              ///< Most bed rays cast in one frame
		float RaySpacing = 64.0f;            ///< World units between bed rays
		bool MeshCutObstacles = true;        ///< Water-line obstacles from cutting placed meshes at the water height (D15)
		bool WaterEnabled = false;           ///< Experimental: simulate shallow water on the grid (needs the depth map)
		float WakeStrength = 1.0f;           ///< How strongly the player drags the water along
		uint WaterResolution = 512;          ///< Near water texels per side; its own grid, separate from the wind's
		float WaterSize = 4096.0f;           ///< Near water width in world units (full detail: obstacles, wakes)
		bool FarWater = true;                ///< A coarser water level around the near one, so rivers stay simulated further out
		float FarWaterScale = 4.0f;          ///< Far water width as a multiple of the near width, at the same resolution
		float SurfaceNormalStrength = 1.0f;  ///< How strongly the simulated waves tilt the water's surface normal
		float WaveDamping = 0.3f;            ///< Fraction of the water's speed lost per second, as an exponential rate
		float SplashStrength = 1.0f;         ///< How hard actors entering or leaving the water push it
		float RiverSpeed = 150.0f;           ///< River current at full flowmap strength, world units per second (0: test current only)
		float CurrentPull = 0.25f;           ///< Per-second pull of the water toward the river current; lower lets obstacles shape the flow
		float Turbulence = 0.4f;             ///< Vorticity confinement: keeps eddies behind obstacles alive (0: off)
		float FlowDetail = 1.2f;             ///< Strength of the surface detail carried by the simulated flow
		float FoamStrength = 1.0f;           ///< How much foam the flow makes at obstacles, rapids, wakes and splashes (0: none)
		bool ReplaceVanillaFoam = true;      ///< Hide vanilla's placed foam, splash and spray effects where the water is simulated
		bool Waterfalls = true;              ///< Waterfalls landing on the simulated water drive it (D22)
		bool HideUnderwaterFalls = true;     ///< Hide the parts of vanilla's falling-water meshes under the water above or below them (D36)
		bool FallWhitening = false;          ///< The falls whiten with their foam and speed (D34); D86: off until the foam layer replaces it
		float FallSpeed = 1.75f;             ///< D90: the falls' flow (texture and wobble) as a multiple of vanilla's speed
		float FallFoamStart = 32.0f;         ///< World units the water falls (the bake's height fallen) before it takes air or foam
		float FallAerationFoam = 1.0f;       ///< How much of the bake's aeration turns to white foam (0: none)
		float FallAerationFrom = 0.3f;       ///< The bake's aeration where that foam begins (whole at 1)
		float FallAerationMilk = 1.0f;       ///< Scales how milky aeration makes the clear water
		float WaterfallStrength = 1.0f;      ///< How hard waterfalls churn, push and foam the water
		float WaterTint = 0.5f;              ///< How much the water type's colour tints what is seen through all water (0: vanilla)
		bool InteractiveFog = true;          ///< Ground fog that actors and the weather's wind move (D21)
		float FogDensity = 0.08f;            ///< Ground fog extinction per world unit inside the layer, in its banks
		float FogBrightness = 0.15f;         ///< How brightly the ground fog is lit, as a fraction of the light around it
		float FogThickness = 18.0f;          ///< Height of the ground fog layer's top above the ground, in its banks, world units
		float FogStir = 1.0f;                ///< How strongly moving actors push the air and part the fog
		float FogRefillSeconds = 10.0f;      ///< Seconds for parted fog to fill back in (about two thirds of the way)
		bool ExportSiteInputs = false;       ///< Writes each cached site's sheets, statics and landscape next to the log for tools/unify (JSON only)
	};

	Settings settings;

	/** @brief Draws the ImGui settings UI. */
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	/** @brief Registers the loading-screen listener that resets the field, and installs the water hook. */
	virtual void PostPostLoad() override;

	/** @brief Steps the simulation once per frame. */
	virtual void Prepass() override;

	virtual void ClearShaderCache() override;

	/** @brief FLUID_FIELD, defined for the water shader only. */
	virtual std::string_view GetShaderDefineName() override { return "FLUID_FIELD"; }
	virtual bool HasShaderDefine(RE::BSShader::Type a_type) override { return a_type == RE::BSShader::Type::Water; }

	/** @brief Water grid placement for the water shader (SharedData::fluidFieldSettings); 16-byte sized. */
	struct WaterShaderData
	{
		int32_t GridMin[2]{};
		uint32_t GridRes = 0;
		float CellSize = 0.0f;
		float NormalStrength = 0.0f;
		uint32_t Enabled = 0;
		float FlowDetail = 0.0f;
		float WaterTint = 0.0f;   ///< Applies to all water, simulated or not
		int32_t FarGridMin[2]{};  ///< The far level's placement; FarEnabled = 0 when it has no valid output
		uint32_t FarGridRes = 0;
		float FarCellSize = 0.0f;
		uint32_t FarEnabled = 0;
		float Fade = 0.0f;       ///< 0 to 1 over the first moments after a (re)start, so the water eases in
		uint32_t DebugView = 0;  ///< Unused (0), kept for the layout
		float farPad = 0.0f;
	};
	static_assert(sizeof(WaterShaderData) % 16 == 0);
	/** @brief This frame's water grid placement, or Enabled = 0 when the water outputs are not valid. */
	WaterShaderData GetCommonBufferData() const;

	/** @brief Clamps loaded settings to the ranges the solver supports. */
	static Settings Sanitize(Settings a_settings);

private:
	/** @brief Creates GPU resources and shaders as needed. Returns false if the solver cannot run. */
	bool EnsureSolver();
	void ReleaseSolver();

	/** @brief World units per texel. */
	float GetCellSize() const { return settings.GridSize / static_cast<float>(settings.GridResolution); }
	/** @brief Obstacle points per fine bed-ray point per axis, enough to match the simulation's cell size (at most 4). */
	int32_t GetObstacleRatio() const;
	/** @brief World units between obstacle (mesh-cut) points: the bed rays' fine spacing over GetObstacleRatio. */
	float GetObstacleSpacing() const;

	FluidFieldSim::WindSolver solver;
	FluidFieldSim::LandscapeCache landscape;
	FluidFieldSim::WaterSurfaces water;
	FluidFieldSim::BedRaycasts bedRays;
	FluidFieldSim::MeshCut meshCut;

	/** @brief Drops the depth map's data. */
	void ClearDepthMap();

	/** @brief One water simulation level: its solver, map and this step's placement on the world. */
	struct WaterLevel
	{
		FluidFieldSim::ShallowWaterSolver solver;
		FluidFieldSim::WaterMapBuilder map;
		bool awaitingMap = true;      ///< After a reset, wait for a map built from settled inputs
		uint32_t warmupSubsteps = 0;  ///< Substeps still to run at once, so the water starts already flowing
		float lastCellSize = 0.0f;    ///< A change invalidates the level's contents
		int32_t gridMin[2]{};         ///< World cell of logical texel (0, 0) this step
		bool resourceFailed = false;
	};
	/** @brief Simulated seconds run at once when a level starts, so it begins already flowing around obstacles. */
	static constexpr float WARMUP_SECONDS = 2.0f;
	/** @brief Samples along each pixel's ray through the ground fog. */
	static constexpr uint32_t GROUND_FOG_STEPS = 48;
	/** @brief Matches GroundFogCB in GroundFogCS.hlsl. */
	struct alignas(16) GroundFogConstants
	{
		int32_t gridMin[2];
		uint32_t gridRes;
		float cellSize;
		float density;
		float thickness;
		float maxHeight;
		uint32_t stepCount;
		float albedo[3];
		float maxDistance;
		int32_t groundMin[2];
		uint32_t groundRes;
		float groundCellSize;
		float patternOffset[2];
		uint32_t debugMode;
		float pad;
	};
	static_assert(sizeof(GroundFogConstants) == 80);
	/** @brief The fog's ground map: texels per side and their size (twice the wind grid's width). */
	static constexpr uint32_t FOG_GROUND_RESOLUTION = 512;
	static constexpr float FOG_GROUND_CELL = 16.0f;
	/** @brief Fog drift at the weather's full wind speed, world units per second. */
	static constexpr float MAX_WEATHER_DRIFT = 150.0f;
	/** @brief Distance within which the waterfall sites' debug view is drawn, world units. */
	/** @brief The far water level's foam: a fraction of the near level's, and its most per cell. */
	static constexpr float FAR_FOAM_SCALE = 0.25f;
	static constexpr float FAR_FOAM_MAX = 0.5f;
	/** @brief Substep length of the warm-up, as the regular step's shortest substep. */
	static constexpr float WARMUP_SUBSTEP = 0.012f;
	/** @brief Seconds over which the simulated water fades in after a (re)start. */
	static constexpr float FADE_IN_SECONDS = 1.5f;
	float waterFade = 0.0f;  ///< Current fade-in, 0 to 1

	WaterLevel nearWater;  ///< Full detail around the player: obstacles, bed detail, wakes
	WaterLevel farWater;   ///< Coarser and wider: river flow and turbulence further out
	bool waterShaderFailed = false;
	bool waterResourceFailed = false;
	/** @brief Near water world units per texel. */
	float GetWaterCellSize() const { return settings.WaterSize / static_cast<float>(settings.WaterResolution); }
	FluidFieldSim::WakeSources wakeSources;
	FluidFieldSim::AirSources airSources;
	FluidFieldSim::FogParams fogParams;  ///< This step's fog inputs, when the fog is on

	FluidFieldSim::GroundHeightMap groundHeight;
	winrt::com_ptr<ID3D11ComputeShader> groundFogCS;
	winrt::com_ptr<ID3D11Buffer> groundFogCB;
	bool groundFogFailed = false;
	int32_t fogGridMin[2]{};                      ///< The wind grid's placement this frame, for the fog pass
	int32_t fogGroundMin[2]{};                    ///< The ground map's placement this frame
	float fogPatternOffset[2]{};                  ///< How far the resting fog has drifted with the weather's wind
	bool fogReady = false;                        ///< The fog layer and ground height are valid this frame
	winrt::com_ptr<ID3D11Buffer> cameraSnapshot;  ///< The main camera's per-frame constants, copied during the deferred passes

public:
	/**
	 * @brief D21: draws the ground fog after the world (water and transparents included), before first person, TAA,
	 * upscaling and tonemapping. Called from Deferred's Main_RenderWorld hook.
	 */
	void DrawGroundFogAfterWorld();

	/** @brief From the deferred passes: snapshots the main camera for the fog and falls drawn after the world, copies the scene for the falls. */
	void DrawGroundFogBeforeWater();

	/** @brief D159: from State::Draw before each draw: captures the river's draw state (once a frame) to shade the falls with. */
	void CaptureRiverDraw(RE::BSShader* a_shader, uint32_t a_vertexDescriptor, uint32_t a_pixelDescriptor);
	/** @brief D171: from BSWaterShader::SetupGeometry; tells which water type the next water draw is. */
	void SetupWaterGeometry(RE::BSRenderPass* a_pass);

	struct Hooks
	{
		/** @brief After the engine's water setup: records the water geometry drawn next (SetupWaterGeometry). */
		struct BSWaterShader_SetupGeometry
		{
			static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};
		static void Install();
	};

private:
	void DrawGroundFog(ID3D11UnorderedAccessView* a_main, ID3D11ShaderResourceView* a_depth);

	FluidFieldSim::VanillaFoam vanillaFoam;
	FluidFieldSim::FallSites fallSites;
	float fallSurfaceTime = 0.0f;          ///< Seconds, for the surfaces' flow
	float fallPhase = 0.0f;                ///< D67: the falls' shared scroll (wrapped), so a cascade's meshes match
	uint32_t fallPhaseFrame = UINT32_MAX;  ///< D85: the rendered frame fallDrawPhase was latched for
	float fallDrawPhase = 0.0f;            ///< D85: this frame's phase for every falls draw (identical in both depth passes)
	float fallDrawPhasePrevious = 0.0f;    ///< D85: previous frame's, for the wobble's motion vectors
	float fallRate = 0.0f;                 ///< D67: its rate, texture coords/s (vanilla's, averaged over materials)

	/** @brief D28, D36: pool surface for cutting vanilla's falls, and each lit falls mesh's flow (D59). Matches FallGroundCB. */
	struct FallGroundData
	{
		float Origin[2];
		float InverseCell;
		float Size;
		uint32_t Enabled;
		uint32_t DebugMode;
		float Time;
		float PoolFoamHeight;
		float Scroll[2];
		float TexOffset[2];
		float CameraOffset[3];
		uint32_t Whitening;
		uint32_t VertexBase;
		uint32_t HideUnderwater;
		float InverseScale[2];
		uint32_t BakeOn;
		float PreviousPhase;    ///< D85: last frame's Scroll[0], for the wobble's motion vectors
		float BlendAlpha;       ///< Unused, kept for FallGroundCB's layout
		uint32_t BakeVelocity;  ///< A unified sheet: the flow's velocity from its bake's coordinates (its own uv is not vanilla's scale)
		float BakeRect[4];
		float AerationLook[4];  ///< x FallFoamStart, y FallAerationFoam, z FallAerationMilk, w FallAerationFrom
	};
	static_assert(sizeof(FallGroundData) == 128);
	/**
	 * @brief D56-D85: fills fallGroundData for a grounded falls mesh (scroll removed, latched phase, vertex base); o_bake gets
	 * its bake or null. Shared by the lit and refractive (D91) passes so both read the same flow.
	 */
	void PrepareFallGroundData(const RE::BSGeometry* a_geometry, RE::BSShaderProperty* a_property, ID3D11ShaderResourceView*& o_bake);
	/** @brief D91: the cached sites' falls as see-through water, shaded by their river's own shader, drawn after the world. True if drawn. */
	bool DrawRefractiveFalls(ID3D11RenderTargetView* a_target, ID3D11Texture2D* a_targetTexture, ID3D11DepthStencilView* a_depth, ID3D11ShaderResourceView* a_sceneDepth);
	/** @brief D91: whether the falls pass runs this frame. */
	bool RefractiveFallsActive() const;
	/** @brief Our pass has drawn the falls lately, so vanilla's pieces at the cached sites are hidden. */
	bool fallsShown = false;
	/** @brief Frames since our pass last drew the falls. */
	uint32_t fallsMissed = 0;
	/** @brief Frames without our pass drawing (no river captured, its shader still compiling) before vanilla's falls show again. */
	static constexpr uint32_t FALLS_FALLBACK_FRAMES = 60;
	FluidFieldSim::GpuSpans gpuSpans;                                                        ///< D192: GPU timings of water, falls and simulation; logged when slow
	FluidFieldSim::FallRefraction fallRefraction;                                            ///< D91
	std::unordered_map<const RE::TESWaterForm*, FluidFieldSim::RiverCapture> riverCaptures;  ///< D159, D171: each water type's draw state, for the falls drawn by its shader
	const RE::BSGeometry* waterGeometry = nullptr;                                           ///< D171: the water geometry last set up (SetupWaterGeometry)
	std::unordered_map<const RE::TESWaterForm*, const RE::BSGeometry*> riverPieces;          ///< D171: each water type's river piece, kept while it is drawn
	/** @brief D98: the water type each refractive falls piece pours from (null: none); refreshed every FALL_WATER_REFRESH frames. */
	std::unordered_map<const RE::BSGeometry*, const RE::TESWaterForm*> fallWaterTypes;
	uint32_t fallWaterTypesAge = 0;
	static constexpr uint32_t FALL_WATER_REFRESH = 120;
	static constexpr uint32_t FALL_GROUND_SIZE = 1024;
	static constexpr float FALL_GROUND_CELL = 16.0f;        ///< Must equal FallCapture::CELL: pools are copied texel for texel
	static constexpr float DEFAULT_FALL_SCROLL = 0.15f;     ///< D60: vanilla's scroll before it is measured (texture coordinates per second)
	static constexpr float FALL_GROUND_RECENTRE = 4096.0f;  ///< How far the player moves before the map recentres
	static constexpr float FALL_POOL_FOAM = 26.0f;          ///< World units above the water below over which the falls foam into it (D36)
	winrt::com_ptr<ID3D11Texture2D> fallGroundTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> fallGroundSRV;
	winrt::com_ptr<ID3D11Buffer> fallClipBuffer;  ///< D56: the falls' per-vertex rock cut (FallSites::GetVertexClips)
	winrt::com_ptr<ID3D11ShaderResourceView> fallClipSRV;
	uint64_t fallClipVersion = UINT64_MAX;
	std::unordered_map<const RE::BSGeometry*, winrt::com_ptr<ID3D11ShaderResourceView>> fallBakeSRVs;  ///< D59: each lit falls mesh's flow bake
	uint64_t fallBakeVersion = UINT64_MAX;
	/** @brief D56: vanilla's scroll of a falls material, measured from its texture offset over time. */
	struct FallScroll
	{
		float offset = 0.0f;
		float time = 0.0f;
		float rate = 0.0f;
		bool known = false;
		float scale = 1.0f;      ///< Vanilla's texture scale along v, averaged to drop its slow zoom
		float scaleTime = 0.0f;  ///< fallSurfaceTime of that scale
	};
	std::unordered_map<const void*, FallScroll> fallScrolls;
	FallGroundData fallGroundData{};
	float fallGroundOrigin[2]{};
	uint64_t fallGroundVersion = UINT64_MAX;
	std::vector<float> fallGroundScratch;
	std::vector<float> fallPoolScratch;
	winrt::com_ptr<ID3D11Texture2D> fallPlungeTexture;  ///< FallSites::BuildPlungeMap, for the water's foam (D44)
	winrt::com_ptr<ID3D11ShaderResourceView> fallPlungeSRV;
	std::vector<float> fallPlungeScratch;
	winrt::com_ptr<ID3D11Texture2D> fallLipTexture;  ///< D170: FallSites::BuildLipMap, the river drawn on toward the falls' lips
	winrt::com_ptr<ID3D11ShaderResourceView> fallLipSRV;
	std::vector<float> fallLipScratch;
	/** @brief Rebuilds the ground, pool and plunge maps when the sites change or the player moves far; uploads the constants. */
	void UpdateFallGround(const RE::NiPoint3& a_playerPos);
	void ReleaseFallGround();
	std::vector<FluidFieldSim::WaterEmitter> levelEmitters;  ///< This level's emitters: wakes and waterfalls

	/** @brief Creates the water layer's GPU resources and shaders as needed. Returns false if it cannot run. */
	bool EnsureWater(WaterLevel& a_level, uint32_t a_resolution, float a_size, bool a_obstacles);
	void ReleaseWater();
	/** @brief Updates one level's water map, then steps it centred on the player; the near level also takes obstacles and wakes. */
	void StepWater(ID3D11DeviceContext* a_context, WaterLevel& a_level, bool a_near, uint32_t a_resolution, float a_size, const RE::NiPoint3& a_playerPos, float a_deltaTime);
	/**
	 * @brief Uploads changed obstacle blocks (ray detail with mesh-cut solids on top). Mesh-cut blocks stay resident only
	 * inside the obstacle texture's window, since further blocks would alias in the wrapped texture.
	 */
	void UploadObstacles(ID3D11DeviceContext* a_context, const RE::NiPoint3& a_playerPos);
	/** @brief One obstacle block at fine coordinates: ray detail where refined, mesh-cut solids over it. */
	std::array<float, FluidFieldSim::MeshCut::BLOCK * FluidFieldSim::MeshCut::BLOCK * 4> ComposeObstacleBlock(int32_t a_originX, int32_t a_originY) const;
	/** @brief Mesh-cut blocks currently uploaded, keyed by block coordinates. */
	std::unordered_set<uint64_t> residentMeshBlocks;
	/** @brief Placement of the water outputs last written, for the water shader. */
	WaterShaderData waterShaderData;
	/** @brief Binds the water surface output for the water shader (t66) when it is valid. */
	void BindWaterOutputs(ID3D11DeviceContext* a_context) const;
	int32_t meshWindowCentre[2]{ INT32_MIN, INT32_MIN };
	bool shaderCompileFailed = false;
	bool resourceCreateFailed = false;

	/** @brief Set by the loading-screen listener on the UI thread, consumed on the render thread. */
	std::atomic<bool> queuedReset = true;
	/** @brief Same as queuedReset, for the landscape cache; consumed on the main thread. */
	std::atomic<bool> queuedLandscapeClear = true;

	/** @brief Cell size the field was last stepped with; a change invalidates its contents. */
	float lastCellSize = 0.0f;

	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
		static bool Register();
	};
};
