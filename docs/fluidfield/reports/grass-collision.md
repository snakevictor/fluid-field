# Grass Collision: read-only report for Fluid Field

Oct 13, 2026 · Phase 0 groundwork report (ROADMAP.md:67) · branch `feature/fluid-field`

Scope: what the Community Shaders Grass Collision feature does, how it touches the engine and the GPU, and what Fluid Field can take from it for the Phase 1 scrolling grid (ROADMAP.md:69) and the Phase 2 input layer (ROADMAP.md:72-73). ROADMAP.md:33 names it as the source for "actor tracking around the player and the virtualized collision texture".

Summary: Grass Collision is small (C++ 536 lines, HLSL 280 lines). Each frame it collects up to 64 actors within 2048 units of the camera, turns their Havok collision bodies into spheres, uploads them to two dynamic structured buffers, and runs one 512x512 compute pass. That pass writes a camera-centred toroidal (clipmap) heightfield with temporal decay. The grass vertex shader then samples the heightfield. It reads no bones, no velocities and nothing besides actors. The toroidal addressing and the CPU-to-GPU upload pattern carry over to Fluid Field almost directly. The actor gathering code works, but it needs its caps, priorities and threading revisited first.

---

## 1. Where the code lives

| Path                                                                     | Lines                            | Role                                                                                                                                          |
| ------------------------------------------------------------------------ | -------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/Features/GrassCollision.h`                                          | 1-129                            | Feature class, `Settings`, GPU structs `BoundingBoxPacked`/`PerFrame`, resource members, `Hooks` struct with install                          |
| `src/Features/GrassCollision.cpp`                                        | 1-407                            | Gathering actors (`QueueCollisions`), per-frame upload and scroll (`Update`), resource creation, CS dispatch, settings, hook thunks           |
| `src/Utils/ActorUtils.h` / `.cpp`                                        | h:1-111, cpp:1-89                | `Util::GetShapeBound`, `Util::ExtractShapeBound` (Havok shape to sphere); `GetRagdollCenter`/`GetActorDisplayInfo` (not used by this feature) |
| `src/Utils/Game.cpp`                                                     | 183-187                          | `Util::GetEyePosition()` = `shadowState->GetRuntimeData().posAdjust.getEye()`                                                                 |
| `src/Utils/Game.h` / `Game.cpp`                                          | h:143-156, cpp:369-372           | `Util::FrameChecker` (once-per-frame gate keyed on `State::frameCount`)                                                                       |
| `features/Grass Collision/Shaders/GrassCollision/CollisionUpdateCS.hlsl` | 1-104                            | Compute shader: scroll invalidation, decay, sphere splatting into the heightfield                                                             |
| `features/Grass Collision/Shaders/GrassCollision/GrassCollision.hlsli`   | 1-175                            | VS-side sampling: toroidal lookup, bilinear filtering, normal from heights, displacement                                                      |
| `package/Shaders/RunGrass.hlsl`                                          | 88-90, 179-192, 239-243, 260-263 | Grass VS includes the hlsli under `GRASS_COLLISION`; two paths: Grass Optimizations instanced and vanilla                                     |
| `features/Grass Collision/Shaders/Features/GrassCollision.ini`           | 1-2                              | `[Info] Version = 3-2-0`                                                                                                                      |
| `features/Grass Collision/CORE`                                          | (empty)                          | CORE marker                                                                                                                                   |
| `src/Globals.h:9,98`, `src/Globals.cpp:11,65`, `src/Feature.cpp:12,229`  |                                  | Global instance and feature-list registration                                                                                                 |
| `package/SKSE/Plugins/CommunityShaders/Translations/{en,ru,zh_CN}.json`  | en:862-870                       | i18n strings                                                                                                                                  |

---

## 2. How it hooks the engine

### 2.1 Hooks (installed in `PostPostLoad`, GrassCollision.cpp:249-252, GrassCollision.h:122-127)

| Hook                          | Mechanism                                                                                                               | Target                                                                                     | Thunk                                                          |
| ----------------------------- | ----------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------ | -------------------------------------------------------------- |
| `BSGrassShader_SetupGeometry` | `stl::write_vfunc<0x6>` on `RE::VTABLE_BSGrassShader[0]` (h:124)                                                        | `BSGrassShader::SetupGeometry(BSRenderPass*, flags)`                                       | Calls `Update()` **before** the original (cpp:342-346)         |
| `MainUpdate_QueueCollisions`  | `stl::write_thunk_call` at `REL::RelocationID(35565, 36564) + VersionedRelocation::Select(0x748, 0xC26, 0xC38)` (h:125) | A call site inside the game's main-loop function (SE / AE pre-1.7.99 / AE 1.7.99+ offsets) | Calls the original, **then** `QueueCollisions()` (cpp:336-340) |

-   `PostPostLoad` hooks are installed only for loaded features (`src/XSEPlugin.cpp:95-96`), so they are absent when the feature is not installed.
-   Grass Optimizations hooks the same vfunc 0x6 (`src/Features/GrassOptimizations.h:282`, thunk `GrassOptimizations.cpp:796-811`). The two thunks chain, and install order decides which runs first.
-   No other code in `src` names function 35565/36564. The general CS main-update hook is a different function, `RelocationID(35551, 36544)+0x11F/0x160` (`src/Hooks.cpp:1086`). The exact identity of 35565 is unverified (see section 6).
-   VR: VR support was removed repo-wide (`c9d7c0524 chore: remove all VR support (#2475)`), so the hook has no VR offset.

### 2.2 Gathering actors (`QueueCollisions`, cpp:37-153)

-   Thread: the call site above runs on the main game-loop thread, once per frame, after the hooked callee returns.
-   Reference point: `Util::GetEyePosition()` (cpp:43), the render camera eye from `BSShaderManager` posAdjust, not the player's position.
-   Source list: `RE::ProcessLists::GetSingleton()->highActorHandles` (cpp:56-60, code credited to po3 under MIT, cpp:54-55), plus `PlayerCharacter::GetSingleton()` (cpp:62-64). Middle- and low-process actors are ignored, and so are dead actors outside `highActorHandles`.
-   Filter: `Is3DLoaded()` and squared distance to the camera at most `MAX_ACTOR_DISTANCE` = 2048 (cpp:14-15, 45-52).
-   Priority: sorted by distance only (cpp:66-68). Caps: `MAX_BOUNDING_BOXES` = 64 actors, `MAX_COLLISIONS_PER_BOUNDING_BOX` = 64 shapes, 4096 shapes in total (cpp:11-13, 138-146).
-   What it reads per actor: no bones and no velocities. It walks `actor->Get3D(false)` with `RE::BSVisit::TraverseScenegraphCollision` (cpp:89-107) and, for every `bhkNiCollisionObject`, calls `Util::GetShapeBound` (ActorUtils.cpp:7-24):
    -   centre = `bhkRigidBody::GetCenterOfMassWorld` scaled by `bhkWorld::GetWorldScaleInverse()` (ActorUtils.cpp:15-20);
    -   radius = from `hkpShape::GetMaximumProjection` along ±X/Y/Z, then per shape type: capsule, sphere, box, cylinder, convex or triangle, fallback (ActorUtils.cpp:26-88).
    -   In practice this returns the ragdoll or biped collision bodies (one per limb) and the character controller, if it has a `bhkNiCollisionObject` in the 3D.
-   Shape culling by size: shapes with `radius < distance * 0.001` are dropped (cpp:16, 93-94). Shapes are sorted by radius, largest first, before the 64 cap (cpp:109-111).
-   Each shape is stored as a `float4(centre - cameraPos, radius)`, camera-relative (cpp:96-104). A 2D XY AABB per actor is built from the shapes (cpp:113-140).
-   Output: `queuedBoundingBoxes` and `queuedCollisions` are member vectors filled with `std::move` (cpp:151-152). There is no lock, and `Update()` on the render side reads them.
-   Per-frame CPU cost (not measured): one pass over `highActorHandles` with `handle.get()` (smart-pointer lookup), a sort, and a scenegraph traversal per actor in range. Several `eastl::vector` heap allocations happen per frame, including one per actor (`collisionShapes`, cpp:87). It is not profiled: there is no Tracy or `profiler` zone around `QueueCollisions` (the only profiler pass is GPU-side, cpp:394-396).

### 2.3 Per-frame GPU update (`Update`, cpp:155-232)

-   Runs from the first `BSGrassShader::SetupGeometry` of each frame and is gated by a static `Util::FrameChecker` (cpp:157-158). The first grass draw of the frame can be a depth or shadow pass, so it may come before the main pass. **Frames with no grass draw (interiors, menus) never run `Update`, so the texture freezes.**
-   Computes the scroll (section 3.3) and fills the `PerFrame` CB: `TimeDelta = *deltaTime * !GameIsPaused()` (cpp:191) and `CameraHeightDelta = prevEye.z - eye.z` (cpp:193).
-   Uploads with `Map(WRITE_DISCARD)` and `memcpy_s` into the two dynamic structured buffers (cpp:199-213), then clears the queues (cpp:215-216).
-   Calls `UpdateCollisionTexture()` (cpp:220, body cpp:364-406). It binds the engine FrameBuffer CB `*globals::game::perFrame` to CS b12 (cpp:374-377), its own CB to b0, SRVs t0/t1 and UAV u0, then runs `Dispatch(64, 64, 1)` inside `profiler->BeginPass("GrassCollision::CollisionUpdate")` (cpp:379-397). It unbinds the CS, CB0 and UAV0 afterwards, but leaves SRVs t0/t1 and CB b12 bound (cpp:399-405).
-   Binds its own CB to **VS b5** and the collision SRV to **VS t100** and never unbinds them (cpp:225-230). The grass VS then reads them.
-   When `EnableGrassCollision` is false it only clears the UAV to 0 (cpp:368-372) and still binds VS resources. A value of 0 decodes to height +2048, which means no collision.

### 2.4 Render passes and shaders touched

-   Only the Grass shader type: `HasShaderDefine` returns true for `RE::BSShader::Type::Grass` (cpp:326-334). The define is `GRASS_COLLISION` (h:12).
-   `RunGrass.hlsl` VS only (the include sits inside `#ifdef VSHADER`, RunGrass.hlsl:86-90):
    -   Grass Optimizations path (RunGrass.hlsl:179-192): runs only when the per-instance `collisionFlag > 0.5`, set by Grass Optimizations' `CollisionDistance` (`GrassOptimizations.cpp:121-126, 336-339`). Positions are passed camera-relative.
    -   Vanilla path (RunGrass.hlsl:239-243, 260-263): `GetDisplacedPosition` applies `World`.
    -   Both write the current and previous displacement into the motion-vector input (`previousMsPosition`), including under `RENDER_DEPTH`.
-   VS cost per vertex: `ComputeCollision` makes 3 `GetCollision` calls, each a manual 2x2 bilinear with 4 `Load`s, so 12 texel loads (hlsli:32-90, 102-134). It is skipped when `Color.w == 0` (rocks, grass bases) or beyond 2048 units (hlsli:145-147).

### 2.5 Feature callbacks overridden

`GetName/GetDisplayName/GetShortName/GetShaderDefineName/GetCategory(kGrass)/GetFeatureSummary` (h:9-23), `HasShaderDefine` (cpp:326), `ClearShaderCache` (cpp:348-353), `SetupResources` (cpp:254-324), `DrawSettings` (cpp:29-35), `LoadSettings/SaveSettings/RestoreDefaultSettings` (cpp:234-247), `PostPostLoad` (cpp:249-252). It does **not** override `Reset`, `DataLoaded`, `Prepass`/`EarlyPrepass` or `GameLoaded`.

---

## 3. GPU resources it owns

### 3.1 Inventory (all created in `SetupResources`, cpp:254-324)

| Resource                 | Type / format                                                                    | Size   | Binding        | Notes                                                                           |
| ------------------------ | -------------------------------------------------------------------------------- | ------ | -------------- | ------------------------------------------------------------------------------- |
| `perFrame`               | `ConstantBuffer<PerFrame>` (h:48-60)                                             | 48 B   | CS b0, VS b5   | raw `new` (cpp:256)                                                             |
| `collisionTexture`       | `Texture2D` 512x512, `R16G16B16A16_FLOAT`, SRV+UAV (cpp:259-286)                 | 2 MiB  | CS u0, VS t100 | raw `new` (cpp:284)                                                             |
| `collisionBoundingBoxes` | Dynamic structured buffer, stride 32, 64 elements (cpp:290-304)                  | 2 KiB  | CS t0          | `eastl::unique_ptr<Buffer>`                                                     |
| `collisionInstances`     | Dynamic structured buffer of `float4`, 4096 elements (cpp:308-322)               | 64 KiB | CS t1          | `eastl::unique_ptr<Buffer>`                                                     |
| `collisionUpdateCS`      | `cs_5_0` from `Data\Shaders\GrassCollision\CollisionUpdateCS.hlsl` (cpp:355-362) |        |                | Compiled lazily on first dispatch; released in `ClearShaderCache` (cpp:348-353) |

-   Creation: once, from `State::Setup` through `ForEachLoadedFeature("SetupResources")` (`src/State.cpp:340`).
-   Freeing: none. There is no destructor or teardown. `perFrame` and `collisionTexture` are raw pointers that are never deleted. All of these live for the process.
-   Nothing is recreated on resolution change, load or cell change, and nothing needs to be, because nothing depends on the screen size.

### 3.2 Texel layout (CollisionUpdateCS.hlsl:41-103, hlsli:17-20)

-   Coverage 4096 x 4096 units, 512 texels, so 8 units per texel (`CELL_SIZE`). The vertical encoding maps `ZRANGE` (2048, -2048) camera-relative to [0,1] with `lerp`, where 0 means "no collision" at +2048.
-   `.x`: lowest sphere-bottom height so far, recovering upward at `TimeDelta * 1` units/s (slow persistence). `.y`: same, recovering at `TimeDelta * 100` (fast trail, drives the procedural spring in `ProceduralAnimation`, hlsli:22-30). `.zw`: the previous frame's `.xy`, used for previous-frame displacement and motion vectors (CS:58-68, 100-103).
-   Splat: for each texel, test the actor AABBs held in groupshared memory (CS:32-39, 71-74), then each sphere in the box. Height = `z - sqrt(r² - d²)`, the sphere's lowest point (CS:76-95).

### 3.3 Centring and toroidal scrolling (cpp:163-189, CS:45-53, hlsli:34-65)

-   Snapping: `cellID = round(eyePos.xy / 8)`, `cellOrigin = cellID * 8`. `PosOffset = cellOrigin - eyePos` is the sub-texel offset of the grid, camera-relative (cpp:173-182).
-   Toroidal origin: `ArrayOrigin = (cellID - 256) % 512` in **unsigned** arithmetic (`textureArrayDims` is `uint`, cpp:171, 184-187). Because 2^32 is a multiple of 512, negative cell IDs still wrap correctly. That correctness is implicit and fragile.
-   CS write side: logical index `= (dispatchID - ArrayOrigin) % 512`. HLSL promotes the int/uint mix to uint, so the `max(...,0)` at CS:45 is a no-op and the wrap again relies on unsigned overflow. Read side (hlsli:65): `tex = (logical + ArrayOrigin) % 512`. The two are consistent.
-   Invalidation of newly exposed edges: `ValidMargin = prevCellID - cellID` (cpp:179, 189). In the CS, texels whose logical index falls in the newly exposed strip skip reading history and start fresh (CS:50-53, 60-69). Old texels are never moved or copied, so scrolling costs nothing beyond the normal full-texture pass.
-   Vertical scrolling: there is no vertical grid. Heights are camera-relative, so every texel is shifted by `CameraHeightDelta` each frame (CS:64-65).
-   Teleports, loads and cell changes: there is no explicit reset. A large jump makes `|ValidMargin| ≥ 512`, which invalidates every texel and amounts to an implicit full clear. `prevCellID` and `prevEyePosNI` are function-local statics (cpp:163, 166), and `Update` does not run in frames without grass, so after an interior the first exterior frame compares against the last exterior position. A short jump (< 4096 units) after a load would therefore keep stale history, offset by `CameraHeightDelta`. Nothing hooks `LoadingMenu` or `Reset`.

### 3.4 Engine resources borrowed

-   `*globals::game::perFrame` (the engine FrameBuffer CB), bound to CS b12 during dispatch only (cpp:375-376).
-   `globals::game::deltaTime`, `globals::game::ui->GameIsPaused()` (cpp:191), `shadowState` posAdjust via `GetEyePosition` (Game.cpp:183-187), `globals::d3d::context`.
-   The VS binding slots b5 and t100 in the grass VS. Note that `Common/SharedData.hlsli` also declares `cbuffer SharedData : register(b5)` (SharedData.hlsli:9) and RunGrass includes it at top level (RunGrass.hlsl:8). This works only because the VS never references SharedData. See section 6.
-   It holds no references to engine textures, which matches ROADMAP.md:59's "one owner" rule.

---

## 4. Settings, menu and packaging

-   `Settings` (h:31-36): `EnableGrassCollision = 1`, `TrackRagdolls = 1`, `EnableBlur = 1`.
-   JSON: `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT` serializes only `EnableGrassCollision` and `TrackRagdolls` (cpp:24-27). `EnableBlur` is neither serialized nor read anywhere. `TrackRagdolls` is serialized but never read in this feature: `QueueCollisions` does not check `IsDead()`, and the only consumer of a `trackRagdolls` parameter is `Util::GetActorDisplayInfo` (ActorUtils.h:87-110), which has no caller under `src`. Both are dead settings.
-   Load/save/restore: whole-struct JSON assignment (cpp:234-247).
-   UI: one tree node with a single "Enable Grass Collision" checkbox (cpp:29-35). It toggles at runtime with no restart: the CS clears instead of splatting (cpp:368-372), and `QueueCollisions` returns early (cpp:39-40). The shader define stays compiled in, so VS cost remains when the setting is disabled.
-   i18n: prefix `feature.grass_collision.` (cpp:9). Keys: `name`, `description`, `key_feature_1..5`, `grass_collision`, `enable` (h:10-22, cpp:31-32). They exist in en, ru and zh_CN (9 keys each, en.json:862-870). de, es, fr, ja, nl and pl have none and fall back to English. `key_feature_2` claims "up to 256 simultaneous interactions", but the code caps at 64 actors × 64 shapes (cpp:11-13).
-   `.ini`: `Version = 3-2-0`, no `Alpha`/`Beta`/`Unreleased` flag, so the stage is Release (GrassCollision.ini:1-2; stage logic in `src/Feature.h:109-117`).
-   CORE: the `features/Grass Collision/CORE` marker makes CMake add it to `FEATURE_CORE_NAMES` (CMakeLists.txt:336-345). It is copied into the main zip (CMakeLists.txt:1040-1055) and into `Package-Core` (CMakeLists.txt:1168-1174), with **no** `Package-GrassCollision` target (CMakeLists.txt:1198-1201 skips CORE). It is a Release CORE feature, so it is enabled by default (`src/Feature.h:149`).

---

## 5. What Fluid Field can reuse

| Piece                             | Location                           | How directly it can be reused                                                                                                                                                                                  | Notes for Fluid Field                                                                                                                                                                                                                                                                                                                                                                   |
| --------------------------------- | ---------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Toroidal clipmap addressing (CPU) | cpp:163-189                        | **Copy almost verbatim** into a `ScrollingGrid` helper with grid size and texel size as parameters. Make the modulo explicitly signed and positive (`((x % n) + n) % n`) rather than relying on uint overflow. | Phase 1 grid (ROADMAP.md:55, 69). Snapping to texel size avoids sub-texel swimming. `PosOffset` carries the fractional part.                                                                                                                                                                                                                                                            |
| Toroidal addressing (GPU)         | CS:45-53, hlsli:34-65              | **Extract to a shared `.hlsli`** with `LogicalToTex`, `TexToLogical`, `IsNewlyExposed(ValidMargin)` and `WorldToGridUV`.                                                                                       | Fluid solvers sample neighbours, and with wrapping addressing that maps directly onto `(tex ± 1) % N`. Advection needs to know that newly exposed edge texels are "fresh" (set to boundary or vanilla flow), so the `ValidMargin` test is the hook for edge seeding and for the R3 sponge boundary.                                                                                     |
| Camera-height rebasing            | CS:64-65, cpp:193                  | Pattern only                                                                                                                                                                                                   | Storing heights camera-relative needs a per-frame shift. For water heights Fluid Field should store **absolute** Z (or relative to a slowly snapped reference), which avoids fp16 drift; fp16 over a 4096-unit range resolves about 1-2 units near the top of the range.                                                                                                                |
| Gathering actors                  | cpp:37-153                         | **Adapt, do not share as-is.** Lift the candidate loop (cpp:42-68) into a reusable `ActorCandidates(maxDist)` utility.                                                                                         | Phase 2 may need middle-high-process actors (`ProcessLists::middleHighActorHandles`, CommonLibSSE-NG ProcessLists.h:74), dead bodies (ragdoll flag that actually works), horses (already actors), projectiles and loose physics objects (not covered; Snow Deformation is the reference there, ROADMAP.md:34). Priority today is distance only; Phase 2 wants distance plus importance. |
| Havok body to sphere              | ActorUtils.cpp:7-88                | **Reuse directly** (already in `Util::`).                                                                                                                                                                      | Gives per-limb spheres. Fine as obstacle or impulse sources. The `hkpListShape` guard at ActorUtils.cpp:14 casts the rigid body, not the shape, so it never triggers (see section 6).                                                                                                                                                                                                   |
| Velocities                        | none                               | **Not available**                                                                                                                                                                                              | Grass Collision gets motion implicitly from the temporal trail (`.y` decay). Fluid Field Phase 2 needs explicit velocity: keep last frame's sphere centres per `ActorHandle` and difference them with `deltaTime`, or read `bhkRigidBody` linear velocity (needs R7-adjacent checking).                                                                                                 |
| CPU-to-GPU upload                 | cpp:199-213, 290-322               | **Copy the pattern**: dynamic structured buffer, `WRITE_DISCARD`, count in the CB, fixed max capacity.                                                                                                         | Fits ROADMAP.md:55's "small list of impulses". Two-level culling (per-source AABB in groupshared, CS:32-39, 71-74, then the source's shapes) is a good template for an impulse splat CS. Fluid Field should splat per-impulse footprints, or bin by tile, instead of looping all boxes over every texel, once sources reach 32+ with large radii.                                       |
| Caps                              | cpp:11-16, 66-68, 109-111, 138-146 | Pattern                                                                                                                                                                                                        | Distance sort, max sources, max shapes per source and a size-vs-distance cull (`radius < d*0.001`). Phase 2's "32 sources < 0.1 ms CPU" gate (ROADMAP.md:73) needs pre-reserved, reused vectors (no per-frame or per-actor allocation, cpp:42, 70-74, 87) and a Tracy zone.                                                                                                             |
| Once-per-frame gate               | cpp:157-158, Game.cpp:369-372      | Reuse `Util::FrameChecker`                                                                                                                                                                                     | Fluid Field should **not** hang its sim step off a grass draw. Use a CS-owned per-frame point (e.g. `Prepass`/`EarlyPrepass`, `src/Feature.h:182-185`) so the sim runs in interiors and grass-less areas.                                                                                                                                                                               |
| Hooks                             | h:122-127                          | Avoid copying                                                                                                                                                                                                  | The vfunc 0x6 slot is already shared by two features. For inputs Fluid Field needs a main-thread gather point. It could reuse the same call site (35565/36564) or `Hooks.cpp:1086`'s `Main_Update`, after confirming the thread and timing.                                                                                                                                             |

