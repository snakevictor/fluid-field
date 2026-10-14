# Flowing water slice 1, round 2: in-game test results

Oct 14, 2026 · D8 flowing water slice, R1 · branch `feature/fluid-field` at `ea16fae41` (fine pillars and arches) with `0cc3f5c7c` (calm water after loads)

Scope: @Victor's second in-game test of the water slice, on the build that refines the bed around pillars and arches (4×4 rays per lattice square near meshes and overheads) and holds the water solver after a load until rays finish. FluidField 0-7-0. Watched the same way as round 1 (`water-slice-1-test.md`): the profiler and scene polled twice a second over devbench's HTTP API, plus the log, crash logs, process and four Community Shaders screenshots.

Summary: the **post-load fix works**. After each load the water row waits, then starts on the right map: no flooded grid and no harbour draining. Ripples part around the bridge. But three new problems showed up:

-   **The fine refinement and overhead checks overload the ray budget.** Bed Raycasts averages 0.10 ms over the whole session and 0.17–0.20 ms when running or sprinting. One frame took 2.70 ms, the first Bed Raycasts frame over 2 ms in any session. The coarse map falls behind the player and is still incomplete 50 s after a load.
-   **Water turns turbulent wherever the bed changes under live water.** This happens when new strips scroll in while walking and when fine obstacles land under settled water, which also makes ripples while the player stands still.
-   **The bridge's dry footprint looks larger than a pillar.**

Solitude and the remaining slice checks were not covered. No crash, warning or error.

---

## 1. Setup

| Item     | Value                                                                                                                                                                                                     |
| -------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Runtime  | SkyrimSE.exe, SE (not VR), pid 19868; Crash Logger 1.25.0 installed                                                                                                                                       |
| Feature  | `FluidField` 0-7-0, Alpha; build deployed 02:50, game started 02:52                                                                                                                                       |
| Settings | 512² grid, 4096 units; Enabled, Build Depth Map, Raycast Bed, Simulate Water, Show Layer Overlay, Log Depth Map Details, Verify Landscape and Water; 64-unit spacing, 32 rays per frame                   |
| Session  | 02:55:25 (save loaded) to 03:03:03, 898 samples at 2 Hz; quit through the System menu at 03:02:56                                                                                                         |
| Route    | Riverwood start → through the village (Riverwood04, 02) → the bridge (RiverwoodBridge, both banks) → Riften Ratway Vaults (load, 03:00:54) → Riverwood (load, 03:02:05) → into the river under the bridge |
| Loads    | Save load at 02:55:25, the Ratway Vaults at 03:00:54, Riverwood at 03:02:05 (a reset at each)                                                                                                             |

Screenshots (in `Screenshots/`):

| File                             | Where                                                          |
| -------------------------------- | -------------------------------------------------------------- |
| `CS_2026-10-14_02-56-16_081.png` | Riverwood start, standing still about 50 s after the save load |
| `CS_2026-10-14_02-57-02_722.png` | Walking through Riverwood village, on dry land                 |
| `CS_2026-10-14_02-57-48_382.png` | Running north toward the bridge                                |
| `CS_2026-10-14_03-02-54_867.png` | Swimming in the river under the bridge, beside a pillar        |

## 2. Results against the test plan

| Step                                         | Result                                                                                                                                              |
| -------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| 2. Pillars and arches                        | **Partly.** Water runs under the arches, but the dry shape at the bridge looks larger than one pillar (section 5). No screenshot from on the bridge |
| 3. Ripples part around the pillar            | **Pass.** Rings spread from the player and part around the bridge shape (`03-02-54`)                                                                |
| 4. Calm after loads                          | **Pass for loads** (section 3). **Fails while walking** (section 4)                                                                                 |
| 4. Solitude piers                            | Not covered (no Solitude visit this round)                                                                                                          |
| 5. Stand still: ripples fade                 | **Not clean.** Ripples appeared while standing still (section 4); no screenshot after 20–30 s still                                                 |
| 5. Test current                              | Not seen in the screenshots. The solvers paused for about 85 s (02:59:17–03:00:43), probably while the menu was open                                |
| 5. Sprint in shallows, swim deep at Solitude | Not covered                                                                                                                                         |

## 3. Calm after loads: fixed

The water solver now waits after a reset until the map is ready, then starts from vanilla water on the new location's map.

