# Strand sheet: offline prototype on real mesh dumps

Date: 2026-10-20. This tests the recommendation in
[realtime-sim-survey.md](realtime-sim-survey.md), section 4: model each lit
falls mesh as a Lagrangian strand sheet, using the method of characteristics
for a supercritical sheet. It is the first run against the meshes we actually
draw, the 18 `FluidField_bakeN.bin` dumps (D74).

-   Code: scratchpad `strandsim.py`. It loads the dumps through `simbake.py`.
-   Plots: scratchpad `strandsim_*.png`. The scratchpad is not in the repo.
    Rerun with `python strandsim.py 3 7 11 0 13` (about 45 s, including the
    `simbake.py` comparison and the capsule test).
-   Nothing in the mod was changed, built or run.

---

## 0. Summary

**The model works on real meshes.** Strands part at rocks, leave open gaps
behind them that heal downstream, pile up against rock faces (thickening and
whitening), and carry run time and a seamless streak label. The per-frame
update is semi-Lagrangian along each strand. It reproduces the steady march,
sends a player cut down the curtain at the flow speed and heals it top first,
and carries inflow pulses exactly along the characteristics. Per mesh it costs
O(N x rows) with **no row-serial dependency**, so on the GPU one dispatch of
(mesh, row) thread groups of N threads does the update, the across-row
constraints and the texel gather together. The estimate is **0.03-0.1 ms for
20 meshes**.

**Three things in the data matter more than the model:**

1.  **v is not downhill on these meshes.** In all 18 dumps, the first 27-67% of
    each column (median) _climbs_ 150-470 world units before a crest; 12-68% of
    the water texels have dP/dv pointing up. Family B (dumps 2-7 and 13) climbs
    250-500 units over only 25-90 horizontal units, nearly vertically. A
    physical along-speed therefore decays on half the mesh. 49-68% of the
    strand nodes sit at the speed floor, and run times reach 7-14 s. This needs
    checking in game (open question 1). Either way, the along speed must not
    come from physics alone: floor it at vanilla's scroll speed.
2.  **Too much is rock for a parting model.** 19-61% of the covered texels are
    rock (dump 13: 61%). The current bake's 2D incompressible solve hides this
    by filling every shadow with an eddy. Strands, correctly, leave a dry wake
    behind every through-rock. With today's rock mask, 24-77% of a mesh's
    covered texels end up wet (water texels alone: 63-95%). The through/under
    split from the survey (section 5) is a **prerequisite**, not a refinement.
    The prototype's stand-in: a rock more than 14 texels to escape on both sides
    counts as under the sheet, and the water runs over it.
3.  **Rows are very uneven in world length.** On family B a texture row is 3.4
    world units at the top and 25-28 in the drop, an 8x range. The drop, where
    everything interesting happens, gets about 40 of the 192 rows, and a
    player-sized cut is only 2-4 rows tall there. Strand nodes, or the bake's
    height, should be spaced by world arc length.

All 18 dumps are instances of just **two meshes** (two UV rectangles). Each is a
single chart with v monotone (zero direction flips) and ragged sides only.
Multi-chart and reversed-v meshes are not covered by this test set.

---

## 1. Model as prototyped

### Grid and metric (per texel, from the dumped positions)

-   `t_u`, `t_v` are the world vectors per texel step (central differences, as
    in simbake).
-   `e_v = t_v/|t_v|` is the along direction. `e_a` is `t_u` minus its
    component along `e_v`, normalised: the across direction.
-   `l_v = |t_v|` (world units per row) and `l_a = |t_u - (t_u·e_v)e_v|` (world
    units per across texel).
-   `g_v = g·e_v` and `g_a = g·e_a`, with `g = (0, 0, -686)`.
    `n_z = |n̂·ẑ|` measures how much the surface lies flat (1 = flat).
-   Off-mesh texels take the nearest covered texel's values, so sampling at the
    edges works.

**Solid, wall and exit.**

