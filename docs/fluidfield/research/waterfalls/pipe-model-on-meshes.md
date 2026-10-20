# A pipe-model (Cities: Skylines style) water simulation on the waterfall meshes themselves

**Question.** Can we run a real-time simulation of the whole fall, curtains included, in each falls mesh's own texture space? The idea is "Cities: Skylines water, constrained to the vanilla waterfall meshes". The water should interact with rocks, terrain, the pools below and the neighbouring falls meshes. The budget is under 0.3 ms GPU for about 20 nearby falls meshes.

**Short answer.** Yes. A metric-aware virtual-pipe / shallow-water solver on the mesh's UV grid works on the real bakes (FluidField_bake0..17):

-   **It is stable.** Every run finished with no blow-ups, including dt = one frame.
-   **Water parts round rocks.** It piles into pillows upstream of them and leaves dry wakes behind.
-   **It speeds up on steep parts** and thins as it speeds up.
-   **It conserves mass exactly** with seepage off.
-   **It reaches steady state in about 4-6 s** of sim time from dry.
-   **The recommended configuration** is half resolution (about 14 units per texel), one substep per frame, and semi-Lagrangian momentum advection. That matches the full-resolution, converged result within about 3% on throughput and speed, and is estimated at **about 0.05-0.1 ms GPU for 20 meshes**.
-   **Full resolution needs 2 substeps.** It is estimated at about 0.15-0.35 ms for 20 meshes, so keep it for the nearest few.

Two things need extra care, and both are design decisions rather than flaws:

1. **On a vertical curtain the hydrostatic pressure is zero.** Without an extra "sheet stiffness" term, water hitting a rock on a vertical face piles up without limit (h max 1354 units vs 55 with the term).
2. **Inflow.** Every dumped mesh rises along v for its first 40-70% before the lip, so the inflow has to be placed at the crest (see section 5.2).

