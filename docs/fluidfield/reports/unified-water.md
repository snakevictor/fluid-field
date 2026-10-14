# Report: Unified Water and Water Effects

Phase 0 read-only report for Fluid Field. Branch `feature/fluid-field`, read on 2026-10-13. Every claim cites `path:start-end`. "UW" means Unified Water and "WE" means Water Effects.

## TL;DR

- **UW is mostly a CPU and scene-graph feature.** It replaces distant LOD water with cloned LOD0 water tiles. It builds one world-wide Tamriel flowmap and installs it in the engine's flowmap global. It does not own a single D3D resource after startup, and it has no Settings struct.
- **WE is very small.** It loads one caustics DDS and binds it to PS slot t65. It also gates `WaterParallax.hlsli` in `Water.hlsl` and `WaterCaustics.hlsli` in `Lighting.hlsl` behind `WATER_EFFECTS`.
- **The ripple, flow and normal code lives in shared shaders, not in either feature.** Look in `package/Shaders/Water.hlsl` (`GetWaterNormal`, the flowmap helpers) and in Wetness Effects (`GetRainDrops`, `GetFlowAwareRippleOffset`).
- **Fluid Field's injection point is `GetWaterNormal()`** (`package/Shaders/Water.hlsl:628-801`). The best spot is between the WADING displacement block and the Wetness ripple block.
- **Main risks:**
  - The water mesh is a tiny NIF, so height can only be shown in the pixel shader (normals and parallax), not by moving vertices.
  - UW permanently NOPs the vanilla dynamic flowmap writes.
  - Water Parallax offsets UVs before the normal blend.
  - Several other hooks are chained on the same water functions.

## 1. Where the code lives

| File | Purpose |
| --- | --- |
| `src/Features/UnifiedWater.h:1-140` | `UnifiedWater : OverlayFeature`: hook structs, overrides, engine-global pointers, worldspace and map-menu atomics |
| `src/Features/UnifiedWater.cpp:1-846` | Hook install, the hook thunks, DataLoaded bootstrap, debug UI, cache-progress overlay, load-order hash |
| `src/Features/UnifiedWater/Flowmap.h:1-51` / `Flowmap.cpp:1-357` | Stitches vanilla `flow.X.Y.dds` per-cell textures into one `Tamriel-Flowmap.W.H.ox.oy.dds`, then loads it as an `NiSourceTexture` |
| `src/Features/UnifiedWater/WaterCache.h:1-226` / `WaterCache.cpp:1-933` | Builds, per worldspace and on a thread pool, the water-tile placement "instructions" for every LOD level; disk cache `.wc`, Tamriel precache `.wpc` |
| `src/Features/WaterEffects.h:1-37` / `WaterEffects.cpp:1-23` | Caustics texture load and t65 bind; define `WATER_EFFECTS` |
| `src/Features/OverlayFeature.h:12-36` | Base class for UW's overlay |
| `features/Unified Water/Shaders/Features/UnifiedWater.ini:1-2` | `[Info] Version = 1-0-0` |
| `features/Unified Water/CORE` | Empty marker that makes UW a core feature |
| `features/Unified Water/meshes/water/WaterMesh.nif` | 783-byte tile mesh that is cloned for every tile (very low vertex count) |
| `features/Unified Water/SKSE/Plugins/CommunityShaders/UnifiedWaterCache/Tamriel_precache.wpc` | Offline height precache (sheson extended Tamriel data, `WaterCache.cpp:122-147`) |
| `features/Water Effects/Shaders/Features/WaterEffects.ini:1` | `[Info] Version = 1-3-0` |
| `features/Water Effects/CORE` | Core marker |
| `features/Water Effects/Shaders/WaterEffects/WaterParallax.hlsli:1-234` | POM on Normals01-03 alpha and on flowmap-normal alpha |
| `features/Water Effects/Shaders/WaterEffects/WaterCaustics.hlsli:1-64` | Caustics for `Lighting.hlsl` (t65) |
| `features/Water Effects/Shaders/WaterEffects/watercaustics.dds` | Caustics texture (43 KB) |
| `package/Shaders/Water.hlsl:1-1388` | The full water VS/PS: all techniques, flowmap helpers, normal blend, SSR, fog, `UNIFIED_WATER` branches |
| `package/Shaders/ISWaterFlow.hlsl:1-44` | Vanilla imagespace pass that writes actor/flow impulses into the flowmap (UW disables its callers, see 2.1) |
| `package/Shaders/ISWaterDisplacement.hlsl:1-162` | Vanilla wading and rain heightmap sim: `HEIGHTMAP_*` passes, displacement normals |
| `package/Shaders/ISWaterBlend.hlsl:1-88` | Water temporal blend; history clear hook at `src/Hooks.cpp:312-330,1091` |
| `package/Shaders/ISReflectionsRayTracing.hlsl:34-36,65-68,81,168` | SSR: `#ifndef UNIFIED_WATER` removes the max-depth cutoff |
| `package/Shaders/Lighting.hlsl:778-784,797-799,2127,2250-2253` | WE caustics on lit surfaces under water |
| `package/Shaders/Common/SharedData.hlsli:11,511-530` | `WaterData[25]`: 5x5 cells around the camera (colour, height) via `GetWaterData` |
| `src/State.cpp:1123-1130`, `src/Utils/Game.cpp:118-160` | CPU fill of `WaterData[25]` each frame |
| `features/Wetness Effects/Shaders/WetnessEffects/WetnessEffects.hlsli:28-123,227-244` | `GetRainDrops`, `GetFlowAwareRippleOffset`, both used by `Water.hlsl` |
| `package/Shaders/Common/Shading.hlsli:34-52` | `ReorientNormal` (RNM normal blending) |