| Load                    | Water solver started | Wait           |
| ----------------------- | -------------------- | -------------- |
| Save load, 02:55:25     | 02:55:45.8           | **About 20 s** |
| Ratway Vaults, 03:00:54 | 03:00:56.8           | About 2.5 s    |
| Riverwood, 03:02:05     | 03:02:07.0           | About 2 s      |

-   The screenshot about 50 s after the save load (`02-56-16`) shows Water Depth only along the river, matching the depth map. There's no flooded grid and no bright flow, unlike round 1.
-   The first wait, about 20 s, is long. The plan expects "a second or two". It is the first fill of the session, with the fine refinement running and the ray budget saturated (section 6), so the solver waits for rays that arrive slowly.

## 4. Turbulence when the bed changes under live water

Two screenshots show water stirred up with no player in it:

-   **Standing still** (`02-56-16`): red rings on the river at the top-left of the grid, about 1500 units from the player. The only water emitter in the code is the player's own wake (`FluidField.cpp:666–679`), which needs a speed over 20 units/s. So the rings were not from any actor.
-   **Walking through the village, on dry land** (`02-57-02`): Water Depth shows water across a large area right of the river, where Depth says dry ground. It runs into a region whose map is not built yet (dark grey in Water Surface and Depth, grey and streaky in Raycasts). Flow is bright across it, and Waves shows a large red and blue band.
-   **Running** (`02-57-48`): the whole river is blue in Waves (below vanilla), and Flow has bright streaks along the top and at the right edge, where strips scroll in.

Cause, from `WaterForcesCS.hlsl:31–46`:

-   The forces pass re-reads `bed` from the water map (and the fine obstacle texture) **every step**, while `depth` is carried over from the previous step.
-   Cells that scroll in start as "calm vanilla water" on whatever map is there at the time.
-   The map for new ground arrives in stages: coarse map, then rays, then the fine 4×4 obstacles (`FluidField.cpp:647–652`, uploaded block by block).
-   Every stage changes the bed under water that is already simulating. The surface (bed + depth) jumps with it, and that makes a wave. Many such changes at the scrolling edge add up to the turbulence. Fine obstacle blocks landing under settled river water explain the rings while the player stood still.

The load fix covers only the first map after a reset. Possible fixes:

-   When a cell's bed changes by more than a small threshold, reset that cell to vanilla depth and the rest velocity, rather than keeping its depth.
-   Or keep newly scrolled cells out of the simulation (at vanilla, held) until their map, including the fine obstacles, is complete.
-   Or fade bed changes in over a few frames.

## 5. Bridge: pillars and arches

From `03-02-54`, in the river under an arch beside a pillar:

-   **The water runs under the arches.** Water Surface, Depth and Water Depth are continuous on both sides of the bridge.
-   **The dry shape is a rectangle with notched edges,** roughly 1000 × 400 units at the image scale. One pillar should be a few hundred units across. It may cover two pillars and the footing between them, which would go against "the gaps between pillars stay open". Worth checking against the bridge in game: if there is open water between those pillars, the fine obstacles are too wide.
-   **Raycasts** shows orange along the whole bridge line (fine refinement around the deck and pillars) and green elsewhere.

## 6. Cost: the ray budget is overloaded

| Pass                    | Rolling average (session mean / max) | Worst sampled frame | GPU (avg / worst)                   |
| ----------------------- | ------------------------------------ | ------------------- | ----------------------------------- |
| **Bed Raycasts**        | **0.104 / 0.589 ms**                 | **2.70 ms**         | —                                   |
| Depth Map View          | 0.287 / 0.429 ms                     | 2.09 ms             | 0.003 / 1.01 ms                     |
| Water Map               | 0.060 / 0.111 ms                     | 0.28 ms             | —                                   |
| Landscape Cache         | 0.003 / 0.011 ms                     | 0.05 ms             | —                                   |
| Water solver (5 passes) | about 0.002 CPU                      | 0.01 ms             | about 0.04 / 2.63 ms (Water Forces) |
| Wind solver             | about 0.005 CPU                      | 0.05 ms             | 0.19 / 4.10 ms (Pressure Solve)     |

Bed Raycasts by movement (rolling average):

