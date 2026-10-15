# Fog slice: ground fog the wind moves (D21)

Goal: ground fog that actors and the weather's wind push around, so it parts and swirls as the player walks through it and drifts with the wind. A first slice of Phase 4. Fluid Field draws the fog itself (D21, revised).

## How it fits together

-   **Wind (exists).** The wind solver: 2D, incompressible, 256 cells over 4096 units around the player, toroidal. So far only the test puff drives it.
-   **Fog layer (new).** How much fog lies on each cell of the wind grid (0 to 1). Carried along by the wind (semi-Lagrangian, like the water's foam), carved where actors pass, thinned where the air is stirred, and refilling over some seconds toward a resting pattern of banks and clearings.
-   **Drivers (new).**
    -   Actors: each moving foot, lower leg and hip (from the skeleton, not the collision capsule; the whole body for creatures) pushes the air along its velocity and ploughs the fog aside by its speed. Standing still pushes nothing, so the fog settles back. The wind keeps the swirls alive with vorticity confinement.
    -   Weather wind: the current weather's wind speed and direction carry the fog as a uniform drift.
-   **Area.** The fog is drawn over 8192 units around the player (the ground map, 512 texels of 16 units); only the wind grid's area is simulated, and beyond it the fog lies in its resting pattern (`FogPattern.hlsli`, shared with the simulation), drifting with the weather's wind.
-   **Rendering (Fluid Field's own pass).** Runs after the world is rendered (water and transparents included), before first person, TAA, upscaling and tonemapping, reading the live depth; over water the fog lies on the water surface. After the deferred composite, a compute pass marches each pixel's ray up to the scene depth through fog lying on the ground: density = Fog Density x the fog layer x exp(-height above ground / Fog Thickness), ground height from the landscape cache under the wind grid. Lit by the sun (Henyey-Greenstein forward scattering) and the ambient light; jittered for TAA.

## Steps

1. **Density field.** Fog density texture (R16F, ping-pong) in the wind solver, a fog pass after velocity: advect, refill toward 1, thin by stirring. Debug overlay image. Setting: Interactive Fog (on/off).
2. **Drivers.** Actors as wind emitters; weather wind as drift in the fog pass.
3. **Draw it.** Ground height map, the ground fog pass, and its call from the deferred passes. Settings: Ground Fog, Fog Density, Fog Thickness, Fog Stirring, Fog Refill.
4. **Test.** At Riverwood: walk and run through it, compare with the field off. Measure cost.

## Limits of the slice

-   Not shadowed by the sun yet.
-   Ground height is the landscape only: fog lies under rocks and floors of buildings it should sit on.
-   The wind ignores terrain and buildings: fog can be pushed through walls. Obstacles for the wind are a later step (the water already has an obstacle map).
-   Shouts, spells and explosions as drivers come with Phase 2's inputs.
