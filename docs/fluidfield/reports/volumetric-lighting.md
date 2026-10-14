# Volumetric Lighting: read-only report for Fluid Field

Branch `feature/fluid-field`, Oct 13, 2026. Phase 0 report (see `docs/fluidfield/ROADMAP.md:67`), input to spikes R4/R5 (`ROADMAP.md:98-99`).

**Main finding.** The "Volumetric Lighting" feature (VL) is a thin wrapper around Skyrim's **vanilla** VL pipeline. It owns almost no GPU resources. It changes the vanilla 3D texture size, swaps in replacement shaders, and collapses the raymarch loop into one dispatch. Its volume holds **one scalar channel**: sun-only, shadow × noise × phase "power". It has no colour, no extinction and no local lights. The thing that actually matches a camera-aligned froxel fog renderer is the **volumetric fog in Exponential Height Fog (EHF)**. EHF is a UE-style near and far froxel pipeline that it creates, owns and frees itself, with sun, moon, sky/IBL, local lights and local shadows. R4 should start from EHF and treat VL as something to coexist with, not something to build on.

---

## 1. Where the code lives

| Piece                                                                                       | Path                                                                                                                                                                                                                                                                                                                      | Notes                                                                                              |
| ------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------- |
| Feature class                                                                               | `src/Features/VolumetricLighting.h:1-179`                                                                                                                                                                                                                                                                                 | `Settings`, `TextureSize`, cached `BSImagespaceShader*` wrappers                                   |
| Feature impl                                                                                | `src/Features/VolumetricLighting.cpp:1-307`                                                                                                                                                                                                                                                                               | UI, REL patches, quality and size switching                                                        |
| Dispatch interception                                                                       | `src/Hooks.cpp:914-985` (`CSShadersSupport`), installs at `src/Hooks.cpp:1089-1099`                                                                                                                                                                                                                                       | Routes vanilla VL compute dispatches to CS shaders                                                 |
| Shader descriptors                                                                          | `src/ShaderCache.cpp:1991-1996, 2015-2016, 2082, 2086-2089`                                                                                                                                                                                                                                                               | Maps VL shader names to cache descriptors (Generate=257, Raymarch=256)                             |
| GPU annotations                                                                             | `src/FrameAnnotations.cpp:522-523, 535-546, 560-566, 795-839`                                                                                                                                                                                                                                                             | RenderDoc/Tracy markers only                                                                       |
| Global instance                                                                             | `src/Globals.cpp:85`, `src/Globals.h:118`; `bEnableVolumetricLighting` at `src/Globals.cpp:126,239`, `src/Globals.h:213`                                                                                                                                                                                                  |                                                                                                    |
| Feature registration                                                                        | `src/Feature.cpp:39, 247`                                                                                                                                                                                                                                                                                                 |                                                                                                    |
| Replacement shaders (all in `package/Shaders/`, **not** in `features/Volumetric Lighting/`) | `ISVolumetricLightingGenerateCS.hlsl:1-104`, `ISVolumetricLightingRaymarchCS.hlsl:1-18`, `ISApplyVolumetricLighting.hlsl:1-64`, `ISVolumetricLightingBlurHCS.hlsl:1-71`, `ISVolumetricLightingBlurVCS.hlsl` (71 lines, mirror of H), `ISVolumetricLighting.hlsl:1-34`, `ISCompositeLensFlareVolumetricLighting.hlsl:1-57` | Shipped with core CS, so they are active whether or not the feature is loaded                      |
| Feature folder                                                                              | `features/Volumetric Lighting/CORE` (marker), `features/Volumetric Lighting/Shaders/Features/VolumetricLighting.ini:1-2`                                                                                                                                                                                                  | No feature-local HLSL                                                                              |
| Weather-form editor (related)                                                               | `src/CSEditor/Weather/VolumetricLightingWidget.cpp:30-230`                                                                                                                                                                                                                                                                | Edits `BGSVolumetricLighting` forms (intensity, density size/wind/fall speed, phase, range factor) |
| Sky Sync VL dimming (related)                                                               | `src/Features/SkySync.cpp:247-248, 296-297, 335-336, 345-389`                                                                                                                                                                                                                                                             | Writes `BSVolumetricLightingRenderData` intensity                                                  |

## 2. How it hooks the engine

