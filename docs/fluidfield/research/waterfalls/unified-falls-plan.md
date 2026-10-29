# Unified falls: one surface per site — plan

Date: 2026-10-26. Goal (Victor): merge the falls meshes that meet into one solid surface per site, then raycast what lies behind
it, as the pipeline already does, and relax the creases at the joins toward it, so they look natural. This replaces stitching
separate meshes together. Background: `mesh-merge-study.md` (study 3). This plan takes the merge route and keeps the study's
blockers in view.

## What changes, in one sentence

Today each vanilla falls mesh gets its own dense copy, bake grid, draw and cross-mesh stitching. Instead, each site gets **one
surface we build**, with **one flow-aligned bake grid**, drawn by **our own pass**. Vanilla's meshes only seed the shape.

## What we already have

| Piece                                                                                  | Where                                              | Use in the unified surface                                                           |
| -------------------------------------------------------------------------------------- | -------------------------------------------------- | ------------------------------------------------------------------------------------ |
| Sites (groups of falls refs, planes, levels)                                           | `FallSites::Site`                                  | The unit of merging: one surface per site.                                           |
| Dense copies (vanilla meshes split to ~8 units, smoothed by bend)                      | `ComputeDense`, D186-D190                          | The merge's input geometry.                                                          |
| Hidden-part removal: lead-ins, buried, inside other sheets                             | `TrimHidden`, culling, D95, D126, D173             | Decides which parts of overlapping sheets survive the merge.                         |
| CPU BVH rays on workers (statics' render triangles, landscape)                         | `FallVisibility`, `GatherSolids`/`GatherOccluders` | The raycast behind the crease: 13.6 M rays in 4 s.                                   |
| Clearance behind each vertex                                                           | D104/D106 (`VertexClip d.y`)                       | Clamps the relaxation so water isn't pulled into rock.                               |
| Join detection                                                                         | `ComputeFades`, `MeetDistance`, meet emitters      | Finds where sheets meet and must be merged.                                          |
| Strand march on a texel grid (gravity, friction, rock steering, aeration, speed, drop) | `FallStrands` (D80, D192)                          | Runs unchanged on the new grid: it only needs positions, covered and rock per texel. |
| Flow coordinates the shaders texture with (across/along), not vanilla's UVs            | `FlowSample::uvA`, `LinkBakes`                     | The rendering already ignores vanilla's UVs, so we only need our own grid.           |
| Our own falls draw (dense copies, Water.hlsl's falls variant)                          | `FallRefraction`, D91, D159                        | Draws the unified surface. One bake per draw is already the model.                   |
| Offline harness, frozen bake dumps, flatmap/bakestats                                  | `tools/harness`, `flatmap.py`, `bakestats.py`      | The offline prototype and the A/B numbers.                                           |

## What has to be built

1. **Merge** (per site): take the trimmed dense copies, cut each sheet along where it meets another (the intersection curves),
   drop the hidden parts, and weld the boundaries into one surface. Then remesh evenly (about 8 units) so the join has no sliver
   triangles.
2. **Crease relaxation**: in a band around each weld, cast rays along -normal (and down) through the BVH, and draw the band toward
   a surface a fixed clearance off what is hit. Fade it out over the band and clamp it by the baked clearance. Then fair the
   result (the bend redistribution we already have).
3. **Site parameterisation**: the crux. Build u (across) and v (down the flow) over the merged surface:

    - gravity projected on the surface gives a downhill field;
    - v is the travel down it, from the crests;
    - u labels the streamlines by where they leave the crests (several crests are concatenated).

    Rasterise to a texel grid the strands can march. Branching around rock and two crests feeding one sheet are the hard cases;
    the grid's covered mask and rock texels already express both.

4. **One bake per site**: grid size per site (today capped at 256 a side, which would be too coarse for a site), the cache key
   per site, plunges, progress, swell and disturbance as today but over one grid. LinkBakes stays only between sites and
   across pools.
5. **Draw**: one mesh per site (or a few chunks), 32-bit indices, `FallRefraction` binding the site's bake. The vertex shader keeps
   reading the per-vertex `FallVertex` data (cut, wobble, clearance), so the shaders change little.
6. **Spray**: meet emitters need two sheets, so they go. Emit instead where the strands converge or squeeze over the weld,
   which the bake already measures.

## What carries on, what breaks, what retires

| Aspect                                                                          | Effect                                                                                                                                                                              |
| ------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Water.hlsl shading (ribbons, rapids, foam, relief, bubbles, SSR, crest, colour) | **Carries on.** It reads the bake per pixel; the bake is now per site.                                                                                                              |
| Strands bake (speed, aeration, drop, progress, swell)                           | **Carries on**, on a site grid; cross-mesh carry becomes plain continuity.                                                                                                          |
| Wobble and swell vertex moves, rock contact ramps                               | **Carry on** (per vertex on our mesh).                                                                                                                                              |
| Burial and pool cuts, waterline foam                                            | **Carry on** (per vertex); "foot" redefined per site.                                                                                                                               |
| Plunges, landing foam, pool foam                                                | **Carry on** on the site grid.                                                                                                                                                      |
| Seam fade (D67), seam moves (D69), meet foam and sheet gaps (D128, D174)        | **Retire inside a site.** Kept only where a site still meets an unmerged mesh.                                                                                                      |
| Inner-side hiding (D100/D103), front-surface pre-pass (D125)                    | Mostly **retire** (one surface has no overlapping layers); kept as a guard for folds.                                                                                               |
| LinkBakes feeders, pool links (D191)                                            | **Shrink** to links between sites and across pools.                                                                                                                                 |
| Meet emitters (D184)                                                            | **Break**; replaced by convergence emitters (built item 6).                                                                                                                         |
| Engine-drawn lit falls (`RefractiveFalls` off, the code's default)              | **Break**: the engine can only draw vanilla's meshes. Our pass must become the only falls path, or the lit path keeps today's per-mesh pipeline as a fallback. **Decision needed.** |
| Thin falls, creeks, rapids effects (study 4)                                    | **Unaffected.** Separate work.                                                                                                                                                      |
| Bake caches                                                                     | Re-keyed per site, so every site rebakes once.                                                                                                                                      |
| Load time                                                                       | Merge, remesh and parameterisation are new work, measured in phase 1. They replace the seams, part of the culling and the cross-mesh linking.                                       |

## How we build it: phases with gates

**Phase 0, a data dump (one short game round).** Add a debug-only dump of each site's trimmed dense copies (positions, normals,
indices, per-vertex clearance) and the statics' triangles near it. Run once at White River. No shader changes, so the run is quick:
no shader rebuild, and the bakes are already cached.

**Phase 1, offline merge and relax (harness, no game).** Merge, weld and remesh one White River site, relax the creases against the
dumped triangles, and plot the result with flatmap-style shading next to today's meshes. **Gate A:** the joins read smooth and follow
the rock, with no folds or holes.

**Phase 2, offline parameterisation and bake (harness).** Build the site grid, run `FallStrands` on it in the harness, and plot
speed, progress and aeration across the joins. **Gate B:** the grid covers the site without folds, and the strands flow through the
joins with no restart.

**Phase 3, in game behind a toggle.** Add "Unified falls (experimental)", per site, so in one session we can switch between today's
per-mesh path and the unified surface without a restart. C++ only; no shader edits unless a shader must read something new.
Test only the join poses (CAM 2, 5, 7, 11-14, plus a pose to record at site #4), A/B in the same session, at one time and weather.
No other camera rounds, no unrelated shader compiles. **Gate C:** better joins, no regressions at those poses, load time measured.

**Phase 4, cleanup.** With the toggle on by default, retire the per-mesh stitching inside sites, and decide the lit path's fate.

Rough size: Phase 0 half a day; Phase 1 2-3 days; Phase 2 3-5 days (the hard one); Phase 3 4-6 days; Phase 4 2-3 days.
Phases 1 and 2 are the spike: if Gate A or B fails, we stop with a clear reason and fall back to relax-only (study 3).

## Risks

-   **Parameterisation** over branching, multi-crest sites (Phase 2) is the main unknown.
-   **Look at the welds**: D31 looked "shallow and slow", but it used its own shading and a top-down flow; we keep Water.hlsl and the
    strands, so the risk is lower but not zero.
-   **Load time**: dense copies are already 50 s on a worker; merging and remeshing must not add much.
-   **The lit path**: shipping with our pass as the only falls path is a product decision.

## Decisions for Victor

1. Should our pass become the only falls path, or does the engine-lit path stay as a per-mesh fallback?
2. Which sites go first: White River only, or White River plus site #4 (cell 8, -5)?
3. Is Phase 0's debug dump (a small C++ change, off by default) OK?

## Decisions (Victor, 2026-10-26)

1. See the results first. If the unified surface isn't worth it, return to the per-mesh approach; branch
   `fluid-field/per-mesh-falls` marks it.
2. All the falls on the White River from Riverwood to Whiterun (every site in FluidField_FallsRock).
3. The Phase 0 debug dump is approved.

Thin falls and other falling-water effects are not merged; they get their own solution (water-inventory.md).

## Agreed spec (Victor, 2026-10-26)

-   **Shape:** mostly near the joins. A weighted pass over the rest of each sheet moves it toward the same target with a bounded
    deviation from vanilla's shape (tuned in Phase 1).
-   **The crease shape is mixed by steepness:**

    -   where the water slides down rock, it follows the rock at a small clearance;
    -   where it falls freely, it bridges smoothly and ignores small rock detail.

    The bake's steepness sets the blend at each point.

-   **Rock that pokes through:** the surface lets it through. The water bulges where the flow meets the rock (upstream) and foams
    where the flow resumes behind it (downstream); the flow direction decides which side is which. This matches the bake's existing
    pile-up swell and wake foam.
-   **Pools:** falls on either side of a pool stay separate surfaces. They link dynamically through the pool: a fall's crest takes
    its foam from the pool's own live (simulated) foam where it pours out, not from the falls that fed the pool. This replaces
    D191's bake-time pool link.
-   **Load time:** under 30 s extra per site on a cold visit; cached visits near zero.

## Decision (Victor, 2026-10-26, later): ship precomputed

Assume the unified falls meshes, and the bakes, are precomputed and shipped as caches with the mod (whether that fits Community
Shaders' culture is still open). So build time doesn't constrain the method: use the best-quality pipeline.

Method chosen after the offline trials (tools/unify):

-   **Stitching the existing triangles failed:** nearest-vertex welding, chain zipping, the exact margin-free cut. The cut edges
    have different spacing, so 40-90% of the seams stayed open.
-   **An averaging implicit fit kept the valleys between domes:** the field is an average of the sheets' tangent planes.
-   **Voxel remesh works (ZBrush DynaMesh / Blender Voxel Remesh):**

    -   thin shells round the culled dense copies in a distance volume, then their union;
    -   a morphological closing (grow by R, shrink by R) rounds the concave creases;
    -   one surface extracted, front face kept.

    Exact (sub-voxel) distances are needed; voxel-quantised distance transforms terrace the result. Still to do:

    -   smoothing and isotropic remeshing to runtime density;
    -   the rock raycast along smoothed normals.

## Shipping constraints (the CS team lead, 2026-10-26)

-   Shipping baked data is fine: Terrain Shadows ships heightmaps and Unified Water ships its water mesh NIF.
-   Shipped meshes must be low in file size: 30-50 MB of meshes would need another solution.
-   Pre-bake as much as possible; performance is paramount.
-   Victor: compression comes later. Keep it in mind for the format: ship only what can't be recomputed cheaply. For example,
    ship the corefined and faired seam bands and the dropped-patch list per site, and rebuild the rest from the dense copies
    at load.

## Engine integration (branch fluid-field/unified-falls, 2026-10-27)

The offline sheet is built: per site, one continuous surface from the dense sheets and the world in the game files (tools in
C:/dev/cs-deploy/tools/unify: arrange, visibility, Poisson, remesh; NOTES.md). Rock never cuts it: faces inside rock carry a
flag the bake ignores. The game now has to load it in place of the per-mesh dense copies.

### What the game does today, per vanilla mesh (the parts a unified site replaces)

-   **Dense copy:** `ReadDense` → `ComputeDense` (densify, shape, the D172 land trim) → `TrimHidden` (D173) → `AttachDense`
    (an engine BSTriShape under the original's parent, the original app-culled, the material cloned).
-   **Culling and seams:** `ComputeFades` (a.y fade, gapShifts), `BuryStep` rock, `CullStep` stages 2-5 (inside, island,
    side flags, meet distance), the lead-in cut. All exist because sheets overlapped; a unified site needs none of them.
-   **Bake:** per mesh, rasterised in the mesh's own UV space (`StartBake`), then `LinkBakes` chains meshes (feeders, the
    pool link). The disk cache keys on the mesh's ref, name, transform, grid and the solids.

### The plan

1. **The site cache file** (versioned; small, compressible): the sheet's vertices (positions quantised in the site box),
   16-bit triangles (a site over 65535 vertices is split in chunks), a UV per vertex (below), the per-face flags (inside
   rock), the precomputed per-vertex fields that don't depend on the run (d.y "behind"), and the identity of the site
   (its member refs' form IDs and placements). Later: the baked flow fields too (the offline bake).
2. **UVs:** the bake rasterises in UV space and vanilla's textures scroll along v, so the sheet gets one continuous,
   flow-aligned unwrap: v along the downhill direction (gravity projected on the surface), u across it, solved as two
   least-squares fields on the sheet (one chart: the sheet is a disc). The bake grid is this UV box; `BAKE_MOST_SIDE`
   (256) rises for sites.
3. **One engine shape per site,** built as the dense copies are (`BuildDenseShape`), under the first member's parent;
   every Body member's lit shapes app-culled and never densified; the first member's lit material cloned (both vanilla
   sources share it: FXwaterTile01 / 02_n). Every consumer keyed by `BSGeometry*` keeps working (vertex clip, bake,
   refraction, the lit and depth paths).
4. **Hook:** `RefreshGrounded`'s site loop: a complete site with a cache loads it synchronously and registers the one
   shape in `grounded`, `litMeshList`, `clipQueue`, `bakeQueue`; its members are recorded for `RemoveDense`,
   `ApplyLitHidden` and the unload sweep.
5. **Skipped for a unified site:** ReadDense/ComputeDense/TrimHidden/AttachDense, ComputeFades (a.y = 1, no gap shifts),
   BuryStep's rock, CullStep stages 2-5, the lead-in cut, LinkBakes inside the site.
6. **Kept, made per site:** ClipStep (the water cut, a.z, d.z, the foot bit: its "this mesh's bottom" and "upper half"
   heuristics become the site's), PoolCut and the submerged cut, the bake (one grid per site) with the pool link,
   plunges, wobble and swell.
7. **A/B:** a setting (Unified Falls: on/off) so a site draws either way in the same build.

### Order

1. Offline: the flow-aligned UVs and the cache writer (site A); check the UVs in Blender (a checker texture).
2. Engine: the cache reader and the site shape, everything after it untouched; in game the bake runs on it as it is.
   Gate: site A draws as one sheet, no seams, the bake's flow and foam plausible.
3. Engine: the skips and the per-site ClipStep; the inside-rock flag read by the bake.
4. Offline bake, compared with the in-game bake on the same sheet; shipped when at least as good.
