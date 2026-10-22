# Next visuals: the falls and the water they join

Oct 21, 2026 · drafted with @Victor after D174 (absorbs the falls-realism plan agreed after D173)

The falls are drawn by the river's own water shader (D159, D171), so they match the water around them. The steps below make that water read as falling water and join it to the river and the pools without a seam. The principle: most of a waterfall's look follows from a few quantities of its flow (speed, aeration, the surface's large shape), so the flow bake produces them and the shading reads them, rather than effects being placed by hand. Each step ends in a build verified in game (screenshots at the same spots, PresentMon for anything that adds work per frame) and a decision record.

## 1. Contact foam per pixel (D174, done)

Foam wherever the sheets touch something, per pixel, in the river's foam pattern on fixed world planes, drifting down the falls; the falls' detail normal on the sheet's own axes. See D174. Left to check on screen: the rim along the lip and the waterline.

## 2. No seam between the river and the falls (after the flow sim)

Two tries failed (a cross-fade of the water type's maps sampled at world x, y; the river's flowmap normals at world x, y): see scratchpad research_crest_blend.md. The user's direction: only the river's existing normals come onto the crest and morph into the falls' baked normals; no glassy band at the lip, no geometry change.

-   **One shared parameterization:** per sheet vertex, the arc length from the lip, the position across it and the lip point. On the sheet, the river's own normal function sampled at the lip point carried downstream by the run time (the river's own phase clock, its flowmap read at the lip only), its slopes laid on the sheet's frame. At the lip it is the river's surface exactly; further down it stretches as accelerating water does, and hands over to the falls' baked normals.

## 3. Flow sim v2 (in progress, after the crest blend's two failed tries; the blend waits on research)

The march (FallStrands) already computes physical speed, aeration (impacts, pile-ups, height fallen), swell (surface height) and disturbance per texel, but only aeration reaches the shader (the bake's "wake" channel) and swell only moves vertices. Steps:

1.  **The river speeds up around the falls** (D170's solver part, FallLips.cpp; the falls-side texture changes left out): the simulated water is pulled toward each lip and drawn down over 384 units upstream and 64 to each side (the user: the surrounding area, not the whole river).
2.  **A second bake texture** with what the march knows: speed in world units a second, aeration (with drops far more aerated than runs: from how steep and how far fallen), surface height, disturbance.
3.  **The shading reads them, for a 3D look of the water as a volume** (the user):
    -   Surface height to a per-pixel normal (finite differences on the bake), so the sheet bulges, piles up and sags between its vertices.
    -   Aeration as **bubbling** (the user): a fine animated bubble layer carried by the flow, denser with aeration, scattering light (milky, lit from within), not only a whiter colour; opacity and roughness rising with it.
    -   Speed: streaks and glints stretched along the flow, the texture's scroll from the real speed.
    -   Thickness behind the surface (the water's depth through the sheet) tinting what is seen through it.

## 4. The sheet's shading on those fields

-   The glassy tongue at the lip (smooth, clear, mirror-like) breaking up over a length into streaks, then white water.
-   Patterns and glints stretched along the flow with speed: streaks, not the round sparkles (and their bloom squares) seen now.
-   Translucency: light through the water when the sun is behind it.
-   Darker where the sheet hugs the rock; the rock's shadows on it.
-   Wet rocks: Community Shaders' Wetness Effects should already do it; if not, its implementation is reused.

## 5. Motion vectors for the falls

The falls write none, so TAA and the upscalers smear the moving water. Write them from the flow's scroll, as the wobble's are (D85).

## 6. The plunge pool

Replaces the flat white river foam on plunge pools.

-   **Fake underwater turbulence:**
    -   A churning bubble plume glowing white under the impact, fading with depth.
    -   The bed seen through the water wobbling more near it.
    -   A boil ring where the water wells up.
-   **The outflow:** foam streaks and ripples spreading from the impact, fed into the river simulation (plans/waterfalls.md's emitter, radial current and foam rate).

## Open from D181 (next session)

-   **Mist puffs need improvement** (the user): they read as cards cutting through the water and the falls. Turn off the hard depth test for mist and fade it softly against a depth copy that includes the water planes and the falls (C++, FallSurfaceRenderer); then their shape and lighting.
-   **Spray emitter count**: 19,554 striking emitters after the finer bake and the aeration emitters. Raise AERATED_SPRAY, widen the spacing, cap the total; then spray and mist per emitter back up.
-   **Foam stretch along fast water**: scale the pattern's local frequency from the bake's stretch (its third rows), never the run coordinate itself.
-   **The swell normal** (SWELL_HEIGHT 0): its slope from the bake's field, not screen derivatives.
-   **The crest blend** (step 2): the shared parameterization.

## Later (after 1-6, @Victor)

### 7. Denser falls near the player

-   **Disk cache:** cache the trimmed meshes and their bakes to disk, so the build moves off the play session (loading screen or offline).
-   **LOD:** add mesh LOD, with near falls subdivided further.
-   **Depth rays:** still Havok on the render thread at build, about 20 ms a mesh, and move with the build.

### 8. Coverage

-   The small stream under the Whiterun bridge (ref 0003AC4C): make it shallow water or a fall.

### 9. With the GPU particles

Mist and spray (with a rainbow at its angle from the sun), jets detaching from the rock, edges tearing into streamers.

## Decided against

-   **Thickness from continuity:** not wanted.
-   **Caustics:** would barely show.