### 2.1 REL addresses and patches (`PostPostLoad`, `VolumetricLighting.cpp:162-175`)

-   Engine quality-size globals (`TextureSize*`, as W/H/D ints): Low `RelocationID(527970, 414916)`, Medium `(527973, 414919)`, High `(527976, 414922)`, at `VolumetricLighting.cpp:164-166`. The vanilla High size is cached in `defaultSizeHigh` (`:167`). The defaults in the struct are 320×192×90 (`VolumetricLighting.h:8-13`).
-   **Raymarch loop patch**: two `REL::safe_fill` NOPs in the engine VL render function `RelocationID(100309, 107023)`, at `+0xA4/0x406` (3 bytes, skips the first dispatch) and `+0x147/0x4A9` (6 bytes, exits the per-slice loop after one iteration), at `VolumetricLighting.cpp:169-174`. Vanilla dispatches the raymarch once per depth slice. The replacement `ISVolumetricLightingRaymarchCS.hlsl:10-18` does a prefix sum over Z in one dispatch.
-   Engine functions called: `GetVLDescriptor` `RelocationID(100297, 107014)` (`:233-238`) and `SetVLQuality(descriptor, 0..2)` `RelocationID(100299, 107016)` (`:240-245`).
-   `bEnableVolumetricLighting` (INI bool) `RelocationID(527940, 414913)` (`src/Globals.cpp:239`) is written in `SetupVL` (`VolumetricLighting.cpp:220-231`).
-   Custom quality: the feature overwrites the engine "High" size global with the custom size, then calls `SetVLQuality` (`:224-225, 228-229`). The engine then (re)allocates its own 3D texture.

### 2.2 Compute dispatch interception (`src/Hooks.cpp:914-985`)

-   `BSImagespaceShader::DispatchComputeShader` detour `RelocationID(100952, 107734)` (`Hooks.cpp:1090`) records the current IS shader.
-   `BSComputeShader` vfunc 0x02 (`Hooks.cpp:1096`) records the current `BSComputeShader` and technique.
-   `Renderer::DispatchCSShader` detour `RelocationID(75532, 77329)` (`Hooks.cpp:1099`). When `vl.loaded` is true, it matches by name: `ISVolumetricLightingGenerateCS` and `ISVolumetricLightingRaymarchCS` (dispatched with no IS wrapper) get a fake `BSImagespaceShader` created by `CreateShader` (`VolumetricLighting.cpp:247-257`). `BlurHCS` and `BlurVCS` also bind `vlDataCB` at CS b1 and override the thread-group counts to the dynamic-resolution area (`Hooks.cpp:958-973`, `VolumetricLighting.cpp:287-305`). Finally it swaps in the CS-compiled shader from `shaderCache->GetComputeShader` (`Hooks.cpp:975-979`).
-   If VL is **not** loaded, Generate and Raymarch keep their vanilla compiled shaders. The vanilla raymarch loop is then not patched either, because the patch lives in `PostPostLoad`.

### 2.3 Vanilla VL pass chain (as replaced)

Order inferred from the shader I/O, not verified in RenderDoc:

1. Sun shadow maps plus a dedicated VL shadow map (`RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM`, referenced at `VolumetricShadows.cpp:191`).
2. **GenerateCS**: one thread per froxel, 32×32×1 groups (`ISVolumetricLightingGenerateCS.hlsl:43-103`).
    - Froxel → NDC through a 1D `InverseRepartitionTex` (depth distribution, `:57-58`).
    - Sun cascade shadow (`:69-83`) plus the VL shadow map when `EnableShadowCasting < 0.5` (`:79-82`).
    - 3D noise density, scrolled by `WindInput`, with a fixed height falloff over 300 units (`:85-88`).
    - Phase term (`:90-92`).
    - CS adds Terrain/Cloud shadows through `ShadowSampling::GetWorldShadow` when those defines are set (`:96-98`).
    - Output `RWTexture3D<float> DensityRW` (`:15, :102`).
