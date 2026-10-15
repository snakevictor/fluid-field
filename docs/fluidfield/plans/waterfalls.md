# Waterfalls drive the water (later; brainstorm 2026-10-15)

Not a simulation of falling water: vanilla's waterfall bodies stay, and where they land they drive the shallow water so the plunge pool churns, foams and flows away.

## Finding them

-   Scan loaded references as VanillaFoam and MeshCut do (grid cells, interior cell, persistent cell), matching model paths with waterfallbody, waterfallthin or waterfalltall (the ones VanillaFoam keeps). Cache per form ID; rescan every 30 updates.
-   Plunge point, width and fall direction from the body's geometry (lowest vertex band; MeshCut's vertex reading), falling back to `worldBound`. Unverified: whether effect-shader waterfall shapes keep CPU vertex data.
-   Landing: `WaterSurfaces::SampleHeight` at the base, with the surface inside the body's height range. No water there, or a base outside the grid: no source, vanilla stays.
-   The water type at the base (`kEnableFlowmap`) tells a river base from a still pool.

## Inputs to the simulation (new `WaterfallSources`, beside `WakeSources`)

1. **Impact:** a persistent water emitter at the plunge (radius from the width, negative depth rate), pulsed with noise for churn. Small mean, so level relaxation does not fight it.
2. **Outflow:** a radial current from the plunge, biased downstream. Use the emitter's spare `Pad1.x` as a radial speed in WaterForcesCS (cleaner than a ring of emitters).
3. **Foam:** the impact already foams through `SPLASH_RATE`; add `Pad1.y` as an explicit foam rate.
4. **Churn:** scale the existing curl-noise stirring by a waterfall weight; vorticity confinement keeps the eddies.

## Shader

A plunge mask in the flow output's spare `w`: more roughness (with the horizon fade), denser foam cover, more aeration.

## Vanilla effects

Hide base effects (splash, churn, skirt, base mist) only where a waterfall source covers them; today they are hidden wherever the water is simulated. Keep bodies and tall mist.

## Limits

Cap at 4 waterfalls, nearest first, wakes first (emitters are shared, 16); near level only; mods that rename waterfall meshes are missed.

## First slice

1. `WaterfallSources` scan and landing test, with a count in the menu.
2. Impact and outflow emitters appended to the wakes (no shader changes).
3. `Pad1` radial speed and foam rate in WaterForcesCS and WaterFoamCS.
4. Plunge mask in WaterOutputCS, used by WaterNormal.hlsli.
5. VanillaFoam hides base effects only under a waterfall source.
