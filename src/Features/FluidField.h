#pragma once

#include "FluidField/AirSources.h"
#include "FluidField/BedRaycasts.h"
#include "FluidField/DepthMapView.h"
#include "FluidField/FallSites.h"
#include "FluidField/GroundHeightMap.h"
#include "FluidField/LandscapeCache.h"
#include "FluidField/MeshCut.h"
#include "FluidField/ShallowWaterSolver.h"
#include "FluidField/VanillaFoam.h"
#include "FluidField/WakeSources.h"
#include "FluidField/WaterMapBuilder.h"
#include "FluidField/WaterSurfaces.h"
#include "FluidField/WaterfallSources.h"
#include "FluidField/WindSolver.h"
#include "OverlayFeature.h"

/**
 * @brief Player-centred fluid simulation shared by water, fog, smoke and buoyancy outputs.
 *
 * Phase 1: a wind layer solved with stable fluids on a grid that follows the player with
 * toroidal addressing, a depth map of riverbed and vanilla water, and (flowing water slice) a
 * shallow-water layer on the same grid. Every GPU resource is created and freed here, and the
 * field is cleared after every loading screen. While disabled it creates nothing and dispatches
 * nothing.
 */
struct FluidField : OverlayFeature
{
	virtual inline std::string GetName() override { return "Fluid Field"; }
	virtual std::string GetDisplayName() override { return T("feature.fluid_field.name", "Fluid Field"); }
	virtual inline std::string GetShortName() override { return "FluidField"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kWater; }

	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.fluid_field.description", "A real-time fluid simulation around the player that drives interactive water, fog and smoke."),
			{ T("feature.fluid_field.key_feature_1", "Shallow water that flows around obstacles and carries wakes"),
				T("feature.fluid_field.key_feature_2", "Fog and smoke pushed by actors, shouts and spells") } };
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
		float TestPuffStrength = 800.0f;     ///< Test puff wind speed at its centre, world units per second
		float TestPuffRadius = 256.0f;       ///< Test puff radius, world units
		bool DepthMapEnabled = false;        ///< Experimental: build the riverbed and obstacle map (R1)
		bool VerifyLandscape = false;        ///< Debug: compare cached landscape heights with the engine's
		bool VerifyWater = false;            ///< Debug: compare water surface heights with the engine's
		bool RaycastBed = true;              ///< Find rocks, banks and riverbed meshes under water with Havok rays
		uint RaysPerFrame = 32;              ///< Most bed rays cast in one frame
		float RaySpacing = 64.0f;            ///< World units between bed rays
		bool LogDepthMap = false;            ///< Debug: log what bed rays hit and periodic depth map statistics
		bool MeshCutObstacles = true;        ///< Water-line obstacles from cutting placed meshes at the water height (D15)
		bool WaterEnabled = false;           ///< Experimental: simulate shallow water on the grid (needs the depth map)
		float WakeStrength = 1.0f;           ///< How strongly the player drags the water along
		float TestCurrentSpeed = 0.0f;       ///< Test current until vanilla flow is read, world units per second
		float TestCurrentAngle = 0.0f;       ///< Test current heading in degrees, 0 = north, clockwise
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
		int WaterfallDebug = 0;              ///< Debug: 1 the flow's speed, foam and water; 2 its streamlines; 3 grounding without the streamline texture
		uint32_t FallLayerMask = 0x1;        ///< Which parts of vanilla's falling-water models show while waterfall effects are hidden (bits: inner surface, white-water sheet, streaks, particles)
		bool GroundWaterfalls = true;        ///< Bend vanilla's waterfall meshes onto the rock and flow them around it (D26)
		bool HideUnderwaterFalls = true;     ///< Hide the parts of vanilla's falling-water meshes under the water above or below them (D36)
		bool FallWhitening = true;           ///< The falls whiten with their foam and speed (D34)
		float WaterfallStrength = 1.0f;      ///< How hard waterfalls churn, push and foam the water
		int SprayAmount = 200;               ///< Spray droplets alive per emitter at once (D31)
		float SpraySize = 1.4f;              ///< Spray droplet width, world units
		float SprayOpacity = 0.2f;           ///< How opaque each droplet is
		int MistAmount = 0;                  ///< Mist puffs alive per emitter at once
		float MistOpacity = 0.06f;           ///< How dense each mist puff is
		float WaterTint = 0.5f;              ///< How much the water type's colour tints what is seen through all water (0: vanilla)
		bool InteractiveFog = true;          ///< Ground fog that actors and the weather's wind move (D21)
		float FogDensity = 0.08f;            ///< Ground fog extinction per world unit inside the layer, in its banks
		float FogBrightness = 0.15f;         ///< How brightly the ground fog is lit, as a fraction of the light around it
		float FogThickness = 18.0f;          ///< Height of the ground fog layer's top above the ground, in its banks, world units
		float FogStir = 1.0f;                ///< How strongly moving actors push the air and part the fog
		float FogRefillSeconds = 10.0f;      ///< Seconds for parted fog to fill back in (about two thirds of the way)
	};

	Settings settings;

	/** @brief Draws the ImGui settings UI. */
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	/** @brief Registers the loading-screen listener that resets the field. */
	virtual void PostPostLoad() override;

	/** @brief Steps the simulation once per frame. */
	virtual void Prepass() override;

	virtual void ClearShaderCache() override;

	/** @brief FLUID_FIELD, defined for the water shader only. */
	virtual std::string_view GetShaderDefineName() override { return "FLUID_FIELD"; }
	virtual bool HasShaderDefine(RE::BSShader::Type a_type) override { return a_type == RE::BSShader::Type::Water || a_type == RE::BSShader::Type::Effect || a_type == RE::BSShader::Type::Lighting || a_type == RE::BSShader::Type::Utility; }

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
		float Fade = 0.0f;  ///< 0 to 1 over the first moments after a (re)start, so the water eases in
		float farPad[2]{};
	};
	static_assert(sizeof(WaterShaderData) % 16 == 0);
	/** @brief This frame's water grid placement, or Enabled = 0 when the water outputs are not valid. */
	WaterShaderData GetCommonBufferData() const;

	/** @brief Draws the per-layer debug overlay when it is turned on from the menu. */
	virtual void DrawOverlay() override;
	/** @brief True while the debug overlay is turned on and has layer images to show. */
	virtual bool IsOverlayVisible() const override;

	/** @brief Clamps loaded settings to the ranges the solver supports. */
	static Settings Sanitize(Settings a_settings);

