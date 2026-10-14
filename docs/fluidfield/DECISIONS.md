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
- Date: 2026-10-13
- Phase / spike: Phase 0
- Decision: `feature/fluid-field` branches from `dev`.
- Alternatives rejected: `main` — only updated by the release workflow and
  135 commits behind `dev`; upstream PRs target `dev`.
- Decided by: @Victor
- References: `.claude/CLAUDE.md` "Commits and branches"

## D2: AI context files
- Date: 2026-10-13
- Phase / spike: Phase 0
- Decision: There is no `AI-INSTRUCTIONS.md` in this fork. `.claude/CLAUDE.md`
  is the primary AI instruction file, with `AGENTS.md`,
  `.github/copilot-instructions.md` and `CONTRIBUTING.md` as supporting
  guidance.
- Alternatives rejected: creating a new `AI-INSTRUCTIONS.md` — would
  duplicate `.claude/CLAUDE.md`.
- Decided by: @Victor
- References: `docs/fluidfield/ROADMAP.md` "Working with AI agents"

## D3: Deploy builds straight into the Steam install
- Date: 2026-10-13
- Phase / spike: Phase 0
- Decision: Local builds auto-deploy to the plain Steam install's `Data`
  folder (no mod manager), overwriting the installed Community Shaders, with no
  backup.
- Alternatives rejected: backing up first; staging to a separate folder.
- Decided by: @Victor
- References: `CMakeUserPresets.json` (local, untracked)

## D4: Shape of the Phase 0 scaffold
- Date: 2026-10-13
- Phase / spike: Phase 0
- Decision: Fluid Field follows LOD Blending, the smallest feature with
  saved settings: `src/Features/FluidField.{h,cpp}`, a `Settings` struct
  serialized with `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT`, and
  registration in `Globals.h`, `Globals.cpp` and `Feature.cpp`. Unlike LOD
  Blending it has no `CORE` marker, so it gets its own `Package-FluidField`
  zip as the roadmap asks. The `.ini` is `Version = 0-1-0`, `Alpha = True`.
  `Settings::Enabled` defaults to `false`. No shader define, no hooks, no
  per-frame overrides and no GPU resources.
- Alternatives rejected: CORE feature (it would ship with the base mod);
  `Unreleased = True` (CONTRIBUTING.md reserves it for finished features that
  cannot ship yet); a shader define (it would add shader permutations for
  nothing).
- Decided by: agent, pending review by @Victor
- References: `docs/new-feature-template/NewFeatureReadme.md`,
  `src/Features/LODBlending.{h,cpp}`
