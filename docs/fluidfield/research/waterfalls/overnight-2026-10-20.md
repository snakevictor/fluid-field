# Overnight research, 2026-10-20: real-time falls on vanilla's meshes

Four studies, each with its own document in this folder:

| Study                                                                    | Document                         | Kind                                                      |
| ------------------------------------------------------------------------ | -------------------------------- | --------------------------------------------------------- |
| Survey of real-time surface-water techniques and shipped games           | `realtime-sim-survey.md`         | Web research                                              |
| Pipe model (Cities: Skylines style) on each mesh's texture grid          | `pipe-model-on-meshes.md`        | Prototype on the 18 bake dumps (scratchpad `pipesim*.py`) |
| Lagrangian "strand sheet"                                                | `strand-sheet-prototype.md`      | Prototype on the same dumps (scratchpad `strandsim.py`)   |
| Engine integration (frame order, atlas, binding, displacement, coupling) | `realtime-integration-design.md` | Codebase and engine design                                |

## What we learned about the meshes

-   **Every vanilla falls mesh climbs a hidden upstream face before its lip.** For the first 40-70% of v the mesh rises: the top fall starts at z -515, under its pool at -250, and crests at -226. The D74 sim fed water in at the top edge and pushed it uphill, which caused the slow water, dead pockets and jelly.
    -   **Fixed in D79:** each column now starts at its crest.
-   **Rows vary up to 8x in world length.** The drop gets about 40 of 192 rows. Fine detail there, such as a player-sized cut, spans only 2-4 rows.
-   **Cascades do not meet edge to edge.** An upper curtain hangs over the rising tongue of the lower mesh. The natural hand-off is through the pool: upper foot outflow, then the pool level, then lower inflow.
-   **Some neighbouring meshes overlap over thousands of texels** (pairs 2/3/4, 6/7 and 8/11 in the dumps). Each overlap needs one owner.
-   **Rock below the crest is 17-57% of each mesh.** It is mostly the sheet's sides and foot tucking into the banks, which correctly act as walls.

## The physics in one line

A falling sheet only passes information downstream (Froude number 3-30), except right in front of an obstacle. On a free curtain there is no pressure term, and a sheet thins as it speeds up (discharge q = h·u is what is conserved). So the visible behaviour is:

-   parting round obstacles;
-   wakes that stay open behind them;
-   thinning;
-   breakup, after about 6·q^0.32 m of fall.

It is not waves.

## Candidates, all real time and lightweight

All costs below are estimates, not measurements.

1.  **Strand sheet (recommended by the survey).**
    -   **Model:** 64-128 streamlines per mesh, marched down v each frame, steered by a distance field of the rock and by actor capsules. Strands never cross, and where they bunch the water whitens. Thickness = discharge / (speed × spacing).
    -   **Prototype results:**
        -   Rocks part the sheet, and gaps stay open and heal downstream.
        -   A player cut travels down at the flow speed and heals from the top about 0.7 s after they leave.
        -   Pulses from the lip ride down the travel-time lines.
        -   The streak label never folds, and the result feeds today's t124 layout unchanged.
    -   **Cost:** about 0.01-0.03 ms for the usual 3-6 nearby meshes, about 0.09 ms for 20.
    -   **Limits:** flat ledges and eddies (subcritical flow), and UV charts that do not run top to bottom.
2.  **Pipe model / shallow water on the mesh's texture grid.** This is the Cities: Skylines family; Sea of Thieves ships it on its waterfall and stream meshes.
    -   **Model:** the gravity split into and along the surface (Wang, Miller and Turk 2007), plus an artificial "sheet stiffness" κ that is required on vertical faces.
    -   **Prototype results:** stable at one step per frame and mass-conserving. Water parts round rocks with pillows upstream and dry wakes behind, and speeds up down drops. A pool coupling between two meshes balanced inflow and outflow.
    -   **Cost:** about 0.05-0.1 ms for 20 meshes at half resolution.
    -   **Limits:**
        -   κ is a tuning knob, not physics.
        -   Water collects in the troughs along mesh edges.
        -   Streaks smear without a stream-function solve on top.
        -   It needs a 4-6 s prewarm on cell load.
3.  **Cheapest option.** Keep the static bake and add actor "cut" decals driven by the baked run time, at about 0.01 ms. It gives player response only.

Shared rendering upgrades:

-   texture coordinates advected by the sim (streak label, run time), so the texture moves without pulsing;
-   thickness → translucency through hashed stochastic discard in both depth passes, resolved by TAA;
-   breakup by alpha erosion;
-   spray where obstacles stop the water's momentum;
-   strand or flow ends feeding the plunge emitters;
-   a light vertex offset applied in both depth passes, with the previous offset for motion vectors.

## Engine findings (integration design)

-   **The sim must run in a new `FluidField::EarlyPrepass`.** From the hook offsets alone (not yet checked in game), the lit falls' depth pre-pass seems to run before `FluidField::Prepass` in the same frame. If the sim stepped in `Prepass`, the two depth passes would see different states.
-   **Possible bug in the seam dither today (D67).** It takes its frame from `Time`, which advances in `Prepass`, so the depth pre-pass and the lit pass may use different patterns.
    -   **To check in game:** log both passes' frame numbers (phase 0 of the design).
-   **Atlas:** one atlas in 32×32 blocks with first-fit allocation, so meshes stream in and out without repacking. A tile list over awake meshes gives one dispatch per pass. t124 becomes the atlas, with a per-draw rectangle in b13.
-   **Why D53 translucency drew nothing:** the engine probably chooses blended or opaque when the geometry is set up. The test is to attach the alpha property to the D73 dense copy before `SetupGeometry`.

## Suggested next steps

1.  Judge D78 (old solver retired) and D79 (crest start) in game.
2.  Phase 0: confirm the frame order and freeze the dither frame in `EarlyPrepass`.
3.  Port the strand march to C++ in place of the D74 solve inside the existing async bake. The shader needs no change, so the strands can be judged in game before any GPU work.
4.  Move it to the GPU per frame (atlas, actor capsules), then thickness translucency and breakup.