| Moving                  | Samples | Mean     | Max      | Round 1 (route run) |
| ----------------------- | ------- | -------- | -------- | ------------------- |
| Standing (under 20 u/s) | 588     | 0.080 ms | 0.589 ms | about 0.001 ms      |
| Walking (20–250)        | 146     | 0.126 ms | 0.271 ms | 0.025 ms            |
| Running (250–450)       | 95      | 0.172 ms | 0.344 ms | about 0.08 ms       |
| Sprinting (over 450)    | 43      | 0.195 ms | 0.259 ms | 0.116 ms (gallop)   |

-   **Every movement speed is now over the 0.1 ms budget,** two to three times the route run's cost. While running, each 300-update summary shows 8000–9500 rays, about 30 per frame against the 32 cap: the cap is binding all the time.
-   **One 2.70 ms frame on the bridge** (02:58:21.9), over R1's 2 ms per-frame limit. The rays-per-frame cap counts rays, but the overhead path and fine refinement add several rays per lattice point, and the 0.07 ms time cap does not stop a batch already in progress.
-   **The work does not catch up when the player stops.** Standing still from 02:57:49 to 02:57:59, Bed Raycasts kept spending 0.16–0.23 ms per frame with `idle no`. After the Riverwood load at 03:02:05 it was still busy at the end of the session, a minute later. The `03-02-54` screenshot still has uncast rows (dark streaks) and a grey block 50 s after the load.
-   **Where the rays go:** the overhead counter rose from 14,813 to 22,400 in about 35 s while running, and reached 52,628 by the end. Each overhead costs an upward probe and a second downward cast, so about a third of all rays go to overhead checks. Of the 724 overheads in the per-ray log, 721 are static meshes (layer 1) and 3 trees (layer 9); none are landscape. So the round 1 suspicion that landscape banks trigger overheads is not supported. The count is high because there are many meshes near the river, and every rock, bank and footing is probed.
-   **The coarse map falls behind the player.** In `02-57-48` the area around the player is not built yet while running.
-   **Depth Map View doubled** to 0.287 ms average (was about 0.16 ms), with two slices just over 2 ms (2.07 and 2.09 ms), from drawing the fine detail. Only while the overlay is open.
-   **Water Map** costs 0.060 ms of CPU, a little less than in round 1 (0.083 ms).
-   **GPU spikes:** Water Forces (2.63 ms) and Water Advect (2.38 ms) each had one large GPU frame, against averages of about 0.01 ms. These are the first large spikes from the water solver. They may come from an obstacle texture update in the same frame. The wind solver's Pressure Solve had its usual occasional spike (4.10 ms).

Suggestions:

1. Keep the coarse lattice first: refine only squares whose coarse points are all done, and pause refinement while the player moves fast.
2. Charge the time cap per ray segment (probe, recast, each fine ray), not per lattice point, so one point cannot overshoot a frame.
3. Cache overhead results per mesh reference, or skip the upward probe when the first hit is far below anything that could be a deck.

## 7. Other observations

-   **Landscape checks:** 52, all with vertex error 0.00.
-   **Water checks:** 5, all exact. The Riverwood water object `000F762D` again has the 16.53 plane gap. Its centre was reported at (24033, −46222) once instead of (22690, −47122). It is a large object, so the reported centre may depend on which of its bounds is checked.
-   **The Ratway Vaults** loaded calmly: interior water, the solver started 2.5 s after the load, and the rays refilled in about 16 s at up to 0.54 ms per frame.
-   **Solver pauses:** the water and wind solvers stopped for about 85 s (02:59:17–03:00:43) while the depth map kept running, and briefly at 02:56:56 and 03:01:08–03:01:56. These are probably menu or console time.
-   **Frame rate:** 59.5 fps mean.

## 8. Stability

No crash, and Crash Logger wrote nothing. No warnings or errors and no shader compile failures. The game quit through the System menu at 03:02:56.

## 9. Next steps

1. Stop bed changes from disturbing live water (section 4): reset or hold cells whose bed changes, including newly scrolled strips and fine obstacle blocks.
2. Bring the ray budget back under 0.1 ms and the worst frame under 2 ms (section 6), coarse lattice first.
3. Check the bridge's dry footprint against the real pillars (section 5).
4. Run the steps not covered: Solitude piers and deep water, stand still 20–30 s, the test current, sprinting through shallows.
5. Look at the first-load wait (about 20 s) once the ray budget is fixed.
