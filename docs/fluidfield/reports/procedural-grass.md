# Procedural Grass: read-only report for Fluid Field

Oct 13, 2026 · read-only study of an experimental feature

**Scope:** what the experimental Community Shaders **Procedural Grass** feature by Dwemer Engineer does, how it sits next to vanilla grass and the existing CS grass features, and where Fluid Field (FF) can connect to it in either direction. FF can drive the blades with wind, and grass can act as drag or as an obstacle in the field. Context: `ROADMAP.md` (grass bending is the optional side track, ROADMAP.md:16, 87), `reports/grass-collision.md`, `research/fluidninja-live2.md`, and FF's grid on `ff/grid-core` (`features/Fluid Field/Shaders/FluidField/Common.hlsli`, `src/Features/FluidField.h`).

**Source:** no source repository exists. Everything here comes from the author's test build: a full CS AIO dated 2026-10-08, with shaders dated 2026-09-30. Paths are relative to that build and written as `pgrass:<path>:<lines>`. The HLSL ships as source. The C++ was studied only through printable strings in `SKSE/Plugins/CommunityShaders.dll`: setting keys, log messages, GPU resource debug names, profiler pass names and shader paths. The DLL was not disassembled or decompiled. A `CommunityShaders.pdb` ships beside it. Anything taken from strings alone, or inferred from shader structure, is marked **UNVERIFIED**. **Update (Oct 14, 2026):** the DLL was later decompiled with Ghidra using that PDB (D9 as amended). Section 7 holds the C++ findings, and they take precedence over the string-based guesses in sections 2–4.

**Licence:** Procedural Grass was shared on the Community Shaders Discord. It is Community Shaders code under GPL-3.0-or-later, so Fluid Field may study and reuse it with credit to Dwemer Engineer. Excerpts below are quoted where they help the explanation.

**Summary:** Procedural Grass is a full GPU grass renderer (about 5.4k lines of HLSL plus a top-down occlusion pass). It does not use vanilla grass meshes.

-   **Placement.** A compute generator places blades on a world-aligned lattice of 2048-unit "quadrants". Placement uses landscape heights from LAND, per-landscape-texture grass types, Voronoi clumping, slope limits and a top-down map of static objects.
-   **Tiers and culling.** There are four tiers (High, Mid, Low, Far), culled against a shared Hi-Z pyramid. The generator writes packed blade records and indirect draw arguments.
-   **Rendering.** High, Mid and Low draw into the CS deferred G-buffer. Far is forward-shaded. A terrain canopy pass and a darkening pass shade the ground under distant grass.
-   **Wind.** Wind is a single global direction and speed, set in the feature's own settings (`WindSpeed`, `WindAngle`). Each blade gets a scalar sway, computed in the generator with vanilla's gust waveform. The vertex shader then shifts the tip along the global direction, weighted by t².
-   **Grass Collision.** Grass Collision is read once per blade, in the generator, and cached in the blade record.
-   **Where FF plugs in.** The plug point for an external velocity field is the generator's wind block (`PGrassBladeConstruction.hlsli:347-362`) or the collision cache next to it (`:619-641`). The vertex shader (`PGrassVS.hlsl:322-331`) only needs a per-blade 2D offset instead of `windDir * scalar`.
-   **Biggest reuse finding.** `TopDownOcclusion`, the feature's GPU top-down capture of static geometry (highest and lowest Z around the player), is in effect R1 Prototype A already built. It is a strong candidate for D7 stage 4 (statics).

## 1. File inventory

| Build path                                                                                        | Lines         | Role                                                                                                                |
| ------------------------------------------------------------------------------------------------- | ------------- | ------------------------------------------------------------------------------------------------------------------- |
| `Shaders/ProceduralGrass/PGrassCommon.hlsli`                                                      | 381           | `GrassGlobals` b8, `GrassTypes` b9, `GrassGeneratorTypes` b10, `Blade` records, Far helpers, object-surface map t66 |
| `Shaders/ProceduralGrass/PGrassBladeGeneratorCS.hlsl`                                             | 463           | Generator compute shader: work list, LAND height sampling, group-shared staging, indirect-args append               |
| `Shaders/ProceduralGrass/PGrassGeneration.hlsli`                                                  | 314           | Patch candidates: lattice, LOD keep, grass-map lookup, slope fill, Far handoff                                      |
| `Shaders/ProceduralGrass/PGrassPlacement.hlsli`                                                   | 149           | Height and slope sampling, terrain lift, grass-cell lookup, grass-type pick                                         |
| `Shaders/ProceduralGrass/PGrassBladeConstruction.hlsli`                                           | 648           | Per-blade build: LOD dither, clumping, height and width, **wind**, packing, cached **collision**, skylighting SH    |
| `Shaders/ProceduralGrass/PGrassCulling.hlsli`                                                     | 391           | Frustum and Hi-Z tests (patch, tile, blade, Far triangle)                                                           |
| `Shaders/ProceduralGrass/PGrassCoverage.hlsli`                                                    | 74            | Object clearance from the top-down maps (t2/t4), grass-map edge warp and coverage                                   |
| `Shaders/ProceduralGrass/PGrassCanopy.hlsli`                                                      | 159           | Terrain canopy type cache (t64), Far interior and coverage tests                                                    |
| `Shaders/ProceduralGrass/PGrassVS.hlsl`                                                           | 473           | Vertex shader for all tiers: Bezier blade, **wind offset**, collision offset, view thickening                       |
| `Shaders/ProceduralGrass/PGrassPS.hlsl`                                                           | 1125          | Deferred (High/Mid/Low) and forward (Far) pixel shader, OpenPBR-style material                                      |
| `Shaders/ProceduralGrass/PGrassLighting.hlsli`, `PGrassFarLighting.hlsli`, `PGrassMaterial.hlsli` | 252 / 99 / 87 | Lighting and material helpers                                                                                       |
| `Shaders/ProceduralGrass/PGrassTierIO.hlsli`                                                      | 69            | VS→PS interface for each tier                                                                                       |
| `Shaders/ProceduralGrass/PGrassDepthPS.hlsl`                                                      | 13            | Depth prepass PS (High and Mid)                                                                                     |
| `Shaders/ProceduralGrass/PGrassBatchArgsCS.hlsl`                                                  | 16            | Turns blade counts into batched indirect draw arguments                                                             |
| `Shaders/ProceduralGrass/PGrassDensityGatherCS.hlsl`                                              | 75            | Grass presence (ids) → density texture for canopy AO                                                                |
| `Shaders/ProceduralGrass/PGrassDensityAOVS.hlsl`, `PGrassDensityAOPS.hlsl`                        | 14 / 230      | Fullscreen terrain darkening under grass                                                                            |
| `Shaders/ProceduralGrass/PGrassTerrainLiftCS.hlsl`                                                | 178           | Toroidal world map of rendered-terrain lift above LAND, for Low and Far roots                                       |
| `Shaders/ProceduralGrass/PGrassTerrainCanopyCS.hlsl`                                              | 189           | Shades the distant canopy directly into the lit scene (`MainLighting` UAV)                                          |
| `Shaders/ProceduralGrass/PGrassAmbientLUTCS.hlsl`                                                 | 26            | 32² ambient and IBL lookup table by normal                                                                          |
| `Shaders/TopDownOcclusion/ObjectCapture.hlsl`, `OcclusionPadCS.hlsl`                              | 98 / 38       | Top-down rasterisation of world geometry into highest/lowest Z and grass-surface records, then max/min padding      |
| `Shaders/Common/HiZ/GrassHiZCS.hlsl`, `Shaders/Common/HiZ/SPD.hlsl`                               | 34 / 369      | Hi-Z pyramid, moved here from Grass Optimizations (section 2.3)                                                     |
| `Shaders/RunGrass.hlsl`, `Shaders/GrassCollision/*`, `Shaders/GrassOptimizations/*`               |               | Vanilla grass path and the changes made for sharing (section 2)                                                     |
| `Shaders/Features/ProceduralGrass.ini`                                                            | 2             | `[Info] Version = 1-0-0`. No CORE marker or stage flag seen.                                                        |
| `SKSE/Plugins/CommunityShaders/Translations/en.json`                                              | 1121-1348     | 228 `feature.procedural_grass.*` keys                                                                               |

C++ files named in the DLL strings (their `__FILE__` paths): `src/Features/ProceduralGrass.cpp`, `src/Features/ProceduralGrass/PGrassRenderer.cpp`, `ProceduralGrassCanopy.cpp`, `ProceduralGrassSettings.cpp`, `TopDownOcclusion.cpp`. Two shared modules also appear: `src/HiZPyramid.cpp` and `src/TerrainHeightMap.cpp`.

## 2. Relation to vanilla grass and existing CS grass features

### 2.1 Replace or augment

Procedural Grass is a **separate renderer**. It never touches `BSGrassShader` or vanilla grass geometry. Its blades come only from the generator buffer (`PGrassVS.hlsl:13, 61`).

-   **Vanilla grass is left alone by default, UNVERIFIED.** A UI toggle "Toggle Vanilla Grass Rendering" exists (en.json:1319). How it hides vanilla grass is unknown, and so is whether that is a skip in a hook or a setting.
-   **Grass types are per landscape texture (LTEX).** Each texture can hold weighted variants. "A texture with no variants grows the base type when it supports vanilla grass. Configured variants also enable procedural grass on textures with no vanilla grass." (en.json:1234). The labels "(no vanilla grass)" (1272) and "Only grass-growing" (1282) are filters in that list.
-   **Type configs are saved** to `ProceduralGrassTypes.json`, with keys `LandTexture`, `Variant`, `Weight`, `NoGrass` and `Overrides` (DLL strings).
-   **The vanilla INI value `fTexturePctThreshold:Grass` is read** (DLL string). That suggests the vanilla texture-percentage threshold decides which LAND vertices "grow grass". UNVERIFIED.
-   **Grass on statics.** A second source, "Grass on static objects, such as dirt cliffs" (`ObjectGrassEnabled`, `ObjectGrassTextures`), comes from the top-down object surface map (section 3.4).

