# Flowing water slice 1, round 3: in-game test results

Oct 14, 2026 · D8 flowing water slice, R1 · branch `feature/fluid-field` at `31379d1a1` (cheaper rays, steady water on new beds)

Scope: @Victor's third in-game test of the water slice. Since round 2 (`water-slice-1-round-2.md`):

-   The coarse lattice no longer probes for decks.
-   Fine refinement runs only within about 1000 units of the player, after the coarse pass.
-   A hard time limit is checked before every ray.
-   When the bed under existing water changes, the water surface is held instead of making a wave.
-   The Raycasts image distinguishes purple (open under a deck or arch), orange (a mesh above the water), brown (a mesh below it) and green (landscape).

FluidField 0-7-0. Watched as before: the profiler and scene polled twice a second over devbench's HTTP API, plus the log, crash logs, process, and one screenshot.

Summary: **the ray cost is fixed.** Bed Raycasts averages 0.057 ms over the session, 0.075 ms when sprinting, and stops within about 3–10 s of standing still. Apart from the intentional post-load burst (0.50 ms), no frame exceeded 0.36 ms, against 2.70 ms in round 2. **No stray waves** were seen. **The bridge is still too solid:** the Raycasts image shows the bridge's line mostly orange, with only one small purple patch, so most arches are not recognised as open. Jumping and attacking do not disturb the water; that input does not exist yet. The stand-still, test-current and sprint checks were not covered. No crash, warning or error.

---

## 1. Setup

| Item       | Value                                                                                                                                                                                                             |
| ---------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Runtime    | SkyrimSE.exe, SE (not VR), pid 15216; Crash Logger 1.25.0 installed                                                                                                                                               |
| Feature    | `FluidField` 0-7-0, Alpha; build deployed 03:10, launched through SKSE at 03:11                                                                                                                                   |
| Settings   | 512² grid, 4096 units; Enabled, Build Depth Map, Raycast Bed, Simulate Water, Show Layer Overlay, Log Depth Map Details, Verify Landscape and Water; 64-unit spacing, 32 rays per frame                           |
| Session    | 03:14:51 to 03:19:35, 547 samples at 2 Hz; quit through the System menu at 03:19:33                                                                                                                               |
| Route      | Save near Whiterun (by the White River, east of the city) → load to Riverwood (03:16:44) → along the river through Riverwood01 and 02 → the bridge (RiverwoodBridge) → in the river under an arch beside a pillar |
| Screenshot | `CS_2026-10-14_03-18-49_899.png`: in the river beside a bridge pillar, under an arch                                                                                                                              |

## 2. Results against the test plan

| Check                                                 | Result                                                                                                               |
| ----------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- |
| 1. Ray cost around 0.1 ms or less, no big frames      | **Pass.** 0.053–0.075 ms average at every speed; worst frame outside the load burst 0.36 ms                          |
| 1. Raycasts finishes within a few seconds of stopping | **Pass.** About 3–10 s (section 3)                                                                                   |
| 2. No stray waves                                     | **Pass in what was seen.** Waves shows only the player's own rings in the one screenshot, and the user reported none |
| 3. Bridge: big dry shape mostly orange?               | **Yes**, so the clearance rule looks too strict (section 4)                                                          |
| 3. Purple under the arches?                           | **Barely.** One small purple patch; the arch beside the player is not purple                                         |
| 4. Stand still 20–30 s, test current, sprint          | Not covered                                                                                                          |

## 3. Ray cost

| Moving                  | Samples | Rolling average, mean | Rolling average, max | Worst frame | Round 2 mean |
| ----------------------- | ------- | --------------------- | -------------------- | ----------- | ------------ |
| Standing (under 20 u/s) | 301     | 0.055 ms              | 0.461 ms             | 0.50 ms     | 0.080 ms     |
| Walking (20–250)        | 103     | 0.053 ms              | 0.161 ms             | 0.32 ms     | 0.126 ms     |
| Running (250–450)       | 92      | 0.062 ms              | 0.088 ms             | 0.36 ms     | 0.172 ms     |
| Sprinting (over 450)    | 12      | 0.075 ms              | 0.117 ms             | 0.08 ms     | 0.195 ms     |
| **Whole session**       | 510     | **0.057 ms**          | 0.504 ms             | **0.50 ms** | 0.104 ms     |

-   **The time limit holds.** While there is work, Bed Raycasts runs at a steady 0.070–0.079 ms per frame. Every busy 300-update summary reports 22–25 ms in total (about 0.075 ms per update), whatever the ray count. Rays per busy summary fell from 8000–9500 in round 2 to 1200–3800.
-   **Load bursts:** the first fill after the save and the Riverwood load each spent 0.50 ms per frame for about a second (5880 rays in one summary after the Riverwood load), as intended. No frame came near 2 ms; round 2's worst was 2.70 ms.
-   **It finishes after stopping:**

    | Stopped at | Idle (frame ≈ 0) by | Time   |
    | ---------- | ------------------- | ------ |
    | 03:17:40   | 03:17:45–50         | 5–10 s |
    | 03:18:37   | 03:18:40            | ~3 s   |

    At the bridge the summaries still said `idle no` for a while, with only 1–24 rays per 300 updates but 3–7 ms total. Some per-update work continues without casting, about 0.02 ms per update on average, in occasional frames.

