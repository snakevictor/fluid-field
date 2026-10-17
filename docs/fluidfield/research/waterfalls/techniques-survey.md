# Realistic waterfalls on vanilla meshes: technique research

Context: Community Shaders Fluid Field (docs/fluidfield/DECISIONS.md D25-D57; read up to D57).
Status after D57: forward = mesh +v at vanilla scroll rate; the top-down solve only steers on flat water; rock cut per vertex via ray parity; Translucency blocked (blended draw is invisible).

Departure scale used below (how far an option moves away from "vanilla meshes + Skyrim renderer + existing data"):

-   **D0**: none. Only new shader code, per-draw SRVs and CPU bakes from data we already have.
-   **D1**: small. Extra lightweight geometry or a pass of our own (quads, decals, a screen copy), vanilla meshes untouched.
-   **D2**: moderate. Vanilla meshes edited in memory (subdivided or augmented) or tessellated. Same shape, more vertices.
-   **D3**: large. Vanilla meshes replaced. Already tried and abandoned (D30); not recommended.

---

## A. Obstacle-aware flow on the mesh surface itself

Shared facts that decide this:

-   Vanilla falls pieces have 180-490 triangles, and those triangles are large. A boulder in the middle of a curtain is often smaller than one triangle. Any field stored **per vertex** and linearly interpolated cannot bend the flow around it. It can only tilt a whole triangle. Guerrilla ran into the same limit in Horizon Forbidden West: foam stored in vertex colour "can't create features smaller than triangles... undersampling problems, like foam popping" (Malan, SIGGRAPH 2022 Advances, "Not enough triangles" slide) [1].
-   UV space is already a natural surface parameterization for these meshes: v runs monotonically downstream over 100% of the area and u runs across. A 2D grid in (u, v) is therefore a "surface-following" grid that does not see curtains edge-on. This is the same idea as Stam's "Flows on Surfaces of Arbitrary Topology" (SIGGRAPH 2003): he ran the 2D Stable Fluids solver in each patch's parameter space, using the metric of the parameterization (curvilinear coordinates) [2].

### A1. Per-mesh 2D flow solve in the mesh's own UV space (RECOMMENDED, D0)

How it works with our data:

1. **Rasterize each lit falls piece into its UV bounding box** on the CPU at a world-scaled resolution, about one texel per 6-8 world units, which is typically 32-128 x 64-512 texels. Use the unwrapped UV range: if v spans more than 1, do not wrap it. Each texel stores the interpolated world position, the normal, and the two tangent vectors dP/du and dP/dv (constant per triangle, from the triangle's position/UV Jacobian). Texels outside every triangle are "void". They become a free-slip, zero-flux boundary, like the open u-edges.
2. **Obstacles per texel**: use the ray parity test we already use per vertex (D57), but per texel, giving a solid mask. Also cast 4-8 short rays in the tangent plane plus one along -N/+N, giving the distance to rock along the surface (an SDF) and the rock's surface normal. A boulder passing through the curtain becomes a solid blob in UV space at the resolution of the texels, not of the triangles.
3. **Solve** a steady incompressible flow in UV space with the metric. The base flow is uniform +v at vanilla's rate (inflow at v_min, outflow at v_max). Then apply a pressure projection or potential solve (Jacobi or SOR, a few hundred iterations on at most 64k cells, about 1-5 ms per piece once) with solid cells as no-flux. Use the per-texel |dP/du|, |dP/dv| as metric weights in the Laplacian (Stam's curvilinear form), so stretched drops behave correctly. Potential flow around an obstacle splits and rejoins. It does not reverse except exactly at the stagnation point.
4. **Clamp "never back"**: dv >= 0.3 x base, |du| <= tan(60 deg) x dv. Then re-scale speed so the area-weighted flux is preserved. This replaces the current 35-degree clamp for curtains.
5. **Output one RGBA16F texture per piece instance (reference)**: RG = flow in UV/s (no tangent conversion needed, because the flowmap already lives in UV space). B = rock SDF along the surface. A = foam/stagnation term: max(0, -dot(baseFlow, gradSDF)) near rock, plus wake/shadow (low speed behind the rock), plus "clash" (see B). Bind it as an extra SRV in Lighting/Effect/Utility. A 64x256 RGBA16F texture is 128 KB.

