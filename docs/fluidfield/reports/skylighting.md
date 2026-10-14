# Skylighting: read-only report for Fluid Field

Branch `feature/fluid-field`, Oct 13, 2026. Scope: how Skylighting works, what it owns, and whether its overhead view can serve as Fluid Field's depth and obstacle map (research spike R1). Every claim cites `path:start-end`. Anything not verified in source is marked **(unverified)**.

## TL;DR for R1

-   Skylighting does **not** keep a top-down heightmap. Each frame it re-renders the engine's precipitation occlusion pass into its own depth texture, from a **random, tilted direction** inside a cone of up to `MaxZenith` (default 90 deg), and only **one quarter** of the footprint per frame (`src/Features/Skylighting.cpp:497-513`, `590-617`).
-   The real product is a 256x256x128 grid of sky-visibility SH probes covering 10000 x 10000 x 5000 units around the eye, which is a temporal accumulation and not a depth map (`src/Features/Skylighting.h:96-97`, `features/Skylighting/Shaders/Skylighting/Skylighting.hlsli:23-25`).
-   The single-frame depth texture (`texOcclusion`) can be reached from another feature (`globals::features::skylighting.texOcclusion`, `OcclusionTransform`), but its direction and quadrant change every frame. It is not bound to any shader slot outside Skylighting's own compute dispatch. The useful reuse is the **technique**, not the data: hijack the precipitation occlusion camera, swap the depth target, set a fixed straight-down direction and your own cube size.
-   Recommendation for R1: copy the pattern into Fluid Field with Fluid Field's own depth texture, a fixed `-Z` direction, a smaller cube and no quadrant split. Do not read Skylighting's texture directly.

## 1. Where the code lives

