# Merging falls that meet into one mesh: feasibility study

Date: 2026-10-26. Brief: Pre-Ship Notes 2026-10-26, section 3, plus Victor's clarification: merge the meeting meshes into one solid mesh, raycast what lies beneath it with the existing ray code, and relax the crease toward the hit surface. Nothing was built or run. Timings come from the 18:51-18:53 run in `CommunityShaders.log`, with 7 sites and 19 meshes, cold bakes and cached captures.

## Recommendation: try the smaller alternative first

Don't merge. Do the same ground-shaped relaxation on the existing per-mesh dense copies, only near each join, and write it into the copies' positions. Most of the visible gain comes from the relaxation, not from having one mesh. Merging breaks four things that the per-mesh pipeline relies on:

-   the bake grid;
-   16-bit indices;
-   the per-draw shader state;
-   the meet-based spray.

|                    | Merge, then relax                                                                                                  | Relax only near the join                 |
| ------------------ | ------------------------------------------------------------------------------------------------------------------ | ---------------------------------------- |
| Work               | 10-15 days                                                                                                         | 2-3 days, including an offline prototype |
| Load cost per site | Remesh, plus a re-bake of every merged grid, plus a re-run of every later stage: about 1-3 s more on workers, cold | About 20-100 ms on a worker              |
| Shader change      | Per-vertex bake choice in 4 vertex paths and the bake reads                                                        | None                                     |
| Risk               | High: overlapping UVs, coarser bakes, a painted-looking crease, lost meet emitters                                 | Low: one more move, like D69 and D94     |

## Earlier attempts, and whether this avoids them

| Attempt                                      | Why it failed or stopped                                                                                                                                                  | Merge proposal                                                                                                                                           | Relax-only                                                                    |
| -------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------- |
| D30/D31 merged surface (FallSurfaceRenderer) | Its UVs came from a top-down flow, not vanilla's. Its own shading "looks shit", "too shallow and slow" (D32 switched it off). D78 deleted it with its input (FallStream). | Partly. It keeps vanilla's UVs away from the crease, but the crease itself needs new UVs and a new flow: the D31 look returns exactly where people look. | Yes. Vanilla's UVs and the current Water.hlsl shading stay everywhere.        |
| D67 overlap fade, D69 gap moves              | Both still run. In this run, 4,523 vertices fade and 4,026 move across a gap, out of 45,337 open-edge vertices (FallSites.cpp:2390).                                      | Replaces them inside merged pairs, but they are still needed against every other mesh.                                                                   | Builds on them: one more term in the same shift.                              |
| D72 mesh-to-mesh flow pull                   | It drew chevrons and a crease (SS 464). Off since D76, deleted in D77.                                                                                                    | Same risk. One grid must blend two flows across the crease.                                                                                              | No flow is blended. Only the shape moves; each bake keeps its own flow (D80). |
| D23-D25 curtains, traced sheets              | Shading invented from scratch looked painted next to vanilla.                                                                                                             | At risk inside the merged band.                                                                                                                          | Not at risk.                                                                  |

## The six questions

**1. Where the joins are.** They are already found per vertex. LinkBakes catches only cascades: here 4 of 19 meshes are fed (FallSites.cpp:4549, LINK_REACH 48 at FallSites.h:828). Several near-joins go unlinked: tops 8, 18, 4 and -1 units from a higher mesh. The reliable sources are these:

-   ComputeFades (FallSites.cpp:2111): overlap, and gap within 40 units.
-   MeetDistance (FallSites.cpp:3279): each vertex's distance to another sheet's shown surface, up to MEET_REACH 64 (FallSites.h:574). This run found 87,985 vertices within 64 units and 4,593 inside another sheet.
-   The meet emitters (D184): 7,094 texels where sheets meet.

**2. The ground.**

-   **Capture: too coarse.** It is 16 units (about 23 cm) a texel (FallCapture.h:19), at most 512 a side per site (FallCapture.cpp:14). The 1024² GPU ground map is the same 16-unit grid over 16,384 units (FluidField.h:350-351). It is a single height per xy from rays straight down, smoothed by a 3x3 median (FallCapture.cpp:155). It skips the falls' own collision, and it cannot see the cliff face behind a curtain or under an overhang. A crease is 20-64 units wide, so the capture gives it 1-4 texels.
-   **Statics' render triangles: accurate.** GatherSolids (FallSites.cpp:3490, within SOLID_REACH 64) and GatherOccluders (FallSites.cpp:5212) read the statics' render triangles, so they are exactly what is drawn.
-   **Landscape: matches what is drawn.** The height cache uses vertices every 128 units (LandscapeCache.h:20), the same grid the game renders.

**3. Reusing the existing ray code.** "Beneath" a curtain means behind it, along -normal, not straight down.

-   **The tool to reuse: FallVisibility's CPU BVH rays** (TrimHidden, FallSites.cpp:5282). They run on workers, against those triangles and the landscape. They cast 13.6 million rays in 4.0 s in this run.
-   **The others don't fit.** Havok rays crash on workers (D172). The capture and BakeRays run on the render thread at 5 ms a frame (FallSites.h:617). BakeRays and RockAlong only answer whether a point is inside rock.
-   **Half of it is already computed.** D104/D106 already bake each vertex's clearance behind the sheet to the first rock (VertexClip d.y).

**4. Building it from the dense copies.**

