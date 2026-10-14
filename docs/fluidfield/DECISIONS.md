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

## D7: Phase 1 grid and wind solver shape

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
-   Decided by: agent, pending review by @Victor
-   References: `features/Fluid Field/Shaders/FluidField/Common.hlsli`

## D8: Solver referee is a standalone WARP test

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
