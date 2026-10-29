#pragma once

#include "Feature.h"
#include "FluidField/FallRefraction.h"
#include "FluidField/FallSites.h"
#include "FluidField/RiverCapture.h"

/**
 * @brief Waterfalls: the cached falls sites drawn as see-through water, shaded by their river's own shader.
 *
 * Shallow Water reads the plunges and maps built here; nothing here needs it.
 */
struct FluidFieldWaterfalls : Feature
{
	virtual inline std::string GetName() override { return "Waterfalls"; }
	virtual std::string GetDisplayName() override { return T("feature.fluid_field_waterfalls.name", "Waterfalls"); }
	virtual inline std::string GetShortName() override { return "FluidFieldWaterfalls"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kFluidField; }

	/** @brief The UI summary panel's description and key features. */
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
		float WaterfallStrength = 1.0f;   ///< How hard the plunges churn, push and foam Shallow Water's water
		bool HideUnderwaterFalls = true;  ///< Hide vanilla's falls where buried in water or rock (D36)
		float FallSpeed = 1.75f;          ///< D90: flow speed as a multiple of vanilla's
		float FallFoamStart = 32.0f;      ///< World units fallen before the water takes air or foam
		float FallAerationFoam = 1.0f;    ///< Share of the bake's aeration that turns to white foam
		float FallAerationFrom = 0.3f;    ///< Aeration where that foam begins (whole at 1)
		float FallAerationMilk = 1.0f;    ///< Scales how milky aeration makes the clear water
		bool ExportSiteInputs = false;    ///< Dev: writes each site's inputs next to the log for tools/unify (JSON only)
		bool HotReloadBakes = false;      ///< Dev: re-reads a site's bake when its file changes (JSON only)
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

	/** @brief Updates the falls sites and their maps. */
	virtual void Prepass() override;

	/** @brief Drops the falls' shaders so they recompile. */
	virtual void ClearShaderCache() override;

	/** @brief Clamps or replaces invalid loaded settings. */
	static Settings Sanitize(Settings a_settings);

	/** @brief Loaded and enabled. */
	bool IsActive() const { return loaded && settings.Enabled; }

	/** @brief D35: the plunges' pushes on one shallow water level, after its wakes. */
	void AppendWaterEmitters(std::vector<FluidFieldSim::WaterEmitter>& a_emitters, const int32_t a_gridMin[2], float a_cellSize, uint32_t a_gridRes, uint32_t a_maxEmitters,
		bool a_foam) const;

	/** @brief The plunge and lip maps on the falls' ground lattice, for Shallow Water's foam and flow. */
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

	/** @brief Releases last frame's river captures and copies the scene for the falls to refract. */
	void DrawBeforeWater();
	/** @brief D91: draws the falls over the finished world, water included. */
	void DrawAfterWorld();

	/** @brief D159: called before each draw; captures the river's draw state to shade the falls with. */
	void CaptureRiverDraw(RE::BSShader* a_shader, uint32_t a_vertexDescriptor, uint32_t a_pixelDescriptor);
	/** @brief D171: records the water geometry the next water draw uses. */
	void SetupWaterGeometry(RE::BSRenderPass* a_pass);

	struct Hooks
	{
		/** @brief Calls SetupWaterGeometry after the engine's water setup. */
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
	float fallPhase = 0.0f;                ///< D67: shared scroll (wrapped), so a cascade's meshes match
	uint32_t fallPhaseFrame = UINT32_MAX;  ///< D85: frame fallDrawPhase was latched for
	float fallDrawPhase = 0.0f;            ///< D85: this frame's phase for every falls draw
	float fallDrawPhasePrevious = 0.0f;    ///< D85: last frame's, for the wobble's motion vectors
	float fallRate = 0.0f;                 ///< D67: scroll rate, texture coords/s (vanilla's, averaged over materials)

