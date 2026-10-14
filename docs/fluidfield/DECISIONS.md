# Fluid Field — Decisions Log

One entry per decision. Research spikes (R1–R9) end with an entry here that
points to `research/R<n>.md`; build agents start from these entries, never from
their own guess about which technique to use.

Entry format:

```
## D<n>: <title>
- Date: YYYY-MM-DD
- Phase / spike: Phase <n> / R<n>
- Decision: <what was chosen>
- Alternatives rejected: <option — why>
- Decided by: <person>
- References: <research note, report, PR>
```

## D1: Branch from `dev`, not `main`

-   Date: 2026-10-13
-   Phase / spike: Phase 0
-   Decision: `feature/fluid-field` branches from `dev`.
-   Alternatives rejected: `main` — only updated by the release workflow and
    135 commits behind `dev`; upstream PRs target `dev`.
-   Decided by: @Victor
-   References: `.claude/CLAUDE.md` "Commits and branches"

## D2: AI context files

-   Date: 2026-10-13
-   Phase / spike: Phase 0
-   Decision: There is no `AI-INSTRUCTIONS.md` in this fork. `.claude/CLAUDE.md`
    is the primary AI instruction file, with `AGENTS.md`,
    `.github/copilot-instructions.md` and `CONTRIBUTING.md` as supporting
    guidance.
-   Alternatives rejected: creating a new `AI-INSTRUCTIONS.md` — would
    duplicate `.claude/CLAUDE.md`.
-   Decided by: @Victor
-   References: `docs/fluidfield/ROADMAP.md` "Working with AI agents"

## D3: Deploy builds straight into the Steam install

-   Date: 2026-10-13
-   Phase / spike: Phase 0
-   Decision: Local builds auto-deploy to the plain Steam install's `Data`
    folder (no mod manager), overwriting the installed Community Shaders, with no
    backup.
-   Alternatives rejected: backing up first; staging to a separate folder.
-   Decided by: @Victor
-   References: `CMakeUserPresets.json` (local, untracked)

## D4: Shape of the Phase 0 scaffold

-   Date: 2026-10-13
-   Phase / spike: Phase 0
-   Decision: Fluid Field follows LOD Blending, the smallest feature with
    saved settings: `src/Features/FluidField.{h,cpp}`, a `Settings` struct
    serialized with `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT`, and
    registration in `Globals.h`, `Globals.cpp` and `Feature.cpp`. Unlike LOD
    Blending it has no `CORE` marker, so it gets its own `Package-FluidField`
    zip as the roadmap asks. The `.ini` is `Version = 0-1-0`, `Alpha = True`.
    `Settings::Enabled` defaults to `false`. No shader define, no hooks, no
    per-frame overrides and no GPU resources.
-   Alternatives rejected: CORE feature (it would ship with the base mod);
    `Unreleased = True` (CONTRIBUTING.md reserves it for finished features that
    cannot ship yet); a shader define (it would add shader permutations for
    nothing).
-   Decided by: agent, pending review by @Victor
-   References: `docs/new-feature-template/NewFeatureReadme.md`,
    `src/Features/LODBlending.{h,cpp}`

## D5: Move Fluid Field to its own "Fluids" category (planned)

-   Date: 2026-10-13
-   Phase / spike: later phase, not yet scheduled
-   Decision: Fluid Field will move from `FeatureCategories::kWater` to a new
    "Fluids" category. That means adding `kFluids` to
    `src/FeatureCategories.h`, plus any i18n key the category label needs,
    and changing `FluidField::GetCategory()`.
-   Alternatives rejected: staying under Water. Fluid Field also drives fog,
    smoke and buoyancy.
-   Decided by: @Victor
-   References: `src/FeatureCategories.h`, `src/Features/FluidField.h`

## D6: Phase 0 gate passed

-   Date: 2026-10-13
-   Phase / spike: Phase 0 gate
-   Decision: Phase 0 is closed and Phase 1 can start with spike R1. The
    in-game checklist passed with no issues. The empty Fluid Field feature
    loads, appears in the menu, saves its setting, toggles cleanly and costs
    nothing when off.
-   Alternatives rejected: none.
-   Decided by: @Victor (in-game test)
-   References: build `ca4a6a665`, deployed to the Steam install

## D7: R1 — adopt the CPU data map (Prototype B)