3. **RaymarchCS**: in-place accumulation along Z (`ISVolumetricLightingRaymarchCS.hlsl:10-18`).
4. **ISApplyVolumetricLighting** (PS): samples the 3D texture at the scene-depth slice using `RepartitionTex` (`ISApplyVolumetricLighting.hlsl:39-42`), adds noise, applies temporal reprojection with motion vectors and depth rejection (`:48-60`). Output is a 2D **scalar** RT.
5. **BlurHCS / BlurVCS**: depth-aware 5-tap separable blur (`ISVolumetricLightingBlurHCS.hlsl:29-70`).
6. **ISCompositeLensFlareVolumetricLighting** (PS): `color += VolumetricLightingColor.rgb * Color::VolumetricLighting(power)` (`ISCompositeLensFlareVolumetricLighting.hlsl:31-42`). An ENB tint path uses `SharedData::enbSettings.VolumetricRays*` (`:35-40`). The result is added on top of the HDR scene, so it is not a fog with transmittance.

-   `ISVolumetricLighting.hlsl:26-33` is a stub PS that outputs the light colour × intensity.

### 2.4 Feature callbacks overridden

`DrawSettings`, `LoadSettings`, `SaveSettings`, `RestoreDefaultSettings`, `DataLoaded` (empty), `PostPostLoad`, `SetupResources`, `EarlyPrepass`, `IsCore()=true` (`VolumetricLighting.h:44-58`). No `Prepass`, `ClearShaderCache` or `HasShaderDefine`. `EarlyPrepass` runs right after shadow maps render (`src/Deferred.cpp:211-234`, hook `Main_RenderShadowMaps` at `src/Deferred.cpp:652-656`). It:

-   updates the dynamic-resolution blur bounds (`VolumetricLighting.cpp:182-204`);
-   detects interior/exterior and interior-with-sun transitions through `InteriorSun::IsInteriorWithSun` (`:206-217`), calling `SetupVL` only on a change.

### 2.5 Shader defines

VL defines none of its own. The replacement IS shaders get the defines of every loaded feature with `HasShaderDefine(ImageSpace)` (`src/ShaderCache.cpp:680-700`), for example `TERRAIN_SHADOWS` (`TerrainShadows.h:16,29`), `CLOUD_SHADOWS` (`CloudShadows.h:24,35`), `VOLUMETRIC_SHADOWS`, `EXP_HEIGHT_FOG`, plus vanilla macros (`VOLUMETRIC_LIGHTING`, `LENS_FLARE`, `EFFECTS11`).

## 3. GPU resources

| Resource                                                                                                                 | Owner         | Created                                            | Freed                                                                                                                             | Notes                                   |
| ------------------------------------------------------------------------------------------------------------------------ | ------------- | -------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------- |
| `vlDataCB` (`VLData`, 4 ints)                                                                                            | VL            | `SetupResources`, `VolumetricLighting.cpp:177-180` | Never (raw `new`)                                                                                                                 | Bound at CS b1 for the blur passes only |
| 4 × `RE::BSImagespaceShader` wrappers                                                                                    | VL            | Lazily, `VolumetricLighting.cpp:259-285`           | Never. On a full-resolution change `blurHCS`/`blurVCS` are set to `nullptr` without release (`:187-190`), so the old objects leak | Holders for shader-cache lookup only    |
| Compiled CS shaders                                                                                                      | `ShaderCache` | `GetComputeShader` at `Hooks.cpp:976`              | Shader-cache lifetime                                                                                                             |                                         |
| VL 3D density texture, 2D VL RT, previous-frame RT, blur RTs                                                             | **Engine**    | Engine, sized by the quality globals               | Engine                                                                                                                            | VL only changes their sizes             |
| Shadow maps t0/t1, `InverseRepartitionTex`, `RepartitionTex`, `NoiseTex` (3D), noise-gradient tex, motion vectors, depth | **Engine**    |                                                    |                                                                                                                                   | Borrowed through vanilla bindings       |

-   **Loading screens.** VL holds no engine pointers across frames. Interior/exterior changes re-run `SetupVL`, so the engine reallocates. There is no LoadingMenu sink.
-   **Resolution changes.** Only the blur wrappers are invalidated (`:187-190`). Dynamic resolution only changes `vlData` (`:198-204`).

## 4. Settings, menu, packaging

-   **Settings struct** (`VolumetricLighting.h:15-23`): `ExteriorEnabled`, `ExteriorQuality` (0..3), `ExteriorCustomSize{W,H,D}`, and the same three for interiors.
    -   Serialized with `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT` (`VolumetricLighting.cpp:11-24`).
    -   Clamped on load to W/H 32–640 and D 10–640 (`:135-146`).
    -   Stored under `o_json[GetName()]`, that is `"Volumetric Lighting"` (`src/Feature.cpp:161,175`).