-   **Shared UVs.** In this run the 19 meshes are 7 copies of FXWaterfallBodyTallInner01 and 12 of SlopeMesh01. Two instances of one model have identical UVs, so one merged mesh has overlapping UVs. Q&A 1b names exactly that as breaking the bake.
-   **Texture scales differ.** Each grid takes its own area-weighted world-per-uv scale (StartBake, FallSites.cpp:3388), and D80 (FallSites.cpp:4719) keeps each mesh's own scale because fitting a feeder's scale mirrored, stretched or turned the streaks. One grid over two meshes needs a new parametrisation, which is no longer vanilla's UVs.
-   **Coarser bakes.** One grid would also hit the BAKE_MOST_SIDE 256 cap (FallSites.h:615): coarser than the current 8 units a texel.
-   **16-bit indices.** Dense copies run from 9k to 45k vertices, against the 65,535 limit (FallSites.cpp:1794, 5386). Two copies merged often exceed it.

**5. Rendering.**

-   **Depth passes.** A move stored in the per-vertex shift (ComposeShifts, FallSites.cpp:4003), or written into the copy's positions, reaches all four vertex paths identically: Lighting.hlsl:160, Utility.hlsl:133, Water.hlsl:224, FallSurface.hlsl:97. Both depth passes stay in agreement under either option.
-   **Per-draw state.** It is one VertexBase, one BakeRect and one bake per draw (FallGround.hlsli:10-31, FluidField.cpp:1718-1733). A merged mesh needs a per-vertex bake choice in every path that reads the bake.
-   **Pool cut.** It is per vertex (the foot flag, a.w), so it ports to either option, but "foot" must be redefined for a merged mesh.
-   **Seam fade.** It still applies against the other meshes.
-   **Spray and plunges.** The green meet emitters need two sheets to meet. ComputePlunges and LinkBakes work per grid. The merge needs variants of all of these. Relax-only needs none.

**6. Cost.**

-   **Current load cost.** The run settled in 89.3 s:
    -   dense copies: 50.1 s on a worker (outer shell trim 4.0 s);
    -   solves: 15.5 s for 19 meshes (about 0.8 s each);
    -   culling: 10.1 s over 3 runs;
    -   bake rays: 9.6 s on the render thread;
    -   seams: 4.2 s;
    -   linking: 3.5 s.
-   **Relax-only adds little.** It adds a few rays per band vertex (roughly 10k-50k rays per join, at about 3.4 M/s) plus a harmonic blend: tens of ms on the dense worker.
-   **Caching.** Bakes are cached on disk; dense copies are rebuilt on every load (D185). Moving vertices changes the bake cache key once (bump BAKE_CACHE_VERSION, FallSites.h:749). Ideally the moved copies would themselves be cached on disk.
-   **The merge costs more.** It must also re-bake and re-run culling, seams and linking for every merged pair.

## How the relax-only option fits the pipeline

**Where it runs.** In ComputeDense, on the dense worker, after TrimHidden. The vertices to move are those within about 64 units of another sheet, using the seams' and MeetDistance's rules.

**What it does.**

1. Rays along -normal (and down) through FallVisibility's BVH find the surface behind.
2. Both sheets' band vertices are drawn toward one common surface, a fixed clearance off what the rays hit.
3. The move fades over EDGE_BLEND_REACH 120 with D94's harmonic blend, and is capped like MOST_SHIFT.
4. The moves are written into the copies' positions (Q&A item 2), so the bake, culling and every pass see them.

**Risk.** Water pulled into rock gets cut by burial, exposing a hole. To guard against it, clamp the move to the baked clearance.

## Evidence plan

**Poses.** Save FluidField_FallsRock, at 13:30 in clear weather. Mesh positions come from the log's "fall N: in at / out at" lines. Whether each view really shows a join still needs checking in the screenshots.

| Pose                  | Join it likely shows                                                                                                  |
| --------------------- | --------------------------------------------------------------------------------------------------------------------- |
| CAM 2 (30189, -26832) | Fall 1's foot (30095, -26826) and fall 5's top (30111, -26500): a pool join                                           |
| CAM 5 "contact"       | The feet of falls 0, 2 and 3 near (31000-31770, -27900)                                                               |
| CAM 11, 12, 14        | Cascade edges; hard seams there before (SS 1816, 1881)                                                                |
| CAM 13, CAM 7         | Fall 11 (8 units from the mesh above, not linked); tops 12, 15 and 16 meeting side by side near (29000-30200, -32700) |

**Other site.** Site #4, cell (8, -5): fed meshes 17 and 18 in at (35443, -19352) and (36179, -19291). No pose exists for it yet; record one.

**Offline prototype, before any game round.**

-   **Input.** The dumps are grids of 24-byte header + 16 bytes a texel: covered, rock, world position as 3 floats, collision rock, render rock (FallSites.cpp:3705-3716). Triangulate the covered texels into an 8-unit world mesh per dump. Match dumps to meshes by world position.
-   **Joins.** Find them with ComputeFades' rules.
-   **Ground.** Use `FluidField_rock_site*.bin` (FallCapture::Dump format) for now. A small dump of GatherSolids' triangles would give the render-accurate ground; that is a code change, so it waits for approval.
-   **Compare relax-only against merge-and-relax on the same joins:**
    -   crease angle across the join;
    -   the gap or overlap left;
    -   clearance to the ground;
    -   a flatmap.py-style shaded render.
