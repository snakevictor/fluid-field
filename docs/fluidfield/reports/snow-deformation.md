# Snow Deformation: read-only report for Fluid Field

Oct 13, 2026 · Phase 0 groundwork report (see `docs/fluidfield/ROADMAP.md`, "What to borrow" and Phase 0)

## TL;DR

- **Snow Deformation is not on `feature/fluid-field` and not merged upstream.** It exists in three places outside this branch:
  1. **Upstream PR #2751** (open, unmerged): https://github.com/community-shaders/skyrim-community-shaders/pull/2751. Head `faf938777381840f4f3f90a45d9572c7ce4b4696` on branch `snow/trails-refile` of the contributor fork. It refiles closed PR #2659 (https://github.com/community-shaders/skyrim-community-shaders/pull/2659, head `722c602e7b`). This is the "trails only" slice.
  2. **Contributor fork** https://github.com/josefplyr1/skyrim-community-shaders-dynamic-snow-deformation (public). `main` = `cf2facf84eb7a83187d71e2f94161733b9d62ca7` (2026-08-25) holds the full system, about 23k added lines. The `snow/00-base` … `snow/28-*` branches stack it for review.
  3. **Preview build**: a prerelease, https://github.com/community-shaders/skyrim-community-shaders/releases/tag/v1.9.0-pr2751. This is what the roadmap's "releases" link points at. It is a CI PR build, not a shipped feature.
- **The roadmap's core claim holds:** stamps *are* gathered from Havok collision shapes. The mechanism, though, is the **same `Util::GetShapeBound` + `BSVisit::TraverseScenegraphCollision` helper Grass Collision already uses on this branch** (`src/Utils/ActorUtils.cpp:7-88`, `src/Features/GrassCollision.cpp:89-107`). Each shape is reduced to a centre and a bounding radius. Snow Deformation does not read shape geometry, velocities or contact points.
- The only Snow Deformation code reachable from this checkout is the local tag `v1.9.0-pr2751` (`01ffb760c`, one commit behind the PR head) and doodlum's unrelated PoC branch `origin/snow-collision-poc` (`9af070c4a`), which reuses Grass Collision's texture to perturb terrain normals.

