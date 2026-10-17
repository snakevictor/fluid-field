# Waterfall curtains: smooth, rock-aware texture flow (deep dive)

This follows `waterfall_techniques.md` (survey) and DECISIONS D28-D59, and does not repeat them. As of D59, each lit falls mesh has a per-mesh UV-grid flow bake (t124: across, down, foam, run time; about 10 units per texel). The curtains scroll one continuous sample down +v (D58), and two phases carry only the deviation. Those phases still read as pulsing, and they turn chaotic near rock.

---

## 0. The key observation: a static warp is a stream-function coordinate

Take any static warp of the continuous scroll:

    colour(u,v,t) = T( s(u,v), r(u,v) - c t )     with  s = u + Du(u,v),  r = v + Dv(u,v)

-   **Where the streaks go.** A vanilla falls texture has its streaks roughly along its own v, at fixed texture-across coordinate s. So the visible streaks are the **level sets of s(u,v)**. For streaks to follow the flow, s must be constant along streamlines. That means s is a stream function, or any monotone relabelling of one.
-   **How features move.** A texture feature at fixed (s0, r0) sits where s = s0 and r = r0 + ct. As t grows it slides along its level set of s, which is its streamline, at the rate set by r. **The motion is exactly along the streaks and strictly time-linear, so there is no reset, no phase and no pulsing.** It can never run backwards while ∂r/∂(along-flow) > 0.

So proposal (4), the static warp, and proposal (3), psi across with travel time along (D28/D30), are **the same family**. They differ only in how s and r are chosen and regularised. D28/D30's problems (shear, pinching, smearing) are the known singularities of an _unregularised_ stream function and an _unclamped_ travel time. They are not a flaw of the idea. Everything below is about building a well-conditioned (s, r) pair on the D59 UV grid.

Where the singularities sit. Take potential flow past a round rock of half-width a, with psi = U y (1 - a²/ρ²), where ρ is the distance from the rock's centre:

-   **Sides of the rock:** the streamlines crowd, so |∇s| reaches about 2. That is 2x compression, which is mild.
-   **Front and rear stagnation points:** |∇s| → 0, so a single texture column is smeared across the whole rock width. This is the "fan/smear" D28 showed.
-   **Wake:** with a clamped, non-divergence-free flow (the D59 forward ≥ 0.3 clamp), streamlines that run into the rock end there. Behind the rock, s is then filled by whatever the numerics give, which caused D28's chaos.

The design rule that follows: **put the whitewater masks of section (b) exactly on the singular zones**, the upstream pillow and the downstream wake. They are where real water turns white anyway, and they hide the only places where the warp has to misbehave.

---

## (a) Texture flow options, evaluated

Scores: P = no pulsing, O = obstacle response, C = cost, R = robustness on low-poly meshes, F = fit with our data.

### (4)+(3) Regularised static warp, s = streamline label and r = clamped along-coordinate. RECOMMENDED

**Computing s (Du).** There are three ways, best first.

1.  **Label sweep plus anisotropic least-squares smoothing (best).**

    -   **Sweep.** The D59 flow already guarantees dv ≥ 0.3·base and a turn of at most 60°. So the label transport equation ∂s/∂v + tanθ ∂s/∂u = 0, with s = u on the inflow row, is well posed as a single top-to-bottom sweep. It is semi-Lagrangian and unconditionally stable. Here tanθ = flow_u/flow_v from the bake, in texel units scaled by the per-texel metric:

            for each row j = 1..H-1:
              for each column i:
                du = tanθ(i,j) * Δv            // across displacement over one row, in texels
                s(i,j) = lerp-sample s(·, j-1) at (i - du)   // clamp at open edges
            rock texels: s is carried through (the sweep passes "under" the rock using the
            harmonic-inpainted flow inside rock), so s stays continuous behind it

        This is exactly "streamline displacement = ∫ tanθ dv": the texel's s is the u at which its streamline entered. Numerical diffusion from the linear interpolation smooths the would-be jumps where streamlines converge.

    -   **Regularise.** Solve for s minimising

            E(s) = Σ w_align (∇s·t̂)²  +  w_space (∇s·n̂ − g)²  +  w_fit (s − s_sweep)²

        t̂ and n̂ are the unit flow direction and its normal, in metric-corrected UV. g is the desired streak spacing, which for a stream function is the local speed ratio, clamped to g ∈ [0.6, 1.8]. Use w*align ≫ w_space (for example 20:1). The weights are what make this work: where the "true" stream function would pinch, the solver bends the streaks slightly across the flow instead, and never squeezes them beyond the clamp. This is a linear, anisotropic, screened Poisson problem. The existing Jacobi/SOR solver from D28 handles it in a few hundred iterations on ≤192² texels (CPU, once per mesh, a few ms). It is the same "integrate a non-integrable target gradient in least squares" idea as gradient-domain methods. Knöppel et al.'s Stripe Patterns [4] is its principled big brother: it solves for a \_phase* whose gradient matches a direction-and-spacing field, and inserts singularities (stripes that end or fork) where the target cannot be integrated. That is exactly what a streak pattern should do behind a rock.