## 2. How it hooks the engine

### 2.1 Unified Water hooks (all installed in `PostPostLoad`, `UnifiedWater.cpp:445-471`)

| Hook | Kind / address | Thunk | What it does |
| --- | --- | --- | --- |
| `TES_SetWorldSpace` | detour `RelocationID(13170,13315)` `:447` | `:537-546` | Sets `exteriorWorldspaceActive`, switches the WaterCache worldspace, updates the LOD cull |
| `TES_DestroySkyCell` | detour `(20029,20463)` `:448` | `:548-557` | Clears the worldspace and culls the LOD root |
| `TESWaterSystem_InitializeWater_SetWaterShaderMaterialParams` | call thunk `(31388,32179)+(0x360,0x3BC)` `:450` | `:482-503` | Hashes the 4 noise texture names into a thread_local side channel |
| `BSWaterShaderMaterial_ComputeCRC32` | vfunc 0x4 on `VTABLE_BSWaterShaderMaterial[0]` `:451` | `:505-513` | Folds that hash into the material CRC so forms with different normals do not merge (`:475-480`) |
| `BGSTerrainBlock_Attach` | detour `(30934,31737)` `:453` | `:615-757` | Core of UW: detaches the block's water node, clones `waterMesh` per cache instruction, calls `TESWaterSystem::AddWater` and then pops the entry back out of `waterObjects`, sets water flags, parents the tiles under the global `gWaterLOD` |
| `BGSTerrainBlock_Detach` | detour `(30936,31739)` `:455` | `:759-795` | Hides UW water from the vanilla detach, drops the cloned tiles, parks the node back on the chunk |
| `BGSTerrainNode_UpdateWaterMeshSubVisibility` | detour `VersionedRelocation::ResolveID(31059,31846,523588)` `:457` | `:559-613` | Culls LOD-4 tiles over attached cells that have water. On 1.7.99+ it takes the terrain-cell lock (`:595-603`) |
| `TESWaterSystem_UpdateDisplacementMeshPosition` | detour `(31384,32175)` `:459` | `:826-845` | Rewrites the engine global `CellTexCoordOffset` for the displacement (wading) mesh to index the world flowmap |
| `BSWaterShader_SetupGeometry` | vfunc 0x6 on `VTABLE_BSWaterShader[0]` `:461` | `:797-824` | Per draw, writes engine globals that become VS `ObjectUV` (flowmap W, H, 1-scale), plus `flowX/flowY/cellX/cellY` on the property, which become `CellTexCoordOffset` |

UW also patches engine code in `DisableVanillaWaterLOD()` (`UnifiedWater.cpp:79-104`). This runs once from `DataLoaded`, and it is **irreversible**:

