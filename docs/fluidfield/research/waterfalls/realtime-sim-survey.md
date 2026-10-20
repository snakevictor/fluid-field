# Real-time water on vanilla falls meshes: simulation survey

Date: 2026-10-20. Scope: options for making the water on Skyrim's vanilla falls
meshes (curtains, slopes, arches) respond to the scene in real time. That means
rocks through and beside the sheet, the ground under it, the pools below,
other falls meshes it meets, and the player. The whole fall must be simulated,
including vertical and overhanging parts. The budget is 0.3-0.5 ms of GPU per
frame for all nearby falls on a mid-range GPU.

This builds on [techniques-survey.md](techniques-survey.md) and
[techniques-deep.md](techniques-deep.md) and does not repeat them. Those cover
the static streak warp, obstacle masks and seam foam. Decisions D59-D69 in
[DECISIONS.md](../../DECISIONS.md) describe the current bake.

Cost figures assume a mid-range DX11 GPU of the RTX 3060 or RX 6600 class
(about 360 GB/s and 10-13 TFLOPS). The worst-case domain is 20 meshes x 192 x
192 = 737k cells. All figures are estimates from memory traffic, not
measurements. The paper timings cited were measured on hardware 15-20 years
older.

---

## 0. Summary

**Recommendation.** Simulate each falls mesh as a **Lagrangian strand sheet**
(section 4, ranked 1). This is the method of characteristics for a supercritical
sheet: 64-128 streamlines per mesh, each stored as one value per mesh row,
updated every frame against obstacles. From the strands, rebuild the texture
coordinates the shader already uses, (streak label s, run time r), plus
thickness and aeration. The cost is about 0.05-0.1 ms in total.

If general 2D flow is needed later (flat ledges, side-by-side merging, eddies),
add a **texture-space shallow-water solver on the mesh atlas** (section 3, ranked
2). That is Sea of Thieves' approach, adapted with Wang-Miller-Turk's split of
gravity into normal and tangential parts. Their pool solver's kernels can be
reused.

All options need the same rendering upgrade (section 7, ranked 3). The texture
is sampled at (s, t - r) with per-strand stochastic variation. Thickness and
aeration drive translucency, whiteness and breakup. Discharge pulses are fed in
at the lip and carried down by the simulation.

Key insights:

1.  **A falling sheet is supercritical.** The Froude number is about 3-30
    everywhere except right in front of obstacles. Information only travels
    downstream. So the steady state can be _marched_ row by row down v, and a
    time-dependent version only needs a semi-Lagrangian step along each
    streamline. No pressure solve (CG or Jacobi) is needed or even correct.
2.  **The current bake's incompressible 2D projection is the wrong model for a
    sheet.** A sheet thins as it speeds up, so the conserved quantity is the
    discharge q = h·u. The 2D velocity is not divergence-free. The streak label
    s should be the stream function of the discharge, so that equal discharge
    flows between neighbouring streaks.
3.  **On a free-falling curtain, the pressure term vanishes.** Gravity normal to
    the sheet, g_n, is about 0 when nothing is behind the water. The motion is
    ballistic along the surface: tangential gravity, plus obstacles, plus weak
    surface tension. The sheet's interest lies in parting at obstacles, wakes
    that stay open, thinning, and breakup. It is not in waves.
4.  **Texture advection on a fall needs no regeneration.** A parcel enters at the
    lip and leaves within about 1-5 s. So the advected phase t - r stays
    bounded and well conditioned: the inflow boundary _is_ the regeneration.
    Pulsing is avoidable by construction. Only recirculation zones behind rocks
    need clamping or masking.
5.  **Physical breakup length.** A rectangular nappe breaks up after about
    L_b ≈ 6·q^0.32 m (Horeni, used by Castillo and others; q in m²/s). Typical
    Skyrim falls have q of about 0.05-0.5 m²/s, so L_b is about 2-5 m. Most
    curtains taller than about 5 m should therefore be broken into fingers and
    white by the bottom. This gives a physically grounded driver for aeration
    and translucency.
6.  **Shipped-game precedent.** Sea of Thieves runs a Mei et al. 2007 pipe-model
    shallow-water simulation in the _texture space of waterfall and stream
    meshes_. It projects the camera depth buffer into that texture space so that
    characters occlude the falling stream. That is the closest published match
    to our setup. Everyone else bakes: Uncharted 4 used Houdini flowmaps and
    Horizon Forbidden West used baked deformation volumes for waterfall impacts.
    Heightfield engines turn falls into particles: Chentanez and Müller 2010,
    and Kellomäki's thesis.

---

## 1. The physics we need to capture (and what we can ignore)

Typical numbers. Skyrim units: 1 unit = 1.428 cm and 10 units per texel = 14.3
cm. A 192-texel mesh is about 27 m long.

| Quantity                         | Value                            | Consequence                                                       |
| -------------------------------- | -------------------------------- | ----------------------------------------------------------------- |
| Speed after a 5 / 20 / 40 m drop | 10 / 20 / 28 m/s                 | Up to about 2-3 texels per frame at 60 fps                        |
| Sheet thickness                  | 1-10 cm, thinning roughly as 1/u | g_n·h pressure waves at about 0.3-1 m/s, far slower than the flow |
| Froude number u/√(g_n h)         | 3-30, or ∞ on free curtains      | Supercritical: marching or characteristics are valid              |
| Travel time down a fall          | About 1-4 s                      | The sheet "forgets" its state quickly, so transients are short    |
| Breakup length L_b ≈ 6 q^0.32    | About 2-5 m                      | Below L_b the sheet is fingers, droplets and spray                |

**What matters visually**, and every approach is judged on these:

1.  **Parting at obstacles.** Streaks pile up in front of a rock (pillow), split
    around it, leave a gap or wake behind it, and heal slowly downstream.
2.  **Lateral contraction and spreading** where the surface curves or the ground
    narrows.
3.  **Thinning, aeration and breakup** with fall distance.
4.  **Time behaviour.** Surges and pulses travel down at the flow speed, and the
    response to the player arrives after the travel time.
5.  **Continuity across meshes.** One mesh's water flows into the next.

