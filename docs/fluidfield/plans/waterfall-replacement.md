# Fluid Field: Waterfall Replacement Design

Oct 15, 2026 · @Victor

## Overview

Replace every vanilla Skyrim waterfall with a physically derived one: the waterfall is reconstructed from the vanilla water layout, its fall is traced over real geometry, and it feeds the existing Fluid Field shallow-water sim (SWE) at the bottom. It ships as part of the Fluid Field Community Shaders feature.

**Goals**

-   Waterfalls exist at steady state from the first frame. Nothing fills up over time.
-   Lip position, width, direction and fall path come from the world (vanilla planes, meshes, terrain, statics), not from the authored arch meshes.
-   Falling water accelerates, thins and aerates according to simple physics (gravity, mass conservation).
-   The impact zone drives SWE foam, Fluid Field mist and spray particles, scaled by flow and impact speed.
-   Works on vanilla and on mod-added waterfalls that reuse vanilla assets, with a JSON override for everything else.

**Non-goals**

-   No runtime 3D fluid sim (FLIP/SPH) of the falling water.
-   The SWE never simulates the free fall. It only runs in pools and river stretches.
-   No new authored meshes required. Asset hooks for modders (vertex colors, flow maps) come later.
-   No VR support in the first version; keep code stereo-safe where it is free.

**Guiding principles for the agent**

1. The vanilla layout is ground truth for _where_ water may exist. The sim refines; it never invents water outside it.
2. Steady state is the default. Live simulation is a refinement near the player.
3. Static geometry, moving shading: compute fall paths once, animate only textures and particles.
4. Every stage gets a debug view before the next stage starts.
5. Follow upstream Community Shaders conventions for feature layout, settings and shader registration.

## Background: vanilla water

Vanilla Skyrim has no flowing water volume, so waterfalls cannot be discovered by simulation alone. They must be reconstructed from three kinds of hand-placed data.

| Element                   | What it is in vanilla                                                                          | What we take from it                              |
| ------------------------- | ---------------------------------------------------------------------------------------------- | ------------------------------------------------- |
| Water planes              | Flat surfaces at a fixed height per placement (cell water and placed water objects)            | Target water levels; allowed water region         |
| River staircases          | A descending river is a series of planes at decreasing heights                                 | Every height step is a candidate drop             |
| Waterfall / rapids meshes | Hand-placed NIFs with BSEffectShaderProperty and scrolling UVs, hiding the seams between steps | Markers for where drops are, and roughly how wide |
| Mist / splash FX          | Particle and effect meshes near the base                                                       | Confirms the impact zone; hints at size           |
| Water type current        | Water types appear to carry a current/velocity that pushes actors                              | Flow direction hint (verify)                      |

**Implications**

-   Bethesda already marked every drop: the mesh clumps are the detection signal.
-   Planes are often oversized and buried under terrain, so plane edges alone are unreliable for lip position.
-   There is no discharge value anywhere. Flow rate is a heuristic plus per-site overrides.
-   Free SWE on raw Skyrim geometry wanders and climbs rocks. The design confines it to vanilla water regions and never uses it for free fall.

## Architecture

The pipeline splits into a one-time site build and a small per-frame loop; S10 (vanilla suppression) applies at draw time to every replaced site.

Build (once per site): S1 inventory → S2 clustering → S3 level pairing and tiers → S4 ground capture → S5 lips and flow → S6 fall tracing. Per frame: S7 sheets, S8 impact coupling, S9 confined SWE, S10 vanilla suppression.

The build snakes from inventory to fall tracing; its fall paths become sheets, its impact points drive coupling, and its lips become SWE sinks.

## Stage specification

Ten stages, each with defined inputs and outputs. All distances are in Skyrim units (1 unit is about 1.43 cm); every threshold below is a starting default exposed in settings.

### S1. Inventory

On cell attach, enumerate references in the cell.

-   **Water:** cell water height plus placed water references, with world-space extents and height.
-   **Waterfall FX:** effect meshes classified by source texture path against a configurable list (start with the vanilla `fxwaterfall*`, `fxwaterwall*`, rapids and mist textures).
-   **Output:** per-cell list of water planes and classified FX references with world bounds.

### S2. Site clustering

Group classified FX references into sites with union-find: two references join if their bounds are within `clusterRadius` (default 512).

-   **Output per site:** member refs, 2D footprint (convex hull of projected bounds), `zMin`, `zMax`, centroid.

### S3. Level pairing and tiers

Find water planes whose XY extents overlap the footprint expanded by `pairMargin` (default 256).

