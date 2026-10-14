# Mesh cut 1: live test (2026-10-14)

Build: `feature/fluid-field` at `add562af7` (Fluid Field 0-8-0), Skyrim 1.7.104. Driven through
devbench by Claude; @Victor turned the layer overlay on. Settings: Build Depth Map, Simulate
Water, Log Depth Map Details, Cut Meshes at the Water Line (all on); Grid 512, size 4096, rays
32 per frame at 64-unit spacing.

## Summary

The mesh cut (D15) gives the water the shapes the raycast rule kept missing. At the Riverwood
stone bridge the two pillars are solid and the three arches are open. At the Riften lake docks
every dock post is a separate obstacle under open walkways, and the boats are hulls. The cut costs
0.064 ms of CPU per frame on average. No crash, warning or error.

## 1. Riverwood stone bridge (`00022469`)

| Check                                                                  | Result                                                                                                                                        |
| ---------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| Log line                                                               | `Mesh cut 00022469 'Landscape\Bridges\Bridge01.nif' at -250: 5 shapes (0 skipped), 146 segments, 2 closed loops, 1010 solid points, 0.594 ms` |
| Matches the offline cut (`skyrim-engine-re`, `tools/live/mesh_cut.py`) | Yes: 146 segments, 2 closed loops                                                                                                             |
| Raycasts image                                                         | Two red bars (the pillars) across the purple bridge line                                                                                      |
| Water Surface, Depth, simulated Water Depth                            | Two pillar shapes in the river; open water under the arches                                                                                   |

Screenshot: `CS_2026-10-14_15-25-38_785.png` (player in the river beside the north-west pillar).

## 2. Riften lake docks (Tamriel, around (168,900, -96,300))

| Check                             | Result                                                                                                                                                     |
| --------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| References cut at the lake height | 206 at 10,624                                                                                                                                              |
| Example lines                     | `RTDockPier01`: 263 segments, 22 closed loops (its posts); `ShipRowBoat01`: 3 closed loops; `ShipLongBoat01`: 2 closed loops, 1,963 solid points, 0.948 ms |
| Water maps                        | Rows of dots (the posts) under the walkways; lens-shaped boat hulls; the fishery building solid                                                            |
| Waves                             | The splash from dropping into the water spread as a ring and ran into the posts                                                                            |

Screenshot: `CS_2026-10-14_15-26-32_215.png`.

## 3. Cost (profiler, averaged)

| Pass                          | CPU ms | GPU ms      |
| ----------------------------- | ------ | ----------- |
| Mesh Cut                      | 0.064  | -           |
| Bed Raycasts                  | 0.076  | -           |
| Water Map                     | 0.024  | -           |
| Depth Map View (overlay only) | 0.495  | 0.003       |
| Pressure Solve (wind)         | 0.005  | 0.195       |
| Water passes (five)           | < 0.01 | 0.045 total |

The slowest single cut logged was 1.49 ms (a large mesh, once at load).

## 4. Found and fixed during the test

-   **Persistent references were never cut.** The bridge lives in the worldspace's persistent cell,
    not in a grid cell. The scan now also walks the persistent cell (`add562af7`).
-   **Water and effect meshes were cut.** `Water\TundraStreamTransition01.nif` produced 538
    solid points on the water itself. Only shapes with the lighting shader are cut now. Waterfall
    effect meshes still contribute one lit shape each (probably the rock they pour over); not
    yet checked visually.
-   **The depth map's top row ignored the cut.** Mesh-cut solids now raise its bed, so Water Surface
    and Depth agree with the simulation.

## Not covered

-   Walking or swimming along the river to check that obstacles stay in place as the window moves.
-   The waterfall rock shapes, canal walls in RiftenWorld, and interiors with water.
-   A long play session for leaks or slow growth (references unloading and reloading).