private:
	/** @brief Creates GPU resources and shaders as needed. Returns false if the solver cannot run. */
	bool EnsureSolver();
	void ReleaseSolver();

	/** @brief World units per texel. */
	float GetCellSize() const { return settings.GridSize / static_cast<float>(settings.GridResolution); }
	/**
	 * @brief Obstacle points per bed-ray fine point along each axis: as many as it takes to match the
	 * simulation's cell size, so mesh-cut obstacles are as sharp as the grid can show (at most 4).
	 */
	int32_t GetObstacleRatio() const;
	/** @brief World units between obstacle (mesh-cut) points: the bed rays' fine spacing over GetObstacleRatio. */
	float GetObstacleSpacing() const;

	FluidFieldSim::WindSolver solver;
	FluidFieldSim::LandscapeCache landscape;
	FluidFieldSim::WaterSurfaces water;
	FluidFieldSim::BedRaycasts bedRays;
	FluidFieldSim::MeshCut meshCut;
	FluidFieldSim::DepthMapView depthView;

	/** @brief Drops the depth map's data and debug images. */
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
	/** @brief The fog area: the ground map's texels per side and their size; twice the wind grid's width, around the player. */
	static constexpr uint32_t FOG_GROUND_RESOLUTION = 512;
	static constexpr float FOG_GROUND_CELL = 16.0f;
	/** @brief Fog drift at the weather's full wind speed, world units per second. */
	static constexpr float MAX_WEATHER_DRIFT = 150.0f;
	/** @brief Distance within which the waterfall sites' debug view is drawn, world units. */
	static constexpr float FALL_SITES_DEBUG_RANGE = 12000.0f;
	/** @brief The far water level's foam: a fraction of the near level's, and its most per cell. */
	static constexpr float FAR_FOAM_SCALE = 0.25f;
	static constexpr float FAR_FOAM_MAX = 0.5f;
	/** @brief Time per frame for mapping the rock under waterfalls, milliseconds. */
	static constexpr double FALL_BED_BUDGET_MS = 0.4;
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
	int32_t fogGridMin[2]{};         ///< The wind grid's placement this frame, for the fog pass
	int32_t fogGroundMin[2]{};       ///< The ground map's placement this frame
	float fogPatternOffset[2]{};     ///< How far the resting fog has drifted with the weather's wind
	bool fogReady = false;           ///< The fog layer and ground height are valid this frame
	uint32_t fogDrawsThisFrame = 0;  ///< Debug: fog passes drawn since the last Prepass
	std::string fogDrawTargets;      ///< Debug: the colour and depth targets they drew into
	uint32_t fogLogCountdown = 0;
	int fogDebugMode = 0;  ///< Debug: isolates parts of the ground fog to find visual problems; not saved
	static constexpr int FOG_DEBUG_PREWATER_DEPTH = 5;
	static constexpr int FOG_DEBUG_BEFORE_WATER = 6;
	static constexpr int FOG_DEBUG_LIVE_CAMERA = 8;
	winrt::com_ptr<ID3D11Buffer> cameraSnapshot;  ///< The main camera's per-frame constants, copied during the deferred passes

