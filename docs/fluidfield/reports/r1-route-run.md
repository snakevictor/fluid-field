# R1 route run: results

Oct 14, 2026 · R1 test routes (`research/R1.md` §8, `ROADMAP.md`) · branch `feature/fluid-field` at `eac4cb32b`

Scope: the R1 test routes on the build with cheaper rays and the four-frame depth view (`eac4cb32b`, FluidField 0-6-0). @Victor drove the Riverwood river and bridge; the rest (Whiterun stables, a horse gallop, a fast travel, Solitude dock, the Riften Ratway) was driven through devbench. A script polled the profiler and the scene twice a second over devbench's HTTP API, and the log, crash logs and process were watched throughout. Earlier sessions: `r1-stage-a-checks.md`, `r1-stage-b-play-session.md`, `r1-stage-d-play-session.md`.

Summary: no crash, warning or error across 6 loading screens, a fast travel, a gallop and three interiors. Two bugs were found. **(1) Landscape is never classified as terrain:** its collision is on layer 17 (`kGround`), not 13 (`kTerrain`), so every terrain hit is counted as a mesh. **(2) Bridge and pier decks become the riverbed,** because rays start 256 units above the water and stop on the first static. Against R1's success criteria: idle and walking cost is now negligible, a gallop is just over the 0.1 ms budget (0.116 ms), a fast-travel refill takes about 6.5 s against a 1 s target, no Bed Raycasts frame came near 2 ms (worst 0.91 ms), bridges fail, and accuracy was not measured.

---

## 1. Setup

| Item     | Value                                                                                                                                           |
| -------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| Runtime  | SkyrimSE.exe, SE (not VR), pid 15204; Crash Logger 1.25.0 installed                                                                             |
| Feature  | `FluidField` 0-6-0, Alpha; build deployed 01:49, game started 01:59                                                                             |
| Settings | 512² grid, 4096 units; Build Depth Map, Verify Landscape, Verify Water and Log Depth Map on; Raycast Bed on, 64-unit spacing, 32 rays per frame |
| Overlay  | Layer overlay open throughout                                                                                                                   |
| Session  | 02:01:41 to 02:15:17, 1608 samples at 2 Hz; quit through the System menu at 02:15:10                                                            |
| Sampling | Each sample: frame CPU time and rolling CPU average (about 300 frames) per pass, fps, cell and position                                         |

Route, in order:

| Segment                    | How                                                                                                       | Time              |
| -------------------------- | --------------------------------------------------------------------------------------------------------- | ----------------- |
| Riverwood river and bridge | Played; screenshots on and under the bridge                                                               | 02:01:41–02:04:55 |
| Whiterun stables           | `coc WhiterunStables` (interior), out through its door                                                    | 02:04:55–02:07:00 |
| Horse gallop               | Spawned horse (`00109E3D`), mounted, W + Alt for 28 s; about 450 units/s, WhiterunExterior15 → Wilderness | 02:07:15–02:07:30 |
| Fast travel                | `Game.FastTravel` to Riverwood's map marker (`000162A4`), mounted                                         | 02:07:44–02:07:56 |
| Solitude dock              | `coc SolitudeDocks01`                                                                                     | 02:08:29–02:09:50 |
| Interior with water        | `tgm`, `coc RiftenRatway01`; then played into the Ragged Flagon and the Ratway Vaults                     | 02:09:53–02:15:17 |

## 2. Cost by segment

CPU, in ms. "Rolling" is the pass's average over about the last 300 frames; "frame" is the single sampled frame.

| Segment                 | Bed Raycasts rolling (mean / max) | Bed Raycasts worst frame | Depth Map View worst frame | fps mean |
| ----------------------- | --------------------------------- | ------------------------ | -------------------------- | -------- |
| Riverwood river, bridge | 0.025 / 0.102                     | 0.52                     | 1.27                       | 59       |
| Whiterun stables        | 0.006 / 0.079                     | 0.08                     | 0.93                       | 57       |
| **Horse gallop**        | **0.075 / 0.116**                 | 0.16                     | 1.12                       | 58       |
| **Fast-travel refill**  | **0.078 / 0.088**                 | 0.09                     | 0.56                       | 56       |
| Riverwood, settled      | 0.007 / 0.073                     | 0.00                     | 1.53                       | 60       |
| Solitude dock           | 0.021 / 0.083                     | 0.24                     | 1.26                       | 38       |
| Ratway                  | 0.039 / 0.112                     | 0.39                     | 0.69                       | 60       |
| Ragged Flagon           | 0.061 / 0.115                     | 0.66                     | 0.73                       | 60       |
| Ratway Vaults           | 0.032 / 0.100                     | **0.91**                 | 0.72                       | 60       |
| **Whole run**           | 0.028 / 0.116                     | 0.91                     | **2.58**                   | 56       |

Compared with the stage d session (`r1-stage-d-play-session.md`):