Cost: CPU, bake once per site load. It reuses our Havok raycasts: 64x256 texels x ~10 rays is about 160k rays per piece, so the bake is time-sliced over frames or thread-pooled, or the SDF is computed only near hits from coarse texels. GPU per frame: one extra texture fetch.
Quality: best of the options. It works at sub-triangle resolution, is divergence-free (it splits around rock and speeds up in gaps, which also drives whitewater), and is immune to edge-on curtains.
Risks:

-   (a) Overlapping or mirrored UV islands. Check whether any vanilla falls piece reuses UVs; if it does, solve per island using triangle IDs.
-   (b) Pieces are solved independently, so flow does not carry across piece seams. Vanilla's flow does not either; seams are hidden with foam (B).
-   (c) UV distortion across a fold. The metric handles it, as long as the Jacobian per triangle is non-degenerate.
-   (d) Memory per reference, if many sites are loaded. Cap the size and share a texture between identical placements only if their rock is identical (in practice it never is).

### A2. Per-vertex deflection from raycast distance + Laplacian smoothing (D0, quick fallback)

Per vertex, cast tangent-plane rays, take the nearest rock direction, and push tangentially away with a falloff. Then do a few rounds of cotangent/umbrella Laplacian smoothing over mesh adjacency, and re-project onto +v-forward. It uses the existing SV_VertexID StructuredBuffer and costs microseconds. It cannot split flow around anything smaller than a triangle, though, and on 400-unit triangles it produces whole-triangle shear: exactly the marbling seen in D57's screenshots 598-601. It is fine as an interim step, or once meshes are subdivided (D2).

### A3. Geodesic distance (heat method) from rock contacts on the mesh (D0)

Crane, Weischedel and Wardetzky's "Geodesics in Heat" (2013) [3] gives smooth surface distance from rock-contact vertices with two prefactored sparse solves (CGAL ships an implementation [4]). Its gradient gives "away from rock along the surface" and its value gives a foam falloff. It has the same per-vertex resolution limit as A2, plus a sparse solver dependency. The A1 grid gives the same quantity for free (a distance transform in UV with the metric). Not worth it on its own.

### A4. Stream function / DEC tangent field design on the mesh (D0, but heavy)

Fisher, Schroder, Desbrun and Hoppe, "Design of Tangent Vector Fields" (SIGGRAPH 2007) [5]: smooth fields come from discrete 1-forms with prefactored Laplacians and soft constraints. For divergence-free flow, you solve for a stream function with ψ constant on obstacle boundaries. That requires the boulder to be a hole in the mesh (cut the mesh), and the result is still per-vertex. It is elegant but brings mesh surgery for no gain over A1. Not recommended.

### A5. Subdivide the vanilla meshes (D2), then A2 or A4 per vertex

Do 1-2 levels of midpoint subdivision on the CPU (into our own VB/IB, same shape) or use the planned tessellation. Per-vertex data then reaches about 50-100 unit resolution. This is worth doing anyway for later **displacement** (a bulge where water climbs over a boulder, a thickened lip, impact bulges), which no shading trick provides. Rank it after A1: A1 gives the flow without any geometry change; A5 adds silhouette.

**Recommendation for A: A1** (UV-space solve, baked per piece, sampled per pixel). Keep the top-down solve for flat runs, where it already leads (D57), and blend: on flat texels, the UV solve's boundary/base flow can be seeded from the top-down result projected into UV.

---

## B. How shipped games make waterfalls and rapids look real (applicability)

