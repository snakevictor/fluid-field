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
