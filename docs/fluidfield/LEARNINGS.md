# Fluid Field — Learnings

Engine quirks, tooling surprises and repo conventions discovered while building
Fluid Field. Add an entry whenever something cost time to figure out, so the
next task starts from it. Newest entries go at the bottom of each section.

Entry format: **Short title** (date, phase) — what happens, where it was seen
(file paths / line ranges, RenderDoc capture, Ghidra address), and what to do
about it.

## Engine and rendering

-   **Two features keep engine shadow textures across frames** (2026-10-13, Phase 0). Skylighting (`src/Features/Skylighting.cpp:660-668`) and Exponential Height Fog both keep a reference to an engine shadow-map texture from one frame to the next. Fluid Field must not copy this; see `reports/skylighting.md` and `reports/volumetric-lighting.md`.
-   **Unified Water turns off vanilla actor wakes** (2026-10-13, Phase 0). It disables the vanilla dynamic flowmap writes in `Main::RenderWaterEffects`, so `ISWaterFlow` wakes no longer appear. Water also has no foam term today. See `reports/unified-water.md`.
-   **Hooked call sites are shared** (2026-10-13, Phase 0). Skylighting and Upscaling both hook the precipitation-occlusion call (`RelocationID(35560, 36559)`). Grass Optimizations and Grass Collision both hook `BSGrassShader::SetupGeometry`. Check what is already chained before adding a hook.
-   **`Util::GetShapeBound` never skips list shapes** (2026-10-13, Phase 0). `src/Utils/ActorUtils.cpp:14` casts the rigid body to `hkpListShape` instead of its shape, so the check never fires. It also reduces every Havok shape to a sphere. This is an existing bug, not ours; report it upstream rather than fixing it in this branch.
-   **Skylighting replaces the vanilla precipitation-occlusion call** (2026-10-13, R1). With the shader cache on, the vanilla function at `RelocationID(35560, 36559) + 0x3A1` is never called. Skylighting runs `Precipitation::SetupMask`/`RenderMask` itself (`src/Features/Skylighting.cpp:523-560`). The pass is depth-only: `BSUtilityShader` `RenderDepth` in render mode `kPrecipitationOcclusionMap = 0x1C`. Its depth target's format, size and near/far planes are not in source; reading them needs RenderDoc.
-   **Landscape heights are readable on the CPU** (2026-10-13, R1). `TESObjectLAND::loadedData->heights[4][289]` holds 4 quadrants of 17x17 vertices, so 33x33 per cell at 128-unit spacing. Vertices are row-major `y*17+x`; quadrant bit 0 is +X and bit 1 is +Y (`src/EngineFixes/LandscapeSeamsFix.cpp:45-48,555-558`). The vertices continue under water, so they give a riverbed. Whether they are absolute world Z is UNVERIFIED.
-   **Vanilla water surfaces are in `TESWaterSystem::waterObjects`** (2026-10-13, R1). Each object has an `NiPlane` and `multiBounds` AABBs (`extern/CommonLibSSE-NG/include/RE/T/TESWaterObject.h:27-36`). Unified Water removes its LOD tiles from the list, so it holds only near water. `src/Features/Skin.cpp:294-329` already walks it. `GetExteriorWaterHeight()` returns -inf for interiors.
-   **Wetness Effects has a dead binding** (2026-10-13, R1). It binds the engine rain-occlusion SRV at PS t70 every frame (`src/Features/WetnessEffects.cpp:947-954`), but no shader samples `TexPrecipOcclusion`.
-   **ImGui frames are skipped unless something asks for them** (2026-10-13, Phase 1). `OverlayRenderer::ShouldSkipRendering` (`src/Menu/OverlayRenderer.cpp`) skips the whole ImGui frame unless the menu, the editor, the performance overlay or a few other things are active, so an `OverlayFeature::DrawOverlay` never runs during play. Fluid Field adds itself to that list; any new always-on overlay must do the same.
-   **D3D11 silently nulls an SRV whose resource is still bound as a UAV** (2026-10-13, Phase 1). Ping-pong compute passes must unbind the previous pass's UAV before binding its texture as an SRV. `WindSolver::Dispatch` does this for every pass.

## Resource lifetimes and loading screens

-   **Few features release GPU resources** (2026-10-13, Phase 0). Grass Collision and Water Effects create their resources once and never free them. Skylighting and Screen Space GI queue a reset when a loading screen closes, and that is the pattern to copy. Grass Collision also keeps stale state in function statics and only updates on frames that draw grass, so it stops updating in interiors.

## Threading