-   Date: 2026-10-13
-   Phase / spike: Phase 1 / R1
-   Decision: Build the depth and obstacle map from CPU data, as described
    for Prototype B in `research/R1.md`: landscape heights from
    `TESObjectLAND`, water surfaces from `TESWaterSystem::waterObjects` with
    the cell-water fallback, and downward raycasts for statics. Build it as
    real Phase 1 code behind a debug setting, not as a throwaway prototype.
    The section 8 measurements and success criteria are its gate. Stages,
    each with a commit and an in-game checklist:
    1. Cell cache of landscape heights, including the absolute-Z check
       against `TES::GetLandHeight`.
    2. Water surfaces from `waterObjects`, with the cell-water fallback.
    3. Debug overlay of bed, surface and depth. Stop here so a human can
       judge how much riverbed on the test routes is landscape versus
       placed meshes (R1 open question 3).
    4. Downward raycasts on the main thread under `bhkWorld::worldLock`,
       with a per-frame ray cap. Only after stage 3 is reviewed.
       The map reuses the toroidal grid and addressing from `ff/grid-core`
       instead of a second implementation.
-   Amended 2026-10-14 (@Victor): the stage 3 debug overlay (bed, water
    surface, depth) is built together with stage 2, so progress is visible in
    game early. It is the layer overlay's second row. The stage 3 review of
    landscape against placed riverbed meshes still happens on the test routes
    before stage 4.
-   Alternatives rejected: Prototype A (top-down depth capture through the
    precipitation-occlusion path) is deferred, not rejected. It is the
    documented fallback, built only if B fails the success criteria on the
    test routes. The other rejected options are listed in `research/R1.md`
    section 4.
-   Decided by: @Victor
-   References: `research/R1.md`

## D8: First visible milestone is a "flowing water slice"

-   Date: 2026-10-13
-   Phase / spike: Phase 1–3 bridge, ahead of R2
-   Decision: Build a "flowing water slice" as the first visible result. Goal:
    in Riverwood's river, water visibly flows, the player's wake drifts
    downstream, and waves reflect off the banks. Scope:
    -   Bed and surface from R1's CPU map (D7), landscape only; raycasts
        come later.
    -   A basic virtual-pipe shallow-water solver on the grid core, not the
        full R2 spike. River currents are seeded from vanilla water flow if
        it is readable, otherwise a constant test current.
    -   The player is the only input.
    -   Output normals, plus flow if the water shader can take it, into the
        water shader, fading to vanilla near the grid edge.
    -   A menu toggle, Tracy zones and a reset after loading screens.
    -   A clean interface (map and inputs in; height, flow and normals out)
        so the R2 solver can replace the basic one later.
-   Refinements (2026-10-13, same day):
    -   Principle: extend what Community Shaders and Skyrim already do; do
        not reimplement it.
    -   Order: build the slice right after D7 stage b, once `ff/grid-core`
        has merged. The slice's debug overlay doubles as the stage c review
        (how much riverbed is landscape). Raycasts (stage d) come after the
        slice.
    -   Currents: seed from Unified Water's world flowmap through a one-line
        read-only accessor in `UnifiedWater.h` (approved). Use a constant
        current when Unified Water is off.
    -   Blending in `GetWaterNormal` (`package/Shaders/Water.hlsl`):
        -   Add Fluid Field's large-scale wave normals on top of the base and
            flow normals with `ReorientNormal`.
        -   Keep vanilla wading ripples. Tune the player input so Fluid Field
            does not draw rings at the same scale.
        -   Keep Wetness Effects raindrops, but drift them with Fluid
            Field's flow where it covers the water.
        -   Sample at the Water Parallax-shifted position.
        -   Fade everything to vanilla near the grid edge.
        -   Limit the `FLUID_FIELD` define to the water shader.
    -   Vanilla `ISWaterFlow` actor wakes only exist with Unified Water off.
        Check in game whether they double Fluid Field's wake.
-   Alternatives rejected: a "ripple slice" (normals-only ripples). It
    cannot show flow, wakes carried downstream or reflections, which vanilla
    displacement cannot do either. No ripple-slice plan had been written
    down in this log before this entry.
-   Decided by: @Victor
-   References: `reports/unified-water.md` sections 5.1, 5.2 and 6;
    `research/R1.md`; D7

## D9: Community Shaders Discord builds are reusable with credit

-   Date: 2026-10-13
-   Phase / spike: all
-   Decision: Code shared on the Community Shaders Discord, starting with
    Dwemer Engineer's Procedural Grass test build, is treated as Community
    Shaders code under GPL-3.0-or-later. Fluid Field may study it and reuse
    it. Reused code keeps a credit to its author in the file header and the
    commit message. Shipped shader source is the first material to work from.
    Amended the same day: the build's DLL may also be decompiled with
    Ghidra, using its shipped PDB, scoped to the Procedural Grass code.
    Decompiled output is reference only. Anything reused is rewritten,
    with credit.
-   Alternatives rejected: study only, copying only after the author
    publishes the source or gives permission.
-   Decided by: @Victor
-   References: `ROADMAP.md` "License obligations",
    `reports/procedural-grass.md`

## D10: Phase 5 becomes simulated fire and smoke

