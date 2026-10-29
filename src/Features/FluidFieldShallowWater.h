#pragma once

#include "Feature.h"
#include "FluidField/BedRaycasts.h"
#include "FluidField/MeshCut.h"
#include "FluidField/ShallowWaterSolver.h"
#include "FluidField/VanillaFoam.h"
#include "FluidField/WakeSources.h"
#include "FluidField/WaterMapBuilder.h"

/**
 * @brief Shallow Water: a shallow-water simulation on the rivers around the player, shown on vanilla's water.
 *
 * A near level with obstacles, bed detail and wakes, and a coarser far level around it. Takes the falls' plunges from
 * Waterfalls when it runs. Defines FLUID_FIELD for the water shader, which reads the outputs through
 * SharedData::fluidFieldSettings and t66-t69.
 */
struct FluidFieldShallowWater : Feature
{
	virtual inline std::string GetName() override { return "Shallow Water"; }
	virtual std::string GetDisplayName() override { return T("feature.fluid_field_shallow_water.name", "Shallow Water"); }
	virtual inline std::string GetShortName() override { return "FluidFieldShallowWater"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kFluidField; }

	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.fluid_field_shallow_water.description", "Simulates the water of rivers and streams around the player."),
			{ T("feature.fluid_field_shallow_water.key_feature_1", "Currents that follow the river and flow around rocks and pillars"),
				T("feature.fluid_field_shallow_water.key_feature_2", "Wakes and splashes from actors moving through the water"),
				T("feature.fluid_field_shallow_water.key_feature_3", "Foam that gathers at obstacles and rapids and drifts with the flow") } };
	}

	static constexpr uint MIN_RAYS_PER_FRAME = 1;
	static constexpr uint MAX_RAYS_PER_FRAME = 256;
	static constexpr float MIN_RAY_SPACING = 32.0f;
	static constexpr float MAX_RAY_SPACING = 256.0f;

	struct Settings
	{
		bool Enabled = true;
		float RiverSpeed = 150.0f;           ///< River current at full flowmap strength, world units per second (0: test current only)
		float CurrentPull = 0.25f;           ///< Per-second pull of the water toward the river current; lower lets obstacles shape the flow
		float Turbulence = 0.4f;             ///< Vorticity confinement: keeps eddies behind obstacles alive (0: off)
		float WaveDamping = 0.3f;            ///< Fraction of the water's speed lost per second, as an exponential rate
		float SurfaceNormalStrength = 1.0f;  ///< How strongly the simulated waves tilt the water's surface normal
		float FlowDetail = 1.2f;             ///< Strength of the surface detail carried by the simulated flow
		float FoamStrength = 1.0f;           ///< How much foam the flow makes at obstacles, rapids, wakes and splashes (0: none)
		float WaterTint = 0.5f;              ///< How much the water type's colour tints what is seen through all water (0: vanilla)
		bool ReplaceVanillaFoam = true;      ///< Hide vanilla's placed foam, splash and spray effects where the water is simulated
		float WakeStrength = 1.0f;           ///< How strongly the player drags the water along
		float SplashStrength = 1.0f;         ///< How hard actors entering or leaving the water push it
		uint WaterResolution = 1024;         ///< Near water texels per side
		float WaterSize = 8818.0f;           ///< Near water width in world units (full detail: obstacles, wakes)
		bool FarWater = true;                ///< A coarser water level around the near one, so rivers stay simulated further out
		float FarWaterScale = 8.0f;          ///< Far water width as a multiple of the near width, at the same resolution
		bool RaycastBed = true;              ///< Find rocks, banks and riverbed meshes under water with Havok rays
		uint RaysPerFrame = 32;              ///< Most bed rays cast in one frame
		float RaySpacing = 64.0f;            ///< World units between bed rays
		bool MeshCutObstacles = true;        ///< Water-line obstacles from cutting placed meshes at the water height (D15)
	};

	Settings settings;

	/** @brief Draws the ImGui settings UI. */
	virtual void DrawSettings() override;

	/** @brief Loads and sanitizes the settings. */
	virtual void LoadSettings(json& o_json) override;
	/** @brief Saves the settings. */
	virtual void SaveSettings(json& o_json) override;
	/** @brief Restores the default settings. */
	virtual void RestoreDefaultSettings() override;

	/** @brief Registers the shared loading-screen listener. */
	virtual void PostPostLoad() override;

	/** @brief Steps the water once per frame. */
	virtual void Prepass() override;

	/** @brief Drops the water's compute shaders so they recompile. */
	virtual void ClearShaderCache() override;

	/** @brief FLUID_FIELD, defined for the water shader only. */
	virtual std::string_view GetShaderDefineName() override { return "FLUID_FIELD"; }
	/** @brief Only the water shader reads the simulation. */
	virtual bool HasShaderDefine(RE::BSShader::Type a_type) override { return a_type == RE::BSShader::Type::Water; }

	/** @brief Clamps loaded settings to the ranges the solver supports. */
	static Settings Sanitize(Settings a_settings);

	/** @brief Loaded and enabled. */
	bool IsActive() const { return loaded && settings.Enabled; }

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
	/** @brief This frame's water grid placement, or all zero when the water is off. */
	WaterShaderData GetCommonBufferData() const;

private:
	/** @brief Releases the water levels, bed and obstacle detection, and restores vanilla's foam. */
	void Release();
	bool released = true;
	/** @brief Loading screens this feature has reset for (Field::ConsumeLoad). */
	uint32_t seenLoads = UINT32_MAX;

	/** @brief Obstacle points per fine bed-ray point per axis, enough to match the simulation's cell size (at most 4). */
	int32_t GetObstacleRatio() const;
	/** @brief World units between obstacle (mesh-cut) points: the bed rays' fine spacing over GetObstacleRatio. */
	float GetObstacleSpacing() const;

	FluidFieldSim::BedRaycasts bedRays;
	FluidFieldSim::MeshCut meshCut;

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
	FluidFieldSim::VanillaFoam vanillaFoam;
	std::vector<FluidFieldSim::WaterEmitter> levelEmitters;  ///< This level's emitters: wakes and waterfalls

	/** @brief Creates the water layer's GPU resources and shaders as needed. Returns false if it cannot run. */
	bool EnsureWater(WaterLevel& a_level, uint32_t a_resolution, float a_size, bool a_obstacles);
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
};