**What we can ignore:** dispersive waves on the curtain, true 3D detachment of
the sheet from the mesh (the vanilla mesh _is_ the sheet's shape), and
recirculation on the curtain itself. The pools already have the top-down
shallow-water solver.

---

## 2. Cities: Skylines (original) and the virtual-pipe model

### How Cities: Skylines water works

-   Colossal Order is in Tampere. Their water was built with Timo Kellomäki
    (Tampere University of Technology), whose thesis _Large-Scale Water
    Simulation in Games_ (2015) was written alongside the Cities: Skylines water
    work.
-   The method is a **heightfield shallow-water model of the virtual-pipe
    family**. Kellomäki's 2013 WSCG paper _Interaction with Dynamic Large
    Bodies in Efficient, Real-Time Water Simulation_ generalises "the extremely
    fast virtual pipe method" so that water diverts around large dynamic bodies,
    for example dams built from game objects.
-   The thesis also uses particles for 3D effects such as splashes and
    waterfalls.
-   One finding in it matters here. A user study found that **removing velocity
    self-advection had no statistically significant perceptual impact** for
    large-scale water. This holds for rivers. It does not hold for falls wakes:
    see the pitfalls below.
-   No primary source was found for the exact Cities: Skylines grid resolution
    or threading. It is commonly described as a fixed grid over the whole map,
    updated on the simulation thread. Treat any specific number as unverified.

### The virtual-pipe model (O'Brien and Hodgins 1995; Mei, Decaudin and Hu 2007)

Each cell stores its water depth d. Each face stores an outflow flux f (4 per
cell, or 2 per cell on a staggered layout). One step:

```
f_L(x,y) ← max(0, f_L + dt·A·g·Δh_L / l)     Δh_L = (b + d)(x,y) − (b + d)(x−1,y)
K = min(1, d·lx·ly / ((f_L+f_R+f_T+f_B)·dt))   (scale outflow so depth stays ≥ 0)
d ← d + dt·(Σ inflow − Σ outflow) / (lx·ly)
velocity (optional) from the averaged fluxes and the mean depth
```

-   It is explicit and needs 2 passes per step: flux, then depth. Mass is
    conserved by construction.
-   Its momentum is the persistence of each face flux. There is no advection
    term.
-   Cost data: Mei 2007 measured, on a GeForce 8800 GTX, 1.29 ms for 256² and
    2.38 ms for 512² per simulation step _including erosion_. Jako (CESCG 2011)
    measured 0.3 ms for 128² and 15.5 ms for 512² on a Radeon HD 4870, also
    with erosion.
-   Modern estimate for the bare pipe step: reading and writing about 40 bytes
    per cell over 737k cells is about 30 MB, so **about 0.08-0.12 ms per step**,
    or half that with fp16 state.

### Can it run on a mesh atlas, a surface manifold? Yes, simply

On a curved surface, replace the "terrain height" b in the pipe acceleration
with the **world height z of the texel's surface point**. Replace the water head
with g_n·h, where g_n = max(0, −g·n̂) is the gravity component that presses the
water onto the surface:

```
Δ(head)_ij = g·(z_i − z_j) + (g_n,i·h_i − g_n,j·h_j)     → pipe acceleration along edge ij
cell area  = |∂P/∂u × ∂P/∂v|·Δu·Δv                       → depth update
pipe length = |P_i − P_j|  (world distance between texel centres)
```

-   The term g(z_i − z_j)/|P_i − P_j| is exactly tangential gravity along the
    edge. So vertical and overhanging parts work: on a vertical curtain, g_n is
    0 and the water is purely accelerated down.
-   This is the virtual-pipe version of Wang, Miller and Turk 2007 (GSWE,
    section 3.2), which builds the height field along surface normals and splits
    gravity into normal pressure and tangential acceleration.

### Pitfalls for falls

1.  **Speed cap from the outflow limit.**
    -   The scale factor K stops a cell from sending more water than it holds per
        step. So water cannot move more than about one cell per step:
        v_max ≈ dx/dt = 14.3 cm / (1/60 s) ≈ 8.6 m/s.
    -   Falls reach 10-28 m/s, so 3-4 substeps per frame or coarser cells are
        needed near the bottom of tall falls.
2.  **No momentum transport, so no wakes.**
    -   A pipe-model sheet split by a rock immediately refills behind it, because
        each face only sees local head differences.
    -   The V-shaped gap, the "rooster tail" and the lateral throw are lost.
    -   Kellomäki's finding applies to rivers, not to supercritical sheets. For
        falls, momentum transport is the very effect we want.
3.  **Projection artefacts.** Dry-cell handling (depth near 0) on a curtain that
    breaks up is fiddly. It is better to keep the sheet "wet" and represent
    breakup as aeration (section 7).

**Verdict.** The pipe model is a fine general engine on the atlas. It is not the
best tool for wakes on supercritical sheets. Use it, or the semi-Lagrangian
shallow-water variant in section 3, only if we need general 2D behaviour.

---

## 3. Texture-space shallow water on the mesh atlas (manifold SWE)

### How it works

Use the same grid we already bake: one texel per about 10 units, laid out by the
mesh UVs, up to 192². Precompute per texel:

-   The world position P, the tangent frame ∂P/∂u and ∂P/∂v, and from them the
    **metric** g_ij and its inverse. Stam 2003 shows that ignoring the metric
    produces visible parametrisation artefacts.
-   The outward normal n̂ and the tangential gravity expressed in UV
    coordinates: g_t^uv = G⁻¹·(J^T·g).
-   **Attachment**, a 0-1 value saying whether there is bed directly behind the
    water. It comes from the existing ray parity and rock data. g_n is applied
    only where water is attached.
-   **Obstacle SDF** in texel units: rock through the sheet, baked once (section
    5).

State: h, the velocity in UV units, plus passive scalars (streak label s, age r
and aeration a).

Per step:

1.  **Forces:** tangential gravity, bed friction where attached
    (Manning-style C_f·|u|u/h), and the obstacle response.
2.  **Advection:** semi-Lagrangian or MacCormack/BFECC for the velocity and
    scalars, back-tracing in UV with the metric. Back-traces that leave the
    chart go through the ghost or indirection table (section 6).
3.  **Height update:** upwind flux divergence. This is literally Fluid Field's
    `WaterHeightCS`, with cell areas and face lengths from the metric.
4.  **Pressure term** −∇(g_n·h): only where attached, so free curtains skip it.

Steps 1 and 4 are local, so the solver is fully explicit. **There is no pressure
solve.** Stable Fluids' projection is for incompressible flow, and a sheet is
not incompressible in 2D.

### Shipped and published precedent

-   **Sea of Thieves** (Rare, SIGGRAPH 2018 talk). A GPU water surface
    simulation based on Mei 2007 is used for deck splashes. For waterfalls and
    streams it is supplemented by "projecting the depth buffer from the
    perspective of the camera onto the surface of a mesh, into the texture space
    of its shallow water simulation". A character intersecting a waterfall
    occludes the falling stream, and a character standing in a stream gets foam
    around their feet.
-   **Wang, Miller and Turk 2007**, _Solving General Shallow Wave Equations on
    Surfaces_. Height is measured along normals and gravity is split into a
    normal pressure part and a tangential acceleration. Implicit gravity and
    surface tension, and it runs on curved meshes without parametrisation. It
    managed 1-11 fps on 2007 hardware with implicit solves. We do not need the
    implicit parts at our speeds and thicknesses.
-   **Stam 2003**, _Flows on Surfaces of Arbitrary Topology_. Stable Fluids in
    curvilinear coordinates on Catmull-Clark patch atlases. It adds a ghost
    layer per patch, filled from neighbouring patches with transition functions,
    and a `Clip` routine that walks a semi-Lagrangian back-trace across patches
    while accumulating the Jacobian. This is the canonical seam treatment.
-   **Carvalho, Andrade and Velho 2012**, _Fluid Simulation on Surfaces in the
    GPU_. Stam's approach on the GPU.
-   **Products:** Unity's _Fluid Flow_ asset (texture-space flow over meshes with
    gravity maps; "fluid can flow over UV seams"; about 40 B per pixel plus 32 B
    per triangle), Jung and Behr 2009 (GPU on-surface droplet flow in texture
    memory), and Tatarchuk and Isidoro 2006 (ToyShop: droplets trickling down
    glass, simulated in a texture).