Prototype: `scratchpad/pipesim.py` (plus `pipesim_psi.py`). Plots are `scratchpad/pipesim_*.png`. The scratchpad is `C:\Users\victa\AppData\Local\Temp\claude\c--dev\d607eb62-cbde-467b-ada7-a91a434086b8\scratchpad\`.

---

## 1. What Cities: Skylines (2015) actually does

### Verified

-   **It is a variant of the virtual pipe method.** Kellomäki's PhD thesis (Tampere 2015, p.5 and section 4.5) says:

    > The results of [P1] and discussions with the game company Colossal Order led them to use a variant of the pipe method in their hit game Cities: Skylines.

    [K15] This is the only primary statement of the algorithm that could be found. Colossal Order "worked with a research scientist on this feature" [PCW].

-   **The grid is 1081 × 1081 cells** (`TerrainManager.RAW_RESOLUTION + 1`), with **16 m cells** over a 17.28 km map (origin -8640 m).

    -   Water state lives in a double-buffered `WaterSimulation.Cell[][] m_waterBuffers`.
    -   The stable buffer is `buffers[~(m_waterFrameIndex >> 6) & 1]`, so the buffers swap every 64 frame-index ticks. The simulation runs on the simulation thread, decoupled from rendering.
    -   `Cell.m_height` is the water **depth** as a ushort in 1/64 m.
    -   These facts come from two mods that read the internals by reflection [VEL] [BSP].

-   **Water sources are explicit sources and sinks with a target surface height.** They live in `m_waterSources`, with fields m_inputRate, m_outputRate, m_target and m_water.

    -   A source below the water level acts as a drain [RAIN] [CSWIKI].
    -   The sea is a fixed level at the border that absorbs any inflow ("no matter how many rivers flow into the sea") [CSWIKI].
    -   This is the same Dirichlet head boundary as our "reservoir at the lip" (section 5.2).

-   **Rendering:** foam is driven by movement (the red channel of the foam texture is "foam when the water is moving"). Normals cross-fade with a flipped and rotated copy rather than being flow-advected [CSLMOD].

-   **Cities: Skylines II** moved the solver to GPU compute.
    -   Kernels: Velocity, Depth, Add, Evaporate, …, FlowPostProcess.
    -   Parameters: m_Damping, m_Fluidness, MaxVelocity, …
    -   It derives a **render flow map from the sim velocity, downsampled and blurred** [CS2MOD].
    -   The editor lets you run 0-128 sim iterations per frame [CS2WIKI].

### Not verified (no public decompile found)

-   The rest of `Cell` (presumably velocity and pollution).
-   The exact update equations, sim rate and cost.
-   Players report sloshing and "tsunamis" after shoreline edits [STEAM]. That is consistent with an explicit, lightly damped pipe model.

### Why the pipe model was robust enough for a city builder

These reasons come from the literature behind it [MDH07] [ST08] [K15]:

-   **One flux per face, explicit update.** A cell can only lose what it holds: Mei's scale factor K = min(1, d·lx·ly / (Σf_out·Δt)). So depth never goes negative and the scheme never blows up, only damps.
-   **No advection term.** That makes it the linearised SWE. There is nothing to go unstable at high Froude number, and Kellomäki's user study found no significant perceived difference from full SWE [K15].
-   **Fixed-head sources and sinks**, and a very coarse cell (16 m) relative to wave speeds.
-   **Cost is a 5-point stencil per cell per step**, so it is bandwidth-bound:

| Source                                      | Grid                | Time per step |
| ------------------------------------------- | ------------------- | ------------- |
| Kellomäki [K15]                             | 65k cells, Δt 25 ms | 0.55 ms       |
| Šťava et al. [ST08]                         | 512²                | 0.57 ms       |
| Mei et al. [MDH07] (8800 GTX, incl. render) | 256²                | about 1.3 ms  |

### Lessons that carry over

-   **One signed flux per face.**
-   **A donor limiter.**
-   **Upwind cross-section A = h_up·W** (Kellomäki: "varying A acts as a viscosity control").
-   **Fixed-level boundaries.**
-   **fp32 state.** Šťava: "16-bit precision may result in serious issues with water flow."

What does **not** carry over: C:S is a top-down heightfield. Our D24 attempt showed that a heightfield cannot hold near-vertical drops. Chentanez & Müller [CM10] also special-case "waterfall faces" (slope > 3) because steep terrain destabilises SWE. Running the same model **in the sheet's own 2D frame** avoids that entirely: a curtain is just a steep slope in its own coordinates.

---

## 2. The model on a curved sheet (equations as prototyped)

### Geometry per texel

All of this comes from the bake (position P per texel, row-major, v downstream):

| Quantity                                     | Definition                                                                                                    |
| -------------------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| a_u, a_v                                     | Tangents: central differences of P along x and y (world units per texel)                                      |
| A_i = \|a_u × a_v\|                          | Texel area                                                                                                    |
| c_i = \|n_z\|                                | "Contact": the gravity component pushing the water against the bed. 1 on flat ground, 0 on a vertical curtain |
| G = [[a_u·a_u, a_u·a_v], [a_u·a_v, a_v·a_v]] | Metric; its inverse is kept for advection                                                                     |

Per face f, between texels i and j along x or y:

| Quantity            | Definition                                                                       |
| ------------------- | -------------------------------------------------------------------------------- |
| L_f = \|P_j − P_i\| | Pipe length                                                                      |
| W_f                 | Pipe width: the mean cross-texel size (\|a_v\| for u-faces, \|a_u\| for v-faces) |
| Δz_f = z_j − z_i    | Height difference                                                                |
| c_f                 | Mean of the two texels' contact                                                  |

A face is open only if both texels are water (covered and not rock) and L_f < 12× the median texel size. The cut-off catches UV-island jumps.

> **Measured gotcha:** on vertical parts the UV is stretched to about 25 units per texel along v. A 3× threshold closed every face of the curtain.

The texel skew on these meshes is moderate: median |cos(a_u, a_v)| is 0.16-0.19 and the 90th percentile is about 0.5. Pipes therefore stay on the grid axes; the skew only enters the advection, through G⁻¹.

### State

-   **h_i:** sheet thickness along the normal, in world units, per texel.
-   **u_f:** signed world speed along each pipe, staggered like a MAC grid. U is (H, W+1) and V is (H+1, W).

### One substep (Δt)

**1. Momentum advection** (the "sw" mode; skipped in the classic "pipe" mode). Semi-Lagrangian, on the staggered faces, in texel coordinates:

```
cov_u = |a_u|·ū_u ,  cov_v = |a_v|·ū_v          (cell-centred pipe speeds -> covariant components)
ẋ = G⁻¹ [cov_u, cov_v]                           (texels/s)
u_f* = u^n( x_f − Δt·ẋ(x_f) )                     (bilinear)
```

**2. Gravity along the pipe plus sheet pressure:**

```
u_f ← u_f* − Δt/L_f · [ g·Δz_f + g·(c_f + κ)·(h_j − h_i) ]
```

The term g·Δz/L is exactly the tangential gravity along the pipe, so a vertical curtain is "a slope of 1" in its own frame. g(c + κ)h is the depth-averaged pressure:

-   **c·g·h** is the hydrostatic part. It is zero on a vertical curtain.
-   **κ·g·h** is an artificial "sheet stiffness". It stands in for dynamic pressure, surface tension and splash, which make real water part round an obstacle on a vertical face. Default κ = 4.

**3. Friction**, implicit:

```
u_f ← u_f / (1 + Δt·[ c_f·(k₁ + k₂·|u_f| / max(h_up, 0.5)) + g·|u_f| / v_T² ])
```

-   The bed drag is scaled by contact c, because a free-falling curtain has no bed.
-   The quadratic air drag has terminal speed v_T = 1000 u/s.
-   k₁ = 0.2/s and k₂ = 0.002.

**4. Speed clamp** (Chentanez & Müller [CM10]): |u_f| ≤ α·L_f/Δt with α = 0.5. Faces that are walls (rock, uncovered, actor) are set to u = 0. A face is also zeroed if its upwind h is below 1e-3 (dry).

**5. Flux with an upwind cross-section and a depth cap:**

```
Q_f = u_f · W_f · min(h_up, h_cap,f),   h_cap,f = (β·L_f/Δt)² / (g·(c_f + κ)),   β = 0.5
```

The cap is Chentanez & Müller's "h_avgmax" idea, applied to the mass flux. It keeps the gravity-wave Courant number at or below β even at dt = 1 frame. Thick water still feels its full pressure; it just cannot empty faster than the wave CFL allows.

**6. Donor limiter** (Mei [MDH07]):

```
K_i = min(1, h_i·A_i / (Δt·Σ_out Q))
Q_f ← Q_f · K_donor(f)
```

**7. Continuity:**

```
h_i ← max(0, h_i + Δt/A_i · Σ_f ±Q_f)
```

Optional seepage: h /= 1 + Δt·λ where the sheet is stagnant (< 40 u/s). This drains closed basins in the mesh; see the open problems.

**8. Boundaries**

-   **Reservoir texels** (upstream of the crest, section 5.2) are Dirichlet h = H_in, with the bed flattened to the crest height.
-   **The lip faces** get the river speed V_in. It is backed off as the texel below backs up, otherwise dead-end pockets fill without limit (seen: h = 217 586).
-   **Foot texels** (last covered texel of each column in the bottom band, plus foot-band edge texels) are sinks. The volume they drain per texel is the **plunge-emitter feed** for the existing shallow-water solver.

### Time step and speeds

Fall speeds reach 500-900 u/s on 4-11 unit texels, but the vertical parts have long texels along v (about 25 units). Advective CFL at full resolution therefore needs 2-7 substeps per frame. With the clamp and the depth cap, **1 substep is stable** and 2 is converged (section 4).

### Why advection (the "sw" mode) instead of the classic pipe model

Without the (u·∇)u term, the classic pipe model has no momentum. Its steady speed is the _local_ balance of slope against friction, not √(2gΔz). Because the bake's slopes are piecewise constant per (low-poly) triangle, its speed field shows the triangle facets as streaks (`pipesim_pipe_3.png`, `pipesim_pipe_8.png`). Advection carries momentum across the facets, gives smooth acceleration down the drop, and makes jets past rocks.

The classic pipe model is about 3× cheaper in numpy (≈ 1.5× on GPU) and drains about 2.5× more for the same inflow, because it has no inertia at the lip (no weir limit). It is acceptable as a fallback.

### Free fall and overhangs

Ignored on purpose. **A vanilla falls mesh _is_ the artist's trajectory of the falling water**, so the water stays attached to it by construction. Detachment matters only for spray: we already emit spray where the sheet is fast and thin.

### Alternatives considered

-   **Chentanez & Müller 2010 SWE [CM10].** This is essentially what the "sw" mode is: staggered grid, upwind h, velocity clamp, depth cap. It adds MacCormack advection, overshoot reduction for steep fronts, and splash particles at breaking fronts (|∇η| > 0.45·Δx/(gΔt)). These are worth borrowing for foam and spray triggers. Cost: 0.75-2.2 ms per step on a GTX 480 for about 128²-256² grids.
-   **Restricted tall-cell grid [CM11]** and the 3D, heightfield and particle hybrid [CMK14]. These are 3D Eulerian and far over budget. Their 512² SWE component costs 0.62 ms on a 780 Ti, which is a useful per-cell reference for us (section 6).
-   **Lattice-Boltzmann SWE (D2Q9, Zhou) [OS13].**
    -   GPU-friendly: one gather, no limiter.
    -   It is **restricted to subcritical flow, Fr = |u|/√(gh) < 1**. Our curtains have Fr ≈ 600/√(686·5·2) ≈ 7 (even counting κ), so LBM needs Froude capping everywhere that matters. Rejected.
-   **Wang, Miller & Turk 2007, SWE on general surfaces [WMT07].** They build columns along surface normals and split gravity into an implicit normal part g_i = −g·N and a tangential acceleration. That is the same split we use (c and Δz/L). They use an irregular mesh and an implicit solve, whereas our UV grid lets us stay explicit.
-   **Our earlier D74 projection solver (`simbake.py`).** It makes the 2D velocity divergence-free. That is wrong for a free-surface sheet, which **thins** as it accelerates rather than spreading. The pipe model gets thinning for free (h·u = q), and it gives a thickness to render.

---

## 3. Coupling

-   **Rocks.** A rock texel is a wall: its faces have u = 0. Results:

    -   Water parts round the rock.
    -   It piles into a bow "pillow" upstream. The pillow's height is set by κ: h max 55 at κ = 4 and 25 at κ = 12.
    -   It leaves a dry wake.

    See `pipesim_15.png` (best example), `pipesim_0.png` and `pipesim_3.png`.

-   **Inflow from the river: reservoir + lip.** The texels upstream of the crest are held at the river level and the lip faces get the river speed. In game, the reservoir level should be the **actual upstream water plane** (river or intermediate pool), not crest + H_in.

-   **Cascades go through pools.** In the dumps, cascading meshes do **not** meet edge to edge. The upper mesh's lower curtain hangs over the lower mesh's rising "tongue": bake3 rows 164-174 overlap bake0 rows 9-90, and 59% of bake0's touching texels are in its reservoir zone.

    -   So the hand-off is: upper sheet foot sinks → **0-D pool node** (volume → level) → lower sheet's reservoir level.
    -   In the prototype (`multi 2 3 4 0 "3>0" "4>0"`) the pool reaches balance: fill 1.17e6 u³/s, spill 1.22e6 u³/s. Bake0 goes from dry to 97% wet, fed only by bakes 3 and 4 (`pipesim_multilowleak_2-3-4-0_0.png`).
    -   In game the pool _is_ the water body under the falls. Its level is the water plane, and the existing shallow-water solver gets the foot fluxes as plunge emitters.
    -   Direct portals between cascade pairs made a loop (lower tongue → upper foot → pool → lower) and were removed.

-   **Side by side: portal pipes.** Each mesh-border texel of sheet A is linked to the nearest water texel of sheet B within 2.5× the texel size, in world space. The link is an ordinary pipe (gravity + pressure, donor-limited) solved in the same substep over a flat concatenation of all sheets, which is what a shared atlas gives on the GPU.

    -   Site 2-3-4 had 86 links and carries 1e4-4e4 u³/s across.
    -   **Finding:** neighbouring meshes **overlap heavily**. Bakes 2 and 3 share about 4000 texels within 25 units, and 6/7 and 8/11 are similar. So edge portals are not enough to make overlapping copies agree; see open problems.

-   **Player and actors.** The actor is a disc of radius 40 units whose texels close their faces for the frame. `pipesim_actor8.png` shows an actor crossing the lower curtain at about 400 u/s. It leaves a slow-water wake carried downstream, with a weak thickness response, and the sim stays stable.
    -   For a visible splash, also add an impulse: push h out of the disc (h → 0 inside, the removed volume added to the ring) and emit spray particles from the removed volume.

---

## 4. Prototype results (`pipesim.py`, real bakes)

Common setup:

| Parameter | Value                                            |
| --------- | ------------------------------------------------ |
| g         | 686 u/s²                                         |
| H_in      | 6 u                                              |
| V_in      | 250 u/s (prescribed at the lip)                  |
| κ         | 4                                                |
| Friction  | k₁ = 0.2/s, k₂ = 0.002                           |
| v_T       | 1000 u/s                                         |
| Seepage   | λ = 0.5/s                                        |
| Run       | 10 s from dry; steady = h changes < 1% per 0.1 s |

"Sim texels" excludes rock, reservoir and uncovered texels. Speed is world speed in u/s.

### Full resolution, 2 substeps per frame (dt = 8.3 ms): plots `pipesim_N.png`

| bake | grid    | sim texels | steady at | wet  | h median / max | speed median / p99 | foot drain (u³/s) |
| ---- | ------- | ---------- | --------- | ---- | -------------- | ------------------ | ----------------- |
| 0    | 142×184 | 9976       | 5.6 s     | 0.97 | 2.3 / 32       | 257 / 441          | 2.1e5             |
| 3    | 192²    | 9255       | 5.1 s     | 0.76 | 1.3 / 55       | 366 / 672          | 3.8e5             |
| 5    | 192²    | 8189       | 4.7 s     | 0.80 | 2.1 / 43       | 319 / 618          | 6.2e5             |
| 8    | 192²    | 14435      | 5.7 s     | 0.97 | 1.8 / 32       | 255 / 487          | 9.1e5             |
| 13   | 192²    | 4200       | 3.7 s     | 0.69 | 2.1 / 30       | 271 / 421          | 3.8e5             |
| 15   | 192²    | 18595      | 6.3 s     | 0.89 | 1.6 / 32       | 331 / 576          | 6.7e5             |

### Half resolution (2×2 texels, about 14 u/texel), 1 substep (dt = 16.7 ms): plots `pipesim_half_N.png`

| bake | grid  | sim texels | steady at | wet  | h median | speed median / p99 | drain (u³/s) | vs full res |
| ---- | ----- | ---------- | --------- | ---- | -------- | ------------------ | ------------ | ----------- |
| 0    | 71×92 | 2503       | 5.1 s     | 0.97 | 2.5      | 255 / 434          | 2.2e5        | +8%         |
| 3    | 96²   | 2282       | 3.8 s     | 0.74 | 1.2      | 353 / 657          | 3.7e5        | −1%         |
| 5    | 96²   | 2003       | 4.1 s     | 0.79 | 2.2      | 315 / 611          | 6.0e5        | −4%         |
| 8    | 96²   | 3599       | 5.0 s     | 0.96 | 1.8      | 256 / 477          | 9.3e5        | +2%         |
| 13   | 96²   | 1017       | 3.4 s     | 0.71 | 2.0      | 270 / 418          | 3.8e5        | 0%          |
| 15   | 96²   | 4590       | 5.7 s     | 0.89 | 1.6      | 330 / 549          | 7.0e5        | +4%         |

### Stability, convergence and variants (bake 3 / bake 8)

-   **No run ever produced NaN or Inf.**
-   **Full resolution, 1 substep (dt = 16.7 ms)** is stable and within 1-10% on drain. Before the depth cap it showed odd-even checkerboarding in thick ponds, and h max reached 11 000 when the cap was put on the pressure instead of the flux. With the per-face flux cap it converges (steady 6.7 s, bake 8), with a faint checker left in the thickest edge trough (`pipesim_sub1_8.png`). Two substeps are clean (`pipesim_sub2_8.png`).
-   **κ = 0** (hydrostatic only): water piles without limit at rocks on vertical parts, h max 1354 (`pipesim_stiff0_3.png`). κ = 4 gives h max 55 and κ = 12 gives 25 (`pipesim_stiff12_3.png`).
-   **Classic pipe, no advection** (`pipesim_pipe_3.png`, `pipesim_pipe_8.png`): speed median 556 vs 366 for "sw", drain 2.5× higher, and the triangle-facet streaks described in section 2.
-   **Mass audit, seepage off** (half resolution, 10 s):

    | bake | injected (u³) | drained + held (u³)      |
    | ---- | ------------- | ------------------------ |
    | 8    | 1.08e7        | 6.4e6 + 4.38e6 = 1.078e7 |
    | 3    | 5.06e6        | 2.60e6 + 2.46e6 = 5.06e6 |

    This is exact to rounding.

-   **With seepage λ = 0.5/s**, about 25% of the injected water seeps away from stagnant edge troughs: too much, use about 0.05. Foot drain rates barely change.
-   **Wall-clock time in numpy** (CPU, one thread, not representative of GPU): "sw" 10 ms per step at 192², 2.8 ms at 96²; "pipe" 3 ms per step at 192².

### What it looks like

`pipesim_15.png` is the best illustration:

-   Water enters at the lip and fans out.
-   White pillows form upstream of each boulder, and dry wakes and shadows sit behind them.
-   Streamlines bend round rocks and re-converge.
-   Speed rises from about 200 to 500-600 u/s down the drop while the sheet thins (h about 1-2 units on the drop).
-   Edge troughs carry thick, slower water.

In bake0 coupled to bakes 3 and 4 (`pipesim_multilowleak_2-3-4-0_0.png`), the flow converges into the chute between the two big rocks and accelerates through the narrows.

`pipe_geom_0.png` and `pipe_geom_3.png` show z and |n_z| per texel: the rising tongue, then the drop.

---

## 5. Design recommendation

### 5.1 Where it runs

-   **Shared atlas.** Use one R32 h texture plus R32 U and V face textures (or one RGBA32F with h, U, V and a spare channel, with U and V on the texel's "left" and "top" faces) for all active falls meshes. Each mesh has its own tile at its bake's grid, downsampled 2×.
-   **Static per-texel geometry**, computed once per mesh from the PN-subdivided mesh, ideally fp16:
    -   Δz_u, Δz_v, L_u, L_v, W_u, W_v, c_u, c_v, A
    -   G⁻¹ (3 values)
    -   Flags: water, rock, reservoir, sink, portal
-   **Portal links** go in a small structured buffer of (texel A, texel B, L, W, Δz). Pool nodes are a few floats each.

### 5.2 Inflow: crest, not top row

In **every** dump the mesh rises along v before the lip:

| bake | z at v = 0  | z at the crest | crest position |
| ---- | ----------- | -------------- | -------------- |
| 0    | −1191       | −916           | row 72 of 184  |
| 3    | −515        | −226           | row 108 of 192 |
| 5    | about −2820 | about −2430    | about 62% of v |
| 13   | about −1750 | about −1250    | about 68% of v |

Feeding at the top row makes the water fill this uphill basin for seconds before anything falls (first runs: `out 0` after 4 s).

The prototype finds the crest per column (the highest texel in the upper 70%). It treats everything upstream as a reservoir with a flattened bed and puts the river inflow at the lip.

> **To confirm in game:** is this rising part under the river surface, buried in rock, or the tongue under an upper fall's plunge, as the 3→0 overlap suggests? The mod already cuts submerged parts. Either way, the reservoir level should come from the water plane.

### 5.3 Kernels per substep

Three dispatches over active 8×8 tiles only. Tiles with no sim texel are skipped, which removes about 50-75%: sim texels are 11-50% of the 192² grid.

1. **Velocity.** Advect U and V (2 bilinear taps per face plus the cell-centred contravariant velocity), add gravity, pressure and friction, clamp, apply walls. Write u_f and Q_f.
2. **Limiter.** Compute K_i per texel from its 4 faces. Kellomäki fuses this into kernel 1 by recomputing the neighbours' pipes, which is conservative.
3. **Continuity.** Update h_i from the 4 face Q scaled by the donor K. Apply reservoir, sink, seepage and the portal fluxes (a separate tiny dispatch over links). Accumulate the foot-drain volume per sink texel into the plunge-emitter buffer.

**Prewarm:** steady state takes 4-6 s from dry. On cell load, run about 300 half-resolution steps (well under 1 ms total), or persist the steady state per reference.

### 5.4 Rendering from the sim

Sample the sim in the vertex and pixel shaders through the bake's uvMin/uvSize mapping.

-   **Thickness → translucency and colour.** Use Beer-Lambert, α = 1 − exp(−σ·h), and multiply vanilla alpha by it. Where h ≈ 0 (rock wakes, dry lee sides) the mesh fades and shows the rock behind it. Darken and tint with h.

-   **Foam and whitewater** come from sim quantities, never painted:

    -   Pillows and jumps: deceleration along the flow, −(u·∇)|u| above a threshold.
    -   Convergence: ∇·u < 0, together with h above its local free-stream value.
    -   Aeration on drops: |u| > about 500 and h small.
    -   Actor contact.

    Carry foam as an advected scalar with decay (one more channel through the same semi-Lagrangian kernel), so pillows trail foam downstream. The same triggers seed spray (cf. Chentanez's breaking-front test [CM10]).

-   **Texture motion without pulsing.** Rank the options:

    1. **Stream function of the sim's flux as the across-flow coordinate s** (`pipesim_psi.py`, `pipesim_psi_15.png`). In steady state the flux field is divergence-free away from sources and sinks, so a least-squares ψ with rot ψ ≈ Q exists. Its level sets are the streamlines round each rock. This is exactly the "regularised static warp" that `techniques-deep.md` recommends, now driven by physics instead of a heuristic flow bake. Pair it with r = travel time along the flow and scroll T(s, r − c·t): time-linear, so no reset and no pulse.
        - Caveats: streak spacing is ∝ discharge per width (dense in thick edge troughs), so normalise s by a smoothed local q. The least-squares residual was 33-47% because of non-steady ponds and seepage, so recompute it only every few seconds and blend between versions.
    2. **Advected texture coordinates** (Max & Becker / Neyret [N03]): 2-3 coordinate sets advected by the same kernel and regenerated by strain. Good for transient disturbances (actor wakes) on top of option 1.
    3. **Two-phase flowmap from the sim velocity** with per-texel phase noise and per-phase UV jump [VLA10] [CATLIKE]. It is cheap, but it stretches on accelerating curtains. This is what C:S II does with a blurred sim flow map.
    4. If layers are blended, normalise by √Σw² to keep contrast [YNBH11].

-   **Mild vertex displacement:** offset along the normal by s_d·(h − h̄_local), with s_d of about 0.3 and a clamp of a few units. Do it in **both** depth passes from the same texture and the same frame (the EQUAL pre-pass constraint, D29/D38).

### 5.5 Coupling to the existing systems

-   Foot sinks → plunge emitters of the shallow-water solver. This is a real volume rate per texel, which replaces the heuristic emitter strength.
-   The pool level from the shallow-water solver and water plane → the next sheet's reservoir level.
-   Rocks: the existing per-texel rock mask.
-   Actors: discs from the collision proxies we already gather.

---

## 6. Cost estimate (GPU)

### Sizes

-   Sim texels per mesh: 4k-19k at full resolution and 1k-4.6k at half resolution (average about 10k and 2.5k).
-   For 20 meshes, that is about 200k texels at full resolution and about 50k at half resolution. Tile overhead adds about +30%.

### Traffic per substep per texel

-   State read and write: h, U, V in fp32, about 24 B.
-   Statics in fp16: about 24 B.
-   Q and K between kernels: about 16-20 B.
-   Total: about 65-70 B of unique DRAM traffic. Stencil neighbours come from L2 or LDS.
-   ALU is trivial: about 100-150 ops per texel.

### Estimates

| Configuration                         | Texel-steps per frame | Traffic      | Bandwidth time (300-450 GB/s) | Plus dispatch and barrier              | Total                 |
| ------------------------------------- | --------------------- | ------------ | ----------------------------- | -------------------------------------- | --------------------- |
| **Half res, 1 substep (recommended)** | about 65k             | about 4.5 MB | about 0.01-0.015 ms           | 4 small dispatches, about 0.02-0.04 ms | **about 0.05 ms**     |
| Full res, 2 substeps                  | about 520k            | about 35 MB  | about 0.08-0.12 ms            | 8 dispatches                           | **about 0.15-0.2 ms** |

### Cross-check against published per-cell rates

Chentanez, Müller & Kim's 512² SWE ran at 0.62 ms per step on a 780 Ti, about 2.4 ns per cell including MacCormack and limiters [CMK14]. At that rate:

| Configuration        | 780 Ti-class | Modern mid-range (≈ 3× the compute, 1.3-1.5× the bandwidth) |
| -------------------- | ------------ | ----------------------------------------------------------- |
| Half res, 1 substep  | 0.16 ms      | about 0.06-0.1 ms                                           |
| Full res, 2 substeps | 1.25 ms      | about 0.3-0.5 ms                                            |

Kellomäki's pipe model (65k cells, 0.55 ms on his hardware) [K15] and Šťava's 0.57 ms at 512² [ST08] agree with this order of magnitude.

### Verdict

-   **Half resolution for all 20 meshes fits comfortably under 0.3 ms**, about 0.05-0.1 ms.
-   **Full resolution with 2 substeps** is borderline for 20 meshes, but fine for the nearest 4-6 (LOD by distance).
-   Running the far meshes at 30 Hz halves their cost again, though CFL then needs 2 substeps at half resolution.

---

## 7. Open problems

1. **The crest and reservoir heuristic.** The upstream rise of every mesh must be understood in game (river surface, rock, or upper-plunge tongue). The reservoir level should be the real water-plane height. Per-column crests vary by hundreds of units on some meshes (bake0 sides vs centre), so with one real level only the low columns spill. That may be correct, but it needs an eye test.
2. **Ponding in edge troughs and closed basins.** Mesh edges curl and hold thick, slow water (dark strips in `pipesim_8.png` and `pipesim_15.png`). The options are seepage (λ about 0.05; 0.5 lost about 25% of the flow), a spill-off-the-side-edge sink, or treating troughs as walls. All need an eye test.
3. **Overlapping neighbour meshes.** Bakes 2/3/4, 6/7 and 8/11 overlap over thousands of texels. Edge portals do not make the two copies agree inside the overlap, so the same water could render twice with different thickness. Choose an owner per overlap (render-time fade) or link every overlapping texel pair. The second is more links, still cheap.
4. **κ (sheet stiffness) is a tuning knob,** not physics. It sets pillow height and lateral spreading. Values of 4-12 behave well. It also raises the wave speed, which the depth cap handles.
5. **Speeds are friction-tuned.** Medians are 255-370 u/s with p99 at 420-670. Vanilla's UV scroll implies about 410-475 u/s on drops (`vanilla-nif-dissection.md`), so bed friction on steep parts may need lowering, or v_T raising, to match.
6. **Low-poly facets.** Slopes are piecewise constant per triangle. Bake the geometry from the PN-subdivided mesh, not the vanilla one, or the classic pipe mode shows facet streaks. The "sw" mode mostly hides them.
7. **Actor interaction is weak in thickness.** Blocking faces only gives a speed wake. Add an h impulse and spray emission.
8. **Stream-function texture coordinates** need a robust, incremental solve (Jacobi or multigrid over a few frames) and discharge normalisation. The least-squares residual is 33-47% today because of non-steady ponds.
9. **fp32 state is required** [ST08]. Do not pack h, U or V into fp16.
10. **Not validated:** many sheets in one atlas on the GPU, real frame timing, and visual quality in game. Everything above is offline, on 18 dumped bakes.

---

## References

### Primary sources (fetched and read)

-   [K15] T. Kellomäki, _Large-Scale Water Simulation in Games_, PhD thesis, Tampere Univ. of Technology, 2015. https://trepo.tuni.fi/handle/10024/115052 (PDF: https://trepo.tuni.fi/bitstream/handle/10024/115052/kellomaki_1354.pdf)
-   [MDH07] X. Mei, P. Decaudin, B.-G. Hu, "Fast Hydraulic Erosion Simulation and Visualization on GPU", Pacific Graphics 2007. http://www-evasion.imag.fr/Publications/2007/MDH07/FastErosion_PG07.pdf
-   [ST08] O. Šťava, B. Beneš, M. Brisbin, J. Křivánek, "Interactive Terrain Modeling Using Hydraulic Erosion", SCA 2008. https://www.cs.purdue.edu/cgvlab/www/resources/papers/Stava-2008-Interactive_Terrain_Modeling_Using_Hydraulic_Erosion.pdf
-   [JT11] B. Jákó, B. Tóth, "Fast Hydraulic and Thermal Erosion on GPU", CESCG 2011. https://www.old.cescg.org/CESCG-2011/papers/TUBudapest-Jako-Balazs.pdf
-   [OH95] J. O'Brien, J. Hodgins, "Dynamic Simulation of Splashing Fluids", Computer Animation 1995. https://arxiv.org/pdf/2302.06087
-   [CM10] N. Chentanez, M. Müller, "Real-Time Simulation of Large Bodies of Water with Small Scale Details", SCA 2010. https://matthias-research.github.io/pages/publications/hfFluid.pdf
-   [CM11] N. Chentanez, M. Müller, "Real-Time Eulerian Water Simulation Using a Restricted Tall Cell Grid", SIGGRAPH 2011. https://matthias-research.github.io/pages/publications/tallCells.pdf
-   [CMK14] N. Chentanez, M. Müller, T.-Y. Kim, "Coupling 3D Eulerian, Heightfield and Particle Methods for Interactive Simulation of Large Scale Liquid Phenomena", SCA 2014. https://matthias-research.github.io/pages/publications/hybridsim_preprinted.pdf
-   [WMT07] H. Wang, G. Miller, G. Turk, "Solving General Shallow Wave Equations on Surfaces", SCA 2007. https://faculty.cc.gatech.edu/~turk/paper_pages/2007_shallow_waves/shallow_water.pdf
-   [OS13] J. Ojeda, A. Susín, "Enhanced Lattice Boltzmann Shallow Water" (EG 2013 short). https://web.mat.upc.edu/toni.susin/files/ParticleLBMSW-cgf-sub.pdf
-   [N03] F. Neyret, "Advected Textures", SCA 2003. https://inria.hal.science/inria-00537472/document
-   [YNBH11] Q. Yu, F. Neyret, E. Bruneton, N. Holzschuch, "Lagrangian Texture Advection", IEEE TVCG 17(11), 2011. https://inria.hal.science/inria-00536064/document
-   [VLA10] A. Vlachos, "Water Flow in Portal 2", SIGGRAPH 2010 Advances course. https://www.advances.realtimerendering.com/s2010/Vlachos-Waterflow(SIGGRAPH%202010%20Advanced%20RealTime%20Rendering%20Course).pdf
-   [CATLIKE] Catlike Coding, "Texture Distortion". https://catlikecoding.com/unity/tutorials/flow/texture-distortion/

### Cities: Skylines internals and wikis

-   [VEL] vellum (reads the C:S water grid): https://github.com/sebas-tcotd/vellum
-   [BSP] BuildingSpawnPoints mod, SpawnPoint.cs: https://github.com/MacSergey/BuildingSpawnPoints
-   [RAIN] Rainfall mod (WaterSimulation sources and patches): https://github.com/yenyang/rainfall
-   [CSWIKI] C:S Map Editor wiki: https://skylines.paradoxwikis.com/Map_Editor
-   [CSLMOD] C:S theme modding (water and foam textures): https://cslmodding.info/theme/
-   [CS2MOD] CS2 modding guide, WaterSystem member list: https://github.com/ps1ke/Cities-Skylines-2-Modding-Guide
-   [CS2WIKI] CS2 Map Creation: Water: https://cs2.paradoxwikis.com/Map_Creation:_Water
-   [PCW] PCWorld on C:S water: https://www.pcworld.com/article/430895/cities-skylines-is-more-like-simcity-than-simcity.html
-   [STEAM] Players on sloshing and "tsunamis" (unverified anecdote): https://steamcommunity.com/app/255710/discussions/0/1732087825003522978

### Related project docs

-   `techniques-deep.md`: the static-warp / stream-function texture coordinate.
-   `vanilla-nif-dissection.md`: UV density and vanilla speeds.
-   `history.md`: D24, why a heightfield fails on drops.

### Prototype files (scratchpad)

-   `pipesim.py`: model, portals, pools, actor.
-   `pipesim_psi.py`: stream function.
-   `simbake.py`: loader, and the D74 projection solver for comparison.
-   `pipesim_v1.py`: first version, top-row inflow.

### Plots

| File                                            | What it shows                       |
| ----------------------------------------------- | ----------------------------------- |
| `pipesim_{0,3,5,8,13,15}.png`                   | Full resolution, 2 substeps         |
| `pipesim_half_{0,3,5,8,13,15}.png`              | Half resolution, 1 substep          |
| `pipesim_pipe_{3,8}.png`                        | Classic pipe model, no advection    |
| `pipesim_stiff0_3.png`, `pipesim_stiff12_3.png` | κ = 0 and κ = 12                    |
| `pipesim_sub1_8.png`, `pipesim_sub2_8.png`      | Full resolution at 1 and 2 substeps |
| `pipesim_actor8.png`                            | Actor crossing the curtain          |
| `pipesim_multi2-3-4-0_*.png`                    | Coupled site, seepage 0.5           |
| `pipesim_multilowleak_2-3-4-0_*.png`            | Coupled site, seepage 0.05          |
| `pipesim_psi_{3,15}.png`                        | Stream function of the sim flux     |
| `pipe_geom_{0,3}.png`                           | z and \|n_z\| per texel             |