### 2.2 How placement reads the landscape

-   **Height.** The CPU caches LAND per **quadrant**, a 2048-unit square holding 17×17 height samples at 128-unit spacing, which is LAND vertex spacing (`PGrassBladeGeneratorCS.hlsl:141-173`, `QuadrantHeights` t3). The generator reads them bilinearly, with the slope taken from the same four corners. Where no LAND is cached, it falls back to a whole-worldspace height texture (`PGrassPlacement.hlsli:6-23`, `TerrainHeightTexture` t0, `heightMapScale/Offset/ZRange` in b8). The DLL has a shared `TerrainHeightMap` module ("Listing xLODGen height maps…") and the debug UI shows "Height map worldspace: %s". This looks like the same heightmap loader as Terrain Shadows (repo `src/Features/TerrainShadows.cpp:292`), moved out for sharing. UNVERIFIED.
-   **Debug strings show where LAND comes from.** One reject chain reads `cells -> exterior -> land -> loadedData -> mesh -> preProcessed` (en.json:1206). Others are "LAND anchor applied / mesh world Z", "Ignore preprocessed-node check" and "Includes LAND meshes that are not marked as preprocessed" (en.json:1191-1197). Heights are read from loaded cells' `TESObjectLAND` data and anchored against the landscape mesh's world Z. **This matches FF's D7 stage 1** (cell cache of LAND heights with an absolute-Z check). UNVERIFIED in detail.
-   **Grass type.** `QuadrantGrassCells` (t6) holds 16×16 cells per quadrant. Each is a uint packing the 8-bit grass-type IDs of its four LAND corners (`PGrassPlacement.hlsli:43-48`). `ComputeGrassType` picks a corner by bilinear weight and fades density near bare corners with world-space noise (`PGrassPlacement.hlsli:71-111`, `PGrassCoverage.hlsli:116-148`). `OccupancyRows` (t12) is a per-quadrant bitmask of the 16×16 cells, used for early rejection (`PGrassPlacement.hlsli:50-63`).
-   **Slope limits** are per type (`PGrassBladeConstruction.hlsli:163-166`). Slope fill adds extra candidates in proportion to the extra ground area on steep terrain (`PGrassCommon.hlsli:142-146`).
-   **Rendered terrain versus LAND.** Far LOD terrain differs from LAND, so Low and Far roots are raised by a "terrain lift". It is measured from scene depth into a world-aligned **toroidal** 512² map of 256-unit cells (`PGrassCommon.hlsli:49-59`, `PGrassTerrainLiftCS.hlsl:90-109`, `PGrassPlacement.hlsli:32-40`).

### 2.3 Changes to shared grass shaders (build compared with repo `feature/fluid-field` @ 92fd91c76)

**`RunGrass.hlsl`.** The only change for Procedural Grass wraps the Grass Collision include in `GRASS_COLLISION_REGULAR_GRASS` (`pgrass:Shaders/RunGrass.hlsl:88-92`). Every other difference is **drift in the base version**: the build predates three repo changes that its file lacks.

-   #2697: grass SSS and screen transmission shadows. The build keeps the older `GetSoftLightMultiplier`.
-   #2757: LLF cached local shadows. The build has no `LocalShadow` branch.
-   #2825: ISL merged into LLF. The build still has `#if defined(ISL)` blocks.

Ignore these when porting.

**`GrassCollision/GrassCollision.hlsli`.** This file was refactored so a compute shader can use it:

-   The cbuffer register is now a macro, `GRASS_COLLISION_CBUFFER_REGISTER`, defaulting to b5 (`:5-9`). The generator sets it to b11 (`PGrassBladeGeneratorCS.hlsl:17`).
-   `ProceduralAnimation` gains `smoothRecovery`/`recoveryRate`, an exponential smoothstep recovery instead of the damped cosine spring (`:26-41`).
-   `GRASS_COLLISION_CURRENT_ONLY` skips the previous-frame samples, which Mid uses (`:84, 95, 102, 147, 177, 182`).
-   A generic `GetDisplacedPosition(worldPos, centre, alpha, maxDistance, smoothRecovery, recoveryRate, …)` takes over from the `VS_INPUT` overloads (`:157-190`). The old overloads become thin wrappers under `GRASS_COLLISION_REGULAR_GRASS` (`:193-209`).

**Behaviour change for vanilla grass:** with Grass Optimizations on, vanilla grass now uses the smooth recovery (`true, 1.5`) and a 1.5-cell sample delta. Without it, vanilla grass keeps the old spring (`false, 1.5`). `CollisionUpdateCS.hlsl` is unchanged.

**Grass Optimizations.** `GrassCullingCS.hlsl` and `DetectComplexCS.hlsl` are byte-identical to the repo. `GrassHiZCS.hlsl` and `SPD/SPD.hlsl` moved to `Shaders/Common/HiZ/`, and the C++ moved from `src/Features/GrassOptimizations/HiZPyramid.cpp` to `src/HiZPyramid.cpp` (DLL string). `GrassHiZCS` now prefers the **live main depth** and falls back to the post-Z-prepass copy (`pgrass:Shaders/Common/HiZ/GrassHiZCS.hlsl:1-2`). The DLL log reports `source=LIVE kMAIN copy | POST_ZPREPASS_COPY (stale fallback)`. The pyramid is shared by Grass Optimizations and Procedural Grass (`GrassHiZ` t8, "Shared current-frame scene-depth pyramid", `PGrassBladeGeneratorCS.hlsl:198`).

## 3. Pipeline, engine hooks and GPU resources

### 3.1 Tiers

| Tier               | Renderer template (DLL)  | Patch blades | Geometry                                                                                                                         | Wind                             | Collision                  | Notes                                                                                                              |
| ------------------ | ------------------------ | ------------ | -------------------------------------------------------------------------------------------------------------------------------- | -------------------------------- | -------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| High (inner/outer) | `PGrassRenderer<25,4>`   | 4            | inner: 7 levels, 15–16 vertices; outer: Mid shape (`PGrassVS.hlsl:63-74`)                                                        | sway + facing                    | cached, current + previous | Detail fade 512–1536 (`PGrassBladeConstruction.hlsli:373-377`); per-blade skylighting SH, terrain and cloud shadow |
| Mid                | `PGrassRenderer<81,2>`   | 2            | 2 levels, 5 vertices; morphs to Low's straight profile from 4096 to 6144 (`PGrassCommon.hlsli:148-155`, `PGrassVS.hlsl:297-301`) | sway (fades with morph) + facing | cached, current only       | Hands off to Low at 6144, over a 1024 band                                                                         |
| Low                | `PGrassRenderer<169,1>`  | 1            | 3–4 vertices, straight                                                                                                           | facing only                      | (not computed)             | Terrain lift; no depth prepass                                                                                     |
| Far                | `PGrassRenderer<4000,1>` | 1            | 1 triangle (double near the handoff)                                                                                             | facing only                      | none                       | Forward-shaded; width ×1.5–32 and height up to ×2 with distance; canopy replacement                                |

The template arguments appear in DLL function signatures. Reading them as (quadrant capacity, patch blade count) is UNVERIFIED: 25 = 5², 81 = 9², 169 = 13², and the second argument matches `PATCH_BLADE_COUNT`. Generator compile defines from the strings: `THREADGROUP_SIZE, DENSITY, QUADRANT_DATA_SIZE, PATCH_BLADE_COUNT, SLOPE_EXTRA_BLADES, BLADE_BATCH_SIZE, HIGH_GEOMETRY_LOD, MID/LOW_OUTER_GEOMETRY, FAR_DOUBLE_GEOMETRY, PGRASS_FAR_HANDOFF, PGRASS_CACHED_COLLISION`. PS defines: `HIGH_INNER, PGRASS_DRY_WETNESS, PGRASS_NO_LOCAL_LIGHTS, DEPTH, DEPTH_CLIP, LINEAR_LIGHTING`.

### 3.2 Pipeline order

Inferred from resource dependencies and the profiler pass names in the DLL, which appear in this order. The exact order is UNVERIFIED.

1.  **CPU: visible quadrants** (`ProceduralGrass::Visible Quadrants`). Cache LAND heights and grass cells per quadrant. Build `VisibleWork` (a packed task list with quadrant id, lane, frustum, tile, object and compact-Far flags; `PGrassBladeGeneratorCS.hlsl:175-188, 214-223`), `QuadrantsCB` (b7) and `TileHeightBounds`.
2.  **Top-down occlusion.** Rasterise statics into `HeightMap` / `HeightMapLow` (highest and lowest Z) and grass surface records, then pad with a separable max/min pass (`TopDownOcclusion/ObjectCapture.hlsl:85-97`, `OcclusionPadCS.hlsl:115-135`). The window is centred at `occlusionParams.xy` with half extent `occlusionHalfExtent` (b8), and the map is **not** toroidal (see section 3.4).
3.  **Density gather and ambient LUT.** Presence ids become a blade-density texture (`PGrassDensityGatherCS.hlsl:204-225`). A 32² ambient and IBL LUT is built by normal (`PGrassAmbientLUTCS.hlsl`).
4.  **Hi-Z build** from current-frame depth (shared `HiZPyramid`, section 2.3).
5.  **Generation for each tier** (`ProceduralGrass::{High,Mid,Low,Far} Generation`). One thread per patch. The tests run in this order:

    1. tile and group Hi-Z test (`PGrassBladeGeneratorCS.hlsl:418-432`)
    2. occupancy (`PGrassGeneration.hlsli:106`)
    3. LOD keep or dither (`:117-137`)
    4. grass cell (`:139-144`)
    5. surface plane (`:155-165`)
    6. slope fill (`:186-204`)
    7. `BuildBlade`, which runs type, slope, clump search, LOD, frustum, object clearance, height, facing, **wind**, packing, per-blade Hi-Z, skylighting SH and **collision** (`PGrassBladeConstruction.hlsli:119-646`)

    Blades are staged in groupshared memory and appended with one `InterlockedAdd` per group into `IndirectArgs` (u1), with an inner list at offset 4 and an outer list at offsets 24 and 36 growing from the tail (`PGrassBladeGeneratorCS.hlsl:357-412`).