| Technique                                                                                                                                      | Source                                                                                              | Use for us                                                                                                             | Departure |
| ---------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------- | --------- |
| Two-phase flowmap with a per-pixel noise phase offset to hide pulsing; flow speed also scales normal strength and tiling                       | Vlachos, "Water Flow in Portal 2", SIGGRAPH 2010 [6]; Grimes, GDC 2011 [7]; Catlike Coding Flow [8] | Already the base. Add the noise phase offset and speed-dependent tiling if they are missing                            | D0        |
| Directional flow (rotating the pattern to align with flow instead of distorting it), which avoids smearing at high divergence around obstacles | Catlike Coding "Directional Flow" [8]                                                               | Use it in the split/wake region around a boulder, where two-phase distortion smears                                    | D0        |
| Advected-texture regeneration driven by accumulated distortion                                                                                 | Neyret, "Advected Textures", SCA 2003 [9]                                                           | Shorten the phase period where the A1 field is strongly divergent                                                      | D0        |
| Layered scrolling sheets: a dark body, panning whitewater streaks at different speeds, vertex-colour or mask "streams" for opacity             | RiME (Trumpler) [10]; realtimevfx breakdowns [11]                                                   | Vanilla already has layers (Effect.hlsl); drive their masks from the A1 speed and fall time                            | D0        |
| Scattering colour: blend deep and subsurface tint by view angle, sun direction and a thickness/peak mask                                       | Sea of Thieves, SIGGRAPH 2018 [12]                                                                  | Thickness = Q/(width x speed): thin and bright where fast, plus back-lit transmission when the sun is behind the sheet | D0        |
| Speed and divergence-driven whitewater/foam; obstacles reshape the flowmap at runtime                                                          | Uncharted 4 rapids (Gonzalez-Ochoa, Advances 2016) [13]                                             | Foam = f(A1 speed, stagnation, -div), plus aeration with fall time (below)                                             | D0        |
| Baked flipbook "localized 2D water effects" instanced at every waterfall impact                                                                | Horizon Forbidden West (Malan 2022) [1]                                                             | Impact rings/boils on the pool at each plunge line                                                                     | D1        |
| A composited waterfall material inside one water system                                                                                        | Far Cry 5, GDC 2018 [14]                                                                            | Confirms that falls and pools should share foam/colour terms                                                           | n/a       |
| Mist plume, droplets in arcs, camera-facing splash flipbooks at the base                                                                       | realtimevfx [11]; Roblox waterfall tutorial [15]                                                    | Already have GPU spray; mist cards were removed (D49), so revisit as soft-depth-faded cards                            | D1        |

Concrete shading terms for our pixel shader (all D0, given the A1 texture):

