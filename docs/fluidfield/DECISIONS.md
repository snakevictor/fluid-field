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
