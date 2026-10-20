# Fluid Field — Decisions Log

One entry per decision. Research spikes (R1–R9) end with an entry here that
points to `research/R<n>.md`; build agents start from these entries, never from
their own guess about which technique to use.

Entry format:

```
## D<n>: <title>
- Date: YYYY-MM-DD
- Phase / spike: Phase <n> / R<n>
- Decision: <what was chosen>
- Alternatives rejected: <option — why>
- Decided by: <person>
- References: <research note, report, PR>
```

## D1: Branch from `dev`, not `main`

-   Date: 2026-10-13
-   Phase / spike: Phase 0
-   Decision: `feature/fluid-field` branches from `dev`.
-   Alternatives rejected: `main` — only updated by the release workflow and
    135 commits behind `dev`; upstream PRs target `dev`.
-   Decided by: @Victor
-   References: `.claude/CLAUDE.md` "Commits and branches"

## D2: AI context files

-   Date: 2026-10-13
-   Phase / spike: Phase 0
-   Decision: There is no `AI-INSTRUCTIONS.md` in this fork. `.claude/CLAUDE.md`
    is the primary AI instruction file, with `AGENTS.md`,
    `.github/copilot-instructions.md` and `CONTRIBUTING.md` as supporting
    guidance.
-   Alternatives rejected: creating a new `AI-INSTRUCTIONS.md` — would
    duplicate `.claude/CLAUDE.md`.
-   Decided by: @Victor
-   References: `docs/fluidfield/ROADMAP.md` "Working with AI agents"

## D3: Deploy builds straight into the Steam install

-   Date: 2026-10-13
-   Phase / spike: Phase 0
-   Decision: Local builds auto-deploy to the plain Steam install's `Data`
    folder (no mod manager), overwriting the installed Community Shaders, with no
    backup.
-   Alternatives rejected: backing up first; staging to a separate folder.
-   Decided by: @Victor
-   References: `CMakeUserPresets.json` (local, untracked)

## D4: Shape of the Phase 0 scaffold

-   Date: 2026-10-13
-   Phase / spike: Phase 0
-   Decision: Fluid Field follows LOD Blending, the smallest feature with
    saved settings: `src/Features/FluidField.{h,cpp}`, a `Settings` struct
    serialized with `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT`, and
    registration in `Globals.h`, `Globals.cpp` and `Feature.cpp`. Unlike LOD
    Blending it has no `CORE` marker, so it gets its own `Package-FluidField`
    zip as the roadmap asks. The `.ini` is `Version = 0-1-0`, `Alpha = True`.
    `Settings::Enabled` defaults to `false`. No shader define, no hooks, no
    per-frame overrides and no GPU resources.
-   Alternatives rejected: CORE feature (it would ship with the base mod);
    `Unreleased = True` (CONTRIBUTING.md reserves it for finished features that
    cannot ship yet); a shader define (it would add shader permutations for
    nothing).
-   Decided by: agent, pending review by @Victor
-   References: `docs/new-feature-template/NewFeatureReadme.md`,
    `src/Features/LODBlending.{h,cpp}`

## D5: Move Fluid Field to its own "Fluids" category (planned)

-   Date: 2026-10-13
-   Phase / spike: later phase, not yet scheduled
-   Decision: Fluid Field will move from `FeatureCategories::kWater` to a new
    "Fluids" category. That means adding `kFluids` to
    `src/FeatureCategories.h`, plus any i18n key the category label needs,
    and changing `FluidField::GetCategory()`.
-   Alternatives rejected: staying under Water. Fluid Field also drives fog,
    smoke and buoyancy.
-   Decided by: @Victor
-   References: `src/FeatureCategories.h`, `src/Features/FluidField.h`

## D6: Phase 0 gate passed

-   Date: 2026-10-13
-   Phase / spike: Phase 0 gate
-   Decision: Phase 0 is closed and Phase 1 can start with spike R1. The
    in-game checklist passed with no issues. The empty Fluid Field feature
    loads, appears in the menu, saves its setting, toggles cleanly and costs
    nothing when off.
-   Alternatives rejected: none.
-   Decided by: @Victor (in-game test)
-   References: build `ca4a6a665`, deployed to the Steam install

## D7: R1 — adopt the CPU data map (Prototype B)

-   Date: 2026-10-13
-   Phase / spike: Phase 1 / R1
-   Decision: Build the depth and obstacle map from CPU data, as described
    for Prototype B in `research/R1.md`: landscape heights from
    `TESObjectLAND`, water surfaces from `TESWaterSystem::waterObjects` with
    the cell-water fallback, and downward raycasts for statics. Build it as
    real Phase 1 code behind a debug setting, not as a throwaway prototype.
    The section 8 measurements and success criteria are its gate. Stages,
    each with a commit and an in-game checklist:
    1. Cell cache of landscape heights, including the absolute-Z check
       against `TES::GetLandHeight`.
    2. Water surfaces from `waterObjects`, with the cell-water fallback.
    3. Debug overlay of bed, surface and depth. Stop here so a human can
       judge how much riverbed on the test routes is landscape versus
       placed meshes (R1 open question 3).
    4. Downward raycasts on the main thread under `bhkWorld::worldLock`,
       with a per-frame ray cap. Only after stage 3 is reviewed.
       The map reuses the toroidal grid and addressing from `ff/grid-core`
       instead of a second implementation.
-   Amended 2026-10-14 (@Victor): the stage 3 debug overlay (bed, water
    surface, depth) is built together with stage 2, so progress is visible in
    game early. It is the layer overlay's second row. The stage 3 review of
    landscape against placed riverbed meshes still happens on the test routes
    before stage 4.
-   Amended 2026-10-14 after the route run (`reports/r1-route-run.md`):
    stage 4 classifies `kGround` as landscape; when the first hit is above the
    water, an upward probe from under the surface separates overhead
    structures (water flows under) from solids rising out of the water
    (obstacles); and the first 90 updates after a load or teleport get a
    larger ray budget (0.5 ms) so the area refills quickly. A gallop at
    0.116 ms, just over the 0.1 ms target, is accepted for now.
-   Alternatives rejected: Prototype A (top-down depth capture through the
    precipitation-occlusion path) is deferred, not rejected. It is the
    documented fallback, built only if B fails the success criteria on the
    test routes. The other rejected options are listed in `research/R1.md`
    section 4.
-   Decided by: @Victor
-   References: `research/R1.md`

## D8: First visible milestone is a "flowing water slice"

-   Date: 2026-10-13
-   Phase / spike: Phase 1–3 bridge, ahead of R2
-   Decision: Build a "flowing water slice" as the first visible result. Goal:
    in Riverwood's river, water visibly flows, the player's wake drifts
    downstream, and waves reflect off the banks. Scope:
    -   Bed and surface from R1's CPU map (D7), landscape only; raycasts
        come later.
    -   A basic virtual-pipe shallow-water solver on the grid core, not the
        full R2 spike. River currents are seeded from vanilla water flow if
        it is readable, otherwise a constant test current.
    -   The player is the only input.
    -   Output normals, plus flow if the water shader can take it, into the
        water shader, fading to vanilla near the grid edge.
    -   A menu toggle, Tracy zones and a reset after loading screens.
    -   A clean interface (map and inputs in; height, flow and normals out)
        so the R2 solver can replace the basic one later.
-   Refinements (2026-10-13, same day):
    -   Principle: extend what Community Shaders and Skyrim already do; do
        not reimplement it.
    -   Order: build the slice right after D7 stage b, once `ff/grid-core`
        has merged. The slice's debug overlay doubles as the stage c review
        (how much riverbed is landscape). Raycasts (stage d) come after the
        slice.
    -   Currents: seed from Unified Water's world flowmap through a one-line
        read-only accessor in `UnifiedWater.h` (approved). Use a constant
        current when Unified Water is off.
    -   Blending in `GetWaterNormal` (`package/Shaders/Water.hlsl`):
        -   Add Fluid Field's large-scale wave normals on top of the base and
            flow normals with `ReorientNormal`.
        -   Keep vanilla wading ripples. Tune the player input so Fluid Field
            does not draw rings at the same scale.
        -   Keep Wetness Effects raindrops, but drift them with Fluid
            Field's flow where it covers the water.
        -   Sample at the Water Parallax-shifted position.
        -   Fade everything to vanilla near the grid edge.
        -   Limit the `FLUID_FIELD` define to the water shader.
    -   Vanilla `ISWaterFlow` actor wakes only exist with Unified Water off.
        Check in game whether they double Fluid Field's wake.
-   Amended 2026-10-14 (@Victor): the slice's solver is height-and-velocity
    shallow water on a staggered grid with semi-Lagrangian advection
    (Bridson & Mueller-Fischer 2007; Mueller, GDC 2008), not a virtual-pipe
    model. Pipes do not carry waves with the current, so a wake would not
    drift downstream without an extra advection step. It is also an R2
    candidate. Built in two steps: the simulation with an overlay view
    (`ShallowWaterSolver`, `WaterMapBuilder`, test current), then the water
    shader output and currents from Unified Water's flowmap.
-   Alternatives rejected: a "ripple slice" (normals-only ripples). It
    cannot show flow, wakes carried downstream or reflections, which vanilla
    displacement cannot do either. No ripple-slice plan had been written
    down in this log before this entry.
-   Decided by: @Victor
-   References: `reports/unified-water.md` sections 5.1, 5.2 and 6;
    `research/R1.md`; D7

## D9: Community Shaders Discord builds are reusable with credit

-   Date: 2026-10-13
-   Phase / spike: all
-   Decision: Code shared on the Community Shaders Discord, starting with
    Dwemer Engineer's Procedural Grass test build, is treated as Community
    Shaders code under GPL-3.0-or-later. Fluid Field may study it and reuse
    it. Reused code keeps a credit to its author in the file header and the
    commit message. Shipped shader source is the first material to work from.
    Amended the same day: the build's DLL may also be decompiled with
    Ghidra, using its shipped PDB, scoped to the Procedural Grass code.
    Decompiled output is reference only. Anything reused is rewritten,
    with credit.
-   Alternatives rejected: study only, copying only after the author
    publishes the source or gives permission.
-   Decided by: @Victor
-   References: `ROADMAP.md` "License obligations",
    `reports/procedural-grass.md`

## D10: Phase 5 becomes simulated fire and smoke

-   Date: 2026-10-13
-   Phase / spike: Phase 5 / R5
-   Decision: Phase 5 is widened from "volumetric smoke" to fire and smoke
    simulated as fluids, in the spirit of FluidNinja LIVE. Near the player,
    the most visible emitters (campfires, forges, chimneys, torches, fire
    spells) get small local fluid grids with velocity, density and
    temperature, driven by the field's wind and shouts. These replace the
    vanilla sprites, which fade back in beyond a distance band and an
    emitter cap.
    -   Vanilla data stays the source: emitter positions, sizes and colours.
    -   Community Shaders' lights do the lighting.
    -   Particles (sparks, embers) are an optional extra, not the core.
    -   R5 is rescoped to compare the techniques (camera-facing 2D cards,
        extruded volumes, small 3D grids) and to measure a fire budget.
    -   The 1.5 ms fog-and-smoke budget is unchanged until R5 reports.
-   Alternatives rejected:
    -   Smoke-only volumes with vanilla fire left untouched.
    -   Only distorting or bending the vanilla sprites with the field
        (`research/smoke-and-fire.md` options a and b). These may still ship
        as cheap fallbacks for emitters beyond the cap.
-   Decided by: @Victor
-   References: `research/smoke-and-fire.md`,
    `research/fluidninja-live2.md`, `ROADMAP.md` Phase 5 and R5

## D11: Phase 1 grid and wind solver shape

-   Date: 2026-10-13
-   Phase / spike: Phase 1 (grid core; R1 still open)
-   Decision: The grid is centred on the player's position, snapped to whole
    cells, and addressed toroidally: world cell `c` always lives in storage
    texel `c mod GridRes`, so scrolling only changes `GridMin`. Resolutions are
    powers of two (64 to 512) so the shaders wrap with a bitmask. Defaults:
    256 x 256 over 4096 units (16 units per cell), 30 Jacobi iterations. Wind is
    a 2D stable-fluids step (add forces, semi-Lagrangian advection, divergence,
    Jacobi pressure from zero each step, projection) on `R32G32_FLOAT` velocity
    and `R32_FLOAT` pressure and divergence. The edge is open: zero-gradient
    velocity, zero pressure. There are no obstacles until R1 lands. The solver
    lives in `src/Features/FluidField/WindSolver.{h,cpp}`, which depends only on
    D3D11 and C++/WinRT.
-   Alternatives rejected: centring on the camera eye like Grass Collision (the
    roadmap says player-centred; third-person offset is small next to the grid);
    warm-starting pressure from the previous step (would need its own scroll
    invalidation and makes the CPU reference harder); 16-bit formats (revisit if
    the 0.5 ms budget is tight).
-   Decided by: agent; approved by @Victor on 2026-10-14
-   Follow-up: starting the pressure solve from zero leaves the wind
    "soft" (30 iterations remove only about half the divergence). Revisit
    reusing last frame's pressure, with scroll-aware clearing of new
    strips, once devbench timings and the in-game look are in.
-   References: `features/Fluid Field/Shaders/FluidField/Common.hlsli`

## D12: Solver referee is a standalone WARP test

-   Date: 2026-10-13
-   Phase / spike: Phase 1
-   Decision: `tests/FluidField` is its own CMake project, not part of the root
    build. It compiles the real `WindSolver.cpp` and HLSL, runs them on a WARP
    device and compares every layer with a CPU implementation (one step from
    rest, then a scrolled second step). It needs no GPU, no game and no vcpkg.
-   Alternatives rejected: Catch2 (an extra framework for one test); an in-game
    self-test button (agents cannot run it); an option in the root
    `CMakeLists.txt` (that file is not gersemi-clean on `dev`, so touching it
    drags unrelated reformatting into the commit).
-   Decided by: @Victor (chose the WARP exe), layout by agent
-   References: `tests/FluidField/CMakeLists.txt`

## D13: Third-party mods, and the fire, smoke and fog target

-   Date: 2026-10-14
-   Phase / spike: Phases 2–5
-   Decision:
    -   SurfaceTides is not relevant to Fluid Field.
    -   Particle Wind is set aside for now; Fluid Field uses its own wind.
        Detecting it to avoid pushing particles twice is revisited later.
    -   Wade In Water Redone is studied for actor submersion and inputs
        (`reports/wade-in-water.md`).
    -   Embers XD and similar fire mods are card and particle effects, not
        simulations. Phase 5 replaces such effects near the player rather
        than integrating with them; they remain test targets.
    -   Target for the later phases: realistic, simulated-looking fire, smoke
        and fog within a set distance of the player, with the distance to be
        decided. Beyond it, vanilla (or replacer) effects stay.
