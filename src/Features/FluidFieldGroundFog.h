#pragma once

#include "Feature.h"
#include "FluidField/AirSources.h"
#include "FluidField/GroundHeightMap.h"
#include "FluidField/WindSolver.h"

/**
 * @brief Ground Fog: fog lying on the ground around the player, carried by a stable-fluids wind layer.
 *
 * The wind runs on a toroidal grid that follows the player; moving actors push the air and part the fog, and the weather's
 * wind drifts it. Drawn over the finished world. The wind only runs while this feature is on.
 */
struct FluidFieldGroundFog : Feature
{
	virtual inline std::string GetName() override { return "Ground Fog"; }
	virtual std::string GetDisplayName() override { return T("feature.fluid_field_ground_fog.name", "Ground Fog"); }
	virtual inline std::string GetShortName() override { return "FluidFieldGroundFog"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kFluidField; }

	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.fluid_field_ground_fog.description", "Low fog lying on the ground around the player, in banks and clearings."),
			{ T("feature.fluid_field_ground_fog.key_feature_1", "Parts and swirls around moving actors, then settles back"),
				T("feature.fluid_field_ground_fog.key_feature_2", "Drifts with the weather's wind") } };
	}

	static constexpr uint MIN_JACOBI_ITERATIONS = 1;
	static constexpr uint MAX_JACOBI_ITERATIONS = 100;

	struct Settings
	{
		bool Enabled = false;
		float FogDensity = 0.08f;        ///< Ground fog extinction per world unit inside the layer, in its banks
		float FogBrightness = 0.15f;     ///< How brightly the ground fog is lit, as a fraction of the light around it
		float FogThickness = 18.0f;      ///< Height of the ground fog layer's top above the ground, in its banks, world units
		float FogStir = 1.0f;            ///< How strongly moving actors push the air and part the fog
		float FogRefillSeconds = 10.0f;  ///< Seconds for parted fog to fill back in (about two thirds of the way)
		uint GridResolution = 512;       ///< Wind texels per side
		float GridSize = 4096.0f;        ///< Wind grid width, world units
		uint JacobiIterations = 42;      ///< Pressure solve iterations per step
		float WindDamping = 0.2f;        ///< Fraction of wind speed lost per second, as an exponential rate
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

	/** @brief Steps the wind and fog once per frame. */
	virtual void Prepass() override;

	/** @brief Drops the wind and fog shaders so they recompile. */
	virtual void ClearShaderCache() override;

	/** @brief Clamps loaded settings to the ranges the solver supports. */
	static Settings Sanitize(Settings a_settings);

	/** @brief Loaded and enabled. */
	bool IsActive() const { return loaded && settings.Enabled; }

	/**
	 * @brief D21: draws the ground fog after the world (water and transparents included), before first person, TAA,
	 * upscaling and tonemapping.
	 */
	void DrawAfterWorld();

private:
	/** @brief Creates GPU resources and shaders as needed. Returns false if the solver cannot run. */
	bool EnsureSolver();
	/** @brief Releases the wind, the fog's ground map and its pass. */
	void Release();
	bool released = true;
	/** @brief Loading screens this feature has reset for (Field::ConsumeLoad). */
	uint32_t seenLoads = UINT32_MAX;

	/** @brief World units per texel. */
	float GetCellSize() const { return settings.GridSize / static_cast<float>(settings.GridResolution); }

	void DrawGroundFog(ID3D11UnorderedAccessView* a_main, ID3D11ShaderResourceView* a_depth);

	FluidFieldSim::WindSolver solver;
	FluidFieldSim::AirSources airSources;
	FluidFieldSim::FogParams fogParams;  ///< This step's fog inputs

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

	FluidFieldSim::GroundHeightMap groundHeight;
	winrt::com_ptr<ID3D11ComputeShader> groundFogCS;
	winrt::com_ptr<ID3D11Buffer> groundFogCB;
	bool groundFogFailed = false;
	int32_t fogGridMin[2]{};      ///< The wind grid's placement this frame, for the fog pass
	int32_t fogGroundMin[2]{};    ///< The ground map's placement this frame
	float fogPatternOffset[2]{};  ///< How far the resting fog has drifted with the weather's wind
	bool fogReady = false;        ///< The fog layer and ground height are valid this frame

	bool shaderCompileFailed = false;
	bool resourceCreateFailed = false;
	/** @brief Set by the Reset button on the UI thread, consumed on the render thread. */
	std::atomic<bool> queuedReset = true;
	/** @brief Cell size the wind was last stepped with; a change invalidates its contents. */
	float lastCellSize = 0.0f;
};