### Cost for 20 x 192²

| Pass                                                    | Bytes per cell (fp16 state) | Estimate               |
| ------------------------------------------------------- | --------------------------- | ---------------------- |
| Forces plus obstacle                                    | About 24                    | 0.03 ms                |
| Advection: velocity plus 3 scalars, bilinear back-trace | About 40                    | 0.06 ms                |
| Height update                                           | About 20                    | 0.03 ms                |
| Ghost or seam fill (only boundary texels)               | Small                       | <0.01 ms               |
| **Per step**                                            |                             | **About 0.12-0.15 ms** |

-   Speeds up to 28 m/s are about 3 texels per frame. Semi-Lagrangian advection
    is unconditionally stable, but the explicit upwind height update needs
    CFL < 1. So expect 2-3 substeps on tall falls, or a CFL-adaptive count per
    mesh: about **0.25-0.4 ms** worst case.
-   **Amortisation brings it into budget:**
    -   Simulate only meshes within about 3-4k units.
    -   Use 96² for meshes beyond about 1.5k units, which costs 4x less.
    -   Freeze far meshes on their last state.
    -   Pack every mesh into **one atlas texture or Texture2DArray**, so that
        each pass is one dispatch, not 20. On DX11, dispatch overhead of
        5-10 µs times passes times meshes otherwise dominates.
-   The realistic total with 6-8 meshes near the camera is about 0.1-0.15 ms.

### Fit with our setup

-   Strong: the grid, positions, normals and rock classification already exist,
    and so do the pool solver's advect and height kernels. The bake's
    "steady-state CPU sim" becomes the GPU sim's initial state and its
    equilibrium.
-   Pitfalls:
    -   Numerical diffusion of s smears streaks. Use BFECC/MacCormack for s,
        or carry s on strands instead (section 4).
    -   Wet and dry fronts.
    -   Thin charts: a chart a few texels wide needs ghost cells on both sides.
    -   Metric singularities where the UVs are degenerate.
    -   Explicit CFL near the bottom of tall falls.

---

## 4. Lagrangian strand sheet (method of characteristics). RECOMMENDED

### Idea

Because the sheet is supercritical and v runs downstream on every vanilla falls
mesh (D60/D67), represent the water as **K streamlines ("strands") per mesh**.
Each strand k stores, per mesh row j (fixed v_j):

-   U_k(j): its across position in UV, which is the streak's location.
-   W_k(j): the across velocity, du/dt.
-   T_k(j): the run time from the lip, which is the travel time.
-   q_k: the discharge it carries, fixed per strand (equal shares, or from the
    bake).
-   a_k(j): aeration, plus optionally a thickness or pulse amplitude.

This is the method of characteristics. It is also the space-marching scheme
used for steady supercritical channel flow (Glaister 1991; Jimenez and
Chaudhry 1988), and the continuous version of the stream-function and run-time
coordinates we bake today. The streak label is simply s = k/K (+ interpolation),
so it is _exactly_ a discharge stream function with no numerical diffusion.

### Per frame (time-dependent, semi-Lagrangian along each strand)

For each strand and row:

```
along speed  w_j     = from energy: ½|V|² = ½|V_lip|² + g·(z_lip − z(U,v_j)) − losses   (ballistic sheet)
               (or carried: V_j = V_{j-1} + g_t·Δt_j − friction, with Δt_j = Δs_j / |V|)
back-trace   j*      = j − w_j·dt / Δv   (fractional row; read the previous frame's strand at j*)
U_k(j)  ←  U_k^old(j*) + W_k^old(j*)·dt                 (transport along the strand)
W_k(j)  ←  W^old(j*) + dt·[ g_t,across − ∂(g_n·h)/∂u (attached only) + obstacle(U, v_j) + cohesion(U_{k±1}) ]
T_k(j)  ←  T_k(j−1) + Δs_j / |V_j|                       (run time down the strand)
h_k(j)  =  q_k / (spacing_k(j)·|V_j|)                    spacing = ½(U_{k+1} − U_{k−1})·|∂P/∂u|
a_k(j)  =  aeration from fall distance / L_b(q_k), plus impact and pile-up sources, advected like U
```

**Obstacle response, position-based.**

-   Look up the rock or actor SDF at (U, v_j). Inside or within a radius, push
    U along ∇SDF and remove the into-obstacle velocity. That is an inelastic
    slide, and the momentum removed feeds spray and foam.
-   Each strand chooses its side by the SDF gradient's sign. Strands behind the
    obstacle's centreline go left or right, so the sheet _parts_.
-   Downstream, the parted strands keep their across velocity. So the gap behind
    the rock stays open in a V and closes only through cohesion, surface tension
    pulling neighbours together, and convergence of the ground. That is the
    "split gap that heals downstream" we want, for free.
-   **Ordering constraint:** U\_{k+1} ≥ U_k + ε, so strands never cross, which
    keeps s monotonic. Where two strands would cross, set them at ε apart and
    mark a **pile-up** (h rises): the sheet thickens and whitens. That is the
    pillow and ridge physics. Real water also forms a raised seam or rooster
    tail where two deflected streams collide.

**Time behaviour.**

-   Changes travel down at the flow speed, with the correct delay. A player
    stepping into the curtain at mid-height cuts it there. The gap below opens
    over the next T seconds, and heals when they leave, top first.