-   Date: 2026-10-13
-   Phase / spike: Phase 5 / R5
-   Decision: Phase 5 is widened from "volumetric smoke" to fire and smoke
    simulated as fluids, in the spirit of FluidNinja LIVE. Near the player,
    the most visible emitters (campfires, forges, chimneys, torches, fire
    spells) get small local fluid grids with velocity, density and
    temperature, driven by the field's wind and shouts. These replace the
    vanilla sprites, which fade back in beyond a distance band and an
    emitter cap.
    -   Vanilla data stays the source: emitter positions, sizes and colours.
    -   Community Shaders' lights do the lighting.
    -   Particles (sparks, embers) are an optional extra, not the core.
    -   R5 is rescoped to compare the techniques (camera-facing 2D cards,
        extruded volumes, small 3D grids) and to measure a fire budget.
    -   The 1.5 ms fog-and-smoke budget is unchanged until R5 reports.
-   Alternatives rejected:
    -   Smoke-only volumes with vanilla fire left untouched.
    -   Only distorting or bending the vanilla sprites with the field
        (`research/smoke-and-fire.md` options a and b). These may still ship
        as cheap fallbacks for emitters beyond the cap.
-   Decided by: @Victor
-   References: `research/smoke-and-fire.md`,
    `research/fluidninja-live2.md`, `ROADMAP.md` Phase 5 and R5

## D11: Phase 1 grid and wind solver shape

-   Date: 2026-10-13
-   Phase / spike: Phase 1 (grid core; R1 still open)
-   Decision: The grid is centred on the player's position, snapped to whole
    cells, and addressed toroidally: world cell `c` always lives in storage
    texel `c mod GridRes`, so scrolling only changes `GridMin`. Resolutions are
    powers of two (64 to 512) so the shaders wrap with a bitmask. Defaults:
    256 x 256 over 4096 units (16 units per cell), 30 Jacobi iterations. Wind is
    a 2D stable-fluids step (add forces, semi-Lagrangian advection, divergence,
    Jacobi pressure from zero each step, projection) on `R32G32_FLOAT` velocity
    and `R32_FLOAT` pressure and divergence. The edge is open: zero-gradient
    velocity, zero pressure. There are no obstacles until R1 lands. The solver
    lives in `src/Features/FluidField/WindSolver.{h,cpp}`, which depends only on
    D3D11 and C++/WinRT.
-   Alternatives rejected: centring on the camera eye like Grass Collision (the
    roadmap says player-centred; third-person offset is small next to the grid);
    warm-starting pressure from the previous step (would need its own scroll
    invalidation and makes the CPU reference harder); 16-bit formats (revisit if
    the 0.5 ms budget is tight).
-   Decided by: agent; approved by @Victor on 2026-10-14
-   Follow-up: starting the pressure solve from zero leaves the wind
    "soft" (30 iterations remove only about half the divergence). Revisit
    reusing last frame's pressure, with scroll-aware clearing of new
    strips, once devbench timings and the in-game look are in.
-   References: `features/Fluid Field/Shaders/FluidField/Common.hlsli`

## D12: Solver referee is a standalone WARP test

-   Date: 2026-10-13
-   Phase / spike: Phase 1
-   Decision: `tests/FluidField` is its own CMake project, not part of the root
    build. It compiles the real `WindSolver.cpp` and HLSL, runs them on a WARP
    device and compares every layer with a CPU implementation (one step from
    rest, then a scrolled second step). It needs no GPU, no game and no vcpkg.
-   Alternatives rejected: Catch2 (an extra framework for one test); an in-game
    self-test button (agents cannot run it); an option in the root
    `CMakeLists.txt` (that file is not gersemi-clean on `dev`, so touching it
    drags unrelated reformatting into the commit).
-   Decided by: @Victor (chose the WARP exe), layout by agent
-   References: `tests/FluidField/CMakeLists.txt`

## D13: Third-party mods, and the fire, smoke and fog target

-   Date: 2026-10-14
-   Phase / spike: Phases 2–5
-   Decision:
    -   SurfaceTides is not relevant to Fluid Field.
    -   Particle Wind is set aside for now; Fluid Field uses its own wind.
        Detecting it to avoid pushing particles twice is revisited later.
    -   Wade In Water Redone is studied for actor submersion and inputs
        (`reports/wade-in-water.md`).
    -   Embers XD and similar fire mods are card and particle effects, not
        simulations. Phase 5 replaces such effects near the player rather
        than integrating with them; they remain test targets.
    -   Target for the later phases: realistic, simulated-looking fire, smoke
        and fog within a set distance of the player, with the distance to be
        decided. Beyond it, vanilla (or replacer) effects stay.
-   Alternatives rejected: depending on Particle Wind for wind on particles;
    detecting fire replacers by file path.
-   Decided by: @Victor
-   References: `research/existing-mods.md`, D10
