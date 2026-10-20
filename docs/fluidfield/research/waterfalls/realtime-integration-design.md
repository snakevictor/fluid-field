# Real-time GPU waterfall sim: engine and framework integration design

Date: 2026-10-20. Branch `feature/fluid-field` at HEAD `5f67b1987` (D77). Another agent was retiring the old
top-down solver (D78) while this was written, so code is cited from **git HEAD** (`git show HEAD:<path>`),
not the working tree. Nothing here was built or run; claims marked **(L1)** come from reading code and hook
offsets only and need a runtime check (phase 0).

Question: move the falls from a one-off CPU bake per mesh (D59, D74) to a live GPU sim in each vanilla falls
mesh's texture space. Up to 192x192 per mesh and about 20 meshes near the player. The sim's state drives the
shading every frame (thickness, velocity, foam, texture coordinates) and possibly a light vertex
displacement. It is coupled to rock, actors and the shallow-water solver (SWE).

---

## 0. Summary of the recommendation

1. **Dispatch in `EarlyPrepass`, not `Prepass`.** By the hook offsets, the lit falls' depth pre-pass
   (`Main_RenderDepth`) runs **between** `EarlyPrepass` and `Prepass` (section 1.1). If anything the
   depth pre-pass reads is written in `Prepass`, the two passes disagree, and depth-EQUAL turns the
   surface into holes or milky pixels (LEARNINGS, "Lit geometry must keep its exact vertex position").
   The same ordering suggests a **latent bug in D67's seam dither**: `FallGround::Dithered` keys its
   pattern on `Time`, which `Prepass` advances. The pre-pass then dithers with last frame's pattern and
   the lit pass with this frame's (section 1.1, phase 0).
2. **One shared atlas.** A 1024x1024 texture set, allocated in 32x32-texel blocks with first-fit over a
   32x32 occupancy bitmap, so meshes stream in and out without repacking. A tile list (8x8 tiles that
   hold water, from awake meshes only) feeds one `Dispatch` per pass for all meshes.
3. **Sim model:** thin-film shallow water on the mesh's own surface, in texel coordinates, using the metric
   and the tangent gravity that `SolveBake` already computes (`FallSites.cpp:2597-2627` at HEAD).
   Explicit and semi-Lagrangian, like `ShallowWaterSolver`. **Warm-started from the CPU bake**, so a mesh
   never shows spin-up. The CPU bake stays as the source of the static inputs (rasterised positions, rock)
   and the initial state.
4. **Texture coordinates become sim state.** Advect the D67 pair (streak S, run time T) every step:
   `S <- S(x - v dt)` and `T <- T(x - v dt) + dt`, with inflow boundary conditions from the mesh or link
   feeding it. In steady flow these are exactly D67's baked fields. When the flow changes they relax over
   the water's residence time. No regeneration, no phases, no `rate * time`, so nothing pulses.
5. **Displacement through a per-vertex buffer, not texture reads in the vertex shaders.** A small compute
   pass writes each dense-copy vertex's offset (current and previous) into a structured buffer. Both
   Lighting.hlsl and Utility.hlsl add it the way D69's `Shift` already does, a path proven to keep the two
   depth passes equal. The previous offset goes into `previousInputPosition`, as TREE_ANIM does
   (Lighting.hlsl:163-170), so TAA and motion blur get correct motion vectors.
6. **Translucency by hashed (stochastic) discard in both passes, resolved by TAA.** This extends D67's
   seam dither. Do not attempt blended lit falls: they leave the deferred path, and the D53-D55 failure
   is most likely accumulator routing that was fixed at setup time (section 5.2).
7. **Coupling, GPU to GPU.** The foot outflow is splatted into a GPU-written version of the plunge map the
   SWE already reads (`WaterParams::plungeMap`). Actors are a small capsule buffer tested against each
   texel's world position. Rock comes from the bake's mask.

Cost estimate: about 0.05-0.3 ms GPU per frame for about 200k live texels with 2 substeps. The SWE's 512²
grid measured about 0.04 ms per frame (`reports/water-slice-1-round-3.md:66`). CPU cost is about 0.05 ms per
frame plus the existing one-time bake.

Phase 1 (smallest testable step): a GPU replay of the **current bake**. Atlas, upload, binding in
`SetupFallGeometry`, the lit pass reading the atlas instead of the per-mesh t124. The result should look
identical to D77. Details in section 7.

---

## 1. Where the compute runs, budget, persistence, LOD, streaming

### 1.1 Frame order (what CS hooks, and when)

| Order | Point                                                                                                                               | Source                                                                        | What it means for us                                                                                                                                                                    |
| ----- | ----------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1     | `Main_RenderShadowMaps` (frame fn 35560/36559 +0x2EC) then `Deferred::EarlyPrepasses` → `Feature::EarlyPrepass`                     | `src/Deferred.h:159`, `src/Deferred.cpp:211-235, 656-660`                     | Shadow maps are done. Earliest per-frame hook. **The sim dispatch goes here.**                                                                                                          |
| 2     | `Main_RenderDepth` (35560 +0x395): the z-prepass. BSUtilityShader draws lit geometry's depth, falls included                        | `src/Features/TerrainBlending.h:126-127` ("z-prepass depth vs shadows depth") | Lit falls are drawn to depth through `BSUtilityShader_SetupGeometry` → `SetupFallGeometry` (`src/Hooks.cpp:258-271`). **(L1)**: the order comes from call-site offsets in one function. |
| 3     | `Main_RenderWorld` (35560 +0x831) → `Main_RenderWorld_Start` (99938 +0x8E) → `StartDeferred` → `PrepassPasses` → `Feature::Prepass` | `src/Deferred.h:161-162`, `src/Deferred.cpp:250, 297, 677-685`                | `FluidField::Prepass` (`FluidField.cpp:594` HEAD) runs here: wind, the SWE, `fallSites.Update`, `UpdateFallGround`, the scroll phase.                                                   |
| 4     | Opaque lit draws (deferred) → `BSLightingShader_SetupGeometry` → `SetupFallGeometry`                                                | `src/Hooks.cpp:229-240`                                                       | Lit falls are drawn with depth EQUAL against step 2.                                                                                                                                    |
| 5     | BlendedDecals → `EndDeferred` → `DeferredPasses`, then water and blended geometry                                                   | `src/Deferred.cpp:687-715, 423-456`                                           | Forward or blended passes after this point. The SWE output is read by Water.hlsl here.                                                                                                  |

Consequences:

-   **The sim must step before step 2.** Then the z-prepass and the lit pass read the same atlas. If it
    stepped in `Prepass`, the pre-pass would use frame N-1's displacement or alpha and the lit pass frame
    N's. Any per-pixel discard (translucency) or vertex displacement would then break depth-EQUAL.
    `FluidField` does not override `EarlyPrepass` today (`FluidField.h:125` HEAD has only `Prepass`). Add
    it and move the falls sim and its constant updates there. The SWE can stay in `Prepass`: it reads the
    falls' outflow from the same frame, which by then is already written.
-   **Latent bug to verify (phase 0):** `FallGround::Dithered` (FallGround.hlsli:139-147 HEAD) takes its
    frame index from `Time`. `Time` is `fallSurfaceTime` (`FluidField.cpp:846` HEAD), advanced in
    `Prepass` and copied into b13 by `SetupFallGeometry`. If step 2 really comes before step 3, the D67
    seam dither uses a different pattern in the pre-pass and in the lit pass. Pixels kept by one pass and
    discarded by the other then show the G-buffer clear colour. TAA may have hidden this as "softness". Fix
    it either way: freeze a `DitherFrame` integer in `EarlyPrepass`.
-   The camera constants are not trustworthy outside the deferred passes (LEARNINGS, 2026-10-15). The sim
    does not need the camera: LOD distances are computed on the CPU from the player's position.
-   Shadow maps (step 1) read the previous frame's state. That is harmless: shadows need no equality.
-   Reflections (`ReflectionsPrepass`, cube maps and water reflections) re-draw the falls through the same
    `SetupFallGeometry`, so they pick up the same atlas and the same per-vertex buffer automatically.

### 1.2 How ShallowWaterSolver does it (the pattern to copy)

-   `ShallowWaterSolver` (`ShallowWaterSolver.h`) owns its textures, does ping-pong state (`depth`,
    `velocity` and `foam` as `std::array<Layer,2>`), and has a `Dispatch` helper that binds t0..t3 plus
    UAVs and unbinds them. That matters because D3D11 silently nulls an SRV still bound as a UAV
    (LEARNINGS). `Step()` is a template that takes a `wrap(passName, runPass)` callback, so the caller
    adds Tracy zones and `globals::profiler->BeginPass/EndPass` per pass (`FluidField.cpp:769-781` HEAD).
    The built-in profiler cannot nest, so each pass is timed on its own.
-   Substepping: `MAX_SUBSTEP = 0.012`, 1 to 4 substeps, with `deltaTime` capped at 1/20 s
    (`FluidField.cpp:1359-1366`, `FluidField.h:50` HEAD). A warm-up of fixed substeps runs when a new map
    arrives (`:1375-1383`).
-   Keep it standalone, with no PCH or CommonLib in the solver class, so `tests/FluidField` can run it on
    WARP (LEARNINGS, "A standalone test can use the Windows SDK's C++/WinRT"). The new
    `FallSurfaceSolver` should follow the same rule. That gives an offline test bed next to the existing
    `simbake.py` harness (D74).

Budget measurement: wrap each pass as `FluidField::Falls <Pass>`, and read the numbers with devbench
`communityshaders.inspect kind=profiler` (`skyrim-engine-re/kb/runtime-capabilities.md`). Add a menu
line with the live texel count, awake meshes and substeps, as the water section already shows its counts.

### 1.3 Atlas layout

| Resource               | Format                                 | Contents                                                                                           | Written                      |
| ---------------------- | -------------------------------------- | -------------------------------------------------------------------------------------------------- | ---------------------------- |
| `StateA/B` (ping-pong) | RGBA16F                                | h (thickness, world units), u, v (texels/s), foam                                                  | each substep                 |
| `CoordA/B` (ping-pong) | RG32F (or RG16F with per-mesh offsets) | S (streak, texture u), T (run time, s)                                                             | each substep                 |
| `Static0`              | RGBA32F                                | world position (absolute xyz) and flags (packed bits: covered, rock, inflow, outflow, edge, lying) | once per mesh                |
| `Static1`              | RGBA16F                                | inverse metric (G⁻¹ as 3 floats: uu, uv, vv) and normal gravity g_n                                | once per mesh                |
| `Static2`              | RG16F                                  | tangent gravity (gu, gv) in texels/s² (`SolveBake`'s `gu`, `gv`)                                   | once per mesh                |
| `Bake`                 | RGBA16F                                | the CPU bake's steady state (S, T, wake, foam): warm start and fallback                            | once per mesh                |
| `Render`               | RGBA16F                                | what the lit pass reads: S, T (or uv directly), foam, thickness                                    | once per frame (output pass) |

Size: 1024x1024 at about 60 B/texel is about 63 MB. That is too much for a set that is mostly empty, so:

-   **Start at 512x1024 and grow to 1024x1024 only when allocation fails.** Growing reallocates and
    re-uploads the static textures from the CPU copies, and warm-starts the state from `Bake`. That is the
    same path a cell load takes. The working set measured so far: D72 drew 67,826 texels over 18 meshes
    (a subset), and D70 counted 68,764 flat texels. Expect 100k-250k covered texels in a waterfall-rich
    area, so 512x1024 (524k) holds it with 2x slack.
-   **Allocation:** 32x32-texel blocks, first-fit over a 32x32 (or 16x32) occupancy bitmap. Each mesh
    takes ceil((w+2)/32) x ceil((h+2)/32) blocks, keeping a 1-texel guard ring for bilinear sampling and
    ghost cells. A 192x192 mesh takes 7x7 blocks. Free means clearing the bits. Fragmentation is
    bounded because every request is at most 7x7 blocks. Fall back to growing, then to "this mesh keeps
    its static bake". (Jylänki's survey [6] ranks skyline as fastest for runtime packing. A block bitmap
    is simpler and good enough at this scale.)
-   Alternative considered: a `Texture2DArray` with one 192x192 slice per mesh. It is trivial to stream,
    but small meshes waste most of their slice (32 slices are about 1.2M texels) and memory triples. Not
    recommended.

### 1.4 One dispatch for all meshes

-   `MeshInfo` structured buffer, one entry per atlas slot: atlas origin (x, y), size (w, h), dt scale
    (LOD), flags (awake, has feeder), world origin, inflow speed, and an index into the link range.