-   Inflow _pulses_: modulate q_k(t) and the lip speed with smooth noise per
    strand. The surges and finger-like streaks then ride down the fall
    coherently. This is the cheap source of the "living" look a scrolling
    texture lacks.

### From strands to what the shader reads

One gather pass per texel, using the mesh atlas:

-   For texel (u, v_j), binary-search K strands at row j: 7 steps for K = 128.
-   Interpolate s, T, h, a and the across velocity.
-   Outside the outermost strands, or inside gaps wider than a threshold, the
    texel is **dry or air**: alpha falls off, and the shader shows rock or
    nothing.
-   Write an RGBA16F texture per mesh: (s, T, h, a), with the across velocity
    optional. That is the same layout and slot as today's bake (t124), so the
    lit shader, depth shaders and vertex clipping need minimal change.

### Cost (20 meshes, K = 128 strands x 192 rows)

| Pass                       | Work                                                                                                                  | Estimate                                                                            |
| -------------------------- | --------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------- |
| Strand update              | 20 x 128 x 192 = 492k node updates, each with an SDF fetch and a few neighbour reads                                  | About 0.03-0.05 ms                                                                  |
| Gather into texel textures | 737k texels x 7-step binary search in groupshared memory (one thread group per row stages that row's K strand values) | About 0.04-0.06 ms                                                                  |
| **Total**                  |                                                                                                                       | **About 0.07-0.1 ms worst case. About 0.03 ms with LOD (K = 64, 96 rows when far)** |

The strand state is tiny, about 20 x 128 x 192 x 16 B = 7.9 MB at full size, or
much less in fp16.

### Seams and multiple meshes

-   **Cascades** (mesh A's bottom flows into B's top): link table from A's last
    row to positions on B's first row. This is the D67 cascade link, extended to
    per-strand mapping. B's strands inherit q, s, T, aeration and speed. The
    label s and run time continue, so the texture continues across the join with
    no seam in either coordinate. Only the geometry seam (D69) remains.
-   **Side-by-side meshes that meet**, or two falls merging:
    -   Treat the boundary strand of each as an obstacle for the other, using a
        world-space distance test of the edge strands' positions.
    -   Or, simpler: where both meshes' sheets are within a few cm in world
        space, mark a confluence foam band and let both continue.
    -   True lateral merging (two sheets becoming one) needs section 3's
        Eulerian solver on a shared chart.
-   **UV charts within one mesh:** strands follow v, and charts on vanilla falls
    are strips along v. A strand that reaches a chart's side boundary continues
    into the adjacent chart via the same edge-adjacency table (section 6). In
    practice, clamp at mesh edges first and add adjacency only where a mesh
    really has side-by-side charts.

### Limitations

-   It assumes a monotone downstream v. This holds for every vanilla falls mesh
    we inspected (vanilla-nif-dissection.md).
-   No recirculation or eddies. Those are pool territory, already covered by the
    top-down shallow-water solver.
-   Lateral pressure on attached parts is approximate: a neighbour-spacing term.
    This is fine at Fr ≫ 1.
-   Strands cannot represent a sheet hitting a ledge and _throwing_ off the
    surface in 3D. The vanilla mesh shape is fixed. Express it as spray plus
    whitening.
-   A full-width rock across the whole sheet leaves no strands. Then the sheet
    is "over the rock" (the rock is under the water, not through it). The
    ray-parity bake must distinguish _through_ from _under_. Under-water rock
    should only slow, thicken and whiten, never part.

---

## 5. Coupling with the environment

| Coupling                            | Detection                                                                                                                                                                                                                                                                   | Update rate                 | Notes                                                                                                                                                                                                                               |
| ----------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Rock through or beside the sheet    | Existing ray parity against collision and static render meshes, converted to an **SDF in texel units** (jump flooding, about 10 passes once per mesh)                                                                                                                       | Baked at load               | The SDF gives the strand push direction and the Eulerian solid mask with slip normals. Keep "through" (parts the sheet) separate from "behind or under" (attached: friction, g_n pressure, thickening)                              |
| Ground under or behind the sheet    | Same rays: distance from the texel along −n̂ to the first solid                                                                                                                                                                                                              | Baked                       | Gives the attachment flag and the friction, and where the sheet lands (impact foam and spray)                                                                                                                                       |
| Pools it falls into                 | The pool solver's water height at the texel's world xy (already sampled for the D58 cut)                                                                                                                                                                                    | Every frame (cheap read)    | Below the water, the strand ends: emit the plunge (discharge and speed) into the pool solver's emitters (D22/D44), and the foam bloom at the seam. Sharing the strand's aeration with the pool foam fixes the "one foam look" issue |
| Other falls meshes                  | Cascade link (strand handoff), plus edge proximity for side-by-side                                                                                                                                                                                                         | Baked links, per-frame data | See section 4                                                                                                                                                                                                                       |
| Player and NPCs                     | **Capsules** from the actor tracking Fluid Field already has (AirSources and WakeSources follow feet, lower legs and hips). Per mesh, cull capsules by its bounding box. Per strand node, measure the distance from the node's world point to each capsule                  | Every frame                 | Exact, works off-screen, about 10 capsules: negligible cost                                                                                                                                                                         |
| Arrows, dropped items, Havok debris | Optional: **camera depth reprojection** (Sea of Thieves). In the texel pass, project the texel's world position into the depth buffer of the previous frame's opaque pass, before the falls draw. If the stored depth is within ±r in front of the sheet, it is an obstacle | Every frame                 | Only for camera-visible occluders and needs the pre-falls depth copy. Use as a supplement to the capsules                                                                                                                           |

**Splashes from coupling:**

-   The momentum removed per node per frame (Δp = q·|ΔV_normal-to-obstacle|) is
    a physically meaningful spray rate.
-   Feed it into the existing GPU spray emitters: rock hits now become dynamic,
    and the player in the falls throws spray.
-   Ihmsen et al. 2012 use the same "kinetic energy x trapped-air or crest
    potential" heuristics for spray and foam generation.

---

## 6. Seams across UV charts and meshes

The options, from simplest:

1.  **Avoid them structurally.** Strands carry s and T across joins, so the
    _texture coordinates_ are seamless even where the _grids_ are not. This is
    the main reason to prefer strands for our case.
2.  **Ghost or halo texels with an indirection table** (Stam 2003; Fluid Flow
    asset).
    -   Pad each chart by 2 texels in the atlas.
    -   On the CPU, from the mesh's edge adjacency, compute for each halo texel
        the (meshId, chart texel coordinate) of the surface point across the
        seam, plus the 2x2 Jacobian between the two charts' UV frames.
    -   A "seam fill" pass copies the scalars, and the vectors through the
        Jacobian, into the halo before each solver pass.
    -   Semi-Lagrangian back-traces that land in the halo just sample it. Long
        back-traces need Stam's `Clip` walk, but at 1-3 texels per step a 2-3
        texel halo suffices.
    -   With all meshes in one atlas or Texture2DArray, cross-mesh seams use the
        same mechanism (meshId = array slice).
3.  **Seam-free parametrisations:** Invisible Seams (Ray et al. 2010), Seamless
    Texture Atlases (Purnomo et al. 2004), Mesh Colors (Yuksel et al. 2010) and
    Ptex. They are correct but need re-parametrisation of the vanilla meshes, so
    they are too invasive. Mentioned for completeness.
4.  **Hide what remains:** foam bands and dithering at geometric seams (D67/D69),
    which we already have.

---

## 7. Rendering on top of any of the simulations

### 7a. Texture coordinates: (s, t - r), not flowmaps

-   **Flowmaps** (Vlachos 2010, Portal 2; Catlike Coding) need two phases and a
    cross-fade, so they pulse or need noise offsets, and they cannot show
    parting. Already rejected in techniques-deep.md.
-   **Advected texture coordinates** (Neyret 2003; Yu, Neyret, Bruneton and
    Holzschuch 2011, _Lagrangian Texture Advection_) need regeneration because
    distortion accumulates without bound in closed flows.
    -   On a fall, the age r is bounded by the travel time, and the inflow edge
        regenerates continuously.
    -   So **colour = T(s·S, (t − r)·V0)** is advected exactly. The phase
        φ = t − r satisfies Dφ/Dt = 0, because Dr/Dt = 1. It is pulse-free.
    -   Features stretch where the water accelerates, which is physically right.
    -   Neyret-style layering is needed only where r or s gradients blow up:
        stagnation in front of rocks and recirculation behind them. Mask those
        with the pillow and wake foam (techniques-deep.md section b).
-   **Precision and mips:**
    -   Keep t wrapped to a multiple of the texture's repeat period.
    -   Compute derivatives from the unwrapped (s, φ). Or pass analytic
        gradients from the gather pass as d(s, φ)/dx, using SampleGrad, so that
        wrap discontinuities never hit the mip selection.
-   **When r changes quickly**, for example the player cuts the sheet: the strand
    sim changes T smoothly, because rows update as the new water arrives. But
    in the Eulerian variant, rate-limit ∂r/∂t to avoid a visible texture "jump".

### 7b. Breaking the vanilla look

-   **Per-strand variation:** hash(k) gives a texture across-offset, a speed
    scale (±10-20%) and a brightness. Streaks then fork and slide at different
    speeds instead of moving as one sheet. This was candidate 1 in the D60
    status report, and is now physically motivated by q_k pulses.
-   **Stochastic or hex-tiled sampling** in (s, φ) space: Heitz and Neyret
    2018; Deliot and Heitz 2019; Mikkelsen 2022 hex-tiling (JCGT), a cheaper
    variant with a contrast ramp in place of histogram preservation. This kills
    repetition along tall falls. Use 3 samples instead of 1. Tiles are fixed in
    (s, φ), so they move with the water and do not swim.
-   **Our own streak and foam layer** in the (s, φ) frame, over vanilla's
    texture, as in techniques-deep (6).

### 7c. Thickness and aeration drive the look

-   **Aeration** a(L), with L the fall distance along the strand:
    -   About 0 near the lip.
    -   Rises past about 0.3·L_b, and saturates by L_b ≈ 6·q^0.32 m (Horeni;
        Castillo's nappe papers give refinements with turbulence intensity).
    -   Add sources: impacts (obstacle Δp), pile-ups, and attached friction on
        rough rock.
-   **Translucency:** Beer-Lambert through thickness h for clear water near the
    lip, blended toward opaque white with a. A thin clear sheet looks dark and
    glassy at the lip, then turns white, which is the classic real-falls
    gradient.
    -   This needs the blended-pass fix from the D53-D56 issues.
    -   Until then, use dithered alpha (TAA-resolved) against the a and h masks.
-   **Breakup:** above a ≈ 0.6, erode the alpha with a strand-aligned noise in
    (s, φ). The curtain becomes fingers and drops. Spawn spray in proportion to
    da/dt.

### 7d. Vertex displacement (we can replace meshes with denser copies)

-   Displace along n̂ by k_h·h, plus a pillow bump in front of obstacles (from
    the pile-up), plus pulse surges (q_k(t) noise carried down). Amplitude: a
    few cm to tens of cm. Even subtle displacement helps silhouettes and specular
    highlights.
-   Write the same displacement in both depth passes, as D57/D69 already do for
    clipping and edges, to avoid depth fighting.
-   Horizon Forbidden West's lesson: vertex density limits features. Malan notes
    "foam popping" with per-vertex foam. Keep foam and aeration in **texture
    space** and use vertices only for low-frequency shape.

### 7e. Mist and spray

The existing GPU spray (D30/D78) gets dynamic emitters from section 5: obstacle
Δp, aeration rate and plunge discharge. Mist density can come from the summed
plunge energy q·V² per site.

---

## 8. Other approaches considered

### 8a. Particle, SPH, FLIP or PBF restricted to the sheet

-   **How it works.** 2D particles live in UV, with Position-Based Fluids density
    constraints (Macklin and Müller 2013) or SPH kernels using the metric.
    Rendered by splatting into the texel textures.
-   **Cost.**
    -   Enough particles to resolve 192² needs about 1 per 2x2 texels, so 9k
        per mesh and 180k in total.
    -   PBF with neighbour grids and 2-4 iterations is about 0.5-1.5 ms on a
        mid GPU.
    -   Plus splatting and reconstruction. Over budget.
-   **Pitfalls.**
    -   Density noise reads as flicker.
    -   Thin parts need many particles.
    -   Texture coordinates still have to come from somewhere: Lagrangian
        texture sprites (Yu et al. 2011) add blending artefacts.
-   **Use instead as:** spray, droplets and finger tips beyond breakup (already
    have them).
-   Thin-film SPH (Wang et al. 2021) and Batty et al.'s Discrete Viscous Sheets
    (2012) are offline-grade.

### 8b. Thin-sheet and 3D Lagrangian sheet methods

-   Discrete Viscous Sheets (Batty, Uribe, Audoly and Grinspun 2012), thin-film
    SPH (Wang, Deng, Kong, Prasad, Xiong and Zhu 2021), and codimensional
    methods for sheets and filaments.
-   These model _free 3D sheet shape_ (sagging, rupture) at minutes per frame.
-   They are not applicable in real time. The useful idea is **thickness as a
    state variable with exact volume conservation**: our q = h·V per strand is
    exactly that.

### 8c. Hybrid heightfield plus 3D (Chentanez and Müller 2010, 2014; Kellomäki)

-   In these, falls are represented by converting heightfield water to particles
    at steep drops or overhangs.
-   The user's requirement explicitly rules out top-down heightfields for
    curtains. The surface-manifold solver (section 3) is the correct analogue.

### 8d. Baked offline simulation, played back (Uncharted 4, Horizon Forbidden West)

-   **Uncharted 4** (Popka, SideFX): flowmaps (direction and intensity in RG,
    wave height in B) from a mix of simulation and hand-drawn curves, plus wave
    particles for rapids (Gonzalez-Ochoa, Advances 2016).
-   **Horizon Forbidden West** (Malan, Advances 2022): Houdini sims baked to
    128-layer deformation volumes, with a shrink-wrap to map them onto arbitrary
    meshes, giving "all the waterfall impacts". Placed by Houdini procedurally.
-   This is high quality but static: no player response. It is our current bake,
    in essence.
-   **Keep the bake as the equilibrium and fallback** for meshes outside
    simulation range.

### 8e. Static bake plus a real-time perturbation layer (the cheapest dynamic option)

-   Keep today's baked (s, r). Add a small per-mesh perturbation for dynamic
    obstacles only, the player:
    -   Per capsule hit, drop an analytic "cut" into a UV-space decal: a gap
        that opens downstream at the flow speed and closes after leaving,
        driven by T(u, v) from the bake.
-   Cost: about 0.01 ms. Easy and robust.
-   It does not make rocks dynamic, and parting stays as baked. It is a good
    _first deliverable_ if the strand sim is delayed. Strands subsume it later.

### 8f. Wave particles or water-surface wavelets on the sheet

-   Yuksel et al. 2007; Jeschke et al. 2018; Skrivan et al. 2020 (Wave Curves).
-   These are for dispersive surface waves on pools and rivers. They are
    irrelevant on curtains, where Fr ≫ 1 and waves are swept away at once.
-   Usable on the pool side (already covered by the shallow-water solver).

### 8g. What shipped games do (summary)

| Game or engine                                            | Waterfalls                                                                                                                                   | Real-time response                 | Source                                       |
| --------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------- | -------------------------------------------- |
| Sea of Thieves (UE4)                                      | Mesh plus **texture-space shallow water (Mei 2007 pipes)**; camera depth projected into texture space for occlusion and foam                 | Yes: characters occlude the stream | Ang et al., SIGGRAPH 2018 Talks              |
| Uncharted 4                                               | Mesh plus Houdini flowmaps and height maps; wave particles for rapids                                                                        | No (baked)                         | SideFX "FX adventures in UC4"; Advances 2016 |
| Horizon Forbidden West                                    | Baked deformation volumes from Houdini sims for impacts; per-vertex foam                                                                     | No (baked loops)                   | Malan, Advances 2022                         |
| Cities: Skylines                                          | Heightfield virtual-pipe-family shallow water; particles for 3D effects                                                                      | Gameplay-scale (dams)              | Kellomäki 2013, 2015                         |
| Unreal (Fluid Flux, Niagara Fluids)                       | 2D shallow water heightfield, "improved on slopes"; Niagara Grid2D shallow-water templates                                                   | Yes, heightfield only              | Epic docs; Fluid Flux                        |
| Unity (Fluid Flow asset)                                  | Texture-space flow on arbitrary meshes, across UV seams                                                                                      | Yes                                | Asset forum thread                           |
| Typical stylised games (League of Legends, Season)        | Layered meshes, scrolling and stretched UVs, World Position Offset, particles                                                                | No                                 | realtimevfx breakdowns                       |
| RDR2, God of War, Ghost of Tsushima, Witcher 3, Far Cry 5 | No public technical detail on waterfall _flow_ found. Far Cry 5's GDC 2018 talk covers waterfall _rendering_ and compositing, not simulation | n/a                                | —                                            |

---

## 9. Ranking for our constraints

The constraints: real time, 0.3-0.5 ms in total, vanilla meshes kept, whole
fall including vertical parts, texture-space data that the existing shaders
already consume.

| Rank  | Approach                                                                                                                      | Realism gain                                                                                           | GPU cost (worst / typical)                          | Invasiveness                          | Risk                                                                               |
| ----- | ----------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ | --------------------------------------------------- | ------------------------------------- | ---------------------------------------------------------------------------------- |
| **1** | **Lagrangian strand sheet** (section 4) plus coupling (section 5) plus rendering (section 7)                                  | Dynamic parting, open wakes, thinning, pile-ups, pulses, player cuts, seamless s and T across cascades | 0.1 / 0.03 ms (plus about 0.05 ms rendering extras) | Low: same bake slot and shader inputs | Medium: strand-side choice at obstacles; monotone-v assumption (holds for vanilla) |
| **2** | **Texture-space manifold shallow water** (section 3; Sea of Thieves plus Wang-Miller-Turk), reusing the pool solver's kernels | General: also flat ledges, merges and eddies                                                           | 0.4 / 0.12 ms                                       | Medium: atlas, ghost cells, metric    | Medium-high: diffusion of s, CFL substeps, wet/dry                                 |
| **3** | **Bake plus real-time perturbation decals** (8e), with the section 7 rendering upgrades                                       | Player response only; rocks stay static                                                                | 0.02 / 0.01 ms                                      | Lowest                                | Low                                                                                |

Why strands win:

-   They are the exact structure of supercritical sheet flow.
-   They deliver the (s, r) coordinates the shader already uses with zero
    numerical diffusion.
-   Thickness comes for free from q and spacing.
-   They handle seams by label continuity rather than grid stitching.
-   They cost about a tenth of an Eulerian solver.

The Eulerian solver remains the upgrade path if we ever need flow that is not
along v.

---

## 10. Concrete plan for the strand sheet

Each stage is built and tested in one round, bundled as the user prefers.

**Stage 1: data (CPU, at load; extends the D59/D67 bake)**

1.  Per lit falls mesh, using the existing texel grid, store the positions P,
    normals, tangent frame, attachment, and the rock "through" mask versus the
    "behind/under" mask (from ray parity).
2.  Build the **obstacle SDF** in texels (CPU, or jump flooding on GPU once).
3.  Choose K (64-128, proportional to mesh width in world units, about 1 strand
    per 15-20 cm). Initialise the strands from the current steady bake:
    U_k(j) = the level set s = k/K of the baked streak coordinate, and T_k(j)
    from the baked run time. Equilibrium is then already the bake, and the
    first frame matches today's look.
4.  Set q_k from the baked discharge, or uniform × the site's flow estimate.
5.  Cascade links: for each mesh whose bottom row lands on another mesh's top
    (D67), map strand k to the receiving mesh's across coordinate.

**Stage 2: GPU strand update (one compute dispatch, all meshes)**

-   Use one thread per strand node, or one thread group per (mesh, strand
    block) looping rows top-down for the steady pre-pass.
-   The time-dependent update is the semi-Lagrangian step of section 4:
    -   Read the previous frame's strand buffer at back-traced row j\*.
    -   Apply tangential gravity and the obstacle SDF push.
    -   Apply the ordering constraint and cohesion relaxation in 2-3 Jacobi
        sweeps across k.
    -   Compute T, h and a.
-   Cascade receivers read the senders' last row from the previous frame. The
    one-frame lag is invisible.
-   Dynamic obstacles: actor capsules, as a structured buffer of up to about 16
    world-space capsules, are tested per node against the node's world point.
    The node's world point comes from bilinear P at (U, v_j).
-   Sources:
    -   q_k(t) modulated by 2-octave smooth noise per strand (period 0.5-3 s,
        ±15%).
    -   The lip speed from the site's flow.

**Stage 3: gather (one dispatch)**

-   For each texel, binary-search the strands in its row and write (s, T, h, a)
    to the same RGBA16F t124 slot the lit and depth shaders read.
-   Also write a coverage or gap value (alpha) and the momentum-loss rate for
    spray, either packed or in a second small texture.

**Stage 4: shading (lit plus both depth passes)**

1.  Sample at (s, t − T) with per-strand hash offsets and speed, and hex-tiled
    3-tap stochastic sampling.
2.  Aeration and thickness drive the colour, the whiteness and (when the
    blended pass is fixed) the translucency. Use dithered alpha until then.
3.  Gap and breakup alpha erosion: clip in both depth passes, as D57 does.
4.  Optional displacement along n̂ from h and the pile-up, in both depth passes.

**Stage 5: coupling outputs**

-   Momentum loss at obstacles and aeration rate feed the spray emitters.
-   The strand's end at the pool surface feeds the plunge emitter and foam seed
    in the pool solver, giving one foam look across the seam.

**Budget check, worst case of 20 meshes near the camera:**

| Item                                         | Cost                                                   |
| -------------------------------------------- | ------------------------------------------------------ |
| Strand update                                | About 0.05 ms                                          |
| Gather                                       | About 0.05 ms                                          |
| Extra shading taps (3-tap hex plus aeration) | About 0.05-0.1 ms, depending on screen coverage        |
| **Total**                                    | **About 0.15-0.2 ms**, inside 0.3-0.5 ms with headroom |

-   LOD: far meshes use K/2 strands and every second row, or freeze and use the
    bake.

**Validation, judged in motion as before:**

-   Fall `0001AC75` (rock pillar): visible split and gap below the pillar.
-   The player walking into a curtain: a gap that opens downward and heals from
    the top.
-   A cascade: continuous streaks and run time across meshes.
-   A tall fall: clear at the lip, white fingers by the bottom.
-   The log line and debug view: a "strands" mode drawing U_k(j) lines over the
    mesh.

**Pitfalls to watch:**

-   Strands bunching permanently against a rock face. Add a small restoring
    force toward the baked equilibrium U_k^bake(j) with a time constant of about
    2-5 s.
-   Side-choice flicker for strands exactly on an obstacle's centreline. Add
    hysteresis: keep last frame's side.
-   Rows where v is not strictly downstream, for example the arch top. Use
    the run time T rather than the row as the march variable there, or clamp
    the along speed to at least a minimum.
-   T precision. Store T relative to the lip, in seconds, in fp32 in the strand
    buffer and fp16 in the texture: fine for falls under about 60 s of travel.
-   Through versus under rock misclassification. It shows as parting where
    water should flow over. Expose a debug view.

---

## References

Cities: Skylines, virtual pipes, heightfield shallow water

-   Kellomäki, _Large-Scale Water Simulation in Games_, PhD thesis, TUT 2015:
    https://trepo.tuni.fi/handle/10024/115052 and
    https://researchportal.tuni.fi/fi/publications/large-scale-water-simulation-in-games/
-   Kellomäki, _Interaction with Dynamic Large Bodies in Efficient, Real-Time
    Water Simulation_, WSCG 2013:
    https://dspace.zcu.cz/items/c7a1f93b-4705-4731-8b6e-1e13558a1804
-   Habrador blog (links the thesis to the Cities: Skylines water work):
    https://blog.habrador.com/2019/04/fluid-simulations-unity-sprinkle-game.html
-   Mei, Decaudin and Hu, _Fast Hydraulic Erosion Simulation and Visualization
    on GPU_, PG 2007:
    http://www-evasion.imag.fr/Publications/2007/MDH07/FastErosion_PG07.pdf
-   Jako, _Fast Hydraulic and Thermal Erosion on the GPU_, CESCG 2011:
    https://www.old.cescg.org/CESCG-2011/papers/TUBudapest-Jako-Balazs.pdf
-   O'Brien and Hodgins, _Dynamic Simulation of Splashing Fluids_, Computer
    Animation 1995 (the origin of virtual pipes).
-   lisyarus, _Simulating water over terrain_ (a clear modern write-up of the
    pipe model and its pitfalls):
    https://lisyarus.github.io/blog/posts/simulating-water-over-terrain.html
-   Chentanez and Müller, _Real-time Simulation of Large Bodies of Water with
    Small Scale Details_, SCA 2010:
    https://diglib.eg.org/items/d0320015-4b07-416b-8f41-047485c9f7f3
-   Chentanez, Müller and Kim, _Coupling 3D Eulerian, Heightfield and Particle
    Methods..._, SCA 2014:
    https://diglib.eg.org/handle/10.2312/sca.20141117.001-010

Flow on surfaces and manifolds

-   Wang, Miller and Turk, _Solving General Shallow Wave Equations on Surfaces_,
    SCA 2007:
    https://faculty.cc.gatech.edu/~turk/paper_pages/2007_shallow_waves/index.html
-   Stam, _Flows on Surfaces of Arbitrary Topology_, SIGGRAPH 2003:
    https://wdv.com/Aerospace/Fluids/StamSurfaceFlow.pdf and
    https://history.siggraph.org/?p=115925
-   Carvalho, Andrade and Velho, _Fluid Simulation on Surfaces in the GPU_,
    SIBGRAPI 2012.
-   Jung and Behr, _GPU-based Real-time On-surface Droplet Flow in X3D_, 2009:
    https://diglib.eg.org/items/f033df7f-9825-46c0-86f7-3c7cb9f0c3ef/full
-   Tatarchuk and Isidoro, _Artist-Directable Real-Time Rain Rendering in City
    Environments_, 2006:
    https://advances.realtimerendering.com/s2006/Tatarchuk-Rain.pdf
-   Fluid Flow (Unity asset, texture-space flow across UV seams):
    https://discussions.unity.com/t/updated-fluid-flow-2-5-realtime-flow-simulation/743300
-   Savage-Hutter bed-normal shallow flow on steep curved beds (Gray, Wieland
    and Hutter, JFM 1999):
    https://personalpages.manchester.ac.uk/staff/nico.gray/Papers/JFM_392_1999.pdf

Supercritical flow and space marching

-   Glaister, _A shock capturing scheme for steady, supercritical, free-surface
    flow_: https://centaur.reading.ac.uk/15636
-   Jimenez and Chaudhry, _Computation of Supercritical Free-Surface Flows_,
    J. Hydraul. Eng. 1988:
    https://ascelibrary.com/doi/10.1061/%28ASCE%290733-9429%281988%29114%3A4%28377%29

Nappe breakup and aeration

-   Ervine and Falvey 1987, _Behaviour of turbulent water jets in the
    atmosphere and in plunge pools_, Proc. ICE (breakup and turbulence).
-   Castillo et al., rectangular nappe breakup, spread and thickness
    estimators: https://www.upct.es/hidrom/publicaciones/congresos/ (several
    papers), and _Air-Water Properties in Rectangular Free-Falling Jets_,
    Water 2021: https://doi.org/10.3390/w13111593
-   _Experimental Study of the Breakup of a Free-Falling Turbulent Water Jet in
    Air_, J. Hydraul. Eng.:
    https://ascelibrary.com/doi/10.1061/%28ASCE%29HY.1943-7900.0001188

Shipped games

-   Ang, Catling, Ciardi and Kozin, _The Technical Art of Sea of Thieves_,
    SIGGRAPH 2018 Talks:
    https://history.siggraph.org/wp-content/uploads/2022/09/2018-Talks-Ang_The-Technical-Art-of-Sea-of-Thieves.pdf
-   Malan, _Real-time Water Rendering in Horizon Forbidden West_, Advances 2022:
    https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Water-Malan.pdf
-   SideFX, _FX Adventures in Uncharted 4_:
    https://www.sidefx.com/community/fx-adventures-in-uncharted-4-a-thiefs-end
-   Gonzalez-Ochoa, _Rendering Rapids in Uncharted 4_, Advances 2016:
    https://advances.realtimerendering.com/
-   Gonzalez-Ochoa, _Water Technology of Uncharted_, GDC 2012:
    https://www.slideshare.net/slideshow/water-technologyofunchartedgdc2012/12977478
-   _Water Rendering in Far Cry 5_, GDC 2018:
    https://80.lv/articles/the-gdc-talk-water-rendering-in-far-cry-5
-   _How we made the waterfall in Season_:
    https://realtimevfx.com/t/how-we-made-waterfall-in-season-a-letter-to-the-future/23420
-   _Summoner's Rift waterfall breakdown_:
    https://realtimevfx.com/t/summoners-rift-water-fall-break-down/1775
-   Unreal Engine fluid simulation (Niagara Fluids):
    https://dev.epicgames.com/documentation/en-us/unreal-engine/fluid-simulation-in-unreal-engine---overview
-   Fluid Flux (UE shallow water):
    https://80.lv/articles/fluid-flux-a-cool-water-simulation-system-for-unreal-engine/

Texture advection and stochastic texturing

-   Neyret, _Advected Textures_, SCA 2003:
    https://diglib.eg.org/items/c4bf80cc-23f0-4107-958d-e03c0b9c8ac4/full
-   Yu, Neyret, Bruneton and Holzschuch, _Lagrangian Texture Advection_, TVCG 2011.
-   Vlachos, _Water Flow in Portal 2_, SIGGRAPH 2010:
    https://alex.vlachos.com/graphics/Vlachos-SIGGRAPH10-WaterFlow.pdf
-   Heitz and Neyret, _High-Performance By-Example Noise using a
    Histogram-Preserving Blending Operator_, HPG 2018.
-   Deliot and Heitz, _Procedural Stochastic Textures by Tiling and Blending_,
    GPU Zen 2, 2019.
-   Mikkelsen, _Practical Real-Time Hex-Tiling_, JCGT 2022:
    https://jcgt.org/published/0011/03/05/

Particles, sheets and whitewater

-   Macklin and Müller, _Position Based Fluids_, SIGGRAPH 2013.
-   Ihmsen, Akinci, Akinci and Teschner, _Unified Spray, Foam and Bubbles for
    Particle-Based Fluids_, CGI 2012:
    https://cg.informatik.uni-freiburg.de/publications/2012_CGI_sprayFoamBubbles.pdf
-   Batty, Uribe, Audoly and Grinspun, _Discrete Viscous Sheets_, SIGGRAPH
    2012: https://www.cs.columbia.edu/cg/DVS
-   Wang, Deng, Kong, Prasad, Xiong and Zhu, _Thin-Film Smoothed Particle
    Hydrodynamics Fluid_, SIGGRAPH 2021:
    https://ar5iv.labs.arxiv.org/html/2105.07656
-   Wretborn, Flynn and Stomakhin, _Guided Bubbles and Wet Foam for Realistic
    Whitewater Simulation_, SIGGRAPH 2022:
    https://history.siggraph.org/?p=117891
-   Yuksel, House and Keyser, _Wave Particles_, SIGGRAPH 2007.
-   Jeschke et al., _Water Surface Wavelets_, 2018. Skrivan et al., _Wave
    Curves_, 2020:
    https://pub.ista.ac.at/group_wojtan/projects/2020_Skrivan_WaveCurves/wave_curves_2020.pdf

Seams

-   Ray, Nivoliers, Lefebvre and Lévy, _Invisible Seams_, EGSR 2010:
    https://diglib.eg.org/handle/10.2312/CGF.v29i4pp1489-1496
-   Purnomo, Cohen and Kumar, _Seamless Texture Atlases_, SGP 2004.
-   Yuksel, Keyser and House, _Mesh Colors_, TOG 2010:
    https://www.cemyuksel.com/research/meshcolors/meshcolors_SIGGRAPH2010.pdf