Possible extraction: a shared `ActorProximityTracker` (C++, main thread, double-buffered output) and a shared `ScrollingGrid.hlsli` would let Grass Collision become the first consumer. It would also give the "grass bending from the field" side track (ROADMAP.md:87) a clean path to reading Fluid Field's velocity layer instead of its own texture.

---

## 6. Open questions

1. **Which function is `RelocationID(35565, 36564)`, and is the call site at 0x748/0xC26/0xC38 on the main thread, before or after Havok's step?** This matters for using ragdoll positions in the same frame and for Phase 2's gather timing. Verify in Ghidra.
2. **Thread safety:** `QueueCollisions` writes `queuedBoundingBoxes`/`queuedCollisions` (cpp:151-152) and `Update` reads and clears them (cpp:195-216) with no synchronisation. This is safe only if both run on the same thread or are strictly ordered. Is rendering ever on a different thread here (for example with SKSE engine-fixes or multithreaded render options)?
3. **Stale history after loads and interiors:** statics at cpp:163-166 survive loading screens, and `Update` is skipped in grass-less frames. Is a reset on `LoadingMenu` close needed? Fluid Field needs one (ROADMAP.md:71 gate).
4. **Register b5 overlap** between `GrassCollisionPerFrame` (hlsli:5) and `SharedData` (SharedData.hlsli:9), both visible in the grass VS. It works today because the VS does not use SharedData. Does `hlslkit-buffer-scan` (.claude/CLAUDE.md:36-37) flag it? Fluid Field should reserve its own slots.
5. **CS leaves SRVs t0/t1 and CB b12 bound** after dispatch (cpp:399-405). Is that harmless for later engine CS work?
6. **`hkpListShape` guard** at ActorUtils.cpp:14 casts the `hkpRigidBody`, not the shape, so it is probably always null. Do list shapes on some creatures give oversized radii?
7. **Dead settings:** `TrackRagdolls` and `EnableBlur` (h:33-35). Dead actors in `highActorHandles` are still tracked regardless. Was ragdoll gating lost in a refactor? Check `git log -S TrackRagdolls`.
8. **Coverage:** only `highActorHandles` plus the player. Are mounts and distant high-process NPCs handled as expected? Projectiles and physics objects are absent. Is that intended scope or a gap Fluid Field should fill?
9. **Cost:** no measured CPU or GPU numbers exist. The CS is a fixed 262k-thread pass every frame, even with zero actors (only the inner loop shrinks). Capture Tracy and RenderDoc timings on the ROADMAP test routes (ROADMAP.md:116) before using it as a baseline.
10. **Snapping to the eye position:** the grid follows the camera eye, not the player. In third person or with a free camera, actors near the player but more than 2048 from the camera drop out. Fluid Field should decide whether its centre is the player or the camera.
