# Waterfalls: status at D60 (2026-10-17)

Where Fluid Field's waterfalls stand after the D52-D60 rounds: what works,
what does not, and where to pick up. The decisions behind each step are
D25-D60 in [DECISIONS.md](../DECISIONS.md). The supporting research is in
[research/waterfalls](../research/waterfalls).

## Verdict from the last review

The streak warp (D60) holds the texture steady: it no longer jumps or runs
backwards. But "besides coloring, the flow itself doesn't look much
different from vanilla". The bend around boulders is too subtle to read.
Recolouring (foam rims, wakes, whitening) is visible. The water's motion is
not.

## What works

-   **Vanilla's falls meshes stay, cut cleanly.**
    -   The falls are cut per vertex where they are inside rock (D57 ray
        parity) and under the pools (D58 bilinear pool cut), with
        SV_ClipDistance in both the depth pre-pass and the lit pass.
    -   Holes from the top-down ground map are gone.
    -   One vanilla gap (screenshot 603) remains. The plan is to cover it
        with foam.
-   **Downstream direction is always right.**
    -   Every vanilla falls mesh's v runs downstream.
    -   The scroll is vanilla's own, integrated per material on the CPU
        (D60).
    -   The backflow, marbling and back-and-forth of D53-D59 are fixed at
        their root causes.
-   **Each mesh's own flow is solved on its texture grid** (D59/D60).
    -   This is the first data that sees curtains face-on, so a boulder
        through a vertical sheet is visible to it.
    -   It costs one texture per mesh, baked in about 10 s after load for 18
        meshes.
-   **Spray.** GPU particles from rock hits, seams, drops and plunges. The
    user liked these.
-   **Shallow water.** The falls feed its flow and foam without denting its
    surface (D55/D56).

## What does not work yet

-   **Visible flow.** The warp keeps 75% of a bend, only within 14 texels of
    rock, eased back where streaks squeeze, on vanilla's low-contrast
    texture. That leaves the motion looking like vanilla's.
-   **Translucency.** The lit surface has no alpha property. Borrowing the
    layers' blend state draws it invisible (D53-D56). The setting defaults
    to 0.
-   **The seam where falls meet the pools.** It is better than before but
    still shows breaks (611/612, 584).
-   **Waterfall fog.** Removed (D51), pending a better approach.

## Candidate next steps

These are ranked by expected visual gain. The detail is in
[techniques-deep.md](../research/waterfalls/techniques-deep.md).

1.  **Make the motion read.** Options:

    -   Stronger and wider bends: the full warp, a larger reach, and
        "octave" streak layers so streaks fork rather than pinch.
    -   Per-streak speed variation from a hash of the streak label.
    -   Flow noise in white water.
    -   Our own streak or foam texture laid in the warped (s, r) frame, over
        vanilla's.

    Judge each in motion, not in stills.

2.  **Obstacle masks as the main cue.** A pillow upstream of each rock, its
    rim, a split gap below it that heals downstream, and V-wake streaks.
    Real water is white exactly where the warp is weakest.
3.  **One foam look across the seam.** Shared foam shading between the falls
    and the water shader. The bottom of each fall reads the shallow water's
    foam, and a "boil" covers the line.
4.  **Translucency.** Draw the falls in the blended pass properly. That
    means registering them with it, not only switching their alpha state.
5.  **Subdivision or tessellation (later).** Shape over rock, driven by the
    same bake.

## How to test

-   Launch with the scratchpad `launch.sh`. Set `set gamehour to 11` and
    `fw 81a`.
-   Use the devbench free camera `frame` on falls references, for example
    `0001AC75`, the fall with the rock pillar.
-   For long-session effects, run `sgtm 8` for a minute instead of playing.
    Use `sgtm 0.1` plus captures (with `allowTimeScale`) to measure motion
    between frames.
-   Waterfall Shading debug 1 shows the bake: red is the streaks' spread,
    green the wake, blue foam.
-   The log lines "Waterfall rock cut" and "Waterfall flow bakes" give the
    per-site stats.