-   Alternatives rejected: depending on Particle Wind for wind on particles;
    detecting fire replacers by file path.
-   Decided by: @Victor
-   References: `research/existing-mods.md`, D10

## D14: Obstacles at the water line come from cutting meshes with the water plane

-   Date: 2026-10-14
-   Phase / spike: Phase 1 (R1 stage d follow-up), water slice 1
-   Decision: Find where water can be at the water line by cutting the
    vanilla water plane against the static meshes (and landscape) near the
    player on the CPU: intersect each mesh's triangles with the plane, mark
    the crossed cells and flood-fill open water inward. Cut each reference
    once, since statics do not move. Data path, in order of preference:
    -   the meshes' CPU vertex and index copy (`BSGraphics::TriShape`
        `rawVertexData` and `rawIndexData`)
    -   Havok collision queried with a thin box at the water line
    -   GPU vertex buffer readback, as a last resort
        Raycasts stay for riverbed depth. Staged as in `research/mesh-cut.md`:
        stage 0 surveys which statics near water keep CPU vertex data.
-   Alternatives rejected:
    -   Deciding open versus solid with up and down raycasts. Thin decks,
        collision that differs from visuals, and sampling limits kept it
        wrong at bridges after four rounds.
    -   A render-based water-line slice (Prototype A). It needs the contested
        render hooks, and the mesh cut gives the same answer without them.
-   Decided by: @Victor
-   References: `research/mesh-cut.md`, `reports/water-slice-1-round-3.md`

## D15: Mesh-cut obstacles replace the raycast solid test

-   Date: 2026-10-14
-   Phase / spike: Phase 1, water slice 1 (D14 stages 0 to 3)
-   Decision: Water-line obstacles come from `MeshCut`, from the meshes'
    CPU vertex and index copies. For each loaded static, movable static,
    activator, furniture, door or container that has collision and whose
    bound spans the local water height, each triangle is cut at that
    height. Every segment is drawn as a line of solid fine points, and the
    inside of each closed loop is filled, on the bed rays' fine lattice
    (RaySpacing / 4). Each reference is cut once while it stays loaded.
    Solid points go into the existing obstacle blocks, over the rays' detail.
    Rays now only measure the bed: with Mesh Cut on, a mesh above the water
    is always looked under, and no point is judged solid from a probe.
    Setting `MeshCutObstacles`, on by default.
-   Evidence (from the RE workspace, `skyrim-engine-re/kb/evidence/2026-10-14-live-bridge-mesh-cut.md`):
    the Riverwood stone bridge cut gives one closed loop per pillar and
    nothing across the arches. 160 references at the Riften canal and 602
    at the lake docks were surveyed: every dock, pier, wall, bridge and boat
    keeps CPU data, and only tree shapes do not. Cutting takes 1 to 14 ms
    per reference in Python.
-   Alternatives rejected:
    -   Keeping the normal-based probe rule. It got the arches right, but
        missed every pillar (bridge test, 2026-10-14).
    -   Havok collision as the source. It needs a shape-type walk and the
        visual mesh is already available.
-   Decided by: @Victor ("yep, quit and deploy", after the survey)
-   References: D14, `research/mesh-cut.md`

## D16: Phase 1 soak deferred; water slice step 2 next

-   Date: 2026-10-14
-   Phase / spike: Phase 1 gate, water slice (D8)
-   Decision: The Phase 1 gate's hour-long soak (stability, cell transitions,
    loading screens) is deferred. It must run before the v0.1 beta gate.
    Work moves to water slice step 2: Fluid Field's water drives the vanilla
    water shader's normals (D8 refinements), so the simulation is visible in
    the world.
-   Status of the rest of the gate: the GPU budget is met so far (wind about
    0.20 ms, water passes about 0.05 ms, RTX 5060 Ti at 1440p, 2026-10-14
    profiler); cell transitions and loading screens worked through the day's
    testing but were not soaked.