- It turns two branches into unconditional jumps, so the engine skips its own attached-mesh and `AddLODWater` loops (`:87-97`).
- It NOPs three 5-byte calls in `Main::RenderWaterEffects` `(35561,36560)+0x1B7/0x1EA/0x202`, which are the vanilla flowmap-write passes (`:99-102`). That means the dynamic flow perturbation from `ISWaterFlow.hlsl` is gone whenever UW is active.

UW also reads these engine globals (`UnifiedWater.cpp:463-468`):

| Global | Address |
| --- | --- |
| `gWaterLOD` | `516171/402322` |
| `gFlowMapSize` | `527644/414596` |
| `gFlowMapSourceTex` | `527694/414616` |
| `gDisplacementCellTexCoordOffset` | `528184/415129` |
| `gDisplacementMeshPos` | `516235/402400` |
| `gDisplacementMeshFlowCellOffset` | `528164/415109` |

Other hooks on the same functions, so they are chained:

- `Main_RenderWaterEffects (35561)` is also detoured by `src/EngineFixes/WaterReflectionCubemapFix.cpp:5-8,40-44` and `src/FrameAnnotations.cpp:248-259,944`.
- BSWaterShader vfunc 0x6 and 0x7 are also hooked by `src/FrameAnnotations.cpp:348-349,367-368`.
- `ISWaterBlend` Render is hooked at `src/Hooks.cpp:1091`.

Event sink: `MenuOpenCloseEventHandler` (`UnifiedWater.cpp:264-308`). It re-culls when LoadingMenu closes and tracks MapMenu (`:271-278`). It is registered once from `DataLoaded` (`:259-261`).

### 2.2 Shader defines and permutations

- **Define injection.**
  - UW returns `"UNIFIED_WATER"` with `HasShaderDefine` = true for **every** shader type (`UnifiedWater.h:15,27`).
  - WE returns `"WATER_EFFECTS"` for every type (`WaterEffects.h:14`, `WaterEffects.cpp:20-23`).
  - The water path appends them in `GetWaterShaderDefines` (`src/ShaderCache.cpp:450-512`, loop `:506-510`). That function also maps the descriptor bits `VC, NORMAL_TEXCOORD, REFLECTIONS, REFRACTIONS, DEPTH, INTERIOR, WADING, VERTEX_ALPHA_DEPTH, CUBEMAP, FLOWMAP, BLEND_NORMALS` and the techniques `UNDERWATER/LOD/STENCIL/SIMPLE/SPECULAR+NUM_SPECULAR_LIGHTS` (`:456-503`; enums in `src/ShaderCache.h:689-711`).
- **Permutation reduction.** `State::ModifyShaderLookup` strips `Reflections|Cubemap|Interior` from the water descriptors (`src/State.cpp:1007-1014`).
- **CB layout map.** The water VS and PS constant-name-to-index maps are at `src/ShaderCache.cpp:1008-1063`. These are the engine's own CBs; neither feature adds a cbuffer.

`UNIFIED_WATER` branches in `Water.hlsl`:

| Lines | Change |
| --- | --- |
| `:97-100` | Drops the VS `FogParam` output |
| `:190-194` | Drops the VS fog computation |
| `:215-218` | `scaledUV` from `ObjectUV.z` (tile scale) |
| `:254-258` | Wading flowmap UVs |
| `:265-269` | Flowmap UVs indexed into the world flowmap with `ObjectUV.xy` = W,H |
| `:645-649` | `flowmapDimensions = TexCoord4.xy` (vanilla uses `.xx`) |
| `:1025-1027` | `distanceBlendFactor = 1` (no LOD distance fade) |
| `:1241-1247`, `:1304-1310` | Per-pixel fog instead of VS fog |

WE in `Water.hlsl`:

- `#undef WATER_EFFECTS` and `WETNESS_EFFECTS` for `LOD` (`:603-606`).
- `WATER_PARALLAX` is defined and `WaterParallax.hlsli` included only when `WATER_EFFECTS && !VC` (`:608-611`).

### 2.3 Feature callbacks overridden (vs `src/Feature.h:168-209`)