-   Rock is closed with a 3x3 kernel: one-texel slits between rock pieces made
    strands flip sides from frame to frame.
-   Off-mesh texels in the foot band (lowest 15% of rows, or below a column's
    last texel when that column ends in the foot band) are **exit**: a strand
    that reaches one ends (a plunge).
-   Every other off-mesh texel is a **wall**.
-   The SDF is in texels (EDT, + in water), smoothed with σ = 0.7. Each row has
    two prefix-scan fields: the nearest free x on the left and on the right, at
    clearances 1.2 (steering) and 0.5 (projection).

### Strand state

There are N strands (128; 64 tested) and one node per texture row j. The
strands are seeded along the inflow edge (columns whose first texel is in the
top band, as simbake does) at **equal world arc length**, so they carry equal
discharge `q_k = h0·V0·W/N`, with h0 = 6 u (8.6 cm), V0 = 250 u/s and W the
lip width in world units.

| Field | Meaning                                                        |
| ----- | -------------------------------------------------------------- |
| s     | across position, texel x (float)                               |
| W     | across speed, world u/s along e_a                              |
| V     | along speed, world u/s                                         |
| T     | run time from the lip, s                                       |
| q     | discharge, carried (pulses)                                    |
| a     | aeration 0-1                                                   |
| L     | free-fall distance: drop counted where steep                   |
| comp  | compression vs the reference spacing                           |
| side  | -1/0/+1: which way it passes the current obstacle (hysteresis) |

### Advance one node by dy rows (dy = 1 in the steady march, c in the per-frame update)

```
dt   = dy·l_v / max(V, VMIN)
V'   = max(VMIN, V + dt·(g_v − CF·n_z·V − (g/V_term²)·V²))          CF = 1.5/s, V_term = 900, VMIN = 150
W'   = (W + dt·(g_a + ω²(s_ref − s)·l_a + 2ω(W_ref − W))) / (1 + dt·CF·n_z)      ω = 2/HEAL, HEAL = 0.6 s
s'   = s + ½(W + W')·dt / l_a
look ahead k = 1..5 rows along slope W'/V'·l_v/l_a: first row with SDF < 1.2 →
     side = nearer escape (L or R free x, hysteresis on the centreline),
     W_des = (x_escape − s')·l_a·V' / ((k+1)·l_v);  W' += (dy/(k+1))·(W_des − W')  (only toward the escape)
     both escapes > 14 texels → the rock is under the sheet: no steer, the water runs over it
project: SDF(s', j) < 0.5 → s' = nearest free x (preferring `side`); the into-rock part of W' is removed
         and becomes the impact rate imp
|W'| ≤ tan 60°·V'
T'   = T + dt;   L' = L + max(0, z(s) − z(s'))·clamp((0.8 − n_z)/0.6, 0, 1)
a'   = max( smoothstep(0.3·L_b, L_b, L'),  a·e^(−dt/3) + 1.5·dt·max(comp − 1.5, 0) + 0.6·imp/V' )
L_b  = 6·q_m^0.32 m (Horeni / Castillo), q_m = lip unit discharge in m²/s; ≈ 290 u for h0·V0
```

Then, **across each row** (4 Gauss-Seidel-like iterations):

-   Ordering: s\_{k+1} − s_k ≥ ε = 0.2 × the reference spacing. A violating
    pair is pushed apart symmetrically and their W averaged (an inelastic
    collision: equal q, so momentum is conserved). That is the pile-up, up to
    5x thickening.
-   Re-project any node pushed into solid, except over-rock nodes.

**Thickness and open gaps.**

-   A gap wider than max(3 texels, 3 × the reference spacing) is **open**
    (dry).
-   A node's spacing uses only its closed sides. An isolated node gets the
    reference spacing.
-   `h = q / (V · spacing · l_a)`, and `comp = reference spacing / spacing`.

### Changes from the survey's sketch, and why

