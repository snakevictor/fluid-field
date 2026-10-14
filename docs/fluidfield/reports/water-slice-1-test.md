# Flowing water slice 1: in-game test results

Oct 14, 2026 · D8 flowing water slice, R1 fixes · branch `feature/fluid-field` at `836a104c7` (code at `2b5778a86`)

Scope: @Victor's in-game test of the R1 fixes (`2b5778a86`: bridges, landscape layer, refill) and the first shallow-water slice (`e45377d93`), FluidField 0-7-0. The test plan had three parts: A, the bridge fix; B, the water simulation; C, refill after travel. The session was watched from outside: a script polled the profiler and the scene twice a second over devbench's HTTP API, and the log, crash logs and process were watched. Seven Community Shaders screenshots were taken. Earlier: `r1-route-run.md`.

Summary: **Part A passes.** Rays now classify landscape correctly, and the Riverwood bridge and Solitude piers no longer block the water; only the bridge pier stays dry, as it should. **The refill fix works:** the depth map refills in about 1–1.5 s after a load, at about 0.5 ms per frame. **Part B partly works:** ripples spread from the player and bend round the bridge pier, and the whole water simulation costs about 0.05 ms of GPU. But **after every load the water is not calm.** It floods, drains and flows fast across the whole grid, because the solver restarts from the previous location's map before the new one is uploaded (section 4). Steps 8–10 (calming down, the test current, sprinting and swimming) were not covered by the screenshots. No crash, warning or error.

---

## 1. Setup

| Item     | Value                                                                                                                                                                  |
| -------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Runtime  | SkyrimSE.exe, SE (not VR), pid 18464; Crash Logger 1.25.0 installed                                                                                                    |
| Feature  | `FluidField` 0-7-0, Alpha; build deployed 02:24, game started 02:27                                                                                                    |
| Settings | 512² grid, 4096 units; Enabled, Build Depth Map, Raycast Bed, Simulate Water, Show Layer Overlay, Log Depth Map Details, Verify Landscape and Verify Water             |
| Session  | 02:30:34 to 02:34:59 (511 samples at 2 Hz); quit through the System menu at 02:34:53                                                                                   |
| Route    | Riverwood, along the river to the bridge (Riverwood02, RiverwoodBridge), into the river by a pier; `coc SolitudeDocks01`; `coc` Riverwood; `coc SolitudeDocks01` again |
| Loads    | 4 or 5: Solitude 02:33:34, a second `coc SolitudeDocks01` from the console around 02:33:48, Riverwood 02:34:03, Solitude 02:34:15 (resets at each)                     |

Screenshots (in `Screenshots/`):

| File                             | Where                                                     |
| -------------------------------- | --------------------------------------------------------- |
| `CS_2026-10-14_02-32-18_062.png` | River bend near Riverwood02, from the bank                |
| `CS_2026-10-14_02-32-21_430.png` | Bank facing the bridge, about to wade in                  |
| `CS_2026-10-14_02-32-59_990.png` | **In the river under the bridge, beside a pier**          |
| `CS_2026-10-14_02-33-40_443.png` | Solitude dock, about 6 s after the load                   |
| `CS_2026-10-14_02-33-49_647.png` | Solitude dock with the console just closed (no water row) |
| `CS_2026-10-14_02-34-04_450.png` | **Riverwood, about 1 s after the load (black frame)**     |
| `CS_2026-10-14_02-34-18_245.png` | **Solitude dock, about 3 s after the load**               |

## 2. Part A: bridges and landscape

**Passes.**

-   **Landscape is terrain now.** The ray summaries cache thousands of terrain points (up to 6186) where the route run had none. The Raycasts image is mostly green outdoors, with orange in patches for rocks, banks, piers and footings.
-   **The Riverwood bridge no longer blocks the river** (`02-32-59`). In the river beside a pier, Water Surface, Depth and Water Depth all run continuously under the bridge. The one dry patch is a small blob at the player's marker, shaped like the **pier's footprint**: a real obstacle, correctly kept. Raycasts shows an orange band along the bridge's line (piers and footings), but the deck is no longer taken as the bed. Compare the route run's screenshots, where a tan strip cut the river at this spot.
-   **The Solitude piers no longer block the harbour** (`02-33-40`). The tan areas in Depth are the shore and one long shape out in the water. Raycasts has an orange oval at the same spot, most likely a ship's hull reaching below the water, which is a real obstacle.
-   **Overheads are found:** 4725 over the session. Some logged overheads are landscape hits (layer 17) on banks well above the water. For example, at (19328, −47744) the surface is −250, the first hit is landscape at −105, and the ray re-casts underneath. This is unverified: if the upward probe can hit the heightfield or a nearby static from below, a bank could be recast as wet bed. Several dark-blue ("nothing in reach") specks along the banks in the Raycasts images may be this. Worth a look.

## 3. Part C: refill after travel

**Mostly passes.**

| Load                | Bed Raycasts during refill           | Refill        |
| ------------------- | ------------------------------------ | ------------- |
| Solitude, 02:33:34  | 0.51–0.52 ms per frame for 3 samples | About 1–1.5 s |
| Riverwood, 02:34:03 | 0.51 ms per frame for 3 samples      | About 1–1.5 s |
| Solitude, 02:34:15  | 0.52–0.53 ms per frame for 2 samples | About 1 s     |