6.  **Batch args.** Blade counts become batched instanced draws (`PGrassBatchArgsCS.hlsl`). The VS rejects the final batch's tail (`PGrassVS.hlsl:25-54`).
7.  **High and Mid depth prepass** (`ProceduralGrass::High Depth`, `Mid Depth`; `PGrassDepthPS.hlsl` clips below `grassHiZBounds.w`).
8.  **Terrain lift** (`ProceduralGrass::Terrain Lift`). Reads scene depth "including the High and Mid prepass" (`PGrassTerrainLiftCS.hlsl:10`), refreshes one quarter of the texels per frame (`terrainLiftPhase`), and feeds the next frame's Low and Far roots.
9.  **Deferred G-buffer draw for High, Mid and Low** (`ProceduralGrass::Deferred`). The PS writes CS's eight deferred targets: diffuse, motion vectors, normal, albedo, specular, reflectance and masks (`PGrassPS.hlsl:62-74`).
10. **Terrain darkening and shadow** (`ProceduralGrass::Terrain Shadow`, `PGrass::TerrainDarkeningSceneCopy`; `PGrassDensityAOVS/PS.hlsl`, `PGRASS_DARKENING_COPY`). The terrain under grass is darkened by coverage.
11. **Terrain canopy** (`ProceduralGrass::Terrain Canopy`). A compute shader shades the distant grass canopy into `MainLighting` on landscape pixels (`PGrassTerrainCanopyCS.hlsl:21, 28, 129-130`). It needs typed-UAV loads; the DLL logs "Terrain canopy unavailable: scene format does not support typed UAV loads".
12. **Far forward** (`ProceduralGrass::Far Forward`). Far writes only diffuse and motion vectors (`PGrassPS.hlsl:62-74`).

**Lighting integration:** `PGrassPS.hlsl` and the generator use the CS features listed below.

-   Skylighting (SH baked per blade in the generator, `PGrassBladeConstruction.hlsli:604-616`)
-   Terrain Shadows and Cloud Shadows (baked per blade for High, `:565-576`)
-   Screen-Space Shadows
-   Light Limit Fix and ISL
-   Wetness Effects
-   IBL (ambient LUT)

### 3.3 Engine hooks (from DLL strings only)

_Superseded by section 7.2, which lists the full hook set and the vanilla-grass mechanism, and by 7.3 for loading-screen behaviour._

-   `[Procedural Grass] Installed hooks` is logged from `ProceduralGrass::PostPostLoad`.
-   **One logged hook:** `stl::write_thunk_call<ProceduralGrass::Main_RenderShadowmasks_UpdateCamera, 0x05>`. This is a call-site thunk inside `Main::RenderShadowmasks`. Its role, probably a per-frame camera and main-thread point for steps 1–2, is UNVERIFIED. Hooks installed without a logged `__FUNCSIG__` would not show up as strings.
-   **Loading screens:** no string mentions `LoadingMenu`, reset or cell-change handling for Procedural Grass. The terrain lift map re-validates wrapped texels by itself (`PGrassTerrainLiftCS.hlsl:92-109`). Whether the quadrant cache is cleared on worldspace change is unknown. The debug UI shows "Grass quadrants cached: %zu" and "Data handler unavailable (not in a loaded game)" (en.json:1176, 1207). **UNVERIFIED; ask the author.**

### 3.4 GPU resources

Debug names come from the DLL. Registers come from the shaders. "CS" means the generator unless another shader is named.

| Resource (debug name)                                                     | Shader binding                                                | Content                                                                                                                 |
| ------------------------------------------------------------------------- | ------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| `PGrass::GlobalsCB`                                                       | b8 (CS, VS, PS) `GrassGlobals` (`PGrassCommon.hlsli:1-47`)    | wind, occlusion window, AO/light params, height map transform, Hi-Z params, LOD origin, terrain lift and canopy windows |
| `PGrass::TypesCB`                                                         | b9 `GrassTypes[128]` (`PGrassCommon.hlsli:120-123`)           | per-type shape and material                                                                                             |
| `PGrass::GeneratorTypesCB`                                                | b10 `GrassGeneratorTypes[128]` (`PGrassCommon.hlsli:265-268`) | compact per-type generator data                                                                                         |
| `PGrass::QuadrantsCB`                                                     | b7 `QuadrantData` (`PGrassBladeGeneratorCS.hlsl:105-110`)     | LOD fade ramps, per-quadrant world position, hash and flags                                                             |
| `PGrass::WorkRange`                                                       | b0 `GrassWorkRange` (`:178-182`)                              | work-list offset and compact share                                                                                      |
| Grass Collision per-frame CB                                              | b11 in the CS (`:17`)                                         | reused from Grass Collision                                                                                             |
| terrain heightmap (`TerrainHeightMap::HeightMap`)                         | t0 (CS), t5 (DensityAO), t0 (TerrainLift)                     | worldspace height texture                                                                                               |
| `TopDownOcclusion::HeightMap` / `HeightMapLow`                            | t2 / t4 `OcclusionMaskHigh/Low` (`PGrassCoverage.hlsli:6-7`)  | padded highest and lowest static Z in the window                                                                        |
| `PGrass::QuadrantHeights`                                                 | t3                                                            | 17×17 LAND heights per quadrant                                                                                         |
| `PGrass::VisibleWork`                                                     | t5                                                            | task list                                                                                                               |
| `PGrass::QuadrantGrassCells`                                              | t6                                                            | packed corner grass ids                                                                                                 |
| `PGrass::GrassDensity`                                                    | t7 (CS), t71 (PS), u0 (gather)                                | `uint` blade count per texel in the occlusion window                                                                    |
| `HiZPyramid::Texture`                                                     | t8 `GrassHiZ`                                                 | shared Hi-Z                                                                                                             |
| `PGrass::TerrainLift` / `TerrainLiftSurface` / `TerrainLiftMeasured`      | t9 / t10 (Low/Far CS); u1 / u2 / u0 (TerrainLift CS)          | toroidal 512² map, 256-unit cells                                                                                       |
| `PGrass::TileHeightBounds`, `PGrass::OccupancyRows`                       | t11, t12                                                      | culling and occupancy                                                                                                   |
| Skylighting probes                                                        | t50 (`SKYLIGHTING_PROBE_REGISTER`, `:24-27`)                  | CS-shared                                                                                                               |
| `PGrass::TerrainCanopyTypes`                                              | t64 (`PGrassCanopy.hlsli:5`)                                  | per-LAND-sample type cache                                                                                              |
| `PGrass::ObjectSurfaces` (+ `ObjectSurfaceDepth`)                         | t66 (`PGrassCommon.hlsli:309`)                                | topmost object surface: Z, normal XY, type and density                                                                  |
| `PGrass::DistantAmbientLUT`                                               | t73 (PS, canopy), u0 (LUT CS)                                 | 32² ambient                                                                                                             |
| grass scene depth, material detail, screen AO                             | t74, t75 (array), t76 (PS, canopy)                            |                                                                                                                         |
| shadow mask                                                               | t14 / s14 (PS, canopy); t45 `GrassScreenShadow` (canopy)      |                                                                                                                         |
| `PGrass::GrassPresence`                                                   | t1 (DensityAO), t0 (gather)                                   | grass id per sample around the player (Low-tier map)                                                                    |
| Grass Collision texture                                                   | t100                                                          | reused from Grass Collision                                                                                             |
| `PGrass::Blades`                                                          | u0 (CS), t0 (VS)                                              | blade records, see the sizes below                                                                                      |
| `PGrass::IndirectArgs`, `BatchArgs`, `IndirectArgsStaging`                | u1 (CS), t1 (VS), t13 / u0 (BatchArgs)                        | the staging copy is probably a CPU readback for the high-water-mark log (UNVERIFIED)                                    |
| `PGrass::{High,HighOuter,Mid,MidOuter,Low,LowOuter,Far,FarDouble}Indices` | index buffers                                                 | fixed blade topologies                                                                                                  |
| samplers                                                                  | s0 (linear), s13 (detail), s14 (shadow)                       |                                                                                                                         |

**Blade record sizes** follow from `PGrassCommon.hlsli:272-305`:

-   Far: 16 bytes
-   Low: 24 bytes
-   Mid: 36 bytes, or 40 with the collision cache
-   High: 32 bytes, or 44 with the collision cache

The DLL logs `[Procedural Grass] {} blade buffer high-water mark: {} blades ({:.1f} MiB), {} required` and errors with "Procedural grass blade buffer exceeds the D3D11 buffer-size limit".

## 4. Wind and interaction

### 4.1 Wind source