-   **Aeration with fall time**: t(v) = ∫dv / rate(v). Precompute it per texel in the A1 bake (a cumulative sum along v). Use whiteness = 1 - exp(-t/τ) with τ ≈ 0.5-1 s, boosted by A1 speed. Real falls start glassy at the lip and whiten as they fall; this is the single biggest realism cue and needs nothing new.
-   **Patchy translucency**: alpha = saturate(thickness x lowfreq_advected_noise), with thickness ∝ 1/speed and alpha forced to 1 where whiteness is high. This needs the blended draw fixed (D56: the mesh must be registered with the blended pass). Until then, fake it with opaque but darker, refraction-less "thin" tint plus back-light transmission.
-   **Edge lace**: on open u-edges and void-adjacent texels (distance to void from the A1 bake), apply alpha erosion with an advected noise threshold.
-   **Clash foam**: at bake time, give each texel its distance to any _other_ falls piece of the site (closest point on the other meshes' triangles), store it in A, and draw foam where it is below about 16 units. This hides overlapping-piece seams.
-   **Rock foam**: the stagnation term upstream of rock plus a thin rim at SDF ≈ 0. Behind the rock, leave a wake of low alpha (a translucent hole) plus streaks.

---

## C. Hiding the join between sheet and pool

1. **Height fade, not depth fade** (D0). Soft-particle depth fades fail against transparent water, which writes no depth [16]. We know each pool's level analytically (D45). In the falls PS: fade = saturate((y_world - poolLevel) / 24), whiteness → 1 over the same band, and alpha erosion with advected noise, so the sheet "dissolves into foam" rather than meeting a line. Unreal's DepthFade does the same against opaque geometry [17].
2. **Shared foam band on both sides** (D0). Rasterize the plunge line (already found, D47/D50) into the pool's 2D SDF. The pool shader draws foam/lace by SDF distance with an outward-advected noise, and the falls shader draws its own band by height. Matching colour and noise scale makes it read as one foam mass.
3. **Impact rings and boils** (D1). As in Horizon [1], a ring/boil flipbook or a procedural expanding-ring normal perturbation in the pool shader along the plunge SDF, with phase = time - dist/c. A cheap procedural version needs no new geometry (D0).
4. **Push into the SWE** (already done, D55) for the far-field foam and current.
5. **Mist** (D1): a few soft, height- and depth-faded billboards or a short-range fog band at the plunge, driven by A1 speed at the outlet. D41-D51 had trouble with a site-wide fog; keep it local to plunge segments.

---

## D. Recommended architecture and order (most visual win per effort first)

1. **PS shading pass, D0, existing data.** Aeration with fall time (approximate t from v x rate per vertex for now), speed-scaled whitewater, edge lace, back-lit transmission, height fade and foam band at pools, and a noise phase offset on the flowmap. Small effort, large realism gain.
2. **Pool join, D0, then D1.** The pool shader's foam band from the plunge SDF, then procedural impact rings, then optional flipbook decals and local mist.
3. **A1 UV-space bake, D0.** CPU job per piece: rasterize UV → texel world pos/normal/Jacobian → ray parity solid mask and surface SDF → metric potential solve → forward clamp → fall-time cumulative sum → clash distance to other pieces. Output one RGBA16F per piece plus a small extra texture (fall time, clash) or packed channels. Bind it in Lighting/Effect/Utility. It replaces the per-vertex steering on curtains and feeds foam, translucency and spray.
4. **Fix the blended draw** (register with the blended pass) to unlock real patchy translucency. An optional screen-copy refraction is D1.
5. **Spray from the bake.** Emit at stagnation texels, at gaps (high speed) and at clash texels instead of the current heuristics.
6. **Subdivision/tessellation, D2**, for displacement (water mounding over rock, lips, impact bulges), using the same A1 texture for the displacement amount.

Avoid D3 (replacing meshes); it was already tried (D30).

---

## Sources

1. Malan, "Water in Horizon Forbidden West", SIGGRAPH 2022 Advances: https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Water-Malan.pdf
2. Stam, "Flows on Surfaces of Arbitrary Topology", SIGGRAPH 2003: https://history.siggraph.org/?p=115925 and https://www-int.autodesk.com/research/publications/flows-on-surfaces-of
3. Crane, Weischedel, Wardetzky, "Geodesics in Heat": https://arxiv.org/pdf/1204.6216
4. CGAL heat method: https://www.cgal.org/2019/01/23/Heat_Method/
5. Fisher, Schroder, Desbrun, Hoppe, "Design of Tangent Vector Fields", SIGGRAPH 2007: https://hhoppe.com/proj/vfdesign/
6. Vlachos, "Water Flow in Portal 2", SIGGRAPH 2010: https://www.advances.realtimerendering.com/s2010/Vlachos-Waterflow(SIGGRAPH%202010%20Advanced%20RealTime%20Rendering%20Course).pdf
7. Grimes, "Shading a Bigger, Better Sequel: Techniques in Left 4 Dead 2" / nonstandard textures, GDC 2011: https://www.hlportal.de/files/GDC2011_BronwenGrimes_NonstandardTextures.pdf
8. Catlike Coding, Flow series: https://catlikecoding.com/unity/tutorials/flow/texture-distortion/ and https://catlikecoding.com/unity/tutorials/flow/directional-flow/
9. Neyret, "Advected Textures", SCA 2003: https://diglib.eg.org/items/c4bf80cc-23f0-4107-958d-e03c0b9c8ac4/full
10. Trumpler, "Stylized VFX in RiME", Unreal Fest Europe 2018: https://www.unrealengine.com/events/unreal-fest-europe-2018/stylized-vfx-in-rime
11. "How we made waterfall in Season": https://realtimevfx.com/t/how-we-made-waterfall-in-season-a-letter-to-the-future/23420
12. Ang, "The Technical Art of Sea of Thieves", SIGGRAPH 2018: https://history.siggraph.org/wp-content/uploads/2022/09/2018-Talks-Ang_The-Technical-Art-of-Sea-of-Thieves.pdf
13. Gonzalez-Ochoa, "Rendering Rapids in Uncharted 4", SIGGRAPH 2016 Advances: https://www.advances.realtimerendering.com/ (2016 course); summary in https://github.com/acskyline/wave-particles-with-interactive-vortices
14. Grujic and Cutocheras, "Water Rendering in Far Cry 5", GDC 2018: https://gdcvault.com/play/1025033/Advanced-Graphics-Techniques-Tutorial-Water and https://80.lv/articles/the-gdc-talk-water-rendering-in-far-cry-5
15. Roblox, Create waterfalls with VFX: https://create.roblox.com/docs/tutorials/building/effects/creating-waterfalls
16. Unity discussion, soft particles on water: https://discussions.unity.com/t/soft-particles-on-water-shaders-possible/1562473
17. Unreal DepthFade: https://answers.unrealengine.com/questions/764425/index.html

Not found: public technical talks on the waterfalls of RDR2, Ghost of Tsushima (only general VFX: https://blog.playstation.com/?p=345372), The Last of Us or Witcher 3. Claims about those games are deliberately omitted.