| Change                                                                                                                | Why                                                                                                                                                                                                                                                                                         |
| --------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Heal toward vanilla's authored streak** (`s_ref = x0`, straight down v), not toward an obstacle-free physical march | With the physical march as the reference, lateral gravity on the domed meshes rolled the strands into a few ropes and left a third of the mesh dry (first run). Vanilla's v is the artist's flow, and D70/D71 asked for vanilla's way to drive harder. `REF=free` keeps the physical option |
| **Look-ahead steering, proportional**                                                                                 | A hard projection alone hits a rock face-on with no side information, and every strand slid along the face. A bang-bang W = W_des zig-zagged row to row (half the per-frame jitter). The proportional share removed it                                                                      |
| **Wide rock = under the sheet** (escape > 14 texels both ways)                                                        | Without it, strands meeting the wide "rock" bands (dumps 3, 11, 13) slid 70 texels sideways along one row. That is the survey's through/under limitation, made concrete                                                                                                                     |
| **Turn limit of 60°**                                                                                                 | Same: stops sideways sliding along long faces                                                                                                                                                                                                                                               |
| **Rock closing (3x3) and SDF blur**                                                                                   | Strands squeezing through one-texel slits flipped sides each frame (equilibrium drift fell from 6.7% to 2.9-3.7% of nodes)                                                                                                                                                                  |
| **Speed floor VMIN**                                                                                                  | v climbs on half the mesh (section 0), and the sheet would stop. The floor stands in for the push from the water behind. Better: vanilla's scroll speed                                                                                                                                     |
| **Free-fall distance weighted by steepness**                                                                          | The raw drop counted the sliding run and whitened everything below the crest                                                                                                                                                                                                                |

---

## 2. Results (steady march)

Plots, scratchpad `strandsim_N.png` (N = 3, 7, 11, 0, 13). Each has five
panels:

1.  The strands over the rock mask, coloured by mean aeration; cyan shows the
    obstacle-free paths.
2.  Thickness.
3.  Aeration.
4.  The label s (colour) with run-time contours every 0.1 s.
5.  The current D74 bake's streamlines (simbake.py) for comparison.

`strandsim_heights.png` shows the height of all 18 dumps; the dome is plain to
see.

| Dump | Rock / covered | Wet / water texels | Wet / covered | Nodes at VMIN | T max  | Fr < 1 | Fr < 2 |
| ---- | -------------- | ------------------ | ------------- | ------------- | ------ | ------ | ------ |
| 0    | 0.19           | 0.95               | 0.77          | 0.49          | 8.9 s  | 0.11   | 0.38   |
| 3    | 0.33           | 0.88               | 0.55          | 0.59          | 7.9 s  | 0.21   | 0.45   |
| 7    | 0.23           | 0.90               | 0.66          | 0.60          | 6.7 s  | 0.11   | 0.32   |
| 11   | 0.27           | 0.95               | 0.69          | 0.68          | 14.3 s | 0.27   | 0.47   |
| 13   | 0.61           | 0.63               | 0.24          | 0.68          | 11.0 s | 0.12   | 0.45   |

Fr = V/√(g·h·n_z), per node. The subcritical nodes are on the floored (climbing
or flat) parts and in the pile-up ropes.

### What worked

-   **Parting and open wakes.** For example, dump 0's two side rocks and dump
    7's right boulder: the sheet splits, a V-shaped dry wake stays open, and it
    heals over about HEAL × V downstream. The label s never folds or crosses
    (ordering), so the streaks bend round the rock with no whorls. Compare the
    simbake panel: it fills every shadow with recirculation (bake 3:
    counter-rotating eddies behind each rock).
-   **Pile-ups.** Ridges form where strands meet a rock face or squeeze between
    two rocks (dump 3, rows 100-125). Thickness reaches 5x and aeration rises.
    These are the pillow and rooster-tail places, so foam belongs there.
-   **Run time and label.** T contours bend smoothly around rocks and lag in
    the wakes. The label is an exact discharge stream function (equal q between
    strands).