-   **Inputs.** `GrassGlobals` holds `windSpeed`, `previousWindSpeed`, `windDir`, `previousWindDir`, `windAngle` and `windRotationScale` (`PGrassCommon.hlsli:8-15, 35`).
-   **They come from settings.** The settings keys are `WindSpeed` and `WindAngle` (DLL strings), with UI "Wind Speed: Controls how quickly wind waves move through the grass" and "Wind Direction" (en.json:1344-1348).
-   **No weather link found.** None of the Procedural Grass strings ties wind to weather. The weather wind strings in the DLL belong to the CS Editor. So wind is a **global, user-set constant**, not from the weather and not spatially varying. UNVERIFIED: the C++ might still scale it by sky wind.

### 4.2 Bend model

There are four parts. The first two run per blade in the generator, the last two per vertex in the VS.

1.  **Facing toward the wind** (generator, `PGrassBladeConstruction.hlsli:28-42, 347-348`). The blade's yaw turns toward `windAngle` by an amount that grows with width × height × `windRotationScale`. Per-type `rotationalStiffness` reduces it, and it never turns past the wind direction. This is a static lean of the whole field, and it applies to **every tier, Low and Far included**.
2.  **Gust sway** (generator, `:10-26, 349-362`; High and Mid only). This is "vanilla grass's gust waveform", a sum of `sin`/`cos` of a phase that travels along world x+y with `SharedData::Timer`, plus 512-unit value noise and a per-blade phase and strength. The output is a **signed scalar** scaled by `speed × height response`:

    ```hlsl
    float gustAngle = 0.4f * ((worldPosition.x + worldPosition.y) * -0.0078125f + timer) + windNoise * 0.5f + bladePhase;
    ...
    return heightResponse * speed * gustStrength * ((gust1 + gust2) * 0.3f + gust0) * 0.5f;
    ```

    High also computes the previous frame's value with `Timer - miscParams.w` and `previousWindSpeed`, for motion vectors (`:354-355`). Both are stored as f16: `Blade.facingAndWind` bits 16–31 and `Blade.previousWind` bits 0–15 (`:597-598`).

3.  **Blade shape** (VS, `PGrassVS.hlsl:258-302`). The blade is a quadratic Bezier in its (facing, up) plane:

    -   the root is fixed
    -   the tip is `tiltDir × height`, where the tilt is a random angle times `tipWeight`
    -   the midpoint is `tip × mid + perp(tip) × randBend`, with `randBend = stiffness × random`

    Wind is then added as a **horizontal shear of the tip**, with no change in blade length:

    ```hlsl
    float windWeight = t * t;
    float2 windOffset = windDir * windDisplacement;   // global direction × per-blade scalar
    positionOffset.xy += windOffset * windWeight;
    ```

    (`PGrassVS.hlsl:316-331`). The PS adds the matching tangent derivative `2t · windOffset`, so normals follow the bend (`PGrassPS.hlsl:317-322`). Mid fades the wind out as it morphs to Low (`PGrassVS.hlsl:324-326`).

4.  **Collision** (generator, then VS). With `PGRASS_CACHED_COLLISION`, the generator samples Grass Collision **once per blade, at the wind-displaced tip**. It calls `GrassCollision::GetDisplacedPosition(tip, root, 1, 2048, smoothRecovery=true, 0.75, …)` and packs the 3D displacement as f16: current and previous for High, current only for Mid (`PGrassBladeConstruction.hlsli:619-641`). The VS adds it with a smoothstep weight `t²(3 − 2t)` (`PGrassVS.hlsl:338-350`). This removes the per-vertex cost of 12 texel loads that vanilla grass pays (grass-collision.md, section 2).

**Culling margin.** Hi-Z cull volumes for High and Mid are padded by `grassHiZBounds.z`, the "wind reach" (`PGrassCommon.hlsli:38`, `PGrassCulling.hlsli:283, 334`). The CPU computes this from the current wind (UNVERIFIED). Any external wind has to stay within it, or the reach has to grow.

**Regeneration.** Blades appear to be regenerated every frame. Their positions are camera-relative f16 values, wind uses the current `Timer`, and Hi-Z is the current frame. Every frame's wind and collision are therefore fresh. Whether the Far tier is cached across frames is UNVERIFIED.

### 4.3 Where a Fluid Field velocity field plugs in

**What FF provides.** On `ff/grid-core`, FF's wind layer is a `R32G32_FLOAT` texture of world units per second (`WindSolver.cpp:54-55`), toroidally addressed: world cell `c` lives at storage texel `c & (GridRes − 1)` (`Common.hlsli:1-46`). A consumer needs `GridMin`, `GridRes` and `CellSize`, plus the SRV (`WindSolver::GetVelocitySRV`, `WindSolver.h:105`). Sampling at a world position `p`:

```hlsl
float2 logical = p.xy / CellSize - float2(GridMin);       // texel units, centres at +0.5
bool inside = all(logical >= 0.5) && all(logical <= GridRes - 0.5);
float2 v = inside ? SampleBilinear(FFVelocity, logical) : 0;   // Common.hlsli:63-77
```

Fade to 0 over the last few cells before the grid edge so blades blend back to the global wind.

**Plug point A, in the generator. Recommended.** In `BuildBlade`, right after the global sway (`PGrassBladeConstruction.hlsli:349-362`):

-   `bladeWorldPos2D`, `randHeight`, `generatorType.stiffness` and the blade hash are all available there.
-   Sampling once per blade costs one bilinear fetch, against roughly 15 vertices per blade, the same trade the author made for collision.
-   Convert `v` into a tip offset with a stiffness response, for example `offset = clamp(v · height · k / stiffness, maxReach)`, and add it to `windDir · windDisplacement`.
-   For motion vectors, High needs a previous offset too. Either keep FF's previous velocity or store last frame's offset per world cell.

**What has to change in the blade record.** Today the blade stores only a scalar along the global `windDir` (`facingAndWind` high 16 bits). Two options:

-   (i) **Reuse the collision cache.** This adds no bytes. Add the FF offset to `collisionDisplacement` before packing (`PGrassBladeConstruction.hlsli:625-641`); the VS already applies it in 3D (`PGrassVS.hlsl:338-350`). The catch: it only exists when `PGRASS_CACHED_COLLISION` is defined (Grass Collision on), it uses the smoothstep weight instead of t², and Mid has no previous-frame slot.
-   (ii) **Pack a 2D offset.** Replace the f16 scalar with two SNORM8 values times `windReach`, or add one uint to `Blade`: +4 bytes for High and Mid, which raises the blade-buffer high-water mark. Then change `PGrassVS.hlsl:322-331` to use the per-blade 2D offset. The PS tangent path (`WindLodDensity.xy`, `WindRootPosition.xy`) already takes a 2D offset, so the PS needs no change.

**Plug point B, facing.** `CalculateWindAdjustedAngle(clumpedAngle, windAngle, …)` (`PGrassBladeConstruction.hlsli:348`) could take `atan2(v)` locally instead of the global `windAngle`. That gives Low and Far, which have no sway, a field-driven lean at no cost per vertex.

**Plug point C, VS only. Not recommended.** Sampling FF in `PGrassVS.hlsl` per vertex costs more, and the VS has no world-space blade root for Far without reconstruction.

**Bend model caveat.** The tip shear keeps the root and only adds a horizontal offset to the tip (no length preservation, `z` unchanged), and the collision path only clamps `z` downward. Strong FF gusts, such as shouts, would visibly stretch blades. A length-preserving variant would lower the tip by about `|offset|² / (2·height)`, or rotate the Bezier control points instead. This is an open question for the author.

**Ordering.** FF steps in `Prepass` (`src/Deferred.cpp:249`, called from `StartDeferred` at `:296`). If Procedural Grass generates before that point in the frame, FF wind lags by one frame, which is acceptable. The exact Procedural Grass generation point is UNVERIFIED (section 3.3).

## 5. Performance hints

-   **Budgets in settings** (keys and UI):
    -   `Quality` / "Density Preset" sets the High density and matching Mid, Low and Far densities (en.json:1242-1243). There are also `MidDensity`, `LowDensity`, `FarDensity` and `FarEdgeDensity`.
    -   `FarRadius` is in exterior cells beyond loaded grass (en.json:1225-1226).
    -   `ViewThicken`, `GrassMapEdgeNoise`, `OcclusionHalfExtent`, `OcclusionPadding` and `OcclusionClearance`.
-   **Fixed constants:**
    -   128 grass types (`PGrassCommon.hlsli:118`)
    -   quadrant 2048 units, LAND samples 17² (`PGrassBladeGeneratorCS.hlsl:75, 141-142`)
    -   High detail fade 512–1536, Mid→Low morph 4096–6144, terrain lift blend 6144–8192
    -   Far handoff fill fade 4096 (`PGrassGeneration.hlsli:7`)
    -   collision radius 2048
-   **Cost-saving structure:**
    -   tile and group Hi-Z rejection before any per-patch work
    -   occupancy bitmasks
    -   group-shared staging with one global atomic per group
    -   f16 and 8-bit packing
    -   batched indirect draws (`BLADE_BATCH_SIZE`)
    -   per-blade (not per-vertex) wind, collision and skylighting
    -   terrain lift refreshing a quarter of its texels per frame
