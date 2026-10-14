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
-   Alternatives rejected: Prototype A (top-down depth capture through the
    precipitation-occlusion path) is deferred, not rejected. It is the
    documented fallback, built only if B fails the success criteria on the
    test routes. The other rejected options are listed in `research/R1.md`
    section 4.
-   Decided by: @Victor
-   References: `research/R1.md`
