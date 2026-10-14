# FluidNinja LIVE 2: capabilities and what fits Skyrim

Research note, 2026-10-13. This is not a numbered spike. It feeds the roadmap
and the spikes R2–R9.

**Sources:** public descriptions only. That means the public LIVE-2 manual,
the release page, the itch.io demo page, CG Channel and 80.lv (links below).
No FluidNinja code, Blueprints, materials or presets were fetched, and none
may be copied (see `ROADMAP.md`, "License obligations").

**Section headings:** `M§x` refers to the manual's sections.

**Guiding principle:** extend what Community Shaders and Skyrim already do;
do not rebuild it (`DECISIONS.md` D8 refinements).

## 1. What LIVE 2 is

LIVE 2.0.0.56 shipped around 2026-09-03 for UE 5.6–5.8, Win64 only. It is a
rework of LIVE 1 in Niagara, wrapped in Blueprints, and ships as a project
rather than a plugin ([CG Channel](https://www.cgchannel.com/?p=178469),
M§15). The author describes it as "a motion-trajectory painter, feeding a 2D
fluidsim, feeding output materials that visualize sim buffers" (M§4.1).

The core design matches Fluid Field's roadmap almost exactly:

-   **A world-pinned grid.** A 2D grid attached to the player, pinned to the
    world and scrolled in quantized steps (M§4.5). Our grid core does the
    same.
-   **Two kinds of input:**
    -   points: bones, object pivots and particles
    -   fields: height, shape masks and bitmaps
-   **A Navier-Stokes simulation.**
-   **Outputs are plain textures:** velocity, density, height, a wet map,
    pressure and divergence (M§4.9). Each receiver samples them using the
    grid's position and scale.
-   **Passive patterns beyond the edge.** Outside the simulated area,
    materials fall back to tiled passive patterns (M§6.3).

**Published costs** (RTX 3080, M§12):

| Case                                                | Cost   |
| --------------------------------------------------- | ------ |
| 512² smoke simulation                               | 0.2 ms |
| 2048² water simulation                              | 1.2 ms |
| Water rendering (tessellated, caustics, whitewater) | 3 ms   |
| Self-shadowed volume rendering (512×512×128)        | 2 ms   |

The author's rule of thumb is that visualization, not simulation, sets the
cost (M§12, M§12.5). That supports our budgets: the 0.5 ms grid budget is
realistic, and the risk is on the output side (fog and smoke rendering).

**Stated limitations (M§14):**

-   no deep water: only a layer about 1 m thick
-   buoyancy listed as a limitation (M§14.8; see the note below)
-   no dynamic underwater effect
-   no physics-engine collision
-   an experimental wave generator
-   prototype-quality particle rendering

Two of these are Fluid Field goals: true shallow water and obstacles taken
from Havok.

**Buoyancy, a correction.** The manual lists "Buoyancy" under limitations
(M§14.8). A re-read of the page did not return that section's body, so the
workarounds our first pass recorded are UNVERIFIED: surface-aligned
particles, Z-constrained bodies, and snapping a hull to the surface. LIVE 2
demo videos reportedly show floating objects, which could not be checked
here. The likely reading is that LIVE shows visual bobbing (objects moved to
the simulated surface) rather than physics forces. That still matters: a
visual-only bob is the roadmap's own fallback if R7 finds no safe way to push
Havok bodies. Re-check against the videos before claiming anything either
way.

## 2. Feature-by-feature feasibility in Skyrim

Verdicts:

-   **Have**: already built or decided.
-   **Realistic**: fits our current plan and budget.
-   **Later**: realistic, but after a named phase or spike.
-   **Research**: plausible, but blocked by an unknown.
-   **No**: not worth doing, or not possible in Skyrim.

### 2.1 Simulation core

| LIVE 2 capability                                                                             | Skyrim / CS equivalent to extend                                                                                            | Verdict                                                                                               |
| --------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| 2D Navier-Stokes grid that follows the player, world-pinned, quantized scroll (M§4.5, M§14.1) | `ff/grid-core` wind layer (toroidal, cell-snapped)                                                                          | **Have**                                                                                              |
| Sparse density (smoke, dust, wind, flat water) (M§6.1)                                        | A density layer on the same grid                                                                                            | **Later** (Phase 4 fog)                                                                               |
| "Dense" mode: liquid accumulates and runs downhill, about 1 m thick (M§6.1, M§14)             | Our shallow-water solver (D8 slice, R2) on R1's bed                                                                         | **Have (planned), beyond LIVE.** A real shallow-water model handles depth, which LIVE says it cannot. |
| Coastal wave generator from terrain slope (experimental, M§6.4)                               | R1 bed slope next to the water surface; waves into the water normals                                                        | **Later** (Phase 3). Cheap once bed and surface exist.                                                |
| Passive tiled patterns outside the simulated area (M§6.3)                                     | Vanilla water normals and Unified Water's flowmap are already the passive pattern                                           | **Have** by design: D8 fades to vanilla at the grid edge. Nothing to build.                           |
| Multiple independent simulations (M§2.1, M§12.6)                                              | Small local 3D smoke grids around emitters (Phase 5)                                                                        | **Later** (R5)                                                                                        |
| Camera-facing simulations for flames and explosions (M§4.1)                                   | Fire is `BSEffectShader` meshes and particles. Distorting their UVs with a small simulation would need effect-shader hooks. | **Research.** Only after Phase 5; cost and hook risk unknown.                                         |
| Sleep by proximity or visibility, lower update rate by distance (M§12.3–12.4)                 | Skip the water step when R1 finds no water in the grid; skip fog when no fog is active; lower the rate indoors              | **Realistic.** Cheap; supports "costs nothing when unused".                                           |
| Sim snapshot cache, so a creek starts already flowing (M§7.3)                                 | Warm start after loading screens: seed from Unified Water's flowmap and pre-roll a few steps instead of starting blank      | **Realistic** (with the D8 slice). No save files needed.                                              |

### 2.2 Inputs

| LIVE 2 capability                                                                  | Skyrim / CS equivalent to extend                                                                                                                             | Verdict                                                                                                                                            |
| ---------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| Skeletal bones as points, 10–200 safe, CPU (M§4.2, M§12.5)                         | Grass Collision's actor gathering (`ProcessLists` + Havok bounds, `reports/grass-collision.md`), extended with bone nodes and velocity                       | **Later** (Phase 2). Extend Grass Collision's gatherer; don't build a second one.                                                                  |
| Physics object pivots and velocities (M§4.2)                                       | `Util::GetShapeBound` on Havok bodies (`src/Utils/ActorUtils.cpp`), as Grass Collision and Snow Deformation do                                               | **Later** (Phase 2). Fix the `hkpListShape` bug upstream first (`LEARNINGS.md`).                                                                   |
| Mesh distance fields for shape imprints and dynamic collision masks (M§4.3, M§5.2) | Skyrim has no mesh distance fields                                                                                                                           | **No.** Substitutes: Havok sphere bounds for moving objects, and R1's raycast map for statics.                                                     |
| Niagara particles through data channels, up to about 10k (M§5.6)                   | Skyrim particle systems                                                                                                                                      | **Research.** Reading many particle systems on the CPU is probably too costly. Projectile and spell positions (Phase 2) cover the important cases. |
| Chaos destructibles (M§5.5, M§5.7)                                                 | No Chaos. Explosions are events.                                                                                                                             | **No** as a feature. Explosions and shouts become impulse inputs (Phase 2).                                                                        |
| Splines that set river flow direction (M§5.3, M§9)                                 | Unified Water's world flowmap already encodes river flow                                                                                                     | **Have** (D8 seeds currents from it). Better than splines: already authored for every river.                                                       |
| Landscape height for flow and alignment (M§5.4)                                    | `TESObjectLAND` heights (R1, D7 stage a)                                                                                                                     | **Have (planned)**                                                                                                                                 |
| Top-down scene-capture height (M§5.4, M§7.4)                                       | R1 Prototype A (depth capture through the precipitation-occlusion path)                                                                                      | **Deferred** (D7 fallback)                                                                                                                         |
| Bitmap or render-target velocity and density, e.g. noise (M§5.1)                   | **Weather wind:** feed the current weather's wind speed and direction into the field as a global input, so the field agrees with vanilla grass and tree wind | **Realistic.** Cheap; best "extend vanilla" input. Weather wind fields and their CommonLib access are UNVERIFIED.                                  |
| Line interpolation between frames for fast movers (M§8.4)                          | Horses, sprinting, projectiles                                                                                                                               | **Realistic** (Phase 2). Avoids dotted wakes at low frame rates.                                                                                   |
| Velocity threshold, so still objects make no ripples (M§8.4)                       | Same                                                                                                                                                         | **Realistic** (Phase 2)                                                                                                                            |
| Negative brush that erases density, e.g. a track through fog (M§8.4)               | Fog parting around actors (Phase 4 goal)                                                                                                                     | **Later** (Phase 4)                                                                                                                                |
| Owner-only tracking for torches and burning limbs (M§14.3)                         | Torches, flame-cloak and fire-spell hands as emitters                                                                                                        | **Later** (Phase 2 inputs, Phase 5 smoke)                                                                                                          |
| Wind as a dedicated input                                                          | Not found in LIVE (UNVERIFIED). Our wind layer plus shouts (Unrelenting Force, Whirlwind) and weather                                                        | **Later.** Shouts are a roadmap goal.                                                                                                              |

### 2.3 Outputs

| LIVE 2 capability                                                                              | Skyrim / CS equivalent to extend                                                                                                       | Verdict                                                                                                                                                |
| ---------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Water surface as LOD'd tiles displaced by height (M§2.8)                                       | Unified Water's water mesh is too sparse to move vertices (`reports/unified-water.md`)                                                 | **Normals: Have (D8).** **Real displacement: Research.** It would need our own tessellated or projected-grid surface; not before Phase 3 is stable.    |
| Caustics advected by the flow (M§2.8, M§12.5)                                                  | Water Effects already owns caustics (PS t65, `reports/unified-water.md`)                                                               | **Realistic.** Distort Water Effects' caustics lookup with our flow. Small, visible win.                                                               |
| Flow-mapped normal detail (M§4.10)                                                             | `Water.hlsl` flowmap helpers (`GetFlowmapNormal`)                                                                                      | **Have** (vanilla). Our flow can later steer it where the grid covers.                                                                                 |
| Parallax on density and height (M§4.10)                                                        | Water Parallax (`WaterParallax.hlsli`)                                                                                                 | **Extend.** Sample at the parallax-shifted position (D8).                                                                                              |
| Fog volumes (heterogeneous or fog-volume renderers) (M§2.9, M§11.1)                            | Exponential Height Fog's froxel fog. Injection point at `VolumetricFogMaterialCS.hlsl:12-16` (`reports/volumetric-lighting.md`).       | **Later** (R4). Extend Exponential Height Fog rather than writing a new renderer. It is still Unreleased and turns off under ENB.                      |
| Unlit fast smoke volume for low-end GPUs (M§4.7)                                               | A cheap quality preset for Phase 5 smoke                                                                                               | **Later** (R5)                                                                                                                                         |
| Volumetric clouds (M§2.9)                                                                      | Skyrim clouds are sky textures                                                                                                         | **No.** Out of scope.                                                                                                                                  |
| Particle advection by simulation velocity (experimental, M§2.10)                               | Rain and snow are drawn by the precipitation cube emitter. Pushing them with our wind or shouts in their shader could make snow swirl. | **Research.** Whether precipitation positions are shader-side is UNVERIFIED. High visual value if it works.                                            |
| Foliage bending from velocity (M§4.9, M§12.6)                                                  | Grass Collision's texture plus vanilla grass wind                                                                                      | **Later** (optional side track). Feed our velocity into the grass shader alongside Grass Collision. Trees use vanilla tree animation, which is harder. |
| Landscape footprints and tracks (Simple Painter, M§2.7)                                        | Snow Deformation (PR #2751 and fork)                                                                                                   | **No duplicate.** Offer our field as an input to Snow Deformation later, if its authors want it.                                                       |
| Wet map in the velocity/density buffer (M§4.9)                                                 | Wetness Effects' wetness and puddles                                                                                                   | **Realistic later.** Shores stay wet where waves ran up and splashes landed. A clear "extend CS" win after Phase 3.                                    |
| Drive any tagged material from the buffers (M§4.8)                                             | Bind the field's layers in one shared slot for any CS shader                                                                           | **Realistic** design rule for Phase 3+. Pick slots with `hlslkit-buffer-scan` to avoid register conflicts.                                             |
| Dynamic underwater effects (missing in LIVE, M§14.9)                                           | Vanilla underwater is fine                                                                                                             | **No**                                                                                                                                                 |
| Buoyancy (listed as a limitation, M§14.8; videos reportedly show floating objects, UNVERIFIED) | Phase 6 (R6, R7)                                                                                                                       | **Planned** (Phase 6). Visual bobbing as the fallback, as in the roadmap.                                                                              |

### 2.4 Tools and production

| LIVE 2 capability                            | Skyrim / CS equivalent                                                                                          | Verdict                                                                                         |
| -------------------------------------------- | --------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------- |
| Presets for the whole state (M§7.1)          | CS settings JSON, plus the weather system's per-weather variables (`RegisterWeatherVariables`, `src/Feature.h`) | **Realistic.** Per-weather fog density and wind strength come from an existing system for free. |
| Height cache baked from a capture (M§7.4)    | Not needed: landscape data is available in memory (R1)                                                          | **No**                                                                                          |
| Sequencer and Movie Render Queue (M§13)      | No equivalent                                                                                                   | **No**                                                                                          |
| Multiplayer replication (M§14.10)            | Not applicable                                                                                                  | **No**                                                                                          |
| Docs for LLM use, planned MCP tooling (M§17) | `docs/fluidfield/` plus `.claude/` already serve this role                                                      | **Have**                                                                                        |

## 3. Brainstorm: ideas worth adding to the plan

These are ranked by value for cost. Each builds on something that already
exists. None is decided; each needs a human call before it enters the
roadmap.

1.  **Warm start instead of a blank reset after loading screens.**
    -   Seed currents from Unified Water's flowmap.
    -   Pre-roll a few steps while the loading screen is still up.
    -   Water then never visibly "starts up" when you arrive.
    -   Fits the D8 slice.
2.  **Skip work when nothing needs simulating.**
    -   Use R1's surface layer to skip the water step when the grid has no
        water.
    -   Lower the update rate indoors.
    -   Directly serves the roadmap's "costs nothing when unused".
3.  **Caustics that follow the current.** A small change in Water Effects'
    caustics lookup, offset by our flow. Visible in shallow rivers, which is
    exactly where Fluid Field runs.
4.  **Weather wind as a global input.** The field's wind agrees with vanilla
    grass, trees and clouds, so shouts and the player add to the weather
    instead of fighting it.
5.  **Wet shores.**
    -   Track where water has run up or splashed.
    -   Feed that into Wetness Effects, so banks darken where waves reached.
    -   Comes after the wave generator works.
6.  **Shore waves from the bed slope.** LIVE's wave generator idea, applied
    to R1's bed next to vanilla water planes. Gentle lapping at lake and
    river banks.
7.  **Rain and snow swirled by wind and shouts.** Needs a short look at the
    precipitation shader first. High visual value, cost unknown.
8.  **One shared binding for the field's layers.**
    -   Any CS shader can sample velocity, height, density and wetness
        through one documented slot.
    -   Grass, precipitation, caustics and Wetness then need no new plumbing
        each.

## 3a. Thick liquids: blood and mud

LIVE 2 shows viscous liquids. Its manual has a "Viscous Fluids" section
(M§8.3), and 80.lv covered its realistic mud
([2026-09-28](https://80.lv/articles/fluidninja-live-2-for-ue5-showcases-realistic-mud-effects)).
Our first pass recorded viscosity as a parameter trick (lower divergence and
velocity feedback, more accumulation). The re-read did not return that text,
so the exact method is UNVERIFIED. It does not matter for us: we would derive
viscosity from public fluid literature, not from LIVE.

**What it would take in Skyrim.** Each part extends something that exists.

-   **Simulation.**
    -   A second liquid layer on the same grid: a thin film that flows
        downhill on R1's bed, with high friction and viscosity.
    -   It is the shallow-water solver with different constants, plus a
        per-texel "liquid type" (water, mud, blood).
    -   It covers dry ground, unlike the water layer, which follows vanilla
        water planes.
-   **Sources.**
    -   **Blood:** hit events on actors (Phase 2 inputs), plus vanilla blood
        impact data for colour and amount.
    -   **Mud:** wet, soft ground types (landscape textures from
        `TESObjectLAND`, as Terrain Helper reads them) churned by footsteps
        and hooves while it rains.
-   **Rendering.**
    -   Not the water shader: these liquids sit on terrain and statics.
    -   Wetness Effects already darkens surfaces and draws puddles in the
        lighting shader.
    -   Extend that path with colour, roughness and a thin-film height read
        from our layer. That gives blood pools that run downhill and mud
        smears around footsteps.
-   **Overlap with Snow Deformation.** Mud trails as deformation belong
    there. Fluid Field would only supply the flowing film on top.
-   **Cost.** Same order as the water layer, but it runs only near active
    sources: it sleeps when no blood or mud exists in the grid.
-   **Risks.**
    -   Vanilla blood decals already exist, so pools must complement them,
        not double them.
    -   Mod-heavy load orders change landscape textures, which affects mud
        classification.
    -   Lighting-shader edits need care, since every lit object uses that
        shader.

**Verdict: Later.** It fits after Phase 3, once the shallow-water solver and
the R1 bed are proven. It would be a new roadmap goal, so it needs a
decision before it is scheduled.

## 4. Where Fluid Field aims beyond LIVE

-   **Water depth:** a real shallow-water model on a real riverbed (R1, R2),
    against LIVE's 1 m "dense" layer.
-   **Obstacles from the physics engine:** Havok raycasts (D7 stage d), where
    LIVE ignores physics collision.
-   **Buoyancy with physics forces:** Phase 6 applies real forces through Havok. Whether LIVE does more than visual bobbing is UNVERIFIED (see section 1).
-   **Authored river flow:** Unified Water's flowmap for every river, instead
    of hand-placed splines.

## 5. Open points

-   Which effects Clair Obscur: Expedition 33 and Oblivion Remastered used
    LIVE for is not public (UNVERIFIED). It does not affect our plan.
-   The Fab listing could not be read (HTTP 403). Feature claims rely on the
    public manual.
-   Weather wind access through CommonLib, and whether precipitation positions
    are computed in the shader, are both unchecked. Both need a short look
    before ideas 4 and 7 can be scheduled.

## Sources

-   LIVE-2 manual (public): https://majorsmash.github.io/fnl-docs-v2/manual/ninjalive2-manual/
-   LIVE-2 releases page: https://majorsmash.github.io/fnl-docs-v2/releases/
-   itch.io demo: https://fluidninja.itch.io/fluidninja-live-2-demo
-   CG Channel, 2026-09-03: https://www.cgchannel.com/?p=178469
-   80.lv, 2026-09-28, LIVE-2 mud effects: https://80.lv/articles/fluidninja-live-2-for-ue5-showcases-realistic-mud-effects
-   80.lv, 2025-03-31, LIVE 2.0 pre-alpha: https://80.lv/articles/check-out-water-filling-gaps-in-ninjalive-2-0-pre-alpha-for-ue5
-   80.lv, 2022-07-07, LIVE 1.7 and VFX Tools: https://80.lv/articles/a-new-version-of-fluidninja-vfx-tools-for-unreal-engine