-   **Profiler pass names** (`profiler->BeginPass` style; Tracy integration is UNVERIFIED): `ProceduralGrass::{Visible Quadrants, High/Mid/Low/Far Generation, High Depth, Mid Depth, Deferred, Far Forward, Terrain Lift, Terrain Shadow, Terrain Canopy}` and `PGrass::TerrainDarkeningSceneCopy`. Also `GrassCollision::CollisionUpdate` and the `HiZPyramid` log line.
-   **Debug UI:** "Blades generated high: %u mid: %u low: %u far: %u", "Quadrants high/mid/low/far", "Occluders drawn: %u", "Visualize tiers", "Disable ALL generator culls" (en.json:1177-1211). The build ships no cost numbers.
-   **Driver workarounds worth knowing:**
    -   An AMD `/O3` miscompile of `bool` struct members forces `uint` flags (`PGrassGeneration.hlsli:50`).
    -   An AMD loop miscompile forces an unrolled emit loop (`:302-303`).
    -   FXC removes stores from separate lanes (`PGrassBatchArgsCS.hlsl:8`).

## 6. What Fluid Field could reuse or integrate

### 6.1 FF wind driving Procedural Grass blades

-   This is the ROADMAP's optional "grass bending" side track (ROADMAP.md:87), done through Procedural Grass's generator rather than a new grass system. It is the plug point A design in section 4.3.
-   It is also cheap on the FF side: export the velocity SRV plus a small consumer CB (`GridMin`, `GridRes`, `CellSize`, edge fade) through a public accessor.
-   Vanilla grass can use the same accessor in `RunGrass.hlsl` later. That would complete grass-collision.md's suggestion (section 6) that grass read FF's velocity layer instead of its own texture.
-   **Weather wind:** Procedural Grass's global wind is user-set (section 4.1). FF's planned weather-wind input (fluidninja-live2.md, section 2.2) could also feed Procedural Grass's `windSpeed`/`windAngle`, so grass, trees and the field agree.

### 6.2 Grass as drag or obstacle for FF wind and fog

**Sources** (all GPU, all around the player):

-   `GrassDensity` (t7/t71): uint blade count per texel over the occlusion window. Full coverage is `grassAOParams.z`, the size is `grassAOParams.x`, and the centre and extent are `occlusionParams.xy` / `occlusionHalfExtent` (`PGrassDensityGatherCS.hlsl:204-225`).
-   `GrassPresence`: grass id per sample. Its min corner, spacing and size are in `grassPresenceParams`.
-   Per-type `height` from `GrassTypes` b9.
-   `grassAOParams.w`, the canopy height.
-   `TerrainCanopyTypes` (t64) for the distant ring.

**Use.** A per-cell canopy drag term `v *= exp(−k · density · min(1, canopyHeight / layerHeight) · dt)` in FF's advection or force pass. For fog (Phase 4), the same density can hold ground fog inside the canopy or slow it there.

**Resampling.** These windows re-centre and are not toroidal, so FF would resample them into its grid with one small compute pass per frame.

**Needs from Procedural Grass.** A C++ accessor for the SRVs and window parameters, since these are not globally bound.

### 6.3 Static obstacles and the riverbed: TopDownOcclusion

`TopDownOcclusion` rasterises world geometry top-down into the highest and lowest Z per texel, max/min-pads it, and can also write the topmost "grass surface" record with its normal (`TopDownOcclusion/ObjectCapture.hlsl:1-98`, `OcclusionPadCS.hlsl`). That is **R1 Prototype A** (a top-down depth capture), already built and debugged in CS.

-   **For D7 stage 4 (statics),** it is a GPU alternative or complement to downward raycasts. Rocks, logs and bridges give obstacles (highest Z) and overhang clearance (lowest Z).
-   **Differences from what FF needs:**
    -   The window is re-rasterised around a centre rather than toroidal, and its refresh cadence is unknown.
    -   Its object set is chosen for grass occlusion; "Occluders drawn: %u" hints at a filtered list.
    -   Riverbed meshes and water surfaces are not included.

Extend it, for example with a toroidal mode or an FF-sized window, rather than writing a second capture.

### 6.4 Other reusable pieces

-   **The terrain lift map** (`PGrassCommon.hlsli:49-53`, `PGrassTerrainLiftCS.hlsl:90-94`) uses the same toroidal scheme as FF's `Common.hlsli`: a world cell maps to `cell & (Dim − 1)`, and the origin is kept for this frame and the last. This adds weight to grass-collision.md's proposed shared `ScrollingGrid.hlsli`.
-   **The LAND quadrant cache** (17² heights per 2048-unit quadrant with mesh-Z anchoring, section 2.2) overlaps D7 stage 1. A shared LAND height cache would serve both features. Compare FF's in-progress code against the author's before duplicating it.
-   **The shared `HiZPyramid`** (live depth) could cull FF's fog and smoke volumes.
-   **The collision-cache pattern** (sample once per blade, pack f16, apply in the VS) is the model for any FF consumer with many vertices.

### 6.5 Risks

**Register slots.** FF on `ff/grid-core` binds only during its own dispatches (b0, t0…, u0…) and leaves nothing bound globally, so there is **no conflict today**. Any FF _consumer_ binding must avoid these Procedural Grass slots:

| Stage                 | Slots used by Procedural Grass (or by the shaders it shares)                                                                                                                                             |
| --------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Generator CS          | b0, b7, b8, b9, b10, b11; t0, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t50, t64, t66, t100; u0, u1; s0. Also the slots of the included Terrain Shadows and Cloud Shadows headers, not listed here. |
| VS                    | b8, b9 (b10 for depth and Mid), t0, t1, t66                                                                                                                                                              |
| PS                    | b8, b9; t14, t66, t71, t73, t74, t75, t76; s0, s13, s14. Plus the slots of Skylighting, LLF, SSS and Wetness.                                                                                            |
| TerrainCanopy CS      | t2, t8, t14, t45, t64, t73, t74, t76; u0; s0, s14                                                                                                                                                        |
| TerrainLift CS        | t0, t1; u0, u1, u2                                                                                                                                                                                       |
| DensityAO PS          | t0, t1, t3, t5, t6; u0                                                                                                                                                                                   |
| Vanilla `RunGrass` VS | b2, b7, b8; t2 (Grass Optimizations `InstanceExtras`); t100 / b5 (Grass Collision)                                                                                                                       |

Two specific conflicts follow:

-   FF's `FluidFieldCB` sits at **b0**, which collides with the generator's `GrassWorkRange`. A consumer CB for the generator needs its own slot (b12 or b13, after checking what CS globally binds there; grass-collision.md notes b12 is used by the Grass Collision CS).
-   The FF velocity SRV needs a free t slot. The t100 range is used, so the t101–t127 range is worth checking with `hlslkit-buffer-scan`.

**Other risks:**

-   **Culling reach.** FF offsets beyond `grassHiZBounds.z` will cause pop-in at tile edges. Clamp in the shader and feed FF's maximum speed into the CPU reach.
-   **Grass Collision behaviour change.** The build switches vanilla grass under Grass Optimizations to smooth recovery (section 2.3). FF work on Grass Collision should rebase onto that refactor rather than on the repo version.
-   **Base drift.** The build predates #2697, #2757 and #2825. Merging Procedural Grass into this branch needs a careful three-way merge of `RunGrass.hlsl`.
-   **Experimental status.** The build is `Version = 1-0-0` with an unknown stage. The settings and blade layout may still change, so FF should depend only on a small accessor surface.

### 6.6 Open questions for the author

Status after the decompile (section 7). **Answered** means the build itself answers the question; the author may still intend different behaviour, so the remaining part of each question is kept.

1.  **Answered (7.1, 7.2).** Where in the frame does generation run, relative to CS `EarlyPrepass` and `Prepass`? What does the `Main_RenderShadowmasks_UpdateCamera` hook do? Are there other hooks?
    -   Generation runs inside the `Main::RenderShadowmasks` call-site thunk. That is after the engine depth prepass and before CS `Prepass`.
    -   There is one more patch: a 7-byte NOP in `Main::RenderDepth`.
    -   _Still to ask:_ why this site was chosen, and whether the NOP should stay active when the feature is disabled.
2.  **Answered (7.6).** Is wind ever taken from the weather or sky wind, or only from `WindSpeed`/`WindAngle`? How is `grassHiZBounds.z` (wind reach) computed?
    -   Wind comes only from the settings.
    -   The reach is `|WindSpeed| × nearHiZRadius × 0.8748`.
    -   _Still to ask:_ is a weather link planned?
3.  Open: would you accept a per-blade 2D wind offset? Either a new `Blade` field, or folding external wind into the collision cache. Which tiers should receive it (Low and Far facing only)?
4.  Open: is a length-preserving bend wanted for strong gusts?
5.  **Answered (7.2).** How does "Toggle Vanilla Grass Rendering" hide vanilla grass, and what is the default? Is Procedural Grass meant to replace vanilla grass eventually?
    -   While the feature is enabled, it calls the engine's `ToggleGrass` console function whenever `BGSGrassManager::enableGrass` is true. Vanilla grass is therefore always off.
    -   No code turns it back on.
    -   _Still to ask:_ is the one-way behaviour intended, and where is the string used? No exported function references it.
6.  **Mostly answered (7.3).** How does the feature behave on loading screens and worldspace changes: quadrant cache, top-down window, terrain lift map, collision history?
    -   The top-down capture is dropped on a worldspace change, in interiors, on `GameLoaded` and when the loaded cells change.
    -   The far LAND cache is flushed by a generation counter.
    -   The near LAND cache is validated only by pointer equality.
    -   _Still to ask:_ is pointer-only validation safe across a detach and reattach?
7.  **Partly answered (7.5).** Could TopDownOcclusion become a shared service with a toroidal mode, configurable extent and object filter, so FF can use it for statics (D7 stage 4)? Which objects does it draw, and how often is it refreshed?
    -   It draws every non-actor reference with loaded 3D within 1.5 × the half extent, after a few flag filters.
    -   It fully redraws every time the snapped centre moves.
    -   _Still open:_ the shared-service question, now with a Z band added.
