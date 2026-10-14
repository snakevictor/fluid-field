# R1 stage d: play-session results

Oct 14, 2026 · R1 / D7 stage 4 (`DECISIONS.md`) · branch `feature/fluid-field` at `47eccb4b2`

Scope: a manual play session on the build that adds downward Havok raycasts for the riverbed (`BedRaycasts`, stage d). @Victor played; the session was measured from outside the game. A script polled devbench's local HTTP API (`POST /api/tool/<name>`) once a second for the Community Shaders profiler and the player's scene, and the log and process list were watched for errors and crashes. Earlier stages: `r1-stage-a-checks.md`, `r1-stage-b-play-session.md`.

Summary: no crash, warning or error. Bed Raycasts costs **0.083 ms of CPU per frame on average** over the session. It goes **over R1's 0.1 ms budget while running or sprinting**: the rolling average peaked at 0.159 ms, and it averaged 0.104 ms in the fastest stretches. Standing still it settles at **0.038 ms**, a floor that comes from the lattice sweep rather than from rays. No sampled frame spent more than 0.42 ms in it, far under the 2 ms per-frame limit. The depth view overlay costs 1.75–4.28 ms on each of its rebuild frames, but only while the overlay is open. Landscape and water checks stayed exact.

---

## 1. Setup

| Item       | Value                                                                                                                                     |
| ---------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| Runtime    | SkyrimSE.exe, SE (not VR), pid 19400; Crash Logger 1.25.0 installed                                                                       |
| Feature    | `FluidField` 0-5-0, Alpha; build deployed 01:27, game started 01:29                                                                       |
| Settings   | 512² grid, 4096 units; Build Depth Map, Verify Landscape and Verify Water on; Raycast Bed on, 64-unit spacing, 32 rays per frame          |
| Overlay    | Layer overlay open (the Depth Map View pass only runs while it is)                                                                        |
| Session    | Measured 01:33:11 to 01:36:08 (166 one-second samples); log from 01:32:53                                                                 |
| Route      | West of Riverwood, past Embershard Mine and the Guardian Stones, west through the pine forest, then north to Brittleshin Pass by the lake |
| Speed      | From position changes between samples: 295 units/s median, 527 max (running and sprinting); still for the last 17 s                       |
| Frame rate | 59.7 fps mean; one 33 fps sample at 01:35:55, the frame a screenshot was taken                                                            |

## 2. CPU cost per pass

Each sample holds the pass's time in the sampled frame and its rolling average over about the last 300 frames. Sampling once a second sees one frame in about sixty, so short spikes can be missed.

| Pass            | Rolling average, mean | Rolling average, max | Sampled frame, p50 | Sampled frame, p90 | Sampled frame, max |
| --------------- | --------------------- | -------------------- | ------------------ | ------------------ | ------------------ |
| Bed Raycasts    | **0.083 ms**          | **0.159 ms**         | 0.057 ms           | 0.166 ms           | 0.42 ms            |
| Depth Map View  | 0.204 ms              | 0.265 ms             | 0                  | 0                  | 4.28 ms            |
| Landscape Cache | 0.005 ms              | 0.007 ms             | 0.004 ms           | 0.007 ms           | 0.20 ms            |
| Water Surfaces  | 0.000 ms              | 0.001 ms             | 0                  | 0                  | 0.01 ms            |
| Solver passes   | ≤ 0.004 ms each       | 0.006 ms             | —                  | —                  | 0.01 ms            |

GPU: Bed Raycasts, Landscape Cache and Water Surfaces use none. The worst GPU frame in any history was the solver's Pressure Solve at 1.89 ms (usual 0.18 ms, as in the Phase 1 report). The depth view's texture updates peaked at 0.96 ms.

## 3. Bed Raycasts and speed

The rolling average of Bed Raycasts, grouped by how fast the player moved in that second:

| Speed (units/s)      | Samples | Mean         | Max          |
| -------------------- | ------- | ------------ | ------------ |
| Standing (under 20)  | 20      | 0.044 ms     | 0.068 ms     |
| Walking (20–250)     | 25      | 0.077 ms     | 0.104 ms     |
| Running (250–450)    | 98      | 0.088 ms     | 0.156 ms     |
| Sprinting (over 450) | 21      | **0.104 ms** | **0.159 ms** |