-   **Overheads are only fine refinement now:** 6302 over the session, against 52,628 in round 2. In the logged rays, 299 of 1203 were overheads (all on static meshes, layer 1).
-   **Other passes:**
    -   Depth Map View: 0.252 ms average, worst slice 1.70 ms (25 sampled slices over 1 ms, none over 2 ms).
    -   Water Map: 0.030 ms of CPU, half of round 2.
    -   Water solver GPU: about 0.04 ms per frame; worst single frame 0.59 ms (Water Forces). Round 2's 2.4–2.6 ms spikes did not recur.
    -   Wind solver's Pressure Solve: its occasional GPU spike (3.45 ms).
-   **Frame rate:** 59.3 fps mean.

## 4. Bridge

From `03-18-49`, in the river beside a pillar under an arch:

-   **Raycasts:** a wide orange band along the bridge's whole line, and a single small purple patch near the top of the image. Landscape is green and riverbed meshes are a little brown. The arch the player is standing beside, with open water under it in game, is orange rather than purple.
-   **Water Surface and Depth:** a thin dim or tan strip follows the bridge line across the river. The bridge is still read as solid along its length.
-   **Water Depth (the simulation):** better than round 2. The dry obstacle near the player is a compact pillar-sized shape, with water around it on both sides, not the 1000 × 400 block from round 2.
-   **Waves:** the player's rings spread and are interrupted by the pillar shape; the rest is calm.

Reading it by the test plan's rule: the orange band means the code treats most of the bridge's footprint as stone rising out of the water. In game the arches are open. So the **clearance rule is too strict**: an underside that is only a little above the water at the edges of the arch is treated as touching it, and the arch's open middle is not reached at the fine spacing. Possible checks:

-   Log the measured underside height above the water under one arch to see how close the arch's lowest edge sits to the surface.
-   Compare with the 8-unit "touching" threshold from `ea16fae41`.
-   Check that fine rays are cast under the arch's middle, not only at the pillars.

## 5. Stray waves

**Passes as far as seen.** Walking through the village and along the river, the user reported no stray waves, and the one screenshot shows only the player's own rings. This is the case that failed in round 2 (`02-57-02`, `02-57-48`), so holding the surface when the bed changes works. Only one screenshot was taken this round, so this rests mostly on what the user saw.

## 6. Water inputs that do not exist yet

Reported by the user during the session:

-   **Jumping does nothing to the water.** The only water input is the player's wake (`FluidField.cpp:656–679`). It uses horizontal speed only (it needs more than 20 units/s), its `depthRate` is 0, and it switches off when the player is more than 32 units above the surface. So jumping in place, landing and bobbing make no splash.
-   **Attacking does nothing to the water.** Weapon swings, spells and projectiles add no emitters, and neither do other actors or objects.

Both are missing features rather than bugs. Candidates for a later slice:

-   A splash pulse (`depthRate` or an outward velocity ring) when the player enters the surface with downward speed.
-   Emitters from weapon or hand nodes moving under the water.
-   Wakes from other actors.

## 7. Other observations

-   **The White River is confirmed.** The water object `000F762D` with the 16.53 plane gap was reported centred at (31074, −11768) near Whiterun, as well as (22690, −47122) and (24033, −46222) near Riverwood. It is one water object spanning both, as guessed in the stage b report. The reported centre depends on which of its bounds is checked.
-   **Loads:** the water solver started about 1.5 s after the Riverwood load (03:16:44.7 → 03:16:46.2). After the save load, about 28 s passed before it started (03:14:51 → 03:15:19.7), but the first samples may still have been in the loading screen.
-   **Landscape checks:** 39, all with vertex error 0.00. **Water checks:** 3, all exact.

## 8. Stability

No crash, and Crash Logger wrote nothing. No warnings or errors, and no shader compile failures. The game quit through the System menu at 03:19:33.

## 9. Next steps

1. Relax or fix the arch clearance so open arches show purple and the river runs unbroken under them (section 4). Then a screenshot from under each arch.
2. Run the remaining slice checks: stand still 20–30 s, the test current at 150 downstream, sprinting through shallows, and swimming in deep water at Solitude.
3. Decide whether jumping, splashes, attacks and other actors belong in slice 1 or a later slice (section 6).
4. Look at the leftover per-update work while the lattice is almost done (about 0.02 ms, `idle no` with few rays).
