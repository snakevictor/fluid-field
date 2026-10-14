# Fluid Field for Community Shaders — Roadmap

Oct 13, 2026 · @Victor

## Overview

Build one shared, real-time fluid simulation around the player as a Community Shaders feature (working name: **Fluid Field**), and use it for four things, in priority order: realistic shallow water, interactive fog, volumetric smoke, and buoyancy for physics-enabled objects. It is a FluidNinja LIVE-style system reimplemented from public techniques, with no FluidNinja code or assets.

| Priority | Goal                                                                                                               | Release |
| -------- | ------------------------------------------------------------------------------------------------------------------ | ------- |
| 1        | Shallow water that flows around rocks, carries wakes, reflects off shores and blends into vanilla rivers and lakes | v0.1    |
| 2        | Ground fog that parts around actors and is blown away by shouts and spells                                         | v0.2    |
| 3        | Lit, self-shadowed smoke from fires, forges, chimneys and fire magic, bent by the same wind                        | v1.0    |
| 4        | Loose physics objects and bodies that bob on simulated waves and drift with currents                               | v1.1    |

Grass bending and snow and mud trails become an optional side track. They are cheap once the field exists and can ship whenever a contributor picks them up.

Out of scope for now: steerable boats and anything else with real gameplay weight, large-scale ocean simulation, and an offline baking tool.

Several design choices depend on techniques that move fast, so the plan marks **research spikes** (R1 to R9) where an agent investigates the current state of the art before anyone builds. They are listed in their own section and referenced from each phase.

## Foundations and licensing