-   **Top level:** highest plane near `zMax`. **Bottom level:** lowest plane near `zMin`.
-   **Tiers:** more than two distinct plane heights in a site means a cascade; split it into ordered tiers, one lip per step.
-   **Classification:** fall height `H = top − bottom`. Below `rapidsThreshold` (default 64) the site is rapids: no sheet, SWE plus foam only.
-   **No bottom plane:** the fall ends on ground; terminate at terrain and emit mist only.

### S4. Ground capture

Render a top-down orthographic height capture of terrain and statics for each site (footprint plus margin) and for each SWE region.

-   Exclude actors, effect meshes, water and foliage.
-   **Bridge rule:** ignore geometry above `topLevel + captureCeiling` (default 128) so bridges and arches do not become dams.
-   Clean up: fill holes, then a median filter to remove spikes.
-   **Output:** R32F heightfield, resolution `captureTexel` (default 16 units per texel).

### S5. Lip extraction and flow estimate

1. Extract the iso-contour at `topLevel` on the captured heightfield inside the footprint (marching squares).
2. Keep segments whose downhill side drops more than `minDrop` (default 96) within `dropProbe` (default 64).
3. Merge segments with gaps under `gapMerge`; discard segments shorter than `minLip`.
4. Fit a smoothing spline per segment and resample at spacing `ds` (default 16).
5. Outward direction per sample: negated smoothed heightfield gradient, cross-checked against the top-to-bottom centroid vector.
6. **Fallback:** if no valid contour, use the top edge of the mesh cluster.

**Flow estimate.** Unit discharge `q` (flow per unit lip width) comes from a size-class table keyed by the vanilla mesh variant, or from a site override. A free overfall passes through critical depth, so lip depth and exit speed follow directly:

```latex
h_c = \left(\frac{q^2}{g}\right)^{1/3}, \qquad v_0 = \sqrt{g\, h_c}
```

-   **Output per sample:** position, tangent, outward normal, `q`, `v0`.

### S6. Fall tracing

For each lip sample, integrate a ballistic path from position `p0` with velocity `v0` along the outward normal, under gravity, against the site heightfield.

-   **Hit near bottom level (within `levelTol`):** impact into the pool.
-   **Hit on a shallow ledge** (slope below `clingAngle`, default 50°): re-entry. The ledge becomes an SWE source and is searched for a new lip (recursion depth limit 4). This produces cascades and splits.
-   **Hit on a steep face:** clinging flow. Project velocity onto the surface, apply friction, keep sliding.
-   **Output per sample:** polyline with arc length `s`, speed `v(s)`, travel time `t(s)`, end type and impact point.

This runs once per site on load, and again only if the site inputs change.

### S7. Sheet geometry and shading

Build quad strips between neighboring streamlines. Split a strip where neighbors diverge by more than `splitDistance`, so curtains break apart naturally.

**Vertex attributes:** `s`, `v`, thickness `τ = q / v` (mass conservation per unit width), aeration `α` from a configurable curve of fall distance and `q`, edge falloff.

**Shading:**

-   Two-phase scrolling along `s` at the local speed `v`, so streaks accelerate correctly.
-   Layers for body, streaks and foam; opacity from `τ`, whiteness from `α`.
-   Sun forward scattering when backlit, ambient from existing CS lighting, Fresnel-weighted reflections.
-   Depth fade against scene depth, plus contact foam at intersections.
-   Write the upscaler reactive mask so DLSS, FSR and TAA do not smear the sheet.

The sheet is a custom translucent draw owned by the feature, not a BSEffectShader mesh.

### S8. Impact coupling

Per impact point, with impact speed `v_i`:

-   **Mass:** add `q · ds` to the SWE at the impact cell; remove the same amount at the lip cells, so region levels stay at target.
-   **Momentum:** inject the horizontal component of `v_i` with an efficiency factor.
-   **Foam:** emission rate proportional to `q · v_i²` into SWE foam advection.
-   **Mist:** Fluid Field fog density injection, same scaling, with upward initial velocity.
-   **Spray:** GPU particle bursts under a global budget.

### S9. SWE confinement and steady state

The existing SWE runs only inside an allowed mask: vanilla plane footprints, limited to ground below the plane level plus a margin. Cells outside are walls.

-   **Target levels:** relax each region softly toward its vanilla plane height.
-   **Numerics to verify or add:** hydrostatic reconstruction (well-balanced, so still water stays still on slopes), Manning bed friction, velocity clamp, CFL timestep, wet/dry threshold.
-   **Initial state:** load the baked state if present; otherwise fill the mask to vanilla levels (priority-flood), route flow from sources to lip sinks, then run hidden substeps during cell load.

### S10. Vanilla suppression