public:
	/**
	 * @brief Draws the ground fog into the scene (D21) once the world is rendered, water and
	 * transparent geometry included, before first person, TAA, upscaling and tonemapping. Called
	 * from Deferred's Main_RenderWorld hook.
	 */
	void DrawGroundFogAfterWorld();

	/**
	 * @brief Called as each effect, lighting or utility geometry is set up for drawing: binds the waterfall
	 * flow and cuts (D29, D56) when it is part of a grounded falling-water mesh, and turns them off
	 * again for the next draw otherwise.
	 */
	void SetupFallGeometry(RE::BSRenderPass* a_pass);

	/**
	 * @brief Called from the deferred passes: snapshots the main camera for the ground fog drawn
	 * after the world, and (debug) draws the fog there instead.
	 */
	void DrawGroundFogBeforeWater(ID3D11UnorderedAccessView* a_main);

private:
	void DrawGroundFog(ID3D11UnorderedAccessView* a_main, ID3D11ShaderResourceView* a_depth);

public:
private:
	FluidFieldSim::VanillaFoam vanillaFoam;
	FluidFieldSim::WaterfallSources waterfalls;
	FluidFieldSim::FallSites fallSites;
	FluidFieldSim::FallSurfaceRenderer fallSurface;  ///< D30: the falls' own water surfaces
	float fallSurfaceTime = 0.0f;                    ///< Seconds, for the surfaces' flow
	float fallPhase = 0.0f;                          ///< D67: the falls' one scroll, integrated each frame (a repeat at most): every mesh of a cascade reads the same
	float fallRate = 0.0f;                           ///< D67: ...and its rate, texture coordinates per second (vanilla's, averaged over the falls' materials)

	/**
	 * @brief D28 and D36: the pools' surface around the player, where vanilla's falling water is cut, and
	 * the falls' steady flow, whose streamlines vanilla's streaks follow. Matches FallGroundCB in FallGround.hlsli.
	 */
	struct FallGroundData
	{
		float Origin[2];
		float InverseCell;
		float Size;
		uint32_t Enabled;
		uint32_t DebugMode;
		float Time;
		float FlowSpeed;
		float Scroll[2];
		float TexOffset[2];
		float CameraOffset[3];
		uint32_t Whitening;
		uint32_t VertexBase;
		uint32_t HideUnderwater;
		float PoolFoamHeight;
		uint32_t BakeOn;
		float InverseScale[2];
		float Pad[2];
		float BakeRect[4];
	};
	static_assert(sizeof(FallGroundData) == 112);
	static constexpr uint32_t FALL_GROUND_SIZE = 1024;
	static constexpr float FALL_GROUND_CELL = 16.0f;        ///< Must be FallCapture::CELL: the flows are copied texel for texel
	static constexpr float DEFAULT_FALL_SCROLL = 0.15f;     ///< D60: vanilla's scroll before it is measured (texture coordinates per second)
	static constexpr float FALL_GROUND_RECENTRE = 4096.0f;  ///< How far the player moves before the map recentres
	static constexpr float FALL_POOL_FOAM = 26.0f;          ///< World units above the water below over which the falls foam into it (D36)
	winrt::com_ptr<ID3D11Texture2D> fallGroundTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> fallGroundSRV;
	winrt::com_ptr<ID3D11Texture2D> fallStreamTexture;  ///< FallSites::BuildStreamMap: velocity (x, y), foam, water
	winrt::com_ptr<ID3D11ShaderResourceView> fallStreamSRV;
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
		float scale = 1.0f;      ///< Vanilla's texture scale along v, averaged (its slow zoom left out)
		float scaleTime = 0.0f;  ///< ...as of this fallSurfaceTime
	};
	std::unordered_map<const void*, FallScroll> fallScrolls;
	std::vector<float> fallStreamScratch;
	winrt::com_ptr<ID3D11Buffer> fallGroundOn;  ///< Constants for grounded draws, rewritten per draw (each mesh's texture scroll)
	FallGroundData fallGroundData{};
	winrt::com_ptr<ID3D11Buffer> fallGroundOff;  ///< Enabled = 0, for every other effect draw
	float fallGroundOrigin[2]{};
	uint64_t fallGroundVersion = UINT64_MAX;
	bool fallGroundBound = false;  ///< The last effect draw bound the grounding
	std::vector<float> fallGroundScratch;
	std::vector<float> fallPoolScratch;
	winrt::com_ptr<ID3D11Texture2D> fallPlungeTexture;  ///< FallSites::BuildPlungeMap, for the water's foam (D44)
	winrt::com_ptr<ID3D11ShaderResourceView> fallPlungeSRV;
	std::vector<float> fallPlungeScratch;
	/** @brief Rebuilds the ground and flow maps when the sites change or the player moves far; uploads the constants. */
	void UpdateFallGround(const RE::NiPoint3& a_playerPos);
	void ReleaseFallGround();
	int fallDebugView = 0;                                   ///< Debug: 1 vanilla's fall meshes, 2 their intersections and the spray emitters (D36); not saved
	bool showFallSites = false;                              ///< Debug: draws the waterfall sites (D25) in the world; not saved
	int fallMeshDebug = 0;                                   ///< D75: the falls' meshes debug overlay (FallSites::DrawMeshDebug: 1 wireframe, 2 flow, 3 both); not saved
	bool hideFallEffects = true;                             ///< Debug: hides vanilla's mist, skirt, splash and rapids meshes; not saved
	std::vector<FluidFieldSim::WaterEmitter> levelEmitters;  ///< This level's emitters: wakes and waterfalls

	/** @brief Creates the water layer's GPU resources and shaders as needed. Returns false if it cannot run. */
	bool EnsureWater(WaterLevel& a_level, uint32_t a_resolution, float a_size, bool a_obstacles);
	void ReleaseWater();
	/** @brief Rebuilds the water map, then steps and writes out the water layer for this frame. */
	/** @brief Steps one water level, centred on the player. The near level also takes obstacles and wakes. */
	void StepWater(ID3D11DeviceContext* a_context, WaterLevel& a_level, bool a_near, uint32_t a_resolution, float a_size, const RE::NiPoint3& a_playerPos, float a_deltaTime);
	/**
	 * @brief Uploads changed obstacle blocks: refined ray detail with mesh-cut solids on top.
	 * Mesh-cut blocks are kept resident only inside the obstacle texture's window around the player,
	 * since blocks further away would alias in the wrapped texture.
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
	/** @brief A test puff was requested from the menu; consumed by the next step. */
	bool queuedTestPuff = false;

	// Last step's grid placement, for the overlay's player marker
	int32_t lastGridMin[2]{};
	float lastPlayerTexel[2]{};
	float lastHeading = 0.0f;
	float lastPlayerPos[2]{};

	// Debug overlay, not saved
	bool showDebugOverlay = false;
	bool hasDebugViews = false;
	float debugOverlayScale = 1.0f;
	float debugVelocityRange = 500.0f;   ///< Wind speed shown at full brightness
	float debugDivergenceRange = 1.0f;   ///< Divergence shown at full colour
	float debugPressureRange = 1000.0f;  ///< Pressure shown at full colour

	/** @brief Cell size the field was last stepped with; a change invalidates its contents. */
	float lastCellSize = 0.0f;

	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
		static bool Register();
	};
};