2.  **Harmonic (Laplace) displacement.** Solve ∇²s = 0, with s = u on the inflow row and the open edges, Neumann at the outflow, and s = const*k on rock k. This \_is* the potential-flow stream function, the classic harmonic fill (Bridson's curl noise forces psi to a constant on boundaries in the same spirit [7]). With anisotropic Laplacian weights (k_v > k_u), the bend starts earlier above the rock and heals later below it. That gives a "bend length" knob and lowers the side compression. Each rock's constant should be the inflow label of the streamline through its centre: no circulation, so it splits evenly. The method is simple, but it ignores the solved flow, so it is a fallback or a quick first test.

3.  **Partial collapse λ (any method).** Use s_λ = u + λ(s − u) with λ ≈ 0.6-0.8. The rock no longer has to collapse to a single texture column: it maps to a band of width (1 − λ)·W. Front and rear stretch are then bounded by about 1/(1 − λ) instead of ∞. The streaks that "should" go around now run under the rock instead, but **the rock is opaque geometry piercing the sheet, so it hides them**. This is the most robust knob and costs nothing. Make λ spatial: λ = 1 far from rock, fading to λ_rock within ~2 rock radii, using the SDF already baked in D59.

**Hard limits to enforce after the solve.** Use J = ∂(s,r)/∂(u,v) in metric units:

-   Clamp the across-flow compression |∇s·n̂| to [0.5, 2.0]. Beyond 2, streaks alias into mip blur. Below 0.5, they visibly smear.
-   Keep ∂r/∂(along) in [0.6, 1.4].
-   Clamp the shear |∂r/∂u| ≤ 0.5. Above that, horizontal texture features such as ripple bands tilt noticeably.
-   Where a clamp bites, blend locally toward s = u, r = v. The blend region should be covered by the whitewater masks.

**Computing r (Dv).** Use r = v by default. Feature speed is then vanilla's along v, slightly faster along tilted streamlines (c/cosθ), which reads naturally. Optionally, let water speed up through gaps: integrate dr = ds_along / clamp(speedRatio, 0.7, 1.5) along streamlines with the same sweep. **Never use raw travel time.** It diverges at the stagnation point, which caused D30's shear and stall, and it gives huge ∂r/∂u across neighbouring streamlines. Clamping speed from below before integrating is the fix.

**Fixing D28's specific issues.**

-   **Shear** (r differing wildly between neighbouring streamlines): clamp speed before integrating, cap ∂r/∂u, or use r = v.
-   **Pinching** (s gradient → ∞ beside or between rocks): use the least-squares spacing term or the clamp.
-   **Smear** (∇s → 0 at stagnation): use the partial collapse λ, with foam over the remainder.
-   **Behind the rock** (undefined s): carry the sweep through the harmonic-inpainted rock and smooth.
-   **Extreme compression:** use **octave layers**, the spatial analogue of Neyret's temporal regeneration. Let k = log2(|∇s·n̂|). Sample T at (s·2^floor(k)) and (s·2^(floor(k)+1)) and blend by frac(k) with a variance-preserving blend [5]. Streaks then appear to "fork" or "merge", as in a stripe pattern, instead of compressing. Use it only where the clamp would otherwise bite (one extra fetch, branch-free with lerp).

**Shader (Lighting/Effect, per falls draw).**

    float4 w  = t124.SampleLevel(lin, uv, 0);         // Du, Dr packed (float16, small values)
    float2 st = float2(uv.x + w.x, uv.y + w.y);
    float2 suv = st - float2(0, c * time);             // the same single continuous scroll as D58
    float4 col = tex.Sample(aniso, suv);               // hardware derivatives of st do the mip/aniso
    // normal maps: bring the perturbation back to mesh UV space with J^T
    float2x2 J = float2x2(ddx(st), ddy(st)) * inverse(float2x2(ddx(uv), ddy(uv)));  // or bake J
    n.xy = mul(transpose(J), n.xy); n = normalize(n);

**Precision and storage.** Store Du and Dr, not s and r. They are small values, so float16 is precise (s itself can exceed 4, where fp16 steps are about 0.004). Bilinear interpolation at 10 units per texel is smooth, and it is independent of the triangle size: per-pixel, so low-poly meshes are a non-issue. One RG16F fetch, or two channels of t124 if velocity is dropped (the velocity is no longer needed for texture motion).

**Score.** P: perfect, nothing depends on time except a linear scroll. O: good. Streaks bend round, part and rejoin; the stagnation and wake zones need the masks. C: one fetch plus a few ALU, under 0.02 ms. R: per-texel, triangle-independent. F: excellent, it reuses the D59 bake, solver, SDF and metric.

**What it cannot do:**

-   Recirculation or eddies (irrelevant on curtains).
-   Show texture "piling up". Water that slows shows streak spreading, not accumulation. Use the masks.
-   Represent flow that changes over time. Steady sites are fine.

### (6) Procedural or noise-based water layered in the warped (s, r) frame. RECOMMENDED as a complement

Do not replace vanilla's texture. Add procedural detail in the same warped frame, so it inherits the bend for free:

-   **Per-streak speed variation.** Hash the streak column id = floor(s·N) into a speed factor in [0.85, 1.25]. Sample anisotropic noise at (s·N, (r − c·f(id)·t)·M), crossfaded between neighbouring ids with a smooth weight. Each streak falls at its own speed, with no resets, which is the classic "streaky curtain" look (realtimevfx/Unity waterfall breakdowns use two offset Voronoi layers for this [10][11]).
-   **Flow noise.** Perlin and Neyret's flow noise [6] (gradient rotation plus pseudo-advection of small scales by large ones) gives "boiling" without any advection reset. Use it in whitewater zones only, at 2-3 octaves of simplex noise (around 0.05 ms at the screen coverage of falls).
-   **Curl noise** [7] is for velocity, not appearance. It is useful for spray/mist particle motion near rocks, with the psi ramp to zero at the rock SDF so particles slide round. It is not needed for the sheet.

Score: P: perfect. O: inherits from (4). C: low. R: good. F: good. Risk: it may not look "Skyrim". Keep the amplitude modest, and use it mainly as a mask modulator for lace, whitewater and alpha.

### (2) Directional flow (van Hoesel 2010 [2]; Catlike Coding [3]). NOT for curtains

There is no time reset: each grid cell rotates the tile to its flow direction and scrolls it continuously, and four half-offset cells are blended with triangle weights. This is good for isotropic ripple normals on rivers. On **streaky** waterfall textures it fails in two ways:

1. Streaks do not connect across cells (each cell is an independent rotated copy). Around a rock, where the direction changes quickly, you get cross-hatched "plaid".
2. The four-way blend loses contrast and shows the grid in speculars (Catlike documents both artifacts and only "obfuscates" them with a second grid).

It is also 4-8 fetches. It could serve the _pools_ if the SWE flowmap pulsing ever becomes a problem there. Score: P good, O fair, C medium, R good, F fair.

### (1) Advected textures with regeneration hidden by noise (Neyret 2003 [1]). NO for curtains

This is the general form of what was tried. Pulsing has two separate causes:

-   **Contrast pulsing.** Averaging two uncorrelated samples at 50/50 cuts variance to 0.5. Fix: variance-preserving blending, (Σ w_i (x_i − μ)) / sqrt(Σ w_i²) + μ, from Heitz and Neyret's histogram-preserving blend [5]. D46 already moved in this direction.
-   **Visible displacement reset.** The phase period is too long for the local stretch. Neyret's fix is a per-pixel latency driven by _accumulated deformation_, with several layers at different periods. That brings back ghosting and costs M×N samples.

On a steady curtain, all of this machinery exists to emulate what the static warp gives for free. Use it only where the flow is truly unsteady (the SWE pools). Score: P fair at best, O good, C medium, R good, F good.

### (5) Lagrangian texture particles (Yu, Neyret, Bruneton, Holzschuch 2011 [8]). OPTIONAL accent layer

Particles carry texture patches along the flow and are splatted with spectrum-preserving blending. Density control (spawn where sparse, kill where dense) keeps statistics stable. This handles splitting and stagnation naturally, because particles simply go one way or the other.

For us: the falls are steady, so the base sheet does not need it. It is very good as **whitewater streak particles**:

-   Emit along the lip and at rock pillows.
-   Move each particle in the D59 UV grid using the baked flow.
-   Store world position per texel in the bake (it is already rasterised in D59), so the GPU particle system can place surface-hugging, flow-aligned stretched quads.
-   Fade in and out over 0.3 s, so spawn and kill never pop.

This reuses our working GPU spray pipeline. Cost: about 2-5k quads per site, around 0.1-0.2 ms. Score: P good with fades, O excellent, C medium, R excellent (independent of mesh), F good.

### Ranking for (a)

1. Regularised static warp (s, r) from the D59 bake, with λ partial collapse and clamps. **Remove the two phases entirely.**
2. Procedural per-streak speed variation and flow noise in the (s, r) frame.
3. Lagrangian whitewater streak particles at rocks (later).
4. Directional flow, for pools only if needed.
5. Neyret regeneration: no.

---

## (b) Obstacle response visuals (masks, not true flow)

What real water does where rock pierces a falling sheet:

-   a **pillow** (bow wave) piles up on the upstream face and is white;
-   the sheet **splits**; the edges of the split thicken into ropes and lace;
-   a **dry or shadow gap** behind the rock heals over a few rock widths as the sheet re-closes;
-   a **V of whitewater** trails downstream from the rejoin;
-   **spray** comes off the sides, where the flow is fastest;
-   the rock face is **wet** and dark.

Each of these has a mask, and all of them come from the D59 bake plus one extra downstream sweep:

| Mask          | Bake rule                                                                                                                                            | Shader use                                                                                                                    |
| ------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| Pillow        | SDF < d0 and dot(flow, −∇SDF) > 0, scaled by speed; the band height grows with speed (≈ 0.5-1.5 rock radii)                                          | whitewater = 1, slight brightness boost, flow noise; later, tessellation bulge                                                |
| Rim           | \|SDF\| < ~6 units                                                                                                                                   | thin lace, alpha erode with noise, darker "rope" just outside                                                                 |
| Wake / shadow | downstream sweep: w(i,j) = max(rock(i,j), w(back-traced, j−1)·exp(−Δs/L)), with L ≈ 2-4 rock widths and slight lateral spread (blur 1 texel per row) | alpha ↓ near the rock (translucent gap, once the blended draw works; until then darker and thinner), whiteness ↑ further down |
| V-wake        | gradient magnitude of the wake mask across the flow, i.e. its edges                                                                                  | whitewater streaks; also feeds the octave layer                                                                               |
| Gap speed     | \|∇s·n̂\| > 1.3 (from the warp)                                                                                                                       | brighter and glassier, more specular; spray emitters                                                                          |
| Side spray    | rim × gap speed                                                                                                                                      | GPU spray emit points                                                                                                         |

Notes:

-   **Put the warp's singular zones under these masks on purpose.** Pillow means front stagnation; wake means rear stagnation. Then the λ partial collapse can be aggressive.
-   Animate the masks with the same (s, r) scroll (sample noise at (s, r − ct)), so foam moves with the streaks instead of sitting still.
-   Rock wetness: CS Wetness Effects handles wet rock globally. A per-site wet band is optional and needs the rock's shader, so it is out of scope.
-   References: Uncharted 4 foam from flow divergence and obstacles [12]; Horizon Forbidden West foam and impact masks [13]; "Season" waterfall layered masks [10]; realtimevfx rock-collision thread (flowmap plus a foam layer "sells it mostly") [14].

---

## (c) Falls into the pool and SWE: matching foam across the seam

1. **One foam material, two inputs.** Factor out a `FoamShade(foamAmount, worldPos, normal, light)` function shared by the falls Lighting/Effect pass and the Water shader. It should use the same albedo, roughness, lace noise **in world-space units** and the same lighting terms. The pool already lights foam with direct and ambient light (D33-era foam pass). Most visible seams are mismatches of lighting and noise scale, not of foam amount.
2. **Falls read the pool's foam at the bottom.** In the bottom band (height above the pool level < ~48 units), lerp the falls' foam amount toward the SWE foam texture sampled at the pixel's world XY. Both sides then show the _same_ foam mass at the seam.
3. **The pool receives the falls' foam.** At the plunge line, inject foam at the rate = the falls' bottom whiteness × flux (the D59 run-time aeration at the outlet row). Also inject the bake's lateral flow, so foam leaves the curtain the way the streaks were heading. D55 already pushes; this makes the amount _consistent_.
4. **Noise continuity.** Near the seam, sample lace noise in 3D world space on both sides: on the falls it scrolls down, in the pool it is advected outward. At the line itself, use a "boil" with high foam coverage and low-contrast flow noise, so the change of motion direction is not readable.
5. **Pre-warm the SWE foam** after a load (run N steps), so the pool does not start clean while the falls are already white.
6. **Height fade, not depth fade** (survey C1), and keep spray and mist emitters on the plunge line to cover the remainder.
7. The UE5 "Water Dynamics" plugin (2025-26) does the same: gravity-sheet falls with landing foam, droplets, mist and momentum into the pool, and foam transported by the solved velocity [15]. Far Cry 5 and Horizon also share foam terms between falls and water bodies [13][16].

---

## (d) Replacing vanilla's falls surface entirely (D3): can it work?

It _can_, but the D30/D31 failures map onto known pitfalls:

1. **Shape.** A top-down grid loses vertical and arched shape (D31: "waaay too shallow and slow"). If you replace the surface, re-mesh _vanilla's own sheets_. D31 did this, but merged them, which created new problems.
2. **Look.** Vanilla's falls draw through specific Lighting and Effect shader permutations with several layers (lit sheet, whitewater, streaks). Re-rendering them in your own pass loses their exact lighting, fog, sorting and the users' texture mods ("the water itself looks shit", D32). **The look must come from the original draw calls.**
3. **Passes.** The depth prepass, lit pass, effect layers, shadows/reflections and any ENB or other mod passes all have to agree. Any geometry change must be applied in _every_ pass that draws the mesh (D29).
4. **Ownership and lifetime.** Swapping renderer buffers breaks on cell unload (D30).
5. **Sorting.** Translucent sheets are sorted against effect layers and particles. A separate draw breaks the order.
6. **UVs.** Generated UVs reintroduce exactly the s/r singularities of section 0, unless regularised.

**The "done right" version is D2, not D3.** Hook the draw of each vanilla falls shape in _all_ passes and substitute a CPU-subdivided copy (same shape, about 40-60 unit edges) with the same vertex layout, or use hardware tessellation in those passes. Displace it by the bake (pillow bulge, lip thickening). Keep vanilla's material, textures and draw order. Our resources are refcounted and released on the shape's detach or destruction. Everything in (a)-(c) is texture-space and per-pixel, so it does not depend on this. Do it only for silhouettes, after (a) and (b) ship. Full replacement stays not recommended.

---

## Recommended order (about 1 ms budget; expected cost about 0.1-0.3 ms)

1. Bake s (label sweep → anisotropic LSQ → λ collapse → clamps) and r (= v, with optional clamped speed). Store Du/Dr in t124 and drop the two phases. Add a debug view drawing the s level sets.
2. Bake the masks (pillow, rim, wake sweep, V, gap) into one RGBA8. Shade them with noise scrolled in (s, r).
3. Add per-streak speed variation and flow-noise accents.
4. The shared FoamShade, the falls reading SWE foam near the pool, and consistent injection.
5. Optional: Lagrangian whitewater streak particles from the bake.
6. Optional: D2 subdivision/tessellation in all passes, for bulges.

## Sources

1. Neyret, "Advected Textures", SCA 2003: https://diglib.eg.org/items/c4bf80cc-23f0-4107-958d-e03c0b9c8ac4/full
2. van Hoesel, "Tiled Directional Flow", SIGGRAPH 2010 talk: https://history.siggraph.org/?p=129661
3. Catlike Coding, Directional Flow: https://catlikecoding.com/unity/tutorials/flow/directional-flow/
4. Knöppel, Crane, Pinkall, Schröder, "Stripe Patterns on Surfaces", SIGGRAPH 2015: https://www.cs.cmu.edu/~kmcrane/Projects/StripePatterns/
5. Heitz, Neyret, "High-Performance By-Example Noise using a Histogram-Preserving Blending Operator", HPG 2018: https://hal.inria.fr/hal-01824773 (variance-preserving blend: divide by sqrt(Σw²))
6. Perlin, Neyret, "Flow Noise", SIGGRAPH 2001 sketch: https://evasion.inrialpes.fr/Publications/2001/PN01 and https://evasion.inrialpes.fr/~Fabrice.Neyret/flownoise/index.gb.html
7. Bridson, Hourihan, Nordenstam, "Curl-Noise for Procedural Fluid Flow", SIGGRAPH 2007: https://www.dneg.com/wp-content/uploads/2022/11/2007.pdf; Ding and Batty, "Differentiable Curl-Noise" 2023: https://cs.uwaterloo.ca/~c2batty/papers/Ding2023/Differentiable_Curl_Noise.pdf
8. Yu, Neyret, Bruneton, Holzschuch, "Lagrangian Texture Advection: Preserving both Spectrum and Velocity Field", IEEE TVCG 2011: https://artis.inrialpes.fr/Members/Nicolas.Holzschuch/PublicationsACM.html
9. Kwatra et al., "Texturing Fluids", TVCG 2007, and Narain et al., "Feature-Guided Dynamic Texture Synthesis on Continuous Flows", EGSR 2007: https://gamma.cs.unc.edu/DTS_FLOW/ (offline; shows that flow-guided synthesis needs spacing control, as here)
10. "How we made waterfall in Season": https://realtimevfx.com/t/how-we-made-waterfall-in-season-a-letter-to-the-future/23420
11. Unity, nature shaders (waterfall with two offset Voronoi layers): https://unity.com/blog/engine-platform/nature-shaders-with-shader-graph-in-2022-lts ; Roblox waterfall VFX: https://create.roblox.com/docs/en-us/tutorials/use-case-tutorials/vfx/create-waterfalls.md
12. Gonzalez-Ochoa, "Rendering Rapids in Uncharted 4", SIGGRAPH 2016 Advances: https://advances.realtimerendering.com/s2016/
13. Malan, "Water in Horizon Forbidden West", SIGGRAPH 2022 Advances: https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Water-Malan.pdf
14. realtimevfx, "Create a collision between rock and water": https://realtimevfx.com/t/create-a-collision-between-rock-and-water-river/16024
15. Water Dynamics (UE5 plugin): https://forums.unrealengine.com/t/timursibatov-water-dynamics-interactive-rivers-waterfalls-lakes/2840244
16. Far Cry 5 water, GDC 2018: https://80.lv/articles/the-gdc-talk-water-rendering-in-far-cry-5
17. Bridson-style psi ramp at boundaries and Curl-Flow (Chang, Batty et al.): https://arxiv.org/pdf/2104.00867

Not verified: no public talk describing a shipped game's waterfall "static UV warp around rocks". That approach is derived here from first principles: the stream-function equivalence and the singularity analysis.
