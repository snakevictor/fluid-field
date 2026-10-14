# Fluid Field — Learnings

Engine quirks, tooling surprises and repo conventions discovered while building
Fluid Field. Add an entry whenever something cost time to figure out, so the
next task starts from it. Newest entries go at the bottom of each section.

Entry format: **Short title** (date, phase) — what happens, where it was seen
(file paths / line ranges, RenderDoc capture, Ghidra address), and what to do
about it.

## Engine and rendering

- **Two features keep engine shadow textures across frames** (2026-10-13, Phase 0). Skylighting (`src/Features/Skylighting.cpp:660-668`) and Exponential Height Fog both keep a reference to an engine shadow-map texture from one frame to the next. Fluid Field must not copy this; see `reports/skylighting.md` and `reports/volumetric-lighting.md`.
- **Unified Water turns off vanilla actor wakes** (2026-10-13, Phase 0). It disables the vanilla dynamic flowmap writes in `Main::RenderWaterEffects`, so `ISWaterFlow` wakes no longer appear. Water also has no foam term today. See `reports/unified-water.md`.
- **Hooked call sites are shared** (2026-10-13, Phase 0). Skylighting and Upscaling both hook the precipitation-occlusion call (`RelocationID(35560, 36559)`). Grass Optimizations and Grass Collision both hook `BSGrassShader::SetupGeometry`. Check what is already chained before adding a hook.
- **`Util::GetShapeBound` never skips list shapes** (2026-10-13, Phase 0). `src/Utils/ActorUtils.cpp:14` casts the rigid body to `hkpListShape` instead of its shape, so the check never fires. It also reduces every Havok shape to a sphere. This is an existing bug, not ours; report it upstream rather than fixing it in this branch.

## Resource lifetimes and loading screens

- **Few features release GPU resources** (2026-10-13, Phase 0). Grass Collision and Water Effects create their resources once and never free them. Skylighting and Screen Space GI queue a reset when a loading screen closes, and that is the pattern to copy. Grass Collision also keeps stale state in function statics and only updates on frames that draw grass, so it stops updating in interiors.

## Threading

## Build, packaging and deployment

- **The build needs the VS 18 developer environment** (2026-10-13, Phase 0). On this machine `cmake` is not on PATH and `VCPKG_ROOT` is unset. Run from a "Developer Command Prompt for VS 2026" (or call `Common7\Tools\VsDevCmd.bat -arch=amd64`) and set `VCPKG_ROOT=C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg`, the copy that originally configured `build/ALL`. A `vswhere.exe not recognized` line printed by VsDevCmd is harmless.
- **cmd will not run `.bat` files from the current directory** (2026-10-13, Phase 0). From a non-interactive shell, `BuildRelease.bat` fails with "not recognized" (NoDefaultCurrentDirectoryInExePath). Call it by its absolute path. `CMakeLists.txt` already works around the same issue for its robocopy wrapper.
- **The ALL preset forces `AUTO_PLUGIN_DEPLOYMENT=OFF`** (2026-10-13, Phase 0). Deployment uses the local `CMakeUserPresets.json`: configure preset `ALL-WITH-AUTO-DEPLOYMENT` (shares `build/ALL`) and build preset `Deploy`, run as `BuildRelease.bat Deploy ALL-WITH-AUTO-DEPLOYMENT`. A plain `BuildRelease.bat ALL` reconfigures `build/ALL` with deployment off again. The first deploy after a commit redeploys everything, because CMake detects the new git HEAD.
- **Python is not installed** (2026-10-13, Phase 0), so `tools/extract-i18n.py` and `sort-i18n.py` could not run. The Fluid Field keys were added to `en.json` by hand in sorted order; run `--check` once Python is available.
- **A non-core feature is packaged from its folder alone** (2026-10-13, Phase 0). CMake creates `dist/<Folder Name>-<timestamp>.zip` for every `features/<Folder>/` without a `CORE` file, and globs `src/` automatically. No CMake edits were needed.

## Repository conventions

- **There is no `AI-INSTRUCTIONS.md`** (2026-10-13, Phase 0). `AGENTS.md` fills that role and points to `.claude/CLAUDE.md`. `.github/copilot-instructions.md` still mentions `AI-INSTRUCTIONS.md`; that is upstream text and we left it alone.
- **VR support was removed from Community Shaders** in `c9d7c0524` (2026-06-07, #2475), but `CMakePresets.json` still sets `ENABLE_SKYRIM_VR=ON` in the ALL preset; `CMakeLists.txt` no longer reads it. The roadmap's VR compatibility row and spike R9 need revisiting.
- **Snow Deformation is not in this repo** (2026-10-13, Phase 0). It lives in open upstream PR #2751 and the fork `josefplyr1/skyrim-community-shaders-dynamic-snow-deformation`. Its Havok input is the same sphere bound that Grass Collision uses, and it reads no velocities, masses or contacts. See `reports/snow-deformation.md`.