| Feature | Overrides | Not overridden |
| --- | --- | --- |
| UW | `DrawSettings` (`UnifiedWater.cpp:128-141`), `DrawOverlay`/`IsOverlayVisible` (`:143-202`), `DataLoaded` (`:204-262`), `PostPostLoad` (`:445-471`), `IsCore` (`UnifiedWater.h:114`) | `SetupResources`, `Prepass`, `Reset`, `Save/LoadSettings` |
| WE | `SetupResources` (`WaterEffects.cpp:5-11`), `Prepass` (`:13-18`), `HasShaderDefine`, `IsCore` (`WaterEffects.h:36`) | Everything else, including `DrawSettings` |

- **Where `Prepass` runs.** It runs inside `Deferred::PrepassPasses` (`src/Deferred.cpp:236-250`), which is called from `StartDeferred` (`:296`).
- **Where `SetupResources` runs.** It runs from `State::Setup` (`src/State.cpp:340`).
- **Order of `PostPostLoad` and `DataLoaded`.** UW's hooks are installed in `PostPostLoad` (`src/XSEPlugin.cpp:96`), which runs before `DataLoaded` (`:138`). Every thunk is therefore guarded by `IsWaterDataReady()` (`UnifiedWater.cpp:515-518`).

## 3. GPU resources

| Resource | Owner | Created | Freed | Notes |
| --- | --- | --- | --- | --- |
| `causticsView` SRV (DDS) | WE (`winrt::com_ptr`, `WaterEffects.h:9`) | `SetupResources` via `CreateDDSTextureFromFile` (`WaterEffects.cpp:5-11`) | Never explicitly; lives until process exit | Bound every frame to PS t65 (`:13-18`), even in interiors. The HRESULT is not checked, so a missing file binds null. Only `Lighting.hlsl` reads t65 (`WaterCaustics.hlsli:1`) |
| World flowmap `NiSourceTexture` | UW `Flowmap::flowmapTex` (`Flowmap.h:42`), **also** stored into the engine global `gFlowMapSourceTex` (`UnifiedWater.cpp:428-443`) | `BSShaderManager::GetTexture` from `textures\water\flowmaps\Tamriel-Flowmap.*.dds` (`Flowmap.cpp:84-162`) | `Flowmap::Reset()` only on regenerate (`:17-26`); otherwise process lifetime | Engine-managed texture. The engine binds it as `FlowMapTex` t8 (`Water.hlsl:338`) |
| Temporary stitch textures plus the 6-mip `B8G8R8A8` target | UW `GenerateFlowmap` | `Flowmap.cpp:252-310` (per-cell `CreateTexture`, `CreateTexture2D`) | End of function scope (com_ptr) | Uses a **deferred context** and `ExecuteCommandList` under `Util::MultithreadScope` (`:175-185,333-340`), then saves to Data (`:342-349`) |
| `waterMesh` (`NiPointer<BSTriShape>`) and its clones | UW (`UnifiedWater.h:120`) | `BSModelDB::Demand("meshes\\water\\watermesh.nif")` (`UnifiedWater.cpp:217-231`); clones per tile (`:680-690`) | Clones are detached on block detach (`:775-779`); orphaned reflection/normal entries go through the engine deferred-release queue (`:107-125,727-728`) | Scene graph objects; the GPU buffers are engine-owned |

What UW borrows from the engine: the global `gWaterLOD` node, the flowmap globals and the displacement globals. It **writes into them every draw** (`UnifiedWater.cpp:804-819,844`) but does not hold any `RE::BSGraphics` render target. Neither feature touches `BSGraphics` RTs.

Behaviour around transitions:

- **Worldspace changes** go through the SetWorldSpace and DestroySkyCell hooks (`:537-557`). The cache swap is mutex-guarded (`WaterCache.h:201-204`).
- **Loading screens** go through the LoadingMenu-close re-cull (`:271-273`). Interior detection tolerates a lagging `TES::interiorCell` (`:20-30,520-524`).
- **Terrain streaming** goes through the Attach and Detach hooks, which take the `TESWaterSystem` spinlock and reserve `waterObjects` to avoid reallocation while other threads iterate it (`:710-730`).
- **Device reset:** no handling in either feature. D3D11 Skyrim does not reset the device; nothing is recreated.
- **Cache build blocks startup.** `DataLoaded` busy-waits for the async cache build (`UnifiedWater.cpp:255-257`).
- **Files written to disk:**
  - `Data/textures/water/flowmaps/*.dds` (`Flowmap.cpp:45,342-344`)
  - `.../CommunityShaders/UnifiedWaterCache/*_cache.wc` (`WaterCache.cpp:159-186,210,301`; `src/Utils/FileSystem.cpp:115-117`)
  - `UWLoadOrder.hash` (`UnifiedWater.cpp:372-426`). A changed hash forces regeneration of everything (`:241-253`).