-   **Aeration.** The sheet is clear on the climb and the run, and whitens
    progressively once it falls more than 0.3·L_b ≈ 90 u, so the drop is white
    by its foot. Impacts and bunching add local white streaks below the rocks.
-   **Funnelling.** On dump 0 the strands converge into the narrow foot channel
    and thicken, which is the right look for a constriction.

### What failed or needs care

-   **Huge dry areas where rock is over-classified**, chiefly dump 13 (24% of
    the covered texels wet) and the left third of dump 11. If those rocks are
    really under or behind the sheet (D59: sheets dip into the rock they lie
    on), the strand model will draw holes where vanilla draws water. The
    through/under split must come first.
-   **Over-rock strands look odd.** A strand that goes over a wide rock is
    pulled back to its authored column by the heal spring. Dump 11 shows
    diagonal strands across the rock.
-   **Side-entry water has no strands.** Columns whose first texel is below
    the top band are not seeded, so the ragged sides stay dry (dump 7, left
    strip). Seed extra low-q strands on side entries.
-   **The speed floor dominates** the upper half of every mesh: 49-68% of nodes
    are at 150 u/s. Run times of 7-14 s are far longer than a 1000-unit drop
    should take (about 2-3 s). Use vanilla's scroll speed as the floor (D67's
    shared scroll times the mesh's world units per v).

---

## 3. Time-dependent run (player capsule plus inflow noise)

`strandsim_capsule_3.png` and `strandsim_capsule_7.png` show eight frames, in
three rows: thickness, strands, and a "shader view" (streaks in s, bands moving
at t − T, white from a). `strandsim_pulse_3.png` and `strandsim_pulse_7.png`
are space-time plots.

**Scenario.**

-   A vertical capsule (radius 28, height 128 + 2r) is placed on the steepest
    part of the drop (g_v > 0.8 g).
-   Timeline: 0.5-2.0 s, it walks in from 140 units in front of the sheet;
    2.0-3.5 s, it slides 160 units across inside the curtain; 3.5-4.5 s, it
    backs out.
-   The lip discharge has ±15% two-octave value noise per strand (periods 1.2 s
    and 0.4 s, correlated over 8 strands), and the lip speed ±7.5%.
-   60 fps for 6 s.

**Per-frame update: semi-Lagrangian along each strand, every node in
parallel.** The obstacle field is this frame's: the static SDF min the
capsule's distance / texel size.

```
c        = V_old(k,j)·dt / l_v(k,j)            rows travelled this frame (0.3-2 here)
y*       = max(j − c, j0_k)                    lip parcels (y* clamped) take this frame's q, V0 noise
state*   = lerp(old(k, ⌊y*⌋), old(k, ⌊y*⌋+1))  along the strand's own rows
new(k,j) = Advance(state*, dy = j − y*, to row j, obstacles(t))
then the row constraints (ordering, projection), spacing, h, comp
```

-   **Equilibrium.** Started from the steady march with static obstacles, it
    settles within 30 frames to a nearby fixed point. 2.9-3.7% of nodes end
    more than 1 texel from the row-march solution: side choices at threshold
    cases. The median T change is 0.002 s, and it does not drift over 300
    frames.
-   **Residual jitter.** 11-26 nodes per frame (of 23k) move more than 0.5
    texel, all at decision thresholds: the 1.2-texel clearance edge, and the
    14-texel over/around choice at the foot. The GPU version needs hysteresis
    on both.
-   **The cut.** It appears where the capsule meets the sheet. The gap below it
    grows down at the flow speed. With HEAL = 0.6 s the dry fraction of the 24
    columns below the capsule rises from 2% to 25% (dump 3) and from 9% to 45%
    (dump 7). After the capsule leaves, the gap closes **top first** within
    about 0.7 s (the far right panel of the pulse plots). Strands slide back in
    behind it, visible as a brief scissor in the shader view at t = 4.5 s.