-   **It is over budget at sprint.** R1's first success criterion is an amortized cost of 0.1 ms or less at sprint. At sprint the mean is 0.104 ms and the worst rolling average 0.159 ms. Moving into new cells keeps new lattice points coming, so the ray cap is reached more often. No horse gallop was tested, and it would be faster still.
-   **The cost when idle is not rays.** Standing still at the end, the average fell to a steady 0.038 ms and stayed there. Every frame, `Update` examines up to 1024 lattice points (`SWEEP_PER_UPDATE`) out of 65 × 65 = 4225, a full pass every 4 or 5 frames. Points without water are not cached (`continue` without storing a sample), so each of them repeats `WaterSurfaces::SampleHeight`, a linear walk over all surfaces (10 to 17 here), every time the sweep comes round. Every full pass also walks every cached sample in `erase_if`. Caching dry points and skipping the sweep when the player has not crossed a lattice line would remove most of this floor.
-   **Ray counts were not measured.** `raysLastFrame` and the hit counts are only shown in the menu, not exposed to devbench or the log, so this report cannot say how many rays each frame cast or how they resolved.
-   **The worst sampled frame was 0.42 ms.** Even with the cap reached, no single frame came close to the 2 ms limit.

## 4. Depth map view (overlay)

The overlay builds four 128² images (bed, water surface, depth, raycasts) on the CPU every 10th frame. 12 of 166 samples caught a rebuild frame.

| Rebuild frames | Min     | Median  | Max     |
| -------------- | ------- | ------- | ------- |
| 12             | 1.75 ms | 2.05 ms | 4.28 ms |

-   Half the rebuilds took over 2 ms, the per-frame limit R1 sets for the depth map. The overlay is a debug tool, so this only matters while it is open, but it makes frame-time readings taken with the overlay open pessimistic.
-   The two largest (4.28 ms at 01:34:03 and 3.74 ms at 01:34:47) came within a second of a water check, so on the first rebuild after new water surfaces arrived. A 3.14 ms rebuild at the Dragon Lair exterior (01:35:40) had no water check next to it.
-   Spreading the rebuild over several frames (one image per frame) would keep each frame under 1 ms.

## 5. What the screenshot shows

At 01:35:55 a screenshot was taken by the lake at Brittleshin Pass, at about (−16405, −52338), facing west, with the overlay open.

-   **Water surface:** one water object covers most of the 4096-unit square, apart from a block in the north-east. The overlay counts 12,899 of its 16,384 texels as water-object water and none as cell water only.
-   **Depth:** almost all tan (water plane under the ground). Only 471 texels are submerged, along the southern edge, deepest 85 units. The player stands on a rock well above the water, and most of the lake in view is more than 2048 units away, beyond the square. The submerged strip matches the near shore and the dock to the south-west.
-   **Raycasts:** grey (dry, no ray) across most of the square. Along the southern shore there is a wide orange band (static meshes: rocks and banks) with a small green patch (terrain). So by this shore most rays end on placed meshes rather than landscape. That is the case stage d exists for (R1 open question 3), and here it makes the riverbed mostly meshes.
-   This is one screenshot, read by eye. Whether the orange band is right (real rocks rather than, say, the dock) needs the in-game check described in section 7.

## 6. Logs and stability

-   **Landscape:** 56 cell checks, every one with vertex error 0.00; none with between-vertex error over 20 units.
-   **Water:** 8 checks, 10 to 17 surfaces, error 0.00, none missed, no sloped water. The 16.53 plane-vs-bound gap from the stage b report appeared once, at 01:32:53, near Riverwood.
-   **Stability:** no warnings or errors, and no crash. The session ended through the System menu (01:36:02); the process exited at 01:36:12, and Crash Logger wrote nothing.

## 7. Not covered

-   **Teleport refill.** There was no fast travel or `coc`, so R1's "refill within 1 s, no frame over 2 ms" criterion is untested.
-   **Accuracy.** Wet/dry agreement and bed-height error against a dense reference (R1 section 8) were not measured.
-   **Bridges.** The route crossed none, so "bridges never block flow" is untested. Rays start 256 units above the water, so a bridge deck lower than that would end the ray on the deck.
-   **Horse gallop, Riverwood's river, Solitude dock, interiors with water.** Not on this route.
-   **Thread safety.** Raycasts run in `Prepass` under `bhkWorld::worldLock`. D7 asks for the main thread; whether `Prepass` qualifies is still the open question from the stage a report. Nothing went wrong this session.
-   **Other runtimes.** AE and VR are untested.

## 8. Next steps

1. Cut the idle floor: cache dry lattice points and skip the sweep while the player stays within one lattice step.
2. Bring sprint cost under 0.1 ms. Options: fewer rays per frame while moving fast, or coarser spacing far from the player.
3. Expose rays per frame and the hit counts to devbench (or log them), so sessions can measure them directly.
4. Spread the overlay rebuild over several frames.
5. Run the remaining test routes: Riverwood's river and bridge, a fast travel, a horse gallop, Solitude dock and an interior with water.

## 9. Notes for running these measurements again

-   devbench answers plain HTTP on its port: `POST http://127.0.0.1:8920/api/tool/<tool>` with the tool's arguments as the JSON body (`communityshaders.inspect`, `inspect`, ...). A script can sample the profiler this way without the MCP client, and without disturbing play.
-   Filter the profiler to `FluidField`: each call returns about 300 GPU history samples per pass, but only the rolling CPU average and the current frame's CPU time.