-   **Locks to take when reading engine data** (2026-10-13, R1): `TESWaterSystem::lock` (a spinlock) while iterating `waterObjects`; on 1.7.99+, the terrain-cell lock `REL::ID(564236)` around `gridCells->GetCell` (as `src/Features/UnifiedWater.cpp:593-603` does); `bhkWorld::worldLock` around raw `hkpWorld::CastRay`. Whether our hook context is safe for Havok queries is UNVERIFIED (R1 open question 2).

## Build, packaging and deployment

-   **CMake and vcpkg come from VS 18** (2026-10-13, Phase 0). Neither was on PATH originally. The user PATH now includes VS's bundled CMake (4.3.1, `Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`), and the user variable `VCPKG_ROOT` points to `C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg`, the copy that originally configured `build/ALL`. Terminals opened before that change need restarting. `BuildRelease.bat` loads vcvars itself for the Ninja preset.
-   **cmd will not run `.bat` files from the current directory** (2026-10-13, Phase 0). From a non-interactive shell, `BuildRelease.bat` fails with "not recognized" (NoDefaultCurrentDirectoryInExePath). Call it by its absolute path. `CMakeLists.txt` already works around the same issue for its robocopy wrapper.
-   **The ALL preset forces `AUTO_PLUGIN_DEPLOYMENT=OFF`** (2026-10-13, Phase 0). Deployment uses the local `CMakeUserPresets.json`: configure preset `ALL-WITH-AUTO-DEPLOYMENT` (shares `build/ALL`) and build preset `Deploy`, run as `BuildRelease.bat Deploy ALL-WITH-AUTO-DEPLOYMENT`. A plain `BuildRelease.bat ALL` reconfigures `build/ALL` with deployment off again. The first deploy after a commit redeploys everything, because CMake detects the new git HEAD.
-   **Dev toolchain setup on this machine** (2026-10-13, Phase 0). Python 3.13 is installed per user (`%LOCALAPPDATA%\Programs\Python\Python313`, on the user PATH), with `pre-commit` and `hlslkit` installed through pip. `pre-commit install` has added the git hook, so clang-format, prettier and gersemi run on every commit. `tools/extract-i18n.py --check`, `--orphans` and `sort-i18n.py --check` all pass with the hand-added Fluid Field keys.
-   **A non-core feature is packaged from its folder alone** (2026-10-13, Phase 0). CMake creates `dist/<Folder Name>-<timestamp>.zip` for every `features/<Folder>/` without a `CORE` file, and globs `src/` automatically. No CMake edits were needed.
-   **Integer modulus is a warning in fxc, and CI allows none** (2026-10-13, Phase 1). `int % int` raises X3556. Keep grid sizes powers of two and wrap with `a & (n - 1)`, which is also correct for negative cells.
-   **A standalone test can use the Windows SDK's C++/WinRT** (2026-10-13, Phase 1). With the Visual Studio generator the SDK `cppwinrt` headers are on the include path, and `winrt::check_hresult` only needs `windowsapp.lib`. `cmake -S tests/FluidField -B build/FluidFieldTest` then builds without vcpkg in a few seconds.
-   **A failed pre-commit hook can merge the next commit** (2026-10-13, Phase 1). If clang-format rejects a commit, its files stay staged and are swept into the next `git commit`. Run `pre-commit run --files <files>` before staging.

## Repository conventions

-   **There is no `AI-INSTRUCTIONS.md`** (2026-10-13, Phase 0). `AGENTS.md` fills that role and points to `.claude/CLAUDE.md`. `.github/copilot-instructions.md` still mentions `AI-INSTRUCTIONS.md`; that is upstream text and we left it alone.
-   **VR support was removed from Community Shaders** in `c9d7c0524` (2026-06-07, #2475), but `CMakePresets.json` still sets `ENABLE_SKYRIM_VR=ON` in the ALL preset; `CMakeLists.txt` no longer reads it. The roadmap's VR compatibility row and spike R9 need revisiting.
-   **Snow Deformation is not in this repo** (2026-10-13, Phase 0). It lives in open upstream PR #2751 and the fork `josefplyr1/skyrim-community-shaders-dynamic-snow-deformation`. Its Havok input is the same sphere bound that Grass Collision uses, and it reads no velocities, masses or contacts. See `reports/snow-deformation.md`.
-   **The root `CMakeLists.txt` is not gersemi-clean on `dev`** (2026-10-13, Phase 1). Any edit makes the hook reformat about 90 unrelated lines. Keep Fluid Field build logic out of it (the solver test is a standalone project for this reason).