## 4. Settings, menu and packaging

- **Settings struct and JSON:** none in either feature. There are no `SaveSettings`, `LoadSettings` or `RestoreDefaultSettings` overrides, and no shader-side settings CB.
- **UW DrawSettings:** a "Debug" tree with two buttons, Regenerate Flowmap and Regenerate Caches (`UnifiedWater.cpp:128-141`).
- **UW overlay:** a cache-build progress bar or error, stacked below the shader-compile windows (`:143-197`).
- **WE UI:** none beyond the summary text.
- **i18n.** Keys use `T(TKEY(...))` with prefix `feature.unified_water.` (`UnifiedWater.cpp:9`). The English strings are in `package/SKSE/Plugins/CommunityShaders/Translations/en.json:1737-1747` (UW) and `:1836-1842` (WE). ru and zh_CN also exist.
- **ini versions:** UW `1-0-0`, WE `1-3-0`. Neither has an `Alpha`, `Beta` or `Unreleased` flag, so both are Release stage. UW moved to release in commit `d1019c21d`.
- **CORE and packaging.**
  - Both features have `CORE` markers, so CMake adds them to `FEATURE_CORE_NAMES` (`CMakeLists.txt:336-345`).
  - Core features are skipped for `Package-<Feature>` (`CMakeLists.txt:1197-1201`). They are bundled into `Package-Core` and AIO instead (`:1168-1195`), and the CORE file is removed from the AIO output (`:662-669`).
  - UW and WE also override `IsCore()` to return true.
- **Registration.**
  - Globals: `src/Globals.cpp:84,86`, `src/Globals.h:117,119`.
  - Feature list order: `src/Feature.cpp:237` (WE) and `:260` (UW).
- **Legacy features.** `WaterBlending`, `WaterCaustics` and `WaterParallax` are marked obsolete and replaced by WE (`src/FeatureIssues.cpp:65-88`).
- **Load gating.** A feature loads only if `Data\Shaders\Features\<Short>.ini` exists with a known version (`src/Feature.cpp:53-140`). If UW's `DataLoaded` fails it sets `failedLoadedMessage` but keeps `loaded`, so `UNIFIED_WATER` is still injected (`UnifiedWater.cpp:212-215`).

## 5. What Fluid Field can reuse

### 5.1 Flow-map functions (`package/Shaders/Water.hlsl`, only under `FLOWMAP`)

| Function | Lines | Use for FF |
| --- | --- | --- |
| `struct FlowmapData` | `:408-412` | Flowmap encoding: RG = rotation sin/cos, B = strength, A = mask |
| `GetFlowmapDataTextureSpace` | `:434-441` | Raw sample at `TexCoord2.zw` |
| `GetFlowmapDataUV` | `:462-469` | UV-space flow vector for normal scrolling |
| `GetFlowmapDataWorldSpace` (2 overloads) | `:578-600` | World-space flow; documented as the one to use for "ripples, debris, foam". **This is the hook for seeding FF currents from vanilla flow** (ROADMAP Phase 3) |
| `GetFlowmapNormal` | `:550-560` | 4-phase flow normal sample with a corrected mip footprint |
| `GetFlowmapHeightBlended` / `GetFlowmapMipLevel` | `:483-514`, `:527-538` | Height from the flow-normal alpha |

The world flowmap from UW is `B8G8R8A8` at 64 px per cell (`Flowmap.cpp:266-304`). Its width, height and offsets are on the C++ side (`Flowmap.h:14-24`). FF can read them through `globals::features::unifiedWater`, but the `flowmap` member is private (`UnifiedWater.h:121`), so FF would need an accessor. A CPU-side bake of the flow under the FF grid could also sample the same file.

### 5.2 Ripples and normal blending