-   `Tiles` structured buffer of `uint2` tile origins plus a mesh index, rebuilt on the CPU when the set
    of awake meshes changes. Only 8x8 tiles that contain covered texels are listed (from the bake's
    `covered` mask). Each pass is then `Dispatch(tileCount, 1, 1)` with `[numthreads(8,8,1)]`, and the
    thread looks up its tile, so empty corners of a mesh rectangle and sleeping meshes cost nothing. This
    is the "stamp only touched tiles" pattern Snow Deformation used to go from 0.58 to 0.012 ms
    (`reports/snow-deformation.md:78`).

### 1.5 Model and passes (per substep)

Parametrise each mesh by its texture grid (u across, v down; v is downstream on every vanilla falls mesh,
per `vanilla-nif-dissection.md`). At each texel the world tangents are `tu` and `tv`, from which come the
metric G, gravity's tangent part (gu, gv) and its normal part g_n = g·n (negative when gravity presses the
water onto the surface). `SolveBake` already computes tu, tv, the inverse metric and gu, gv
(`FallSites.cpp:2597-2627` HEAD). They move into `Static1/2` unchanged. This is the general shallow-wave
formulation on surfaces of Wang, Miller and Turk [1], restricted to a parametrised chart.

1.  **Exchange** (links, section 3): write ghost and inflow cells from feeder meshes and across chart seams.
2.  **Velocity**: semi-Lagrangian advection of (u, v) in texel space, plus tangent gravity, plus the
    hydrostatic term `-max(-g_n, 0) ∇h` (only where gravity presses the film onto the surface; on a
    curtain g_n ≈ 0 and the sheet is in free fall, which is right), bed friction (`SIM_FRICTION` 3/s),
    and drag toward the terminal speed (`SIM_TERMINAL` 900), with world speed from G. Walls (rock,
    off-mesh) zero the normal component. Actors push here (section 4).
3.  **Height**: flux-form upwind update of h, with face fluxes `h_up · v_face · edge length`. Divide by
    the cell's area √det G so a sheet thickens where it narrows instead of racing. That is the property
    D74 needed a pressure projection for; the explicit film gets it from mass conservation. Optional:
    pipe-model fluxes [2] (outflow scaled so h ≥ 0) for robust wet/dry fronts.
4.  **Coordinates and foam**: advect S, T (T also gains dt) and foam. Foam sources: velocity into walls,
    shear |∇v|, speed jumps (Froude > 2.5, D71's bar), and actor intrusion. Foam decays.
5.  **Output** (once a frame, after the last substep): `Render`, the spray emitter strengths
    (section 5.4), the foot outflow splat (section 4), and the vertex displacement (section 2.3).

CFL: free flow reaches about 900 u/s at 10 u/texel, which allows about 11 ms per step. Semi-Lagrangian
advection is unconditionally stable, so use 2 substeps at 60 fps (as `MAX_SUBSTEP = 0.012`) and clamp
|v|·dt ≤ 2 texels for the flux pass. Gravity waves in a 5-unit film travel at √(686·5) ≈ 60 u/s, so
advection dominates the time step.

### 1.6 Per-frame cost estimate

| Item                       | Count                                       | Estimate                                                                                                                            |
| -------------------------- | ------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| Live texels (awake meshes) | 100k-250k typical; 737k worst (20 x 192²)   |                                                                                                                                     |
| Passes per substep         | 4 (exchange is tiny)                        |                                                                                                                                     |
| Substeps                   | 2 at 60 fps                                 |                                                                                                                                     |
| Traffic                    | about 60-100 B/texel/pass                   | 250k x 8 x 80 B ≈ 160 MB/frame worst typical → about 0.3-0.5 ms at 400 GB/s; 100k texels ≈ 0.1-0.2 ms                               |
| Reference                  | SWE 512² (262k), 5 passes, up to 4 substeps | about 0.04 ms per frame measured (`water-slice-1-round-3.md:66`), so the real cost is probably well under the bandwidth bound above |
| Vertex displacement pass   | about 16k vertices per dense mesh x 20      | under 0.01 ms                                                                                                                       |
| CPU                        | tiles, CBs, actor capsules                  | under 0.05 ms; bakes stay on their existing 3 ms/frame ray budget (`BAKE_BUDGET_MS`, FallSites.h:302)                               |

Gate: 0.5 ms GPU on the test falls. If the sim goes over, halve the step rate for meshes beyond 2048 units
(section 1.8) before anything else.

### 1.7 Persistence across frames and cell loads

-   **Within a session**, state lives in the atlas while the mesh is resident.
-   **Across cell unload/reload, persist nothing on the GPU.** A falls flow is steady: warm-starting from
    the bake's steady state (`Bake` → `State` and `Coord`, plus 0.5 s of warm-up substeps like
    `WARMUP_SECONDS`) is indistinguishable from continuity. The expensive part is the CPU bake (rays, 3 ms
    per frame over seconds). Cache **that**, keyed by something that survives reloads, not by the
    `BSGeometry*` (dense copies are rebuilt and pointers change): the reference form ID, the geometry's
    name or index path under the reference's root, and a hash of the world transform and vertex count.
    Keep a memory LRU (about 64 meshes). Later, consider a disk cache next to the shader cache.
-   **Loading screens**: `MenuOpenCloseEventHandler` already sets `queuedReset` (`FluidField.cpp`,
    LoadingMenu close). Free all atlas slots there, and keep the LRU bake cache.

### 1.8 LOD and sleeping

