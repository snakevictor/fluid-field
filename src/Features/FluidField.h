#pragma once

#include "FluidField/BedRaycasts.h"
#include "FluidField/DepthMapView.h"
#include "FluidField/LandscapeCache.h"
#include "FluidField/MeshCut.h"
#include "FluidField/ShallowWaterSolver.h"
#include "FluidField/WaterMapBuilder.h"
#include "FluidField/WaterSurfaces.h"
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
	static constexpr std::array<uint, 4> GRID_RESOLUTIONS = { 64, 128, 256, 512 };
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
		uint GridResolution = 256;        ///< Texels per side
		float GridSize = 4096.0f;         ///< World units per side
		uint JacobiIterations = 30;       ///< Pressure solve iterations per step
		float WindDamping = 0.3f;         ///< Fraction of wind speed lost per second, as an exponential rate
		float TestPuffStrength = 800.0f;  ///< Test puff wind speed at its centre, world units per second
		float TestPuffRadius = 256.0f;    ///< Test puff radius, world units
		bool DepthMapEnabled = false;     ///< Experimental: build the riverbed and obstacle map (R1)
		bool VerifyLandscape = false;     ///< Debug: compare cached landscape heights with the engine's
		bool VerifyWater = false;         ///< Debug: compare water surface heights with the engine's
		bool RaycastBed = true;           ///< Find rocks, banks and riverbed meshes under water with Havok rays
		uint RaysPerFrame = 32;           ///< Most bed rays cast in one frame
		float RaySpacing = 64.0f;         ///< World units between bed rays
		bool LogDepthMap = false;         ///< Debug: log what bed rays hit and periodic depth map statistics
		bool MeshCutObstacles = true;     ///< Water-line obstacles from cutting placed meshes at the water height (D15)
		bool WaterEnabled = false;        ///< Experimental: simulate shallow water on the grid (needs the depth map)
		float WakeStrength = 1.0f;        ///< How strongly the player drags the water along
		float TestCurrentSpeed = 0.0f;    ///< Test current until vanilla flow is read, world units per second
		float TestCurrentAngle = 0.0f;    ///< Test current heading in degrees, 0 = north, clockwise
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

	/**
	 * @brief Always valid: Fluid Field adds no defines to engine shaders, so its version and install
	 * state cannot change them, and its own compute shaders compile at runtime. Without this, every
	 * version bump discarded the engine shader cache index. Remove both overrides once Fluid Field
	 * adds a define to an engine shader (the flowing water slice's FLUID_FIELD in Water.hlsl).
	 */
	virtual bool ValidateCache(CSimpleIniA&) override { return true; }
	virtual void WriteDiskCacheInfo(CSimpleIniA&) override {}

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

	FluidFieldSim::ShallowWaterSolver waterSolver;
	FluidFieldSim::WaterMapBuilder waterMap;
	bool waterShaderFailed = false;
	bool waterResourceFailed = false;
	RE::NiPoint3 lastPlayerPos;
	bool hasLastPlayerPos = false;
	/** @brief After a reset, the water waits for a map built from this location's settled inputs. */
	bool waterAwaitingMap = true;

	/** @brief Creates the water layer's GPU resources and shaders as needed. Returns false if it cannot run. */
	bool EnsureWater();
	void ReleaseWater();
	/** @brief Rebuilds the water map, then steps and writes out the water layer for this frame. */
	void StepWater(ID3D11DeviceContext* a_context, const RE::NiPoint3& a_playerPos, const int32_t a_gridMin[2], float a_cellSize, float a_deltaTime);
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