Code citations use `<sha>:<path>:<lines>`. "PR" means `faf93877` (PR #2751 head) unless stated. "Fork main" means `cf2facf8`. Fork code was read from a blobless clone in the session scratchpad. The repo itself was not modified.

## 1. Status

| Where | What | Completeness |
| --- | --- | --- |
| `feature/fluid-field` (this branch) | Nothing. No `src/Features/SnowDeformation*` and no `features/Snow Deformation/`. The snow hits from `git grep -i snow` are vanilla snow-shader plumbing: `src/ShaderCache.h:53-57`, `:596`, `:629-631`, `:685`, and the LOD snow brightness in `src/Features/LODBlending.h:16-31` | n/a |
| Local tag `v1.9.0-pr2751` → `01ffb760c` (3 commits on base `3dcaa4959`, an ancestor of HEAD) | Commits `61a4acd72` feat, `3eaaf3bd0` review fixes, `01ffb760c` refill fix | Same feature as the PR minus the perf rewrite |
| PR #2751 head `faf93877` | Adds `perf(snow): update the deformation map in place` (toroidal map, tiled stamping, cached actor shapes) | **Simulation only.** The map is built every frame, but the lighting shader's only use of it is a **debug overlay** (`faf93877:package/Shaders/Lighting.hlsl:1392-1393` → `SnowDeformation.hlsli:100-110`). It produces no visible trenches, displacement or normals. The ini is `Version = 0-2-0`, `Beta = True` (`faf93877:features/Snow Deformation/Shaders/Features/SnowDeformation.ini:1-3`) |
| Fork `main` `cf2facf8` | Full system: tessellated snow "shell" drawn as extra deferred geometry, statics height capture, spells/shouts/projectiles/hazards as inputs, persistent trench store in the SKSE co-save, accumulation, shadows | ini `Version = 0-65-0`, `Beta = True`, `Unreleased = True` (`cf2facf8:features/Snow Deformation/Shaders/Features/SnowDeformation.ini`). It is being upstreamed slice by slice through the `snow/NN-*` branch stack |
| `origin/snow-collision-poc` `9af070c4a` (doodlum, 2026-01-12) | A shader-only PoC. Lighting.hlsl samples Grass Collision's collision texture and bends landscape normals | Proof of concept: hard-coded constants, no feature class, no toggle |

Release metadata mismatch: the GitHub release's `target_commitish` is `faf93877` and its notes list all four commits. The **tag ref** (local and upstream `git ls-remote`) points at `01ffb760c`. The asset `CommunityShaders_AIO-2026-09-24T15-09Z.7z` was built after `faf93877`, so it probably contains the perf commit. This is unverified.

## 2. Where the code lives (PR #2751)

| File | Role |
| --- | --- |
| `src/Features/SnowDeformation.h` (253 lines) | Feature class, `Settings` (`:40-52`), `SettingsGPU` for the shared cbuffer (`:55-66`), `PerFrame` CS constants with 128 stamps (`:78-99`), resource members (`:101-108`), tile list (`:186-193`), actor shape cache (`:209-225`), prop scan state (`:231-244`), corpse latch (`:246-252`) |
| `src/Features/SnowDeformation.cpp` (365) | `SetupResources` `:15-69`, window advance `GetCommonBufferData` `:71-105`, `ApplyRangeSettings` `:107-119`, `BuildStampTiles` `:121-181`, `Prepass` `:183-312`, shader compile and cache `:314-349`, settings JSON `:351-365` |
| `src/Features/SnowDeformation/Stamping.cpp` (424) | `GatherStamps`: actors, props, trail anchors |
| `src/Features/SnowDeformation/TerrainData.cpp` (143) | Snow-tile detection on landscape and the two hooks |
| `src/Features/SnowDeformation/Menu.cpp` (58) | ImGui settings |
| `features/Snow Deformation/Shaders/SnowDeformation/DeformationUpdateCS.hlsl` (153) | `RingCS`, `RefillCS`, `StampCS`, `StampAllCS` |
| `features/Snow Deformation/Shaders/SnowDeformation/SnowDeformation.hlsli` (111) | Sampling helpers for Lighting.hlsl (`t101`) |
| Shared edits | `src/Feature.cpp`, `src/Globals.{h,cpp}` registration; `src/FeatureBuffer.cpp` appends `GetCommonBufferData(a_inWorld)`; `src/State.h` adds `SnowLandIsSnowMask = 0b111111 << 11`; `package/Shaders/Common/SharedData.hlsli:354-386` adds `SnowDeformationSettings` to `FeatureData` (b6); `Permutation.hlsli` mirrors the bits; `Lighting.hlsl` gets an include (`:870`) and the overlay call (`:1392-1393`); `.github/configs/shader-validation.yaml` adds the `SNOW_DEFORMATION` define; `en.json` adds 24 keys (all from `git diff 3dcaa4959 01ffb760c`) |

Fork `main` adds `Accumulation.cpp`, `LightningArc.cpp`, `Shadows.cpp`, `Shell.cpp`, `Spells.cpp` (2336 lines), `Statics.cpp` (2030), `TrenchStore.cpp`, `TrenchCoSave.cpp`, a generic `src/CoSave.{h,cpp}`, and 18 shaders (e.g. `SnowShell.hlsl` 2394 lines, `SnowHeightCapture.hlsl`). It also edits `Deferred.cpp`, `State.cpp` and `XSEPlugin.cpp` (`git diff --stat 4d668861 cf2facf8`).

## 3. How it hooks the engine and gathers stamps

### Hooks (PR)
- `TESObjectLAND` material setup is detoured: `stl::detour_thunk<SD_TESObjectLAND_SetupMaterial>(REL::RelocationID(18368, 18791))` (`faf93877:src/Features/SnowDeformation/TerrainData.cpp:133-143`, thunk `:105-118`). It is installed in `PostPostLoad`, after TruePBR's detour on the same function, so it sees the final per-quad materials. Per quad it caches a 6-bit "layer is snow" mask keyed by material pointer (`:37-72`). Snow is detected by diffuse-path substring (`grasssnow`, `snowpath`, …) with a fallback on Havok `MATERIAL_ID::kSnow`/`kSnowStairs` (`:12-35`).
- `BSLightingShader` vfunc 0x4 (`SetupMaterial`) is hooked with `stl::write_vfunc<0x4, …>(RE::VTABLE_BSLightingShader[0])` (`:142`, thunk `:120-131`). On landscape draws it writes the mask into `ExtraFeatureDescriptor` bits 11-16 (`:74-103`).
- These hooks serve only landscape snow detection. **There are no render-pass hooks and no Havok hooks.** The update runs in `Feature::Prepass()` (`SnowDeformation.cpp:183-312`). The window origin is advanced inside `GetCommonBufferData`, which is called from `State::UpdateSharedData`, so the cbuffer and texture agree within the frame (`:71-105`).
- Fork `main` adds one more hook, `BSLightingShader` vfunc 0x6 (`SetupGeometry`), which records snow statics for a top-down capture (`cf2facf8:src/Features/SnowDeformation/Statics.cpp:653-673`). It also adds `TESSpellCastEvent`, `TESDeathEvent` and `TESMagicEffectApplyEvent` sinks (`Spells.cpp:878-886`) and direct calls from `Deferred.cpp:312` (`DrawShell`), `Deferred.cpp:429` (`DrawLightningArcs`) and `State.cpp:123` (`CaptureShadowAtlas` in the shadowmask Utility pass).

### Relationship to Grass Collision
- **The PR does not piggyback on Grass Collision.** It has its own map, CS and stamps. It shares only the gathering idiom and the helper `Util::GetShapeBound` (`src/Utils/ActorUtils.cpp:7-24` on this branch). Grass Collision's equivalent loop is `src/Features/GrassCollision.cpp:45-153`.
- **The PoC branch does piggyback.** `9af070c4a:src/Features/GrassCollision.cpp:214-215` additionally binds Grass Collision's per-frame CB to PS `b10` and its collision texture to PS `t112`. `9af070c4a:package/Shaders/Lighting.hlsl:941-1070` re-declares that cbuffer, bilinearly reads the 512² clipmap (4096-unit window, Z range ±2048), builds a normal from 3 taps, and at `:2086-2089` reorients landscape `worldNormal` by it. Note: `b10` is already `BonesBuffer` in `package/Shaders/Common/Skinned.hlsli:9` (VS-side). The PoC registers are ad hoc.

### Stamp gathering (PR, `faf93877:src/Features/SnowDeformation/Stamping.cpp`)
- **Actors** (`:57-206`): the player plus `ProcessLists::highActorHandles` (`:193-199`), culled to the window (`:64-67`). Airborne living actors (`hkpCharacterStateType::kInAir`) are skipped (`:94-98`). The skeleton is walked with `BSVisit::TraverseScenegraphCollision` and `Util::GetShapeBound`, and `{bhkNiCollisionObject, radius}` is cached per actor for 30 frames or until the 3D root changes (`:101-118`). Each frame only the rigid-body centre of mass is re-read (`ShapeCenter`, `:21-35`, via `bhkRigidBody::GetCenterOfMassWorld` × `bhkWorld::GetWorldScaleInverse`). Shapes whose bottom is more than 40 units above the actor's Z are dropped, as are radii outside 4..128 (`:6-10`, `:132-135`).
- **Trails**: each shape keyed `(formID << 16) | index` stamps a capsule from the previous to the current XY position (`:137-151`). A jump of 256 units or more breaks the trail (`:14`). Corpses carve only while moving, with a settle latch (`:77-90`, `:153-165`, `:181-190`).
- **Loose props** (`:208-411`): base form types Misc, Weapon, Armor, Ammo, Book, Ingredient, Alchemy, SoulGem, Key, Light and MovableStatic (`:224-239`). Movement is gated on the 3D root's world translate, "Havok moves the scene graph every frame while the reference position lags" (`:246-259`). Ground comes from `TES::GetLandHeight` (`:267-269`), then the same shape walk. Shapes without a bound fall back to `root->worldBound` (`:312-341`). Cells are scanned one sixth per frame, and known movers are revisited every frame (`:344-411`).
- The budget is `kMaxStamps = 128` stamps per frame (`SnowDeformation.h:35`). Overflowing refs keep their anchors (`Stamping.cpp:414-423`).
- **What "reads Havok collision shapes" means here**: `Util::ExtractShapeBound` (`src/Utils/ActorUtils.cpp:26-88`) turns each `hkpShape` into one radius from `GetMaximumProjection` along ±X/Y/Z (capsule = max half-extent, box = half-diagonal, and so on). No orientation, no velocity (the CPU derives velocity from position deltas), no contacts, no hkpWorld query. Caveat: `ActorUtils.cpp:14` intends to skip `hkpListShape` but casts the *rigid body* (`skyrim_cast<RE::hkpListShape*>(hkpRigid)`), so the guard probably never triggers. List and MOPP shapes then get a projection-based radius rather than the prop fallback.
- **Fork `main` goes further** (`cf2facf8:src/Features/SnowDeformation/Spells.cpp`). It snapshots all three `Projectile::Manager` lists under `projectileLock` (`:1782-1803`). Shouts (`SpellType::kVoicePower`, aimed delivery) become cones classified by element and stagger or projectile force (`:485-530`, `:749-800`). It also handles hazards (`:397`), cloaks and auras (`:889`, `:1811-1830`), death blasts, dashes and ground-contact traces (`:317`).

## 4. GPU resources, lifetime, loading screens

PR (`faf93877:src/Features/SnowDeformation.cpp`):

| Resource | Desc | Created | Freed |
| --- | --- | --- | --- |
| `SnowDeformation::DeformationMap` | 2048² `R16_FLOAT`, SRV+UAV, toroidal and updated in place | `SetupResources` `:19-46` | never (feature lifetime) |
| `SnowDeformation::StampTiles` | dynamic structured buffer of 8192 `uint`, SRV | `:48-64` | `com_ptr`, process lifetime |
| `SnowDeformation::PerFrame` | `ConstantBuffer<PerFrame>` (2 × 128 float4 stamps) | `:17` | never |
| 4 compute shaders | `RingCS`/`RefillCS`/`StampCS`/`StampAllCS` from one file | lazily, `EnsureUpdateShaders` `:314-339`; a failure latch stops per-frame recompiles | `ClearShaderCache` `:341-349` |

- **It borrows nothing from the engine.** It binds its own SRV to PS `t101` every frame, even when disabled, because Lighting samples it whenever the define is compiled in (`:189-193`). It unbinds `t101` around the CS writes (`:271-272`, `:309`).
- **Per-frame work** (`Prepass` `:183-312`): ring-clear only the rows and columns scrolled in (`:214-230`). Refill one of 32 row bands per frame from a "bank", because one frame's refill rounds away in R16F (`:232-258`, `SnowDeformation.h:175-184`). Stamp only the 8×8 tiles the capsule bounds touch, with a full-map fallback on overflow (`:121-181`, `:287-300`). With no work there is no dispatch. The author's figures: 0.58 → 0.012 ms GPU, 0.74 → 0.11 ms CPU gathers (commit message of `faf93877`).
- **Loading screens and pausing**: there is no `Reset()` override and no loading-menu check. Prepass returns early only on `ui->GameIsPaused()` (`:198-200`) or when disabled. The window advances only when `a_inWorld` (`:78`). A teleport larger than the window clears everything through the ring path (`:219-220`), and trail anchors break at 256 units (`Stamping.cpp:14`). The actor shape cache holds `NiPointer`s to actor roots and collision objects for up to 30 gather frames (`SnowDeformation.h:209-225`, eviction `Stamping.cpp:201-204`). These are engine-owned objects whose references live across a cell unload until eviction. That is the same class of risk the roadmap's "one owner" rule warns about, though on the CPU side. Whether Prepass runs during the loading menu was not verified.
- Fork `main` additionally owns 2048² `R32_FLOAT` height maps (RTV+UAV) for the statics capture (`Statics.cpp:873-900`, `SnowDeformation.h:1383`). It records `NiPointer<BSGeometry>` per captured draw (`Statics.cpp:650`), cleared each frame (`SnowDeformation.cpp:536`). It persists trenches across saves through a new co-save channel (`src/CoSave.h`, `TrenchCoSave.cpp:8-22`) and drops world-anchored caches on worldspace change (`TerrainData.cpp:430-443`).

## 5. Settings, menu, packaging

- Settings (`SnowDeformation.h:40-52`, JSON at `.cpp:7-13`): `EnableSnowDeformation`, `StampRadius` (20 = 1.0× shape radius), `RefillTime` (700 s), `RefillOnlyWhenSnowing` (reads `Sky::currentWeather` `kSnow`, `.cpp:239-243`), and `RangeTrenchesM` (29-200 m; a change clears the map, `.cpp:107-119`). `ShowDebugTexture` exists but is not serialized.
- Menu (`Menu.cpp:7-57`): enable, stamp radius, refill, a "Render Distance" node, and "Debugging Options": a map preview `ImGui::Image`, a clear button, the terrain overlay and snow-mask cache counters.
- Packaging: `features/Snow Deformation/` has **no `CORE` file**, so it would ship as its own `Package-SnowDeformation` target. Category `kLandscapeAndTextures` (`SnowDeformation.h:12`). `HasShaderDefine` covers only `Lighting` (`:15-18`).
- Fork `main` has a much larger menu (`Menu.cpp`, 1116 lines) and compiles the debug menu out of public packages (commit `255d606d`).

## 6. What Fluid Field can reuse, and roadmap corrections

**Reusable (pattern or code, GPL-3.0 same repo):**
1. **The input gatherer**: player + `highActorHandles` + per-shape `GetShapeBound`, the 3D-root-transform gate for Havok props, per-shape anchors yielding capsule segments, airborne and corpse gating, a staggered cell scan and a hard stamp budget (`Stamping.cpp:57-423`). For the field, the capsule's (prev→current)/dt gives the velocity impulse that Snow Deformation discards. This maps directly onto Phase 2's "actors, loose physics objects as sources" and its 32-source and 0.1 ms CPU gate.
2. **The scrolling-grid technique**: a whole-texel snapped origin computed in `GetCommonBufferData` so the cbuffer and texture agree (`.cpp:71-105`). Also a toroidal addressing with `Load`-based bilinear that avoids filtering across the wrap seam (`SnowDeformation.hlsli:23-40`), and ring-clear of entering texels (`DeformationUpdateCS.hlsl:53-67`). Fluid Field's player-centred grid needs exactly this. Note that stable-fluids advection reads neighbours, so the wrap must also be honoured in every stencil.
3. **Tiled stamp splatting** (`BuildStampTiles` plus groupshared per-tile stamp lists, `.cpp:121-181`, `.hlsl:77-153`) for injecting impulses cheaply.
4. Fork `main` only, unmerged: projectile, shout, hazard and cloak readers (`Spells.cpp`) are a ready catalogue for Phase 2 shouts, spells and projectiles. The top-down MIN/MAX height capture of statics (`SnowHeightCapture.hlsl:1-11`, `Statics.cpp:946`) is a strong **R1 candidate** for the obstacle map.

**What the roadmap assumes that does not hold (needs correcting):**
- *"Snow Deformation (in progress, releases)"*: it is **not in this repo or upstream `dev`**. It is an open PR (#2751) plus a contributor fork. The "release" is a PR prerelease build. Reference the PR and fork URLs, and pin SHAs.
- *"Stamps gathered from Havok collision shapes, a model for reading physics objects as inputs"*: this is true only in a narrow sense. Shapes become **centre + bounding radius** via a helper that **Grass Collision already uses on this branch** (`ActorUtils.cpp`). So "learn from Snow Deformation" adds the prop scan, anchors and gating, not a new Havok reader. Nothing reads velocities, masses, shape orientation or contacts, which buoyancy (R7) will need. That remains new work through `hkpRigidBody` and CommonLib.
- *Grass Collision's "virtualized collision texture"* is not what Snow Deformation uses. Only the PoC branch reads Grass Collision's texture.
- *Design rule "never hold engine-owned resources"*: Snow Deformation's GPU side complies, but its CPU caches do hold `NiPointer`s to engine objects (see §4). Fluid Field should decide its policy explicitly.
- Register and bit collisions to plan around if #2751 merges: PS `t101`, `FeatureData` b6 tail slot, and `ExtraFeatureDescriptor` bits 11-16. Fork `main` uses bits 10-17 on an older base, which would conflict with `TVMeshVariation` (bit 10) on `dev`.

## 7. Open questions

1. Will #2751 merge, and in what order will the fork's `snow/NN-*` slices follow? Fluid Field should not depend on unmerged code. Copy the patterns, or coordinate with @josefplyr1.
2. Does `Feature::Prepass` run during the loading menu, and do the cached actor and prop `NiPointer`s survive a cell unload safely? This needs a test or a reading of the call site in `State`.
3. Is the `hkpListShape` guard in `src/Utils/ActorUtils.cpp:14` a real bug? (It casts the body, not the shape.) It affects every consumer, including Grass Collision and a future Fluid Field.
4. Should Fluid Field and Snow Deformation share one gatherer (one walk of actors and props per frame) instead of three features each walking skeletons? This is a candidate for the "inputs are data" emitter API.
5. Which commit does the `v1.9.0-pr2751` AIO asset actually contain (tag ref `01ffb760c` vs release target `faf93877`)?
6. Can fork `main`'s statics height capture (R32F MIN/MAX, 2048²) double as Fluid Field's R1 obstacle and bed map, and what does it cost?