At each successfully replaced site, skip the draws of its classified waterfall and rapids meshes. Vanilla mist is a separate toggle. Sites that fail any stage stay fully vanilla. Ambient sound markers are left as they are.

## Data, caching and streaming

Each site is computed once and cached to disk; only the live SWE and shading run per frame.

**Site cache.** One file per worldspace cell, keyed by a hash of the inputs: member ref form IDs and transforms, plane heights, and the capture settings. Any input change invalidates the cache and the site is rebuilt on next attach. Contents: tiers, lip splines with per-sample `q` and `v0`, streamline polylines, strip meshes, impact points.

**SWE state cache.** Optional baked converged state per SWE region (height and velocity, 16-bit). Loaded on attach when present; otherwise the analytic initializer from S9 runs.

**Streaming tiers:**

| Distance            | Sheet                                       | SWE                  | Impact FX              |
| ------------------- | ------------------------------------------- | -------------------- | ---------------------- |
| Loaded grid, near   | Cached strips, full shading                 | Live                 | Live foam, mist, spray |
| Loaded grid, far    | Cached strips, reduced shading              | Frozen at last state | Mist only              |
| Outside loaded grid | Not drawn (distant LOD out of scope for v1) | None                 | None                   |

**Per-site override JSON.** Lives in the feature's data folder, matched by worldspace plus cell plus the form ID of any member reference. Every field is optional and overrides the computed value.

```json
{
    "sites": [
        {
            "match": { "worldspace": "Tamriel", "cell": [5, -10], "memberFormId": "0x000ABCDE" },
            "disable": false,
            "q": 0.35,
            "topLevel": 1840.0,
            "bottomLevel": 1210.0,
            "lipFallback": "meshEdge",
            "keepVanillaMist": true
        }
    ],
    "classification": {
        "waterfallTextures": ["textures/effects/fxwaterfall*", "textures/effects/fxwaterwall*"],
        "sizeClassQ": { "small": 0.1, "medium": 0.3, "large": 0.8 }
    }
}
```

The `cell` coordinates, form ID, levels and `q` values above are placeholders. Units for `q` follow the project's SWE units; pick one convention and document it in code.

## Debug tooling

Every stage ships with an in-game overlay, toggled from the feature's ImGui panel, before the next stage is built.

| Stage         | Overlay                                                                                         |
| ------------- | ----------------------------------------------------------------------------------------------- |
| S1 Inventory  | Classified FX refs outlined by class; water planes drawn as translucent quads with their height |
| S2 Clustering | Site footprints with ID, member count and z range                                               |
| S3 Pairing    | Top and bottom plane per site and tier; rapids sites tinted; sites with no bottom flagged       |
| S4 Capture    | Heightfield as a colored grid; cells removed by the bridge rule highlighted                     |
| S5 Lips       | Lip splines with outward normals; fallback lips drawn in a different color                      |
| S6 Tracing    | Streamlines colored by speed; end type markers (pool, ledge, cling)                             |
| S7 Sheets     | Wireframe strips; attribute views for `τ`, `α`, `v`                                             |
| S8 Impacts    | Impact points sized by `q · v_i²`                                                               |
| S9 SWE        | Allowed mask, target levels, velocity field, mass in vs. out per region                         |

**Also required:**

-   A site inspector: pick a site, list every computed value, and export a starter override JSON entry.
-   A per-site toggle between vanilla and replaced, for side-by-side comparison.
-   A console summary per cell: sites found, replaced, rapids, failed (with the failing stage).
-   Per-stage GPU and CPU timings through the existing CS profiling system.

## Milestones

Six milestones, in order; each one must pass its acceptance check on the test sites before the next begins. Test sites: the Riverwood falls, the Whiterun river cascades, and one large falls of your choice.

1. **M1: Detection (S1 to S3).** All three test sites are found, clustered and paired correctly; cascades split into the right number of tiers; rapids are classified as rapids. Per-cell console summary works.
2. **M2: Capture and lips (S4, S5).** Lips trace the visible rock edge within one capture texel at all test sites; bridges over the Whiterun river do not appear in the capture; fallback triggers only where the contour is genuinely unusable.
3. **M3: Fall paths (S6).** Streamlines land in the pool or on the expected ledges; no path passes through rock; cascades recurse to the correct depth. Site build time under 50 ms per site.
4. **M4: Sheets (S7, S10).** Replaced waterfalls render with accelerating flow, thinning and aeration; vanilla meshes are hidden at replaced sites only; no visible smearing with upscaling on.
5. **M5: Coupling (S8, S9).** Pool levels stay within `levelTol` of the vanilla height over 10 minutes of play; foam and mist scale with the waterfall's size; no water outside the allowed mask.
6. **M6: Caching and robustness.** Second visit loads from cache with no rebuild; a whole-worldspace sweep (coc through every exterior cell with sites) finishes with no crashes, and every failed site falls back to vanilla.