-   **Pulses.** The thickness anomaly along a strand far from the capsule lies
    exactly along the characteristics t0 + T(j) of the steady run time (left
    panel, black lines). Pulses ride down coherently and slow where the flow
    is floored. Numerical diffusion from the row interpolation is visible but
    mild at c ≈ 0.3-1.
-   **Healing time is the main look knob.** At 0.35 s the gap under the player
    is short (about 10 rows on the drop). At 0.6 s it is a clear wedge. Real
    water closes far more slowly (Taylor-Culick retraction is only about 10
    u/s for a 1 cm sheet), so a longer HEAL, or a separate one for dynamic
    obstacles, is defensible.

---

## 4. Cost

### numpy, measured (one core; Python-overhead bound)

| Step                                             | N = 64 | N = 128                   |
| ------------------------------------------------ | ------ | ------------------------- |
| Steady row march (192 rows, Python loop per row) | 318 ms | 320-400 ms                |
| Per-frame update (all nodes vectorised)          | 19 ms  | 35 ms (about 1.5 µs/node) |
| Gather to texels (loop per row)                  | 20 ms  | 20 ms                     |
| Obstacle fields (EDT, scans, capsule)            | 4-5 ms | 4-5 ms                    |

### GPU estimate (RTX 3060 / RX 6600 class; worst case 20 meshes x 128 strands x 192 rows = 492k nodes, 737k texels)

**Fused update and gather.** One dispatch, one thread group per (mesh, row),
with N = 128 threads (thread k = strand k):

-   **Reads:** the previous frame's 2 nodes of its own strand at ⌊y\*⌋ (neighbouring
    threads read neighbouring rows, so these are L2 hits), plus about 17 field
    fetches: 5 metric, 6 SDF (look-ahead), 4 free-x and 2 z. The fields are
    about 1.2 MB per mesh and cache-resident.
-   **ALU:** about 150-250 operations per node.
-   **Row constraints:** 4 ordering iterations in groupshared memory, with
    GroupMemoryBarriers, then spacing, h and comp from the neighbours.
-   **Gather in the same group:** the row's strands are already in groupshared
    memory. Each thread writes `w/N` ≈ 1.5 texels of row j: a 7-step binary
    search, then interpolate.
-   **DRAM traffic:** node state at about 20 B in fp16 (s and T in fp32), read
    and written once each: 492k x 40 B = 20 MB. Texel writes: 737k x 8-16 B =
    6-12 MB. Total 26-32 MB, **about 0.08-0.09 ms at 360 GB/s** for the worst
    case of 20 meshes.
-   **Typical case** (3-6 meshes near the camera, far meshes at K/2 and every
    second row, or frozen): **0.01-0.03 ms**.
-   ALU and fetch are not the limit: 492k x 200 ops is about 0.1 GFLOP.

**Steady march** (load time and teleports). It is row-serial: one thread per
strand, looping over 192 rows. On the CPU in C++ that is about 25k nodes x
about 200 ops, under 5 ms per mesh. It replaces the sparse pressure solve in
`SolveBake` and can run in the existing async bake job.

**Capsules.** Up to about 16 capsules in a cbuffer, culled per mesh by its
bounds. Each node measures the distance from its own world point (bilinear P
at (s, j)) to each capsule. There is no per-frame SDF rebuild.

-   The look-ahead needs the capsule distance at rows j+1 to j+5 too: 5 x 16
    capsule tests per node, which is still trivial.
-   The escape side for a capsule is analytic: project the capsule axis onto the
    sheet and compare the node's s with the projected centre's s. No free-x
    scan is needed.

---

## 5. Problems and open questions

1.  **Climbing v (the dome).**
    -   In-game check: put Falls Mesh Debug (D75) on an instance of each family
        and confirm the arrows climb the first half. Then dump the original
        vanilla vertices next to the dense copy (D73). One subdivision of a
        PN-triangle copy should not make a 300-unit wall, but the copy is the
        mesh the bake reads.
    -   If it is real (for example the back of a lip curl, or the sheet
        climbing out of the river plane), the climbing part is not falling
        water. Classify it (dP/dv·ẑ > 0) and let the strands keep vanilla's
        scroll speed there.