8.  **Partly answered (7.4).** Could the LAND quadrant cache and `TerrainHeightMap` be shared with FF's D7 stage 1 map?
    -   The near cache is keyed by cell and copies `loadedData` heights with an anchor.
    -   The far cache parses VHGT from the plugin files on a thread pool.
    -   _Still open:_ would you split out a shared landscape-height service?
9.  Open: would you expose `GrassDensity` and `GrassPresence` and their window parameters through a C++ accessor, for FF drag and fog?
10. Open: is the source going to be published, for example as a PR or branch, so FF can build against it rather than a binary?

## 7. C++ implementation (from the decompiled build)

**Method.** The build's `CommunityShaders.dll` was decompiled with Ghidra using the shipped full PDB, so the function names are real. 719 functions from the Procedural Grass sources were exported (`ProceduralGrass.cpp` plus `ProceduralGrass/*.cpp`). This was done under D9 as amended: the output is reference only, and only short snippets are quoted here.

**Citations** take the form `decomp:<File>.c` + function + address, because line numbers in the export are not stable.

**Reading caveats:**

-   Ghidra applied a `ProceduralGrass::Settings` layout that is **off by one field**. For example, `LoadSettings` stores `"WindSpeed"` into the field Ghidra calls `windAngle`, and `"OcclusionHalfExtent"` into `spatialFreq` (`decomp:ProceduralGrass.c` `LoadSettings` @ 1804ebd50). Settings fields below are named by their JSON key, not by Ghidra's label.
-   Several small functions are inlined or tail-called: `PostDepthRendering` appears inlined into the hook thunk, and `GameLoaded` is a 16-byte stub. Call order is taken from the non-inlined copies.

### 7.1 Feature lifecycle

**Overrides seen in the export:**

-   `GetName`, `GetShortName`, `GetDisplayName`, `GetFeatureSummary`
-   `SetupResources`, `ClearShaderCache`
-   `DrawSettings`, `SaveSettings`, `LoadSettings`, `RestoreDefaultSettings`
-   `DataLoaded`, `GameLoaded`, `PostPostLoad`

No `Reset`, `Prepass`, `EarlyPrepass`, `Load` or `ReflectionsPrepass` was found. A trivial override could have been folded away, so this is **UNVERIFIED**, but none of the per-frame work goes through these callbacks.

**What each callback does:**

-   **`PostPostLoad`** installs the two engine patches described in 7.2 and logs `[Procedural Grass] Installed hooks` (`decomp:ProceduralGrass.c` `PostPostLoad` @ 1804da900).
-   **`DataLoaded`** calls the vanilla `ToggleGrass` function once, guarded by a one-shot flag (`decomp:ProceduralGrass.c` `DataLoaded` @ 1804da860). Ghidra names the flag `grassCellCache.ready.field_0x31`, which is probably a typing error. **UNVERIFIED.**
-   **`GameLoaded`** (@ 1804da850) is a 16-byte stub. Ghidra shows `TopDownOcclusion::Invalidate`'s body for it, which releases captured geometry, clears grass-surface quadrants, bumps the revision and resets `capturedWorldspace`. It presumably tail-calls `Invalidate`. **UNVERIFIED.**
-   **`SetupResources`** creates all the textures and buffers, compiles the support shaders, and runs `TopDownOcclusion`'s resource setup (7.3).

**Where per-frame work runs.** Everything up to and including Low and Far generation runs in the `Main::RenderShadowmasks` call-site thunk. The order comes from `decomp:ProceduralGrass.c` `PostDepthRendering` @ 1804d7860, which is also inlined into `Main_RenderShadowmasks_UpdateCamera::thunk` @ 1804d7bd0:

1.  Call the original function, then force vanilla grass off (7.2).
2.  If the feature is disabled, the player is missing or the map menu is open, only copy `kMAIN` depth into `kPOST_ZPREPASS_COPY`, then return.
3.  Save the render targets, viewports and shaders that it changes.
4.  `TopDownOcclusion::Render`: gather if needed, redraw if needed, pad.
5.  `GetVisibleQuadrants` (CPU): the grass-cell cache frame step, near and far quadrant lists, and the presence rebuild.
6.  `HiZPyramid::Build` from live depth.
7.  `PostDepthRenderPrep`: `GrassCollision::Update`, then fill and upload `GrassGlobals`, including wind (7.6).
8.  `GenerateBlades(ctx, true)`: High and Mid generation.
9.  `RenderDepth`: High and Mid depth prepass into the main depth buffer.
10. Rebuild Hi-Z if it used live depth. If that fails, zero the Hi-Z params in `GrassGlobals`.
11. `CopyDepthBuffer`: `CopyResource(depthStencils[7], depthStencils[0])`, so the post-Z-prepass copy now contains grass.
12. `UpdateTerrainLift`.
13. `GenerateBlades(ctx, false)`: Low and Far generation.
14. If Terrain Blending is on, `TerrainBlending::MergeSceneDepthIntoBlend` and rebind t17.
15. Restore state.

**Drawing.** Drawing goes through three public non-virtual entry points:

-   `DeferredRendering` @ 1804dab90: `DeferredRenderPrep` plus `RenderGrass` for High, Mid and Low into the G-buffer.
-   `ForwardRenderFar` @ 1804dbb30: Far forward, the terrain canopy, and another depth copy.
-   `DarkenTerrainUnderGrass` @ 1804dad00.

None of them has a caller inside the exported Procedural Grass code. They are presumably called from the build's `Deferred.cpp`, which was not exported. **UNVERIFIED.**

**Consequence for FF.** Procedural Grass generates **before** CS `Prepass`, where FF steps its solver (`Deferred.cpp:249`). A Procedural Grass generator that reads FF wind would see the previous frame's field. That is the one-frame lag section 4.3 already assumed, and it is acceptable.

### 7.2 Engine hooks and patches

Every relocation found in the five decompiled files:

| Site (SE / AE ID, SE / AE offset)                                                                                         | Kind                                                       | Purpose                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| ------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `RelocationID(100422, 107140)` + `0x7B` / `0x69`. The ID is `Main::RenderShadowmasks` per `src/FrameAnnotations.cpp:342`. | `stl::write_thunk_call<…UpdateCamera, 0x05>` (5-byte call) | The whole per-frame pipeline in 7.1. The original callee runs first. The patched call is presumably the camera update the name refers to. **UNVERIFIED.**                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| `RelocationID(100421, 107139)` + `0x523` / `0xA3F`. The ID is `Main::RenderDepth` per `src/FrameAnnotations.cpp:940`.     | `REL::safe_fill(…, 0x90, 7)`: 7-byte NOP                   | **Removes an instruction in the engine's depth prepass, permanently, even with the feature disabled.** The bytes were not disassembled. The likely target is the engine's own `kMAIN` → `kPOST_ZPREPASS_COPY` copy. **UNVERIFIED.** The evidence: both paths of the thunk do that copy themselves, the disabled path unconditionally and the enabled path after the grass depth prepass (`decomp:ProceduralGrass.c` `CopyDepthBuffer` @ 1804d77d0). If this reading is right, every CS consumer of `kPOST_ZPREPASS_COPY` now gets the copy at `RenderShadowmasks` time, and it includes High and Mid grass depth. That matches the "POST_ZPREPASS_COPY (stale fallback)" log in section 2.3. |
| `RelocationID(22391, 22871)`                                                                                              | function call                                              | The vanilla `ToggleGrass` (TG) console handler (`decomp:ProceduralGrass.c` `ConsoleFunc_ToggleGrass` @ 1804d7b40).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| `RelocationID(514783, 400936)`                                                                                            | data                                                       | Default `TESLandTexture*`, the same as `src/TruePBR.cpp:1225` (`GrassCellCache::ParseLandscape`, `GetCellCache`, `PGrassCommon::GetDefaultLandTexture`).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| `RelocationID(67819, 69161)`                                                                                              | function call                                              | The `BSFixedString` constructor, used for the `"BSX"` lookup in `TopDownOcclusion::CollectFrom`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |

**Hooks and IDs that are absent:**

-   No `write_vfunc`, no detour, no other `write_thunk_call`.
-   None of the precipitation-occlusion chain: no `RelocationID(35560, 36559) + 0x3A1`, no `SetViewFrustum` `25643 + 0x5D9`.
-   No `REL::ID(564236)`.

**Hiding vanilla grass.** At the top of the thunk:

```cpp
if (settings.Enabled && grassManager && grassManager->enableGrass) ConsoleFunc_ToggleGrass();
```

-   This flips `BGSGrassManager::enableGrass` off through the engine's own console path, every frame it finds it on.
-   No path calls it to turn grass back on. Disabling Procedural Grass at runtime leaves vanilla grass off until the user types `TG`. **UNVERIFIED** that nothing outside the export re-enables it.
-   The `feature.procedural_grass.toggle_vanilla_grass` key is not referenced by any exported function.

**Runtime targeting.** Offsets are chosen by `IsAE()` only. The VR ID equals the SE ID (`_vrID = _seID`), and VR uses the SE offsets. CS's own hooks at nearby sites use `VersionedRelocation::Select` with separate VR offsets (`src/Deferred.h:159`). VR correctness of both patches is therefore **UNVERIFIED**.

The structure offsets are VR-aware through `REL::Relocate(se, ae, vr)`:

-   `NiAVObject` flags: 0xF4 / 0x10C
-   `NiNode` children: 0x110 / 0x138
-   `BSShaderProperty` slot: 0x120 / 0x160
-   `BSTriShape` triangle count: 0x158 / 0x198

