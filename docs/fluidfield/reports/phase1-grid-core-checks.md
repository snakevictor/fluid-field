# Phase 1 grid core: devbench check results

Oct 14, 2026 · Phase 1 (ROADMAP.md:69) · branch `feature/fluid-field` at `b3c72f97d`

Scope: in-game checks of the Phase 1 grid core (toroidal player grid, 2D wind solver, loading-screen reset, settings validation), run through the devbench MCP tools against a live game. The solver design is D11 and the offline solver test is D12 (`DECISIONS.md`).

Summary: everything devbench can reach passed. The solver costs 0.08 ms of GPU time at the default 256² / 30 iterations and 0.21 ms at 512² / 42 iterations, well inside the 0.5 ms Phase 1 budget. It costs nothing when disabled, clamps bad settings, keeps running when the player teleports past the grid, and resets once after each loading screen. Still open: the wind's look (the puff button and layer overlay are menu-only), the hour of play, fast-travel and horse cell crossings, plus AE/VR runtimes. The GPU is an RTX 5060 Ti at 1440p, a mid-range card at the gate's resolution.

---

## 1. Setup

| Item              | Value                                                                                                                                                               |
| ----------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Runtime           | SkyrimSE.exe, SE (not VR), devbench 1.28.0 on port 8920                                                                                                             |
| Feature           | `FluidField` 0-2-0, Alpha                                                                                                                                           |
| Scene             | Riverwood exterior (Tamriel), cloudy, about 08:30 game time; one test in the Sleeping Giant Inn interior                                                            |
| Timing source     | `communityshaders.inspect kind=profiler` (Community Shaders' built-in profiler, `FluidField::<pass>`)                                                               |
| GPU               | NVIDIA GeForce RTX 5060 Ti, driver 32.0.15.9597, display 2560×1440 (read from Windows after the run). Nsight Perf is not built into this DLL (`NVPERF_SUPPORT` off) |
| Settings restored | Enabled, 512², size 4096, 42 iterations, damping 0.2, puff strength 1020, radius 444 (in memory only, not saved)                                                    |

## 2. GPU timings

Steady-state per-frame GPU time from the profiler's rolling history (about 300 frames), read about 3.5 s after each settings change. Grid size 4096 units throughout.

| Resolution | Iterations | Pressure Solve | Each other pass (Add Forces, Advect, Divergence, Project) | Solver total (approx.) |
| ---------- | ---------- | -------------- | --------------------------------------------------------- | ---------------------- |
| 64²        | 30         | 0.042 ms       | about 0.003-0.004 ms                                      | 0.06 ms                |
| 128²       | 30         | 0.046 ms       | about 0.003-0.004 ms                                      | 0.07 ms                |
| 256²       | 30         | 0.060 ms       | about 0.003-0.004 ms                                      | 0.08 ms                |
| 512²       | 30         | 0.135 ms       | about 0.006-0.007 ms                                      | 0.16 ms                |
| 512²       | 42         | 0.183 ms       | about 0.006-0.007 ms                                      | 0.21 ms                |

-   Pressure Solve dominates. Its cost scales roughly with the iteration count (0.135 ms at 30 iterations, 0.183 ms at 42).
-   64² and 128² cost almost the same, so below 256² the 30 Jacobi dispatches cost more than the texels do. Batching iterations would matter more than shrinking the grid.
-   `FluidField::Reset` costs 0.011 ms and runs only on the frame after a reset.
-   `FluidField::Debug View` did not appear, because the overlay was off.
-   **Unexplained spikes:** right after switching from 512² to 256², four frames of Pressure Solve took 1.36, 2.19, 3.98 and 4.09 ms. They did not come back: over the next 4 s the worst frame was 0.196 ms and p99 was 0.073 ms. It looks like a one-off stall around the resource re-creation rather than the solver, but a longer run should confirm it.

## 3. Functional checks

| Check                    | Method                                                                                                              | Result                                                                                                                                                                       |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Disabled costs nothing   | `feature set` with `Enabled: false`, wait 2 s, profiler filter `FluidField`                                         | Pass. No Fluid Field passes listed                                                                                                                                           |
| Out-of-range settings    | `set` GridResolution 300, GridSize 99999, JacobiIterations 0, TestPuffRadius 1, TestPuffStrength -5, WindDamping -1 | Pass. Read back as 256, 16384, 1, 16, 0, 0                                                                                                                                   |
| Malformed settings       | `set` `{"Enabled": "yes", "GridResolution": "big"}`                                                                 | Pass. Falls back to defaults (feature disabled). Logs one warning: `Malformed settings, using defaults: [json.exception.type_error.302] type must be boolean, but is string` |
| Grid scroll, small move  | `player.setpos x 18724` (+500 units)                                                                                | Pass. Solver keeps running                                                                                                                                                   |
| Grid scroll, past grid   | `player.setpos y -40000` (+7200 units, beyond the 4096-unit grid), lands in a Wilderness cell                       | Pass. Solver keeps running, no errors                                                                                                                                        |
| Loading screen, interior | `coc RiverwoodSleepingGiantInn`, wait for Loading Menu close + 200 ms, profiler filter `FluidField::Reset`          | Pass. Reset ran exactly once (history length 1). Solver runs in the interior                                                                                                 |
| Loading screen, exterior | `coc Riverwood`, same method                                                                                        | Pass. Reset ran exactly once                                                                                                                                                 |
| Log                      | `CommunityShaders.log`                                                                                              | Pass. The only Fluid Field line above info level is the warning from the malformed-settings test. The one error is unrelated (Effects11 missing `enbeffect.fx`)              |

## 4. Not covered

-   **Wind look.** The Puff of Wind button and Show Layer Overlay are menu-only (the overlay flag is not saved), so devbench cannot trigger them. Check by eye that the puff blows the way the player faces and decays smoothly.
-   **Grid scroll correctness.** The tests prove the solver survives teleports, not that `GridMin` and the toroidal wrap are right. The overlay's grid info line and player marker show that; the WARP test (D12) covers it offline.
-   **Phase 1 gate.** Still to do: an hour of play, fast travel, and a horse gallop across a cell border. A devbench recording replayed with `record action=replay restoreScene=true` would make this repeatable.
-   **Budget hardware.** Covered: RTX 5060 Ti at 2560×1440 matches the gate's "1440p on a mid-range card". Rebuild with `NVPERF_SUPPORT=ON` only if per-pass hardware counters are needed.
-   **Other runtimes.** AE and VR are untested.

## 5. Notes for running these checks again

-   The profiler drops a pass from its list when it stops running, so a one-shot pass like `FluidField::Reset` must be read within a few frames. Use a `scenario` with `waitFor menuClosed "Loading Menu"`, then `wait 200`, then the profiler query.
-   Each profiler response includes about 300 history samples per pass. Filter to one pass name (`FluidField::Pressure Solve`) to keep responses small.
-   `communityshaders.feature set` replaces the whole settings blob and runs it through `LoadSettings` and `Sanitize`, so it doubles as the validation test. It does not write to disk unless `communityshaders.settings action=save` follows.
-   Read the current settings with `feature get` first and restore them afterwards. The user's saved settings may not be the defaults.