-   **The idle cost is gone.** Standing still, Bed Raycasts now falls to 0.001–0.002 ms within a few seconds, from a floor of 0.038 ms. It reports `idle yes` once every point around an unmoved centre is resolved.
-   **The gallop is still just over budget.** About 450 units/s held the rolling average at 0.09–0.11 ms, peaking at 0.116 ms (stage d's sprint peak was 0.159 ms). R1 asks for 0.1 ms or less at sprint.
-   **The 0.07 ms cap does not hold.** Single gallop frames ran 0.08–0.16 ms, and interiors produced 0.39, 0.66 and 0.91 ms. The cap is checked between rays, so a ray that steps through several hits, or a slow query in a dense interior, can overshoot it many times over. Still well under the 2 ms per-frame limit.
-   **The depth view is mostly under 2 ms now.** With each rebuild spread over four frames the worst slice was 1.53 ms, apart from one 2.58 ms slice at 02:07:10. That one came during the gallop's stall near the stables (next item).
-   **The low frame rates were not Fluid Field.** At Whiterun stables the frame rate fell to 12–28 fps for about 10 s while the horse was stuck against something. Fluid Field's total over those samples was 0.02 ms except for depth view slices. The Solitude mean of 38 fps includes the frames while the camera was stuck (section 6). Loading screens account for the 0–3 fps minimums.
-   **GPU:** Bed Raycasts, Landscape Cache and Water Surfaces use none. The worst GPU frames were the solver's Pressure Solve (3.28 ms once, usual 0.18 ms) and the depth view's texture update (2.53 ms once), both in loading-screen frames.

## 3. Fast-travel refill

`Game.FastTravel` from the wilderness south-west of Whiterun to Riverwood's map marker, mounted. The loading screen closed at about 02:07:49.1.

| Measure                    | Result                                                            |
| -------------------------- | ----------------------------------------------------------------- |
| Refill time                | **About 6.5 s** (rays until 02:07:55.7; `idle yes` by 02:07:56.9) |
| Rays cast                  | About 2900 (1105 + 1802 in two 300-update summaries)              |
| Bed Raycasts during refill | 0.073–0.089 ms per frame, steady                                  |
| Worst Fluid Field frame    | 0.65 ms (bed rays plus a depth view slice)                        |

-   **It fails R1's "refill within 1 s"** but meets "no frame over 2 ms" easily. The 0.07 ms cap spreads the refill thin.
-   Refilling in 1 s at 60 fps would take about 50 rays per frame, roughly 0.5 ms, still a quarter of the per-frame limit. A burst allowance for the first second after a load or teleport would meet both criteria without raising the cap while moving.
-   Only one fast travel was timed, but the refill time is set by the cap and the lattice size, so a repeat should land close to 6.5 s.

## 4. Bug: landscape hits are never terrain

`BedRaycasts::Cast` labels a static hit as terrain only when its layer is `RE::COL_LAYER::kTerrain` (13). In this game, landscape collision comes back on **`kGround` (17)** with no reference. Every one of 1768 logged hits is on another layer: 867 on layer 1 (`kStatic`, with a reference), 838 on layer 17 (no reference), a few on 2 and 9. Layer 13 never appears. The 300-update summaries say `cached 0 terrain` all session.

Effects:

-   Every terrain hit is counted and drawn as a mesh, so the Raycasts image is all orange outdoors.
-   **The stage d report misreads its screenshot.** Section 5 of `r1-stage-d-play-session.md` takes the orange band along the Brittleshin shore as rocks and banks; it is mostly landscape.
-   The per-ray log is meant to skip terrain, but checks for layer 13, so it logs every landscape hit and hits its per-interval cap early.
-   Bed heights are unaffected: the hit Z is right whatever the label.

Fix at `src/Features/FluidField/BedRaycasts.cpp:131` (and the logging condition above it): treat `kGround` as terrain, or treat a static hit without a reference as terrain.

From the logged sample, roughly half the hits under water are placed meshes (layer 1) and half landscape (layer 17). That speaks to R1 open question 3, but the log is capped per interval and fills with whatever is cast first, so it is not a measure. The fixed classification will give real counts.

## 5. Bug: bridges and piers become the bed

Rays start 256 units above the water surface and end on the first static hit. A bridge deck or pier lower than 256 units above the water is hit first, and becomes the bed.

| Place                    | Screenshot                       | What the depth view shows                                                                                                                                                           |
| ------------------------ | -------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Riverwood bridge, on top | `CS_2026-10-14_02-02-37_877.png` | A straight ridge with parallel edges across the river in Bed; a tan strip (plane under ground) across the river in Depth; the river split in two in Water Surface                   |
| Riverwood bridge, under  | `CS_2026-10-14_02-02-54_641.png` | The same strip under the player, who is waist-deep in the river under the arch                                                                                                      |
| Solitude dock            | `CS_2026-10-14_02-09-38_677.png` | The pier decks as diagonal tan strips in Depth and dim strips in Water Surface. The harbour is up to 868 units deep; open sea past the 1024-unit ray reach shows as "found nothing" |

This fails R1's "bridges never block flow". Both places would block the simulated water exactly where it should flow.

A fix that keeps rocks sticking out of the water as obstacles: when the first static hit is **above** the water surface, cast a second ray from just above the surface. If it finds a hit below the surface, the first hit was something overhead (deck, arch, overhang), and the second is the bed. If not, the first hit is a solid obstacle rising out of the water. R1 §6 already planned rays "just above the water"; the 256-unit start was meant to catch protruding rocks.

## 6. Water and landscape checks

-   **Landscape:** 183 cell checks, all with vertex error 0.00; 9 with between-vertex error over 20 units (steep cells, as in stage b).
-   **Water:** 16 checks. Error 0.00 everywhere except near Whiterun stables:
    -   **Whiterun stables stream:** error **0.53**, plane vs bound **1080.85**, and 1 or 2 sloped surfaces skipped. These are the first non-zero water errors and the first sloped water in any session. A water object there has a plane over 1000 units from its bounds, probably the stream running down past the stables. Worth logging its form ID.
    -   The 16.53 plane gap around Riverwood (stage b, probably the White River) appeared again.
    -   **Interiors:** the Ratway and Ratway Vaults use the interior water height alone (no water objects); the Ragged Flagon adds one water object (the cistern). Error 0.00 in all three.
-   **Interiors in the depth view** (`CS_2026-10-14_02-10-05_737.png`, Ratway): rays trace the floor plan (mesh hits) with the void outside the walls as "found nothing in reach" (2847 of about 4200 points). There are 1582 wet texels around the canal, deepest 245 units. Bed shows magenta (no landscape), as expected indoors. Whether walkways over the canal block it like the bridge was not checked.
-   **Stepped-through hits** were fish (Brook Bass, Salmon), a lumber pile, plants and the spawned horse, as intended.

## 7. Stability

-   No crash, and Crash Logger wrote nothing. The game quit through the System menu (02:15:10); the process ended at 02:15:23.
-   No warnings or errors from any feature, apart from the known ENB and CJK-font lines at start-up.
-   **A test-driving mistake, not a game problem:** `coc` to Solitude was run while mounted. The player moved, but the camera stayed in the mount state (state 10) with the horse in Riverwood, so the screen showed Riverwood's LOD. `Actor.Dismount` and `Game.ForceThirdPerson` fixed it. The spawned horse (`FF0008B8`) was left in Riverwood.

## 8. R1 success criteria

| Criterion (`research/R1.md` §8)                     | Result                                                                                                 |
| --------------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| Amortized cost ≤ 0.1 ms at sprint                   | **Just over:** 0.116 ms at a gallop; 0.104 ms at sprint in stage d. Walking and idle pass easily       |
| Teleport refill ≤ 1 s, no frame over 2 ms           | **Refill fails** (about 6.5 s); **frames pass** (worst Bed Raycasts frame 0.91 ms)                     |
| Wet/dry agreement ≥ 95% against the reference       | **Not measured**: no dense reference run yet                                                           |
| Bed-height error with no visible depth artefact     | **Not measured**; the bridge and pier artefacts (section 5) would fail it                              |
| No crash across loading screens or cell transitions | **Pass** in this session and the last three. The one crash in the stage a session is still unexplained |
| Bridges never block flow                            | **Fail** (section 5)                                                                                   |

## 9. Next steps

1. Classify `kGround` as terrain (section 4), and fix the per-ray log condition with it.
2. Let water pass under overhead structures (section 5).
3. Add a refill burst after loads and teleports, and keep the cap honest by checking it before each ray segment rather than each ray.
4. Re-run Riverwood bridge, Solitude dock and one fast travel to confirm 1–3. The scenarios used here (`coc`, `Game.FastTravel` to `000162A4`, the CS screenshot tool) make that a short scripted run.
5. Build the dense reference (rays every 8 units, §8) and measure wet/dry agreement and bed-height error.
6. Log the form ID with the plane-vs-bound gap, to identify the Whiterun stables stream and the Riverwood water object.

## 10. Notes for running these again

-   Persistent references survive cell unloads, ordinary ones do not. A fast-travel destination must be persistent: map markers are (find one in loaded cells with `inspect kind=refs model=marker_map`), stable gates are not.
-   Dismount (`Actor.Dismount` on the player) before any `coc`, or the camera stays with the horse.
-   A spawned horse follows the camera, not its own heading; set the camera direction before galloping.
-   `communityshaders.capture kind=screenshot` saves a screenshot with the Fluid Field overlay in it. devbench's native capture does not include the overlay.
-   Long scenarios should run with `async: true`, polled with `action: status` (or `POST /api/tool/scenario`), so the MCP client does not time out.