2.  **Flat ledges and subcritical flow.**
    -   11-27% of nodes have Fr < 1, and 32-47% have Fr < 2, all on the floored
        or flat parts and the pile-ups. Strands cannot do hydraulic jumps,
        backwater or pooling on a ledge.
    -   Mitigation: there, keep vanilla's direction, the floor speed and a
        whitening term. That is the survey's rank-2 Eulerian solver's
        territory if it is ever needed.
3.  **Eddies.** There are none by construction. Behind a rock the strand
    model gives a dry wake that heals, not recirculation. That is right for a
    curtain and wrong for a pool face. The pool solver already covers pools.
4.  **Through versus under rock.**
    -   This is the dominant visual risk (section 2).
    -   Needed in the bake (FallSites `InsideSolid`): keep "rock on both sides
        of the sheet" (through) separate from "rock behind or under" (under).
        Only through-rock goes into the strand SDF. Under-rock adds friction,
        aeration and thickening.
    -   Until then, MAX_ESC is the heuristic.
5.  **Merging meshes.**
    -   No dump crosses meshes, so this is untested.
    -   Cascades (D67 LinkBakes): the receiver's strands are seeded from the
        feeder's last-row strands, mapped by world proximity. The receiver
        inherits label, T, q, a and V, with a one-frame lag (read the
        previous frame's buffer). This replaces `across0` and `along0` in
        `LinkBakes`.
    -   Side-by-side meshes: the edge strands' world points act as capsule-like
        obstacles for the other mesh, plus a confluence foam band. True
        merging needs the Eulerian solver.
6.  **UV charts not top-to-bottom.**
    -   The two dumped meshes are single charts with monotone v, and only 9-15
        columns have split coverage (ragged edges).
    -   For meshes with several charts side by side along u, each chart is its
        own strand set (seed per chart's top edge).
    -   For charts stacked along v (a seam row), treat it like a cascade inside
        one mesh.
    -   A chart with v reversed or sideways (dP/dv·ẑ > 0 over most of it, or
        the inflow on a side) should fall back to the current bake.
7.  **Row resolution.**
    -   l_v ranges over 3.4-28 u per row on family B.
    -   Either size the bake height from the drop's l_v (a v-remap table per
        mesh, stored as a 1D texture of v per row, so BakeAt maps uv to row
        through it), or keep the grid and give strands their own uniform-
        arc-length nodes and gather through the remap.
    -   Without this, a player cut in the drop is 2-4 rows tall.
8.  **Decision thresholds.** Hysteresis on: the side (keep the last frame's
    unless the other escape is more than 1.5 texels nearer), the over/around
    choice (keep for 0.3 s) and steering onset (two clearances). Otherwise a
    few nodes flicker sub-texel, and the label at those texels shimmers.
9.  **Tuning knobs that change the look most:** HEAL (wake length), EPS_FRAC
    (maximum pile-up thickening), MAX_ESC (through/under), the look-ahead
    length (pillow size), and the lip h0/V0 (L_b, so where whitening starts).

---

## 6. GPU implementation plan, mapped onto our code

This keeps the survey's section 10 stages and fills in the code mapping.

### Stage 1: bake (CPU, at load: FallSites::StartBake / SolveBake / LinkBakes)

-   `BakeJob` already has `position`, `normal`, `covered` and `rock`.
    -   Add the through/under split, as two masks.
    -   Add the per-texel metric (`l_v`, `l_a`, `g_v`, `g_a`, `n_z`) and z.
    -   Add the SDF and free-x scans for the static through-rock.
    -   Add the exit mask (foot band, as simbake's outflow).
-   `SolveBake`: replace the MAC/pressure solve with the **steady strand
    march**. A C++ port of `Sheet.march` with ref = authored v gives N x H
    nodes plus the gathered (label, T, a, foam, wet) texels.
    -   Keep `turn`, `speed`, `wake` and `foam` filled from the strands for the
        consumers that read them: D75 arrows, D78 plunges and spray. Speed
        becomes V, foam becomes imp plus pile-up, wake becomes aeration.
-   `LinkBakes`: feed each receiver's strand seeds from its feeder's last row
    (label, T, q, a, V). The current `across0`/`along0` fit becomes a per-strand
    map.
-   **First visible milestone, CPU only.** Upload the gathered steady result
    into the existing immutable t124. Use x = vanilla u at the strand's lip
    position, interpolated by label; y = T × the shared scroll rate (what
    `along` is today); z = aeration (the shader's `wake` slot); w = foam. The
    shader is unchanged. This alone tests parting, wakes and seams in game.

### Stage 2: GPU state and update (FluidField.cpp, a new FallStrands.hlsl compute)

**Resources** per active mesh:

-   A structured buffer of N x H nodes, ping-ponged. Bake fields in one
    Texture2DArray slice per mesh (or a packed atlas), RGBA16F: (l_v, l_a, g_v,
    g_a), (n_z, z, sdf, exit), (Lc, Rc, Lm, Rm).
-   t124 becomes `D3D11_USAGE_DEFAULT` with UAV + SRV, in **RGBA16F**: (u
    label, run time, aeration, foam). Wet alpha goes in a second R8 texture,
    or is folded into foam's sign.
-   A capsule cbuffer filled from the actor tracking the AirSources and
    WakeSources already use.

**Dispatch:** one per frame, (H, meshes) groups of N threads, following the
loop of `frame_update`:

1.  Back-trace and interpolate the previous node.
2.  Advance, with steering against the static SDF plus capsules (analytic).
3.  Store to groupshared memory and run the ordering and projection
    iterations.
4.  Compute spacing, h and comp, and write the node.
5.  Gather row j's texels into t124 and the wet texture.

**Sources:**

-   Lip q and V0 noise per strand, from the hash plus the time in the cbuffer.
-   Cascade seeds read from the feeder's previous-frame buffer.

**LOD:**

-   Off-screen and far meshes skip frames, or keep the steady texture (the
    immutable copy).
-   Middle-distance meshes run with N/2 strands.

### Stage 3: shading (FallGround.hlsli BakeAt/Flow, Lighting.hlsl, Utility.hlsl)

-   `BakeAt` is unchanged: bilinear over t124. Under the v-remap (problem 7) it
    reads row = remap(v) first.
-   `Flow`: `uvA = baked.xy − (0, Scroll.x)` as today. Add `result.wet` and
    `result.aeration` (from the z slot, replacing the wake).
-   Gaps: `clip(wet − dither)` in Lighting.hlsl's falls branch **and in
    Utility.hlsl's depth pass**. Utility.hlsl's pixel shader does not bind t124
    today: bind it for falls draws, as D57/D69 did for the per-vertex buffer,
    or the pre-pass will draw the gap.
-   Aeration replaces `aerated` (D59's run-time aeration) in the whitening line
    of Lighting.hlsl. Pile-up and impact feed `foam`. Later, h drives
    translucency once the blended pass exists (D53-D56).

### Stage 4: coupling outputs

-   Per mesh, sum the impact momentum q·imp for spray (D78 BuildPlunges'
    rock-foam spray reads it).
-   The strand's end at the exit band (row, world point, V, q, a) becomes the
    plunge emitter. This replaces BuildPlunges' foot scan, which needs a small
    readback or a GPU append buffer.

### Validation in game

-   Repeat the survey's list: fall 0001AC75's pillar split, the player cut, a
    cascade's continuity, a tall fall's clear-to-white.
-   Also show the "climbing" check (problem 1) and the through/under debug
    view before judging the look.