- **Normal composition is all in `GetWaterNormal`** (`Water.hlsl:628-801`):
  - Parallax offsets (`:640-642`, `:655-663`).
  - 4-sample flowmap normal blend with cell weights `normalMul` (`:666-690`).
  - Normals01-03 amplitude blend (`:713-727`) and flow/base lerp (`:731-736`).
  - WADING displacement normal from the engine's displacement sim on `DisplacementTex` t2 (`:742-751`).
  - Wetness rain ripples with flow-following offset, combined with `ReorientNormal` (`:753-797`).
  - It returns `WaterNormalData{normal, rippleInfo}` (`:622-626`).
- **Ripples:** `WetnessEffects::GetRainDrops(worldPos, t, normal, strength, flowOffset)` (`WetnessEffects.hlsli:28-123`) and `GetFlowAwareRippleOffset` (`:227-244`). These are Wetness Effects code and need `WETNESS_EFFECTS`.
- **Blend helper:** `ReorientNormal(n1, n2)` (`Common/Shading.hlsli:46-52`) is the RNM blend to use for adding an FF detail normal on top of `finalNormal`.
- **Vanilla wading ripples are a precedent.** The engine already runs a player-centred heightfield sim (`ISWaterDisplacement.hlsl`, `HEIGHTMAP_WADING/RAIN/SMOOTH`) that feeds `DisplacementTex` through the displacement mesh. UW hooks its position update (`UnifiedWater.cpp:826-845`).
  - FF must decide whether to replace this sim, feed it, or layer on top of it (see R3).

### 5.3 Proposed injection point for shallow-water output

- **Where to add the code.** In `GetWaterNormal`, add an `#if defined(FLUID_FIELD)` block after the WADING block (`Water.hlsl:742-751`) and before the Wetness block (`:753`). There, sample FF height, normal and flow at `input.WPosition.xy + FrameBuffer::CameraPosAdjust.xy`, the same absolute-position convention as `SharedData::GetWaterData` (`SharedData.hlsli:511-530`).
- **What to output.**
  - Normal: blend with `ReorientNormal` or a lerp by an FF coverage mask that fades at the grid edge.
  - Flow: feed the FF flow into `GetFlowAwareRippleOffset` so rain ripples follow the sim.
  - Foam: there is no foam in `Water.hlsl` today (the only "foam" match is the comment at `:575`). Foam has to be added to the diffuse or specular compose: `finalColorPreFog` at `:1208` (UNDERWATER/SIMPLE), `:1237-1239` (VC), `:1336-1342` (main DEPTH/REFRACTIONS path).
- **Passing data out.** `WaterNormalData` would need a field such as `fluid` (`:622-626`) to carry foam and height out to `main`.
- **Define.** The feature's define is injected automatically through `HasShaderDefine(Type::Water)` (`src/ShaderCache.cpp:506-510`). FF should return true only for `Water` (and `Lighting` if it needs wet shores) to limit recompiles.
- **Resource binding.** Bind the FF SRVs in `Prepass` the way WE does (`WaterEffects.cpp:13-18`), but on free slots. Check with `hlslkit-buffer-scan`; t65 is taken.
  - Note that `Prepass` runs at deferred start, before water draws. Whether a slot survives until the forward water pass depends on the engine not rebinding it. WE's t65 relies on this for Lighting draws, but this is **unverified for water draws** (see 6).
- **Motion vectors (R8).** Only the `STENCIL` technique writes them, from geometry (`Water.hlsl:1373-1381`). PS-only FF waves will not appear in them.

### 5.4 Risks

1. **No vertex displacement.** UW tiles are clones of a 783-byte `WaterMesh.nif` scaled by `instruction.size` (`UnifiedWater.cpp:682-687`), and the VS does no height offset (`Water.hlsl:165-303`). FF height has to be shown in the pixel shader (normals, parallax, refraction offset) unless FF adds its own tessellated or projected-grid surface.
2. **Vanilla dynamic flow is disabled.** UW's `DisableVanillaWaterLOD` permanently removes the flowmap writes (`UnifiedWater.cpp:99-102`). FF cannot rely on `ISWaterFlow` actor wakes when UW is on, and FF's own flow output is the natural replacement.
3. **Water Parallax** (`WaterParallax.hlsli:88-122,146-191,201-227`):
   - It shifts Normals01 UVs and flowmap `TexCoord3` by a view-dependent offset (`Water.hlsl:655-663,691-694`). FF normals sampled at an un-parallaxed position will swim relative to the base normals.
   - Either sample FF at `WPosition.xy + parallaxOffset`, as the ripple code does at `:765-767`, or skip parallax where FF coverage is 1.
   - Parallax is off for `VC` and `LOD` water (`:603-611`).
   - The loop is capped at 64 steps (`WaterParallax.hlsli:45`).
