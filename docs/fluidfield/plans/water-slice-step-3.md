# Water slice step 3: wakes, currents and vanilla hand-over

Plan agreed with @Victor on 2026-10-14 after the first in-game look at step 2 (simulated waves
on the water surface, `7f4ce1cfa`). Decision record: D17.

Feedback that this plan answers:

1. The effect is too strong and too big.
2. It happens even when the player is underwater.
3. It ignores the water's existing flow.
4. A vanilla effect already fakes trails behind actors in water; ours should replace it.

Each step is built, deployed and checked at the Riverwood stone bridge (`coc Riverwood`, then
`player.moveto 22469`) before the next one. Every step ends with a commit.

## Step 1: strength and size (feedback 1)

-   Defaults: Surface Strength 1.0 (from 2.0), Wake Strength 1.0. The 8.0 seen in game was a test
    value.
-   A narrower wake source: radius about 24 units (from 40) and a short Gaussian, so the wake
    starts at body width.
-   Waves fade sooner: a little damping on the height field, set so a wake dies within roughly
    10 to 15 m, plus a setting to tune it.
-   Check: wade along the river at default settings; @Victor tunes the sliders live and the
    chosen values become the defaults.

## Step 2: no wake with the head underwater (feedback 2)

-   An actor makes surface waves only while its head is above the water. The head is the
    skeleton's head node (`NPC Head [Head]`; fall back to the reference's position plus its
    bound height when there is no such node), compared with the water surface height at the
    head's x, y.
-   A diving actor stops making waves as soon as its head goes under, and starts again on
    surfacing.
-   Also check the camera underwater: the water's underside should not show the simulated
    surface normals strangely. If it does, the shader fades the effect when the camera is below
    the surface.

## Step 3: currents from the river's flow (feedback 3)

-   Source: Unified Water's world flowmap, through a one-line read-only accessor in
    `UnifiedWater.h` (approved in D8) returning its `Flowmap` (texture, size, cell offset). The
    flowmap covers the worldspace by cell; its decoding is the one Water.hlsl uses
    (`GetFlowmapDataWorldSpace`: direction from RG, with the 180-degree correction).
-   GPU side: `WaterForcesCS` samples the flowmap at each wet cell's world position and pulls
    the water's velocity toward that flow, with a relaxation time (a setting, about 1 s), so the
    flow develops naturally around pillars and posts instead of being forced through them.
-   Speed: the flowmap gives direction and relative strength; a "River Speed" setting scales it
    to world units per second. Calibrate against the vanilla surface's visible flow speed.
-   When Unified Water is off or has no flowmap, the test current applies as now.
-   Check: wakes drift downstream; water parts around the pillars and leaves a slower patch
    behind each one; the Flow overlay image shows the river direction.

## Step 4: our wakes replace vanilla's (feedback 4, option A)

### 4a. Every nearby actor makes waves

-   Sources: the player plus the nearest actors in the high process (NPCs, horses, creatures)
    within the grid, nearest first, up to the solver's 16 emitters.
-   Each actor's velocity comes from its position between steps, keyed by handle; teleports and
    newly seen actors are ignored for a frame.
-   The same head-above-water rule (step 2) and the water-level band apply to every actor.
-   Size scales with the actor's bound: a horse makes a wider wake than a fox.

### 4b. Jumps and falls into water

-   Vanilla only plays a splash effect. We add a real impulse: when an actor's feet cross the
    surface going down, a short emitter pushes the water down at the entry point, scaled by the
    downward speed (a fall from a bridge makes a big ring, a step off a bank a small one).
    Jumping out of the water makes a smaller upward push.
-   The vanilla splash particles (spray) stay; we do not simulate spray. Revisit if they look
    doubled.

### 4c. Fade out vanilla's displacement trail inside the grid

-   Vanilla's trail is the water shader's wading displacement (the `WADING` block in
    `GetWaterNormal`, sampling `DisplacementTex`). Under `FLUID_FIELD`, its weight is scaled by
    (1 - Fluid Field coverage), so ours replaces it wherever we simulate and vanilla's stays
    beyond the grid edge. No engine patch.
-   Vanilla's flowmap wakes (`ISWaterFlow`) are already off while Unified Water is on.

## Open item carried over: grid-edge artifacts

@Victor saw artifacts at the overlay's edges while moving. With the whole square reaching the
water, they could show as a band about 2,000 units away. Planned after step 4:

-   Update the mesh-cut obstacle window at every block step: cache the block list instead of
    walking all solid points, and drop the 250-unit throttle.
-   Hold newly scrolled-in edge cells at vanilla until their water map slice and obstacles have
    arrived.

## Not in this plan

-   Foam, flow-driven normal advection and Wetness Effects raindrops drifting with our flow (D8
    refinements, later steps).
-   The Phase 1 soak (D16).