-   **UI** (`VolumetricLighting.cpp:26-80`): an enable checkbox, a quality slider (Low/Medium/High/Custom) and custom W/H/D sliders in steps of 32/32/10 texels. A change applies immediately only when it targets the location type the player is in (`inLocationType`).
-   **i18n** prefix is `feature.volumetric_lighting.` (`VolumetricLighting.cpp:9`). Keys are in `package/SKSE/Plugins/CommunityShaders/Translations/en.json:1809-1829` (plus es/fr/ru/zh_CN).
-   **ini**: `Version = 1-1-0` with no Alpha/Beta/Unreleased (`features/Volumetric Lighting/Shaders/Features/VolumetricLighting.ini:1-2`).
-   **CORE** marker present (`features/Volumetric Lighting/CORE`), so the feature is bundled in the core package (`CMakeLists.txt:336-344, 1041, 1070`). `IsCore()` returns true (`VolumetricLighting.h:58`).

## 5. What Fluid Field can reuse (R4/R5)

### 5.1 Vanilla VL: limited value

-   **Volume layout.** The volume is engine-allocated, single-channel and screen-aligned, with depth from `InverseRepartitionTex`. Fluid Field cannot own it (rule at `ROADMAP.md:59`), and its size is tied to engine quality globals.
-   **What to copy as a reference:** the in-place Z prefix sum (`ISVolumetricLightingRaymarchCS.hlsl:10-18`) and the depth-aware dynamic-resolution blur (`ISVolumetricLightingBlurHCS.hlsl:38-70`, `VolumetricLighting.cpp:293-305`).
-   **Density injection point (only if needed).** GenerateCS line `:100` (`vl = shadow * density * phase`) is where a Fluid Field density texture could multiply in. That would mean editing a core shader and binding an extra SRV during an engine-owned dispatch (`Hooks.cpp:958-962`). It is not recommended: the output is only "god-ray power", with no extinction, so fog cannot occlude anything.

### 5.2 EHF volumetric fog: the real starting point

Code: `src/Features/ExponentialHeightFog.h:1-167`, `.cpp:233-982`; shaders in `features/Exponential Height Fog/Shaders/ExponentialHeightFog/`.

-   **Volume layout.** Camera-aligned froxels.
    -   Grid XY is `ceil(renderSize / pixelSize)`, with `pixelSize` 4–64 (default 16). Z is 16–160 slices (default 64). The grid is capped at 16M voxels (`ExponentialHeightFog.cpp:299-323`).
    -   A second **far grid** (default 64 px × 32 slices, capped at 4M voxels) continues from `volumetricNearGridDistance` (8000 units) to `volumetricFogDistance` (`:325-332, 693-703`).
    -   Slice distribution is exponential, UE-style `GridZParams` (`:705-717`; shader `VolumetricFogCommon.hlsli:86-133`). Froxel → world position uses the unjittered inverse view-projection (`:760`; `VolumetricFogCSCommon.hlsli:64-74`).
-   **Passes per grid** (`ExponentialHeightFog.cpp:832-895`), all 8×8×4 groups:
    1. ConservativeDepth (`VolumetricFogConservativeDepthCS.hlsl`).
    2. **MaterialSetup**, which writes `VBufferA = (scattering.rgb, extinction)` (`VolumetricFogMaterialCS.hlsl:5-16`).
    3. LightScattering with temporal history and jitter (`VolumetricFogLightScatteringCS.hlsl:388-444`).
    4. Front-to-back Integration to `(inscatter.rgb, transmittance)` (`VolumetricFogIntegrationCS.hlsl:6-44`).