**Performance budget (initial targets):** sheets plus impact FX under 1.0 ms GPU at 1440p with three sites on screen; SWE cost unchanged from today plus mass coupling.

## Risks and open questions

The biggest risks are noisy lip detection and inconsistent vanilla planes; both are contained by fallbacks to vanilla.

**Verify before coding**

-   [ ] How existing CS features inject custom draws and compute passes, and where a translucent pass can be inserted relative to Skyrim's effect and water passes.
-   [ ] How to enumerate placed water references and their extents through CommonLib, and how cell water height interacts with them.
-   [ ] Whether water types expose a usable current direction or velocity, and whether it is set on river planes.
-   [ ] Which hook gives per-reference draw suppression for effect meshes (cull flag, scenegraph hide, or shader-side discard).
-   [ ] The exact vanilla texture paths and mesh variants for waterfalls, rapids and mist, for the classification list and size-class table.
-   [ ] Whether the existing SWE is well-balanced; if not, add hydrostatic reconstruction before M5.

**Risks**

| Risk                                                  | Effect                          | Mitigation                                              |
| ----------------------------------------------------- | ------------------------------- | ------------------------------------------------------- |
| Noisy rock statics fragment the lip contour           | Many tiny lips, broken curtains | Median filter, `gapMerge`, `minLip`, mesh-edge fallback |
| Vanilla planes overlap or sit at inconsistent heights | Wrong top/bottom pairing        | Pairing overlay, `levelTol`, per-site level override    |
| Bridge rule removes real ledges                       | Falls pass through geometry     | Per-site `captureCeiling` override                      |
| Heuristic `q` looks wrong for some sites              | Sheets too thin or too heavy    | Size-class tuning, per-site `q` override                |
| Overhangs and caves under the lip                     | Top-down capture misses them    | Accept for v1; side captures later                      |
| Interior waterfalls (caves, Blackreach)               | Different water setup           | Same pipeline on interior cells; test after M6          |
| Mods replacing vanilla waterfall meshes               | Classification misses them      | Texture-path list in JSON; documented for mod authors   |

**Open questions**

-   Should rapids also get a thin sheet or foam ribbon, or is SWE plus foam enough? This could come later once the primary falls are well stablished.
-   Distant LOD for waterfalls beyond the loaded grid: impostor sheets, or keep vanilla LOD? No LODs for now, that comes later.
-   Is a mod-author asset contract (vertex colors, flow maps) wanted in v1 or deferred? Yeah we could expose those parameters.

## Adjustments adopted with D25 (2026-10-15)

Agreed with @Victor when this design replaced D24 (simulated water down the rock).

-   **S1 classification** uses model paths (what the existing scans read), not texture paths; the JSON list will take both.
-   **S4 capture** uses downward Havok rays on a 16-unit lattice (the existing `FallBed`), not a render capture: they skip actors, effects and water already, and the bridge rule is the rays' start height (`topLevel + captureCeiling`).
-   **S6 tracing** starts from the D23 per-column trace (slope sliding, wall turning) in git history (commit 1cf94e99f).
-   **S7 shading** adds refraction of the scene behind the sheet (a scene copy, as `SimSurface` did); its absence is what made D23's curtains look painted. The upscaler reactive mask is checked before M4.
-   **S8 spray particles** are out of scope until the effects phase; mist into the fog field is a later extra (that field is a low ground layer).
-   **S9 well-balancedness** is proven with a still-pool test in M5, not assumed.
-   **M6 caching** stays last and may be optional; the rays over the White River falls took a few seconds.
-   **Removed with D25:** the D24 lip sources, the fall bed overriding the solver's bed, and the `SimSurface` pass. Vanilla falls stay visible until M4 replaces them.
-   **Debug overlays** are drawn in the world through ImGui's background draw list, projected with the player's NiCamera.

## D26: grounding instead of replacement

S7 (custom sheets) and S10 (hiding vanilla) were dropped: the traced sheets looked painted next to vanilla's authored falls. Vanilla's falling-water meshes stay and are grounded on S4's captured rock in CS's effect shader: vertices bent onto the rock, the texture flowed around rock in the way, and the water softened into the rock with foam at the contact. S5 (lips) and S6 (tracing) were removed with the sheets. Open items:

-   Blending where two falling-water meshes meet (the upper one pouring into the lower one, not a hard seam).
-   A surface simulation in each mesh's texture space driving its flow and vertex displacement.
-   S8 coupling from where each fall really lands.
-   Vanilla's waterfall mist made volumetric through the ground fog (D21).