Build Fluid Field as a feature inside the Community Shaders repository, not as a separate SKSE plugin. Features already get their own package (`Package-<Feature>` build targets), their own shaders folder and their own toggle in the in-game menu, and shipping inside the repo lets us reuse its hooks directly. Develop on a fork and aim to upstream once Tier 1 is stable. The canonical repo is [community-shaders/skyrim-community-shaders](https://github.com/community-shaders/skyrim-community-shaders).

### What to borrow

| Existing piece                                                                                                     | What Fluid Field takes from it                                                                             |
| ------------------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------- |
| Unified Water and Water Effects                                                                                    | Water shader hooks plus the existing ripple and flow-map functions; the shallow-water output plugs in here |
| Skylighting                                                                                                        | May already render the scene from above; a candidate source for the depth and obstacle map (R1)            |
| Volumetric Lighting, shadows and the GI work                                                                       | Starting point for the fog and smoke renderer and its lighting (R4, R5)                                    |
| Grass Collision ([Nexus](https://nexusmods.com/skyrimspecialedition/mods/87816))                                   | Actor tracking around the player and the virtualized collision texture                                     |
| Snow Deformation (in progress, [releases](https://github.com/community-shaders/skyrim-community-shaders/releases)) | Stamps gathered from Havok collision shapes, a model for reading physics objects as inputs                 |
| Wetness Effects                                                                                                    | Sky occlusion, so sheltered areas get no rain ripples or wind                                              |
| Feature framework, Tracy profiler, RenderDoc workflow                                                              | Settings UI, per-feature packaging, GPU timing and frame capture                                           |

Prior art outside Community Shaders has its own licenses, so study it rather than copy it: [Splashes of Skyrim](https://www.nexusmods.com/skyrimspecialedition/mods/47710) detects projectile impacts on water, [Loki's Wade In Water](https://www.nexusmods.com/skyrimspecialedition/mods/42854) hooks the per-actor submerged-level check, and [Realistic Functional Boats](https://www.nexusmods.com/skyrim/mods/56410) shows vanilla Havok can already float objects.

### License obligations

Community Shaders is [GPL-3.0-or-later with a Modding Exception and a GPL-3.0 Linking Exception](https://github.com/doodlum/skyrim-community-shaders). Borrowing is safe, but it carries obligations:

-   Fluid Field must also be GPL-3.0-or-later, with full source published wherever the DLL is distributed.
-   Shader folders can carry their own LICENSE file. Check each one before copying shader code, and otherwise assume the default license.
-   The Community Shaders logo is not covered by the GPL. Do not reuse it in branding.
-   FluidNinja is a separate commercial product. Reimplement the techniques from public sources only: no code, Blueprints, materials or presets.

## Architecture

Every effect reads from one simulation, so a shout pushes waves, parts the fog, bends the smoke and nudges floating barrels at the same time, without separate systems.

_(Diagram: Fluid Field architecture · 5 inputs, 5 field layers, 4 outputs. See the shared roadmap doc.)_

The grid is centred on the player and scrolls with the camera, so it never covers the whole world. A depth and obstacle map of the same area tells the water where the riverbed, rocks and shores are. Inputs are gathered on the CPU once per frame and uploaded as a small list of impulses; the layers are GPU textures updated by compute shaders. Smoke is the one exception to the 2D grid: a few small 3D volumes sit around the most visible emitters, using the grid's wind. Buoyancy is the only output that goes back to the CPU, through a small readback of the water layer a few frames late.

Three design rules keep it maintainable:

-   **One owner for all GPU resources.** Fluid Field creates and frees its own textures and never holds on to engine-owned ones, which avoids the loading-screen crashes that hit the ray-tracing work.
-   **Layers are optional.** Each output can be switched off, and a layer nobody reads is not simulated.
-   **Inputs are data, not hooks.** New sources (a modder's custom spell, say) register an emitter through a small API instead of patching the feature.

## Phases

Seven phases take Fluid Field from an empty feature to buoyancy. Each phase opens with its research spikes, which must be written up and decided before building starts, and ends at a gate that must pass before the next phase. Budgets are starting targets to validate, not measured figures.

1. **Phase 0, groundwork.** Fork and build Community Shaders, set up RenderDoc, Tracy and the AI context files. Agents write read-only reports on Unified Water, Skylighting, Volumetric Lighting, Grass Collision and Snow Deformation.
    - Gate: an empty Fluid Field feature toggles cleanly from the menu and costs nothing when off.
2. **Phase 1, simulation core and scene capture.** A player-centred grid that scrolls with the camera, a wind layer using stable-fluids passes, and a depth and obstacle map of the area around the player. Include a debug overlay for every layer.
    - Research first: R1 (depth and obstacle capture).
    - Gate: stable for an hour of play, survives cell transitions and loading screens, fits a 0.5 ms GPU budget at 1440p on a mid-range card.
3. **Phase 2, inputs.** Feed the field from actors (position, velocity, selected bones), horses, projectiles, explosions, shouts, spells, and loose physics objects as sources. Cap tracked objects by distance and priority.
    - Gate: every input shows correctly in the debug overlay; tracking 32 sources costs under 0.1 ms on the CPU.
4. **Phase 3, shallow water.** Solve the shallow water equations on the depth map, so water flows around rocks, carries wakes, piles against shores and reflects. Seed currents from vanilla river flow and blend seamlessly into vanilla water at the grid edge. Output height, normals, foam and flow to the water shader.
    - Research first: R2 (solver), R3 (blending with vanilla water and boundaries), R8 (motion vectors for upscalers).
    - Gate: public beta v0.1. Works in rivers, lakes and coast, with Water Parallax and the two most popular water mods, and shows no seam at the grid edge.
5. **Phase 4, interactive fog.** A camera-aligned volumetric fog renderer fed by a density layer in the field, so fog parts around actors and is blown away by shouts. Lit by sun, moon and local lights with existing shadows.
    - Research first: R4 (volumetric fog renderer).
    - Gate: release v0.2. Fog reacts to the player and shouts, sorts correctly with water and particles, and fits a 1.0 ms budget at 1440p.
6. **Phase 5, volumetric smoke.** Small local 3D simulations around the most visible emitters near the player (campfires, forges, chimneys, fire spells), driven by the field's wind and drawn by the fog renderer with self-shadowing.
    - Research first: R5 (smoke simulation and lighting).
    - Gate: release v1.0. Smoke curls, rises, pools under ceilings, and holds a combined fog and smoke budget of 1.5 ms at 1440p.
7. **Phase 6, buoyancy.** Read a small patch of the water layer back to the CPU each frame and apply buoyancy and drag forces to nearby loose physics objects and bodies through SKSE, so they bob on waves and drift with currents.
    - Research first: R6 (GPU-to-CPU readback), R7 (Havok coupling).
    - Gate: release v1.1. Objects float stably at 30 to 144 FPS, no launches or jitter in an hour of testing, and saves load cleanly.

**Optional side track:** grass bending and snow and mud trails from the same field. Any contributor can take these after Phase 2, as long as they stay within their own budgets.

## Research spikes

Before building each risky piece, an agent surveys the current state of the art, prototypes the strongest options and writes a recommendation; a human makes the call. The candidates below are starting points, not conclusions: the agent should look for newer work and say if something better exists.

| ID  | Question                                                                | Starting candidates to compare                                                                                                                                                                                              | Deliverable                                                               | Phase |
| --- | ----------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------- | ----- |
| R1  | Cheapest accurate map of riverbed, shore and obstacles near the player  | Top-down orthographic depth pass redrawn as the grid scrolls; landscape height data plus collision shapes; reuse of Skylighting's overhead view                                                                             | Prototype of the best two, cost and accuracy on the test routes           | 1     |
| R2  | Shallow water solver that is stable, fast and handles wet and dry edges | Virtual pipe model; height-velocity equations on a staggered grid with semi-Lagrangian or MacCormack advection; well-balanced finite-volume schemes such as Kurganov-Petrova; detail layers such as iWave or wave particles | HLSL prototype on a test heightmap, compared for stability, look and cost | 3     |
| R3  | Blending simulated water into vanilla water                             | How vanilla water planes, river flow and LOD water work (Ghidra and RenderDoc); absorbing or sponge boundaries at the grid edge                                                                                             | Report with engine notes and a chosen fade strategy                       | 3     |
| R4  | Volumetric fog renderer                                                 | Froxel-based volumetric fog as used in Frostbite and Assassin's Creed IV; temporal reprojection; how Community Shaders' volumetric lighting and ENB handle fog today                                                        | Design note and a fog-only prototype                                      | 4     |
| R5  | Smoke simulation and lighting                                           | Local 3D stable-fluids grids (GPU Gems 3, chapter 30); sparse or bricked volumes; particle-based alternatives; transmittance volumes or deep shadow maps for self-shadowing                                                 | Comparison and a one-campfire prototype with emitter-count limits         | 5     |
| R6  | Getting water data back to the CPU without stalls                       | D3D11 staging buffers read a few frames late; computing forces per object on the GPU and reading back only those                                                                                                            | Measured latency and cost                                                 | 6     |
| R7  | Applying forces safely in Havok                                         | How vanilla buoyancy works; CommonLib's rigid-body APIs; the safe point in Havok's step to apply forces; thread safety and high-FPS behaviour                                                                               | Ghidra notes and a minimal plugin that makes one barrel bob               | 6     |
| R8  | Motion vectors for upscalers and TAA                                    | What DLSS, FSR, XeSS and Community Shaders' TAA need from moving water and volumes                                                                                                                                          | Checklist of outputs each surface must write                              | 3     |
| R9  | Performance in VR                                                       | Sharing the field between eyes; resolution scaling for volumes; Community Shaders' existing VR paths                                                                                                                        | Budget plan for VR                                                        | 4     |

Rules for every spike:

-   **Timebox it.** A few agent sessions, then report what is known and what is still open.
-   **Cite primary sources.** Papers, GDC and SIGGRAPH talks, official docs and source code; mark anything unverified.
-   **Check licenses.** Ideas from papers are free to use, but any reference code must be GPL-3.0 compatible before it is borrowed.
-   **Write it down.** Each spike produces `research/R<n>.md` with the recommendation, the rejected options and why, and the decision goes into a decisions log.

## Testing, performance and compatibility

The simulation is easy to get working and hard to keep stable, so most testing effort goes into transitions, edge cases and other mods.

**Fixed test routes.** Record a small set of save files and walk routes that hit the hard cases: Riverwood's river and bridge (flow around rocks, a vanilla river edge), a Solitude dock (open water and shorelines), a foggy marsh or forest at dawn, a forge or inn interior (smoke under a ceiling), a campfire camp at night, a fast travel and a horse gallop across a cell border, and a buoyancy pool where barrels and bodies are dropped into moving water. Capture Tracy timings and RenderDoc frames on each route per release.

**Performance budgets.** Track GPU time per pass and the CPU cost of input gathering. Every output has a quality preset (off, low, high), so a low-end player can keep water ripples and drop the rest. The whole feature costs nothing when disabled.

**Compatibility matrix.** Test on each supported runtime (SE 1.5.97, AE 1.6.1170, AE 1.7.x and VR) and against:

| Area                      | What to check                                                                                                     |
| ------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| Water mods                | Popular water replacers and Water Parallax: simulated water lines up, no double normals, no seam at the grid edge |
| Weather and lighting mods | Fog density and colour follow the active weather; no double fog with weather-mod fog                              |
| ENB                       | Fluid Field disables its fog and water outputs cleanly if ENB's own versions are active                           |
| Upscalers and TAA         | Motion vectors for water and volumes, no ghosting behind wakes or smoke                                           |
| Physics mods              | Havok frame-rate fixes and physics-heavy mods: no jitter, launches or crashes with buoyancy on                    |
| VR                        | Both eyes see the same field; budgets hold at VR frame rates                                                      |

**Failure behaviour.** Every hook checks for missing data and falls back to vanilla rendering instead of crashing. The field resets after loading screens rather than carrying stale state.

## Working with AI agents

Agents write most of the code; humans own the look, the budgets and the final merge. The repository is already set up for this: it ships a `.claude` folder (with `AGENTS.md` and `.github/copilot-instructions.md` pointing to it), and pull requests get automated CodeRabbit reviews.

Give agents the same three things that made the recent decompilation projects work:

-   **Reference examples.** Point every task at the closest existing feature (Grass Collision for inputs, Unified Water for water output) and tell the agent to extend that code rather than invent a parallel version.
-   **A referee.** Agents cannot see the game, so each task needs a check they can run: shader compilation, a unit test of the solver on a known input, a Tracy timing threshold, or a RenderDoc capture compared against a saved reference image.
-   **Shared learnings.** Keep `docs/fluidfield/LEARNINGS.md` where agents record engine quirks they discover (when resources are freed, which thread does what, how water planes are split), so the next task starts from them.

Good agent tasks: solver passes in HLSL, scrolling-grid addressing, finding where Skyrim stores projectile and magic-effect data with the annotated Ghidra archives, settings UI, packaging. Keep humans on: art direction and tuning, budget trade-offs, the hook design for anything that touches loading or threading, and reviewing every hook before merge.

**Research agents are a separate role from build agents.** Run spikes in their own session with web access and no write access to the feature code. Give each one its spike row, the relevant engine notes and the budget it must fit, and ask for a ranked recommendation rather than a single answer. A build agent then starts from `research/R<n>.md` and the decision in the log, never from its own guess about which technique to use.

Run parallel build agents in separate git worktrees, one scoped task each, the way the Snowboard Kids decompilation ran four at once. Merge often so each agent sees the others' finished work.

## Risks and open questions

| Risk                                                                                             | Impact | Mitigation                                                                                                                  |
| ------------------------------------------------------------------------------------------------ | ------ | --------------------------------------------------------------------------------------------------------------------------- |
| Havok instability from buoyancy forces: jitter, launched objects, crashes                        | High   | Buoyancy last and behind its own toggle; clamp forces; apply them only at the safe point found in R7; test at 30 to 144 FPS |
| Crashes after loading screens from freed engine resources (the blocker the ray-tracing work hit) | High   | Rebuild the field after each load; Fluid Field owns all its GPU resources                                                   |
| Volumetric fog and smoke too expensive, especially in VR                                         | High   | Low-resolution volumes with temporal reprojection; emitter caps; fog and smoke can ship separately                          |
| Visible seam where simulated water meets vanilla water                                           | Medium | R3 decides the fade strategy before building; seam check is part of the v0.1 gate                                           |
| Looks like a tech demo, not part of Skyrim                                                       | Medium | VFX-minded tester from Phase 0; tune against vanilla art; default strengths low                                             |
| Readback latency makes floating objects lag the visible waves                                    | Medium | Keep latency to a few frames; compute per-object forces on the GPU if R6 favours it                                         |
| Water, weather and ENB conflicts                                                                 | Medium | Detection plus clean fallback; published compatibility list                                                                 |
| Next Bethesda update breaks hooks                                                                | Low    | CommonLib and Address Library IDs only; no hardcoded offsets                                                                |

**Open questions**

-   Does the Community Shaders team want this upstream as a core feature, or as an add-on they host but do not maintain?
-   Should fog and smoke share one volume renderer with Community Shaders' existing volumetric lighting, or sit beside it? R4 should answer this with the maintainers.
-   Which objects get buoyancy: all loose clutter, or only items flagged by a keyword or a list?
-   What grid size and resolution balance view distance against cost? Phase 1 decides this by measurement.

## Team and timeline

A team of three part-time contributors, working with AI agents, can plausibly ship shallow water in about five months, fog and smoke by about nine, and buoyancy about a year in. These dates are estimates to revisit at each gate, not commitments.

_(Diagram: Fluid Field timeline · 7 phases, 4 releases. See the shared roadmap doc.)_

The roles that matter:

-   **Engine and hooks lead.** Owns the input layer, resource lifetimes, the Havok coupling and every hook that touches loading or threading. Reviews all agent-written C++.
-   **Graphics and shader lead.** Owns the solver, the outputs and performance budgets.
-   **VFX and test lead.** Owns the look, the tuning defaults, the test routes and the compatibility matrix. This can start as a dedicated community tester.

Phases 5 and 6 are the least certain estimates. If R5 shows smoke cannot fit the budget, ship fog as v1.0 and move smoke to a later release; if R7 finds no safe way to apply forces, buoyancy stays a visual-only bobbing effect until one is found.

## Sources

-   [Community Shaders repository, README and license](https://github.com/doodlum/skyrim-community-shaders)
-   [Community Shaders releases (Snow Deformation, Unified Water)](https://github.com/community-shaders/skyrim-community-shaders/releases)
-   [Grass Collision on Nexus](https://nexusmods.com/skyrimspecialedition/mods/87816)
-   [Community Shaders RT Systems issue (loading-screen crash blocker)](https://github.com/community-shaders/skyrim-community-shaders/issues/1657)
-   [FluidNinja LIVE-2 release notes, CG Channel](https://www.cgchannel.com/?p=178469)
-   [Splashes of Skyrim, Nexus](https://www.nexusmods.com/skyrimspecialedition/mods/47710)
-   [Loki's Wade In Water, Nexus](https://www.nexusmods.com/skyrimspecialedition/mods/42854)
-   [Realistic Functional Boats, Nexus](https://www.nexusmods.com/skyrim/mods/56410)
-   [Havok Fix, Nexus (Havok behaviour above 60 FPS)](https://www.nexusmods.com/skyrimspecialedition/mods/18160)
