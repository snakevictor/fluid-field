# R1 stage a: devbench check results

Oct 14, 2026 · R1 / D7 stage 1 (`DECISIONS.md`) · branch `feature/fluid-field` at `33cae9349`

Scope: in-game checks of the landscape height cache (`src/Features/FluidField/LandscapeCache.*`), run through the devbench MCP tools against a live game. The cache copies the `TESObjectLAND` heights of each attached exterior cell and drops cells that leave the loaded grid. The Verify Landscape Heights setting compares each newly copied cell with `TES::GetLandHeight`.

Summary: every check passed. R1 open question 1 is answered: `loadedData->heights` are **not** absolute world Z. Adding the anchor `heightExtents.x - min(heights)` makes them match `TES::GetLandHeight` exactly (0.00 units at every vertex in 120 cell checks over 60 cells in two worldspaces). The cache costs about 0.006 ms of CPU per frame and no GPU time. One crash happened in the first session and did not come back in two repeats of the same steps, so its cause is unknown (section 4).

---

## 1. Setup

| Item                | Value                                                                                                                                    |
| ------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| Runtime             | SkyrimSE.exe, SE (not VR), devbench 1.28.0 on port 8920                                                                                  |
| Feature             | `FluidField` 0-3-0, Alpha; build `33cae9349`                                                                                             |
| Scenes              | Riverwood exterior (Tamriel, cell (4, -12)); Sleeping Giant Inn interior; WhiterunOrigin (WhiterunWorld)                                 |
| Loaded grid         | 5 × 5 cells                                                                                                                              |
| Settings under test | Enabled, 512², size 4096, 42 iterations, Build Depth Map and Verify Landscape Heights switched on through `communityshaders.feature set` |
| Timing source       | `communityshaders.inspect kind=profiler`, pass `FluidField::Landscape Cache`                                                             |
| Crash logging       | None in the first session; Crash Logger 1.25.0 installed for the second                                                                  |
| Settings restored   | Build Depth Map and Verify off, rest unchanged (in memory only, not saved)                                                               |

## 2. Height check (R1 open question 1)

Each newly copied cell is checked at five interior vertices and at one point between vertices. Unique cells: 35 in Tamriel around Riverwood and 25 in WhiterunWorld. Including repeats after teleports, loads and toggles, there were 120 cell checks.

| Measure                                     | Result                                                                                                         |
| ------------------------------------------- | -------------------------------------------------------------------------------------------------------------- |
| Raw cached height vs engine, per cell       | Off by the anchor every time; the anchor ranges from −4860 (WhiterunWorld) to +12,940 units (Tamriel (8, −14)) |
| Anchored height vs engine, at vertices      | **0.00** in every check                                                                                        |
| Samples where the engine gave no height     | 0                                                                                                              |
| Bilinear sample vs engine, between vertices | About 6 units mean, 48.75 max (Tamriel (3, −10)), from the 55 cells whose anchor exceeds 100 units             |

-   **The raw heights are relative, not absolute.** The anchor is `heightExtents.x - min(heights)`, the same anchoring Procedural Grass uses. With it applied, the vertex heights are exact.
-   **The interpolated check is not clean yet.** `Verify` compares the _raw_ bilinear sample with the engine, so its "interpolated error" is mostly the anchor. The bilinear-only figures above come from subtracting the anchor's magnitude afterwards, which is only valid when the anchor is large. Small anchors were left out for that reason.
-   **Bilinear sampling and the engine disagree on steep terrain.** The engine interpolates over triangles and the cache bilinearly, so the two differ by up to about 49 units on steep cells. Whether this matters for depth depends on the riverbeds; stage c's overlay will show it.

## 3. Functional checks

| Check                      | Method                                      | Result                                                                                                                                                 |
| -------------------------- | ------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Depth Map off adds no work | Depth Map off, profiler filter `FluidField` | Pass. No `Landscape Cache` pass                                                                                                                        |
| First fill                 | Switch Depth Map and Verify on in Riverwood | Pass. All 25 grid cells copied and checked in one frame                                                                                                |
| Cost                       | Profiler after 3 s                          | Pass. About 0.006 ms CPU per frame outdoors, 0.002 ms indoors, no GPU time. The first-fill frame is not visible; the profiler keeps only a CPU average |
| Cells added                | `player.setpos x 25416` (2 cells east)      | Pass. Exactly the 10 new cells (columns 7 and 8) copied                                                                                                |
| Cells dropped              | `player.setpos x 17224` (back)              | Pass. Columns 2 and 3 copied again with identical values, so they had been dropped. Same counts (25, 10, 10) in both sessions                          |
| Loading screen, interior   | `coc RiverwoodSleepingGiantInn`             | Pass. No cells copied in the interior; the pass still runs and returns early                                                                           |
| Loading screen, exterior   | `coc Riverwood`                             | Pass. All 25 cells copied again, so the cache had been cleared                                                                                         |
| Worldspace change          | `coc WhiterunOrigin`                        | Pass. 25 WhiterunWorld cells copied. A loading screen came with it, so the load clear may have done the work (section 5)                               |
| Feature off clears         | `Enabled: false`, then `true`               | Pass. No Fluid Field passes while off; all 25 cells copied again when back on                                                                          |
| Log                        | `CommunityShaders.log`                      | Pass. No Fluid Field warnings or errors. The only error is unrelated (Effects11 missing `enbeffect.fx`)                                                |

## 4. The crash

In the first session the game exited between 00:38:32 and 00:38:55. That was after the teleport back to Riverwood, with the player standing still and Depth Map and Verify on. Nothing was logged after 00:38:28, Windows recorded no event, and no crash logger was installed.

In the second session, with Crash Logger installed, the same steps ran twice without a crash: once with Depth Map off and once with it on, each followed by 30 s of standing still. The loading-screen, worldspace and toggle checks came after and also ran clean.

The cause is unknown. One possibility to rule out (unverified): `LandscapeCache::Update` reads the engine's cell grid every frame from `FluidField::Prepass`, on the render path. Its header says it must run on the main thread. If the crash comes back, the Crash Logger stack should settle it.

## 5. Not covered

-   **Worldspace change without a loading screen.** The cache also clears itself when the worldspace pointer changes. Every test that changed worldspace also went through a loading screen, so this path was never the one that cleared.
-   **First-fill frame cost.** Copying and checking 25 cells happens in one frame, which the profiler's average hides. Tracy (`FluidField - Landscape Cache` zone) would show it.
-   **R1 test routes.** Solitude dock, fast travel, a horse gallop across a cell border and an interior with water still have to be run.
-   **Other runtimes.** AE and VR are untested. The 1.7.99 terrain-cell lock path in `GetGridCell` only runs on AE.

## 6. Before stage b

1. Store anchored heights (`raw + anchor`) in the cache, so `SampleHeight` returns world Z.
2. Apply the anchor in the interpolated check, so it measures interpolation alone.
3. Settle whether `Update` may run where it does now, or move it to the main thread.

## 7. Notes for running these checks again

-   A devbench scenario that blocks for about 30 s or more comes back as `game not running (target: SE)` even though the game is fine. Run long scenarios with `async: true` and poll `action: status`, and check `tasklist` before calling something a crash.
-   With Verify on, each newly copied cell logs one `[Fluid Field] Landscape check cell (x, y)` line, so the log timestamps show which frame copied what. Grouping them by timestamp (`uniq -c`) is the quickest way to read a teleport or load test.
-   `communityshaders.feature set` replaces the whole settings blob, so pass every field. Read the settings first and restore them afterwards.
