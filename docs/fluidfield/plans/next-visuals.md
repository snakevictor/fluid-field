# Next visuals: the falls and the water they join

Oct 21, 2026 · drafted with @Victor after D174 (absorbs the falls-realism plan agreed after D173)

The falls are drawn by the river's own water shader (D159, D171), so they match the water around them. The steps below make that water read as falling water and join it to the river and the pools without a seam. The principle: most of a waterfall's look follows from a few quantities of its flow (speed, aeration, the surface's large shape), so the flow bake produces them and the shading reads them, rather than effects being placed by hand. Each step ends in a build verified in game (screenshots at the same spots, PresentMon for anything that adds work per frame) and a decision record.

## 1. Contact foam per pixel (D174, done)

Foam wherever the sheets touch something, per pixel, in the river's foam pattern on fixed world planes, drifting down the falls; the falls' detail normal on the sheet's own axes. See D174. Left to check on screen: the rim along the lip and the waterline.

## 2. No seam between the river and the falls

At each crest the falls read about 35% darker than the river (measured across the lip) and start with another texture, speed and direction.

1.  **Brightness:** a striped diagnostic (reflection, the water's colour, the weights, the final) on both sides of a lip. The likely sources:

    -   The falls march their own reflection while the river uses the engine's SSR.
    -   The falls tint by the view ray's path behind the sheet, the river by depth.

    Then match the cause, measured, not guessed.

2.  **Retrofit the crest blend (D170, branch feature/fluid-field-crest-blend):**
    -   Merge it. Its code merges cleanly; DECISIONS and LEARNINGS conflict.
    -   What it adds:
        -   The river draws down and speeds up into each lip (a lip map pulling the water solver).
        -   The river's detail and foam drift faster there.
        -   The falls' texture leaves the crest at the river's speed and eases into its own.
    -   Check it with the river-shader falls: it was written against FallSurface's look. Its streak change may no longer apply.
3.  **Blend the crest's rim into the shallow water:**
    -   The sheet's first stretch takes the river's normals, flow direction and foam where the two overlap, fading into the falls' own over the lip.
    -   Built on D141's fade-in under the lip and D170's matched speed.

## 3. Flow bake v2

New fields per texel, beside the bake's flow, wake and foam:

-   **Speed from the drop:** the river's speed at the lip, gaining with the square root of the height fallen. It is also the speed the textures scroll at.
-   **Aeration:** produced by impacts, shear round rocks, converging flow and the height fallen, carried down the flow and fading in calm water. It drives whiteness, opacity and roughness.
-   **Drops far more aerated than runs:** a free-falling drop shades whiter and more opaque than water running down a slope. This comes from the same field (how steep, how far fallen), not from a separate kind of falls.
-   **Surface height:**
    -   The water's large shapes as a height field: piling against a rock, the bulge over a boulder, standing waves, the hump leaving a lip, the rise at the plunge.
    -   The shader takes its normal per pixel from the slope.
    -   The fine ripples stay the water type's normal maps, so the falls match the river, made rougher with speed and aeration and calmer on the tongue. At a few texels per metre the bake is too coarse for ripples.

The bake must still account for contact with the landscape, rocks and other water bodies (as now: it reads the untrimmed triangles, D136). It is cached on disk, so the new fields cost nothing per frame.

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
