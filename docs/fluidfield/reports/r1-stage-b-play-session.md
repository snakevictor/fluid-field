# R1 stage b: play-session check results

Oct 14, 2026 · R1 / D7 stages 1 and 2 (`DECISIONS.md`) · branch `feature/fluid-field` at `ae9349c36`

Scope: a manual play session on the build that stores landscape world heights (`d5cd3ac2f`) and adds water surfaces and the depth view (`b37038216`). @Victor played; the session was watched from the log (`CommunityShaders.log`) and the process list. No devbench commands were sent, so there are no profiler timings. The stage a results and method are in `r1-stage-a-checks.md`.

Summary: the landscape fix works. Stored heights now match `TES::GetLandHeight` at every checked vertex (0.00 units in 155 cell checks). The between-vertex check now measures interpolation alone: about 5 units on average and 48.75 at most, on steep ground. Every water check matched the engine's water query (0.00 units, 232 samples, none missed). One water object near Riverwood and Whiterun has bounds 16.53 units off its plane. No warnings, errors or crashes, and the game was quit normally.

---

## 1. Setup

| Item     | Value                                                                                                                                 |
| -------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| Runtime  | SkyrimSE.exe, SE (not VR), pid 4552; Crash Logger 1.25.0 installed                                                                    |
| Feature  | `FluidField` 0-4-0, Alpha; build deployed 01:02, game started 01:05                                                                   |
| Settings | Build Depth Map, Verify Landscape Heights and Verify Water Heights on (set from the menu)                                             |
| Session  | 01:09:29 to 01:15:35, Tamriel only                                                                                                    |
| Route    | From Riverwood north-east toward Whiterun (cells up to (11, 0)), fast travel back to Riverwood, then south-west and west to (−6, −17) |
| Cells    | x −6 to 11, y −18 to 0                                                                                                                |
| Watch    | Log lines with `[Fluid Field]` or level W/E/C; new `crash-*.log` files; process exit                                                  |

## 2. Landscape heights

| Measure                                   | Result                                                           |
| ----------------------------------------- | ---------------------------------------------------------------- |
| Cell checks                               | 155                                                              |
| Vertex error (stored height vs engine)    | **0.00** in every check                                          |
| Between-vertex error (bilinear vs engine) | 4.9 mean, 48.75 max                                              |
| Checks over 20 units                      | 4: (3, −10) 48.75, (2, −13) 37.92, (6, −2) 30.70, (10, −6) 23.47 |
| Samples where the engine gave no height   | 0                                                                |

-   **The fix holds.** Stage a stored raw heights, which were off by the anchor. With `raw + anchor` stored, the vertex error is gone across 155 checks, including cells with anchors from −4800 to +14,492.
-   **The between-vertex figures are now direct.** They match the stage a estimates exactly: (3, −10) 48.75 and (2, −13) 37.92. Those estimates were made by subtracting the anchor afterwards, so the method in `r1-stage-a-checks.md` section 2 was right.
-   **The large errors are on steep ground.** They come from bilinear sampling against the engine's triangle interpolation. Two of the four are around Riverwood ((2, −13) and (3, −10)), so the depth view there should show whether this matters for the riverbed. If it does, sample over the same triangles the engine uses.
-   **Dropped cells come back with the same values.** Cells (−3, −13), (−2, −13) and (−1, −13) were dropped when the player moved on and copied again on the way back, with identical results. The fast travel cleared the cache, and all 25 Riverwood cells were copied again.

## 3. Water surfaces

| Measure                     | Result                                     |
| --------------------------- | ------------------------------------------ |
| Water checks                | 20 (one each time the surface set changed) |
| Flat surfaces per check     | 7 to 17                                    |
| Cells with water per check  | 6 to 17                                    |
| Sloped surfaces skipped     | 0                                          |
| Error vs engine water query | **0.00** in every check                    |
| Samples missed              | 0 of 232                                   |
| Interior water              | Not visited                                |
| Plane vs bound              | 16.53 in 9 checks, 0.00 in the other 11    |

-   **What the error measures.** For each flat surface, `WaterSurfaces::Verify` asks `TESObjectCELL::GetWaterHeight` at the centre of its bound and compares with the cache's own sample there. The sample uses the same overlap rule the simulation will see. A 0.00 says the cache agrees with the engine at those points. It does not test points between surfaces or the cell-water fallback on its own.
-   **The check may be partly circular (unverified).** If `GetWaterHeight` reads the same water-object bounds, a match is expected. A RenderDoc or in-game look at the water view settles whether the heights are right on screen.
-   **The 16.53 plane gap.** It appeared near the start (01:09), disappeared while the player was between Riverwood and Whiterun (01:10 to 01:11), came back around Whiterun's outskirts and at Riverwood after the fast travel (01:12 to 01:13), and went away again south-west of Riverwood (01:13:32 on). The value never changed, so it is most likely one water object, and the route fits the White River (unverified). Its bounds are flat (each under the 10-unit cut-off) but sit 16.53 units from its single plane. The cache uses the bound heights, which match the engine, so nothing is wrong. Logging the water object's form ID with the gap would confirm which one it is.
-   **No sloped water was met.** The rapids path (bounds taller than 10 units) was never used on this route.

## 4. Stability

-   No crash. The last lines are the System menu opening at 01:15:28; the process exited at 01:15:35 and Crash Logger wrote nothing.
-   No warnings or errors from any feature during the session, apart from the known ENB and CJK-font lines at start-up.

## 4a. What @Victor saw in the depth view

-   **Bed and Depth images: mostly right.**
-   **Water Surface image: water almost everywhere.** Water objects and cell
    water are planes that cover a whole cell or bound at one height, even
    where the ground rises far above them. The image shows where a plane
    exists, not where it is wet. The Depth image compares plane and bed, so it
    marks planes under the ground in tan and is correct. Follow-up: dim planes
    that lie under the bed in the Water Surface image, so it shows real water.
-   **Non-landscape meshes throw off depth near the surface.** Rocks,
    boulders, banks and the bridge are placed meshes, not landscape, so the
    bed there is wrong. In deep water this hardly matters; around a boulder in
    a stream it does, and surface disturbance will need it. This answers R1
    open question 3: landscape is enough for deep water, and stage d's
    downward raycasts are needed for the surface layer around placed meshes.
-   **Waterfalls do not register.** No sloped water objects were met at all
    (section 3), so waterfalls are not water objects. They are effect meshes
    and particles. They will need to come in as flow sources found from their
    references (R3), not through the sloped-water path.

## 5. Not covered

-   **Cost.** No profiler readings this session. The stage a figure (about 0.006 ms CPU for the landscape cache) does not include the water pass or the depth view's CPU build every 10 frames.
-   **The depth view itself.** The log cannot show whether the bed, water and depth images look right. That is the stage c review on the test routes (D7, amended).
-   **Interiors with water, sloped water, Solitude dock and a horse gallop.** None were on this route.
-   **The stage a crash.** Still not seen again; this is the third session without it.
-   **Other runtimes.** AE and VR are untested.

## 6. Next steps

1. Look at the depth view around Riverwood's river, especially cells (2, −13) and (3, −10), to judge whether bilinear sampling needs to match the engine's triangles.
2. Add the water object's form ID to the plane-gap log, to confirm the White River explanation.
3. Visit an interior with water and some rapids to exercise the interior and sloped paths.
4. Take profiler readings with the depth view on.