| State             | Rule                                                                                                                                 | Cost                                                                                                                                                              |
| ----------------- | ------------------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Awake (full rate) | within 2048 units, or drawn within the last 0.5 s (set a `lastDrawnFrame` per mesh in `SetupFallGeometry`, which is called per draw) | full                                                                                                                                                              |
| Awake (half rate) | 2048-4096 units: stepped every other frame with 2·dt (per-mesh `dtScale` in `MeshInfo`), still within CFL with substeps              | half                                                                                                                                                              |
| Asleep            | beyond 4096 units, or not drawn for 2 s                                                                                              | no tiles listed; state frozen; still rendered from `Render` (the scroll keeps moving because the texture's motion is the CPU phase, so a frozen mesh still flows) |
| Evicted           | beyond the grounded radius, or the reference unloaded                                                                                | slot freed; bake kept in the LRU                                                                                                                                  |

When a mesh wakes, it is already in steady state, so nothing pops. "Drawn" is a better signal than
distance alone: a fall behind the player can sleep.

### 1.9 Mesh streaming

The sources are already event-driven: `FallSites::Update` → `RefreshGrounded` (FallSites.cpp:1665 HEAD)
finds lit falls meshes, `Densify` (`:1915`) builds dense copies, and the bake queue (`BakeStep`, `:3278`)
rasterises, ray-tests and solves them, then `LinkBakes` (`:3010`) links them. Add one more consumer, the
**atlas manager** (`FallAtlas`):

-   on a new bake: allocate blocks, upload `Static*` and `Bake` with `UpdateSubresource` on the rectangle,
    rebuild `MeshInfo`, `Tiles` and the link buffer, and queue warm-up substeps for that mesh only (a
    per-mesh flag in `MeshInfo`; warm-up dispatches list only its tiles);
-   on removal (the geometry left `grounded`, or `RemoveDense`): free the blocks and rebuild the lists;
-   `GetFlowBakeVersion` (FallSites.h:162) is the trigger, as `UpdateFallGround` already uses
    (FluidField.cpp:1510-1530 HEAD).

Rule: NiPointers held by FallSites keep geometries alive. The atlas holds only raw keys (compared, never
followed), as `IsGrounded` does.

---

## 2. Sampling in the lit pass and depth pre-pass, displacement and motion vectors

### 2.1 Today's binding

`SetupFallGeometry` (FluidField.cpp:1561-1655 HEAD) runs after every Lighting, Effect and Utility
`SetupGeometry` (Hooks.cpp:229-271). For a grounded mesh it fills `FallGroundData` and maps it into b13
(`fallGroundOn`, a DYNAMIC buffer written with WRITE_DISCARD per draw). It binds t120 (ground and pool),
t122 (stream, being retired by D78), t123 (per-vertex `FallVertex` buffer, VS and PS) and t124 (that
mesh's own bake texture, PS only). The shaders read b13 as `FallGround::FallGroundCB`
(FallGround.hlsli:12-32 HEAD), and the per-vertex data by `VertexBase + SV_VertexID`
(Lighting.hlsl:58-59, Utility.hlsl:34-36).

### 2.2 With the atlas

-   **Bind once per frame, not per draw**: t124 becomes the atlas `Render` SRV (one texture for all
    meshes), so the per-draw `PSSetShaderResources(124, …)` with a per-mesh SRV goes away. Keep binding it
    in `SetupFallGeometry`, because other passes clobber high slots. The cost is the same.
-   **Per-draw atlas rectangle in b13**: replace `BakeRect` semantics with
    `AtlasRect = (originX, originY, w, h) / atlasSize` and keep the `BakeRect` mapping from mesh UV
    (unscrolled: `(uv - TexOffset) * InverseScale`, as `Flow()` does at FallGround.hlsli:230) to the
    mesh's 0..1 grid. `BakeAt` (`:203-217`) becomes an atlas `SampleLevel` with a clamp to the rectangle
    shrunk by half a texel, so bilinear never reads a neighbour's texels. The 1-texel guard ring is filled
    by the exchange pass with clamped values.
-   **The depth pre-pass needs the atlas only for discard (translucency) and displacement.** Utility.hlsl
    may not have UVs in the depth permutation (`TexCoord0` exists only `#if defined(TEXTURE)`,
    Utility.hlsl:19). So **put the per-vertex mesh grid coordinate in the per-vertex buffer**: extend
    `FallVertex` (t123) with `float2 gridUV` (0..1 in the mesh's grid, computed once on the CPU from the
    dense copy's raw UVs). Both vertex shaders pass `gridUV` through an interpolant (`FALLGRID0`). Both
    pixel shaders map it to the atlas with b13's `AtlasRect`, the same math in both. This also removes the
    `TexOffset`/`InverseScale` unscrolling from that path.

### 2.3 Vertex displacement: constraints and design

Constraints (LEARNINGS, "Lit geometry must keep its exact vertex position"; "Utility shaders got no feature
defines"; D69):

1.  The z-prepass and the lit pass must compute bit-identical clip positions. Even
    `mul(ViewProj, world)` against `mul(modelView, input)` broke it.
2.  So add the displacement to `input.Position` **before** either shader's transform, from **identical
    data**. D69's `Shift` is exactly this (Lighting.hlsl:157-160, Utility.hlsl:131-134) and was verified
    in game.
3.  The data must not change between the two passes. That is why the sim writes in `EarlyPrepass`
    (section 1.1).

Design:

-   A compute pass `FallDisplaceCS` runs over all dense-copy vertices (one structured buffer of
    `{meshSlot, gridUV, normalMS}` per vertex, uploaded once per mesh). It samples `State.h` (smoothed
    over 3x3 texels) and writes `float4 offsetMS` into **`DisplaceCur`**. Before that, the previous
    frame's buffer becomes **`DisplacePrev`** (swap two buffers).
-   Vertex shaders: `input.Position.xyz += Shift(id) + DisplaceCur[VertexBase+id]`. In Lighting.hlsl,
    `previousInputPosition` gets `Shift(id) + DisplacePrev[VertexBase+id]` instead. That is the TREE_ANIM
    pattern (`previousInputPosition.xyz += normal * treeShiftVector.y`, Lighting.hlsl:163-170). It runs
    through `PreviousWorld` (Lighting.hlsl:190), so `GetSSMotionVector(WorldPosition,
PreviousWorldPosition)` (Lighting.hlsl:2817; MotionBlur.hlsli:8-15) yields the true screen motion of a
    bulging sheet. Utility.hlsl has no previous position, so only the current offset applies there.
-   Normal displacement only (along the vertex normal, in the mesh's space), amplitude
    `clamp(k·(h - h_bake), -8, 8)` world units. Vertices are about 20-50 units apart (one subdivision,
    D74), so only low-frequency swelling can show (thickening over a boulder, a bulge at a hydraulic
    jump). Detail stays in the pixel shader (normals from ∇h).
-   Bindings: `DisplaceCur` at t125 and `DisplacePrev` at t126, VS only, set in `SetupFallGeometry`
    (slots 125-127 are unused today).

What breaks, and how to avoid it:

| Risk                                             | Effect                                                 | Mitigation                                                                                                                             |
| ------------------------------------------------ | ------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------- |
| Sim writes after the z-prepass                   | brown or milky falls (depth EQUAL fails)               | dispatch in `EarlyPrepass`; assert in debug builds with a frame counter written in both hooks                                          |
| Displacement is not added identically            | the same brown                                         | buffer read only, the D69 path; no texture sampling in the vertex shaders                                                              |
| `SV_ClipDistance` (FallClip) uses world position | the pool cut moves with the bulge                      | acceptable: same in both passes                                                                                                        |
| Seams between meshes (D69 moves edges)           | cracks open where adjacent meshes bulge differently    | links make h continuous across a seam; also zero the displacement on open-edge vertices (`FallVertex.a.y < 1` or the D69-shifted ones) |
| Motion vectors                                   | without the previous offset, TAA ghosts swelling water | `DisplacePrev` as above                                                                                                                |
| Shadows                                          | use the previous frame's offset                        | harmless                                                                                                                               |

### 2.4 Previous position in CS and Skyrim, for reference

Non-skinned geometry gets `previousWorldPosition = mul(PreviousWorld, inputPosition)` (Lighting.hlsl:190).
`PreviousWorld` is the engine's per-geometry previous transform in b1 `PerGeometry` (`packoffset(c3)`,
Lighting.hlsl:122). Skinned geometry uses `PreviousBones` (Lighting.hlsl:179-182). Tree animation uses a
second timer (`WindTimers.y`). So the engine never knows about our vertex motion. Anything we add must
supply its own previous value, which is the design above. The falls' **texture** motion has no motion
vector at all (the surface is static, the texture scrolls). That is already true at HEAD, and TAA's
neighbourhood clamp handles it.

---

## 3. Per-mesh adjacency: chart borders, cross-mesh links, ghost cells

### 3.1 Within a mesh (UV chart borders)

`StartBake` (FallSites.cpp:2261-2360 HEAD) rasterises triangles into the grid by UV and assumes the UV map
is injective. Two cases break that:

-   **Overlapping UV islands** (tiled or mirrored pieces): two triangles write the same texel with
    different world positions, and the later one silently wins. Detect it while rasterising: keep a write
    count and the first position; flag overlap when the positions differ by more than 2 texels of world
    distance. Per mesh, if overlapping texels exceed 2%, split the mesh into charts (connected components
    of triangles in UV), each with its own atlas rectangle and its own `MeshInfo`. The per-vertex
    `gridUV` then refers to its chart.
-   **UV seams inside one chart set**: a mesh edge shared by two triangles whose UVs differ at the shared
    vertices. These are the same world vertices (a seam). Walk the index buffer once: for each seam edge,
    sample N points along it (one per texel) and emit **intra-mesh links** pairing the texel on side A
    with side B.

### 3.2 Across meshes (cascades, side-by-side sheets)

Today's world-proximity logic:

-   `LinkBakes` (FallSites.cpp:3010) finds, per mesh, the feeder whose lowest `LINK_ROWS` (10) rows lie
    within `LINK_REACH` (48) of its top edge, with per-column links `{column, feeder x, feeder y}`
    (`Node::links`).
-   `ComputeFades` (`:2092`) and the D69 gap closer pair open-edge vertices with other meshes' surfaces or
    edges (`FADE_REACH` 32, `GAP_REACH` 40).
-   D72's world grid (32 units) paired texels of different meshes within 40 units, removed in D77.

Generalise these into one GPU **link buffer**, built on the CPU at link time:

```
struct Link {            // 32 bytes
  uint dstTexel;         // atlas texel (packed x,y) of the receiving ghost or inflow cell
  float2 srcAtlas;       // atlas position (texels, fractional) in the source mesh: bilinear
  float4 frame;          // 2x2 map from source texel axes to destination texel axes (for velocity)
  uint kind_weight;      // kind: feed (top edge), side (open edge), seam (intra-mesh); weight 0..1 (fp16)
};
```

-   Build the frames from world tangents: `frame = [tu_dst tv_dst]⁺ · [tu_src tv_src]`, a 2x2 least
    squares fit.
-   **Feed links** come from `LinkBakes`' per-column pairs. They set the destination's inflow: h, the
    velocity mapped through the frame, S continued (the feeder's S mapped to this mesh's u range: keep
    D67's convention that S is in the destination's own texture u), and T continued (`T_dst = T_src`, so
    run time and aeration flow on).
-   **Side links** come from the `ComputeFades` and D69 pairs. They write ghost cells, so water spreading
    sideways crosses into the neighbour and two sheets merge (what D72 tried statically).
-   **Seam links** come from section 3.1.
-   Pass: `Exchange` runs `Dispatch(ceil(links/64))` at the start of each substep, reads `State`/`Coord`
    (the current buffer) and writes the destination texels of the same buffer before velocity advection.
    Destinations are ghost or boundary cells that no other pass writes in that phase, so there is no
    race. Lag is at most one substep, which is fine for a steady flow.
-   Mass: feed links carry flux (h·v·width), not just h. Scale the inflow so that flux in equals the
    feeder's flux out at its foot. This prevents water from being created or destroyed at each cascade
    step.

### 3.3 What the bake keeps doing

Still on the CPU, once per mesh: rasterisation, ray rock (`BakeStep`), mesh-parity rock (`GatherSolids`
`:2362`, `InsideSolid` `:2464`, D68/D76), the metric, and the steady solve (`SolveBake`, `:2525`) as the
warm start. Linking moves from "rewrite the bake's fields" to "emit links". D67's carry-over of S and T
through cascades becomes a property of the live sim.

---

## 4. Coupling on the GPU: rock, terrain, actors, pools

-   **Rock and terrain** are static inputs. They stay as the bake's `rock` mask (ray parity from both sides
    of the sheet, `BakeStep`; the statics' meshes, `InsideSolid`; covers over lying texels, D76), stored as
    flag bits in `Static0`. Wall faces are those between a water texel and a rock or off-mesh texel. Add a
    **"near rock" distance** (texels to the nearest rock, a CPU distance transform) to drive foam and the
    D71 strong turns without searching neighbours on the GPU.
-   **Actors**: build a small structured buffer of capsules `{p0, p1, radius, velocity}` per frame on the
    CPU from the same sources `WakeSources` and `AirSources` walk (the player plus high-process actors,
    nearest first; `WakeSources.h:24-46`). They currently emit in SWE grid texels, so a world-space list
    is a new, small function on the same history. Cap it at 16. Cull per mesh on the CPU (capsule AABB
    against the mesh AABB → a 16-bit mask in `MeshInfo`), so most texels test 0-2 capsules. In the
    velocity pass, a texel inside a capsule (by world position from `Static0`) becomes a moving wall: its
    velocity relaxes to the actor's tangential velocity, water is pushed out sideways, and foam and spray
    strength jump. Result: walking behind or through a curtain parts it, and standing in a cascade makes a
    pillow of white water.
-   **Falls → SWE (plunges)**: today the SWE gets CPU emitters (`AppendWaterEmitters`,
    FluidField.cpp:1306 HEAD) and a CPU-built plunge map (`BuildPlungeMap` → `fallPlungeSRV`, read
    through `params.plungeMap`, `:1327`). Replace the map's producer with a GPU **foot splat** pass. Each
    outflow texel (flag) with flux > 0 adds `flux` and `flux·velocity_world.xy` into an R32_UINT
    fixed-point world grid with the same lattice (`FALL_GROUND_CELL`, `FALL_GROUND_SIZE`) using
    `InterlockedAdd`. A resolve pass converts that to the RGBA16F the SWE forces pass samples (strength,
    and the push direction it now gets from emitters). The SWE runs later in the same frame (`Prepass`),
    so it sees this frame's plunge. D55's rule stays: drive flow and foam, never dent the surface.
-   **SWE → falls (pools)**: foot texels whose world z is below the local SWE surface (the SWE's surface
    SRV, sampled at world xy, with the vanilla plane as fallback, i.e. `CollectWaterPlanes`/`Pool()`) are
    outflow sinks. Their h relaxes to 0 and their foam rises (plunge foam on the sheet side). The SWE
    surface comes from the previous frame, a one-frame lag that is fine. This replaces the static
    `WATER_CUT_BAND` logic for "where does the sheet end" with a dynamic one. The visual cut (FallClip)
    can stay as it is.
-   **Wind** (later): the wind solver's velocity at the texel's world position as a small tangential drag
    on thin, fast sheets (spray drift).

---

## 5. Rendering upgrades a live sim enables

### 5.1 Texture advection without pulsing

Lessons on record: never `rate * time` (LEARNINGS 2026-10-17, D60); integrated phase; two-phase
cross-fades marbled (D58, D59); D67's (S, T) runs streaks along the flow with only the scroll moving in
time. The prior research says the same: a static warp is a stream-function coordinate
(`techniques-deep.md` §0), and advected textures with regeneration were rejected for curtains (§(a)(1)).

With a live sim, **advect D67's coordinates, not the texture**:

-   `S` is a passive scalar: `S ← S(x - v dt)`, with inflow S = the column's u (or the feeder's S through
    a link). In steady flow, v·∇S = 0, so S is the stream function D60/D67 computed by sweeping.
-   `T` is age: `T ← T(x - v dt) + dt`, with inflow T = 0 (or the feeder's T). In steady flow, v·∇T = 1,
    so T is D67's run time.
-   The shader reads `uv = (S, T·c - phase)`, with `phase = ∫c dt` (the existing `fallPhase`,
    `FluidField.cpp:846-860` HEAD) and c the shared scroll rate. A feature at fixed (S, T·c) moves exactly
    with the water. There is no reset and no regeneration: T is bounded by the residence time on the mesh
    (water leaves at the foot), so precision is fine.
-   Changing c moves the texture by Δc·T. T is bounded (a few seconds), unlike `rate * session time`, so
    the shift is small. Still, keep c constant or slowly averaged, as now.
-   Numerical diffusion of semi-Lagrangian bilinear advection smooths S across streamlines. That only
    smooths _where_ streaks bend, not the texture itself, and it is bounded by the residence time. Use
    MacCormack/BFECC for S if the bends look too soft. Clamp folding the way D70 does (S monotone along
    rows) in the output pass, not in the state.
-   Merges (two streams meeting with different T) give a seam in T. Small diffusion of T (one Jacobi blur
    per frame) softens it, and the merge line is exactly where foam belongs (`techniques-deep.md` §0's
    rule: put whitewater on the singular zones).
-   Optional accent: Lagrangian texture particles (Yu, Neyret, Bruneton, Holzschuch 2011 [3]) for the foam
    layer only.

### 5.2 Thickness → translucency

**Why the D53-D55 blended lit falls drew nothing** (unresolved; ranked hypotheses to check in RenderDoc,
which is untested on this machine according to `kb/runtime-capabilities.md`):

1.  **Routing happens at setup, not per draw.** The accumulator and batch renderer choose the opaque or
    blended group from the property and alpha state when the geometry is set up (BSShaderProperty flags
    pushed by `FinishSetupGeometry`, kb `2026-10-18-runtime-geometry.md`; vfunc 0x28). Swapping the
    `NiAlphaProperty` on a live geometry (D53/D54) leaves it registered as opaque. Then the opaque path
    draws it with a blend state whose source alpha is the shader's output, while the depth pre-pass was
    hidden too (D77 lists "the hidden depth pass"). That gives an invisible or depth-failed draw.
    `history.md` already noted "needs registering with the blended pass". **Cheap test:** D73's dense
    copies are built by us, so attach the 0x10ED alpha property _before_ `SetupGeometry` and
    `FinishSetupGeometry` on the copy.
2.  **Deferred alpha**: in the deferred path the lit shader's `Diffuse.w` also goes to
    `MotionVectors.w` (Lighting.hlsl:3141-3142), and CS overrides blend states for targets 1-2 to
    SrcAlpha/InvSrcAlpha (`Deferred.cpp:490-500`). Albedo, normals and roughness for a blended pixel are
    still overwritten or half-blended, and the composite relights the result as opaque. Even if it drew,
    the result would be wrong.
3.  The base texture's alpha or vertex alpha is 0 on the inner falls meshes (the alpha channel is unused
    when opaque).

Even with (1) fixed, blended lit falls are forward-shaded: no deferred GI, SSR or wetness, sorting against
the effect layers and spray, and no prepass. **Recommendation: hashed stochastic transparency in both
passes**, which needs nothing new in the pipeline:

-   `alpha = 1 - exp(-σ·h)`, raised toward 1 by foam and aeration, with a minimum per mesh (vanilla look).
-   Threshold from a stable hash of (pixel, frame), shared by Utility.hlsl and Lighting.hlsl, with the
    frame index frozen in `EarlyPrepass` (section 1.1). Use interleaved gradient noise or blue noise with
    a golden-ratio frame offset (Wyman & McGuire's hashed alpha testing [5]; Enderton et al.'s stochastic
    transparency [4]). D67's `Dithered` is this already, so extend it to take an alpha.
-   TAA (or the upscaler) resolves the noise into translucency. The rock behind is in depth and the
    G-buffer, so it shows lit correctly. Discarded pixels write the background's motion vectors, which is
    correct.
-   Costs: discard in the lit pass kills early-Z for the falls (the pre-pass already discards, D67). With
    TAA off it looks noisy, so fall back to opaque (alpha floor 1) when TAA is disabled.
-   Both passes must compute the same alpha. Both sample the same atlas texel from the same interpolated
    `gridUV`. The interpolants come from the same per-vertex buffer data, and the pixel position is the
    same, so the results match.

### 5.3 Foam layer: an effect-shaded copy offset along the normal

-   Build a second engine copy per dense mesh (the D62/D73 builder `77465`, same indices and UVs) with a
    `BSEffectShaderProperty` (ctor `106872`) and an `NiAlphaProperty` 0x10ED attached **before** setup
    (the decal recipe, kb runtime-geometry). Effect shaders are blended natively and drawn after the
    deferred passes, so there is no depth-EQUAL constraint and no prepass.
-   Effect.hlsl gets back a small FLUID_FIELD path (D77 removed the old one): the vertex shader adds
    `normal · (h + 1.5)` from `DisplaceCur` (this copy's own vertex range; same buffer layout). The pixel
    shader reads the atlas foam, `alpha = smoothstep(foam)`, and the texture from (S, T). LEARNINGS:
    apply alpha after the palette block, and the effect vertex shader cannot use b12 (use
    `PerGeometry.PosAdjust`).
-   Motion vectors: effect blends do not write them, the same as vanilla's white-water layers.

### 5.4 Spray from the sim

`FallSurfaceRenderer` draws stateless particles from a `StructuredBuffer<Emitter>` (FallSpray.hlsl, t100;
48-byte `FallEmitter`, FallSurfaceRenderer.h). Keep that. On the CPU, place fixed **emitter slots** per
mesh once (lip rows, rock rims from the near-rock distance, the foot, every few texels). Each frame the
output pass writes `Strength`, `Direction` and `Speed` for each slot from the live sim (a
RWStructuredBuffer aliasing the emitter buffer: a DEFAULT buffer with UAV and SRV views). Strength comes
from foam generation, |v| and actors. The draw count stays `slots × sprayPerEmitter`, with no indirect
draws and no readback. An actor stepping into a fall throws spray where they stand.

---

## 6. Data structures (C++)

```cpp
namespace FluidFieldSim {
  struct FallMeshInfo {          // StructuredBuffer, one per atlas slot (matches FallSim.hlsli)
    uint32_t atlasOrigin[2];     // texels
    uint32_t size[2];            // w, h
    float    uvMin[2], uvInv[2]; // mesh UV -> 0..1 grid (BakeRect today)
    float    dtScale;            // 0 asleep, 1, 2 (LOD)
    uint32_t flags;              // awake, warmup, hasFeeder
    uint32_t linkFirst, linkCount;
    float    inflowSpeed;        // texels/s at the top band (SIM_INFLOW / |tv|)
    uint32_t actorMask;          // capsules touching this mesh
    float    pad[2];
  };
  struct FallLink { uint32_t dst; float src[2]; float frame[4]; uint32_t kindWeight; };   // 32 B
  struct FallCapsule { float p0[3]; float radius; float p1[3]; float pad; float velocity[3]; float pad2; };

  class FallSurfaceSolver {     // standalone like ShallowWaterSolver (WARP-testable)
  public:
    void CreateResources(ID3D11Device*, uint32_t atlasW, uint32_t atlasH, const NameFn&);
    bool CompileShaders(const CompileFn&);
    void UploadMesh(ID3D11DeviceContext*, uint32_t slot, const FallMeshStatic&);   // Static*, Bake, vertices
    void SetLists(ID3D11DeviceContext*, std::span<const FallMeshInfo>, std::span<const uint32_t> tiles,
                  std::span<const FallLink>, std::span<const FallCapsule>);
    template <class Wrap> void Step(ID3D11DeviceContext*, float dt, Wrap&&);    // Exchange, Velocity, Height, Coords
    void WriteOutputs(ID3D11DeviceContext*);  // Render, emitters, foot splat, displacement (swap prev/cur)
    ID3D11ShaderResourceView* GetRenderSRV() const;
    ID3D11ShaderResourceView* GetDisplaceSRV(bool previous) const;
    ID3D11ShaderResourceView* GetPlungeSRV() const;   // for WaterParams::plungeMap
  };

  class FallAtlas {             // CPU allocator: 32x32 blocks, first-fit bitmap
  public:
    std::optional<Rect> Allocate(uint32_t w, uint32_t h);
    void Free(const Rect&);
  };
}
```

FallSites keeps producing `BakeField` (positions, normals, metric, rock, steady state) and gains
`GetLinks()`. FluidField owns the solver and the atlas, maps `const BSGeometry*` to a slot, and in
`SetupFallGeometry` sets `AtlasRect`, `VertexBase` and the t124/t125/t126 bindings.

Shader side: `FallSim.hlsli` (the shared CB, MeshInfo and flags) and `FallSimExchangeCS`,
`FallSimVelocityCS`, `FallSimHeightCS`, `FallSimCoordsCS`, `FallSimOutputCS`, `FallDisplaceCS` and
`FallPlungeSplatCS` in `features/Fluid Field/Shaders/FluidField/`. FallGround.hlsli's `BakeAt` and `Flow`
read the atlas.

---

## 7. Phased plan (smallest testable steps first)

Each phase ends in one Skyrim round with A/B captures. Bundle the shader and C++ changes per phase (memory:
larger iteration steps), and quit the game before every build.

**Phase 0 — verify the frame order (no visual change).** Add a debug counter: increment in `EarlyPrepass`
(new override) and `Prepass`. In `SetupFallGeometry` log, once per second, which counter values the first
Utility draw and the first Lighting draw of a grounded mesh see. Expected (L1): Utility draws see
`early == N, prepass == N-1`. If confirmed, fix the dither frame (freeze `DitherFrame` in `EarlyPrepass`
and use it in `Dithered`). Seams may get sharper or cleaner, so compare D67/D69 seams on the test falls.
Record the result in LEARNINGS and in the kb render-integration dossier (it moves to L2).

**Phase 1 — atlas replay of the current bake (looks identical to D77).**

-   `FallAtlas` plus a `Render` atlas texture. On bake ready, allocate and upload `FlowBake.texels` (RGBA,
    D67's S, T, wake, foam) into its rectangle, instead of creating one SRV per mesh
    (`fallBakeSRVs`, FluidField.cpp:1510-1530 HEAD).
-   b13: `AtlasRect`. t124 is the atlas. `BakeAt` samples within the rectangle.
-   Free on removal, and on the loading-screen reset.
-   Test: debug mode 1 and screenshots against D77 in the same view; profiler shows no new cost. Menu: atlas
    use (blocks used out of total).
-   Value: proves allocation, streaming and the binding path, with zero physics risk.

**Phase 2 — the live sim, shading only (no displacement, no translucency).**

-   `FallSurfaceSolver` with `Static*`, `State`, `Coord`, the tile list and the four passes. Dispatch in
    `EarlyPrepass` with the profiler wrap. Warm start from `Bake`.
-   The output pass writes `Render` = (S, T, wake from the near-rock distance, foam). The shading code is
    unchanged.
-   Test: at rest it should match phase 1 (the steady state equals the bake). Then `sgtm 0.1` motion
    checks (LEARNINGS: judge flow in motion; phase correlation). Budget under 0.5 ms. A `tests/FluidField`
    WARP case against `simbake.py` dumps (`FluidField_bakeN.bin`).

**Phase 3 — links.** Feed links from `LinkBakes`, then side and seam links. Test: S and T continuous
across cascade seams (debug view of T as a colour ramp), with no mass jump at seams (integral of flux per
mesh in a debug readback, debug builds only).

**Phase 4 — actors and plunges.** Capsule buffer; actor walls, foam and spray. The GPU foot splat
replaces `BuildPlungeMap` as the SWE's `plungeMap`. Test: walk through the test fall's curtain; the
plunge foam in the SWE follows the falls' live flux.

**Phase 5 — translucency (hashed).** `alpha(h, foam)` in both passes via `gridUV` in the per-vertex
buffer, with the frozen dither frame. Test with TAA, DLSS or FSR, and with them off (fallback opaque).

**Phase 6 — displacement.** `FallDisplaceCS` and `DisplaceCur`/`DisplacePrev`, added in both vertex
shaders, with the previous offset in Lighting. Test: no brown or milky pixels (the D69 checklist), and a
motion-vector debug view without ghosting on swelling water.

**Phase 7 — foam layer and spray from the sim.** The effect copy (5.3) and GPU-written emitter strengths
(5.4).

**Phase 8 — LOD, sleeping, bake LRU cache.** `lastDrawnFrame`, `dtScale`, eviction, and the cache keyed by
reference and node path. Test: walking away from and back to Whiterun-route falls re-wakes them without a
visible change, and a cell reload does not re-ray-cast.

---

## 8. Risks

| Risk                                                                                                      | Likelihood | Impact                                                   | Mitigation                                                                                                                                                    |
| --------------------------------------------------------------------------------------------------------- | ---------- | -------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Frame order differs from L1 reading (z-prepass after `Prepass`)                                           | low        | the plan still works (EarlyPrepass is earlier than both) | phase 0 measures it                                                                                                                                           |
| Overlapping UV islands on some vanilla falls meshes                                                       | medium     | water teleports between pieces                           | overlap detection, then chart split (3.1)                                                                                                                     |
| Explicit film unstable where the metric degenerates (sliver triangles, folded lips: D76's "frame twists") | medium     | spikes or NaN                                            | clamp the inverse metric (`det < 1e-6` → wall, as SolveBake does), clamp speed, guard NaN in the output pass                                                  |
| Live flow drifts from the hand-tuned D74/D76 look (turn limits, lip easing, capped stretch 1.6)           | high       | "it was better before"                                   | warm start from the bake; keep D76's caps in the output mapping (stretch cap, lip easing rows); A/B toggle "Live sim" against the baked replay (phase 1 path) |
| Atlas memory on low VRAM                                                                                  | low        | 30-60 MB                                                 | start at 512x1024; RG16F coordinates with a per-mesh T offset                                                                                                 |
| Hashed transparency looks noisy without TAA                                                               | medium     | sparkle                                                  | opaque fallback; alpha floor                                                                                                                                  |
| Many dispatches in `EarlyPrepass` delay shadows or other features                                         | low        | sub-ms                                                   | tile lists, LOD                                                                                                                                               |
| Async shader compile shows stale shaders for 5-10 s after loading (LEARNINGS)                             | certain    | misjudged captures                                       | wait 10 s before captures                                                                                                                                     |
| Concurrent edits (D78 retiring t122/`FallStream`)                                                         | —          | merge conflicts                                          | this design does not use t122 or the top-down stream map                                                                                                      |

## Sources

1.  H. Wang, G. Miller, G. Turk, "Solving General Shallow Wave Equations on Surfaces", SCA 2007.
    [project page](https://faculty.cc.gatech.edu/~turk/paper_pages/2007_shallow_waves/index.html),
    [EG digital library](https://diglib.eg.org/items/501094cb-b514-44a5-aa1d-5a5728d115f3/full)
2.  X. Mei, P. Decaudin, B.-G. Hu, "Fast Hydraulic Erosion Simulation and Visualization on GPU" (virtual
    pipe model), Pacific Graphics 2007. [hgpu](https://hgpu.org/?p=2256),
    [slides](https://slidetodoc.com/fast-hydraulic-erosion-simulation-and-visualization-on-gpu/)
3.  Q. Yu, F. Neyret, E. Bruneton, N. Holzschuch, "Lagrangian Texture Advection: Preserving both Spectrum
    and Velocity Field", IEEE TVCG 17(11), 2011.
    [author list](https://artis.inrialpes.fr/Members/Nicolas.Holzschuch/PublicationsACM.html)
4.  E. Enderton, E. Sintorn, P. Shirley, D. Luebke, "Stochastic Transparency", I3D 2010.
    [paper](https://www.cse.chalmers.se/~d00sint/StochasticTransparency_I3D2010.pdf),
    [TVCG version](https://research.nvidia.com/sites/default/files/pubs/2011-08_Stochastic-Transparency/stochtransp-tvcg.pdf)
5.  C. Wyman, M. McGuire, "Hashed Alpha Testing", I3D 2017.
    [paper](https://research.nvidia.com/sites/default/files/pubs/2017-02_Hashed-Alpha-Testing/Wyman2017Hashed.pdf)
6.  J. Jylänki, "A Thousand Ways to Pack the Bin" (2010), with code at
    [juj/RectangleBinPack](https://github.com/juj/RectangleBinPack)
7.  A. Vlachos, "Water Flow in Portal 2", SIGGRAPH 2010 Advances course (two-phase flowmaps, which we
    moved away from).
    [slides](<https://www.advances.realtimerendering.com/s2010/Vlachos-Waterflow(SIGGRAPH%202010%20Advanced%20RealTime%20Rendering%20Course).pdf>)
8.  Internal: `techniques-deep.md` (stream-function coordinates), `techniques-survey.md`, `history.md`,
    `vanilla-nif-dissection.md`; `skyrim-engine-re/kb/evidence/2026-10-18-runtime-geometry.md` (engine
    builder, property recipe, `FinishSetupGeometry`).