### 7.3 GPU resources, lifetime and loading screens

**Created in `SetupResources`** (`decomp:ProceduralGrass.c` `SetupResources` @ 1804e6830, `CreateGrassTextures` @ 1804e8fb0, `CreateIndexBuffers` @ 1804e9cf0):

| Resource                                                           | Size and format                                                                            |
| ------------------------------------------------------------------ | ------------------------------------------------------------------------------------------ |
| `PGrass::MaterialDetail`                                           | 64×64 × 512 slices, `R8G8B8A8_UNORM`, SRV                                                  |
| `PGrass::GrassDensity`                                             | 256², `R32_UINT`, UAV                                                                      |
| `PGrass::DistantAmbientLUT`                                        | 32², `R16G16B16A16_FLOAT`                                                                  |
| `PGrass::TerrainLift`, `TerrainLiftSurface`, `TerrainLiftMeasured` | 512² each, `R32_FLOAT`                                                                     |
| `PGrass::GrassPresence`                                            | 177², `R8_UINT`, no UAV. 177 = 11 quadrants × 16 + 1, i.e. ±5 quadrants around the player. |
| constant buffers                                                   | `GlobalsCB`, `TypesCB`, `GeneratorTypesCB`, `QuadrantsCB`, `WorkRange`                     |
| index buffers                                                      | the eight fixed blade-index buffers                                                        |

**Created lazily:**

-   `PGrass::TerrainCanopyTypes`: 1024², `R32_UINT`, in `UpdateTerrainCanopy` @ 1804e1240.
-   `PGrass::TerrainDarkeningSceneCopy`, in `DarkenTerrainUnderGrass`.
-   `PGrass::ObjectSurfaces` and `ObjectSurfaceDepth`, in `TopDownOcclusion::CreateGrassSurfaceResources` @ 180501120.
-   Per-tier `PGrass::Blades`, `IndirectArgs`, `BatchArgs`, `IndirectArgsStaging`, `QuadrantHeights`, `QuadrantGrassCells`, `VisibleWork`, `TileHeightBounds` and `OccupancyRows`, in `PGrassRenderer<N,M>`.

`PGrassRenderer::EnsureBladeCapacity` grows the blade buffer on demand. `ResetBladeCapacity` exists but has no caller in the export, so the buffer only grows. **UNVERIFIED.**

**TopDownOcclusion resources** (`decomp:TopDownOcclusion.c` `SetupResources` @ 180504800):

-   `TopDownOcclusion::HeightMap` and `HeightMapLow`: `mapDim²`, `R32_FLOAT`, SRV + RTV + UAV.
-   `HeightMapTmp` and `HeightMapLowTmp`: `R32_FLOAT`, SRV + UAV.
-   `HeightCB` (dynamic, updated per draw) and `PadCB`.
-   `snapDim` is forced to 256 in `SetupResources`.
-   The default `mapDim` is not visible in the exported code. The debug UI prints it ("Occlusion map: %s %u x %u"). **UNVERIFIED.**

Freeing happens only in the destructors and when shaders are recompiled (`ClearShaderCache` → `TopDownOcclusion::Invalidate`). No GPU resource is freed or recreated on loading screens.

**Loading screens, cell changes and worldspace changes** (the CPU-side state that is reset):

| State                              | Reset trigger                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| ---------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| TopDownOcclusion captured geometry | Dropped and re-gathered on each of these: interiors (`TES::interiorCell`), a worldspace with flags `0x60` (`kNoSky \| kFixedDimensions`), a worldspace change, `GameLoaded`, a change in the loaded-cell stamp, the window centre moving at least `max(halfExtent/4, one snap texel)` from the last capture, and half-extent edits. The stamp is an FNV-1a hash of each `gridCells` cell pointer, its `loadedData` pointer and `IsAttached()` (`decomp:TopDownOcclusion.c` `Render` @ 1805059e0). |
| Near LAND cache (`grassMapCache`)  | Entry rebuilt when its `TESObjectLAND*` or `loadedData*` differs. LRU-evicted above 256 entries, never evicting the current frame's (`EvictGrassMapCache` @ 1804fdd00). Fully cleared only from the settings UI and `RestoreDefaultSettings`. **Its key is cell (x, y) without the worldspace** (7.7).                                                                                                                                                                                            |
| Far LAND cache (`GrassCellCache`)  | On a root-worldspace change: `generation` is bumped and the plugin file list is rebuilt, so in-flight jobs from the old worldspace are presumably discarded in `DrainCompleted`. **UNVERIFIED.** LRU above 2048 cells (`EvictUntouched`).                                                                                                                                                                                                                                                         |
| Terrain lift                       | Self-validating per texel (section 2.2). No C++ reset was found.                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| Wind history (`previousWind*`)     | Reset when the shader timer jumps by less than 0 or more than 0.1 s (`PostDepthRenderPrep` @ 1804d5eb0).                                                                                                                                                                                                                                                                                                                                                                                          |

**Engine-owned resources held across frames: yes, in TopDownOcclusion.** Each `CapturedGeometry` keeps:

-   an `NiPointer<BSGeometry>` (`IncRefCount`)
-   AddRef'd `com_ptr`s to the geometry's own engine vertex and index buffers, taken from `rendererData`
-   a snapshot of the world transform

These live until the next re-gather (`decomp:TopDownOcclusion.c` `CollectFrom` @ 180503080, `GatherGeometry` @ 180502f20). The refcounts keep this memory-safe, but it is exactly what FF's rule forbids. Any FF reuse of this code must re-fetch the geometry each time it draws, or hold only reference handles.

### 7.4 Landscape data

There are **two independent paths**.

**Near path: loaded cells, render thread.** `ProceduralGrass::RebuildNearQuadrants` @ 1804fc830 runs whenever a visibility stamp changes.

-   It walks `TES::gridCells` and keeps only cells with every element of the chain present: exterior data, LAND, `loadedData`, a quadrant mesh, and the mesh flag `0x1000` set on `NiAVObject` flags. These are the debug counters `cells → exterior → land → loadedData → mesh → preProcessed`. The flag check can be switched off from debug.
-   For each quadrant it calls `GetCellCache(land, cellX, cellY, quadrant)` @ 1804fed70:

    -   **Key:** `(cellX << 32) | cellY` in an `unordered_map`, with no worldspace in the key.
    -   **Heights:** copies `loadedData->heights[4][289]` plus an anchor:

        ```cpp
        anchor = loadedData->heightExtents.x - min(heights[0..3][*]);   // stored height = raw + anchor
        ```

        Absolute Z is anchored to `heightExtents.x`. The debug panel shows "LAND anchor applied / mesh world Z" to compare against the mesh's world Z. This is the absolute-Z question in R1 (open question 1 there). The author's answer is an anchor to `heightExtents.x`. FF can reproduce it and still run the `TES::GetLandHeight` check from D7 stage 1.

    -   **Per-vertex grass type:** read from `loadedData->quadTextures[q][0..4]` and `percents[q][vertex][layer]`, with `defQuadTextures[q]` as the base. A texture only "grows grass" above `fTexturePctThreshold:Grass × 255`; the INI value is read and defaults to 0.005. Types are resolved per LTEX via `ResolveGrassType`, then slope-limited.
    -   **Also stored:** per-quadrant min and max height, a 16×16 occupancy bitmask, and a `cacheVersion`.

-   **Tier membership** is decided by Chebyshev distance in 2048-unit quadrants from the **player** position:

    | Tier | Distance | Cap |
    | ---- | -------- | --- |
    | High | < 3      | 25  |
    | Mid  | 1–4      | 81  |
    | Low  | 2–6      | 169 |

    This confirms that the `PGrassRenderer<N,M>` template's N is the per-tier quadrant capacity.

-   **No terrain-cell lock is taken.** `REL::ID(564236)` does not appear anywhere.

**Far path: plugin files, worker threads.** `GrassCellCache` (`decomp:GrassCellCache.c`):

-   **Thread pool.** A `BS::thread_pool` with `clamp(hardware_concurrency / 4, 1, 4)` threads, created in `BeginFrame` @ 1804e00f0.
-   **Request.** `Request` @ 1804df100 FNV-hashes `(x, y)`, records the cell in `pending`, captures the current `generation`, and `detach_task`s a job.
-   **ReadCell** (@ 1804de330):
    -   It walks the loaded plugins **from last to first**, so the winning override is found first.
    -   For each plugin it does `TESFile::Duplicate(file, 0x4000)`, then `SeekCell(worldspace, x, y)`, then `SeekLandscapeForCurrentCell`.
    -   It then calls `ParseLandscape` @ 1804dcb90, which decodes `VHGT` itself: a 1096-byte record holding the offset and 33×33 int8 deltas, accumulated by rows, ×8. It also reads `BTXT`, `ATXT` and `VTXT`.
    -   Each result is a 5976-byte `CellGrass`: 4 × 289 floats of heights, initialised to −3e38, plus 4 × 289 grass ids.
-   **Completion.** Results queue under `completedMutex` and are moved into the map by `DrainCompleted` on the render thread. `EvictUntouched` keeps at most 2048 cells, about 12 MB.
-   **What this gives.** Absolute heights for **any** cell in the worldspace, loaded or not. It is used for the Far tier out to `FarRadius`.

**Compared with R1 and D7 stage 1.**

-   The **near path** is almost exactly R1 Prototype B's "bed from landscape heights": same source, same 4 × 17² layout. It differs in three ways:
    -   it is a Procedural Grass-owned singleton keyed by cell
    -   it carries grass-type data
    -   it is gated on the preprocessed-mesh flag, which FF does not want for a riverbed (a cell without the flag still has valid heights)