-   **Density input.** **`VolumetricFogMaterialCS.hlsl:12-16`** is the injection point. Today extinction comes from `EvaluateHeightFogExtinction` (two exponential height layers × value noise, `VolumetricFogCommon.hlsli:180-213`). A Fluid Field density layer (a 2D field texture plus a height profile, or a 3D smoke volume in world space) could be sampled there and added to or multiplied with extinction, at `positionWS + CameraPosAdjust`. Lighting, temporal filtering, integration and the compositing into Lighting, Effect, Water, Sky, Grass, DistantTree and ISSAOComposite then come for free (`EXP_HEIGHT_FOG` consumers, for example `package/Shaders/ISSAOComposite.hlsl:133-211`, `Effect.hlsl:822-865`, `Water.hlsl:386-397,995`). This sorting with water and particles is what Phase 4's gate needs (`ROADMAP.md:79`).
-   **Sun and moon.** `SharedData::DirLightDirection/DirLightColor` is the current directional light, whether sun or moon. The phase function is Henyey-Greenstein (`VolumetricFogLightScatteringCS.hlsl:352-364`; `VolumetricFogCommon.hlsli:9-14`).
-   **Shadows.**
    -   Sun CSM with 5-tap PCF, cascade blending and far fade (`VolumetricFogLightScatteringCS.hlsl:137-200`). It uses an engine shadow-map SRV captured from PS t4 at the shadow-mask pass (`ExponentialHeightFog.cpp:290-297`, called from `src/State.cpp:176-185`) and cascade matrices from `Deferred::CopyShadowLightData` t98 (`src/Deferred.cpp:553-582`).
    -   Terrain and Cloud shadows through the defines (`VolumetricFogLightScatteringCS.hlsl:202-215`, `ExponentialHeightFog.cpp:575-610`).
-   **Sky light.** Skylighting SH probes at t50 and IBL at t76-77 (`VolumetricFogLightScatteringCS.hlsl:217-238`, `ExponentialHeightFog.cpp:800-807`).
-   **Local lights.** Light Limit Fix clustered lights at t35-37 (`.cpp:867-874`), LLF local shadow cache at t102-103, and engine local shadow maps at t5 with CB b1 built in `UpdateEngineLocalShadowData` (`.cpp:484-559`; shader `VolumetricFogLightScatteringCS.hlsl:240-342`). Local lights apply to the near grid only (`:369-379`).
-   **Ownership.** Every texture is a `std::unique_ptr<Texture3D/2D>` named for RenderDoc. They are created in `EnsureVolumetricResources` (`.cpp:299-447`), which recreates them on any grid-size change, so resolution and dynamic-resolution changes are handled. They are freed in `ReleaseVolumetricResources` (`.cpp:449-474`) when the fog is disabled (`:642-645`). History is invalidated when a frame is skipped (`:681-691`), which covers loading screens.
    -   Engine SRVs (the shadow map) are held in a `com_ptr` across frames (`.h:151`, `.cpp:294`). That is the "holding engine resources" pattern the roadmap forbids for Fluid Field (`ROADMAP.md:59`).
-   **Pass placement.** `Feature::Prepass` runs from `Deferred::StartDeferred` → `PrepassPasses` (`src/Deferred.cpp:252-299`), after shadow maps. Results are bound to **PS t19/t22** for the whole frame (`ExponentialHeightFog.cpp:476-482`).
-   **Recommendation for R4.** Either (a) add a density hook to EHF's MaterialCS, giving one fog system, or (b) clone the EHF pipeline into Fluid Field with its own registers. Option (a) avoids double fog but couples Fluid Field to EHF, which is `Unreleased = True` (`features/Exponential Height Fog/Shaders/Features/ExponentialHeightFog.ini:1-6`) and has no CORE marker.

### 5.3 Volumetric Shadows

`src/Features/VolumetricShadows.h:1-80`, `.cpp:1-389`. Despite the name, this is a downsampled **VSM** of the sun CSM: 512² R16G16 with 2 mips (one per cascade), separable blur (`.cpp:75-319`). It is bound at **PS t18** (`.h:19`, `.cpp:321-324`) for particles and effects (`features/Volumetric Shadows/Shaders/VolumetricShadows/VolumetricShadows.hlsli:9-57`, used by `package/Shaders/Common/ShadowSampling.hlsli`).

-   Useful for R5 smoke: soft, filterable sun shadowing with a ray-segment VSM sampler `SampleVSMCascade3D` (`VolumetricShadows.hlsli:31-58`).
-   It reads the engine shadow map and the `kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM` depth target (`.cpp:191-194`) and releases its references each frame (`:306-317`), which is the safe pattern.
-   `PostPostLoad` installs a `CreateDepthStencil_VolumetricLighting` thunk that sets the VL shadow map to 1024² (`.cpp:369-384`). See Q3.
-   `ini` `2-1-0`, CORE.