-   The burst works: about 0.5 ms per frame for about a second, then a trickle (0.03–0.09 ms) as the player moves. The last summary shows 6346 rays in 300 updates (45 ms in all), with `idle yes` already reached.
-   R1 asks for a refill within 1 s and no frame over 2 ms. The refill takes about 1–1.5 s; at 2 Hz sampling it cannot be pinned down more closely. Frames peak at 0.53 ms, far under 2 ms. The route run took about 6.5 s.
-   In the screenshot about 1 s after the Riverwood load (`02-34-04`), the bottom of the Raycasts image is still grey (not yet cast). The image 6 s after the Solitude load (`02-33-40`) is complete.
-   Minor: one summary reports `5906 cast in the last 300 updates (0.000 ms total)`, so the burst's time does not always reach the ray-time counter.

## 4. Part B: the water simulation

### What works

-   **Ripples:** under the bridge (`02-32-59`), Waves shows red and blue rings spreading from the player, with rings bending round the pier.
-   **Flow** is faint while wading, as expected for slow movement.
-   **Cost:** the solver's passes (Forces, Advect, Height, Velocity, Output) cost about **0.05 ms of GPU per frame in total**, with single-frame peaks up to 0.96 ms (Water Advect). **Water Map**, the CPU side that builds the solver's bed and surface input, costs **0.083 ms of CPU** on average (peak 0.33 ms), almost as much as Bed Raycasts.

### The water is not calm after a load

Every screenshot taken soon after a load shows the water far from rest, while nobody is in it:

| Screenshot                        | Water Depth                                                                          | Flow                                        | Waves                                                   |
| --------------------------------- | ------------------------------------------------------------------------------------ | ------------------------------------------- | ------------------------------------------------------- |
| Riverwood, about 1 s (`02-34-04`) | **Water across the whole grid**, where Depth says dry ground except a strip of river | Bright, every direction, across the grid    | A red band (above vanilla) along the river              |
| Solitude, about 3 s (`02-34-18`)  | Shallow almost everywhere, though the harbour is up to 673 units deep                | Bright, every direction, across the harbour | **The whole harbour blue** (below vanilla)              |
| Solitude, about 6 s (`02-33-40`)  | Filling from the edges                                                               | Streams along the shore                     | The harbour blue, a red mound near the south-west shore |

The likely cause: `ShallowWaterSolver::Reset()` (`src/Features/FluidField/ShallowWaterSolver.cpp:159`) only clears `historyValid`, so the next step "starts every cell from vanilla water". It does not clear `mapUploaded`. After a load, the solver restarts at once on the **previous location's** bed and surface map, until `WaterMapBuilder` has built and uploaded the new one.

-   Arriving in Riverwood from Solitude, the solver fills the grid using the harbour's map (wet almost everywhere). It is then given Riverwood's map (mostly dry), and the water pours off the land. That is the flooded grid and chaotic flow in `02-34-04`.
-   Arriving in Solitude from Riverwood, it starts on Riverwood's mostly dry map, so the harbour starts nearly empty. It then fills from the edges once the harbour map arrives: the blue (below vanilla) harbour in `02-34-18` and `02-33-40`.
-   The blue band on the river in `02-32-21`, the first minutes of the session, may be the same effect from the game's first load.

Fix: on reset, mark the map as not uploaded, and hold the solver (or keep the water at vanilla level) until a map built for the new location has been uploaded. Then start from vanilla water on that map. The two screenshots taken more than a few seconds after a load were both at Solitude, both still unsettled; whether the water settles to the vanilla level on its own, given time, was not seen.

### Not covered

-   **Step 8, calming down:** no screenshot after standing still.
-   **Step 9, the test current:** the log and screenshots do not show it set; no settings were read during the session.
-   **Step 10, sprinting through shallow water and swimming deep:** no screenshot. The instability above came from loads, not from anything the player did.
-   **Step 11, a ripple screenshot:** `02-32-59` covers it.

## 5. Other observations

-   **The water object behind the 16.53 plane gap** is `000F762D`, centred at (22690, −47122), just east of Riverwood. The new log field identifies it.
-   **Water checks:** all exact (error 0.00, none missed), 12 to 21 surfaces.
-   **Landscape checks:** 143, all with vertex error 0.00.
-   **The console pauses the simulation.** With the console open (02:33:36–02:33:54) only Water Surfaces ran, and the overlay dropped the water row (`02-33-49`). The simulation resumed when it closed. This looks intended.
-   **The wind solver's Pressure Solve** had one 4.96 ms GPU frame (average 0.19 ms, as before).
-   **Depth Map View:** worst slice 1.14 ms, now consistently under 2 ms.
-   **Frame rate:** 58.8 fps mean; the minimums are loading-screen frames.

## 6. Stability

No crash, and Crash Logger wrote nothing. No warnings or errors from any feature, and no shader compile failures. The game quit through the System menu at 02:34:53.

## 7. Next steps

1. Hold or rebuild the water solver until the new location's map is uploaded after a load (section 4), then repeat Part C.
2. Run steps 8–10 with screenshots: stand still 20–30 s, set the test current to 150 downstream, sprint through shallows, swim at Solitude.
3. Check whether landscape banks are counted as overheads and re-cast as wet bed (section 2).
4. Send the burst's ray time to the ray-time counter.
5. Keep an eye on Water Map's 0.083 ms of CPU against the budget.