| File                                                                                                                                                                 | Purpose                                                                                                                                           |
| -------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/Features/Skylighting.h:1-177`                                                                                                                                   | Feature class: settings, CB layout, GPU resource pointers, hook structs, LoadingMenu event sink                                                   |
| `src/Features/Skylighting.cpp:1-740`                                                                                                                                 | Settings I/O, UI, resource creation, CB fill, probe dispatch, occlusion render, hooks, alpha-pool patch                                           |
| `features/Skylighting/Shaders/Skylighting/UpdateProbesCS.hlsl:1-165`                                                                                                 | Compute shader: integrates the occlusion depth into SH probes and accumulates a directional-shadow bitmask per probe                              |
| `features/Skylighting/Shaders/Skylighting/Skylighting.hlsli:1-222`                                                                                                   | Shared include: probe SRV declaration (t50 or override), grid constants, `Sample`, `SampleNoBias`, `EvaluateDiffuse/Specular`, `ApplySkylighting` |
| `features/Skylighting/Shaders/Features/Skylighting.ini:1-7`                                                                                                          | Version `1-5-4`, Nexus id 139352                                                                                                                  |
| `package/Shaders/Common/SharedData.hlsli:138-150`, `455-463`                                                                                                         | `SkylightingSettings` struct inside the `FeatureData` cbuffer at b6                                                                               |
| `src/FeatureBuffer.cpp:45-54`                                                                                                                                        | Packs `Skylighting::GetCommonBufferData(a_inWorld)` into the shared feature CB                                                                    |
| `src/Deferred.cpp:336`, `369`, `606-607`                                                                                                                             | Binds the probe SRV to the deferred composite at t8 and adds `SKYLIGHTING` to its defines                                                         |
| `package/Shaders/DeferredCompositeCS.hlsl:36-38`, `137-141`, `202-264`                                                                                               | Deferred consumer (diffuse and specular occlusion)                                                                                                |
| `package/Shaders/Lighting.hlsl:788-792`, `907-918`, `2102-2146`, `2346-2348`, `2688-2702`, `2841-2859`                                                               | Forward consumer, plus `SKYLIGHTING_SHADOW_VIS` for soft, rim and back lighting, and wetness occlusion                                            |
| `package/Shaders/RunGrass.hlsl:360-362`, `package/Shaders/Effect.hlsl:461-534`                                                                                       | Grass and effect consumers                                                                                                                        |
| `features/Dynamic Cubemaps/Shaders/DynamicCubemaps/DynamicCubemaps.hlsli:6-114`                                                                                      | Specular occlusion consumer                                                                                                                       |
| `src/Features/ExponentialHeightFog.cpp:673-806`, `features/Exponential Height Fog/Shaders/ExponentialHeightFog/VolumetricFogLightScatteringCS.hlsl:28-29`, `223-237` | Volumetric fog reads the probes at CS t50 (an example of cross-feature reuse)                                                                     |
| `src/State.cpp:130`, `183-184`                                                                                                                                       | Calls `CaptureShadowCascadeSRV()` when the RenderShadowmask utility shader is set                                                                 |
| `src/Globals.h:21`, `110`; `src/Globals.cpp:77`                                                                                                                      | `globals::features::skylighting` instance                                                                                                         |
| `src/Feature.cpp:242`                                                                                                                                                | Position in the feature list, which sets the Prepass order                                                                                        |

No `CORE` marker exists in `features/Skylighting/` (only `Shaders/`), so it ships as a separate package (see section 4).

## 2. How it hooks the engine

### Hooks installed in `PostPostLoad` (`src/Features/Skylighting.cpp:310-322`)

| Hook                                                                             | Target                                                                           | Purpose                                                                                                                                                                                    |
| -------------------------------------------------------------------------------- | -------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `write_vfunc<0x2D>` on `RE::VTABLE_BSLightingShaderProperty[0]`                  | `BSLightingShaderProperty::GetPrecipitationOcclusionMapRenderPasses`             | Rebuilds the occlusion pass list per geometry, deciding which geometry is drawn into the map (`.cpp:313`, `401-495`)                                                                       |
| `write_thunk_call` at `RelocationID(35560, 36559) + Select(0x3A1, 0x3A1, 0x3BF)` | Call in Main draw (35560/36559) that renders the precipitation occlusion         | Replaced by `Skylighting::RenderOcclusion()` (`.cpp:314`, `670-673`)                                                                                                                       |
| `write_thunk_call` at `RelocationID(25643, 26185) + Relocate(0x5D9, 0x59D)`      | Call inside `Precipitation::ComputeProjection` (25643/26185) to `SetViewFrustum` | While `inOcclusion`, shrinks the ortho frustum to one quadrant chosen by `frameCount % 4` (`.cpp:316`, `497-513`)                                                                          |
| `write_vfunc<0x28>` on `RE::VTABLE_BSShaderAccumulator[0]` plus code patch       | `BSShaderAccumulator::StartGroupingAlphas`                                       | Doubles the engine's alpha geometry-group pool from 512 to 1024 by rewriting 5 RIP-relative `lea` sites, and returns null instead of overflowing (`.cpp:318-319`, `675-728`; `.h:140-152`) |
| `MenuOpenCloseEventHandler::Register()`                                          | `UI` `MenuOpenCloseEvent`                                                        | When LoadingMenu closes, sets `queuedResetSkylighting` (`.cpp:321`, `730-739`; `.h:155-176`)                                                                                               |

Note: **Upscaling hooks the same call site** (`src/Features/Upscaling.cpp:589-590`, thunk `1734-1740`, which sets `dynamicResolutionLock` around it). Both use `write_thunk_call` at the same address, so they chain. The one installed last runs first **(order unverified)**. A Fluid Field capture hook on this call would be a third link in the chain.

Main draw call sites on 35560/36559, for ordering: shadow maps at `0x2EC` (`src/Deferred.h:159`), depth prepass at `0x395` (`src/Features/TerrainBlending.h:127`), precipitation occlusion at `0x3A1`, then RenderWorld at `0x831` (`src/Deferred.h:161`). So per frame: shadow maps, depth, **occlusion capture**, then world. `StartDeferred()` then runs inside RenderWorld (`src/Deferred.cpp:669-673`) and calls `PrepassPasses()` (`src/Deferred.cpp:236-249`, `296`), which is where the probe update dispatches.

### Scene from above: `RenderOcclusion()` (`src/Features/Skylighting.cpp:515-658`)

1. If the shader cache is disabled, it calls the vanilla function and returns (`523-529`). It does nothing in interiors (`531-532`).
2. It renders the **vanilla precipitation mask** first, using the current or last precip object and `precip->SetupMask()` / `RenderMask(rain)` (`535-560`).
3. It runs any queued reset (`566-567`) and increments `frameCount` (`569`).
4. **Swaps the engine depth target.** It copies `DepthStencilData` for `kPRECIPITATION_OCCLUSION_MAP`, then points `depthSRV`, `texture` and `views[0]` at `texOcclusion` (`571-576`). It restores them at `647`.
5. It overrides engine globals: `PrecipitationShaderCubeSize` (`RelocationID(515451, 401590)`) is set to `occlusionDistance = 10000` (`578-585`; `.h:97`). It also sets `precip->lastCubeSize` (`587-588`) and `PrecipitationShaderDirection` (`RelocationID(515509, 401648)`, `581-582`, `617`).
6. Direction: an R2 low-discrepancy point mapped to a disc of radius `sin(MaxZenith)`, giving a downward vector `-(x, y, sqrt(1-r^2))` (`590-617`). With the default `MaxZenith = pi/2` (`.h:54`), the view can be almost horizontal.
7. It calls `Precipitation::ComputeProjection` (`RelocationID(25643, 26185)`, `619-624`). The `SetViewFrustum` hook clips the frustum to one quadrant during this call (`497-513`).
8. It renders using a heap-allocated fake `BSParticleShaderRainEmitter` (4064-byte blob, `326-330`) through `precip->RenderMask` (`626-632`). It reads `occlusionProjection` from the blob into `OcclusionTransform` (`638`), then deletes it (`640`).
9. It computes the SH basis of the direction once per capture (`635-637`), then restores the cube size, direction and depth target and recomputes the vanilla projection (`642-652`).

The camera is the engine's own **orthographic precipitation occlusion camera** (`precip->occlusionData.camera`, `619-622`), centred by the engine **(centering rule unverified; presumably on the camera or eye position)**.

### Geometry filter in the pass-list hook (`.cpp:401-495`)

-   It always clears `geometryGroups[14]->flags & 1` (`409-410`; purpose undocumented).
-   Skinned geometry is skipped. During the Skylighting pass, skinned geometry with `kTreeAnim` is kept (`418-424`), so **actors are excluded**.
-   An occluder must have `kZBufferWrite`, must not be refraction, temp refraction, `kLODLandscape`, eye reflect, decal or dynamic decal, and must have `worldBound.radius > 32` (`426-431`).
-   **Terrain is included only in Skylighting's pass.** `kMultiTextureLandscape` and `kNoLODLandBlend` are excluded from the vanilla precip pass but allowed while `inOcclusion` (`429`).
-   Objects whose bound top is below `probeGridBottomZ - 512` are culled (`433-436`; `.h:99`, `105`; `.cpp:221`).
-   Objects under a `BSFadeNode` with BSX flags Ragdoll, Dynamic, Addon, NeedsTransformUpdate, MagicShaderParticles, Lights, Breakable or SearchedBreakable are culled (`438-465`). So **loose physics clutter is excluded**.
-   The technique is `BSUtilityShader` `RenderDepth`, plus `Vc`, `Texture|AlphaTest` (alpha-tested foliage), `LodObject` and `TreeAnim` as applicable (`468-493`; enum `332-397`).
-   Only `BSLightingShaderProperty` is hooked. Water (`BSWaterShaderProperty`), effect and grass properties go through their own vanilla vfuncs **(whether those emit occlusion passes is unverified)**.

### Feature callbacks overridden (`src/Feature.h` vs `src/Features/Skylighting.h`)

`GetName`, `GetDisplayName`, `GetShortName`, `GetFeatureModLink`, `GetShaderDefineName` (`"SKYLIGHTING"`), `GetCategory` (`kLighting`), `GetFeatureSummary` (`.h:11-26`); `HasShaderDefine` returns true for every type (`.h:28`); `RestoreDefaultSettings`, `DrawSettings`, `LoadSettings`, `SaveSettings` (`.h:30-35`); `SetupResources`, `ClearShaderCache` (`.h:38-40`); `Prepass` (`.h:45`); `PostPostLoad` (`.h:48`). It does **not** override `Reset`, `EarlyPrepass`, `ReflectionsPrepass`, `DataLoaded` or `GameLoaded` (`src/Feature.h:168-209`).

### Defines

-   `SKYLIGHTING` goes into every BSShader type via `HasShaderDefine` and `ShaderCache` (`src/ShaderCache.cpp:159-160`, `177-178`, `201-202`, `277-278`), and into DeferredComposite (`src/Deferred.cpp:606-607`).
-   Derived defines in shaders: `SKYLIGHTING_SHADOW_VIS` (`package/Shaders/Lighting.hlsl:791-792`, `package/Shaders/RunGrass.hlsl:360-362`) and `SKYLIGHTING_PROBE_REGISTER` (DeferredComposite t8 at `package/Shaders/DeferredCompositeCS.hlsl:37`, fog t50 at `VolumetricFogLightScatteringCS.hlsl:28`). `WORLD_MAP` undefines `SKYLIGHTING` (`Lighting.hlsl:787-789`).
-   The compute shader is compiled with no defines (`.cpp:188-191`).

## 3. GPU resources

### Owned (created in `SetupResources`, `src/Features/Skylighting.cpp:79-165`)

| Resource                      | Format and size                                                                                                                                                         | Views    | Created        | Use                                                              |
| ----------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------- | -------------- | ---------------------------------------------------------------- |
| `texOcclusion` (`Texture2D`)  | Desc **copied from the engine's `kPRECIPITATION_OCCLUSION_MAP`** texture, SRV and DSV (`85-97`); size and format are therefore the engine's **(resolution unverified)** | SRV, DSV | `.cpp:95-97`   | Depth target of the occlusion capture; CS t0 in the probe update |
| `texProbeArray` (`Texture3D`) | 256x256x128 R16G16B16A16_FLOAT (`.h:96`; `.cpp:101-130`), about 64 MiB                                                                                                  | SRV, UAV | `.cpp:128-130` | SH2 sky visibility probes; PS t50, deferred t8, fog CS t50       |
| `texAccumFramesArray`         | 256x256x128 R8_UINT, about 8 MiB                                                                                                                                        | SRV, UAV | `.cpp:132-136` | Per-probe accumulation count                                     |
| `texShadowBitmask`            | 256x256x128 R32_UINT, about 32 MiB                                                                                                                                      | SRV, UAV | `.cpp:138-142` | 32-frame directional shadow history                              |
| `texShadowVisibility`         | 256x256x128 R8_UNORM, about 8 MiB                                                                                                                                       | SRV, UAV | `.cpp:144-148` | Shadow visibility; PS t53                                        |
| `comparisonSampler`           | COMPARISON_MIN_MAG_MIP_LINEAR, clamp, LESS_EQUAL                                                                                                                        | none     | `.cpp:151-161` | CS s0                                                            |
| `probeUpdateCompute`          | cs_5_0 from `Data\Shaders\Skylighting\UpdateProbesCS.hlsl`                                                                                                              | none     | `.cpp:179-198` | Probe update; recompiled on `ClearShaderCache` (`167-177`)       |

The CB has no buffer of its own. `SkylightingCB` (`.h:59-74`) goes into the shared `FeatureData` cbuffer b6 (`src/FeatureBuffer.cpp:45-54`; `SharedData.hlsli:455-463`). It is zeroed when `!a_inWorld` or the map menu is open (`.cpp:200-206`).

**Freeing.** The textures are raw `new` pointers (`.h:85-89`) and are never deleted. There is no `Reset` override and no destructor. They live for the whole process. `Texture2D` and `Texture3D` are RAII wrappers (`src/Buffer.h:310-330`, `378`), but nothing destroys them.

### Engine-owned resources it borrows or holds

| Resource                                                                                             | How                                                                                                                                                                                           | Lifetime risk                                                                                    |
| ---------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| Precipitation occlusion `DepthStencilData`                                                           | Desc read at setup (`.cpp:85-93`). Temporarily **replaced** with Skylighting's texture during the capture, then restored (`571-576`, `647`)                                                   | Short; restored the same frame                                                                   |
| Shadow cascade SRV (PS t4 at shadowmask time)                                                        | `PSGetShaderResources(4, ...)` AddRefs it, and it is **held across frames** in `shadowCascadeSRV`, released only on the next capture (`.cpp:660-668`; triggered from `src/State.cpp:180-184`) | Holds a ref on an engine resource indefinitely, which is exactly the pattern the roadmap forbids |
| `kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM` depth SRV                                                    | Read per frame in Prepass, not held (`.cpp:257-264`)                                                                                                                                          | Low                                                                                              |
| `globals::deferred->directionalShadowLights` SRV                                                     | Read per frame (`.cpp:263`)                                                                                                                                                                   | Owned by Deferred                                                                                |
| Engine globals `PrecipitationShaderCubeSize`, `PrecipitationShaderDirection`, `precip->lastCubeSize` | Overwritten and restored around the capture (`578-588`, `642-645`)                                                                                                                            | Restored the same frame                                                                          |
| Alpha group pool                                                                                     | Permanently relocated to a trampoline allocation (`675-715`)                                                                                                                                  | Process lifetime                                                                                 |

### Loading screens and cell changes

-   On LoadingMenu close, `queuedResetSkylighting = true` (`.cpp:730-739`). The reset itself runs on the render thread at the next capture (`566-567`). It clears all four UAVs to unit SH, 0, all bits lit and 1.0 (`38-58`). Initial value: `true` (`.h:102`).
-   Scrolling within a worldspace is handled by toroidal addressing (section 5), so no reset is needed when moving.
-   Interiors: `Prepass` returns early when `Sky::mode != kFull` (`.cpp:244-250`). `RenderOcclusion` skips when `Util::IsInterior()` (`532`). Shaders return unit SH when `SharedData::InInterior` (`Skylighting.hlsli:103-104`).
-   Map menu: Prepass skipped and CB zeroed (`.cpp:205-206`, `241-242`).
-   The "Rebuild Skylighting" button calls `ResetSkylighting()` directly from the UI thread context (`.cpp:68-69`). The other path defers the reset to the render thread.

## 4. Settings, menu and packaging

-   `Settings { MaxZenith = pi/2; MinDiffuseVisibility = 0.1; MinSpecularVisibility = 0.1; }` (`src/Features/Skylighting.h:52-57`).
-   Serialization: `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT` (`src/Features/Skylighting.cpp:15-19`). Load clamps `MaxZenith` to [0, pi/2] and resets it if non-finite (`21-26`). Save is at `28-31`, defaults at `33-36`.
-   `DrawSettings` (`60-77`): two visibility sliders, a Rebuild button with tooltip, and a Max Zenith `SliderAngle` (0-90 deg).
-   i18n prefix `feature.skylighting.` (`.cpp:13`). Keys: `name`, `description`, `key_feature_1..5` (`.h:12-25`), `min_visibility_desc`, `diffuse_min_visibility`, `specular_min_visibility`, `rebuild`, `rebuild_tooltip`, `max_zenith`, `max_zenith_tooltip` (`.cpp:62-76`). English strings are at `package/SKSE/Plugins/CommunityShaders/Translations/en.json:1608-1621`, with ru and zh_CN alongside.
-   `.ini`: `Version = 1-5-4`, no Alpha, Beta or Unreleased flags, Nexus `139352`, `autoupload = true` (`features/Skylighting/Shaders/Features/Skylighting.ini:1-7`). The minimum version is checked through generated `FeatureVersions::FEATURE_MINIMAL_VERSIONS` (`cmake/FeatureVersions.h.in:9`; `src/Feature.cpp:422-437`).
-   There is no CORE marker, so CMake treats it as a separate feature. The core detection (`CMakeLists.txt:336-344`) skips it, and it gets its own `Package-Skylighting` zip target (`CMakeLists.txt:1197-1221`).
-   Override JSON support: `MyMod_Skylighting.json` (`package/SKSE/Plugins/CommunityShaders/Overrides/README.md:25`, `105`).

## 5. What Fluid Field could reuse (R1)

### What the overhead view actually is

| Property           | Value                                                                                                                                                                                                      | Source                                  |
| ------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------- |
| Projection         | Engine precipitation ortho camera, cube size forced to 10000 units                                                                                                                                         | `.cpp:578-588`, `619-623`               |
| Direction          | Random each frame within a cone of `MaxZenith` (default 90 deg, so up to grazing); R2 sequence, reseeded every 1000 frames                                                                                 | `.cpp:590-617`; `.h:54`                 |
| Coverage per frame | One of 4 frustum quadrants (`frameCount % 4`); the full cube footprint takes 4 frames                                                                                                                      | `.cpp:497-513`, `569`                   |
| Resolution         | Same as the engine precip occlusion map; one quadrant mapped onto the whole texture, so roughly 2x linear density per frame **(engine size and the fTop = half-size assumption unverified)**               | `.cpp:85-97`, `504-509`                 |
| Cadence            | Every frame in exteriors when the shader cache is enabled                                                                                                                                                  | `.cpp:523-532`                          |
| Contents           | Static lighting-shader geometry with radius > 32: terrain (in this pass), statics, trees, alpha-tested foliage, LOD objects                                                                                | `.cpp:418-493`                          |
| Excludes           | Actors (skinned), dynamic/ragdoll/breakable/addon clutter, decals, refraction, LOD landscape, small objects (r <= 32), anything below the probe grid bottom minus 512, and probably water (not BSLighting) | `.cpp:418-465`                          |
| Value stored       | Ortho depth along the jittered direction (unorm depth, compared with `SampleCmpLevelZero`)                                                                                                                 | `UpdateProbesCS.hlsl:5`, `71-77`        |
| Matrix             | `OcclusionTransform` (`rain->occlusionProjection`), applied to camera-relative ("MS") positions, with Y flipped and `uv = xy*0.5+0.5`                                                                      | `.cpp:638`; `UpdateProbesCS.hlsl:71-73` |

### The probe grid (scrolling and centering reference)

-   Extent is 10000 x 10000 x 5000 units, at 256 x 256 x 128 cells of about 39 units (`Skylighting.hlsli:23-25`; `.h:96-97`).
-   Centering: cell size is `occlusionDistance / dims`, with z using half the distance (`.cpp:213-217`). The eye position is snapped to `round(eye / cellSize)` (`218-220`). `PosOffset = cellOrigin - eyePos` (`228`).
-   Scrolling: toroidal. `ArrayOrigin = (cellID - dims/2) % dims` (`229-232`), and the shader reads `(cellID + ArrayOrigin) % ARRAY_DIM` (`Skylighting.hlsli:139`, `212`).
-   `ValidMargin = prevCellID - cellID` (`.cpp:222-223`, `233`). The compute shader treats cells outside the valid range as new and reseeds them (`UpdateProbesCS.hlsl:64-67`, `91-94`). The grid fades out at the edges (`Skylighting.hlsli:27-33`).
-   This snapped, toroidal, margin-invalidation scheme is directly reusable as the addressing model for Fluid Field's player-centred 2D grid.

### Can it be Fluid Field's depth and obstacle map? Not as data

-   Wrong view. The direction is randomised and can be near-horizontal, so the map is a sky-occlusion sample and not a height field. Forcing `MaxZenith = 0` would change Skylighting's look, and that is a user setting.
-   Partial per frame. Only one quadrant is valid at a time.
-   Transient. `texOcclusion` is overwritten every frame. It is only bound at CS t0 inside Skylighting's Prepass (`.cpp:260-283`) and is not exposed at a PS/CS slot for others.
-   Wrong range. 10000 units is far larger than a likely fluid grid, and the vertical cull is tied to the probe grid (`.cpp:435`).
-   Wrong contents for water. Water surfaces are probably absent, which is good for a riverbed. But culling small objects (r <= 32) and dynamic clutter drops small rocks and props that a shallow-water obstacle map would want.
-   Dependency. Skylighting may be disabled or absent; it is a non-CORE package.

### What to reuse

1. **The capture technique.** Run a second capture through the same engine path with Fluid Field's own `Texture2D` depth target: swap `DepthStencilData`, override cube size and direction (straight down, `(0,0,-1)`), call `ComputeProjection` and `RenderMask` with a scratch emitter, then restore everything (`.cpp:571-652`). A third thunk on `0x3A1` (or calling Fluid Field's capture from inside the existing chain) is the likely integration point.
2. **The pass-list filter idea.** It decides what is drawn (`.cpp:401-495`). Fluid Field would need its own flag (as `inOcclusion` does) to admit terrain, choose the radius threshold and decide on dynamic clutter. Today the vfunc hook belongs to Skylighting; two features cannot both `write_vfunc` slot 0x2D cleanly. A shared helper or a mode flag would need maintainer agreement.
3. **The toroidal snapped grid addressing** (`.cpp:208-236`; `UpdateProbesCS.hlsl:64-69`).
4. **The reset-on-LoadingMenu-close, applied on the render thread** pattern (`.cpp:566-567`, `730-739`), already copied by `SceneSettingsManager` (`src/SceneSettingsManager.cpp:294`, `src/SceneSettingsManager.h:47`).
5. **Sky visibility for sheltering.** Fluid Field fog and wind could read `Skylighting::SampleNoBias` the same way volumetric fog does. Bind `globals::features::skylighting.texProbeArray->srv` at a CS slot and include `Skylighting.hlsli` with `SKYLIGHTING_PROBE_REGISTER` (`src/Features/ExponentialHeightFog.cpp:673-679`, `801-806`; `VolumetricFogLightScatteringCS.hlsl:28-29`, `223-226`). Always guard on `skylighting.loaded && texProbeArray`. The CB data is already in b6 (`SharedData.hlsli:463`).

### How another feature reads Skylighting today

-   `globals::features::skylighting` is the instance (`src/Globals.h:110`). All members are public.
-   Slots: PS t50 probes and PS t53 shadow visibility, set in Skylighting Prepass (`.cpp:300-307`); deferred t8 (`src/Deferred.cpp:369`); fog CS t50 (`src/Features/ExponentialHeightFog.cpp:806`). `texOcclusion` is CS t0 only, inside Skylighting's dispatch.
-   Order: Prepass runs in feature-list order (`src/Feature.h:345-364`; Skylighting at `src/Feature.cpp:242`). The occlusion capture happens before any Prepass in the same frame (Main `0x3A1` comes before `0x831`), so a later feature's Prepass would see this frame's `texOcclusion` and `OcclusionTransform`.

## 6. Open questions

1. What is the resolution and format of the engine's `kPRECIPITATION_OCCLUSION_MAP`? It sets Skylighting's texel size. Check in RenderDoc or the renderer setup.
2. What exactly is `frustum.fTop` after `ComputeProjection` with cube size 10000? Is it half the cube, and is the camera centred on the eye or the player?
3. Do `BSWaterShaderProperty`, grass and effect properties emit occlusion passes? This decides whether a straight-down capture sees the water surface or the riverbed.
4. Which of the Upscaling and Skylighting thunks at `35560+0x3A1` runs first, and where would a Fluid Field thunk sit in that chain?
5. What does clearing `geometryGroups[14]->flags & 1` do (`.cpp:410`)? Does it matter for a second capture in the same frame?
6. Is it safe to call `RenderMask` twice more per frame (precip, Skylighting, Fluid Field)? What are the CPU cost and the effect on the alpha group pool that Skylighting had to grow (`.cpp:675-728`)?
7. `shadowCascadeSRV` holds an engine SRV across frames (`.cpp:660-668`). Is this implicated in loading-screen crashes, and should Fluid Field avoid any similar capture?
8. Skylighting never frees its textures (about 112 MiB of 3D textures). This is fine today, but it is worth knowing for VRAM budgets (`src/State.cpp:849-881` already warns about this format load).
9. Would the maintainers accept a shared "top-down capture" service (one hook, several consumers) instead of each feature hijacking the precipitation camera?
