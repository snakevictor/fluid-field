#pragma once

#include "Feature.h"
#include "FluidField/FallRefraction.h"
#include "FluidField/FallSites.h"
#include "FluidField/RiverCapture.h"

/**
 * @brief Waterfalls: the cached falls sites drawn as see-through water, shaded by their river's own shader.
 *
 * Rebuilds the falls' ground, pool, plunge and lip maps as the player moves, and draws the falls after the world. Shallow
 * Water reads the plunges and maps prepared here; nothing here needs it.
 */
struct FluidFieldWaterfalls : Feature
{
	virtual inline std::string GetName() override { return "Waterfalls"; }
	virtual std::string GetDisplayName() override { return T("feature.fluid_field_waterfalls.name", "Waterfalls"); }
	virtual inline std::string GetShortName() override { return "FluidFieldWaterfalls"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kFluidField; }

	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.fluid_field_waterfalls.description", "Draws waterfalls as clear, flowing water shaded like the river they pour from."),
			{ T("feature.fluid_field_waterfalls.key_feature_1", "See-through falls that refract what is behind them"),
				T("feature.fluid_field_waterfalls.key_feature_2", "White water where the falls take air and foam"),
				T("feature.fluid_field_waterfalls.key_feature_3", "Hides the parts of vanilla's falls buried in water or rock") } };
	}

	struct Settings
	{
		bool Enabled = true;
		float WaterfallStrength = 1.0f;   ///< How hard waterfalls churn, push and foam the water (with Shallow Water)
		bool HideUnderwaterFalls = true;  ///< Hide the parts of vanilla's falling-water meshes under the water above or below them (D36)
		bool FallWhitening = true;        ///< The falls whiten with their foam and speed (D34)
		float FallSpeed = 1.75f;          ///< D90: the falls' flow (texture and wobble) as a multiple of vanilla's speed
		float FallFoamStart = 32.0f;      ///< World units the water falls (the bake's height fallen) before it takes air or foam
		float FallAerationFoam = 1.0f;    ///< How much of the bake's aeration turns to white foam (0: none)
		float FallAerationFrom = 0.3f;    ///< The bake's aeration where that foam begins (whole at 1)
		float FallAerationMilk = 1.0f;    ///< Scales how milky aeration makes the clear water
		bool ExportSiteInputs = false;    ///< Writes each cached site's sheets, statics and landscape next to the log for tools/unify (JSON only)
		bool HotReloadBakes = false;      ///< Dev: re-reads a unified site's bake when its file changes (JSON only)
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

	/** @brief Registers the shared loading-screen listener and installs the water geometry hook. */
	virtual void PostPostLoad() override;

	/** @brief Updates the falls sites and their maps once per frame. */
	virtual void Prepass() override;

	/** @brief Drops the falls' shaders so they recompile. */
	virtual void ClearShaderCache() override;

	/** @brief Clamps loaded settings to their ranges. */
	static Settings Sanitize(Settings a_settings);

	/** @brief Loaded and enabled. */
	bool IsActive() const { return loaded && settings.Enabled; }

	/** @brief D35: the plunges' pushes on one shallow water level, after its wakes. */
	void AppendWaterEmitters(std::vector<FluidFieldSim::WaterEmitter>& a_emitters, const int32_t a_gridMin[2], float a_cellSize, uint32_t a_gridRes, uint32_t a_maxEmitters,
		bool a_foam) const;

	/** @brief The plunge and lip maps on the falls' ground lattice, for the shallow water's foam and flow. */
	struct PlungeMaps
	{
		ID3D11ShaderResourceView* plunge = nullptr;
		ID3D11ShaderResourceView* lip = nullptr;
		float origin[2]{};
		float inverseCell = 0.0f;
		uint32_t size = 0;
	};
	/** @brief This frame's plunge and lip maps; false when there are none. */
	bool GetPlungeMaps(PlungeMaps& o_maps) const;

	/** @brief From the deferred passes: releases last frame's river captures and copies the scene for the falls to refract. */
	void DrawBeforeWater();
	/** @brief D91: the see-through falls, over the finished world (water included). */
	void DrawAfterWorld();

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
	/** @brief Releases the falls' sites, maps and draw state. */
	void Release();
	bool released = true;

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
	winrt::com_ptr<ID3D11Buffer> fallClipBuffer;  ///< D56: the falls' per-vertex rock cut (FallSites::GetVertexClips); grows only, written in place
	winrt::com_ptr<ID3D11ShaderResourceView> fallClipSRV;
	uint32_t fallClipCapacity = 0;  ///< Elements fallClipBuffer holds
	uint32_t fallClipCount = 0;     ///< Elements fallClipSRV shows
	uint64_t fallClipVersion = UINT64_MAX;
	/** @brief D59: a lit falls mesh's flow bake on the GPU, and the FlowBake::stamp it was made from. */
	struct FallBakeView
	{
		winrt::com_ptr<ID3D11ShaderResourceView> view;
		uint64_t stamp = 0;
	};
	std::unordered_map<const RE::BSGeometry*, FallBakeView> fallBakeSRVs;
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
	std::vector<float> fallGroundUpload;   ///< The ground and pool maps interleaved for upload
	std::vector<uint16_t> fallHalfUpload;  ///< The plunge or lip map as halves for upload
	/** @brief Rebuilds the ground, pool and plunge maps when the sites change or the player moves far; uploads the constants. */
	void UpdateFallGround(const RE::NiPoint3& a_playerPos);
	void ReleaseFallGround();
};
