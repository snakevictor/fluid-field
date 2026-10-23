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

## D90: Faster falls, longer waves, foam at intermediate pools and lips

-   Date: 2026-10-20
-   Decision:
    -   **Falls Speed** setting (default 1.75x vanilla, 0.5-3): the shared scroll
        phase runs at that rate, so the texture and the wobble speed up alike,
        with no stretch (stretching came from speed varying across a mesh) and
        cascades still meeting. The user: "some waters look painfully slow ...
        waterfalls seldom look calm". FluidField.ini 0-34-0.
    -   **The wobble's waves are longer** (2.5 crests per texture repeat, was 6;
        noise lattices 2x2 and 3x4): shorter than the dense copy's 20-50-unit
        vertex spacing, they folded triangles into zigzags (SS 866/867).
    -   **A stronger wobble** (10 calm, 28 most; edge and rock fade over 50 units,
        was 120): D88/D89's fades took most of it on narrow, rocky meshes.
    -   **Plunges at intermediate pools:** a mesh crossing one of our own pools
        (ComputePools) anywhere down its length plunges there; the engine's
        planes stay foot-only (D70).
    -   **Lips:** where each mesh's crest is within 24 units of the pool it
        comes from, a plunge point (strength 0.35, the water drawn on toward the
        crest) stirs and foams the shallow water there.
-   References: D46, D70, D78, D85, D88, D89

## D91: Seamless wobble; lips on any water; even, denser copies; spray out of the face

-   Date: 2026-10-20
-   Decision:
    -   **The wobble loops seamlessly:** a whole number of crests per texture
        repeat (2; D90's 2.5 jumped each time the scroll's phase wrapped, the
        "loop error" the user saw).
    -   **Lips on the engine's water too** (a river's plane; SS 869-871: no foam
        where the falls begin): within 40 units of the crest, strength 0.6.
    -   **Evenly split copies (red-green):** each pass splits only the edges
        longer than DENSE_EDGE (now 20 units); a triangle with one, two or
        three split edges becomes two, three or four (the quad by its shorter
        diagonal), up to 6 passes and 60000 vertices. The swell and wobble
        creased the old copies' 40-50-unit triangles (SS 870), and uniform
        splitting left vanilla's long thin triangles long and thin. Timing logs
        on the copies, seams and swell. BAKE_CACHE_VERSION 3.
    -   **Seams only near open edges:** the overlap test now asks only vertices
        on or next to an open edge (the rest are never read); asking every
        vertex of a dense copy against every nearby triangle was the likely
        stutter of D74's three levels.
    -   **Spray out of the face the mesh shows** (its winding normal, along which
        the swell pushes out), not "up", which threw it into the hollow under a
        curtain (SS 872); none from the hidden lead-in.
-   References: D70, D73, D78, D85, D90

## D92: Outward from the way the water travels; lips upstream; longer edge smoothing

-   Date: 2026-10-20
-   Context: D91 deployed (checked: DLL, shader, log) but did not fix SS 873-877.
-   Decision:
    -   **Outward face:** each mesh's outer side is the one facing the way its
        water travels (from its top rows, under the water above, to its foot,
        out from the cliff), tipped up (z 0.3). D91 trusted the winding, which
        points into the cliff on some meshes: spray from the hollow under the
        curtain (SS 877), and the swell and wobble pushing inward. Spray, swell
        and wobble all use it now; the log counts the normals turned.
    -   **Lips 40 units upstream** of the crest: at the drop the sim's flow is
        fastest and carried the lip's foam straight over the edge (SS 874-876).
    -   **Edge smoothing 80 rounds, at most 60 units:** on the dense copies'
        20-unit edges, 10 rounds smoothed only about 60 units, shorter than
        vanilla's teeth (SS 873).
-   References: D86, D90, D91

## D93: Refractive falls, drawn by a pass of our own (branch feature/fluid-field-refractive-falls)

-   Date: 2026-10-20
-   Phase / spike: real-time falls, translucency (research/waterfalls/realtime-integration-design.md 5.2); on
    the branch `feature/fluid-field-refractive-falls` only, untested in game
