# Fog slice: ground fog the wind moves (D21)

Goal: ground fog that actors and the weather's wind push around, so it parts and swirls as the player walks through it and drifts with the wind. A first slice of Phase 4, built on Exponential Height Fog's volumetric fog instead of a new renderer.

## How it fits together

-   **Wind (exists).** The wind solver: 2D, incompressible, 256 cells over 4096 units around the player, toroidal. So far only the test puff drives it.
-   **Fog density (new).** A 2D multiplier on the fog's density, on the wind grid. Carried along by the wind (semi-Lagrangian, like the water's foam), drawn back to 1 over some seconds (fog refills), and thinned where the air is stirred (actors mix clear air into it). Starts at 1 everywhere, so with nothing moving the fog is exactly vanilla's.
-   **Drivers (new).**
    -   Actors: each moving actor near the player pushes the air along its velocity, like wakes push the water (reuse `WakeSources`' actor scan). This carves trails and swirls.
    -   Weather wind: the current weather's wind speed and direction carry the fog as a uniform drift.
-   **Rendering (extend Exponential Height Fog).** Its volumetric material pass (`VolumetricFogMaterialCS`, near and far grids) multiplies its extinction by the density field where the field covers. Every later pass (lighting, shadows, integration along the view ray, temporal history) handles the rest unchanged. The field's placement goes in `SharedData::fluidFieldSettings`; its texture is bound for the material pass only.

## Steps

1. **Density field.** Fog density texture (R16F, ping-pong) in the wind solver, a fog pass after velocity: advect, refill toward 1, thin by stirring. Debug overlay image. Setting: Interactive Fog (on/off).
2. **Drivers.** Actors as wind emitters; weather wind as drift in the fog pass.
3. **Hook into the fog.** Bind the density SRV in the volumetric material pass when Fluid Field's fog is active; multiply extinction there; fade the field out toward the grid edge. Settings: how strongly actors part the fog, refill time.
4. **Test.** A foggy weather at Riverwood, volumetric fog on: walk and run through it, compare with the field off. Measure cost.

## Limits of the slice

-   Needs Exponential Height Fog's volumetric fog. With it off, nothing changes.
-   2D: the field changes the whole column of fog above a point. Fog is densest near the ground, so that is where the effect shows; height-aware mixing can come later.
-   The wind ignores terrain and buildings: fog can be pushed through walls. Obstacles for the wind are a later step (the water already has an obstacle map).
-   Shouts, spells and explosions as drivers come with Phase 2's inputs.