	/** @brief D36: per-mesh falls constants (pool surface, flow, cut). Matches FallGroundCB. */
	struct FallGroundData
	{
		float Origin[2];
		float InverseCell;
		float Size;
		uint32_t Enabled;
		uint32_t Pad0;
		float Time;
		float Pad1;
		float Scroll[2];
		float TexOffset[2];
		float CameraOffset[3];
		uint32_t Pad2;
		uint32_t VertexBase;
		uint32_t HideUnderwater;
		float InverseScale[2];
		uint32_t Pad3;
		float PreviousPhase;  ///< D85: last frame's Scroll[0], for the wobble's motion vectors
		float Pad4;
		uint32_t Pad5;
		float BakeRect[4];
		float AerationLook[4];  ///< x FallFoamStart, y FallAerationFoam, z FallAerationMilk, w FallAerationFrom
	};
	static_assert(sizeof(FallGroundData) == 128);
	/** @brief D85: fills fallGroundData for a grounded falls mesh; o_bake gets its flow bake or null. */
	void PrepareFallGroundData(const RE::BSGeometry* a_geometry, RE::BSShaderProperty* a_property, ID3D11ShaderResourceView*& o_bake);
	/** @brief D91: draws the cached sites' falls; true if drawn. */
	bool DrawRefractiveFalls(ID3D11RenderTargetView* a_target, ID3D11Texture2D* a_targetTexture, ID3D11DepthStencilView* a_depth, ID3D11ShaderResourceView* a_sceneDepth);
	/** @brief D91: whether the falls pass runs this frame. */
	bool RefractiveFallsActive() const;
	/** @brief Our pass drew the falls lately, so vanilla's pieces at the sites hide. */
	bool fallsShown = false;
	/** @brief Frames since our pass last drew the falls. */
	uint32_t fallsMissed = 0;
	/** @brief Frames without our draw (no river captured, shader compiling) before vanilla's falls show again. */
	static constexpr uint32_t FALLS_FALLBACK_FRAMES = 60;
	FluidFieldSim::FallRefraction fallRefraction;                                            ///< D91
	std::unordered_map<const RE::TESWaterForm*, FluidFieldSim::RiverCapture> riverCaptures;  ///< D159, D171: each water type's draw state, for the falls drawn by its shader
	const RE::BSGeometry* waterGeometry = nullptr;                                           ///< D171: the water geometry last set up (SetupWaterGeometry)
	std::unordered_map<const RE::TESWaterForm*, const RE::BSGeometry*> riverPieces;          ///< D171: each water type's river piece, kept while it is drawn
	/** @brief D98: the water type each falls piece pours from (null: none); refreshed every FALL_WATER_REFRESH frames. */
	std::unordered_map<const RE::BSGeometry*, const RE::TESWaterForm*> fallWaterTypes;
	uint32_t fallWaterTypesAge = 0;
	/** @brief DrawRefractiveFalls' per-frame lists, kept to reuse their memory. */
	std::vector<FallGroundData> fallDrawGrounds;
	std::vector<FluidFieldSim::FallRefraction::MeshDraw> fallDraws;
	std::vector<const RE::TESWaterForm*> fallDrawWaters;  ///< D171: each draw's water type
	std::unordered_set<const RE::BSGeometry*> fallDrawKeep;
	std::unordered_map<const RE::TESWaterForm*, FluidFieldSim::FallRefraction::WaterLook> fallDrawLooks;
	static constexpr uint32_t FALL_WATER_REFRESH = 120;
	static constexpr uint32_t FALL_GROUND_SIZE = 1024;
	static constexpr float FALL_GROUND_CELL = 16.0f;        ///< Must equal FallCapture::CELL: pools are copied texel for texel
	static constexpr float DEFAULT_FALL_SCROLL = 0.15f;     ///< D60: vanilla's scroll before it is measured (texture coordinates per second)
	static constexpr float FALL_GROUND_RECENTRE = 4096.0f;  ///< How far the player moves before the map recentres
	winrt::com_ptr<ID3D11Texture2D> fallGroundTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> fallGroundSRV;
	winrt::com_ptr<ID3D11Buffer> fallClipBuffer;  ///< D56: per-vertex rock cut (FallSites::GetVertexClips); grows only
	winrt::com_ptr<ID3D11ShaderResourceView> fallClipSRV;
	uint32_t fallClipCapacity = 0;  ///< Elements fallClipBuffer holds
	uint32_t fallClipCount = 0;     ///< Elements fallClipSRV shows
	uint64_t fallClipVersion = UINT64_MAX;
	/** @brief D59: a falls mesh's flow bake on the GPU, and the FlowBake::stamp it was made from. */
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
	winrt::com_ptr<ID3D11Texture2D> fallPlungeTexture;  ///< D44: FallSites::BuildPlungeMap, for the water's foam
	winrt::com_ptr<ID3D11ShaderResourceView> fallPlungeSRV;
	std::vector<float> fallPlungeScratch;
	winrt::com_ptr<ID3D11Texture2D> fallLipTexture;  ///< D170: FallSites::BuildLipMap, the river's pull toward the lips
	winrt::com_ptr<ID3D11ShaderResourceView> fallLipSRV;
	std::vector<float> fallLipScratch;
	std::vector<float> fallGroundUpload;   ///< Ground and pool maps interleaved
	std::vector<uint16_t> fallHalfUpload;  ///< Plunge or lip map as halves
	/** @brief Rebuilds the falls' maps when the sites change or the player moves far; fills the constants. */
	void UpdateFallGround(const RE::NiPoint3& a_playerPos);
	void ReleaseFallGround();
};