### 5.4 Screen Space GI

`src/Features/ScreenSpaceGI.h/.cpp`. Screen-space 2D only (AO, diffuse and specular IL textures, `ScreenSpaceGI.h:147-160`), drawn inside the deferred composite (`src/Deferred.cpp:336-342`). It has no volume, so it offers nothing for fog density or lighting. One pattern to copy: a `LoadingMenu`-close sink that queues a history reset (`ScreenSpaceGI.h:60-70`, `.cpp:359-384`). Fluid Field should do the same for fog and simulation history.

### 5.5 VR

VR support was removed from Community Shaders (`git log` c9d7c0524 "chore: remove all VR support (#2475)"). `src/` contains no `IsVR` or stereo paths. `FrameBuffer` is single-eye (`src/Globals.h:137-154`). The CMake presets still set `ENABLE_SKYRIM_VR=ON` (`CMakePresets.json:86`). Neither VL nor EHF has a VR path, so R9 (`ROADMAP.md:103`) is greenfield and the compatibility matrix entry for VR (`ROADMAP.md:120`) may be moot.

### 5.6 Risks

-   **Double and triple fog.** Up to three volumetric systems can be active at once: vanilla VL with the CS replacement (additive god rays, `ISCompositeLensFlareVolumetricLighting.hlsl:31-42`), EHF volumetric fog, and Effects11 (ENB-style) volumetric rays (`src/Features/Effects11.cpp:183-189, 816-835`). A separate Fluid Field fog would add a fourth. EHF already has `disableVanillaFog` and `respectVanillaFogFade` (`ExponentialHeightFog.h:66-67`), but nothing suppresses vanilla VL when EHF fog is on. Fluid Field fog will need a stated policy.
-   **ENB.** EHF turns itself off entirely when Effects11/ENB is active (`IsSuppressed`, `ExponentialHeightFog.cpp:106-110`; UI notice `:115-117`). Fluid Field fog built on EHF would vanish under ENB unless it decouples.
-   **Register collisions.** Taken PS/CS slots: t18 (VSM), t19/t22 (EHF), t98 (dir-shadow data), t50, t76-77, t35-37, t102-103, t5. Run `hlslkit-buffer-scan` (`.claude/CLAUDE.md:36-37`).
-   **Sky Sync** scales vanilla VL intensity (`SkySync.cpp:296-297`), so any vanilla-VL coupling inherits its night fade.
-   **Engine-resource lifetime.** EHF keeps the shadow-map SRV in `directionalShadowMap` across frames (`ExponentialHeightFog.cpp:290-297`). Fluid Field should capture and release per frame, as VolumetricShadows does (`VolumetricShadows.cpp:306-317`).

## 6. Open questions

1. Where exactly do the vanilla VL passes (`RelocationID(100309, 107023)`) run relative to `Deferred::StartDeferred` and the EHF `Prepass`? This needs a RenderDoc capture. `FrameAnnotations.cpp:324-335` defines `VolumetricLightingDescriptor_Render` but never installs it.
2. What format and size does the engine VL 3D texture have, and can the GenerateCS bindings take an extra SRV slot without clashing with engine state? (Relevant only if option 5.1 is pursued.)
3. `CreateDepthStencil_VolumetricLighting` (`VolumetricShadows.cpp:369-379`) writes 1024 into `a_properties` but passes the **unmodified copy** `properties` to `func`. Is the VL shadow map really resized? This looks like a latent bug.
4. Do the leaked blur `BSImagespaceShader` objects on resolution change (`VolumetricLighting.cpp:187-190`) matter? `vlDataCB` is also never freed.
5. Should Fluid Field fog extend EHF (one fog, but it depends on an Unreleased, non-CORE feature) or own a parallel froxel pipeline? This is the decision R4 should make.
6. Policy for vanilla VL god rays when Fluid Field fog is on: keep them additive, suppress them through `bEnableVolumetricLighting`, or fold sun shafts into the froxel scatter?
7. How should smoke volumes (R5) feed EHF's MaterialCS: world-space 3D textures sampled per froxel, or splatted into `VBufferA` in a separate pass?
8. Should the ENB-suppression rule for EHF (`ExponentialHeightFog.cpp:106-110`) also apply to Fluid Field fog?