-   **FF could share it** if it is split into a feature-neutral `LandscapeCellCache` that holds heights, min/max and the anchor, keyed by `(worldspace, x, y)`, with Procedural Grass's type data layered on top.
-   **Calling into Procedural Grass directly is not advisable:**
    -   `GetCellCache` is a member of the feature
    -   it runs only while Procedural Grass is enabled
    -   it is not locked
    -   it uses pointer-only validity
-   **The far path is not needed for FF's grid**, which is 4096 units and inside the loaded cells. Its `VHGT` decoder is a useful reference for a view-independent fallback (open question 1 in R1, and cells that load late), and for checking FF's anchored heights against file data.

### 7.5 TopDownOcclusion in detail

**It is its own mini renderer, not the precipitation-occlusion path.** No `Precipitation::SetupMask`/`RenderMask`, no `BSShaderAccumulator`, no `SetViewFrustum` hook, and no use of `RelocationID(35560, 36559) + 0x3A1`. It therefore never touches the Upscaling → Skylighting chain at that site. It does not need Skylighting's alpha-group pool growth. It does not depend on Upscaling's `dynamicResolutionLock`. No Upscaling symbol appears in the export. Skylighting is referenced only to bind its probe array in `DeferredRenderPrep`.

**Gather** (`GatherGeometry` @ 180502f20, lambda `_Do_call` @ 1805028d0, `CollectFrom` @ 180503080). This runs on the render thread inside the thunk.

1.  It calls `TES::ForEachReferenceInRange(player, 1.5 × halfExtent, …)`.
2.  It skips references that are disabled, deleted (flag `0x20`), without 3D, or actors (form type `0x3E`). The base type `0x22` (`STAT`) only sets a flag used by grass-surface resolution, so **activators, furniture, trees, flora and clutter are all captured**.
3.  `CollectFrom` recurses through `NiNode` children and skips `NiAVObject` objects flagged hidden.
4.  It drops geometry when any of these holds:
    -   world-bound radius ≤ 8
    -   an ancestor's `BSX` flags match `0x3D75`: animated, ragdoll, add-on, editor marker, dynamic, needs-transform-update, magic particles, lights, breakable, searched-breakable
    -   the shader-property flags include `kMultiTextureLandscape | kLODLandscape | kNoLODLandBlend`, so **landscape is excluded**
    -   there is no position stream
5.  The captured list is sorted by input layout.

**Draw** (`Render` @ 1805059e0):

-   **Clears.** `HeightMap` is cleared to −1e30 and `HeightMapLow` to +1e30.
-   **Pipeline state:**
    -   both bound as RTVs with **no depth buffer**
    -   a MAX/MIN blend state (`maxBlend`), no culling
    -   viewport `mapDim²`, triangle list, R16 indices
-   **Per draw.** One `DrawIndexed` per captured geometry, with a per-draw `HeightCB` holding the world 3×4 matrix, `WindowCentre` and `HalfExtent`.
-   **Projection.** The VS maps world XY linearly into the window, `(xy − centre) / halfExtent`, with a constant z of 0.5. There is **no near or far plane and no Z band**: every triangle in the window contributes to max and min Z (`pgrass:Shaders/TopDownOcclusion/ObjectCapture.hlsl`).
-   **After drawing:** `RenderGrassSurfaces` writes the topmost grass-surface record with a real depth test, then `PadMaps` runs a separable max/min pass. Its radius is `mapDim × OcclusionPadding / (2 × halfExtent)` texels.
-   State is saved and restored around all of this.

**Window and update cadence:**

-   **Snapping.** The centre is the camera position (`CameraPosAdjust`) snapped to `2 × halfExtent / snapDim`. At the defaults below, that is 80 units.
-   **Reuse.** `CanReuseRenderedMaps` skips work only if the snapped centre, extent, padding, `mapDim`, `snapDim` and geometry revision all match.
-   **Otherwise a full redraw of every captured draw.** That happens on every 80-unit step of camera movement. It is **not incremental and not toroidal**.
-   **Settings defaults.** `OcclusionHalfExtent` 10240 (UI slider 1024–16384), `OcclusionPadding` 12, `OcclusionClearance` 100. These are read through the shifted field mapping, so **UNVERIFIED**.
-   **Cost.** The only cost hint is the debug counter "Occluders drawn: %u" (`lastDrawCount`). The gather radius is 15360 units by default, so the draw count is likely to be in the thousands. **UNVERIFIED** because it was not measured.

**Can FF use it for D7 stage 4?**

**Consuming Procedural Grass's live output: no.**

-   `HeightMap` is the top surface, so bridges, docks and canopies block flow. That fails R1's "bridges never block flow" criterion.
-   `HeightMapLow` is the underside of the lowest geometry, not the riverbed.
-   Landscape is excluded.
-   The window is camera-centred, about 20k units wide at `mapDim` resolution, and exists only while Procedural Grass is enabled and the player is in an exterior.

**Reusing the code with changes: plausible, and cheaper than R1 estimated for Prototype A**, because it avoids the precipitation path and all of R1's risks for that path. Changes needed:

1.  **Z band.** Add a PS clip of `worldZ > waterSurfaceZ + clearance`, taking the surface from FF's water map (an SRV or a per-region constant). The MAX target then holds the **highest static below the water line**, so bridges are skipped by construction. The MIN target can be dropped.
2.  **Combination.** Combine with FF's LAND bed as `max(LAND, capture)`, since landscape is excluded here.
3.  **FF-sized toroidal grid.** Use 128² texels over 4096 units, render only the exposed strips (scissored viewports) into toroidally addressed texels, and drop the snap-and-full-redraw scheme.
4.  **Object filter.** Use statics, movable statics and perhaps trees. Drop flora, activators and clutter, and keep the `BSX` and marker filters.
5.  **No held engine objects.** Do not keep `NiPointer`/`com_ptr`s to engine geometry between updates: re-walk references for the strip being drawn, or keep reference handles only.
6.  **Interiors.** It skips interiors entirely, so FF still needs Prototype B's rays or cell water there, as R1 already says.
7.  **Threading.** Keep the reference walk on the thread that owns the scene graph. Procedural Grass does it on the render thread without locks; whether that is safe is **UNVERIFIED**.

**Verdict.** This does not change D7. B (rays) stays the plan for stage 4. TopDownOcclusion is a better fallback A than the one R1 designed: a self-contained ortho capture with a known filter. If rays fail, fork it with the changes above rather than going through `Precipitation`.

### 7.6 Wind

**Where wind reaches the GPU.** `PostDepthRenderPrep` @ 1804d5eb0 fills `GrassGlobals` (uploaded to `PGrass::GlobalsCB`, b8) from the settings only:

-   `windSpeed` = `WindSpeed`
-   `previousWindSpeed`
-   `windDir` = `(cos, sin)(WindAngle)`, computed with `sincosf` in `LoadSettings` and `RestoreDefaultSettings`
-   `windAngle` = `atan2(windDir)` wrapped to [0, 2π)
-   `previousWindDir`
-   `windRotationScale` = `0.5 × WindSpeed³`. **UNVERIFIED**, from the CB offset 0xF8.
-   `grassHiZBounds.z` (wind reach) = `|WindSpeed| × nearHiZRadius × 0.8748`. `nearHiZRadius` is derived from the grass-type heights when the types are rebuilt.

**No weather link.** No Procedural Grass file references `RE::Sky`, `TESWeather` or any weather manager. Wind is a user constant, confirming section 4.1.

**For plugging in FF's wind texture:**

-   **Binding.** Generation happens inside the Procedural Grass thunk, so an FF SRV must be bound by Procedural Grass itself, for example in `GrassCollision::BindProceduralGrassGenerationResources` @ 180482440, which already binds Grass Collision for the generator. Use a free slot (section 6.5).
-   **Wind reach.** It is CPU-computed from `WindSpeed`. An FF offset must be clamped to it, or FF's maximum speed must be added to it, or blades pop at Hi-Z tile edges.
-   **Previous-frame values.** They are reset on timer jumps. An FF previous-velocity input should follow the same rule.
-   **Frame lag.** FF's field will be one frame old (7.1).

### 7.7 Other observations (neutral)

-   **Permanent engine patch.** The 7-byte NOP in `Main::RenderDepth` is installed unconditionally. The disabled-feature path compensates by doing the depth copy itself.
-   **One-way vanilla-grass toggle (7.2).** This also changes the state of the user's own `TG` console command.
-   **Near LAND cache without worldspace in the key**, with validity by pointer equality on `TESObjectLAND*` and `loadedData*`. After a detach, the allocator could reuse the same addresses, so stale heights could in theory match. **UNVERIFIED.**
-   **No terrain-cell lock.** `gridCells` and `loadedData` are read on the render thread without `REL::ID(564236)`, which `src/Features/UnifiedWater.cpp:598` uses for the 1.7.99+ terrain-cell map.
-   **File reads on worker threads.** The far cache reads plugin files through `TESFile::Duplicate` on its own threads. This mirrors the engine's own thread-safe file handles, but concurrent use with engine loading is **UNVERIFIED**.
-   **Engine geometry held across frames** by TopDownOcclusion (7.3), and the reference walk runs on the render thread (7.5).
-   **Hardcoded offsets** selected by `IsAE()` only, with VR falling back to SE (7.2).
-   **Player versus camera.** Quadrant selection uses the **player** position, while the occlusion window uses the **camera** position. In free-camera or long third-person views, the two can disagree.