-   Decided by: @Victor ("I'd rather move on to actually making visible
    changes to the water")
-   References: ROADMAP Phase 1 gate, D8, `reports/mesh-cut-1-test.md`

## D17: Wakes from every actor replace vanilla's displacement trail; currents from the flowmap

-   Date: 2026-10-14
-   Phase / spike: water slice (D8), step 3; pulls Phase 2's actor input forward
-   Decision: Inside the grid, Fluid Field's waves replace vanilla's wading
    displacement trail: the shader scales vanilla's displacement by
    (1 - Fluid Field coverage), and every nearby actor (player, NPCs, horses,
    creatures; nearest first, up to 16) feeds the simulation. Waves come only
    from entities that are not fully submerged (the top of the bounding box
    is above the water), which also covers animals and objects. Falls and jumps into water
    push the water down at the entry point. Currents come from Unified
    Water's flowmap (D8 accessor), pulling the water's velocity toward it.
    Plan: `plans/water-slice-step-3.md`.
-   Alternatives rejected:
    -   Patching the engine to stop the player's ripples only. NPCs would keep
        vanilla's fake trails next to the player's real one, and it needs an
        engine patch.
    -   Turning vanilla's displacement off everywhere. Actors outside the
        simulation would lose their trails.
-   Decided by: @Victor (option A, "it also needs to account for jumps")
-   References: D8, D16, `plans/water-slice-step-3.md`

## D18: Water gets its own grid and a far level

-   Date: 2026-10-14
-   Phase / spike: water slice (D8, D17)
-   Decision: The water no longer shares the wind's grid. It has its own
    resolution and size (Water Resolution 512, Water Size 4096) and two
    levels: the near level at full detail (mesh-cut obstacles, bed detail,
    wakes) and a far level at the same resolution over Far Water Scale times
    the width (4x by default: 16,384 units at 32-unit cells), driven by the
    coarse water map and the river current. The water shader cross-fades
    near into far over the near level's 15% edge band, and the far level
    fades into vanilla at its own edge. The levels do not exchange water yet.
    The wind grid returns to 256 by default.
-   Why: the seam at the edge of the simulation (water slice reports of
    2026-10-14). A single 1024 grid over 8,192 units cost 0.98 ms on the GPU,
    two-thirds of it in the wind pressure solve, which the water does not use;
    two 512 water levels cost about 0.2 ms and reach twice as far.
-   Alternatives rejected: one large grid (cost); a far level fed back into
    the near level's border (two-way coupling) - later, if the hand-over
    still shows.
-   Decided by: @Victor ("if feasible, we can drop the detail after X radius
    and blend"; "bigger draw distance for the simulation and a smoother
    blending")
-   References: D17, `plans/water-slice-step-3.md`

## D19: Procedural foam replaces vanilla's placed foam and spray

-   Date: 2026-10-14
-   Phase / spike: water slice (D8, D17, D18)
-   Decision: Inside the simulated area, vanilla's placed foam, splash and
    spray effects are hidden at runtime (`SetAppCulled` on their 3D;
    nothing is saved): models under `Effects\` whose path contains rapids,
    splash, churn, foam, spray, mistblast, skirt, waterfallmist or ripple,
    sitting on water. Waterfall bodies and ambient low mist stay. They show
    again outside the area or with "Replace Vanilla Foam" off.
    Fluid Field makes its own foam: a foam pass per water level advects foam
    along the simulated flow, fades it over about 3 s, and adds foam from
    shear, flow into obstacles and banks, breaking flow (Froude number), and
    wakes and splashes. The water shader draws it lit by the surface's direct
    and ambient light, broken up by vanilla's normal texture dragged along the
    flow, before fog; foam is opaque. Setting: Foam.
-   Alternatives rejected: keeping vanilla's spray meshes until particles
    exist (@Victor wants them gone now); particles for spray (later, with
    Phase 5).
-   Decided by: @Victor ("Objective A) remove vanilla foam and splashes;
    objective B) add procedural foam derived from our simulation")
-   References: D17, D18

## D20: Rivers flow, lakes don't: width decides, and the simulation steers vanilla's flow normals (2026-10-14)

**Context.** Part of Lake Ilinalta uses a water type with `kEnableFlowmap`, and Unified Water's generated flowmap is as strong under the lake (intensity 0.84) as under the Riverwood rapids (0.84 to 0.92). The lake ran at full River Speed: foam everywhere, and splashes swept away. Vanilla's flowmap normals also kept scrolling out of an island where the simulated water was still.

**Decision (the user chose width-based).** Each texel's flow is scaled by its openness: the fraction of water on rings around it (radii 512 to 4096 units, 16 directions), measured on a 256-unit lattice and remembered. Full current up to 0.3 (a river is a band through the rings, about 0.1 to 0.25), still from 0.55 (a lake surrounds the point), smoothstep between. A first version measured inside the water map itself, but the near map is only 4096 units across, so a river widening by the Riverwood bridge read as open and stopped flowing. Where Fluid Field covers the water, vanilla's flowmap sample takes its direction and scroll speed from the simulated flow, and its flow normals fade to vanilla's still-water ripples where the simulated water is still.

**Consequences.** Very wide river reaches flow slower, which is also what real rivers do. Lake shores are partly open (about 0.5), so they keep a weak drift. No per-place data to maintain.

**Revised (same day).** The width rule is withdrawn: neither the water map's own window nor rings out to 4096 units told the Riverwood river from the lake, and the river lost the current that was working well. Rivers are back to full flowmap current. The still-water fade stays. Steering vanilla's flowmap sample (direction and scroll speed) from the simulated flow was also withdrawn: vanilla scrolls its flow normals by the game time times the flowmap's speed and rotates them by its direction, so any change in the steered values jumps the texture by an amount that grows with the time, and the water flickered. Lakes with a flowing water type need another way to be told apart.

## D21: Interactive ground fog, drawn by Fluid Field itself

-   Date: 2026-10-15
-   Phase / spike: Phase 4, first slice (replaces the new renderer R4 planned)
-   Decision: A fog layer on the wind grid (how much fog lies on each cell)
    is carried by the wind and the weather's wind, carved where moving
    actors pass, thinned where they stir the air, and refills toward a
    resting pattern of banks and clearings. Actors push the wind as they
    move. Fluid Field draws it itself: after the deferred composite (before
    water and effects) a compute pass marches each pixel's view ray up to
    the scene depth through fog lying on the ground (ground height from the
    landscape cache under the wind grid, density falling off with height),
    lit by the sun (forward scattering) and the ambient light. Settings:
    Ground Fog, Fog Density, Fog Thickness, Fog Stirring, Fog Refill.
    Plan: `plans/fog-slice.md`.
-   Alternatives rejected: feeding Exponential Height Fog's volumetric fog
    (built first, then withdrawn the same day at @Victor's request: it ties
    the effect to that feature's volumetric mode, which he keeps off, and
    toggling it at runtime crashed the game once while shaders swapped); a
    camera-aligned froxel renderer of our own (more than a ground layer
    needs).
-   Consequences: the fog is drawn before water, so it is not drawn over
    water surfaces yet; it is not shadowed by the sun yet; the wind ignores
    walls, so fog can be pushed through buildings. Needs one call from
    `Deferred::DeferredPasses`.
-   Decided by: @Victor ("can we try something that isnt based of
    exponential height fog? ... just get ground height and use the fluid
    dynamics we already have")
-   References: D11 (wind grid), D13 (fog target), ROADMAP Phase 4

## D22: Waterfalls drive the water where they land

-   Date: 2026-10-15
-   Phase / spike: water slice follow-up (plan: `plans/waterfalls.md`)
-   Decision: Vanilla's falling water stays. Waterfall bodies (models under
    `Effects\` with waterfallbody or waterfallthin) are found among the loaded
    references; each one's plunge, width and fall direction come from its
    lowest vertices (bounds as a fallback). A body whose lowest point is near
    a water surface becomes part of a fall (bodies within 400 units merge;
    nearest 6 kept). Each fall is a water emitter on both levels: a pulsing
    depth push, the water spreading out from the plunge (the emitter's spare
    floats carry a radial speed and a foam rate) and running on in the fall's
    direction, and foam. Settings: Waterfalls, Waterfall Strength.
-   Alternatives rejected: replacing vanilla's base effects now (@Victor:
    "we want the water body before the fx"; later); near level only (he wants
    the far level driven too).
-   Decided by: @Victor
-   References: D17 (emitters), D19 (foam), `plans/waterfalls.md`

## D23: Fluid Field draws its own waterfalls in place of vanilla's

-   Date: 2026-10-15
-   Phase / spike: water slice follow-up (plan: `plans/waterfalls.md`)
-   Decision: Vanilla's waterfall body meshes are hidden near the player
    (runtime only) and replaced by curtains Fluid Field builds and draws. The
    vanilla mesh is only a guide: its top band gives the lip (position, width,
    direction), its bottom band the plunge. Each curtain is a grid of columns
    across the lip and rows down the fall: every column follows a ballistic
    arc from the lip (initial speed chosen so a free fall reaches vanilla's
    plunge), and downward Havok rays find the rock under it; where the arc
    would pass into rock, the water lies on the rock and slides down it. The
    shape is built once per body and cached. A render pass after the world
    (before the ground fog) draws the curtains with depth testing and alpha
    blending, using the camera snapshot from the deferred passes, with a
    falling-water shader (acceleration streaks, thinning and breakup, aeration
    near the base, wind sway). Splashes, spray and mist at the base stay
    vanilla until particles exist. Setting: Replace Waterfalls.
-   Alternatives rejected: a new look for vanilla's meshes only (@Victor wants
    the falls replaced); a free ballistic arc alone (it would cut through rock
    on sloped falls; @Victor: "conforming a bit to the mesh below").
-   Decided by: @Victor
-   References: D21 (render after world, camera snapshot), D22

## D24: Waterfalls are simulated water, drawn by a sim surface pass

-   Date: 2026-10-15
-   Phase / spike: water slice follow-up; supersedes D23's traced curtains
-   Decision: Waterfalls become part of the shallow-water simulation, as
    Fluid Flux does it. Around each waterfall body, downward Havok rays map
    the rock (a "fall bed" on the near level, toroidal like the grid); the
    solver's bed is the higher of the map and the fall bed, so the river above
    pours over the lip and runs down the real rock surface by gravity,
    splitting around boulders, speeding up and foaming like the rest of the
    simulated water. Inside fall areas, cells without vanilla water are not
    pulled back to dry; a slow drain removes thin films instead. A new render
    pass draws the simulated water wherever there is no vanilla water plane:
    a heightfield mesh of bed + depth over the fall areas, shaded as water
    (refraction of the scene behind it, sky reflection, sun glint, simulated
    foam, white breakup where the water is steep and fast). Vanilla bodies
    stay hidden; the traced curtains (D23) are removed.
-   Alternatives rejected: improving the traced curtains (they stay a
    separate effect that does not react to the simulation); a free-fall sheet
    and particles for every fall (most of Skyrim's falls slide down rock;
    kept as a later addition for true vertical drops).
-   Consequences: near level only at first (8-unit cells); vertical drops
    become one-cell steps in the heightfield (automatic ramps if needed);
    rays over each fall area once, spread across frames.
-   Decided by: @Victor ("yeah go ahead"), after research into FluidNinja,
    Fluid Flux and game waterfall techniques
-   References: D22, D23, `plans/waterfalls.md`

## D25: Waterfalls reconstructed from vanilla's layout; the SWE never carries free fall

-   Date: 2026-10-15
-   Phase / spike: water slice follow-up; supersedes D24 (and D23's curtains)
-   Decision: Adopt @Victor's design (`plans/waterfall-replacement.md`):
    sites are reconstructed from vanilla's water planes and waterfall meshes
    (inventory, clustering, level pairing and tiers), the rock is captured,
    lips are extracted at the top level, the fall is traced once per site
    (ballistic, clinging, ledge re-entry), drawn as custom translucent sheets
    with physically derived thickness and aeration, and coupled to the SWE at
    impact with mass balanced at the lip. The SWE stays confined to vanilla's
    water regions and is never used for free fall. Every stage gets an in-game
    debug view before the next starts; milestones M1 to M6 as in the plan.
-   Alternatives rejected: D24 (the SWE running down the rock): a heightfield
    cannot hold near-vertical drops, so it overflowed, spiked at lips and
    spread over the rock; tuning could not fix that.
-   Decided by: @Victor ("Yeah go on full speed ahead")
-   References: D22, D23, D24, `plans/waterfall-replacement.md`

## D26: Keep vanilla's waterfall meshes and ground them on the captured rock

-   Date: 2026-10-15
-   Phase / spike: D25 follow-up; replaces D25's sheet drawing (S7, S10)
-   Decision: Vanilla's falling-water meshes stay, with their authored
    textures and scrolling. D25's capture (S4) feeds a world-space ground map;
    CS's effect shader, under `FLUID_FIELD` and only for draws FluidField
    flags as a grounded falling-water mesh (BSEffectShader SetupGeometry,
    constants at b13, ground at t120), bends their vertices onto the rock
    (out of rock sideways off steep faces, else up; eased down where they
    hover just above it), and softens them into the rock with foam along the
    contact using the post-z-prepass depth copy (t121). The pixel shader also
    flows the texture around rock in the way: the mesh's flow direction comes
    from its texture coordinates (down the fall along v), rock near the
    surface turns it aside, and a two-phase flowmap carries the turn, with
    foam in the wake. Ground Waterfalls toggles it for A/B against vanilla.
    Revised the same day: a rock field built once on the CPU from the
    capture (prominence over the bed, smoothed presence, push away from rock)
    drives a static u-shift of the streaks, a foam collar and pillow, and a
    thinned V wake; vertices move only a little of the push (they tore at
    full push). Tested at the White River falls: the capture is right, but the
    water still reads as cutting through boulders. Next (agreed with
    @Victor): a shallow-water solver on the fall surface, with the captured
    rock as walls, driving the texture flow and the mesh; then blending where
    two falling-water meshes meet; later, vanilla's waterfall mist made
    volumetric through the ground fog.
-   Alternatives rejected: D25's own traced sheets (ribbons per stream): they
    read as flat paper with combs at the lips and strands across the rock;
    shading invented from scratch looks painted next to vanilla's authored
    falls. Removed: the sheet renderer, lip finding and stream tracing (S5 to
    S7), mesh hiding (S10), Replace Waterfalls, and D24's FallBed; git history
    keeps them.
-   Decided by: @Victor ("why cant we base it off the existing meshes but add
    some grounding to it?"; "if we can get those water meshes to interact
    with cliffs/rocks and with each other, we're golden")
-   References: D25, `plans/waterfall-replacement.md`

## D27: Waterfalls flowed by the water solver

-   Date: 2026-10-15
-   Phase / spike: D26 follow-up
-   Decision: A third instance of the shallow-water solver runs over the
    waterfalls around the player (512 texels of 16 units). Its bed is the
    captured rock, walls where none is captured; the surface it relaxes
    toward is vanilla's falling-water meshes rasterised from above, plus each
    site's pools, and only where that surface stands above the rock, so
    boulders through the water are dry and the water must go round them.
    The effect shader samples its velocity and foam at each pixel of a
    grounded mesh: the sideways velocity carries vanilla's streaks as a
    two-phase flowmap, on top of the rock field's fixed bend, and the
    solver's foam whitens the water. The texture warp alone (D26) did not
    part the flow. Mesh deformation from the solver (depth and velocity
    moving the vertices) is the next step after this.
-   Alternatives rejected: deriving a potential flow or stream function on the
    CPU (no wakes, more new code); the D24 approach of drawing the solver's
    water itself (looked wrong on steep rock). Here the solver only supplies
    velocity and foam; vanilla's meshes stay what is seen.
-   Decided by: @Victor ("we need something like the solver for the shallow
    water to divert it"; "use the solver to drive mesh deformation" later)
-   References: D22, D24, D26

## D28: Waterfalls follow a steady flow, solved once per site

-   Date: 2026-10-15
-   Phase / spike: replaces D27
-   Decision: The falls' shallow-water solver is removed. Each waterfall site
    gets a steady flow, solved once on a worker thread on its capture's
    16-unit lattice: the drive is the unit downhill direction of vanilla's
    falling water seen from above (weaker where nearly flat), made
    divergence-free and tangent to the rock by a pressure projection (rock
    standing out of the water is a wall; water near the site's top comes in,
    near its bottom leaves). Its stream function replaces the across
    coordinate (u) of vanilla's falling-water texture, so every streak follows
    a streamline: it parts round a boulder, cannot cross it, closes up behind
    it, and crowds through gaps. Foam marks where the flow stalls beside rock
    (piling up, eddying behind) and where it squeezes past. The debug
    Streamlines view draws the stream function as bands.
-   Alternatives rejected: D27's time-stepped shallow-water solver on the
    falls. It assumes nearly level water (depth measured vertically), so
    steep and vertical falls are outside it; falling water exceeds its
    stability speed cap, so speed saturates and swirl amplifies noise (its
    debug view showed a chaotic velocity field); and a velocity field only
    bends vanilla's streaks sideways, it cannot make them become streamlines.
    It stays for rivers and pools, where it works.
-   Decided by: @Victor ("Take a good look at the solver and if it's at all
    applicable to this situation, if it can be improved or outright
    replaced"; "yeah lets try that")
-   References: D26, D27, `plans/waterfall-replacement.md`

## D29: Only the falls' lit water surface; grounding and flow move to it

-   Date: 2026-10-15
-   Phase / spike: D26 and D28 follow-up
-   Decision: Each vanilla falling-water model is several parts (logged by
    FallSites): an inner surface drawn by the lighting shader
    (`...Inner01`, `...SlopeMesh01`; BSLightingShaderProperty), a white-water
    sheet (`...Foam`, FXWhiteWater01), streak layers (`L2_whiteCrossStream`,
    `fallsMesh`, `Object0x`; FXfluidTile01), and spray particle systems
    (`PCloud...`). Isolating them in game showed the inner surface is the
    water itself, clean and bright; the sheet is a brown translucent overlay
    and the streak layers are the brown ragged strands. While waterfall
    effects are hidden (Hide Waterfall Effects), only the inner surface now
    shows (FallLayerMask, default 1). Grounding and the streamline mapping
    move to it: CS's Lighting.hlsl, under `FLUID_FIELD` and only for flagged
    draws (BSLightingShader SetupGeometry), bends its vertices onto the rock
    and replaces its texture's across coordinate with the stream function.
    Every D26 to D28 effect-shader change had been applied to the overlay
    layers only. The lit surface gets the streamline texture only, in the
    pixel shader: it is drawn after a depth prepass with an equal depth test,
    so any change to its vertex position (even recomputing it with a
    different formula) fails that test and leaves the surface brown. Tested
    at the White River falls: clean water whose streaks part round the
    boulders.
-   Decided by: @Victor ("leave only the water surface for us to work on")
-   References: D26, D28

## D30: The falls' own dense surface, built from their flow

-   Date: 2026-10-15
-   Phase / spike: D28 and D29 follow-up
-   Decision: Each site's solved flow (D28) builds one dense mesh, a vertex
    per 16-unit water cell, on the falls' surface (vanilla's meshes seen
    from above, lightly smoothed): one surface per site, so the seams where
    vanilla's meshes overlap are gone. Its texture coordinates follow the
    flow: across is the stream function, along is the travel time from where
    the water comes in (solved by fast sweeping), scrolling with time, so
    vanilla's water texture is laid on the flow without shear and stretches
    where the water speeds up. The vertex shader displaces it: a mound over
    rock just under the water, standing waves where fast water runs shallow,
    ripples carried downstream. It is drawn after the world by FluidField
    (FallSurfaceRenderer, FallSurface.hlsl) with vanilla's falling-water
    textures, lit by the sun and the sky; vanilla's falling-water models are
    hidden at sites with a surface (Replace Waterfall Surface). Where the
    falls meet still water (where they come in and leave) the water foams
    over two cells, masking the join.
-   Alternatives rejected: displacing or tessellating vanilla's lit surface
    (its depth prepass must match exactly, D29); swapping its renderer
    buffers for a denser mesh (ownership on cell unload).
-   Decided by: @Victor ("now we need a higher poly mesh for the water as
    well as some sort of tesselation/displacement based on the flow"; "fix
    the fucking seams betwen the meshes")
-   References: D28, D29

## D31: Falls' surface from vanilla's meshes; GPU spray and mist

-   Date: 2026-10-16
-   Phase / spike: D30 revision and follow-up
-   Decision: The falls' surface is built from vanilla's own falling-water
    meshes (each model's lit surface, else its largest effect layer), not a
    plan-view grid: the grid version was flat and slow (@Victor: "waaay too
    shallow and slow"). Per site on a worker thread: near-duplicate layers
    (within 3 units) dropped, each piece subdivided to about 12-unit
    triangles, coincident vertices welded, the solved flow's coordinates at
    every vertex (carried six cells past the water's edge). One merged mesh
    per site, so the seams between vanilla's pieces are gone. Vanilla's
    falling-water texture runs along u, so the travel time (with the drop
    below the falls' top counted in) drives u and the streamlines v. Colour
    matched to vanilla by measuring captures. Water within 20 units of a
    site's level foams (the joins with the water above and below). Spray and
    mist are stateless GPU particles: emitters where the surface foams most,
    each particle's position computed from its emitter, a hash and the time
    (spray thrown up and out under gravity; mist rising and fading), drawn as
    camera-facing quads after the surface.
-   Decided by: @Victor ("combining the OG vanilla shapes and tesselating
    them with the flow"; "merged them into one big mesh"; "foam and splashes
    and gpu particles")
-   References: D28, D30

## D32: Vanilla's falling water, moved by the flow as a flowmap

-   Date: 2026-10-16
-   Phase / spike: D31 revision
-   Decision: The falls' own merged surface (D30, D31) is off by default
    (@Victor: "the water itself looks shit"); vanilla's falling-water meshes
    show again (lit surface, white-water sheet, streaks; vanilla's particles
    and mist/splash meshes stay hidden) and the GPU spray and mist (D31) are
    kept over them. Their textures are now carried by the falls' solved
    velocity as a flowmap, replacing D28's stream-function u remap: two
    phases half a cycle apart cross-fade, each reset before it stretches
    (0.5 s cycle, at most 1.5 repeats per cycle), staggered over the surface.
    The velocity is solved from above: on the surface it is lifted onto the
    slope; where the surface is steep, straight down it at free speed or
    faster. It enters each mesh's texture space through the screen-space
    derivatives of position and texture coordinates, so every layer moves
    with the same water whatever its UV layout. Vanilla's own scroll is
    taken back out (the material's current texture offset, per draw). The
    stream map now carries velocity, foam and water.
-   Decided by: @Victor ("go back to the original water meshes and try to
    drive a simulation on them that can redirect the textures (like a
    flowmap) more realistically. keep the gpu shit though")
-   References: D28, D29, D31

## D33: Gravity in the falls' flow; spray reworked; vanilla's overlay layers hidden

-   Date: 2026-10-16
-   Phase / spike: D32 follow-up, from @Victor's review screenshots
-   Decision: The solver now carries a head per cell: the highest water
    surface upstream along the flow, less 0.35 per world unit run (friction),
    at most 900 above the surface. The shaders turn the drop below it into
    speed along the slope (sqrt(v² + 2·g·drop), g = 687 × Fall Acceleration),
    added downhill to the solved velocity, so falling water speeds up and the
    crest no longer reads as flowing backwards where the solved flow is weak.
    The stream map packs foam + 2 on water in its third channel and the head
    in its fourth. Vanilla's white-water sheet and streak layers are hidden
    again by default (they were the grey/brown "spray"); each layer has a
    checkbox. Spray droplets are smaller (2.5), more (32 per emitter, up to
    600k particles, emitters shuffled so a cap thins them evenly), smeared
    along their motion, and lit white by the light's brightness rather than
    its colour (the sky's blue made them glow); mist is sparser and fainter.
    Flow cycle, stretch limit, acceleration and spray/mist amounts are menu
    settings so they tune without rebuilding.
-   Decided by: @Victor ("velocity seems inconsequential, we need a way to
    give gravity to the solver"; "spray and mist are blue"; "too big and too
    few"; "get rid of it")
-   References: D31, D32

## D34: The falls' course from inflow to outflow; white water; seams

-   Date: 2026-10-16
-   Phase / spike: D33 follow-up, from @Victor's review
-   Decision: The solver adds a course potential, 0 on the inflow rim and 1
    on the outflow rim, harmonic over the water with rock as walls (it has
    no hollows, so its slope always leads from where the water comes in to
    where it leaves, round rock). The solved velocity keeps its sideways part
    but its part along the course is at least 0.35 of free flow: the water
    never runs back. In the shaders the course leads; steep water turns down
    its mesh's slope only where that slope agrees with it (Bethesda's meshes
    fold and bulge), and gravity only adds speed along it. The lit surface
    whitens where the flow foams (rock, the joins with still water, seams)
    and a little where it falls fast, broken up by the texture's streaks.
    Seams: points along each vanilla piece's open edges within 20 units of
    another piece foam the flow map (28-unit radius) and throw spray. Spray
    droplets are translucent (Spray Opacity, 0.35).
-   Decided by: @Victor ("the solver must know where the water starts to flow
    and where it is going to"; "the little particles need to be translucent")
-   References: D32, D33

## D35: Spray from the falls' own motion; plunges drive the water; smooth flowmap frame

-   Date: 2026-10-16
-   Phase / spike: D34 follow-up, from @Victor's review screenshots
-   Decision: Spray and mist come from three places only, each leaving with
    the water's own velocity (solved course lifted onto the slope, plus the
    speed gained from its drop): drops (flow cells whose surface falls 40+
    units within two cells downstream), plunges (vertices of the falls within
    14 units of a lower water level whose water has fallen 40+ below its
    head) and strikes (rock foam over 0.6, and seams). Droplets then fall
    under gravity; mist is carried with the water and slowed by drag
    (x = v0·τ(1−e^(−t/τ))), settling rather than rising like steam, and is
    lit mostly by the ambient light (the sun lit it in shadow and glowed it
    orange at dusk). Seams now include the lines where two vanilla pieces cut
    through each other (triangle-triangle intersections), not only open edges
    near another piece. The plunges push the simulated water: every 56 units
    along a plunge, a push that heaves it down in bursts like a body landing,
    pulls it along the falls' direction and foams it, nearest the grid centre
    first; the water solver takes 64 emitters (was 16). The flowmap on the lit
    surface uses the mesh's interpolated tangent frame and its measured texel
    density (per mesh, from its vertices) instead of each triangle's face, so
    the texture no longer jumps at Bethesda's coarse triangle edges. The
    whitening no longer raises the surface's opacity (that showed as a colour
    step down the falls).
-   Decided by: @Victor ("they should happen at 3 points: where the fall
    starts, where it ends, and in intersections along the way"; "it should
    mostly follow the water flow"; "visible texture seam/artifact because of
    the low mesh resolution"; "it should also greatly drive the flow of the
    shallow water")
-   References: D22, D31, D34

## D36: Falls under the water hidden; flow bounded by the pools; debug views; menu reorganised

-   Date: 2026-10-16
-   Phase / spike: D35 follow-up, from @Victor's review screenshots
-   Decision: Water under a pool's surface (vanilla's fall meshes dipping into
    the water above or below, where their flow ran the wrong way) belongs to
    the pool: in the solve it is inflow under the top level and outflow under
    the bottom, so the falls' flow starts where they leave the water above
    and ends where they meet the water below. In the shaders those parts are
    discarded (Hide Falls Under Water; the ground map carries the pools'
    surface in a second channel), and the falls whiten wholly within 26 units
    above the water below. Plunge pushes on the simulated water are closer
    (40 units), wider and foam four times as hard; the solver takes 96
    emitters. Pieces crossing nearly along each other (faces within cos 0.9)
    no longer count as seams. Mist is a soft uneven wisp, twice as large,
    fading over 150 units into what is in front of it and into the water it
    rose from. Debug: a Waterfall Overlay draws vanilla's fall meshes by piece
    (red inside rock) or their intersections (seams by kind, water-level
    crossings, rock contacts, emitters by kind). The menu is reorganised into
    Water, Waterfalls, Ground Fog, Quality and Debug sections; the falls' own
    surface (D30), the test puff and test current are gone from it.
-   Decided by: @Victor ("trace the intersection of the shallow water with
    the waterfall meshes"; "hide the underwater part"; "the 'seam' where the
    water falls into the shallow water should be completely foamy"; "refactor
    our page on the CS menu")
-   References: D30, D34, D35

## D37: Buried falls hidden; the falls' start held to the course; spray only from visible water; falls' fog

-   Date: 2026-10-16
-   Phase / spike: D36 follow-up, from @Victor's review screenshots
-   Decision: Vanilla's fall meshes run on into rock, the ground and the
    pools. Parts more than 6 units under the captured rock and ground, or
    under a pool's surface, are discarded in the shaders (Hide Buried Falls)
    and throw no spray, make no seams and no emitters (the solve's result
    now carries the bed and the pools' cover). Risk noted: water flowing
    under an overhang the capture took as ground would hide too. Near the
    falls' start (the first 0.35 of the course potential) the course rules:
    the water runs on at free speed at least and its sideways part shrinks
    by up to 85%, so the start never flows back. Spray defaults are finer
    and denser (1.4 units, opacity 0.2, 160 per emitter, up to 2 million),
    mist denser (4 per emitter, opacity 0.16). The falls feed the ground fog
    (D21): every 64 units along their plunges and drops a source makes fog
    (2.5 resting amounts per second, 56-unit radius), which the wind carries
    off (Waterfall Fog; needs Ground Fog). The menu's duplicate IDs (section
    headers sharing labels with their checkboxes) are scoped per section.
-   Decided by: @Victor ("at the start of the fall, velocity and gravity
    should be the strongest driving factors"; "they should always come from
    water"; "smaller, fainter and like 5x denser"; "continuously generated,
    short lived but simulated fog"; "completely invisible from the point
    where they enter a rock mesh or a shallow water mesh or even the
    ground/landscape")
-   References: D21, D34, D35, D36

## D38: Buried falls cut in the vertex shaders; walls only where rock pierces; a waterfall fog of its own

-   Date: 2026-10-16
-   Phase / spike: D37 follow-up, from @Victor's review screenshots
-   Decision: D36/D37's pixel discards left the depth pre-pass's depth behind
    (the lit falls are drawn after a depth pre-pass by BSUtilityShader), so
    hidden parts showed as flat, milky G-buffer. The cut is now geometric:
    Lighting.hlsl and Utility.hlsl (now built with FLUID_FIELD) output
    SV_ClipDistance from the same function (FallGround::ClipDistance: height
    above the pools' surface and above the captured rock and ground less 6
    units), from a camera offset in our own constants (Lighting's vertex
    shader has none), so both passes cut the same triangles at the same line.
    A BSUtilityShader SetupGeometry hook binds our constants for the falls'
    utility draws. In the solve, rock is a wall only where it stands 2 units
    through the water's surface: rock lying under the falls no longer counts.
    The falls' fog no longer feeds the ground fog (D37): a separate waterfall
    fog (FallFog.hlsl, a full-screen pass after the spray) marches each view
    ray through a layer over the pools, as thick as the simulated water is
    white (its foam, which the plunges churn and the flow carries) or as the
    falls' own foam, up to Waterfall Fog Height (90) above it, broken into
    wisps drifting with the water's flow. Vanilla's waterfall mist and splash
    effects drive it too: their bounds as weak fog volumes (12% share), and a
    mist emitter at each one's foot. Mist defaults toned back (2 puffs,
    opacity 0.06, 100 units) after they blotted out the view.
-   Decided by: @Victor ("the mesh parts we're discarding are showing up
    milky/white"; "our only concern is meshes that actually intersect with the
    waterfall meshes"; "I want it separate from the ground fog ... as
    'waterfall fog'"; "use the existing vanilla fog particles in waterfalls to
    drive OUR fog and particles effects too")
-   References: D21, D36, D37

## D39: Fog from every collision; whitening in the water's hue, with a toggle

-   Date: 2026-10-16
-   Phase / spike: D38 follow-up, from @Victor's review
-   Decision: The waterfall fog rises wherever the falls' water strikes rock
    or other water, drops and plunges, not only over the pools: their
    emitters are splatted (40-unit radius) into a fog-source map, strength and
    height, carried in the ground map's third and fourth channels (now
    RGBA32F), and the fog shader adds fog round each source's height (risen
    20 units, spread 45) with one lookup per sample. The lit falls' whitening
    keeps the water's own hue (a fixed white albedo lit warm and showed
    orange at dawn) and softens the 16-unit foam map over its neighbourhood
    (it showed as blocks); Whiten Falls turns it off for comparison. Rock
    under the falls stays out of their solve (only rock through the surface
    is a wall, D38); the shallow water's depth handling is unchanged.
-   Decided by: @Victor ("fog should also be generated along colisions
    between water meshes and water meshes with other meshes like stones";
    "for the waterfall though we don't care about whats not poking through")
-   References: D34, D38

## D40: Pools only where their water is; the depth pre-pass gets FLUID_FIELD; fog tamed

-   Date: 2026-10-16
-   Phase / spike: D39 follow-up; root causes found by instrumenting, not guessing
-   Decision: Vanilla's water planes reach this code as whole cells (4096
    units), so "under a pool" covered most of a waterfall: the solve made a
    third of it inflow or outflow and foamed most of it (the cream/tan rims),
    and the clip cut visible water. A pool's water is now only where the
    captured rock lies under its level and either the falls' own surface lies
    within 24 units of it or the rock lies within 160 units under it (a river
    or pool bed, not a fall's face); the pools' map comes from that cover,
    and the solve's surface takes the pools only there. The remaining tan was
    the depth pre-pass: ShaderCache's Utility defines never added feature
    defines, so FLUID_FIELD never reached Utility.hlsl and its clip never ran
    (A/B: the tan went with Hide Buried Falls off). The Utility define
    builder now adds them, as Lighting's does. Waterfall fog was an opaque
    wall from inside its layer: density 0.004 (was 0.02) and none within 40
    units of the camera, fading in over 200; marched at most 1600 units in 64
    steps; its sources bilinear. Waterfall Shading's debug colours now paint
    the lit falls too.
-   Decided by: investigation after @Victor's report ("everything's borked
    besides the flowmap"; "fog/smoke over water seemed proximity dependant")
-   References: D36, D38, D39

## D41: Waterfall fog from the falls' own sources, marched in its height band

-   Date: 2026-10-16
-   Phase / spike: D40 follow-up (@Victor: "fog doesnt show from afar")
-   Decision: Two causes, found with a per-part debug view (Waterfall Shading
    3: red the pools' layer, green the falls' sources, blue vanilla's mist):
    the fog was marched only 1600 units from the camera, and its pool layer
    followed the water simulation's foam, which exists only round the player
    (and restarts after a teleport). The fog is now marched only through its
    height band (lowest to highest pool, source or mist volume, plus its rise;
    logged per surface) out to 8000 units in 96 steps, and carried by the
    falls' own sources wherever the player is: plunges spread fog 180 units
    round them, strikes 90, drops 63, weighted 1, 0.7 and 0.4, standing 60
    units above the source and spreading 120 up and down, six times the pool
    layer's density. The simulation's foam still thickens the pool layer near
    the player.
-   Decided by: investigation after @Victor's report
-   References: D38, D39, D40

## D42: Lace foam on the falls; D43: violent waterfall fog

-   Date: 2026-10-16
-   Phase / spike: D41 follow-up (@Victor: "keep improving our waterfall";
    "waterfall fog should be agitated and violent, not static"; FluidNinja
    LIVE clip as reference)
-   Decision: D42: the falls' whitening is lace, not a flat blend: ridges of
    two-octave noise stretched along the texture's flow axis (u), carried in
    the flowmap's two phases so the filaments stream with the water; few and
    thin where the water is barely white, joining up as it whitens, solid
    where it is wholly white (the plunge, against rock). Speed whitens only
    the fastest drops. D43: the fog's wisps are in a frame along the flow,
    stretched 3.5x along it at speed, carried at the falls' own speed, their
    domain churned by two fast swirling warps (1.6 wisps, changing at 1.1/s)
    and boiling upward at 70 units/s; the cycle is 1.6 s. The fog tears into
    strands (thresholded wisps) with clear air between, sources at 2x the
    pool layer's density; plunges dominate (strikes 0.35, drops 0.3); the
    band reaches 420 above the sources (it sliced their tops flat); the tint
    is cooler and barely sun-lit.
-   References: D34, D38, D41

## D44: Fog only at the meshes' seams; plunge foam; per-pass clip; no uphill flow; neutral colours

-   Date: 2026-10-16
-   Phase / spike: @Victor's review ("fog only when waterfall meshes collide,
    as cheap as possible"; "where the fall drops into shallow water ... needs
    that foam"; "underwater parts ... still visible with reflections";
    "backflow where the drop goes straight into a rock"; "performance is
    shiiiiit"; "weird blue tint at 6am")
-   Decision: Measured first: FluidField::FallSurfaces cost 55 ms GPU, all of
    it the fog march (spray 0.07 ms). The fog is now made only at vanilla's
    fall meshes' seams (open edges against another piece and crossings),
    splatted 70 units round each seam point; no pool layer, no vanilla mist
    volumes; marched in 32 steps over the seams' height band only, with a
    cheaper churn (one warp, two phases, one detail octave, 4 noise lookups
    per lit sample): 55 ms to 3.7 ms. Plunges foam the water: a plunge map
    (R16F, the ground map's lattice, 40 units round every plunge emitter) is
    read by WaterFoamCS (t9; WaterCB gains its placement, 112 to 128 bytes)
    and makes 3 foam/s there, which the flow carries downstream. The clip
    uses the render pass's own posAdjust (RendererShadowState), not the main
    camera's (reflections render from a mirrored camera). The falls' flow
    never runs uphill: an upward direction keeps only its level part, or turns
    down the slope. Foam, spray and fog colours are near-neutral white (the
    water's own hue tinted them blue).
-   References: D36, D38, D41, D43

## D45: The pools from the engine's water, not the site's paired levels

-   Date: 2026-10-16
-   Decision: A tier between two falls is in none of a site's paired levels,
    so its plunges were not found and the falls running into it were not cut
    or foamed. The pools' cover is now sampled from the engine's water
    (WaterSurfaces::SampleHeight on every cell of the site's capture), with
    D40's checks (bed under the level, and the falls' surface at it or the
    bed shallow under it), and plunges are found against that cover as well
    as the paired levels.
-   References: D36, D40, D44

## D46: Contrast-preserving flowmap blend; plunges where steep water meets a pool

-   Date: 2026-10-16
-   Decision: The flowmap's two phases were cross-faded linearly, which
    averages the texture toward its mean mid-blend: the falls looked flat
    and milky next to vanilla (A/B at the same view). The lit falls now use
    the variance-preserving blend: mean (the texture's smallest mip) plus the
    linear blend's offset from it times 1/sqrt(wA^2 + wB^2), for colour and
    normal alike; contrast matches vanilla in captures. Plunges are also
    found where steep water (upward normal under 0.8) reaches a pool's
    level, whatever its head: on a slow site the head barely grows, and its
    plunges went unfoamed (site #2: 472 to 712 plunge emitters).
-   References: D32, D35, D44

## D47: No gaps at cliff edges; plunge foam along the whole intersection; spray by water speed; wider seam foam

-   Date: 2026-10-16
-   Phase / spike: @Victor's review screenshots 456-462
-   Decision: Gaps: the buried cut used the ground map at the pixel, but the
    map is 2.5D at 16 units, so water running down a cliff face lay under
    the cliff top's cell and was cut. It now cuts only water more than 30
    units under the lowest ground within 14 units, and 8 units under a
    pool's surface (the pool's water overlaps the cut, no slit). Plunge foam
    covers the whole intersection: every vertex of the falls' surface within
    20 units of the pools' surface foams the water 64 units round it (0.45
    to 1 by steepness and speed), not only plunge emitters. Seams foam 44
    units round, solid along the seam. Spray follows the speed of the water
    that throws it (full at 700 units/s): slow water throws 15% as much,
    fast water all of it, up to twice as hard, twice as big and denser; 240
    per emitter. The flowmap's contrast boost applies to colour only (boosted
    normals sparkled harshly).
-   References: D36, D44, D45, D46

## D48: Spray toned back; pools foam as lace; the falls cut deeper under the pools

-   Date: 2026-10-17
-   Phase / spike: @Victor's review screenshots 471-479
-   Decision: D47's speed-scaled spray grew droplets to 2.2x and opacity to
    nearly 3x: close to the camera they were blobs, and strikes threw them
    far from the water. Speed now drives only how many are thrown and how
    hard (opacity 0.7-1x, size unscaled); strikes kick spray up near the
    water (capped speed, 0.5x and 0.3x strength) instead of across; droplets
    within 60 units of the camera fade out; 120 per emitter. Plunge foam is
    made at 1/s (was 3), so the pools' foam breaks into lace instead of a
    flat white sheet. The falls are cut 40 units under the pools' surface
    (was 8; through the clear water the cut showed as rock behind a slit) and
    48 units inside the lowest ground nearby (was 30).
-   Next (investigated, not built): denser falls geometry. Hardware
    tessellation would need identical hull/domain shaders in the depth
    pre-pass and the lit pass, with the engine owning the topology; the
    robust route is to replace each vanilla fall BSTriShape's vertex/index
    buffers with a subdivided copy (FallSites already subdivides them),
    so every pass (pre-pass, lit, shadows, reflections) draws the same dense
    mesh, then displace by the flow in a function shared by Lighting.hlsl
    and Utility.hlsl.
-   References: D36, D44, D47

## D49: Impacts replace rock-foam spray; falls hidden under the pools again; ground cut only when deeply buried; no mist cards

-   Date: 2026-10-17
-   Phase / spike: @Victor's review screenshots 488-499
-   Decision: Spray, foam on the falls and fog now come from impacts:
    vertices at least 0.7 flat with water steeper than 0.5 within two cells
    upstream standing 30+ units above them (where falling water lands, and
    where vanilla hid the join with foam cards). They replace the solver's
    rock-foam strikes, which marked wide bands of slow water beside rock and
    threw spray from there. Vanilla's mist and splash refs no longer emit
    (spray out of nowhere); mist cards are off by default (no 2D cards).
    D48's 40-unit cut under the pools showed the falls through the clear
    water: back to 2 units, with a solid foam rim at the waterline (the
    plunge map near 1 makes 5/s more foam) hiding the cut. The ground cut
    only removes water 120+ units under the lowest ground nearby: the ground
    map sees overhangs from above and cut water running under them, while
    the rock's own depth already hides water behind it. Emitters are hidden
    by the same margin.
-   References: D36, D44, D47, D48

## D50: Spray reworked again; plunge pools found; vanilla splashes spawn spray; dead code removed; menu tidied

-   Date: 2026-10-17
-   Phase / spike: @Victor's review screenshots 510-516
-   Decision: The white streaky veils were the waterfall fog fed by
    impacts (its wisps stretch down steep water): Waterfall Fog is off by
    default. Spray is far sparser and falls sooner: 40 per emitter, smeared
    over 5 ms of motion (was 12), thrown at 150 units/s (was 260, and plunges
    at up to 2x that), with gravity at 1100 units/s^2 so droplets turn down
    sooner. Deep plunge pools are pools: the site's lowest level counts
    wherever the rock lies under it (neither of D40's tests held there: the
    bed is deep and the falls' surface above the pool stands far above its
    level), so the plunge pools now foam, whiten the falls and cut their
    underwater part; the falls' vertices within 40 units of a pool (was 20)
    count as meeting it. The pool the falls come from is cut 24 units deeper
    (the lip dipped under it where its water does not show: a hole). Vanilla's
    placed splash and skirt effects spawn spray from their foot, every 48
    units over their footprint (artists put them where the water strikes).
    Cleanup: D30's merged-surface rendering is gone (FallSurface.hlsl, the
    vertex/index upload, the surface constants, the falls' texture lookup,
    the replace-surface switch); the spray and fog share a lean constant
    buffer (FluidField/FallEffects.hlsli); vanilla mist volumes, the ground
    fog's fall emitters (D37) and Waterfall Fog Height are removed. The
    Waterfalls menu is Water, Spray, Fog and an Advanced tree (flow cycle and
    stretch, mist cards, vanilla layers).
-   References: D30, D37, D40, D44, D47, D48, D49

## D51: Waterfall fog removed; spray count restored, placed at collisions

-   Date: 2026-10-17
-   Decision: The waterfall fog (D38-D44) is removed altogether
    (FallFog.hlsl, its draw, the fog-source map and the ground map's fog
    channels: the ground map is RG32F again, rock and pools), until a
    better approach is chosen (@Victor: "the waterfall fog's implementation
    was shit ... remove this current version altogether"). Spray count is
    back to 120 per emitter (fewer was not the ask; better placement was):
    drops now shed only 0.1-0.4 strength, so the spray comes from where the
    water collides (plunges, impacts, vanilla's splash and skirt positions),
    not from every lip. Vanilla's splash and skirt meshes stay hidden; only
    their positions are used (D50).
-   References: D38, D44, D49, D50

## D52: Middle-tier pools flooded in; spray from rock hits and seams; solid rim for every fall

-   Date: 2026-10-17
-   Phase / spike: @Victor's review screenshots 523-524
-   Decision: A deep middle tier failed every pool test (the falls above it
    stand high over its level, its bed is deep, and it is not the lowest
    pool), so its falls were not cut under the water. Pools now flood from
    the confirmed cells through the basin: neighbouring cells at the same
    engine water level with the rock under it, stopping where falls run
    below the surface (a falls' face under a pool's plane stops the flood).
    Spray comes only from (a) rock hits, where an edge of vanilla's falls
    runs from on the rock to 12+ units inside it (the crossing is where the
    water strikes; strength by the height fallen), (b) seams, the falls'
    pieces clashing, at full strength again, and the plunges into the
    pools. Drops, impacts (foam only now) and vanilla's splash and skirt
    positions no longer throw spray. The plunge foam rim is strength 0.85+
    for every fall (slow falls never reached the rim before, leaving a dark
    slit).
-   References: D40, D49, D50, D51

## D53: Pools stop at lips and curtains; always downstream; spray at drops and rock; translucent falls

-   Date: 2026-10-17
-   Phase / spike: @Victor's review screenshots 529-536
-   Decision: The holes in the falls (529, 532, 533, 536) were D52's flood
    running under curtains: the top-down raster skipped near-vertical
    triangles, so cells under a curtain falling from a pool read as deep
    water at its level and the falls there were cut. The raster now also
    keeps each cell's lowest and highest falls, edges included, and a cell
    is not a pool's (seed or flood) where, within 3 cells, the falls drop
    48+ below its level with none standing above it (the bottom pool is
    exempt: nothing drops out of it). Backflow (531): the solve never runs
    up the smoothed surface where it slopes (at least COURSE_FORWARD of free
    flow down it), and in the shader steep water always runs down its slope
    (D34's "agree" no longer lets the solve lead there); water the solve
    sends up a face turns down it. Spray: the drops throw again (where the
    water above reaches the falls), water the solve sees piling against or
    squeezing past rock throws (530: a boulder in a run that no piece dips
    into), slow water throws more (0.45 + 0.55 speed) and the default is
    200 per emitter. Translucency: the falls' lit surface is switched to
    alpha blending through its own NiAlphaProperty while grounded (restored
    after), and the lit shader gives it patches of thin, see-through water
    carried by the flow, opaque where thick or white; the menu's
    Translucency slider (0 keeps vanilla's opaque draw). First run: the lit
    surface (FXWaterfallBody\*Inner/SlopeMesh) has no alpha property at all,
    so nothing was switched; its sibling layers carry 0x10ED (SrcAlpha /
    InvSrcAlpha blending), the one to borrow next. Vanilla's geometry
    stays (the thin, slow little falls are noted, not removed).
-   References: D34, D37, D44, D52

## D54: Softer rock cut, pools reset the head, least flow, closer spray, borrowed alpha

-   Date: 2026-10-17
-   Phase / spike: @Victor's review screenshots 539-558
-   Decision: A vertex buried in rock counts at most 4 units under the line
    (BURIED_SOFT), so on Bethesda's large triangles the cut stays near the
    buried corner instead of hiding the water standing out of the rock
    (540, 556). Water that comes through a pool restarts its head at the
    pool's level (557: an apron under the big fall raced with its head).
    The falls' texture moves at least 0.6 of free flow (the tops leaving a
    pool showed still, unstretched water: 541, 543, 553-555). Spray shed
    off drops keeps 0.2-0.35 of the water's motion and strikes 0.15, so it
    falls by the sheet instead of fanning out (539). The plunge pushes the
    shallow water less and more evenly (70 / 0.2 / every 24 units): the
    40-unit pushes dented its surface below the falls' cut, showing dark
    gaps along the line (558). The lit surface borrows its model's layers'
    alpha property (0x10ED) to draw blended; it then draws invisible, so
    Translucency defaults to 0 until that is understood.
-   References: D37, D47, D49, D53

## D55: The falls drive the shallow water without denting it; pools pass on only their level

-   Date: 2026-10-17
-   Phase / spike: @Victor's review screenshots 563-569
-   Decision: The falls no longer push the shallow water's surface down (the
    plunge pushes and D22's pulse): its dents dropped the surface under the
    falls' cut and broke the foam along it (566). They drive its flow and
    foam only. D54's pool head reset is moved to what a pool passes on: the
    falls over a pool keep their own head (the curtain cells over it had
    dropped to the pool's level beside cells that had not, and the speed
    jump marbled the texture near the bottom, 567-569). Moderate slopes
    follow their slope too (steep from upward normal 0.9, fully at 0.6),
    against the backflow on crests among rocks (563-565). The blended draw
    takes the shader's own alpha; the blended falls still draw invisible,
    so Translucency stays 0 by default.
-   References: D22, D53, D54

## D56: Forward from the mesh itself; rock cut per vertex; the falls' own collision is not rock

-   Date: 2026-10-17
-   Phase / spike: @Victor's review screenshots 577-586, and a dissection of
    vanilla's waterfall models (scratchpad ogfalls/report.md)
-   Decision: A step back from D53-D55, which kept trading one failure for
    another because the flow's direction came from the wrong source: a
    top-down height map and the local slope, which arched, folded and
    curtained meshes defeat (on an arch's back the surface rises from the
    pool, so "downhill" points back).
    -   Every vanilla falls mesh's v increases downstream (all of their
        area) and vanilla scrolls the texture toward +v; the UVs are
        stretched down the drops, so a constant scroll already accelerates
        (about 100-150 units/s on the lips, 400-640 on the drops). The
        flowmap's forward is now that: the mesh's own +v at vanilla's own
        scroll rate, measured per material from its texture offset. The
        solve only turns it aside (at most 35 degrees) and slows or
        quickens it (0.5-1.6x); it never turns it back. The local-downhill
        rules (D44, D53, D55), the least flow (D54) and the gravity speed
        from the head (D33) are gone; the Fall Acceleration setting with
        them.
    -   The rock cut is per vertex, from rays out of each vertex of the lit
        meshes against the game's collision (FallSites::ClipStep), read by
        SV_VertexID in both the depth pre-pass and the lit pass, instead of
        the top-down ground map, which called everything under an overhang
        buried and holed the falls there. First run: the test (a ray that
        meets rock from behind started inside it) found no vertex inside, so
        nothing is cut in rock; the pools' cut is unchanged.
    -   The ground capture passed through the falls' own collision (water
        material on the static layer): 4622 of 32786 rays had taken it for
        rock, under every fall.
    -   The plunges no longer spread the shallow water out from points (it
        lowered the surface at each, the dashes along the line); they push
        it downstream from 32 units past the line. The plunge rim is lace,
        not a flat band (rate 2).
    -   A blended falls mesh is kept out of the depth pre-pass; it still
        does not draw at all once blended, so the blended pass needs the
        mesh registered with it (not done; Translucency stays 0).
-   References: D32, D33, D34, D37, D38, D44, D53, D54, D55

## D57: The solve leads on flat water only; rock by ray parity; open edges reach the rock; deeper pool cut

-   Date: 2026-10-17
-   Phase / spike: @Victor's review screenshots 598-604
-   Decision:
    -   The solve turned the flow aside on curtains too (598-601): from above
        it sees a curtain edge-on, and its turn sheared the texture into
        marble. It now has a say only where the surface is flat (upward
        normal from 0.45, fully at 0.8). There it can turn the flow up to 75
        degrees, so runs follow their channel round rock instead of
        plunging into it (604). Vanilla's animated texture scale is taken
        out with its scroll.
    -   Inside rock is ray parity: rays from outside toward the vertex
        (down, down and in, along its face) that all cross rock an odd
        number of times. D56's test of the hit's facing never fired. A
        buried vertex counts 2 under the cut against 38 over it, so on the
        large triangles the cut stays at the rock's face (a deeper value
        notched black gaps along it). First run: 325 of 1188 vertices
        inside.
    -   Open edges of the lit meshes that stop short of the rock (603, a
        vanilla gap) reach it: each open-edge vertex looks out along the
        surface, and out and down, up to 64 units, and moves 6 past the rock
        (26 vertices), in both passes through the per-vertex buffer.
    -   The pool cut is 20 under the surface (was 2): the shallow water's
        ripples opened a gap under a cut at the surface, the black dashes
        along the line (602).
-   References: D47, D49, D56

## D58: One continuous scroll on the falls; phases only for a deviation; bilinear pool cut

-   Date: 2026-10-17
-   Phase / spike: @Victor's review screenshots 611-624; a code audit
    (subagent) and a technique survey (scratchpad research/waterfall_techniques.md)
-   Decision: The curtains' marbling was the flowmap itself, not the solve
    and not doubled meshes (checked: every vanilla lit falls mesh is one
    sheet, and placed pieces overlap 2-8%): its two phases show two
    different patches of the texture (offset 0.31, 0.17), cross-faded in
    position-dependent bands and boosted by the contrast blend, even where
    the flow is uniform. The falls now scroll as one continuous sample down
    +v at vanilla's rate; the two phases carry only a deviation from it
    (Flow's a_deviation, zero until the per-mesh bake, D59) and fade out
    without one. The contrast boost is colour only. The pool is read
    bilinear over pool texels (nearest stepped the cut every 16 units, the
    saw-tooth at 611) and the clip distance is held to +-48 (a 1e6 far
    value dragged the cut onto the submerged corner); the pool cut is 8
    again; the falls' whitening fades back out under the pool instead of a
    solid white band seen through it. The open-edge reach (D57) is removed:
    it did not close 603, which foam will cover later.
-   References: D32, D46, D56, D57

## D59: Each falls mesh's flow solved on its own texture grid; aeration with run time

-   Date: 2026-10-17
-   Phase / spike: the plan after D58 (steps 2 and 3), from the technique survey
-   Decision: The top-down solve cannot see a boulder through a curtain. Each
    lit falls mesh now gets its flow solved once on its own texture grid
    (u across, v down, every vanilla falls mesh's v runs downstream), at
    about 10 world units per texel (at most 192 a side): the mesh is laid
    into the grid by its texture coordinates, each texel tested against the
    game's collision, and the existing solve (D28) run over it as a sheet
    tilted down +v, coming in at its top edge and leaving at its bottom,
    the rock as walls. The flow never runs back (at least 0.3 of vanilla's
    forward) nor turns more than 60 degrees, is softened over a texel, and
    is stored per mesh as an RGBA16F texture (t124): across and down per
    unit of vanilla's scroll, foam (rock, edges) and run time. The shader
    takes it as the deviation from vanilla's forward (D58); the two phases
    are now the same patch of the texture and always cross-fade in full
    (fading the second out left the first's reset showing: the texture
    jumped back every cycle), at most 0.3 repeats apart, with no contrast
    boost. A texel is rock only where rock lies on both sides of the sheet
    and more than 24 units deep: a curtain before a cliff has the cliff on
    one side only, and vanilla's sheets dip into the rock they lie on (the
    first runs called a third of every curtain rock). The water whitens
    with the time it has run (aeration, to 0.3) and fast, thin water is a
    little brighter. Debug mode 1 shows the bake on baked meshes.
-   References: D28, D56, D57, D58

## D60: The scroll integrated per material; a static streak warp instead of a flowmap

-   Date: 2026-10-17
-   Phase / spike: a full review after D59 (history, root cause and technique
    reports in the scratchpad's research folder)
-   Decision:
    -   The back-and-forth since D58 was `forward * Time` with a measured
        forward: vanilla's slow V zoom (a 6.5 s loop) and frame hitches moved
        the rate, and every change shifted the curtain by the change times
        the whole session (half a repeat after five minutes). The scroll is
        now integrated on the CPU once a frame per material, against an
        averaged scale, and passed as a phase.
    -   The flowmap is gone from the falls. Each mesh's bake (D59) now holds
        a static warp: the streaks' across position carried down the sheet
        row by row by the solved turn (a semi-Lagrangian sweep of the
        stream label), only within 14 texels of rock (away from it the
        solve's drift came from the grid's ragged sides and bent whole
        sheets), at 75%, smoothed, and eased back to vanilla's where the
        streaks squeeze below 0.5 or spread past 2. A straight scroll down
        +v read through it runs along bent streaks; nothing but the scroll
        moves in time, so it cannot pulse or run back. Rock specks of six
        texels or fewer are water; turns are clamped to 45 degrees and
        speeds to 1.6; rock and off-mesh texels take the nearest water's
        values; inflow and outflow are each column's first and last texel.
    -   Masks with it: each rock's rim and the water piling against it
        (foam), and its wake carried down the streaks over 24 texels
        (whitening). Debug mode 1 shows the streaks' spread, the wake and
        the foam.
    -   Measured after a sped-up session (sgtm 8, then 0.1): the texture
        moves steadily down, step after step.
-   References: D28, D58, D59

## D61: Our own waterfall sheets, one per fall, marched from surface A to surface B (prototype)

-   Date: 2026-10-18
-   Phase / spike: @Victor's proposal (four points A-D, a tessellated grid
    deformed by rays against the rock, boulders, water-tight sides, groove
    filling, a solver), and two research reports (scratchpad research/
    ownmesh_codebase.md, ownmesh_techniques.md)
-   Decision: First milestone of the proposal (FallSheets):
    -   One sheet per fall: a site's lit vanilla pieces whose lips stand at
        the same water level. Their lips (top edges: v runs downstream),
        spread across the mean way the water goes, give the lip line A to
        B; every column starts on the upper water's surface.
    -   32 guide columns marched together, a step at a time: riding the
        upper water while over it (the pool map), then ballistic through
        the air, sliding on the rock (gravity along it, less friction) while
        it holds the water, off it where it falls away; ending in the first
        pool, at any of the site's lower water levels, or where it stalls
        on level rock. Each step every column's heading is drawn halfway
        toward its neighbours', so the sheet holds together. C and D are
        results of the march, as the research advised.
    -   Filled to a 96 x 72 grid, softened, the texture's v the water's
        travel time at vanilla's scroll (only ever increasing: no flowmap
        resets).
    -   Drawn by the engine: a clone of the fall's first vanilla piece gets
        our vertex and index buffers in its own vertex layout (its renderer
        data swapped, ours never released by the engine and given back
        before the clone goes), under the same parent, with vanilla's
        material; the vanilla pieces are hidden. Both faces for now.
    -   Findings on the way: the falls' vertex layout reads VF_FULLPREC
        false although positions are four floats (UV at 16); columns
        started at vanilla's lip vertices began inside the rock and fell
        through it; starting upstream on the water, they sank to the
        riverbed and slid under the upper and middle pools until the
        pools stopped them.
    -   Open: columns that stall on ledges leave gaps; a probe that starts
        inside rock can follow faces inside it; boulders, groove filling,
        water-tight sides, the solver and shading are not done. Debug
        dumps (FluidField_sheet\*.bin) and the column overlay (Waterfall
        Shading debug) are in for the iteration.
-   References: D30, D31, D56, D59

## D62: Geometry from the engine's own mesh builder (probe passed)

-   Date: 2026-10-18
-   Phase / spike: engine research (skyrim-engine-re kb, evidence
    2026-10-18-runtime-geometry, L1 until this probe)
-   Decision: Our generated geometry will be built by the engine's own
    builder, `77465` (1.7.104): a complete BSTriShape with its renderer
    data from separate arrays (indices, positions, uv0, normals, tangents,
    bitangents; null arrays left out of the layout), the same function the
    engine uses for water displacement and decals. The engine then owns
    the buffers and releases them with the shape, replacing D61's swap of
    our own buffers into a cloned vanilla piece. Probe: a 2-triangle quad
    in front of the player, given a clone of a vanilla falls piece's lit
    property (SetupGeometry, FinishSetupGeometry, two-sided), attached to
    the player's cell 3D: built (stride 28), not hidden, drawn with
    vanilla's water material, no crash. The draped sheet (D61) is next to
    move onto it. Toggle: Waterfalls debug "Toggle Test Quad", or the
    FallTestQuad setting.
-   References: D61

## D63: Generated sheets built by the engine; the simple arc (in progress)

-   Date: 2026-10-19
-   Phase / spike: @Victor's design for the generated falls (one mesh per
    step between an upper and a lower water, starting where the upper
    water's mesh ends, arcing by the water's speed into the lower water,
    only mildly disturbed by what lies under it or pokes through it;
    plunge depth from speed and mass, never under the bed; edges pulled to
    the channel walls, a free edge curled back in a C)
-   Decision so far:
    -   The sheets are built by the engine's own mesh builder (D62) with a
        copy of the fall's vanilla lit property, under the vanilla pieces'
        parent. No more swapped buffers on a clone.
    -   The drape went through three shapes in one session. Rock pushes
        along a per-vertex facing, wall pulls from a row's middle and edge
        curls all produced chaos: spikes, loops, a sheet half underground.
        At @Victor's direction it is now the simple arc:
        -   It starts where the upper water's engine water object ends.
        -   It falls ballistically at the speed at the top. The speed is
            at least 200 units/s (vanilla's lip spray), scaled by the
            Generated Falls Arc setting.
        -   It is lifted a little by rock within PROBE_LENGTH under it.
        -   It is smoothed, more for a faster fall.
        -   It is clamped above the bed below the lower water.
        -   Rows that end up underground for good (rock crossed an odd
            number of times from far above) collapse onto where they went
            in.
    -   Walls and curls are out until the base shape is right.
-   Open at the end of the session (screenshots 732-734):
    -   The sheet does not reach the water below: trimming ends it in a V
        above the rocks in front of the pool. Parity from above is fooled
        by rock in front of and above the fall, and by overhangs.
    -   One step gets several sheets, as vanilla does, because falls are
        grouped by (site, upper level) and pieces at one step disagree on
        their upper level. It must be one mesh per step between two waters.
    -   The water objects are whole cells, so "where the upper water ends"
        is the cell edge, not the visible lip. The real lip must come from
        the rock: where the bed drops away under the upper water.
    -   The smaller falls (-914, -1037) have no water object at their level
        and start at vanilla's lip.
    -   @Victor also asked to switch off the shallow-water simulation where
        it lies inside rock and landscape the player cannot reach (D-next).
-   References: D61, D62

## D64-D66: Generated sheets routed over the ground, lip from the water plane, one per spill (shelved)

-   Date: 2026-10-19
-   Phase / spike: @Victor's review screenshots 738-747, 131-347 (CS)
-   Decision: The generated sheets (D61-D63) were carried further and then
    shelved at @Victor's call; the code stays behind Generated Falls (off).
    -   D64: the columns are marched in lockstep over the site's captured
        ground (filled by a priority flood, a smoothed downhill field),
        thrown on an arc off drops, ending in the water below; one whole
        sheet, no tears; debug views (wireframe, lip and routes, normals).
    -   D65: the lip is the upper water plane's own end (its border), which
        @Victor marked as the real lips between the rocks.
    -   D66: one sheet per stretch of a water plane's border that spills
        over a drop with a vanilla lip near (vanilla is the trigger), down
        to the next water plane; a stretch with no spill longer than 256
        units (a big rock) splits it. Width from vanilla's pieces at each
        height, thickness from speed; rock taller than half the thickness
        pokes through.
    -   Why shelved: every round traded one artefact for another (whorls of
        the routing, pinching, monstrosities where detection misfired), and
        the flow did not look better than vanilla's own meshes. @Victor:
        back to vanilla's falls as the base, with their flow synced and
        driven by a simulation.
-   References: D61, D62, D63

## D67: Vanilla's falls as the base: cascades linked, one shared scroll, seams blended

-   Date: 2026-10-19
-   Phase / spike: @Victor's direction after D66
-   Decision:
    -   Each lit falls mesh's bake (D59) now stores the texture coordinates
        its water reads, not a warp: across, the streak it is on, carried
        down the solved flow (D60's sweep); along, the time it has run since
        the top of its cascade, in texture coordinates at the shared scroll
        (row length over the solved speed). The shader reads them less one
        scroll, so the texture runs faster where the water does and bends
        round rock, and only the scroll moves in time.
    -   Cascades: a mesh whose top edge lies on the lowest quarter of a
        higher mesh (within 48 units, at least 3 columns or an eighth of
        them) is fed by it: its streaks and run time carry on from that
        mesh's, so one mesh's water runs on into the next. Linked once all
        bakes are solved (FallSites::LinkBakes).
    -   One scroll for all the falls, integrated once a frame at vanilla's
        rate averaged over the falls' materials (per-material phases could
        not meet at a seam).
    -   Seams: an open edge of a lit mesh that lies on a mesh leading it
        (higher, or as high and larger) dithers out, the same pattern in the
        depth pre-pass (Utility.hlsl) and the lit pass, which TAA blends.
        The per-vertex buffer (t123) carries the fade beside the rock cut.
-   References: D56, D58, D59, D60

## D68: Rock from the statics' meshes; flat parts join the main flow; cut under the water below; foam desynced

-   Date: 2026-10-19
-   Phase / spike: @Victor's review screenshots 781-791
-   Decision:
    -   Boulders resting on the ground (RockL02 at 0001B375) did not part
        the flow: the collision test wants rock on both sides of the sheet,
        and the ray from under the boulder started inside the ground. Each
        bake now also gathers the render meshes of the statics round the
        sheet (binned from above) and calls a texel rock where an upward ray
        crosses one mesh's faces an odd number of times and the point lies
        more than 40 units from that mesh (sheets dip into the cliffs they
        lie against).
    -   Water falling onto flat ground (783-785) turns to the site's solved
        flow (seen from above, put in the sheet's texture axes), up to 56
        degrees, only on truly flat texels and only where the main flow
        leaves the mesh's way down by more than 20 degrees. A first version
        (from 0.45 up-facing, up to 72 degrees, everywhere) sheared the
        streaks into whorls.
    -   Falls under the water they fall into (786): each vertex is cut 24
        units under the highest engine water plane below the mesh's top over
        it (under a plane's height there is only water or ground, both hide
        the falls), and whitens as it goes in (its height above that water
        rides in the per-vertex buffer).
    -   Seams fade only where the overlap reaches past the edge (the vertex
        and all its neighbours on the leading mesh): where two meshes only
        meet edge to edge, fading opened the gap between them (789).
    -   The shallow water's foam restarts its two phases at a time of its
        own per patch (a smooth offset over 2.5 tiles): with one clock the
        cross-fade thinned all of it at once and it pulsed (791).
    -   `communityshaders.feature set` replaces the whole blob (LEARNINGS):
        partial sets during this session reset Water Enabled and others to
        defaults; the scratchpad's ffset.py now reads, changes and writes
        the whole blob.
-   Open: gaps between meshes that meet edge to edge (789's dark lines) and
    the hole beside a boulder (781) need the edges moved, not faded; flow
    "plain incorrect at places" (782, 787) to be judged again after this.
-   References: D57, D59, D60, D67

## D69: Seam gaps closed by moving the edges, in both depth passes

-   Date: 2026-10-19
-   Phase / spike: @Victor's review screenshot 789 (dark lines along seams)
-   Decision: Where two lit falls meshes meet edge to edge with air between
    them, fading cannot help. An open edge's vertex that lies off every
    other mesh but within 40 units of another mesh's open edge moves across
    the gap, 4 units past the middle; the other mesh's edge moves to meet
    it the same way. The move is stored in the mesh's own space beside the
    rock cut and fade (the per-vertex buffer is now two float4s) and added
    to the vertex's position at the start of both Lighting.hlsl's and
    Utility.hlsl's vertex shaders, so the depth pre-pass and the lit pass
    still agree (checked: no brown or milky surfaces). First run: 28 of 992
    open-edge vertices of 18 meshes moved; 7 faded (D68's overlap rule).
-   Open: the hole beside a boulder (781) is the rock cut across a large
    triangle, not a seam; to look at next.
-   References: D57, D67, D68

## D70: No rock cut; the water cut only at a mesh's foot; vanilla's way down drives harder; streaks never fold

-   Date: 2026-10-19
-   Phase / spike: @Victor's review screenshots 801-811
-   Decision:
    -   Holes in the sheets (801, 803, 804, 811: a clean polygon round one
        vertex with no rock there): the per-vertex rock cut's parity
        misfired, and D68's water cut fired partway down a fall where a
        plane's bound (a pool partway down, a whole cell) covers it. The
        rock cut is off (ROCK_CUT; @Victor: water inside rock need not be
        culled for now, and rock hides it anyway); the water cut applies
        only within 160 units of the mesh's foot.
    -   Swirls (802, 805-807): @Victor wants vanilla's motion to drive
        harder. The solve may turn the flow 27 degrees at most (was 45),
        60% of the bend is kept (was 75%), streaks never fold (along each
        row they stay at least half their straight spacing apart, in
        order), and flat parts turn to the main flow only near the mesh's
        foot (an arch's crest swirled). First run: 757 flat texels turn
        (was 68764).
-   References: D56, D60, D68

## D71: Pools cut only a mesh's foot; water against rock turns; turns strong only beside rock; calmer river foam

-   Date: 2026-10-19
-   Phase / spike: @Victor's review screenshots 812-815
-   Decision:
    -   The hole that stayed (814, 811, near RockL04Wet 0003AB64) was the
        pools' cut in the shader (D36/D47): near a lip the captured pool
        reaches over the drop, so the curtain just under it was cut where it
        dipped under the upper pool's level, and the rock behind showed.
        Checked against vanilla (our grounding off): no hole. The pools cut
        a mesh only within 160 units of its foot now (a per-vertex flag).
    -   Water running into terrain or big rocks did not curve (813): vanilla
        cuts its sheet round them, and every column's last texel was an
        outflow, so the water drained into the rock. Water leaves only near
        the mesh's foot (its lowest 20%, 64 units at least); columns ending
        higher are walls, and the solve turns the water round them.
    -   Boulders mid-fall bent the flow too little after D70 (812): turns
        reach 45 degrees right beside rock, easing to 27 away from it.
    -   Jelly (815): streaks drift at most 8 texels across from where they
        came in.
    -   Shallow-water foam made where the flow shears, strikes or breaks
        needs more to start (@Victor: not less foam, a higher bar): shear
        from 4/s (was 2), impact from 90 units/s (30), along a wall from 320
        (200), breaking from Froude 2.5 (1.5).
-   References: D36, D47, D59, D60, D70

## D72: Falls meshes' flows draw each other where they meet

-   Date: 2026-10-19
-   Phase / spike: @Victor: "make waterfall meshes flows interact with each other"
-   Decision: When the bakes are linked, every mesh's texels go into one
    world grid (32 units) with the way their water goes (down +v, turned by
    its solve). Where another mesh's water runs within 40 units, a texel's
    flow is drawn toward it (put in this mesh's texture axes, 70% at its
    nearest, softened), except where that water runs back against this
    one: side-by-side flows bend together and merge. First run: 67826
    texels drawn by a mesh beside them, over 18 meshes.
-   References: D67, D68

## D73: Dense copies of vanilla's lit falls meshes

-   Date: 2026-10-19
-   Phase / spike: @Victor's shading ideas (2a: subdivide the low-poly meshes)
-   Decision: Each lit falls mesh is replaced by a denser copy built by the
    engine's mesh builder (D62) in its own space, under the same parent,
    with a copy of its material and its alpha property; the original is
    hidden. Triangles are split until no edge exceeds 40 world units (three
    halvings at most): the new points on the PN-triangle curve through the
    vertices' normals (split texture-seam vertices share the position's mean
    normal, so the curved edges meet), texture coordinates and vertex
    colours (`NiColorA`) averaged, tangents from the texture coordinates.
    Cuts, seams and bakes then work on the copies. First run:
    FXWaterfallBodyTallInner01 276 to 15921 vertices, SlopeMesh01 180 to
    10001; the silhouettes round out and the depth passes still agree.
    The clones' materials do not animate, so the falls scroll at the
    default rate (the shared scroll, D67).
-   Next: the foam layer (idea 1), then light displacement (2b).
-   References: D62, D67

## D74: A real sim on each falls mesh; dense copies at one subdivision

-   Date: 2026-10-20
-   Phase / spike: @Victor's review screenshots 821-824 ("we want a good
    goddamn sim"); offline harness scratchpad simbake.py on dumped inputs
    (FluidField_bakeN.bin beside the log)
-   Decision: The per-mesh solve (D59) treated every mesh as one evenly
    tilted sheet: no real slope, no momentum, so flat parts went to jelly
    and boulders barely deflected. It is replaced by a sim on the mesh's
    texture grid with the mesh's real shape:
    -   per texel, the world across and down one texel; gravity's pull along
        the real surface in those axes;
    -   a staggered (MAC) grid, semi-Lagrangian advection (momentum), bed
        friction 3/s plus drag toward 900 units/s, and a pressure projection
        (Jacobi-preconditioned conjugate gradients, warm started) so water
        neither piles up nor vanishes;
    -   in along the mesh's top band (texture v), out at any water on the
        edge of its low band (v or height: feet are often buried); rock and
        the mesh's sides are walls;
    -   the streaks turn with its direction (to 80 degrees); its speed comes
        from the height fallen (a thin sheet thickens where it narrows rather
        than racing; the 2D sim alone reached 6000-30000 units/s there);
        pockets closed in by rock where it barely moves keep vanilla's way.
    -   Offline first: the first port (collocated grid, 20 Jacobi sweeps,
        no bed friction) ran to 57000 units/s and kinked the streaks; the
        harness showed one inflow cell on sloped meshes (height-based top),
        eddies without bed friction, and racing where few cells let water
        out. All three are fixed above.
    -   The flat-parts join (D68) is off; the sim spreads the water.
-   Dense copies: one subdivision (4x triangles); three stuttered the game.
-   References: D59, D60, D67, D68, D71, D73

## D75: Falls mesh debug overlay

-   Date: 2026-10-20
-   Phase / spike: @Victor asked for a flowmap and wireframe without the console's
-   Decision: Falls Mesh Debug (menu, not saved) draws over everything: the
    falls' meshes as drawn (dense copies yellow) and each mesh's sim as
    world-space arrows (direction, length and colour by speed, white where it
    strikes rock). Arrows are culled inside rock, under the pools, and behind
    rock from the camera (rays, 400 a frame, cached until the camera moves 48
    units); they show the turn the streak sweep used.
-   References: D74

## D76: Water under resting rocks turns; capped stretch; straight over the lip

-   Date: 2026-10-20
-   Phase / spike: @Victor's CS screenshots 568, 464, 605
-   Decision:
    -   568: vanilla's sheet runs on beneath a boulder resting on it (the
        boulder is wholly above: an even number of crossings), so the water
        drained out under it. A lying texel (normal z over 0.5) with a
        static's face within 120 units above is a wall.
    -   464: the texture followed the sim plus D72's mesh-to-mesh pull (the
        arrows showed only the sim): chevrons and a crease. The pull is off.
        Speed against vanilla's scroll is capped at 1.6 again (4x stretched
        the texture into blocks: vanilla's UVs already stretch down drops).
    -   605: on a lip's curl the sim's frame twists and its flow scattered;
        the turn eases in over the first 14 rows of each column.
-   References: D72, D74, D75

## D77: Dead waterfall code removed

-   Date: 2026-10-20
-   Decision: Code that no longer reached the screen is deleted, so what is
    left is what runs:
    -   the generated falls (FallSheets, D61-D64): their settings (Generated
        Falls, Generated Falls Arc), debug view and test quad (D62);
    -   translucency (D53-D55): the setting, the alpha switching on the lit
        meshes, the shader's thickness patches and the hidden depth pass;
    -   the effect layers' grounding (D26, D28): Effect.hlsl is vanilla again
        (the layer checkboxes still pick which vanilla layers show), with the
        ground lift, contact fade, foam tint, buried test and scene depth
        (t121) it read;
    -   the flowmap's two phases (D32): Flow Cycle and Flow Stretch Limit,
        FlowPeriod, FlowMost, uvB, blendB and strength (the bake's single
        phase drives the texture since D67), and TexScale (D35);
    -   the falls' fog (D37, D41) in FogCS (no emitter set it);
    -   in FallSites: the per-vertex rock cut (D56, D57; off since D70), the
        flat-parts join (D68; off since D74) and the meshes' pull on each
        other (D72; off since D76), with the re-link as sites' flows solve,
        the cascade roots, the water-object bounds (D63), the site ground and
        lit-mesh queries (D61, D64) and unused constants;
    -   the merged surface's per-vertex flow fields (only its position,
        normal and speed are read), the spray constants' BaseSpeed and
        DebugMode, and the falls bodies' lip.
-   What is drawn is unchanged: the bakes' turn keeps both blurs, the cut's
    clip value is the constant the off rock cut gave.
-   References: D26, D32, D37, D53, D56, D61, D62, D63, D68, D70, D72, D74

## D78: The top-down site flow retired; its consumers fed from the meshes' own sims

-   Date: 2026-10-20
-   Decision: The site's top-down flow solve (FallStream, D28) and the merged
    surface built on it (D30, D34, D49, D52) are deleted; what still read them
    now reads the per-mesh data (D74):
    -   Whitening on the falls: the bake's own foam (where the water strikes
        rock), its wake, the run time's aeration and the water below; the
        stream map (t122) and its 16-unit foam are gone, so a mesh without a
        bake gets no extra foam. Waterfall Shading's first view shows the
        bake (dark magenta where a mesh has none); the direction view's full
        brightness is a constant (420 units/s).
    -   Pools: worked out on their own once a site's capture is complete
        (FallSites::ComputePools), the same algorithm and results as the
        cover the solve was given (D40, D45, D52, D53); the shader's pool cut,
        the debug arrows and the plunges read it.
    -   Plunges and spray (FallSites::BuildPlunges, when the bakes are linked,
        the pools are found or meshes go): texels on a mesh's foot (its lowest
        160 units) at or under the water below (the pool there, else the
        engine's highest plane under the mesh's top) with the texel above them
        out of it; the crossing, every 24 units, is a plunge point, its way the
        sim's (down the mesh, turned), its speed the fall down the mesh (as
        SolveBake's, to 900 units/s). Spray at the plunges and where the bake's
        foam is high (rock strikes). The shallow water's pushes, the plunge
        foam map and the spray read them unchanged.
    -   Gone with them: the drops' (lip) spray, the seams' and impacts' foam and
        spray, and the Waterfall Overlay's mesh and intersection views (it now
        shows the spray emitters and plunge points).
-   References: D28, D30, D34, D36, D40, D44, D49, D52, D53, D59, D74, D75

## D79: The falls' sim starts at each column's crest

-   Date: 2026-10-20
-   Phase / spike: overnight research (strand-sheet prototype on the bake dumps)
-   Decision: Vanilla's falls sheets start under the upper water and climb its
    hidden upstream face to the lip: the top fall's centre column starts at
    z -515 under its pool at -250 and reaches its crest (-226) a little over
    half way down v. The D74 sim brought the water in at the top edge and pushed
    it uphill: slow water, dead pockets, eddies and jelly on the hidden face. A
    column that climbs more than 48 units to its highest row now starts there:
    the rows before are its lead-in (not simulated; straight, at vanilla's
    speed), and the water comes in at the crest. Offline (simbake.py, CREST=1)
    the eddies on bake 3 are gone and the flow parts cleanly round the rocks
    below the crest.
-   References: D74, D76; research/waterfalls/strand-sheet-prototype.md

## D80: The falls' flow as a strand sheet

-   Date: 2026-10-20
-   Phase / spike: real-time falls, step 3 of research/waterfalls/overnight-2026-10-20.md (CPU, in the existing async bake)
-   Decision: The D74 grid sim (staggered grid, pressure projection) and D67's
    streak sweep are replaced by a sheet of strands (FallStrands::March, after
    research/waterfalls/strand-sheet-prototype.md). In game, the D74 build showed
    stretched texture, curves where there should be none, whorls and back flow.
    -   Up to 128 strands per mesh start at each column's crest (D79; a crest
        inside rock starts at the first water below it). They are marched a row
        at a time by gravity along the mesh, bed friction and air drag. They
        steer round rock they see coming within 5 rows, never cross, and are
        drawn back to their own column over about 0.6 s.
    -   Each texel takes the lip column of the strands either side of it (seed).
        LinkBakes maps that through the feeder's streaks (across0 and along0),
        so cascades carry on as before.
    -   An open gap (the wake behind rock): each side keeps its own streaks at
        their own spacing and they meet in a foamed seam. Spreading one set over
        the gap stretched it 4x.
    -   The texture keeps vanilla's speed down the mesh: the run is texture v
        from the mesh's top row. The strands only bend it sideways, so it never
        stretches along the flow and never runs backwards. With the physics'
        speed, most of each mesh sat at the 0.5x floor (2x squeeze), and strands
        that started lower ran backwards next to ones from higher up.
    -   Sideways gravity is felt at 0.35 of its pull. In full it rolled the
        strands off every dome into ropes along the rock; vanilla's UVs already
        run the right way.
    -   Pressed together, the streaks squeeze 2x at most (the prototype allowed 5x).
    -   The wake slot carries the strands' aeration (fall break-up, strikes,
        pile-ups); foam is strikes and pile-ups plus the rock rim (D60).
-   Offline, on the 18 bake dumps (scratchpad strand/strandbake.cpp builds
    FallStrands.cpp unchanged; plot.py): about 10 ms per mesh; every strand
    reaches the foot. Sideways stretch is 0.8-1.0 at p5 and 1.0 at the median;
    the high tail is along rock outlines and wake seams. Down-stretch is exactly 1.
-   The boulder on the slope run at (31103, -27345), RockL04Wet 0x000B96E2,
    was already rock in bake 14's mask. D74's pressure solve barely bent the
    flow round it, and its streaks carried straight on below, so the water
    looked like it drained under it. The strands part round it and leave a
    wake that closes downstream.
-   Removed: the grid sim's constants and code, the streak sweep (BAKE*WARP*\*,
    spacing and drift guards), BakeField::swept.
-   References: D60, D67, D74, D76, D79; research/waterfalls/strand-sheet-prototype.md
-   First look in game (SS 834-836): the best result yet. Two fixes followed:
    -   Blocky texture (SS 836): the shader read the bake bilinearly, so the
        coordinates' gradients stepped at every bake texel, and the texture's
        scale and mip stepped with them. BakeAt is now a cubic B-spline over
        4x4 texels, and the strands' sideways offset is softened (3 passes)
        on the grid.
    -   A fed mesh's texture ran sideways (SS 835): its streak line, fitted to
        where the feeder's foot meets it, came out nearly flat (links bunched
        on a few feeder texels), so the whole mesh read one texture column.
        The fitted slope is now kept within 0.5-2 (sign kept) and logged.

## D81: Hidden falls effects keep their rock

-   Date: 2026-10-20
-   Decision: With the falls' effects hidden, every non-falls effect model
    (skirts, rapids, splashes, mist) was app-culled whole. FXrapidsRocks01.nif
    carries six real boulders (RockL02, RockL04, RockM01-M04: lit meshes with
    the mountain slab texture), and FXCreekFlatLong01.nif carries rock too, so
    the rock went with them (SS 838, 839, and likely 821-823 under the bridge).
    Now only their geometry without a lit material is hidden (effect layers,
    particle systems); lit shapes stay. Checked: FXRapids, FXRapids02,
    FXrapidsBig01, FXWaterfallSkirtSlope, FXWaterfallMistBlastLite and the
    splash models have no lit shapes.
-   References: D25, D77
-   Second look (SS 840): the texture was squeezed into tight wavy ripples
    along the mesh's sides (strands pressed along the ragged outline and the
    rock, 3-6x), and the middle looked smeared, its detail gone (a fed mesh
    took a slope fitted to its feeder's foot, -0.42 held at -0.5: mirrored and
    stretched 2x).
    -   A fed mesh now keeps its own texture's scale and way across (slope 1);
        the feeder gives only the shift.
    -   The bend fades in over 10 texels from the mesh's sides, and is relaxed
        (300 passes) so the texture across stays within 0.6-1.6 of vanilla's
        scale and the streaks shift at most half a texel per row. The wakes'
        seams become gentle ramps. Offline: p5/p95 across 0.58-0.89 / 1.61-1.91
        (was 0.12 / 6.2), shear p95 0.5; about 150 ms per mesh, on the bake's
        thread.
-   Third look (SS 841): bounded, it looked far better, but the water a bank
    turned no longer swung back into the main flow: the edge fade switched the
    bend off exactly there, and half a texel of shear per row allowed only
    about 27 degrees. The edge fade is gone (the relaxation already bounds the
    squeeze), and the shear is now up to 1.2 texels per row (about 50 degrees).
    Offline: across p95 1.64-2.31, shear p95 0.75-1.12.
-   Fourth look (SS 843): still not curvy enough. Measured as the texture
    mapping's distortion on vanilla's own texel grid (singular values of its
    Jacobian; scratchpad strand/plot.py), the deployed build had median
    anisotropy 1.1-1.6: the run was taken a row at a time, so a turning
    streak's texture sheared instead of turning, and copying the strands'
    bunching squeezed it one way. Now:
    -   The streaks are a least-squares fit over the grid's cells: they follow
        the strands' way at every cell (weight 1) and keep vanilla's spacing
        only weakly (0.05, plus 0.03 on each neighbour pair against a
        checkerboard). Where the water gathers or spreads they close up or
        open out smoothly.
    -   The run is solved as their partner (its gradient their gradient turned
        square), so the texture turns with the flow and shrinks evenly instead
        of squishing.
    -   The strands heal to their column over 1.5 s (was 0.6 s): water a bank
        turns keeps its new way.
    -   Measured in world units on the mesh first, rows 3-28 units apart
        weighted the short ones 3x and bent the texture where it looks like
        vanilla's; the texel grid is the right frame.
    -   Offline: median anisotropy 1.2-1.4 (deployed 1.1-1.6) at the same or
        more turn (p95 29-53 degrees); the strands themselves turn p95 39-69.
        About 0.3-0.5 s per mesh on the bake's thread.

## D82: Bake cache, a vertex swell, and data for a faster rock test

-   Date: 2026-10-20
-   Context: review "Fluid Field Waterfalls Q&A 2026-10-20": the 10+ s wait
    for the falls is the bake's collision rock test (Havok rays on the main
    thread in a 3 ms budget a frame, about 2 s a mesh), not the subdivision.
-   Decision:
    -   **Bake cache.** A finished bake is written to
        `<log dir>/FluidFieldBakes/<ref>_<hash>.bin`, keyed on the reference,
        the model part, its placement, the covered texels, the statics'
        triangles round it (summed, so the cells' visiting order does not
        matter), the ground under it, BAKE_CACHE_VERSION and the bake code's
        build time (while it is tuned, every build bakes afresh). A hit skips
        the rock test and the solve.
    -   **Rock test data.** The render-mesh test (D68) now runs on every
        texel, with the ground (the landscape cache) under the sheet, on the
        worker thread, and its result is logged and dumped against the
        collision test's. The rock used is unchanged (collision or render
        meshes); whether the collision test can go is decided on that data.
    -   **Vertex swell.** The strands' swell (pressed together, striking
        rock), softened 4 passes, pushes the dense copy's vertices out along
        their normals: from swell 1 to full (12 units) at 4, none at open
        edges and half beside them. It rides the seams' shift (D69, now kept
        apart and composed), so both depth passes move it alike. Offline,
        swell is 0-2.5 over most of the sheet and 5-20 where it piles up.
    -   **Scroll probe.** Each fall's hidden original and its copy log their
        measured scroll once, to decide whether vanilla's per-fall speeds can
        come back.
-   References: D67, D68, D69, D73, D80
-   First run (2026-10-20 14:03, 18 meshes):
    -   The render-mesh and ground tests take 9-38 ms a mesh on the worker
        thread (the collision's: about 2 s a mesh on the main thread).
    -   The ground test is unusable: offline it turned 43k texels of today's
        visible water (about a third) into rock. The landscape's 128-unit
        height grid smooths every cliff, so its surface stands in front of a
        curtain falling down one.
    -   Render meshes alone would turn 4.2k texels of collision rock (2.4% of
        the visible water) into water, but those changes cascade through the
        strands' choices: 31% of the visible texels' streaks move more than a
        texel (one mesh by a median of 3.6). The collision test stays; the
        cache makes it a first-sight cost only.
    -   The hidden originals still scroll: the tall falls (TallInner01) at
        0.121 a second, the slopes (SlopeMesh01) at 0.173; the copies, 0 (the
        shared scroll stands in at about their mean).
    -   Swell: 3555 vertices of 18 meshes pushed out, at most 12 units. The
        user: "IT LOOKS GREAT NOW", "not perfect but a definitive improvement".

## D83: Falls appear sooner; a stronger swell; no ground test

-   Date: 2026-10-20
-   Decision:
    -   **The rock test skips the hidden lead-in** that no strand comes near:
        lead-in texels more than 12 texels across from running water in the
        same row (crests lie on different rows, so a row hidden in one column
        runs in the next, and its rock walls that water in). Offline on the 18
        dumps: 37% fewer rays; visible streaks moved more than a texel on
        0.08% of texels on average, 1.5% on the worst mesh (skipping the whole
        lead-in saved 43% but moved 12% on one mesh).
        `FallStrands::FindCrests` is shared by the bake and the march (outputs
        bit-identical on all 18 dumps).
    -   **Nearest first:** the bake queue is sorted by distance from the camera
        before each job.
    -   **Swell:** up to 20 units (was 12), whole at swell 3 (was 4).
    -   **The ground test is gone** (D82: unusable), and with it the ground in
        the cache key: the landscape cache fills in over time, so a fall baked
        before its cell's heights arrived would miss the cache on the next visit.
-   References: D79, D80, D82
-   Tested in game (2026-10-20 14:25-14:55):
    -   First sight, no cache: the first site's 5 meshes baked in 5.6 s (was
        10.1 s), the next 13 in 15.8 s (was about 24 s); 39% fewer texels
        rock-tested.
    -   **Bug, found and fixed:** bakes read from the cache never linked, so the
        falls never took their flow. The link was checked only as a bake
        landed. Originals queued before their dense copies existed sit in the
        queue until passed by; a solved bake lands seconds later (the queue
        already empty), a cached one the next frame (one stale entry left), and
        nothing landed after it. The check now runs after every bake step.
    -   **The cache key** now takes the strand code's build (FallStrands) and
        BAKE_CACHE_VERSION, not FallSites.cpp's own build time: a change to the
        rest of the bake bumps the version by hand.
    -   With the cache, every site takes its flow 0.2-0.6 s after its ground
        capture (18 of 18 read from disk). What is left is the capture itself:
        about 5 s for the first site, 20-25 s for the others.
    -   The user saw three steps: vanilla, then the dense copies (seams, cuts),
        then the flow; with the cache the last two come together.
    -   The swell reaches 20 units (3842 vertices of 18 meshes).

## D84: The swell piles up at rocks and stands up behind boulders

-   Date: 2026-10-20
-   Context: the user (SS 850): the water should bulge behind the rock,
    following the flow, and more "concave-y". The D82 swell came only from
    strands squeezed or striking rock; they steer round rocks smoothly, so
    little showed at a boulder.
-   Decision:
    -   **Pillow:** a strand that sees rock within its 5-row look-ahead piles
        up, more nearer and more head-on (swell up to 3).
    -   **Rooster tail:** behind each boulder (a row's run of rock with water
        at both ends, at most 48 texels), the water stands up in a ridge on
        the wake's middle, none at the boulder's sides (1 - across²), highest
        4 rows behind and fading over 8 (swell 3.5).
    -   **Rounder mounds:** the swell is softened 2 passes (was 4).
    -   Tried first and dropped: a hump where the strands' wake closes. On
        bake 4 the wake behind the boulder narrows from 26 to 9 texels and
        never closes on the mesh, and short-lived gaps at the crests (strands
        starting on different rows) raised false humps.
-   References: D80, D82, D83

## D85: The falls wobble with the flow

-   Date: 2026-10-20
-   Context: the user wanted the bake to drive the water's volume up and down as
    it passes, "not random, but driven by the baked flow" (then: "you can mix
    both"). A shallow-water solve (scratchpad pipesim.py on bake 4) gave pillows
    but a dry wake, not a rooster tail, and settles to a steady state, so no
    wobble; and on near-vertical curtains it needs an invented stiffness (the
    user: "wouldn't shallow water break here because the falls ... get very
    vertical?"). A falling sheet is supercritical: a disturbance only rides
    down the flow, at its speed.
-   Decision:
    -   **Disturbance in the bake:** each strand is stirred where it piles up,
        strikes rock or presses against the rock ahead, and carries that down,
        fading over 0.8 s; behind each boulder its wake's middle is stirred too.
        Softened like the swell; cached (BAKE_CACHE_VERSION 2).
    -   **Per vertex** (the per-vertex buffer grows to 12 floats): its normal
        times the wobble's size (1 unit calm, up to 6 where most stirred; half
        beside an open edge, none on it), and its streak and run from the
        linked bake (what the texture reads).
    -   **In both vertex shaders** (Lighting, Utility): waves square to the flow,
        6 crests per texture repeat, phased by the run less the shared scroll,
        so they ride down with the texture; broken up by a smooth noise carried
        the same way (65/35). Periodic over a repeat, so the phase's wrap never
        jumps. The falls' Lighting shader also offsets its previous position by
        the wobble a frame ago (PreviousPhase), for motion vectors.
-   References: D67, D69, D80, D82, D84

## D86: Smoothed open edges; foam off by default

-   Date: 2026-10-20
-   Decision:
    -   **Open edges smoothed as curves** (SS 852: vanilla's ragged teeth along a
        sheet's lower edge, once covered by its effect layers): each edge vertex
        drawn half way toward the middle of its two neighbours along the edge, 6
        rounds, at most 24 units; joints of three or more edge runs, vertices
        closing a gap (D69) and edges over a leading mesh are kept. Through the
        same shift as the seams, so both depth passes agree.
    -   **"Foam on the Falls" off by default** until the foam layer replaces it:
        at its 30% cap, lace on vanilla's near-white texture, it barely shows.
-   References: D34, D42, D67, D69, D73
-   **Bug, found and fixed (D85's first run): flat white sheets, the falls
    showing only in cracks.** The falls' depth pre-pass runs before
    FluidField::Prepass moves the shared phase on, the lit pass after it. With
    the wobble riding the phase, a frame's step between the passes moved the
    vertices apart, and the lit pass failed its depth test almost everywhere.
    The phase is now latched once per rendered frame at its first falls draw
    (globals::state->frameCount) and every falls draw of that frame reads it;
    the previous frame's latched phase feeds the motion vectors. The texture's
    scroll reads the same latched phase. This confirms the frame-order concern
    of research/waterfalls/realtime-integration-design.md (phase 0); the seam
    dither's Time has the same exposure, smaller.

## D87: A visible wobble; edges over other meshes smoothed too

-   Date: 2026-10-20
-   Decision:
    -   **Wobble size:** 4 units on calm water, up to 20 where most stirred (was
        1 and 6: the user saw it only in a few spots). 12618 vertices of the 18
        meshes wobble. The user: "wobbliness much better already".
    -   **Edge smoothing** now takes the open edges that lie over a leading mesh
        too (they fade into it, but their teeth showed: SS 860), 10 rounds, at
        most 40 units.
-   References: D85, D86

## D88: The wobble is pinned at the banks and rises mostly up

-   Date: 2026-10-20
-   Context: SS 861/862: the sheet's edge dipped under the ground as it
    wobbled, the terrain showing through.
-   Decision:
    -   The wobble fades in from the mesh's open edges over 120 units, measured
        along the mesh (shortest paths over its edges from every open-edge
        vertex), smoothstep; it replaces the half size beside an edge.
    -   The wobble rises mostly up from the mesh: the wave's -1 to 1 maps to
        -0.15 to 0.85 of its size (the user: "mostly up ... a little bit of
        downwards wobble is ok").
-   References: D85, D87
-   **D89 (same day): none inside rock.** SS 864/865: the wobble lifted the part
    of a sheet that runs inside a boulder out through the rock's face. Vertices
    inside rock or ground (the bake's rock mask at their texture coordinate,
    which takes the collision's terrain too) get no swell and no wobble, and
    seed the fade like the open edges: the wobble comes in over 120 units from
    rock as from the banks.