4. **Permutation explosion.** `UNIFIED_WATER` and `WATER_EFFECTS` are injected into *every* shader type (`UnifiedWater.h:27`, `WaterEffects.cpp:20-23`). A new FF define on Water multiplies the water permutations by 2 on top of the existing ones.
5. **Hook chaining.** `BSWaterShader` vfunc 0x6, `Main_RenderWaterEffects`, and the terrain attach/detach detours are shared by UW, FrameAnnotations and WaterReflectionCubemapFix (2.1). Any FF hook there must be chained and must not assume it runs first.
6. **Water mods.**
   - UW's material CRC change and its forced `waterFlags` (`UnifiedWater.cpp:732-741`; flag bits 2 and 7 are unnamed in CommonLib, `extern/CommonLibSSE-NG/include/RE/B/BSWaterShaderProperty.h:16-23`) override per-mesh flags on distant tiles.
   - Mods that ship their own water NIFs or flow DDS (e.g. Water for ENB, Realistic Water Two) change the inputs to `GenerateFlowmap` (`Flowmap.cpp:215-281`, 64x64 with 6 or more mips required) and to the caches.
   - ENB water multipliers already scale the amplitudes (`Water.hlsl:635-638`).
7. **Interiors.** UW flowmap UVs are only rewritten when `IsExteriorWorldspaceActive()` (`UnifiedWater.cpp:801,833`). Interior water uses the vanilla paths, so FF must handle both UV conventions (the `UNIFIED_WATER` `ObjectUV.xy` vs `.xx`, `Water.hlsl:645-649`).
8. **SharedData water info is coarse.** `WaterData[25]` covers only 5x5 cells around the eye, and its `.w` field is water height (`src/State.cpp:1123-1130`; `-FLT_MAX` when there is no water, `Utils/Game.cpp:124`). It is useful for gating FF on the CPU and GPU, but not as a depth map.

## 6. Open questions

1. What is in `WaterMesh.nif`: vertex count, UV layout, and whether tiles have enough density for even coarse VS displacement? The file is binary and was not parsed.
2. Does a PS SRV bound in `Prepass` (`Deferred.cpp:236-250`) survive until the forward water draw, or does the engine's state manager unbind or overwrite it? A RenderDoc capture is needed. WE's t65 is only consumed by `Lighting.hlsl`, so it does not prove anything about water draws.
3. When exactly is `Main::RenderWaterEffects (35561)` called relative to the Prepass? This matters if FF wants to feed the vanilla displacement sim.
4. What do the three NOP'd calls at `+0x1B7/0x1EA/0x202` do, exactly? The code comment says "compute shader calls that write to the flow map", but they were not verified in Ghidra. It is also unknown whether wading displacement (`ISWaterDisplacement`) still runs with UW active. It appears to, since `UpdateDisplacementMeshPosition` is still post-processed.
5. Thread affinity of `BGSTerrainBlock_Attach/Detach`. The comments imply other threads iterate `waterObjects` concurrently (`UnifiedWater.cpp:710-719`). Which thread is it, and is it safe for FF to read UW tile state from the render thread?
6. If `DataLoaded` fails after `PostPostLoad`, `UNIFIED_WATER` shaders run against vanilla water geometry and the vanilla flowmap with the `ObjectUV.xy` convention. Is that mismatch visible, or are the setup globals vanilla-compatible?
7. Which water-mod combinations break `GenerateFlowmap`'s 64x64/6-mip validation (`Flowmap.cpp:266-271`), and does a partial flowmap produce seams in FF flow seeding?
8. Register availability in `Water.hlsl` once Skylighting, DynamicCubemaps, IBL, LLF and ExpHeightFog are included (`:382-390,613-619,1005-1011`). `hlslkit-buffer-scan` was not run, because this task is read-only and no build was done.