-   Decision: the falls should look like the river under them (Water.hlsl with the Fluid Field hooks), not an
    opaque scrolling texture. Two things are built, both off by default:
    -   **The cheap experiment ("Blended Falls Test", Advanced).** D53-D56 switched a blended NiAlphaProperty on
        live lit geometry and it drew nothing; the design note ranks "routing is chosen at setup" first. D73's
        dense copies are ours, so `FallSites::Densify` now gives a copy a clone of its model's effect layers'
        alpha property (flags 0x10ED: blended, SrcAlpha / InvSrcAlpha; the original's own if no layer has one)
        **before** SetupGeometry / FinishSetupGeometry, and the lit shader lowers its alpha (to 0.4 on clear
        water, 1 on white water; `FallGround::BlendAlpha`, FALL_BLEND_TEST_ALPHA). Only copies built after
        it is set (leave the area and come back). Expected: if cause 1 was the whole story, the copies now draw
        in the blended pass, forward lit, out of the depth pre-pass. Even then the result is not the river's
        look (cause 2: Lighting.hlsl's alpha also goes to the G-buffer targets that CS blends, and the
        composite relights them; no refraction, no thickness), so this only answers the D53 question.
    -   **The refractive pass ("See-Through Falls", Waterfalls > Water; Clarity, Refraction).**
        `FallRefraction` draws every grounded lit piece (`FallSites::GetLitMeshes`: the dense copies on AE,
        the originals where there are none) after the world, water and blended geometry included, just before
        the spray (`DrawGroundFogAfterWorld`), with `FluidField/FallSurface.hlsl`:
        -   Our own immutable buffers per piece, built once from its CPU copy (rawVertexData / rawIndexData:
            position, texture coordinate, normal) and read by SV_VertexID (t125), so SV_VertexID indexes the
            per-vertex buffer (t123) exactly as in the lit passes. The engine's own GPU buffers were not used:
            their packed layout would need an input layout per vertex format.
        -   The vertex shader is the lit passes' move: `FallGround::Shift` (seams, swell, wobble) at the frame's
            latched phase (D86), the cut (`ClipDistance`) and the seam fade (`Fade`). The constants come from
            the same code as the lit passes (`PrepareFallGroundData`, split out of `SetupFallGeometry`), so the
            phase, scroll, bake (t124) and cut are the same.
        -   The pixel shader ports Water.hlsl's look: the scene is copied before the pass (what lies behind),
            sampled offset by the normal map's tilt as seen from the camera (less where little water is behind;
            a sample landing in front of the sheet falls back to the straight one); absorbed along the view ray
            by the water between the surface and the scene depth behind it (at most 600 units, a curtain in front
            of a cave is not that thick) plus the sheet's own 8 units over the cosine, with the water type's
            shallow and deep colours (the cell's water, else the worldspace's, times the weather's water
            multiplier, as Util::TryGetWaterData); fresnel (the water type's fresnel amount) over Dynamic
            Cubemaps' reflection cubemap, or the sky's ambient without it; the sun's glint; white water from the
            bake as in the lit pass (foam, aeration, wake, pools; "Foam on the Falls") plus vanilla's streak
            texture read as aeration. Premultiplied, depth tested against the live depth (read-only), no depth
            write. Seams fade by alpha (no dither) and the sheet fades in over its first 4 units of water.
        -   **The lit pieces are hidden while it runs** (app-culled through `FallSites::SetLitHidden`, shown
            again when it is off, on Clear and with the debug views, which the lit pass draws). Refraction needs
            the scene behind the sheet without the sheet in it, in colour and in depth; keeping them for depth
            or shadows would put the opaque sheet in exactly what we refract. Lost with them: their shadows,
            their place in the depth pre-pass (SSR, AO, fog, the spray's soft contact see through them) and in
            water reflections.
        -   Every resource is named; every piece of state touched is saved and restored. A shader failure or a
            scene target that cannot be copied (multisampled) leaves the lit falls on (`IsUsable`).
    -   Settings: RefractiveFalls (off), FallClarity 160, FallRefraction 1, FallBlendedCopies (off);
        FluidField.ini 0-35-0.
-   Untested: none of it has run in game; the C++ was not compiled on the branch (no build folder in the
    worktree); FallSurface.hlsl and Lighting.hlsl were compiled with fxc.
-   Risks:
    -   **TAA and motion vectors.** The pass writes no motion vectors: the pixels keep the rock's behind (a
        static scene's camera motion), so the sheet's own parallax and the wobble ghost under TAA, and the
        refracted scene smears with it. Vanilla water has the same exposure; it may need a motion vector write
        (a second target) or a reactive mask for the upscalers.
    -   **Sorting.** No depth write: overlapping sheets (curtain front and back, cascades) blend in draw
        order; the spray behind a sheet draws over it (the scene depth has no falls); the ground fog lies over
        the falls by the rock's depth. The vanilla effect layers (FallLayerMask) are still drawn earlier,
        behind or in front by their own depth, and are refracted with the scene.
    -   **Thin-sheet absorption.** The scene depth behind a sheet is not the water's thickness; Clarity,
        MAX_PATH and SHEET_THICKNESS are first guesses. Distance fog is not applied to what the pass adds
        (only the refracted scene carries its own fog), so far falls may look too bright.
    -   The hidden pieces' world transforms are read while app-culled (static meshes; updated when attached).
-   References: D53, D54, D55, D56, D67, D73, D85, D86, D90;
    research/waterfalls/realtime-integration-design.md 5.2

## D94-D96: Edges blended in; foam where the falls cross water; buried parts cut; see-through flow fixed

-   Date: 2026-10-20
-   Decision:
    -   **D94, edge moves blended into the surface:** the open edge's smoothing
        and gap moves fade into the mesh over 120 units (harmonic, 60 rounds).
        Moved alone, the edge row folded the dense copy's 20-unit triangles over
        the next row: the zigzags of SS 873, 881, 882 came from D86/D92's
        smoothing itself, worse the more it reached.
    -   **D94, foam where a mesh crosses a water surface** (the user: "detect the
        intersection of the waterfall mesh with the water plane and foam around
        it"): every pair of neighbouring water texels on either side of an
        engine plane over them, or of our pools, makes a foam and stir point
        (strength 0.8) on the line between; it replaces D90-D92's crest lips.
    -   **D95, buried parts cut** (the user: hide the arch inside the top and
        bottom water and where meshes cross, "to make them look as one single
        body of water falling"): the hidden lead-in under the upper water
        (D79's), every vertex lying wholly on a leading mesh (not only edges),
        and the foot 6 units under the water it falls into (was 24).
    -   **D96, see-through flow:** FallSurface.hlsl handed Flow the mesh's raw
        texture coordinates; Flow takes them as the lit pass has them (after
        vanilla's scroll and zoom) and takes those back out, so the see-through
        texture slid with vanilla's scroll against the flow (SS 884).
-   References: D67, D69, D79, D86, D92, D93
-   **Bug (D94's first run): the falls stretched into spikes across the
    valley.** Renaming the edge blend's `next` to fix a shadowing warning
    missed its copy-back line, which then read the edge smoothing's `next`
    (world positions) as moves. Fixed; ComposeShifts now leaves any move that
    is not finite or past 120 units still and logs the count by source.
-   **D97: holes at the falls' backs (SS 887, 888).** D95 cut the whole hidden
    lead-in and every vertex lying on a leading mesh. Where no river lies over
    the lead-in it is the visible sheet, and the leading mesh is itself cut in
    places, so both holed the falls. The lead-in is now cut only 4 units under
    a water surface over it (the engine's planes or our pools); the overlap
    cut is off.

## D98: Cut inside each other's curl; stronger drop foam

-   Date: 2026-10-20
-   Context: the user: where two C-shaped falls meshes cross, "cull the innard
    geometry", as for the arch inside the water (big see-through parts
    extending inside each other); the drop foam works, make it 50% stronger.
-   Decision:
    -   A vertex lying behind a leading mesh's outer face (outer: the side its
        water travels toward, level, tipped up), more than 6 and less than 150
        units behind it, with that face itself shown (not cut by rock or water),
        is cut. Replaces D95's "on a leading mesh" cut, which holed sheets where
        the leading mesh was itself cut.
    -   Where a mesh crosses a water surface its foam reaches 1.5x a plunge's
        radius, at strength 1 (was 0.8).
-   References: D67, D92, D94, D95, D97

## D99: Falls in their river's water; the square patches (branch feature/fluid-field-falls-look)

-   Date: 2026-10-20
-   Phase / spike: See-Through Falls (D93) look; on the branch
    `feature/fluid-field-falls-look` only, untested in game
-   Context: the user, of the see-through falls: "it looks stunning but it's
    still much different from the shallow water color/look"; and SS 889,
    "weird checkerboarding on some viewing angles": bright white squares tens
    of pixels across on the sheets.
-   Decision:
    -   **Each piece's own water.** `FallWaterType` took one water type, the
        cell's at the camera. Now each lit piece looks up the water it pours
        out of (`FindFallWaterType`, cached per piece, again every 120
        frames): the highest engine water object (TESWaterSystem, under its
        lock) whose bound overlaps the piece's footprint (its world bound)
        and lies no more than 64 units over its top, so the river at the
        crest wins over the pool below; else the cell's water at its top, else
        the worldspace's. The pass takes a `WaterLook` per draw.
    -   **Water.hlsl's composition** (FallSurface.hlsl), with every value from
        the water type (TESWaterForm::data, as BSWaterShader's constants):
        shallow to deep over the above-water fog's far distance
        (FogParam.z), and what lies behind hidden by the fog's curve
        (refractionMul = 1 - ((far - depth) / (far - near)) ^ amount), over
        the water along the view ray (behind the sheet, at most 600 units,
        plus the sheet's 8 over the cosine) read **Water Depth** times deeper
        (new setting FallDepth, 2x, 0.25 to 8; it replaces Clarity, D93). The
        colours are the water type's times the weather's water multiplier
        and are **no longer multiplied by our ambient and sun**: Water.hlsl
        takes them unlit (ExtractLighting only splits them for the shadow),
        which was most of the mismatch. Fresnel as GetFresnelValue (the
        fresnel amount; ENB's water settings when on); the reflection tilted
        by the reflection magnitude about the sheet's own normal (Water.hlsl's
        flat up); without a cubemap, the reflection colour times the
        reflection amount (vanilla's water without reflections), instead of
        the sky's ambient; the sun's glint with the sun specular power and
        1 - exp(-sun specular magnitude) (Water.hlsl's DeepColor.w; that
        DeepColor.w is that magnitude is our reading, not checked), the
        directional light through Color::DirectionalLight as GetSunColor;
        ENB's sun scatter in the body. The foam overlay is unchanged.
    -   **The squares (SS 889).** Not the scene copies: SceneColor is a copy
        of the bound target (its own size) and SceneDepth the bound depth,
        both read at SV_Position inside the dynamic-resolution region, so no
        scale mismatch makes blocks. Two causes found, both fixed:
        1.  **The cubemap (most likely, medium confidence).** The pass bound
            Dynamic Cubemaps' _reflections_ cubemap always. The engine's
            shaders are given it only while reflections are the active
            capture and it holds one (`DynamicCubemaps::PostDeferred`): in
            interiors without reflections it is never written, and after a
            cache clear or a load it holds nothing yet. Its texels are then
            undefined, and at grazing angles the fresnel shows them, a few
            texels magnified over the flat sheet: bright squares that depend
            on the view angle. Now the same choice as the engine (the
            reflections one when active and valid, else the plain one when
            valid, else none: the water type's reflection colour), read at
            mip 1 (falling water is broken; prefiltered, so smoother), and
            clamped (a NaN or runaway texel becomes 0 or 64, never a square
            after bloom or the upscaler; D3D's min and max drop a NaN).
        2.  **The pools' foam (likely where it shows at the foot or lip).**
            FallGround::Pool answers yes or no at 0.2 of its bilinear
            footprint, so its foam switched along the pool map's 16-unit
            texels; on the see-through sheet the foam also makes it opaque, so
            each texel showed as a white square. The pass reads the pool
            weight itself and fades the foam in (0.2 to 0.7), and only at a
            mesh's foot, as the cut (D71: near a lip the captured pool
            reaches over the drop). The lit pass (Lighting.hlsl) keeps the
            old test.
        -   Also: every normalise guarded (a zero normal, normal map texel or
            derivative gave NaN), the final colour clamped to 0..64.
    -   FluidField.ini 0-36-0 (FallClarity replaced by FallDepth).
-   Untested: not run in game; the C++ was not compiled (no build folder in
    the worktree); FallSurface.hlsl compiled with fxc (vs_5_0, ps_5_0, no
    warnings), its constant buffer checked against the C++ (144 bytes).
-   Risks: the water bound over a piece's footprint may be a neighbouring
    river (the footprint is the bound sphere's square); the fall's water is
    not fogged by distance (as D93); the river's fog distances may make thin
    sheets almost clear or almost opaque for some water types (Water Depth
    tunes it for all).
-   References: D71, D91, D93

## D100: Only the falls' outer side shows; drop foam set back from the seam

-   Date: 2026-10-20
-   Context: SS 890-892, sheets crossing and nested still showed through each
    other ("ideally only the outward facing meshes would be visible"); the drop
    foam now shows everywhere checked ("instead of strengthening it, start it a
    few units back from the seam").
-   Decision:
    -   Per vertex, whether its winding faces its mesh's outer side (D92's
        travel) rides VertexClip's a.w (+2 inward; the foot flag is its value
        mod 2). See-Through Falls discards a sheet seen from its inner side.
    -   Crossing foam back to strength 0.8 and a plunge's reach, set 32 units
        onto the water's side of the seam.
-   References: D92, D94, D98

## D101: Pushes held to a slope; cuts by distance, faded; soft contact under foam (on branch feature/fluid-field-smooth-edges)

-   Date: 2026-10-20
-   Context: the user keeps seeing "brutal jagging" on the falls: zigzag,
    white-tipped spikes along a sheet's edge where it meets a boulder and along
    its outline. Causes, ranked (an offline experiment, scratchpad exp.py: a
    jittered 20-unit triangle grid round a boulder with a pillow swell, the
    rock mask on 10-unit texels):
    1.  **The swell push stepped from 20 units to 0 over one triangle** at rock
        and open edges. D89's rock test took the nearest 10-unit texel (a
        staircase outline), the swell is highest right against the rock (D84's
        pillow, strikes), and nothing faded it toward the rock; D82's half push
        one row in from an open edge followed the dense copy's irregular rows.
        The experiment: slopes up to 1.6 (units of push per unit along the
        mesh), 70 edges with a step over 10 units, all along the boulder.
    2.  **The see-through pass's alpha:** `saturate(behind / 4 + foam)` made
        foam wholly opaque right at the rock, so the faceted line where the
        sheet's triangles enter the rock's showed as a hard white sawtooth.
    3.  **Whole-vertex cuts** (D97's lead-in under water, D98's curl) at
        -CLIP_RANGE: SV_ClipDistance is linear across a triangle, so the cut
        line ran along the triangles' edges, up to 10 units off a straight line
        (0 with signed distances), and with no fade. (D70 already dropped the
        per-vertex rock cut, so this is not the boulder's jag.)
    4.  The wobble's fade from rock seeded on the staircase: small (its slope
        was at most 0.84 and the seeds within 7 units).
    5.  Open-edge smoothing folding: D94's blend already took this.
-   Decision:
    -   **Pushes held to a slope** (ComputePushes): the wobble's size and the
        swell's push are each lowered to the largest function under them that
        climbs at most 0.5 units per unit along the mesh (WOBBLE_SLOPE,
        PUSH_SLOPE; a min-plus Dijkstra over the mesh's edges), from 0 on open
        edges and in rock. Replaces the half push beside open edges. In the
        experiment: slope at most 0.5 (was 1.6), steps over 10 units only on
        the longest edges (11, was 70). The pillow right against a boulder is
        lower (10 units at the face there, rising over about 40 units).
    -   **Rock read bilinear**, inside from half: a smooth outline.
    -   **Cuts by distance:** the lead-in's by its height over the water's level
        less LEAD_CUT_DEPTH, the curl's by how far it lies behind the leading
        face less INSIDE_MARGIN (only vertices behind it), held to -CLIP_RANGE.
    -   **A dithered fade to every cut** (Lighting and Utility, the same
        pattern): the seam dither's fade times saturate(clip / 6)
        (FallGround::CutFade); ClipDistance returns CLIP_MOST, not 1, where
        nothing is cut, as does Utility's default. The see-through pass fades
        its alpha the same way, and its contact fade holds under foam
        (lerp(contact, sqrt(contact), foam): shorter, never opaque at 0).
-   Cost: per pixel one multiply (lit, depth) or a square root (see-through);
    on the CPU two Dijkstra passes per mesh when the pushes are composed.
-   Untested: the C++ was not compiled (no build folder in the worktree); the
    shaders compiled with fxc. Not seen in game.
-   Risks: the dither's fade reads SV_ClipDistance in both passes; their world
    positions are computed differently (as the cut always was), so a few pixels
    at the band may disagree between the depth pre-pass and the lit pass.
-   References: D56, D70, D82, D84, D88, D89, D93, D94, D97, D98

## D104: Clearances behind the sheet, baked (user's per-vertex depth idea, part 1)

-   Date: 2026-10-20
-   Context: the user: "use every X vertex to cast a ray downwards on its
    perpendicular normal to estimate the area depth": a view-independent depth
    behind the sheet for the see-through look (instead of the screen-depth
    difference), and later foam and spray.
-   Decision (part 1, the bake): during the bake's rock test, two more Havok
    rays per tested texel, from 2 units off the surface along and against its
    normal, record the first rock within 600 units (0 inside rock, 600 for none
    or untested). Stored in the BakeField and the disk cache
    (BAKE_CACHE_VERSION 4). About 50% more rays on a first bake; cached after.
-   Part 2 (after the shading and culling branches merge): pick the inner side
    per vertex (D92/D100 outward), pass the depth behind per vertex to the
    shaders, and let the see-through pass take it as its "behind" depth.
-   References: D57, D82, D92, D93, D100

## D102: See-Through Falls lit as the river; a thin, mostly opaque sheet (on branch feature/fluid-field-falls-water-port)

-   Date: 2026-10-20
-   Phase / spike: See-Through Falls (D93, D99) look; on the branch
    `feature/fluid-field-falls-water-port` only, untested in game
-   Context: the user: the Refraction and Water Depth sliders "didn't really
    change anything", the falls are "nowhere near what the shallow water looks
    like", and they glow at night (bright cyan-white sheets in a dark scene);
    then: not too glossy, keep the surface lively, too refractive (reads as
    glass), "transmit little to no depth".
-   How Water.hlsl builds a pixel (the refracting, depth-tested variant):
    the normal (vanilla's three normal maps or the flowmap's, Fluid Field's
    waves, Wetness Effects' ripples); fresnel (Schlick on FresnelRI.x, or
    ENB's); the reflection (the engine cubemap, Dynamic Cubemaps' capture
    near, SSR over it; or ReflectionColor x amount without reflections); the
    refraction (the scene offset by the normal, VarAmounts.w) and the depth
    behind measured against the ReflectPlane; the water colour lerped
    shallow to deep by depth / fog far, then **ExtractLighting** splits it
    into the directional light's share and the ambient's (by their
    luminance) and the directional share takes the sun's shadow
    (Get3DFilteredShadow: terrain, clouds, Volumetric Shadows); Skylighting
    scales the ambient; refractionMul (the above-water fog curve) blends the
    see-through scene to that colour; the sun's glint (SunColor x
    1 - exp(-DeepColor.w), the specular power, in the surface's shadow);
    Fluid Field's foam (FoamColor: the lit water colour 10% toward the
    shadowed scene light); vanilla distance fog over the result and over the
    refracted scene. The output is the HDR scene colour before tonemapping; no
    exposure is applied in the shader. The colours are never multiplied by a
    light: whatever darkens them at night is already in the constants.
-   Why the falls glowed at night (FallSurface.hlsl, D99): every term the
    river shadows or derives from its own lit colour was unshadowed or white
    here. The white water was 90% white times ambient plus the full
    directional light (the moon) with no shadow, and vanilla's streak texture
    put some of it on the whole sheet (the river's foam is its own water
    colour lifted 10%); the reflection without a cubemap was the reflection
    colour unlit; the glint took no shadow; and nothing shadowed the body.
    Falls sit against cliffs, so most of them are in shadow the river's
    shader would have applied.
-   Decision:
    -   **Ported, reused from CS:** `Common/ShadowSampling.hlsli`
        (ExtractLighting, GetLightingShadow, GetWorldShadow,
        GetDirectionalLighting, GetAmbientLighting) and Fluid Field's
        `FoamColor` (`WaterNormal.hlsli`, its normal texture names mapped to
        ours). The water colour is split and shadowed as Water.hlsl does it;
        the foam is FoamColor of that lit colour under the shadowed scene light,
        lifted 25% further toward the light (falls are whiter than rivers) and
        at most 60% cover (foam and particles will be laid over the base).
        The glint uses the shadowed directional light at half weight; the
        reflection half of the fresnel weight, read at cubemap mip 2 and
        dulled by foam.
    -   **Shadows:** a second pixel shader with `VOLUMETRIC_SHADOWS`, used when
        Volumetric Shadows is loaded and its map (t18) and the cascades (t98)
        are still bound at the draw (checked each frame; an unbound map would
        read as fully shadowed), else the plain one (no directional shadow).
        Terrain and cloud shadows are not compiled in (their defines are not
        passed).
    -   **A guard, `WithinLight`:** the water colour and the cubemap-less
        reflection colour are held to the luminance of the light falling on
        them. Water.hlsl takes the colours as the engine hands them;
        `MakeFallWaterLook` rebuilds them as the water type's times the
        weather's water multiplier (as Util::TryGetWaterData), which is not
        checked against the engine's constants. If the engine darkens them
        further, the guard keeps a fall from outshining its light at night;
        if not, it changes nothing.
    -   **A thin, mostly opaque sheet:** the water a ray crosses is the sheet's
        8 units over the cosine (at least a quarter) plus a quarter of the gap
        behind it, capped at 32 units (LONGEST_PATH 600 to 32), times Water
        Depth. It absorbs exp(-0.17 per unit) (8 units square on let a quarter
        through), aeration makes it up to 60% more opaque, and the water
        type's fog curve still adds (muddier rivers, more opaque falls).
        Shallow to deep reads that thickness at 8 units deep per unit against
        the fog's far distance. The gap behind comes from one function,
        `BehindDepth`, so a baked per-vertex distance to the rock can replace
        the scene depth.
    -   **Refraction:** the offset no longer scales with the gap behind (up to
        48 units) or falls off from 256 units away (now from 1024); 2% of the
        screen height per unit of tilt at Refraction 1, so a few pixels at the
        default.
    -   **Sliders:** both reached the shader (the constant buffer layout and
        the upload match, 144 bytes), but Water Depth only scaled a depth the
        river's fog distances (hundreds of units) barely saw on an 8-unit sheet,
        and Refraction's offset was scaled down by the gap behind and the
        distance, then mostly hidden under a see-through body that showed the
        scene anyway. Water Depth now runs the sheet from mostly clear (0.25,
        about 70% through) to opaque (8); defaults Water Depth 1 (was 2),
        Refraction 0.5 (was 1). The streak foam is 0.4 with Foam on the Falls
        (was 0.6), 0.25 without (was 0.4). Settings structure unchanged; saved
        values are kept (a saved Water Depth of 2 is now twice as opaque).
-   Not ported: SSR (no SSR texture at our draw point), Skylighting's ambient
    occlusion, IBL's fog colour and diffuse (ShadowSampling's IBL path is not
    compiled in), vanilla distance fog (its parameters are the engine's per
    material constants, not in SharedData; the refracted scene keeps its own
    fog, the water colour does not), Light Limit Fix's point light specular.
-   Untested: the C++ was not compiled (no build folder in the worktree);
    FallSurface.hlsl compiled with fxc (vs_5_0, ps_5_0, ps_5_0 with
    VOLUMETRIC_SHADOWS; no warnings). Not seen in game.
-   Risks: Volumetric Shadows' map at t18 being bound at the draw is
    assumed from it being set once per frame for the forward passes; if
    another pass leaves a different texture there, the sheet's shadow is
    wrong. The constants (ABSORPTION, FOAM_LIFT, REFLECTION_WEIGHT,
    GLINT_WEIGHT) are first guesses.
-   References: D91, D93, D98, D99, D100, D101

## D103: One outer side per sheet; inner sides hidden only behind another sheet; the curl cut only where covered (on branch feature/fluid-field-falls-culling)

-   Date: 2026-10-20
-   Context: after D98-D101 the C-shaped sheets' crossings look mostly right,
    but "holes still appear in places where they shouldn't": large chunks of
    sheet missing, the top and lip of a fall showing the cliff through it,
    parts vanishing seen from above or behind, gaps between neighbouring
    sheets.
-   Causes, ranked (offline: scratchpad culling/analyse.py and
    orient_test.cpp on the 18 latest bake dumps, texel grids, not the dense
    meshes):
    1.  **D92's outer side asked per vertex against the travel** (high
        confidence). A sheet's lead-in climbs to its crest facing up and
        upstream on its outer side, against the travel: 51% of all texels
        (on every dump 43-61%, three quarters of them in the mesh's upper
        half) were called inward and their normal turned into the curl.
        D100 then discarded the lead-in and crest seen from above (their real
        outer side), and the sign changed across the crest, where the
        interpolated flag crossed zero.
    2.  **D98's curl cut oriented each leading triangle the same way**
        (high): behind the lead-in turned round was the river side, so the
        lip region of a neighbour lying there was cut.
    3.  **D98's curl cut ignored how far to the side a vertex lies**
        (medium-high): "behind" was the signed distance to the nearest
        triangle's plane, so a neighbouring sheet running on past the leading
        sheet's open edge, slightly behind its plane, was cut up to 150 units
        from it (the gaps between sheets).
    4.  **D100 hid every inner side** (medium-high): a free curtain seen from
        behind or the lip seen from upstream has no other water in front of
        it; hiding its inner side left a hole.
    5.  **D97's lead-in cut read any water plane over it** (medium-low): a
        plane covers its whole bound, so a river or lake higher up the cliff
        could cut a lead-in standing in the open.
-   Decision:
    -   **One outer side per sheet** (OrientSheet): each triangle's side is
        carried to its neighbours across shared edges (vertices welded by
        position, so texture seams join; non-manifold edges not crossed),
        and each connected sheet takes the side its lowest third faces the
        travel with (the curtain over its foot). The curl cut (ComputeFades),
        the swell and wobble normals (ComputePushes) and the see-through
        pass's flags (ComputeSides) all use it. Checked offline: with a third
        of the windings scrambled and a seam split, every dump orients as one
        sheet, its foot facing the travel.
    -   **Inner sides hidden only where covered** (ComputeSides, VertexClip
        a.w +4): a ray from each vertex into its inner side, 6 to 150 units,
        must first hit a shown sheet (not cut) whose outer side faces away
        from it; only there does FallSurface.hlsl discard the inner side. The
        +2 flag now compares the normal the see-through pass reads (vanilla's)
        with the sheet's outer side, and the pass interpolates the turned
        normal itself (no sign crossing zero mid-triangle).
    -   **The curl cut only where the face covers the vertex**: the cut's
        distance is its depth behind the face less INSIDE_LATERAL (2) per unit
        it lies off to the side, so beside a sheet's edge it fades out.
    -   **The lead-in cut only by water at most 48 units over its column's
        crest** (the river feeding it).
-   Cost: on the CPU, once per load: one orientation pass per mesh (twice:
    seams and pushes) and one ray per vertex through the seam bins. Per pixel
    of the see-through pass one normalise more.
-   Untested: the plugin was not built (no build folder in the worktree);
    the helpers compiled and ran standalone (MSVC), FallSurface.hlsl
    compiled with fxc. Not seen in game.
-   Risks: the inner-side discard now leaves both layers of two parallel
    sheets facing the same way (neither covers the other), and shows an
    inner side whose covering sheet is cut (its lead-in under water): seen
    from upstream that is the curtain's back, as in reality. ComputeSides
    runs after ComputeFades and after ComputePushes, so it reads whichever
    cuts exist then. The cover ray follows the vertex's normal: seen at a
    glancing angle the covering sheet may not lie in front of a hidden
    inner side, a small gap near the covering sheet's edge.
-   References: D79, D92, D95, D97, D98, D100, D101
-   **D104 part 2:** the per-vertex buffer grows to 16 floats (FallVertex.d);
    d.x is the bake's clearance on the side the vertex's outer normal points
    away from (nearest texel at its texture coordinate). The see-through pass's
    BehindDepth takes it, or the screen-depth difference where that is less
    (something nearer than the rock, e.g. another sheet).

## D105: The falls translucent like the river; a narrower curl cut

-   Date: 2026-10-20
-   Context: SS 902: "the falls are all foggy now, instead of having the water
    translucency quality that shallow water has"; Refraction and Water Depth
    "seem to do nothing" (CS 793, 576); SS 900/901: culling too aggressive.
-   Decision:
    -   The sheet absorbs by the depth behind it (the baked clearance, D104, or
        the screen difference where nearer), not by a capped quarter of it plus
        its own thickness: ABSORPTION 0.012 per unit (40 units behind show 60%,
        200 units 9%), the depth behind counted whole up to 600, half the sheet's
        own thickness. D102's settings made it about 98% opaque at defaults,
        which also hid what Refraction and Water Depth do: both reach the shader
        (constant buffers checked field by field).
    -   The curl cut reaches 70 units behind a leading face (was 150), from 12
        (was 6): it cut neighbouring sheets meeting each other (SS 901).
-   References: D98, D102, D103, D104

## D106: The depth behind, from rays at the vertices; D107: gentler cuts; a leaner menu

-   Date: 2026-10-20
-   Decision:
    -   **D106, depth by vertex rays** (the user's idea): one vertex per 30-unit
        cell of each dense copy casts a Havok ray along its authored normal,
        from 40 units off its face, through it; the first rock or ground past it
        (up to 600) is its depth behind, spread over the mesh between samples
        (60 rounds), in VertexClip d.y; See-Through Falls reads it alone. A few
        milliseconds a frame (DepthStep), about 60k rays for 18 meshes.
    -   Found on the way: the bake's clearance rays (D104) started inside the
        rock the sheet dips into (Havok sees no face from inside: everything
        read open); triangle winding is not consistent on these meshes; and
        their authored normals point into the bed, away from the visible face
        (logged: against them, two thirds of the rays went up into the air).
        The screen-depth conversion reads as noise in our pass and is unused.
    -   **D107:** the curl cut 45 units behind a leading face, from 20 (was
        70, 12); the lead-in cut 16 units under the water (was 4): both still
        ate visible sheet (SS 913).
    -   **Menu:** removed the vanilla layer toggles, the Blended Falls Test, the
        Waterfall Overlay and Sites views, Hide Vanilla Waterfall Effects, the
        fog debug modes and the verify/log checks.
-   References: D97, D98, D102, D104, D105

## D108-D110: One ray per shown vertex, against a consistent outer side; a depth view to check it

-   Date: 2026-10-20
-   Decision:
    -   **D108:** a single ray per sampled vertex (no "both ways" fallback:
        the user's call, the direction must be right, not hedged), and two
        views to check it: Falls Mesh Debug "Depth Rays" (green to a hit,
        red to the 600 reach) and Waterfall Shading "Depth Behind".
    -   **D109:** each sheet's outer side carried across the mesh
        (OrientSheet) and chosen by the majority of vanilla's authored
        normals; the rays go against it. Logged: 0 of 372,729 vertices
        disagree, so vanilla's normals are consistent per sheet (the earlier
        "inconsistent" reading was a mislabelled counter).
    -   **D110:** rays only from vertices that are shown: not cut (rock, the
        water, the curl, the lead-in: VertexClip a.x > 0) and not hidden
        behind another sheet (a.w < 4). The depth step now also waits for
        the swell's cuts (ComputePushes), and starts each ray where the
        vertex is drawn (rest plus the seam gap and swell moves), not its
        rest place (SS 916's offset). Rays round the buried and hidden parts
        of the C were noise in the overlay (the user).
    -   **D110, Depth Behind in bands:** red under 50 units, yellow to 150,
        green to 300, blue to 600, magenta where nothing was hit. The grey
        ramp read as nearly all white through the tonemap even with medians
        of 60 to 250 units (logged), which made the depth look missing.
    -   Checked in game (Riverwood falls): red rings where a sheet hugs
        rock, yellow on the sloped runs, green and blue on the open
        curtains; magenta only in a few patches. Logged per mesh over shown
        vertices: depth 10% 9-43, median 61-248, 90% 363-600.
-   References: D104, D106

## D111: No depth rays from the underside; the overlay draws from the vertex

-   Date: 2026-10-20
-   Decision:
    -   A vertex whose outer side faces more than about 17 degrees down (unit
        normal z < -0.3: the C's curl back under its crest toward the rock)
        casts no depth ray; its depth is filled from the samples round it.
        Its ray went up into the hollow behind the curtain (SS 955: a mass
        of rays on what is basically the underside, the user); the ground
        is never above.
    -   The Depth Rays overlay draws each ray from the vertex to its hit (or
        the 600 reach), without the 40-unit lead-in off the face, which read
        as rays offset from the vertices (SS 954).
    -   Logged: rays now point down on about 85-90% of the curtains' samples
        (was 75-80%) and about 90% on the sloped runs.
-   References: D106, D110

## D112: One test for what is shown; the culling worked out again as the cuts become known (branch feature/fluid-field-culling-2)

-   Date: 2026-10-20
-   Context: SS 958 with the Depth Rays overlay (CS_2026-10-20_22-02-55_761):
    a large red fan (rays that hit nothing) from the curtain's foot under the
    pool, and green rays from the parts of the sheet buried in the overhang.
    SS 900, 901 and 913 (and 887-899): culling still punches holes where the
    sheet should show. The user wants the overlapping C-shaped curtains to read
    as one body of water: what is inside rock, under water or inside another
    sheet's curl hidden, the visible outer surface whole.
-   Causes:
    1.  **The CPU never knew two of the ways a vertex is hidden.** The pools' cut
        at the foot happens only in the shaders (FallGround::ClipDistance, per
        frame), so a.x > 0 under the pool; and rock hides by the depth test
        alone (D70 dropped the per-vertex rock cut; the bake's rock mask needs
        rock on both sides of the sheet, D59, so a sheet running into a cliff
        is not in it). DepthStep's "shown" test (a.x > 0, a.w < 4) passed both.
    2.  **The curl cut ran once, too early** (likely the main cause of the
        holes, not checked in game): ComputeFades ran it as the seams were done,
        before the lead-in cuts (ComputePushes, after the bakes), the pools
        (PollPools runs after the first ClipStep) and the rock were known. A
        vertex behind a leading face that is itself cut later (the lead-in under
        the river), under the pool, in rock, or dithered out at a seam was cut
        with nothing left in front of it: a hole to the cliff. It was never
        worked out again, and each sheet read its leaders in list order, not
        after their own curl cut.
    3.  **The inner-side cover test** (ComputeSides, D103) took a covering sheet
        as shown on a.x alone: a covering face under the pool or in rock still
        hid the inner side behind it.
    4.  DepthStep added the seam and swell moves (b.xyz, in the mesh's own space)
        to world positions unturned and unscaled.
-   Decision:
    -   **VertexCut** (CPU only, parallel to vertexClips): a.x's parts kept apart
        (the water below, the curl, the lead-in; a.x is their least, ComposeCut),
        plus the pools' cut as the shaders make it at the foot (PoolCutAt:
        BuildPoolMap's level less POOL_CUT, at the drawn position) and a buried
        flag. Each part can be worked out again without the others' mins
        piling up.
    -   **VertexShown**, the one test of "drawn at all": a.x > 0, not faded at a
        seam (a.y >= 0.5), above the pools' cut, not buried. The curl cut's
        leading faces, the inner-side cover test, DepthStep's samples and its
        audit all use it. DepthStep keeps two rules of its own: not a.w's +4
        (inside another sheet's curl, seen only from its hollow, D110) and not
        the underside (D111).
    -   **BuryStep** (Havok, 3 ms a frame, after ClipStep): a vertex is buried
        when rays toward it from 160 units out along its normal, against it,
        from above and from either side along the sheet (level) all meet rock
        more than 24 units short of it (BAKE_ROCK_MARGIN's allowance for sheets
        dipping into the rock they lie on). It stops at the first ray that
        reaches. A ray starting inside rock sees no face (D105) and counts as
        reaching, so the test errs toward shown. Only VertexShown reads it: the
        depth test still does the hiding on screen.
    -   **RefreshCulling**: the pools' cut, the curl cut, a.x and the sides,
        in that order, from one set of sheets (BuildCullSheets, one OrientSheet
        per mesh). Run at the end of the seams (as before), when the rock test
        ends, after the lead-in cuts (ComputePushes) and when new pools arrive.
        The curl cut first undoes itself, then runs over the sheets highest
        first, so a leader's own curl cut is in place before the sheets behind
        read it; a leading face not shown hides nothing. DepthStep waits for the
        rock test.
    -   DepthStep turns and scales the moves into the world.
-   Cost: about one to two rays per vertex once per load (five for a buried
    one), at 3 ms a frame before the depth rays start; RefreshCulling a few
    times per load (one OrientSheet per mesh and the curl and cover tests).
-   Untested: the plugin builds (Release, CommunityShaders target, no warnings in
    FallSites); not run in game. The
    curl cut's thresholds (INSIDE_REACH, INSIDE_MARGIN, INSIDE_LATERAL) are
    unchanged: the holes are put down to cut faces that hid nothing, not to the
    reach.
-   Risks: the rock test's five ways can all be blocked for a sheet that is
    visible down a narrow slot (rock within 136 units on every side and above);
    only its depth ray and the faces it covers would be lost, not its drawing.
    The sort by height only approximates "leads" (as high and larger), so a
    leader within 8 units of a follower's top may be read before its own curl
    cut. With Hide Buried Falls off the shaders cut nothing, but the CPU still
    treats the cuts as made.
-   References: D59, D67, D70, D71, D97, D98, D103, D105, D110, D111

## D120: The falls read as the river's water: no sky mirror, no foam veil, air behind curtains

-   Date: 2026-10-20
-   Decision (See-Through Falls, FallSurface.hlsl; found with banded diagnostics and live shader reloads):
    -   **Normal:** turned to the camera. It was flipped by the triangle's
        winding (not consistent) and, facing away, squashed to graze the
        view: facing under 0.2 on most sheets, fresnel near 1, every sheet a
        half mirror of the bright sky. The main cause of the milky look.
    -   **No foam veil in the base look:** with Foam on the Falls off, no
        foam. The streaks' foam is the scene's full light against the
        water's dim pre-lit colour, so even 15% of it laid a pale beige veil
        over every sheet (SS 971: river dark, falls pale).
    -   **Water path:** the depth behind counts as water only where the
        sheet lies on rock (up to 32 units); past a 48-unit gap (a curtain)
        it is air and only the sheet's 8 units count. Counted whole, the gap
        made curtains solid water colour that no slider moved. Absorption
        0.03 per unit of that layer.
    -   **Aerated water:** coloured with its water type's hue at 1.5 times
        the river's lit brightness (lit from the water's own colour, so dark
        at night), and 0.6 aeration opacity again: falling water is opaque
        by its bubbles, slow water over rock stays clear.
    -   **Refraction:** 0.06 of the screen height per unit of tilt (was
        0.02), shrinking from 4096 units away (was 1024).
    -   Why the old look was turquoise: before D98 every fall used the
        default water type (turquoise), unlit and nearly opaque; since D98
        each takes its river's, a khaki green at Riverwood.
    -   The user: "now we're fucking talking".
-   Open: the pale strip where falls cross the river (the crossing foam,
    D-crossing, its own brightness); foam to be driven by the flow later.
-   References: D91, D98, D102, D105, D110

## D121: Foam where the falls break, lit like the water; a more opaque sheet

-   Date: 2026-10-20
-   Decision (See-Through Falls, with Foam on the Falls on):
    -   Foam along the cut lines (FallGround::ClipDistance within 24 units:
        the lead-in leaving the river above, the foot under the pool or
        river, a sheet cut into another's curl, rock), at seam overlaps
        (0.7), where the sheet meets rock or any mesh (screen depth within
        20 units, 0.9, as the river foams at its shore), at rock strikes and
        wakes (the bake), and over the water below (pool and height bands,
        as before). Broken up by the lace; at most 0.85 cover.
    -   Not by aeration any more: the aerated colour (D120) is the falling
        water's body; as foam it speckled every sheet.
    -   Foam colour: five times the water's lit brightness, 15% of its hue
        (LEARNINGS: foam lit by the scene's light veiled every sheet).
    -   Absorption 0.08 per unit (was 0.03): beside the river the falls read
        too clear (the user: "the river is shallower and more opaque").
-   Open: the band at a cut line is solid rather than lacy; the user is to
    judge strength and reach in game.
-   References: D120, D112

## D122: The falls' foam is the river's foam

-   Date: 2026-10-20
-   Decision: the see-through falls' foam uses the river's colour and
    cover exactly (the user: match the shallow water's foam in colour and
    brightness, driven by the baked flow):
    -   Colour: FluidField::FoamColor(lit water colour, sun times shadow
        plus ambient), as Water.hlsl calls it (the water colour lifted 10%
        toward the light). D121's five times the water's brightness, and
        D102's extra lift toward the light, are gone.
    -   Cover: as GetFoamCover: dense foam solid, thin foam only where a
        bubble pattern lets it through (threshold half the pattern, 0.25
        soft, at most 0.95). The pattern is vanilla's normal map running on
        the baked flow, as the river's runs on its simulated flow.
    -   Amount: the bake's foam (rock strikes) and wakes, plus D121's break
        lines (cut lines, seams, contact with meshes, the water below).
-   The see-through pass draws every sheet layer; inner layers showing
    through (SS 980) are left to the culling (D113+), per the user.
-   References: D121, D120

## D123: The falls refract the scene before the water; D124: long, dense foam at the drop's top and foot

-   Date: 2026-10-20
-   Decision:
    -   **D123:** the see-through falls refract a copy of the scene taken
        after the deferred composite and before the water draws
        (DrawGroundFogBeforeWater), used when the falls' target is the same
        texture in the same frame; otherwise the copy at the falls' draw as
        before. The river's surface seen through a sheet showed as a plane
        behind it, cut by the sheet (SS 981); the user asked to hide it.
        Things drawn after the deferred stage and behind a sheet (particles,
        other transparents) no longer show through it.
    -   **D124:** each vertex keeps how far it lies under the water its fall
        pours out of (VertexClip d.z: the water planes at or over its mesh's
        top, within 96 units of their bound; none for a mesh starting
        mid-fall under another sheet). The foam runs 120 units down from
        that water and 100 up from the water it falls into (was 26), sqrt
        so the bands stay near solid: the whole drop's top and foot, not
        every mesh's (the user: "a lot longer and denser").
-   References: D91, D105, D121, D122

## D125: The falls' front surface hides the layers just behind it

-   Date: 2026-10-20
-   Decision: the see-through pass first draws the falls' depth into a copy
    of the scene's depth (PSDepth: discarding what PSMain hides, the inner
    side over another sheet, the cut, and where it fades out by more than
    half), then shades every sheet but drops a fragment 2 to 48 units
    behind that front surface: inside the same body of water, where real
    water has no surface (overlapping meshes' inner layers). A sheet further
    back (a separate curtain, air between) still shows through the front
    one, as real water would. The user: the water "seems to be transparent
    to itself". Complements the culling (D113), which removes the inner
    layers from the geometry.
-   Falls back to drawing every layer if the depth format is not one it
    knows (D24S8, D32, D32S8) or PSDepth fails to compile (logged).
-   References: D91, D113, D123

## D113: The falls body as a union of volumes: a sheet cut where it enters another; culling over frames; the lead-in in the bed (branch feature/fluid-field-culling-3)

-   Date: 2026-10-20
-   Context: D112 in game: "Waterfall rock test: 1785 of 80882 vertices
    inside rock", "culling took 704.4 ms" (one frame). With the see-through
    pass off (lit falls):
    -   SS 974 (outside), SS 975 and 976 (camera inside): two C-shaped sheets
        cross. One has a hole where they meet, but both keep the rest of their
        inner parts running through each other inside the volume, which makes
        the translucent shading look wrong. The user wants each mesh to stop at
        about where it meets the other (a few triangles past it is fine).
    -   SS 977 (outside), SS 978 (inside): one sheet's crossing line into
        another is plain to see, and nothing there is cut.
    -   SS 979 (camera under the upper river): the start of the drop is mostly
        right, but the top of the mesh curves down into the riverbed and shows
        under the river.
-   Causes:
    1.  D98's curl cut asked only "behind a leading face, within INSIDE_REACH
        (45 units)": a band behind the leader's face, not the space it
        encloses. The follower's parts further in were kept (SS 975, 976), and
        only a band of it was cut, the hole. A follower that is not "led"
        (lower, or as high and smaller) never cut the leader (SS 978).
    2.  RefreshCulling (D112) ran every step on one frame.
    3.  SS 979: BuryStep's rays from under the ground start inside it and read
        as reaching, so a sheet under the riverbed was never buried; and the
        lead-in cut took the water over it only up to 48 units over its crest
        (D103), while the river stands deeper over a submerged crest.
-   Decision:
    -   **VolumeCut replaces the curl cut** (VertexCut::inside). The falls body
        is the union of the sheets' volumes; only its outer surface should
        show. A vertex lies inside another sheet when, of rays from it along
        the six axes (400 units), none meets that sheet coming in through its
        outer face, and at least three leave through it (the rest open on the
        rock, bed or water closing the sheet). Asked only of sheets whose bound
        holds the vertex, both ways for every pair (no leading order). Inside,
        it is cut from VOLUME_MARGIN (20 units, about one edge) within the
        other's surface, its nearest triangle, held to -CLIP_RANGE: the two
        meet a little past their crossing line, so no opening. INSIDE_MARGIN
        and INSIDE_LATERAL are gone; INSIDE_REACH remains the cover test's
        reach (D103).
    -   **CullStep**: the culling (the sheets one per step, the pools' cut, the
        inside cut, a.x, the sides) runs as a job at 3 ms a frame; asking
        again restarts it. DepthStep waits for it. The GPU gets the result
        once it is done.
    -   **Level ground buries**: a vertex 12 units under the landscape, where
        the height grid 64 units round it rises and falls less than 32 units
        (no cliff, D82), is buried without rays.
    -   **The lead-in cut reads our pools with no ceiling** (their water is
        confirmed over the rock, D40, D78); the engine's planes keep D103's.
-   Cost: per vertex in another sheet's bound, six rays through that sheet's
    bins (until the votes are decided), spread over frames with the rest.
-   Untested: the plugin builds (Release, CommunityShaders target); not run in
    game.
-   Risks: a sheet shaped so a ray from inside meets one of its own folds from
    outside first is read as not enclosing (errs toward shown). A sheet whose
    bound holds another's foot under a flat run (the water between the run
    and the bed) cuts that foot: inside the water body, so meant, but new.
    Where the two sheets' volumes are only closed by rock on many sides, fewer
    than three axes may leave through the sheet, and nothing is cut.
-   References: D40, D78, D82, D98, D103, D112

## D126: Only the falls' front surface, however far the layer behind

-   Date: 2026-10-20
-   Decision: D125's 48-unit gap dropped: every falls layer behind the front
    surface is hidden. A C's far wall, hundreds of units behind its front,
    passed as a separate curtain and still showed through (SS 987); with
    front only, "FUCK yes its gone" (the user). A separate curtain behind a
    front one is hidden too; with the sheets this opaque it does not read.
-   References: D125

## D127: The falls composed as the river's water

-   Date: 2026-10-20
-   Decision (from a term-by-term comparison of Water.hlsl and
    FallSurface.hlsl; SS 988-990: the river near-black, rippled and
    sparkling, the falls flat grey-brown with solid white foam bands):
    -   Opacity: the water type's fog curve over the water crossed
        (refractionMul) with Fluid Field's water tint, as the river; at
        least 0.3 where aerated. The separate absorption (D105-D121) is gone.
    -   Colour: shallow to deep linearly over the fog's far distance;
        aeration as the river's, 0.06 of the way to its foam colour (D120's
        1.5x re-saturated colour, then 0.25 toward the foam colour, laid a
        grey-brown veil over every falling sheet).
    -   Reflection at the river's weight (fresnel), scaled with opacity in
        the final lerp over what lies behind; the cubemap's mip 0 where
        clear, 1 where aerated. The river's glint (FluidField::SunGlint,
        roughness from aeration), at full strength, also under opacity.
    -   Normals 0.8 of the falls' map (0.3 left the sheets smooth, without
        the river's sparkle).
    -   Foam fuzzier: two blurred pattern samples at different scales, a
        0.35 soft band, the white water times 0.7 so dense bands stay
        patchy rather than solid paint.
    -   The user: "it's getting a lot better".
-   Open: the river's fine sparkle comes from the water type's own normal
    maps (to bind to the falls pass, C++); distance and height fog, SSR and
    the vanilla cubemap, skylighting, terrain and cloud shadows; foam where
    two sheets meet (SS 992, VertexClip d.w from the culling job).
-   References: D120-D122, D125

## D128: Foam where two sheets meet; D129: the water type's ripples on the falls

-   Date: 2026-10-20
-   Decision:
    -   **D128:** foam within 40 units of another shown falls sheet (0.9 at
        most), from VertexClip d.w (the culling job's distance to the
        nearest other sheet, D126): the join of two sheets had none, the one
        behind hidden by the front surface (SS 992).
    -   **D129:** the falls read their water type's three normal maps
        (TESWaterForm::noiseTextureData, Water.hlsl's Normals01-03) with its
        UV scales and amplitudes, blended as the river does (up plus each map
        times its amplitude), on the sheet's flow coordinates at 512 world
        units per repeat, over the falls' own normal map: the river's fine
        sparkle (SS 988-990). Constants grew to 192 bytes; t106-t108.
-   References: D126, D127

## D126: Only the outer shell survives: exact landscape burial, burial in statics from open air, culling before the bake, emitters from the shell; where sheets meet (branch feature/fluid-field-culling-4)

-   Date: 2026-10-20
-   Context: round 2 in game ("Waterfall culling: 5 lit meshes; 13499 of 46266
    vertices tested lie inside another sheet ... 1275.4 ms of work over 5509
    ms"; "Waterfall rock test: 5270 of 80882 vertices inside rock (3577 under
    level ground; 113665 rays)"). SS 982 (under the upper river): the arch
    still curves down into the riverbed and shows. SS 983-985 (under the
    landscape looking up) and SS 986: parts under the landscape and inside
    big meshes remain, and they make flow and spawn spray. The user: "we only
    want the outward shell of the intersection between each waterfall mesh,
    the landscape mesh and other meshes like boulders, etc to remain. Also
    culling maybe should come first? Then we don't waste resources baking
    flow on mesh parts we won't be using." Then SS 992: where two sheets join
    the line has no foam.
-   Causes: D113 trusted the landscape only where its height grid is level,
    so every part under a sloped or cliff landscape survived; D112's static
    test counted a ray starting inside rock as reaching (from inside, Havok
    sees no face), so parts deep in cliffs survived; the bake ran on the whole
    mesh, and every emitter came from the bake's texels.
-   Decision:
    -   **Landscape burial, exact** (BuryStep, LandHeightAt): a ray straight
        down from 4000 units over the vertex, past every hit that is not the
        landscape's own collision (layer 17 ground or 13 terrain, with no
        reference: LEARNINGS, landscape rays hit layer 17, never 13; placed
        meshes on those layers carry a reference, so stepping is used, not a
        filter). Under it by 8 units: buried, cliffs included. Kept per vertex
        (VertexCut::ground).
    -   **Burial in statics, from open air**: sixteen ways (the vertex's
        normal, against it, the six axes, the eight diagonals), each from 500
        units out. A way reaches the vertex only if its start is above the
        landscape (heights cached per 16-unit cell), a ray from the start
        meets no rock more than 24 units short of the vertex, and a ray from
        the vertex out to the start meets nothing (a start inside a static is
        seen from the vertex's side as the face it lies behind). Buried when
        no way reaches. Stops at the first that does.
    -   **In the river by its bed** (CullStep, VertexCut::submerged, into
        a.x): under our pools' water (confirmed over the rock, D40, D78) by
        LEAD_CUT_DEPTH or more, and at most 64 units over the landscape under
        it, so the air under a lip (far over the ground) is never cut. For the
        part of SS 982's arch that is under water and above the bed (the part
        under the bed is buried above).
    -   **VertexInShell**: not buried, not inside another sheet, not the
        lead-in or the river by its bed (the waterline cuts and seam fades
        aside: the plunges belong at the waterline).
    -   **Culling before the bake**: a mesh's bake waits for its cut, its rock
        test and a settled culling job (one ClipStep never reached bakes whole).
        The texels whose interpolated shell flag is under a half are outside
        the bake's domain (covered 0: no rock rays, no strands, no foam or
        spray). The cache key hashes the kept vertices; BAKE_CACHE_VERSION 7.
    -   **Emitters from the shell**: BuildPlunges masks every field with the
        shell as it stands now (spray, plunges, lips, crossing foam), and runs
        again whenever the culling finishes.
    -   **Where sheets meet** (for the coordinator's foam in FallSurface.hlsl):
        VertexClip d.w is the distance from the vertex where it is drawn to
        the nearest shown surface (a triangle with any corner VertexShown) of
        any other lit sheet, up to 64 units; NO_WATER (1e4) where none, and
        from ClipStep until the culling's last stage computes it.
-   Cost: about 3 rays for a vertex that is seen (the landscape ray, two
    for its first way), up to about 50 for a buried one, at 3 ms a frame; the
    meeting distance is a fifth culling stage. Bakes start later (after the
    culling) and test fewer texels.
-   Untested: the plugin builds (Release, CommunityShaders target); not run
    in game; the shaders were not touched.
-   Risks: landscape hidden by a static on purpose (a cave mouth) is still
    the landscape: a sheet under it would be buried. A vertex seen only
    through a gap narrower than the sixteen ways would be buried. If the
    river over SS 982's arch is not in our pools (outside the site's capture)
    the part above the bed stays.
-   References: D40, D78, D97, D105, D112, D113, D124

## D130: Every flow-carried layer whole along the scroll

-   Date: 2026-10-20
-   Decision: the falls' flow coordinates are the bake's minus the scroll
    phase, which wraps at 1. The falls' own texture repeats at 1, so the
    wrap was seamless, but layers sampled at fractional scales of those
    coordinates jumped once a cycle (the user: "looping issues again"): the
    foam pattern's second sample (0.6), the lace (3.2 and 8.64) and the
    water type's ripples (512 over their scale). Each now has a whole
    number of repeats along v (FlowScale rounds it; the lace 3 and 8, the
    pattern 2), u left free.
-   Lesson (LEARNINGS): any texture laid on flow.uvA must have an integer
    scale along v.
-   References: D85, D127, D129

## D131: Ripples and the foam's pattern in world space; D132: each patch on its own clock

-   Date: 2026-10-21
-   Decision:
    -   **D131:** the water type's ripple maps and the foam's bubble pattern
        are laid on the sheet in world space (tri-planar by the sheet's
        normal, `Drifted`), drifting along the flow in two crossfading
        phases as the river's foam, not on the falls' texture coordinates:
        there the ripples stretched along the flow (the glint streaked) and
        the falls' normal map, as the pattern, carried horizontal bands. The
        foam's pattern is now the water type's first normal map (the river's
        Normals01) at 160 units per repeat. The top and foot bands ease out
        (1 - x^2; sqrt ended on a hard straight line, SS 998); the meet foam
        reaches 64 units (the user: larger).
    -   **D132:** each patch drifts on its own clock, a smooth offset over a
        few repeats, as the river's foam (D68): one clock crossfaded the
        whole sheet at once and it pulsed (the user: "looping is back").
        Drift capped at the river's 120 units a second (200 jumped).
-   Found: the zebra bands and solid foam (SS 997, 999, 1000-1002) are the
    bake's foam and wake fields since the culling started cutting texels
    out of the bake (D126): wake high over most sheets, foam in horizontal
    bands. Sent to the culling work (D133+).
-   Queued (the user): low-frequency world noise varying flow speed, foam and
    ripples to break repetition.
-   References: D68, D127, D129, D130

## D135: Burial cut on the GPU; statics by parity; the river's planes for the submerged cut (branch feature/fluid-field-culling-5)

-   Date: 2026-10-21
-   Context: round 3 in game ("Waterfall rock test: 222939 of 372729 vertices
    inside rock (192976 under the landscape; 1111555 rays)"). SS 993-995
    (camera under the ground or inside rock): sheet parts under the
    landscape and inside rock still show; SS 994/995 are SS 983/985's spots.
    The user: the sheets' crossing (D113) "gets us the results we want as a
    whole": treat the landscape and statics the same way, one union with
    the sheets, only its outer shell kept. SS 996 (under the upper river):
    the lead-in arch is still visible and now pokes through the riverbed.
-   Cause (SS 993-995, the main one): **burial was never cut on the GPU.**
    D112 left "buried" out of a.x, trusting the depth test to hide what lies
    in rock. From under the landscape (one-sided, not drawn from below) or
    inside a mesh, nothing hides it. The landscape test probably called most
    of those pieces buried already; now the log says so per mesh (below).
-   Decision:
    -   **VertexCut::rock, in a.x**: under the landscape, its height less
        BURY_LAND (8) under it, signed, so the cut line runs 8 units under
        the ground; inside a static, from BURY_MARGIN (24) inside the nearest
        face, so a sheet dipping into the rock it lies on keeps its edge in
        the rock.
    -   **Statics by parity** (as VolumeCut, D113): along each of the six
        axes, the static faces (layers 1, 2; the falls' own collision passed)
        between the vertex and BURY_REACH (1000) out, cast from both ends
        and merged (a face seen from both sides counted once), odd: inside
        along that axis. Inside with four of six (a far end can sit in a
        cliff). Replaces D126's "a way from open air reaches it". The
        landscape stays the exact heightfield test.
    -   **Evidence in the log**: per mesh, "rock test '<name>': N vertices with
        no landscape under them, M whose ray passed something first", and the
        four lowest vertices the landscape test kept, with the landscape's
        height, their height over it, the ray's steps and the first hit's
        layer and reference.
    -   **The submerged cut also reads the engine's water planes** over the
        sheet (CollectWaterPlanes), where the plane stands at most 256 units
        over the landscape under the vertex (a river over its bed, not a
        cell's plane high up the cliff, D103), still only within 64 units of
        that landscape. SS 996: our pools did not reach that river.
-   Cost: per vertex not under the landscape, two to six axes of two stepped
    casts each (about 4-12 casts, more where statics stack).
-   Untested: builds; not run in game.
-   Risks: the burial is now visible as a cut: a false "inside" holes the
    sheet (four axes must agree, and the cut starts 24 units in). A static
    whose collision is open (a cliff shell with no back) reads as outside
    along its open side.
-   References: D103, D112, D113, D126

## D136: The bake on the whole mesh again; the cull masks only the emitters

-   Date: 2026-10-21
-   Context: after D126's re-bake the bake's foam and wake (rendered raw by
    the coordinator) were high over nearly every sheet, in bands across the
    flow a few texels apart (the user's "zebra" foam, SS 997, 999; whole
    foamed sheets, SS 1000-1002). Before, they were sparse: rock rims and
    wakes.
-   Cause: D126 left culled texels out of the bake's domain (covered 0).
    Every edge of the cull became an edge of the sheet to the strands, the
    rims and the crests, and the culled bands across the flow (cut at
    landscape and statics, often in rows) restarted them every few texels.
-   Decision: the bake runs on the whole mesh, as before D126, and no longer
    waits for the culling (BAKE_CACHE_VERSION 8, the shell hash out of the
    key). The culled parts are hidden by the cut. BuildPlunges reads the
    mesh whole (neighbours, crests, top and foot) and uses the shell only as
    the gate on each texel's own spray, plunge or crossing, so a cull's edge
    is never a rim or a foot. The user's "cull before the bake to save
    work" is set aside: the culled domain changed the flow.
-   Untested: builds; not run in game; the strand harness was not re-run:
    the bake's inputs are again what they were before D126.
-   References: D126

## D137: The foam's pattern is the falls' own normal map again

-   Date: 2026-10-21
-   Decision: D127's pattern (the falls' normal map, two blurred samples on
    the flow coordinates, whole repeats along the scroll) is back. It was
    dropped (D131) for the zebra bands, which were the bake's foam and wake
    fields (fixed in D136), not the pattern. It moves exactly with the
    water, never resets, and needs no water normal maps (not found, D134).
    The lace stays the fallback.
-   References: D127, D131, D133, D134, D136
-   Open (SS 1016-1018, the user): the drop's top foam band (D124) lies in
    big solid patches at every crest with hard straight edges along mesh
    boundaries: the source water is chosen per mesh, so a neighbour without
    one stops the band dead, and NO_WATER interpolates into a hard line.
    To measure per vertex from the water surface and fade where none.

## D138: The drop's top foam per vertex; the water type's normal maps loaded by path

-   Date: 2026-10-21
-   Decision:
    -   Each falls vertex takes as its source the nearest water plane above
        it (at most 240 units over it, within 96 of the plane's bound), from
        every plane near the mesh: chosen per mesh (the planes over its top),
        one mesh of a crest found its source and the next did not, and the
        top foam stopped dead along their seam (SS 1016-1018).
    -   The vertex's "none" values (1e4) are held to 512 before
        interpolation, which drew the band's end as a hard straight line
        inside a triangle; the top band is at most 0.8 so the bubble pattern
        breaks it (every crest was a solid patch).
    -   The water type's normal maps: TESWaterForm::noiseTextureData is empty
        outside the water shader's use (logged "no no no"), so each is
        loaded by its path with BSShaderManager::GetTexture (as the
        flowmaps) and kept alive. The river's ripples (D129, D131) and the
        Noise1 foam pattern had never been drawn.
-   References: D124, D129, D134

## D139: The foot foam band per vertex, and softer

-   Date: 2026-10-21
-   Decision: the height above the water a falls vertex falls into (for
    its foot foam, VertexClip a.z) is the highest water plane under that
    vertex, from every plane near the mesh, not only the mesh's own. Per
    mesh, the band stopped on a hard straight line where the next mesh up
    found no water, and its 100-unit reach at full strength painted short
    curtains solid white (SS 1031-1032; a band diagnostic showed the foot
    band alone). The band is at most 0.8, and Fade.y is held to 512 before
    interpolation, as D138 did for the top band. The cut at the mesh's foot
    keeps the mesh's own planes (D70).
-   References: D124, D138

## D140: Burial decided by the nearest face; the landscape checked against the engine's height; a cleanup of lone pieces and pinholes (branch feature/fluid-field-culling-6)

-   Date: 2026-10-21
-   Context: after D135-D136 the user found holes in plain, visible sheets
    (SS 1019: the last fall between Whiterun and Riverwood; SS 1025: at a
    fall's foot; SS 1027: a large ragged hole mid-sheet, with scattered
    small ones round it; SS 1028) and kept pieces floating where all round
    them was cut (SS 1020-1024, 1026, 1029; SS 1021: a lone ragged piece in
    the rock). SS 1030: the undersides are clean.
-   Causes (from the code; the new log is to confirm them in game):
    1.  **D135's static parity counted every face out to 1000 units.** In a
        gorge the far end of an axis sits inside the cliff opposite; Havok
        misses that cliff's far face from inside it, so the axis counts one
        face: odd, "inside". Three or four such axes (both walls, the bed's
        rocks) were enough for a hole in a sunlit sheet (likely SS 1027).
    2.  **The landscape ray took any unreferenced hit on the ground layers
        as the landscape.** If some mesh's collision sits there with no
        reference, a sheet under it read as under the ground (possible, not
        confirmed; now guarded and counted).
    3.  **Every cut is decided per vertex**, so its mistakes are scattered
        single vertices or small groups: pinholes in a sheet, and scraps
        kept where everything round them was cut.
-   Decision:
    -   **Statics by the nearest face along each axis.** If the vertex's
        own ray meets a face first, the vertex is outside along that axis
        (unless Havok reports it as the back of a face). If the far end's
        ray meets a face nearer the vertex than anything the vertex's ray
        met (a face Havok hides from inside a shape), the vertex is behind
        it: inside. Still four of six axes; the cut still starts 24 units
        inside the nearest such face. Far-away faces no longer vote.
    -   **The landscape hit must agree with TES::GetLandHeight** (the same
        heightfield) within 16 units; any other ground-layer hit is passed
        by and counted ("ground hits passed by").
    -   **CleanupSheet**, a culling stage after the inside cut, one sheet a
        step, on the sheet's graph with vertices welded by position:
        -   Holes first: a cut piece with no open edge and only kept
            neighbours, cut only by rock or another sheet (never the
            water, the lead-in, the river or the pools), and at most 32
            vertices or 0.2% of the sheet's kept ones, is restored (its
            rock and inside cuts undone).
        -   Then lone pieces: per part of the mesh as authored, the
            largest kept piece is its body. Another kept piece that borders
            a cut vertex and has at most 64 vertices (or 1% of the part's
            kept ones) is cut (VertexCut::island, in a.x). So is the body
            itself if it is a scrap: at most 256 vertices and under 3% of
            its part. Whole pieces switch, so no cut line moves along the
            kept sheet and no sawtooth appears.
        -   VertexInShell excludes the cut pieces, so no spray comes from them.
    -   **The log** says what cut each sheet: under the landscape, in
        statics, inside another sheet, lead-in, in the river, the water
        below, the pools, lone pieces. It also gives its two largest pieces
        cut by rock or a sheet alone (size, centre, the landscape/statics/
        sheet counts, the mean of the six axes that voted inside, vertices
        on the open edge), and the cleanup's counts. Per mesh, the rock
        test also logs ground hits passed by.
-   Cost: one welded graph and three floods per sheet a step (a few ms to
    tens of ms on the largest sheets, one frame each); the static test's
    casts are the same.
-   Untested: builds; not run in game. Whether Havok reports the back of a
    face, and with which normal, was not observed; if it never does, the
    "met by the vertex first" case always reads outside, which errs toward
    shown. TES::GetLandHeight is called inside the Havok read lock (it is
    expected to read the land's data, not cast).
-   Risks: SS 1025's holes at the foot may come from the pools' cut, which
    the shader makes per frame and the cleanup does not see. A small
    visible tail of a mostly buried sheet (under 256 welded vertices, 3%
    of its part) is cut as a scrap.
-   References: D113, D135, D136

## D141: Foam grows with the drop; a glassy lip that blends into the river

-   Date: 2026-10-21
-   Decision (the user's references: Victoria and Niagara for vertical
    falls, rapids from above and the side for inclines):
    -   No foam over the lip: D124's top band is gone. Real water goes over
        the edge glassy and breaks up as it falls.
    -   Foam grows with the drop below the water a fall pours out of
        (VertexClip d.z): from nothing 24 units down to 0.9 at 260, on the
        steep parts only (the flow's vertical share 0.4 to 0.8); a sheet
        with no source (starting mid-fall) by its run time. Inclines foam by
        the bake's rock strikes and wakes.
    -   The lip blends into the river: the sheet fades in over its first 24
        units below the source's surface, and no cut-line foam there (the
        lead-in's cut lies under the river).
    -   No crossing foam where a sheet rises out of the water it pours out
        of (a level within 96 units of the mesh's top): the river foamed
        along every lip. Plunges into the water below keep theirs.
-   References: D98, D121, D124, D138

## D142: Foam from the middle of each drop down; dense foam solid

-   Date: 2026-10-21
-   Decision: D141's foam ramp in world units (24 to 260 below the source)
    whitened tall falls wholly and, with the foot band, short curtains too
    (SS 1034-1036). The user: keep the water look and add lots of foam at
    the mid-to-bottom. The foam now follows the share of the whole drop
    fallen, from the water a fall pours out of (d.z) to the water it falls
    into (a.z): from nothing at 45% to full at 85%, on steep parts; the foot
    band only where the drop is not known. Dense foam fills in: the bubble
    pattern's threshold shrinks with the foam's density, so its dark dashes
    no longer band dense foam (SS 1036).
-   Open: SS 1035, top left: the culling cut away visible falls meshes.
-   References: D139, D141

## D143: A quiet lip; sheets fed by a drop foam

-   Date: 2026-10-21
-   Decision (SS 1037, 1038):
    -   Over the lip (24 units of fade-in, then 48 more) no seam, sheet-meet
        or contact foam either: sheets meet each other and the rock under
        the river along every crest, and drew thin white scraps there.
    -   (Withdrawn, SS 1039: runs far from any plane went white.) A sheet with no calm water over it (no source) is fed by the drop
        above it: 0.85 white from its top, whatever its slope. The inclines
        right under a violent drop churn in the references; they read as
        clear water (the fall foam needed both a source and steepness).
-   References: D141, D142

## D144: Culling biased toward keeping; orphans cut by triangles (branch feature/fluid-field-culling-7)

-   Date: 2026-10-21
-   Context: D140 in game ("cleanup: 205 kept pieces (331 vertices) cut off
    their sheet's body and cut, 395 small enclosed holes (780 vertices)
    restored"). SS 1035: visible falls eaten by the culling (top left, a low
    cascade), and D140's list still stands (holes SS 1019, 1025, 1027, 1028;
    floating bits SS 1020-1024, 1026, 1029). The user: "Culling will never
    be perfect but it should seek to leave only the 'outward' mesh, it's
    better for it to be a little tolerant than for it to eat away at mesh
    parts we actually want. Also if it doesn't already, detecting floating
    orphaned quads/tris and culling them should be a must."
-   Decision, every cut on strong evidence only:
    -   **Landscape**: buried from 24 units under it (was 8): low runs lying
        on the ground dip into it.
    -   **Statics**: five axes of six (was four), or four with the cut
        starting twice as deep (48 units in).
    -   **Inside another sheet**: four rays of six must leave it through its
        outer face (was three), none entering.
    -   **In the river by its bed**: an engine water plane counts only up to
        96 units over the sheet's own top (its source). A cascade's lower
        sheets lie under the upper river's level, inside its plane's bound,
        near their bed, and were cut (likely SS 1035).
    -   **Holes restored** up to 96 vertices or 0.5% of the sheet's kept
        ones (was 32, 0.2%): a hole in a kept face is worse than hidden mesh.
    -   **Orphans by triangles** (replaces D140's kept-vertex pieces). A
        triangle is drawn if any corner is kept, so a lone kept vertex draws
        a fan of slivers. Drawn triangles join across an edge with a kept
        end (two sharing only cut corners show apart), vertices welded by
        position. Per part of the mesh as authored, the largest piece is
        its body. Another piece is an orphan when the cuts made it (it has
        a cut corner) and it has at most 128 triangles or 1% of its part's
        drawn ones. The body too is cut when it is a scrap: at most 512
        triangles and under 3% of its part's triangles. An orphan's kept
        vertices are cut (VertexCut::island), unless a vertex also draws in
        a kept piece (a bow tie's middle). Whole pieces switch, so no cut
        line moves.
    -   **Log per mesh**: "cut '<name>' (V vertices): ... under the
        landscape, ... in statics, ... inside another sheet, ... lead-in, ...
        in the river, ... the water below, ... the pools, ... orphans; N
        orphan pieces (T triangles) cut, H holes (W vertices) restored", and
        the totals in the culling's summary.
-   Untested: builds; not run in game. The thresholds are guesses.
-   Risks: more hidden mesh stays (by intent): a sheet a little inside
    rock, the ground or another sheet's curl now shows where it is seen
    from inside them.
-   References: D113, D135, D140

## D145: The crest foam's real sources

-   Date: 2026-10-21
-   Decision: a diagnostic colouring each foam source (red the bake's foam
    and wake, green the drop's bands, blue the lines) showed the crests
    foamed by two sources D138-D143 never touched:
    -   The bake's foam and wake (flow.foam, flow.wake) mark every sheet's
        rims and crests as rock: dropped from the falls' foam until the
        bake's own aeration replaces them (the solver rework).
    -   The seam, meet, cut and contact lines across the crests of lower
        drops (sheets with no calm water over them, so D143's quiet lip did
        not apply): quiet too for the first 0.3-0.6 s of a sheet's flow
        (its run time).
-   Lesson: when a look keeps coming back, colour every input separately
    in the user's view before changing any.
-   References: D138, D141, D143

## D146: No hand-made foam bands on the falls; line foam only where water falls

-   Date: 2026-10-21
-   Decision (source diagnostic in the user's view, SS 1040):
    -   The drop's bands are off: the pool band, the foot band and the
        share-of-the-drop foam (D124, D139, D142). On a cascade the top
        river is every lower drop's "source", so the lower crests read as
        mid-drop and foamed; the user: the green (these bands) gone. Foam
        where the falls enter the water below comes from the river's own
        simulation; foam on the falls is to come from the bake's aeration.
    -   Seam, meet and cut lines foam only where the water already falls
        (steep): a run's end meeting the next drop's sheet foamed across
        every crest.
    -   Left: thin contact foam where sheets touch rock at their sides.
-   References: D124, D139, D142, D143, D145

## D147: The falls nearer the river's look

-   Date: 2026-10-21
-   Decision (SS 1046: the river a dark body with dense small sparkle, the
    falls a clear film over their rock with a slow wobble and hard white
    rims):
    -   The sheet's colour and opacity read at least 120 units of water
        (BODY_DEPTH), so it reads as a body like the river beside it.
    -   Its reflection is 0.35 of the river's fresnel: seen grazing (as from
        above) the full fresnel made every steep sheet a blue-grey mirror.
    -   A fine detail ripple layer (the water type's third map, 160 units
        per repeat, drifting with the flow, 0.8) and the river's tight glint
        (no roughness from aeration).
    -   The rock contact foam softer and wider (0.45 over 32 units, was 0.9
        over 20): white lines where sheets meet rock.
-   Open: the falls a little browner than the river's olive; sparkle on
    steep sheets is rare (the sun seldom lines up with them).
-   References: D127, D129, D131

## D148: The falls' water depth measured as the river measures it

-   Date: 2026-10-21
-   Decision: the water the view ray crosses is the scene's depth behind
    the sheet along the ray at the refracted pixel (Water.hlsl's
    distanceMul), up to the water's fog distance; where the sheet has left
    its rock (baked depth behind past 24-48 units: a curtain), its own
    thickness. D147's 120-unit floor painted every sheet deep and dark
    ("petroleum", SS 1050) where the river shows its bed when shallow and
    darkens with depth (SS 1049). D106 had dropped the screen depth as
    noise; with only the front surface drawn (D125) and the opaque pass's
    depth behind (D105) it reads cleanly.
-   Open: a bright white blob (also SS 999) on some sheets.
-   References: D105, D106, D125, D127, D147

## D149: Falling water is murkier than its depth

-   Date: 2026-10-21
-   Decision: the falls' water path is read 4 times its depth
    (FALLS_MURK). An opacity diagnostic (bands of refractionMul, user's
    view) showed the thin water over a sheet's rock under 20% opaque almost
    everywhere: the river is murky because it is deep, the falls are a
    shallow film, so under the same fog curve they read nearly clear
    (SS 1052). Falling water is turbid (bubbles, stirred sediment), so it
    reads deeper than it is. Water Depth still scales it.
-   References: D148

## D150: The bed seen through the falls goes milky and dark past the first few units

-   Date: 2026-10-21
-   Decision: what lies behind the sheet is lifted toward the water's foam
    colour (0.45, the river's aeration of its refraction) and dimmed toward
    the water's colour (0.75), by the water path (FALLS_MURK in it): clear
    for 8 units, full over 48 more. Compared with Water.hlsl: the river
    fogs and aerates its refraction before laying its colour over it;
    the falls laid their colour over the raw bed, which stayed sharp under
    every sheet (the user: the river is clear for its first 30 cm, then
    dark, milky and murky). The weather fog on the refraction is still
    not ported (needs the fog colours from C++).
-   References: D148, D149

## D151: The sky's fog on the falls, as on the river; less milk

-   Date: 2026-10-21
-   Decision:
    -   The falls take the sky's distance fog as Water.hlsl does
        (Sky::fogNear, fogFar, fogPower, fogClamp and its kFogNear / kFogFar
        colours, passed per frame): what lies behind fogged at its own
        distance before the water's colour is laid over it (the river's
        refraction fog), and the sheet's own colour fogged at its distance.
        Named SkyFog\* (the water type's FogFar is the depth fog).
    -   The bed's milky lift 0.12 (D150's 0.45): the foam colour is the
        scene's light, and from above at noon the bed read as flat beige
        (SS 1057).
-   References: D150

## D152: The sky's fog only as the river takes it

-   Date: 2026-10-21
-   Decision (SS 1058: the falls slate blue beside the olive river; a fog
    diagnostic put 20-50% fog on them, an A/B with fog off matched the
    river's colour):
    -   No vanilla fog where Exponential Height Fog replaces it (its
        enabled and disableVanillaFog settings), as Water.hlsl does: the
        river was unfogged and the falls took the sky's raw fog colour.
    -   What lies behind is not fogged again: the scene behind was drawn
        with its fog already.
-   Open: the river's dense sun sparkle; the falls read a little flatter.
-   References: D151

## D154: Measured: the falls' depth and water parameters match the river's

-   Date: 2026-10-21
-   Decision: matching diagnostics in Water.hlsl (deployed copy only) and
    FallSurface.hlsl, in the user's view: water depth along the view ray
    150-300 units on both, fog far 100-300 on both, fog amount 0.5-1 on
    both. With the depth at or past fog far the shared formulas make both
    fully opaque and deep-coloured. D149-D153's fudges (murk x4 then x12,
    deep colour at 160 units, the bed's darkening, the reflection at 0.35)
    are removed: the falls run the river's formulas as they stand.
-   Found, still different: the reflection's source. The river reflects
    screen-space reflections (its dark banks) and the vanilla cubemap at
    distance; the falls only the sky cubemap, which at full fresnel greys
    them blue (the user's "petroleum", D147's 0.35 was hiding this).
-   Lesson (LEARNINGS, D145): measure each input on both shaders before
    tuning any.
-   References: D127, D147-D153

## D155: Screen-space reflections on the falls

-   Date: 2026-10-21
-   Decision: the falls march their reflected ray through the scene's
    depth (24 growing steps from 8 units, 48 units thick) and reflect the
    scene's colour where it meets something, over the cubemap, weighted as
    the river's SSR (more where the reflection points along the view),
    faded at the screen's edge and with the march's length. The engine's
    SSR is made for a level water plane, and the falls lean every way; the
    river's darkness comes partly from its SSR reflecting the banks (D154).
-   Open: from above the reflected rays go up to the sky and miss, so the
    cubemap still carries those views.
-   References: D154

## D157-D158: A stricter submerged cut; the view ray's depth without a curtain rule

-   Date: 2026-10-21
-   Decision:
    -   **D157:** the submerged cut only 48 units under the river's water
        and within 16 of its bed (was 16 and 64): once the landscape heights
        came in, the second culling pass cut 70,574 vertices of the sheets
        running through a cascade's pools (8,770 in the first).
    -   **D158:** the falls' water path is always the view ray's depth to
        the scene behind, as the river's (D148 kept D120's curtain rule: past
        48 units of gap behind, a sheet read 8 units of water). The depth
        rays put most sheets 47-218 units off their rock, so the falls went
        clear as soon as the depth job finished: the user saw the murk
        right after a restore, before the job, then lose it.
    -   **D158, also:** no fade-in under the lip (D141): the front-surface
        pre-pass (PSDepth, D125) still claimed those pixels and hid what lay
        behind, so the fade punched holes along the crests showing the rock
        behind, only with See-Through Falls on (SS 1085 lit, 1086
        see-through). The lead-in cut handles the part under the river.
-   References: D120, D126, D148, D154

## D165: The outward shell guarded by construction (branch feature/fluid-field-culling-8)

-   Date: 2026-10-21
-   Context: the user: "culling still needs some work to avoid eating at
    outward meshes". Instead of tuning thresholds again, a vertex open to
    the outside is protected outright. Then a correction (the user): rays
    cast down, or from inside a solid, can read open falsely (Havok misses
    the landscape from below and a static from inside).
-   Decision:
    -   **OpenToOutside**: five rays out of the vertex's outer side, from
        where it is drawn (rest plus the composed moves), 2 units off it:
        along the consistent outer normal, and tilted 35 degrees up, down
        and to either side along the sheet. A ray is open when:
        -   it meets no rock, static or landscape out to 300 units, and the
            ray cast back from its far end meets nothing either (a solid the
            vertex sits inside shows its face from outside);
        -   it meets no shown surface of another sheet within 64 units
            (beyond that it counts as open water space).
            The vertex is open if any ray is.
    -   **Safety (the correction)**:
        -   never open when the vertex is under the landscape (the exact
            heightfield test with BURY_LAND's margin, checked first);
        -   only the upper hemisphere: a ray leaning down is turned level,
            so an outer normal facing down gets level rays;
        -   the far-end confirmation above.
    -   **OutwardGuard**, a culling stage after the inside cut, 16 vertices a
        step under the world's read lock: only vertices cut by the burial,
        the inside or the river cut are asked. The Havok answers are kept per
        vertex (the rock does not move); the other sheets' part is asked
        afresh each pass.
    -   **The guard in a.x (ComposeCut)**: an outward vertex is never cut by
        the burial (landscape, statics), the inside or the river cuts. Only
        the water below, the lead-in, the pools (the shaders') and the
        orphan cleanup remove it. VertexShown and VertexInShell take it as
        not buried.
    -   **Orphans**: a piece with more than 30% of its kept vertices open to
        the outside is never an orphan (asked in the cleanup, under the
        lock).
    -   **Log**:
        -   per mesh: "outward guard '<name>': N cut vertices tested, M open
            to the outside; kept from the landscape …, statics …, inside
            another sheet …, the river …";
        -   totals, Havok rays, orphans kept for being outward;
        -   what the safety refused: vertices under the landscape, rays
            turned level, rays closed from their far end.
-   Cost: up to ten casts per cut vertex once per load (most stop at the
    first open way), then CPU-only checks per pass.
-   Untested: builds; not run in game.
-   Risks: inside a large C-shaped sheet (its far side more than 64 units
    away) a vertex reads open, so D113's inside cut can be undone there by
    design (open water space). A vertex inside a static whose ray's far end
    also lies inside the same static (300 units deep) reads open.
-   References: D105, D113, D135, D143, D157

## D166: The outward guard leaves the sheet-inside-sheet cut alone

-   Date: 2026-10-21
-   Decision: D165's guard no longer overrides the volume cut (D113). Inside
    a big C-shaped sheet a vertex more than 64 units from the far side read
    as open water, so the guard would have restored the inner layers the
    user wanted gone; it still overrides the burial (landscape, statics)
    and the river cuts.
-   References: D113, D165

## D159: The see-through falls shaded by the river's own water shader (branch feature/fluid-field-falls-water)

-   Date: 2026-10-21
-   Context: D127-D158 matched FallSurface.hlsl's port of Water.hlsl to the
    river term by term and still found differences (the reflection's
    source, D154; the river's sparkle, D152). The port can only follow the
    river; the river's own shader is the river.
-   Decision: the falls are drawn by Water.hlsl itself, its
    `FLUID_FIELD_FALLS` variant, with the river's state:
    -   **Shader:** a water descriptor bit (15, `FluidFieldFalls`) emits
        `FLUID_FIELD_FALLS`; the shader cache compiles the variant like any
        water permutation (in the background on first asking).
    -   **The river's state:** State::Draw hands every draw to Fluid Field,
        which captures the first draw of a frame with the water shader's
        refracting surface (technique 0, Refractions): the contents of its
        constant buffers (PS b0-b4: PerTechnique, PerMaterial, PerGeometry,
        Light Limit Fix's, the permutation; VS b0-b1) copied into our own
        buffers (the engine's are shared per size and rewritten by the next
        draw), and references to its 128 pixel shader views and 16 samplers
        (the water type's maps, the cubemaps, shadow maps, Skylighting,
        Light Limit Fix's lights). They are released before the next
        frame's water and after the falls: no engine texture is held across
        frames (LEARNINGS).
    -   **Vertex shader:** reads the sheets as FallSurface.hlsl does (t125,
        FallGround's moves and cut, b7's world transform) and hands the
        river's pixel shader what its own vertex shader hands it:
        camera-relative positions, the normal maps' scroll, vanilla's fog;
        the sheet's normal, flow coordinates and FallGround's per-vertex
        data besides.
    -   **Pixel shader:** the river's, with: its normal maps on the sheet's
        flow coordinates (a whole number of repeats along the scroll, D130)
        and the normal turned from the river's frame into the sheet's (the
        screen-derivative frame of FallSurface's PerturbNormal), the piece's
        own normal map (0.8) laid over it; the sheet's normal where the
        river uses up (the reflection's tilt, Skylighting) and the plane
        through the pixel where it uses ReflectPlane (the depth behind);
        refraction offset across the sheet, projected by the camera; the
        falls' own screen-space march (D155) for SSR (the engine's is made
        for a level plane, on water-mask pixels only); the falls' aeration,
        foam and matte in place of the river simulation's (read at world xy,
        meaningless on a sheet); the water tint by the view ray's path
        (D148); no flowmap, Water Effects parallax or rain ripples;
        FallSurface's discards (front surface, inner side) and fades;
        premultiplied output for FallRefraction's blend. The river's own
        output is unchanged (the define off).
    -   **Shared:** the falls' visibility and white water (FallShading.hlsli)
        is one code for both shaders. D141's lip fade-in is not in it
        (D158: PSDepth still claimed those pixels, holes along the
        crests); what fades a pixel there must also be discarded in PSDepth.
    -   **Draw:** FallRefraction keeps its front-surface pre-pass (D125)
        with FallSurface.hlsl's PSDepth, then binds the captured state, ours
        over the slots the variant reads (t1 the pre-water scene, t7 the
        opaque depth, as FallSurface reads them; t105 the front surface;
        t120-t125 FallGround and the sheet; b7, b12, b13), and draws with
        the variant. Everything it touches is restored, all 128 views
        included. FallSurface's diffuse and normal maps move to t121-t122,
        off Light Limit Fix's local shadows (t102-t103).
    -   **Fallback:** FallSurface.hlsl draws the falls whenever no river
        draw was captured this frame (no refracting water on screen), the
        variant is still compiling, or anything is missing; the log says
        which shader draws them each time that changes ("See-through falls
        shaded by ...").
    -   **Setting:** "Falls Use River Shader" under See-Through Falls, on
        by default; Water Depth and Refraction only apply when it is off.
        FluidField.ini 0-37-0.
-   Untested: none of it has run in game. The plugin builds (Release);
    Water.hlsl compiles with fxc (vs_5_0, ps_5_0) with the river's defines
    and the feature defines, with and without FLUID_FIELD_FALLS, FLOWMAP,
    BLEND_NORMALS, NORMAL_TEXCOORD, VC, WADING and UNIFIED_WATER.
-   References: D93, D100, D103, D123, D125, D126, D127, D130, D148, D154,
    D155

## D160: The river's state from one draw a frame; what the falls variant leaves out

-   Date: 2026-10-21
-   Decision (where D159 departs from its plan):
    -   **One capture a frame, not one per water type.** A draw's
        BSWaterShaderMaterial is not tied to the TESWaterForm a falls piece
        pours out of (FindFallWaterType) without another hook on
        BSWaterShader::SetupGeometry (already hooked by Unified Water and
        Light Limit Fix) and a match from material to form. The first
        refracting water draw of a frame serves every piece: on screen
        beside falls it is usually their river. Where the frame's first
        water is another water body, the falls take its look.
    -   **Our scene and depth over the river's.** t1 (RefractionTex) and t7
        (DepthTex) are FallRefraction's pre-water copy (D123) and the opaque
        pass's depth (D105), what FallSurface reads, not the river's: known
        to hold the scene without the falls at their draw.
    -   **The normal maps' sheet coordinates per pixel**, from the flow
        FallGround::Flow reads there anyway (planned per vertex).
    -   **The tangent frame from screen derivatives** in the pixel shader,
        not per vertex (the sheets carry no tangents).
    -   **Flowmap and per-vertex variants dropped** in C++ and in the shader
        (FLOWMAP, BLEND_NORMALS, NORMAL_TEXCOORD, VC, WADING,
        VERTEX_ALPHA_DEPTH): their coordinates are the river cell's.
    -   **No aeration opacity floor** (FallSurface's 0.3): the river's
        composite divides its colour by its opacity, so changing the opacity
        afterwards breaks it. Aeration only lifts the colour (D127's 0.06).
-   Risks: TAA has no motion vectors for the falls (as D93) and
    ISWaterBlend does not apply to them. The river's PerGeometry (b2) is
    its own: ProjData is the camera's; TextureProj and ReflectPlane are not
    read by the variant. D131 found the water type's maps on the falls'
    texture coordinates stretched along the flow; they are on them again
    here, at 512 units per repeat.
-   References: D98, D105, D123, D131, D159

## Withdrawn: D165-D166, the outward guard

-   Date: 2026-10-21
-   Reverted in game: with the guard, the sheet-inside-sheet cut no longer
    worked and most of the undersides came back (SS 1089, 1090: "a major
    step back", the user). The culling is back to D157 (D144 plus the
    stricter submerged cut). Branch feature/fluid-field-culling-8 keeps it.

## D171: The falls bind their water type's own normal maps; the river's state per water type

-   Date: 2026-10-21
-   Problem: the river-shader falls (D159) flipped between their look and a
    clear, depthless sheet showing the bed under it, every fall in view at
    once, with the smallest camera moves (SS 1093-1136). FallSurface.hlsl
    never did.
-   Cause, measured: in the broken frames Normals02 (t5) read nothing (black
    in a band of the screen, SS 1136). The falls took t4-t6 from the captured
    river draw; a flowmap piece of the river (descriptor 0x21a) binds only the
    first normal map, and which piece was captured changed with the draw
    order, so with the camera. The falls variant has no flowmap code and reads
    all three: with two zero, the ripple normal went flat and tilted, the
    reflection ray went into the cubemaps' ground faces, and the fresnel and
    depth look went with it.
-   Decision:
    -   **The falls bind their water type's three normal maps** (loaded by
        MakeFallWaterLook, D129) over t4-t6 for every piece.
    -   **One capture per water type** (a hook on
        BSWaterShader::SetupGeometry finds the TESWaterObject whose shape is
        drawn), each fall drawn with its own water's state and material.
    -   **The same river piece frame after frame** while it is drawn: the
        piece first drawn changes with the camera, and its material with it.
-   Not the cause, measured and reverted: the shore fade and the refracted
    depth (depth and opacity diagnostics steady across a flip), the reflection
    march, the cubemaps on their own (the reflection was only the messenger),
    which water type or render pass was captured (logged, unchanged at a flip).
-   References: D129, D159, D160, LEARNINGS (one capture, every slot)

## D172: The falls' setup off the render thread; the dense copies cut under the landscape

-   Date: 2026-10-21
-   Problem, measured: with Fluid Field on the game felt sluggish. PresentMon over
    alternating on/off runs: frame time pinned at 16.6 ms either way, but with it on
    about 50 frames in 19 s over 20 ms (p99 55-69 ms), all CPU-bound; off, none.
    A timer on every Fluid Field update found the falls' set-up after a load:
    -   BuryStep: 1 ray down plus up to 12 along the axes per vertex, 1.5 million
        Havok rays, 3.7 ms on every frame for about four minutes. The statics'
        rays cut nothing over every falls mesh logged (all of the rock cut was
        under the landscape).
    -   CullStep: 3.3 ms on every frame for its run; a sheet built in one step
        (40-58 ms).
    -   One-frame passes: the seams (ComputeFades, 1.8 s), the bakes' linking
        (1.2 s), the dense copies (0.4 s), the plunges (35-150 ms, three to five
        times), the pools (30-90 ms).
    -   (The profiler's "RefractiveFalls 45%" was mostly another thing: its depth
        copy waits for the work before it, which its GPU timer bills to it.
        PresentMon: the see-through falls cost 0.7 ms of GPU over vanilla's.)
-   Decision:
    -   **The dense copies cut under the landscape** (Densify, DENSE_TRIM_UNDER 24
        under it, the crossing found against the heights by bisection). Every
        vertex kept, the new ones at the end: the bake reads the untrimmed
        triangles over the same vertices (BakeIndices, D136), and its own rock,
        shell and water tests are unchanged (every bake read from the cache).
    -   **The rock test without rays**: the landscape's height from Fluid Field's
        cache (SetLandscape) for the leftovers and the culling's ground; the
        statics' six-axis test dropped.
    -   **Workers**, one at a time, while Update only polls (Busy): the culling
        (stages 0-5), the seams, the bakes' linking, the dense copies (attached on
        the render thread), the plunges and the pools (their results swapped in
        on the render thread: the spray and the maps read them every frame). The
        ground captures wait behind them (their end grounds the meshes again), a
        step that starts a worker ends the update, and the engine's water planes
        are a copy taken while no worker runs.
    -   **Havok stays on the render thread**: a worker's hkpWorld::CastRay crashed
        (its per-thread memory is not set up off the engine's threads).
-   Results (the same view and save): the worst frame of a load 1.8 s to about
    25 ms; after the falls load, idle (was 7 ms a frame for minutes); about 40% of
    the falls' triangles gone (under the ground).
-   PresentMon, the same on/off protocol as before: built and settled, no frame
    over 20 ms and p99 16.9 ms, as with Fluid Field off (was 47-56 frames over
    20 ms in 19 s, p99 55-69 ms); while rebuilding after switching it on, 16 (one
    of 109 ms). Steady cost: about 2.3 ms of GPU busy (11.7 vs 9.4 ms).
-   The user's direction: normal gameplay is what is optimised; the build may
    take its time, and can move offline, to a cache or behind the loading screen.
-   Left: the depth rays (Havok, about 20 ms a mesh to set up); the sheet-inside-
    sheet cut as geometry (visibility from many directions), then the trimmed
    meshes baked and cached (the plan agreed: trim the vanilla geometry, generate
    ours from it).
-   References: D112, D113, D126, D131, D135, D136, D138, D171

## D173: The dense copies cut to the outer shell (seen from outside), with a margin

-   Date: 2026-10-21
-   Decision: once a batch's dense copies are made (D172, on the worker), their
    triangles that cannot be seen from outside are cut away
    (FallSites::TrimHidden, FallVisibility). A vertex is seen when a ray from it
    along one of 96 directions over the sphere (a Fibonacci lattice, upward
    first) leaves the area (REACH 2500 round the sheets) meeting no triangle of
    the batch's sheets, the statics round them (their render triangles,
    GatherOccluders, OCCLUDER_REACH 1024) or the copies made before, and
    staying over the landscape (the height cache, LAND_MARGIN 8). CPU rays
    through a BVH on several threads: 0.3-0.9 s a batch, 1.3-3.7 million rays.
    -   **A margin, SHELL_MARGIN 64 along the sheet** from the nearest vertex
        seen (cut at whole triangles with none, the meeting edges of two
        sheets stopped short of each other: the ground showed between them,
        SS 1147-1149).
    -   **Cut along the margin's edge**, a triangle crossing it split where it
        crosses (as the landscape cut), not at whole triangles (their
        staircase drew the meeting foam as white wedges, SS 1150).
    -   The bake still reads the untrimmed triangles (BakeIndices, D136).
    -   The GPU's copy of vertexClips is uploaded again when a worker finishes
        (its bumps of the version mid-way could leave it half-written).
-   Results: the per-vertex inside cut (D113) found 16 vertices where it found
    3132; the falls draw 318,810 triangles a pass (737,897 this morning, 389,282
    after the landscape cut).
-   Left (the user, SS 1153-1155: "the logic is there"): the meeting and cut
    foam is per vertex (its edges take the triangles' shape at any margin): to
    be per pixel, from the depth just behind the sheet (as ContactWhite).
-   References: D113, D136, D172

## D174: Contact foam per pixel, on fixed axes; the falls' detail normal on the sheet's own axes

-   Date: 2026-10-21
-   Context: the meeting and cut foam was decided per vertex (VertexClip): its
    edges took the triangles' shape at any margin (white wedges, SS 1150-1155).
    The user: no smeary, distorted look from the bake/flow; foam at every
    contact, driven by the flow; the same look as the shallow water's foam.
-   Decision:
    -   **Contact strength per pixel** (FallShading::ContactStrength): the
        largest of the next sheet just behind (a second falls depth layer,
        PSBehindDepth: the falls' surface nearest behind their front, t112),
        rock, ground or any mesh behind (the scene's depth) and the water below
        (Fade.y), each over its reach (MEET 96, CONTACT 64, WATER 64),
        squared (a narrow solid core, a wide tail broken into the pattern),
        gated by the lip (D145). It stops under the water's surface
        (WATER_FOAM_SINK 8).
    -   **The river's foam look**: its pattern (the water type's Normals01 read
        as bubbles, FOAM_TILE 160, two phases restarting every 0.6 s, the
        river's threshold) and colour. On three fixed world planes blended by
        the sheet's normal (pow 4): laid on the sheet's own axes, dotted with
        world positions ~1e5 units out, a hundredth of a degree between two
        triangles' normals moved the pattern tens of units (a lattice of the
        triangles, SS 1172, 1182). Drift down steep water at 120 units a
        second, along the bake's flow on level water at 40 at most: dragged by
        the bake's velocity per pixel, its changes from texel to texel opened a
        larger lattice on the bake's grid (SS 1191-1193).
    -   **The foam's texture is the river's own**, as captured (t113): the
        form's maps loaded by path (look.noise, bound over t4-t6 since D171)
        are the engine's 16x16 stand-ins, flat. t4-t6 keep those: the real maps
        there turned on the falls' water-type ripples, on their stretched flow
        coordinates (shards in the reflection, SS 1196). Proper ripples on the
        falls are plans/next-visuals.md step 3.
    -   **The detail normal on the sheet's own axes** (FallShading::SheetNormal:
        down the sheet and across it, from its normal alone), not
        PerturbNormal's frame from the screen derivatives of the flow
        coordinates: constant over each triangle, it turned the falls' normal
        map at every edge (hard-edged facets in the reflection, SS 1203, found
        by removing one layer per screen quarter). Along the bake's flow, its
        direction changing from texel to texel did the same.
    -   **Height over the water per vertex** (FallSites): a vertex under every
        plane takes the nearest above (negative), not none (held at 512, mixed
        across the waterline's triangles); on a mesh's upper half a vertex finds
        the water it pours out of up to CREST_RISE 64 under it (the crest's
        bulge), and that plane is not the water it falls into (a rim of foam
        along the lip).
-   Tried and reverted: the falls' own depth test (a sheet weaving under the
    ground drawing only its foam there); the lattice was not that.
-   Left: the rim and waterline changes are not yet checked on screen; the falls
    read about 35% darker than the river across a lip (cause not measured).
-   References: D121, D128, D145, D147, D171, D173, plans/next-visuals.md

## D175: The falls' normal on the mesh's own tangent; the meeting foam closed through the crossing

-   Date: 2026-10-21
-   Context: after D174, hard-edged patches in the falls' reflection at every
    crest's most convex part (SS 1207-1221), a white teardrop (SS 1222, 1223)
    and a dark line through the meeting foam (SS 1209, 1228, 1229, 1233).
-   Decision:
    -   **The detail normal on the mesh's own tangent** (FallRefraction::Vertex
        gains the direction the texture's v runs, from each triangle's
        texture coordinates, a unit direction weighted by area, summed per
        vertex; FallShading::SheetNormal). Found with a screen split into the
        normal's parts (only the final, laid-on-the-sheet normal showed the
        edges). Every frame from the pixel failed: the screen derivatives are
        constant per triangle (D174); the bake's flow changes texel to texel;
        "down the sheet" flips across a crest's ridge, where downhill turns
        from upstream to downstream. Dividing by the texture area instead gave
        huge tangents where the coordinates collapse: NaN, the teardrop.
    -   **No flowmap normal on the falls**: the river simulation's weight at
        their world xy switched it on where a sheet overhangs river.
    -   **The sky read 4 mips down** (REFLECTION_BLUR_MIPS) and the reflection
        march's hit faded with how far behind the scene it landed (no hard
        rock-to-sky split between neighbouring rays).
    -   **The meeting foam through the crossing:** where two sheets cross they
        lie at one depth, and the second depth layer held neither (the
        diagnostic: the contact only failed on the crossing line itself). The
        next sheet's gap is the nearest of a cross of pixels 3 out
        (FallShading::SheetGap) and the vertices' own distance to the nearest
        other sheet (D128, for close views, where the line runs wider than
        the pixels reach). PSBehindDepth leaves out only surfaces within 0.05
        units of the front (2 before). The cut counts as a contact
        (CUT_FOAM_REACH 24), and foam is opaque to the sheet's edge.
-   Tried and reverted: the crest blend (D170) merged as is (SS 1206: its look
    was written for FallSurface's; redo it on the river-shader falls); the
    falls' own depth test.
-   References: D128, D170, D173, D174

## D176: The falls' foam on their flow; their feet in the water; falls are not obstacles

-   Date: 2026-10-21
-   Decision:
    -   **The foam on the bake's flow coordinates** (FallShading::FlowFoamPattern,
        FallGround::Flow's uvA, FOAM_TILE a repeat, whole along the flow): it
        moves, stretches and bends with the baked water (the user: "conform to
        the water flow baked").
    -   **The height over the water below per pixel** (FallShading::AboveWater):
        over the pools' captured surface where the pixel is over a pool (but not
        the pool the falls pour out of), else the vertices'. Each vertex took
        its own plane, and the height leapt across triangles at the waterline.
    -   **The source is water over the mesh's middle** (FallSites): a short
        drop's foot took the pool it lands in for the water it pours out of,
        a lip, and had no waterline foam (SS 1236). With that, the water below's
        foam is gated by the lip again (on a short drop the crest lies within
        its reach of the pool below, SS 1259-1261).
    -   **Under the water only the falls' foam shows**, in its colour, carried on
        WATER_FOAM_SINK 32 under the surface; the cuts under the water are 48
        down (WATER_KEEP, POOL_CUT): the sheets' wobble lifted a cut line into
        view (the user).
    -   **Waterfall and rapids effects are not obstacles** (MeshCut): cut at the
        water's surface, every fall's foot became a row of dry cells, holes in
        the pool's foam (SS 1245, 1246; the user: on the water plane's shader).
    -   **The next sheet's per-pixel gap only where the vertices say another
        sheet is near** (SHEET_GAP_TRUST 96): a falls' own stacked layers,
        brought within a hair of each other by the wobble, foamed in slivers
        (SS 1251-1253).
    -   **Tuning sliders for the plunge foam** (not saved): moved behind the
        falls' line against their flow, and its radius; behind closed the gaps
        behind the curtains (the user).
-   Tried and reverted: a skirt of geometry at each mesh's foot (it was cut
    away again under the ground; the sheets already run under the water).
-   References: D44, D47, D68, D145, D174, D175

## D177: The falls write their depth; the pool's foam reaches the walls

-   Date: 2026-10-21
-   Decision:
    -   **The see-through falls write depth** (FallRefraction's depth state,
        the scene's writable view): the passes after them (distance fog) read
        the depth of what lay behind a sheet, a hard line across it where that
        changed from rock to ground (SS 1268; found with water rendering off:
        the line stayed, so no water plane drew it).
    -   **The simulation's foam carried into dry corners**
        (WaterNormal.hlsli BilinearFlowFoamToWalls): blended toward a wall's
        zero, the pool's foam stopped a cell short of every rock (SS 1264, 1265).
-   Tried and reverted: testing the falls against the opaque scene only (a
    shard, SS 1269); no shore fade and no plane test on the falls (neither was
    the line); a crest morph from the river's normal maps (the water type's
    maps tile ~17 m; the river's visible detail near crests is its flowmap's,
    patch in the scratchpad, morph_attempt.patch).
-   References: D105, D125, D159, D176

## D179: Flow sim v2: the river speeds up toward the lips; the bake's water reaches the shading; bubbles

-   Date: 2026-10-21
-   Decision:
    -   **The river is drawn on toward each lip** (D170's solver part,
        FallLips.cpp and WaterForcesCS, without its falls-side texture changes):
        within 320 units upstream and 64 to each side, the target speed rising
        from the river's own current to the lip's at the brink (pulled toward
        the lip's speed wherever it reached, the whole cascade sped up: the user).
    -   **The bake carries the water's fields** (a second half of its rows,
        FallGround::BakeFieldsAt): speed over vanilla's, swell (compressed to
        0-1), disturbance, aeration. Aeration grows gradually with the height
        fallen down steep faces (1 - exp(-fallen / breakup); a threshold made it
        on or off in broad patches), blurred wider.
    -   **Aeration shows as bubbles and foam** (the user): stirred water (the
        bake's disturbance: a boulder's wake, piling against rock) aerates too;
        aerated water foams in the contact foam's pattern (AERATION_FOAM 0.3,
        not on the lip) and bubbles.
    -   **Bubbles** (FallShading::Bubbles): three layers at depths under the
        surface, parallaxed by the view across the sheet, smaller and dimmer
        the deeper; round on the screen at any slant (camera-facing: on the
        sheet they foreshortened into flat ellipses); shaded as air spheres in
        water (a silvery rim, the sun's highlight, what is behind lensed and
        dimmed); four times as dense in aerated water as in the plain flow, a
        third of the first try overall (the user).
-   Tried and set aside: the swell as a normal from screen derivatives (per
    triangle again: facets; SWELL_HEIGHT 0 until its slope comes from the
    bake's field); milky tint and opacity from aeration (the sheets went flat
    grey).
-   References: D59, D80, D84, D85, D170, D174

## D180: The falls' water is the sheet's own thickness

-   Date: 2026-10-21
-   Context: the falls read near black, darker than the river they pour from
    (the user: "we strayed from the look of the shallow water"). Measured with
    strips of the water shader's parts: refractionMul (the water's opacity) was
    1 over every sheet and the water's own colour near black.
-   Decision: on the falls, every depth measure (distanceMul: the vanilla fog
    and opacity, the Water Tint) is the sheet's own thickness along the view
    (SHEET_DEPTH 24 over the cosine, at most 4x), not the view ray's path to
    what lies behind it: mostly air, metres of it, read as deep water. The
    falls are see-through again, the rock behind showing as in the shallows.
-   References: D148, D159

## D181: The falls as the river's water; a finer bake; spray and mist from the churned water

-   Date: 2026-10-21
-   Decision:
    -   **The river's milkiness and depth** (the user: "missing that depth and
        milkiness"): the falls lifted toward the foam colour by
        FluidField::ApplyAeration's measure (their aeration and speed, up to
        MAX_AERATION), the sheet's water SHEET_DEPTH 96 along the view (24 read
        as glass beside the river) and its tint at least SHEET_TINT_DEPTH 160.
        Tried 40 / 64: too clear (the user).
    -   **Foam** slightly see-through everywhere (FOAM_OPACITY 0.8: the river's,
        the pools', the falls'), broken into clumps by a soft coarse mask
        (FoamBreakup, the same pattern four mips down, at least 0.75).
    -   **A finer bake**: BAKE_TEXEL 6 (10), BAKE_MOST_SIDE 320 (192), its foam
        and aeration blurred wider; cache version 9. Bubbles halved again.
    -   **Spray and mist from the churned water** (the user): emitters where
        the bake's aeration is over AERATED_SPRAY 0.55, out of the sheet's open
        side by the bake's clearances (the travel's side pointed into the cliff
        on overhangs), thrown mostly up; none past SPRAY_FAR 3000 from the
        camera (faded from 2000). Mist puffs softer, lit with forward scatter.
-   Tried and reverted: the foam pattern stretched by the flow coordinates'
    stretch, from screen derivatives (constant per triangle: it jumped at every
    edge) and from the bake (scaling the run coordinate, large far down a sheet,
    by a factor varying across it sheared it into swirls, SS 1315). The bake's
    stretch stays in its third rows for a scale-based use.
-   Left (plans/next-visuals.md): far too many spray emitters (19,554 striking:
    a higher aeration threshold, wider spacing, a cap; spray 24 and mist 3 per
    emitter meanwhile); mist puffs cut by the water and the falls' depth (the
    hardware test against them: test against a copy with them instead, softly).
-   References: D31, D35, D104, D174, D179, D180

## D182: Spray only near the camera, never from the ground; particles shaded as small volumes

-   Date: 2026-10-22
-   Decision:
    -   **Only the emitters near the camera reach the GPU** (all 19,554 after
        D181's finer bake cost the vertex stage): FallSurfaceRenderer::UploadNear
        picks them, nearest first, again each time the camera moves far enough.
        The eye is the main camera's (the cached frame's alternated with other
        passes').
    -   **Never from the ground** (the user, SS 1316, 1317): the bake reads the
        whole mesh (D132), so texels under the landscape threw spray from inside
        it. ComputePlunges reads a copy of the landscape round the falls on its
        worker and drops emitters more than SPRAY_BURIED under it.
    -   **Soft contact** against a copy of the depth with the water planes and
        the falls in it, over a short reach. Droplets fade once they fall back
        below where they were thrown.
    -   **Shading** (the user: catch the light, have depth): a sphere's normal
        across each quad, a wrapped sun term, Henyey-Greenstein forward
        scattering (mist more), the light's colour only in part (PARTICLE_COLOR).
    -   **The bubbles' stretch** laid on the run coordinate wrapped into a tile,
        in two phases half a tile apart (D181's shear, SS 1315, stays within a
        tile).
-   References: D126, D132, D181

## D183: Fixed emitters on the drawn water, stable ids, a debug view

-   Date: 2026-10-22
-   Decision:
    -   **Emitters on the water as drawn**: each texel pushed out by its swell
        (ComputePushes) and EMIT_PROUD over it. Lifting particles by the depth
        buffer was view-dependent (the user saw an emitter's height change with
        the camera's distance, SS 1345-1347).
    -   **A stable id per emitter** (an FNV hash of its floored position and
        kind, 21 bits, packed as kind + 4 id): its particles' random choices and
        its on/off switching follow it, not its place in the near list, which
        changed as the camera moved (the user: jitter).
    -   **Switched on and off** each SWITCH_PERIOD, with a chance rising with the
        simulated foam and flow at it.
    -   **Reach**: SPRAY_NEAR_FADE 6000, SPRAY_FAR 8000 (2000 / 3000 went out in
        plain sight, SS 1351); thinning from SPRAY_LOD_FROM to a fifth, by each
        particle's own hash (no flicker), the rest drawn bigger.
    -   **Bake back to 8 units a texel, 256 a side** (the user: D181's 6 / 320
        too costly); cache version 10.
    -   **DEBUG_EMITTERS** (FallSpray): a marker over everything on each
        emitter, pink crest, green meet, blue strike, dim while off.
    -   Test conditions (the user): clear weather (fw 81a), 13:30, the full
        load, the recorded camera pose.
-   References: D78, D82, D182

## D184: The emitters where the user drew them: crests, meeting sheets, rock

-   Date: 2026-10-23
-   Decision (the user's colour map, SS 1352):
    -   **Pink, the crest**: only on meshes that pour out of a body of water
        (at least SOURCE_LEAST_CRESTS and SOURCE_SHARE of their columns' crests
        at a plane's or a pool's level: the meshes the crest blend works on),
        none from meshes in the middle of a fall. Each column from where its
        bare normal turns from the sky to falling (the first shown texel past
        the crest under LIP_TANGENT 0.94 up), and weaker ones over the band
        upstream of it (CREST_BAND_ROWS, the user, SS 1370). A column that runs
        into rock after its water starts throws none (the user); rock before it
        is the lead-in tucked into the bed. Thrown fast (CREST_LAUNCH, at least
        CREST_THROW), level to a little up or down, fanned in a V (CREST_FAN
        0.3), fading as they fly.
    -   **Green, where sheets meet**: texels whose shown water lies within
        MEET_EMIT of another mesh's shown water (the culling's meet line, D126).
        A grounded heavy mist drifting forward a little (MEET_DRIFT), and
        droplets fanned forward like the crest's but steeper (half the slots).
    -   **Blue, rock in the way**: foam over STRIKE_FOAM with rock within
        HEAD_ON_REACH along the turned flow and none beside it (a face, not a
        bank). Splashes up, leaning on the flow, and a longer mist.
    -   **Never deep under water**: none more than EMIT_DEEP under another
        sheet's shown surface, or EMIT_SUBMERGED under a plane or pool (the
        plane's bound reaches over the fall: not past a crest).
    -   **Droplets** tear-shaped (DROP_LONG, DROP_THIN, each its own width and
        length), DROP_BIG larger, leaving at DROP_OPACITY 0.75, shaded darker
        away from the sun and underneath. Every droplet slot thrown while its
        emitter is on. Mist 30% sheerer (MIST_SHEER).
    -   No more spray from the plunges' feet or the crossings of water surfaces.
-   Left: bake and bake-load times, nearest falls first (the user); the crest
    blend; the foam's relief; a volumetric or parallaxed mist (the user's idea).
-   References: D70, D79, D126, D170, D183

## D185: The falls' load measured, and made three times faster cold, seven times warm

-   Date: 2026-10-23
-   Measured (the user: time it instead of assuming): cold starts with every cache cleared (shader cache, bakes, dumps), the
    falls save, three runs each, from the first site to a new "Falls settled" log line (every capture, copy, bake, culling,
    link and plunge done, nothing new for SETTLE_SECONDS), with each stage's render-thread and worker time; one run under
    WPR for the functions (an elevated prompt).
    -   Before: 125.5 s (124.6, 126.2, 125.7). Only 20 s of it render-thread work: the bakes' rock tests (16 s of Havok rays at
        3 ms a frame) paced the whole load, and stopped whenever a worker ran; culling ran 11 times (37 s on one thread), each
        new site or bake asking again; CommonLib's NiPoint3 operators cost 17 s of calls (its library is out of the
        optimiser's reach); a capture in a cell with no physics world never finished and held the load open.
    -   After: 41.1 s cold (40.9, 41.3, 41.2), 12.0 s warm (82 s before). The rest of a cold start is the shader cache's
        recompile (about 4 minutes, not the falls').
-   Decision:
    -   **The rays go on while workers run**: the captures' and the bakes' rock tests touch nothing the workers have; only
        landing their results (RefreshGrounded, bakeFields) waits (CaptureStep's finish, LandBakes).
    -   **Captures** nearest the camera first, a capture that cannot cast (no physics world) passed by, CAPTURE*BUDGET_MS 3
        (1), and kept on disk (FluidFieldBakes/ground*\*.bin, by its layout and the statics' triangles round it).
    -   **Culling** waits (at most CULL_DEFER_MOST) while captures, dense copies or rock tests that would ask again are coming
        (11 passes to 2), and its inside cut, sides and meets run over every sheet's vertices on several threads; the meets
        skip triangles whose box lies further than the best so far.
    -   **Inline vector maths** (V3) in TriangleDistanceSquared and RayTriangle.
    -   **The rock test on every other texel first**: the texels between take their tested neighbours' answer where those
        agree (all rock, or all open with clearances within CLEAR_AGREE), tested only at the rock's edges (52% of the rays;
        rock texels 39,103 against 38,955 all tested). BAKE_BUDGET_MS 5 (3); cache version 11.
-   Left: the rock tests (9 s of rays at 5 ms a frame), the solves (14 s of CPU on workers), the seams (4-6 s), and a warm
    load's dense copies, seams, culling and linking (2-3 s each).
-   References: D82, D112, D113, D126, D172, D173

## D186: Volumetric mist, lens droplets in the sun's shadow, seam mist where the water lands, smooth sheet frames

-   Date: 2026-10-23
-   Decision (the user's rounds over SS 1374-1412):
    -   **Mist as volumes**: each puff an ellipsoid the view ray is traced through, cut where the scene lies (a soft, true
        contact: no jagged depth-test line, SS 1374; the particles' hardware depth test is off, the shaders test the depth
        copy), denser toward its heart (MIST_CORE), broken into wisps by world-space value noise (parallax), self-shadowed
        toward the sun. It takes light in: a grey albedo, little sun and forward glow, darker the thicker and underneath.
    -   **Droplets**: tear-shaped, each its own width and length, 50% bigger, 75% opacity, shaded as small bodies (sky light
        by the way they face, fresnel rim, glint) and as lenses: the frame behind flipped through them (a copy taken just
        before the spray, the falls in it) and the surroundings' cubemap by fresnel, over their shape at any opacity.
    -   **The sun's shadow on both**: Volumetric Shadows' map and cascades (as FallRefraction found them bound), terrain and
        clouds.
    -   **Crests (pink)**: a narrower V (CREST_FAN 0.18), mostly the flow's way (CREST_FACING), biased toward the river's
        simulated flow; a nudge up; fading as they fly; the band reaching upstream to near-flat water (CREST_FLAT), the
        shown lead-in included.
    -   **Where sheets meet (green)**: the seams' points joined into lines; mist and droplets only on each line's lowest
        MEET_FOOT_BAND (where its water lands), droplets alone higher up (FallEmitterKind::MeetDrops): stacked mist hung down
        the tongues (SS 1402) however single points were judged. Grounded mist drifting forward; droplets fanned forward,
        steeper, carried 50% farther.
    -   **Rock (blue)**: twice as dense (STRIKE_SPACING 28, STRIKE_FOAM 0.65), fewer droplets each (STRIKE_DROPS), a narrow
        cone (STRIKE_CONE); the contact foam solid where it is strong (ContactCover: head on, thick and steady, SS 1409).
    -   **Aeration**: its look eased inward (faint at a patch's rim), less milky (MILK_AERATION, MILK_SPEED), its foam in two
        parallaxed layers under the surface, the bake's field blurred 10 passes (SS 1405); bubbles 7:3 aerated to plain, 10%
        fewer.
    -   **Smooth sheets**: the dense copies' normals shared across texture seams and blurred over ~80 units; the see-through
        falls' normal-map frame laid downhill (FallRefraction: the texture coordinates gave one way per original triangle,
        the creases of SS 1395, 1408; found with Water.hlsl's FALL_NORMAL_DEBUG split screen, SS 1412). Bake cache 14.
-   Left: stronger aeration on vertical plunges and per-texel plunge data in the bake (plunge foam and particles); the
    crest blend.
-   References: D182-D185

## D187: Smooth falls sheets: the flow's frame, a smoothed and adaptive dense copy, lighting on the pushed surface, wide deformation

-   Date: 2026-10-23
-   Decision (the user's rounds over SS 1412-1461, diagnosed with temporary split-screen views of the falls' normals and
    terms in Water.hlsl, since removed):
    -   **The normal maps' frame is the flow's own**: LinkBakes stores, per texel, the world way the along coordinate runs
        with across held (over FRAME_REACH texels each way, FRAME_BLUR passes) in the bake texture's third section (yzw);
        the river shader's falls lay the normal maps and the bubbles' parallax on it. The mesh's tangents (one way per
        original triangle: the creases, SS 1412) and a downhill frame (singular at every dome's top, SS 1416-1417) are gone.
    -   **The dense copy's shape smoothed as it is split**: Taubin pairs after every splitting round (DENSE_SHAPE_LEVEL_PASSES)
        and at the end (DENSE_SHAPE_PASSES), along the normal only, open edges held, at most DENSE_SHAPE_MOST from where
        each vertex was made; normals from the new faces. PN triangles left each original triangle nearly flat: broad
        bands in the light and in everything reading the shape (SS 1438, 1440). Bake cache 16.
    -   **Denser near solids**: edges within DENSE_NEAR_REACH of a static above the ground split to DENSE_EDGE_NEAR, up to
        DENSE_NEAR_BUDGET vertices a copy (near the landscape too, a sheet overflowed 16-bit indices and vanished).
    -   **The light follows the pushes**: ComposeShifts works out each mesh's normals as its seam moves and swell leave it;
        the see-through falls' vertex buffers take them.
    -   **Near rock**: the push and wobble none within ROCK_CONTACT of rock, rising over ROCK_RAMP, softened ROCK_FIELD_PASSES
        times, a gentle slope on the fine mesh; the wobble's coordinates softened (they jump where the strands part round a
        boulder). Churning the wobble toward rock spiked the fine mesh and was reverted (SS 1444-1453); the narrow ridges of
        SS 1461 asked for the deformation in area, not local.
    -   **Vertical drops aerate sooner** (FallStrands: up to VERTICAL_AERATION more toward upright faces).
    -   **Spray**: the mist much more visible (MIST_DENSITY 0.8, MIST_SHEER 1); none from rock strikes (it stacked against
        the rock), only where sheets meet.
-   Left: the falls' water texture stretched and magnified on fast water (SS 1466); the light following the wobble; per-texel
    plunge data in the bake; the dense copies' load time (~17 s on their worker).
-   References: D73, D82, D99, D174, D179, D185, D186
