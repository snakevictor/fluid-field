# Fire and smoke: what exists, and what Fluid Field can add

Research note, 2026-10-13. Not a numbered spike; it feeds R4, R5 and Phase 5
(`docs/fluidfield/ROADMAP.md:80-82, :99`). Paths relative to repo root;
CommonLib = `extern/CommonLibSSE-NG/include/RE`. UNVERIFIED = inference not
checked in game data or RenderDoc.

## 1. How vanilla draws fire and smoke

### 1.1 Three render paths

| Path                                                                                                                                                                                                                 | Used for                                                                                                                                                                                                       | Simulated where                                                                                                                                                                                                    | Evidence                                                                                                                                                                                                                                                                                                                                                                                                              |
| -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **BSEffectShader** (`package/Shaders/Effect.hlsl`) on `NiParticleSystem` / `BSStripParticleSystem` geometry (`PARTICLES`, `STRIP_PARTICLES` permutations) and on plain meshes (fire cards, smoke cards, "fog" cards) | Campfire, forge, torch flames, chimney and wisp smoke, spell hand FX, impact FX, explosions, dragon breath                                                                                                     | **CPU**: NiPSys modifier stack updates `NiPSysData` each frame; positions, sizes, colors and atlas indices are uploaded as vertex data. The VS only transforms vertices, so it has no particle age or emitter info | `NiParticleSystem.h:21-28` (modifierList, isWorldspace), `NiPSysModifier.h:20-32` (ORDER: kEmitter, kForce, kPosUpdate...), `:38` (`Update(...)`), `NiPSysData.h:12-13,32` (per-particle velocity, age), `NiParticlesData.h:25-28` (positions, color, radii, sizes); Effect VS input has only position, UV, normal, color (`Effect.hlsl:22-46`); atlas lookup `IndexedTexCoord[128]` (`Effect.hlsl:131-134, 247-281`) |
| **BSParticleShader** (`package/Shaders/Particle.hlsl`)                                                                                                                                                               | Weather rain and snow (`ENVCUBE`, cube emitter). Non-ENVCUBE path: object and geometry emitters, which are TESEffectShader (EFSH) particle effects such as burning-actor embers (UNVERIFIED for the fire case) | **GPU, analytic**: the VS computes position from birth data and age: `p0 + (v + wind)*t + 0.5*a*t^2`                                                                                                               | `Particle.hlsl:43-61` (CB with `Velocity`, `Acceleration`, `Wind`), `:120-163` (closed-form position; `tmp2 = Normal.w * Position.w` is the age); `BSParticleShaderProperty.h:31-82` (lifetime, velocities, accelerations, emitters); headers `BSParticleShaderObjectEmitter.h`, `BSParticleShaderCubeEmitter.h`, `BSParticleShaderRainEmitter.h`, `BSParticleShaderSnowEmitter.h`                                    |
| **BSLightingShader**                                                                                                                                                                                                 | Logs, coals, brazier meshes, glowmaps (embers). Not smoke                                                                                                                                                      | n/a                                                                                                                                                                                                                | `Lighting.hlsl`                                                                                                                                                                                                                                                                                                                                                                                                       |

`PerformanceOverlay.cpp:130` describes the Particle shader type as "smoke,
sparks". That is a UI string, not evidence of what vanilla content uses.

### 1.2 Wind already reaches CPU particles

-   `BSWindModifier : NiPSysModifier` (`B/BSWindModifier.h:7-30`):
    `Update()` at vtable slot 0x25 (`:25`), a single `strength` (`:30`),
    `Create(name, strength)` (`:27`). VTABLE `Offsets_VTABLE.h:5793`.
-   nif.xml describes it as "Particle Modifier that uses the wind value from
    the gamedata to alter the path of particles", with default strength 0.25
    (https://raw.githubusercontent.com/niftools/nifxml/develop/nif.xml,
    line ~3957).
-   `NiPSysGravityModifier` (`N/NiPSysGravityModifier.h:27-34`): gravity
    object or axis, strength, **turbulence**, scale. Fire and smoke NIFs
    usually rise through a negative-gravity or planar modifier (UNVERIFIED
    which vanilla NIFs).
-   `NiParticleSystem::AddModifier` is a relocated function
    (`NiParticleSystem.h:55-60`, `RELOCATION_ID(72799, 74499)`). A modifier
    can be injected into a loaded system at runtime.
-   Which vanilla fire and smoke NIFs (for example `effects\fxfire*.nif`,
    `fxsmoke*`) already carry `BSWindModifier` is **UNVERIFIED**. Check the
    BSAs with NifSkope.

### 1.3 Blending, soft particles, lighting (vanilla + CS)

-   **Soft particles** (vanilla `SOFT` permutation): the shader reads scene
    depth at t3 and fades alpha with `LightingInfluence.y`, from the
    material's `softFalloffDepth` (`Effect.hlsl:653-656`;
    `BSEffectShaderMaterial.h:36`).
-   **Blend modes**: `ADDBLEND` (fire, glows), `MULTBLEND` and
    `MULTBLEND_DECAL`, and plain alpha blending (smoke)
    (`Effect.hlsl:843-875`).
-   **Fire colour**: comes from a greyscale palette. `GrayscaleToColor` and
    `GrayscaleToAlpha` look up `TexGrayscaleSampler` with the base texture's
    G and A channels (`Effect.hlsl:787-798`; flags in
    `Permutation.hlsli:63-69`). Base texture sample: `Effect.hlsl:729`.
-   **"Volumetric fog" cards**: vanilla soft, falloff, vertex-colour mesh
    cards get the `IS_VOLUMETRIC_FOG` define (`Effect.hlsl:12-17`). CS gives
    them an 8-tap view-ray sun-shadow march (`GetLightingShadow`,
    `Effect.hlsl:560-605`, applied at `:807-810`).
-   **Effect-lit particles** (`LIGHTING`, i.e. `kEffectLighting`): vanilla
    uses 4 point lights. In-world, CS swaps these for LLF clustered lights
    and adds a directional shadow (`Get3DFilteredShadow`), Skylighting
    diffuse and EHF sun attenuation (`Effect.hlsl:476-558, 679-716`).
-   **Refraction**: `kRefraction`, `kRefractionFalloff` and
    `kTempRefraction` are shader-property flags (`B/BSShaderProperty.h:56,
69-70`). Objects write to `kREFRACTION_NORMALS`
    (`BSShaderRenderTargets.h:23`), and the `ISRefraction` pass composites
    the result (`ISRefraction.hlsl:26-50`). Which vanilla fire FX use
    refraction for heat haze is UNVERIFIED.

### 1.4 Finding fire and smoke emitters near the player (CommonLib)

Ranked by cost and robustness.

1.  **Reuse the render-pass classification that CS already has**
    (cheapest).
    -   LLF particle lights inspect every effect render pass through the
        `BSBatchRenderer::RenderPassImmediately` call hooks
        (`LightLimitFix.cpp:1137-1141`, installed at `:1969-1971`).
    -   Filter (`:1061-1080`): `kSoftEffect | kZBufferTest`, alpha flags
        `4109` (additive), a parent of type `NiBillboardNode`, and a
        texture stem matching `Data\ParticleLights\<stem>.ini`
        (`:930-1020`).
    -   It produces `{position, color, radius}` per fire billboard, every
        frame (`LightLimitFix.h:22-27`, `:1022-1059`).
    -   Limitation: it only covers textures that have a ParticleLights
        config (mod-supplied). It runs on the render thread, one frame
        late.
2.  **Mirror the shader's `isFire` heuristic on the CPU.** - `Effect.hlsl:662-676` treats as fire: `ADDBLEND + SOFT +
GrayscaleToColor + GrayscaleToAlpha`, or `ADDBLEND + PARTICLES +
TEXCOORD_INDEX + INDEXED_TEXTURE`. - The same test can run in the existing Effect `SetupGeometry` hook,
    `EffectExtensions::BSEffectShader_SetupGeometry`
    (`Hooks.cpp:241-249`). That hook already writes per-draw
    `permutationData` (`EffectRadius` comes from
    `geometry->worldBound.radius`, `:248`). - Smoke would be the alpha-blended (non-additive) soft particles with
    a greyscale palette. That heuristic is UNVERIFIED.
3.  **Lights.**
    -   `TESObjectLIGH` flags `kFlicker`, `kFlickerSlow`, `kPulse`
        (`T/TESObjectLIGH.h:25-31`); `GetNoFlicker()` (`:104`).
    -   Placed fires nearly always come with a flickering LIGH. LLF already
        walks the active light list every frame.
    -   This marks fires without saying which FX they use.
4.  **Gameplay sources** (Phase 2 input list).
    -   Projectiles: `Projectile::Manager` arrays (`P/Projectile.h:49-59`).
        Types `IsFlamethrower`, `IsCone`, `IsMissile`
        (`B/BGSProjectile.h:34-40, 106-111`). Flames and dragon breath are
        Flamethrower type (UNVERIFIED for the breath). Fireball is a Missile
        with an explosion.
    -   Magic: `EffectSetting.data.resistVariable == ActorValue::kResistFire`
        (`E/EffectSetting.h:73`; `A/ActorValues.h:49`), plus its
        `light`, `effectShader`, `projectileBase`, `explosion`,
        `castingArt` and `hitEffectArt` (`:77-103`).
    -   Explosions: `Explosion` refs (`E/Explosion.h:23`; runtime `age`,
        `lifetime`, `radius` at `:70-73`); `BGSExplosionData.light`,
        `radius` (`B/BGSExplosion.h:28-45`).
    -   Hazards: `BGSHazard`, `Hazard` (fire runes, Firestorm residue).
    -   Cloaks and burning: `ProcessLists::magicEffects` temp effects
        (`P/ProcessLists.h:44, 80-81`; `ShaderReferenceEffect`,
        `ModelReferenceEffect`).
5.  **Scene-graph scan.**
    -   Walk the loaded 3D of nearby refs for `NiParticleSystem` nodes
        whose effect material texture matches a fire or smoke pattern.
    -   Do it once per cell attach (`TESCellAttachDetachEvent` sink exists,
        `Offsets_RTTI.h:4721`) and cache the result. Never scan per frame.
    -   Model-path matching (`effects\fx*fire*`, `*smoke*`) is fragile
        against FX replacers (UNVERIFIED naming).

## 2. What CS already does to these

| Area                    | What it does                                                                                                                                                                                                                                                                                                               | Cite                                                                                                                                      |
| ----------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| Effect shader           | Full replacement of vanilla Effect.hlsl. LLF clustered lights, sun shadow, Skylighting, IBL, EHF fog for every blend mode, ENB-style fire and particle curves                                                                                                                                                              | `Effect.hlsl:476-558, 662-716, 814-887`                                                                                                   |
| Per-draw hook           | Effect `SetupGeometry` vfunc 0x6: ExternalEmittance, aurora flag, `EffectRadius`                                                                                                                                                                                                                                           | `Hooks.cpp:241-249, :1113`; `State.cpp:93-113`                                                                                            |
| Particle shader         | LLF lights with local shadows, sun shadow, IBL ambient; Effects11 raindrops                                                                                                                                                                                                                                                | `Particle.hlsl:340-393, :271-310`                                                                                                         |
| LLF particle lights     | Turns fire billboards into point lights (INI per texture, optional cull)                                                                                                                                                                                                                                                   | `LightLimitFix.cpp:880-1135`; UI `:94`                                                                                                    |
| Effects11               | CS's built-in **ENBSeries-compatible .fx host**. It reads `enbseries.ini` and ports ENB's in-shader tweaks: `[FIRE] Intensity/Curve`, `[PARTICLE]` intensity and lighting influence, `[LIGHTSPRITE]`, rain, its own volumetric rays                                                                                        | `Effects11.h:13-31` (PerFrame CB from `:35`); `Effects11.cpp:175-199`; `EffectManager.cpp:212-215`; shader `Effect.hlsl:662-676, 843-855` |
| Extended Translucency   | Fabric and thin-material transmission. **Lighting shader only**; it does nothing for FX                                                                                                                                                                                                                                    | `ExtendedTranslucency.h:15-23`                                                                                                            |
| Effect No-Decals fix    | Sets `kNoDecals` on `kSoftEffect` geometry at NIF load                                                                                                                                                                                                                                                                     | `src/EngineFixes/EffectShaderNoDecalsFix.h:1-25`                                                                                          |
| EHF volumetric fog      | Froxel fog. Injection point `VolumetricFogMaterialCS.hlsl:5-19` (writes `VBufferA = scattering, extinction`). Defaults: 16 px froxels, 64 slices, near grid 8000 units (`ExponentialHeightFog.h:71-72, 88`; clamps `.cpp:301-331`). Off under Effects11 (`.cpp:106-110`). `Unreleased = True` (`ExponentialHeightFog.ini`) | see `reports/volumetric-lighting.md` §5.2                                                                                                 |
| Volumetric Shadows      | Sun VSM at PS t18 for particles and effects; `SampleVSMCascade3D`                                                                                                                                                                                                                                                          | `reports/volumetric-lighting.md:131-133`                                                                                                  |
| "smoke" / "fire" in src | Only Effects11 FIRE settings and Fluid Field's own strings. No smoke system exists                                                                                                                                                                                                                                         | git grep (`Effects11.cpp:198-199`, `FluidField.h:4,19-21`)                                                                                |

## 3. Fluid Field increments, ranked

The field today (`ff/grid-core`) is a 2D stable-fluids wind layer. It uses
16 world units per texel by default and supports at most 16 emitters per
step. Its velocity texture holds world units per second (`git show
ff/grid-core:src/Features/FluidField/WindSolver.h` lines 17-34, 55, 101).

### 0. Enabler: a fire/smoke emitter list (Realistic, do first)

-   Combine the LLF particle-light queue (1.4.1), the CPU `isFire`
    classifier in the Effect `SetupGeometry` hook (1.4.2), flickering
    lights (1.4.3) and Phase 2 gameplay sources (1.4.4).
-   Output: a capped list of `{pos, radius, heat, smokiness}` that feeds
    every item below.
-   Needs a small read-only accessor on LLF's particle lights (with
    maintainer approval, like the UnifiedWater accessor in D8).
-   Cost: negligible.

### a. Wind bending existing particle systems

| Option                                                                                                                       | Where                                                                                                                   | Feasibility                                                                                                                                                                                                                                                                                                                                                                |
| ---------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **a1. Hook `BSWindModifier::Update`** (vfunc 0x25)                                                                           | CPU, per particle. Replace the global weather wind with wind sampled at `positions[i]`, through a CPU copy of the field | Strong fit with "extend vanilla": NIFs that already opt into wind get local wind and shout gusts. Needs a CPU view of the field. Option 1: an analytic mirror (weather wind plus the decaying Phase 2 impulses, which are already on the CPU). Option 2: a low-res readback a few frames late (shares R6). Cost: µs per system. Works only for NIFs that have the modifier |
| **a2. Inject a `BSWindModifier`** into classified fire/smoke systems that lack one (`AddModifier`, `BSWindModifier::Create`) | CPU, at 3D load                                                                                                         | Broadens a1. Risks: modifier order, local-space systems (`isWorldspace` false) need the wind rotated into node space, and clones and pooling. **Later**                                                                                                                                                                                                                    |
| **a3. VS offset in `Effect.hlsl`**                                                                                           | GPU                                                                                                                     | The VS has no age or emitter origin, so the only option is "lean ∝ height above World origin × wind". Crude, shears quads, and needs matching `PreviousWorld` offsets for motion vectors. Fallback only                                                                                                                                                                    |
| **a4. VS offset in `Particle.hlsl`**                                                                                         | GPU                                                                                                                     | Age is known (`tmp2`), so adding `fieldWind(p0) * age` integrates correctly. Covers rain and snow (FluidNinja idea 7) and EFSH particles. **Realistic** once the field SRV is bound to the VS                                                                                                                                                                              |

**Verdict.** a1 is **Realistic** (after Phase 2 inputs). a4 is
**Realistic**. a2 is **Later**. a3 is **No**, except as a stopgap.

**Risks.**

-   FX replacers (Embers XD and similar; UNVERIFIED names) may ship NIFs
    without the modifier.
-   ENB does not touch the CPU path, so a1 and a2 are ENB-safe.

### b. Flow-distorted flames and smoke in the Effect shader

-   **What it does.** Offset the base-texture UV (`Effect.hlsl:729`) by the
    field's velocity, projected to screen or quad space, plus curl noise.
    Use two-phase flow-map blending to avoid stretching. The palette lookup
    (`:787-798`) then recolours the distorted shape.
-   **Gating.** Use a new `ExtraFlags` bit set per draw in
    `Hooks.cpp:241-249` from the 1.4.2 classifier, so only fire and smoke
    draws pay.
-   **Binding.** Bind a field SRV to the PS (check registers with
    `hlslkit-buffer-scan`). This matches FluidNinja idea 8, one shared
    binding.
-   **Cost.** One extra fetch plus a few ALU per fire pixel. Fire overdraw
    is high but covers little of the screen. Estimated under 0.1 ms
    (UNVERIFIED).
-   **Risks.**
    -   Atlas particles (`TEXCOORD_INDEX`) bleed across cells. Clamp the
        offset to the `IndexedTexCoord` cell.
    -   ENB's own fire curves sit downstream and are unaffected.
    -   Effects11 already branches on `isFire`.
    -   Changing a core shader needs maintainer buy-in.
-   **Verdict: Realistic.** It is the cheapest visible win and does not
    need the 3D smoke.

### c. Local 3D smoke volumes (Phase 5)

-   **Simulation.** Small 3D stable-fluid grids per top-N emitters (from
    item 0), for example 32×32×64 voxels at about 8 to 16 cm. Their
    boundary velocity comes from the 2D wind (bottom and sides). Buoyancy
    comes from the emitter heat.
-   **Render option c1: EHF froxels.** Sample the smoke volumes (world-space
    3D SRVs, or an atlas) in `VolumetricFogMaterialCS` and add to
    extinction and scattering.
    -   Benefit: lighting (sun, moon, IBL, LLF clustered lights, LLF local
        shadow cache, engine local shadows), temporal filtering, and
        **correct sorting with fire sprites and water** for free.
    -   Problem: resolution. 16 px froxels × 64 exponential slices smear
        curls into haze at more than a few metres. Fine for room haze and
        hearth smoke, weak for crisp plumes.
    -   Coupling: EHF is `Unreleased` and off under Effects11.
-   **Render option c2: own raymarch per volume box** (half-res plus
    temporal).
    -   Crisp plumes and true self-shadowing: a transmittance volume toward
        the sun via VSM `SampleVSMCascade3D`, and toward the 1-3 nearest
        LLF lights.
    -   Cost on the order of FluidNinja's 2 ms for 512×512×128 with self
        shadows. Must be much smaller here.
    -   Composites after transparents, which breaks sorting with fire
        sprites inside the smoke unless the depth is split.
-   **Recommendation.**
    -   c1 first, with high-detail density (sim plus flow noise), since it
        matches ROADMAP Phase 5 "drawn by the fog renderer".
    -   c2 only as a near-camera quality tier.
    -   Fallback: FluidNinja's cheap mode, smoke sim mapped on
        camera-facing translucent cards, which could be the existing
        vanilla smoke card textures, flow-distorted (b).
-   **Verdict.** c1 is **Later** (gated on R4 choosing EHF). c2 is
    **Research** (R5).

### d. Heat shimmer and refraction

-   **What it does.** Generate distortion normals around hot emitters (item
    0), advected by field velocity. Write them into `kREFRACTION_NORMALS`
    and let vanilla `ISRefraction` composite the result
    (`ISRefraction.hlsl:26-50`). That extends vanilla. Upscaling already
    carries this RT (`Upscaling.cpp:1511-1605`).
-   **Implementation.** Either a screen-space pass of soft spheres or cones
    per emitter, or a refraction-flagged proxy mesh.
-   **Cost.** Under 0.1 ms.
-   **Risks.**
    -   Does `ISRefraction` run when no vanilla refraction geometry is
        visible? (UNVERIFIED; RenderDoc.)
    -   Ordering against transparents.
    -   Shimmer through the player's own weapon.
-   **Verdict: Later.** It is cheap, but needs the emitter list and a
    RenderDoc check.

### e. Smoke pooling under ceilings indoors

-   **Needs.** A ceiling-height map, i.e. upward raycasts. D7 stage d
    plans only downward rays under `bhkWorld::worldLock` with a ray cap.
    Interiors may need both directions, or the room bounds from the cell.
-   **Approach.** The 3D volumes from c get a solid top boundary. A cheap
    variant adds a 2D "ceiling smoke" layer that spreads density along the
    ceiling height map and renders through c1.
-   **Cost.** Smaller grids indoors.
-   **Risks.**
    -   Interior sun handling (`InteriorSun` feature).
    -   Mods that change interiors.
    -   EHF and Effects11 suppression.
-   **Verdict: Research.** Do it after R1 stage d. It is a Phase 5 gate
    item (`ROADMAP.md:82`).

## 4. FluidNinja LIVE 2: public descriptions (manual, fetched 2026-10-13)

Source: https://majorsmash.github.io/fnl-docs-v2/manual/ninjalive2-manual/
(section numbers M§x refer to the manual's headings).

### Camera-facing sims (M§2.0, §4.5 "Setup E")

-   The manual's mode list says: "Camera facing mode - visualization
    turning towards the player: flames, smoke, explosions".
-   The sim plane aligns with the camera, but its buffers stay
    "pinned to World Space", with inertia from the sim area's motion.
-   The typical use is "a handheld torch in the air - as the player moves.
    FREQUENTLY USED!"
-   Example setups include "a flaming aura, sim attached to the moving
    player, using an internal camera-facing volume to render flames and
    smoke".

### Internal renderers (M§4.7)

Mesh, HVOL, FVOL and SVOL, "for small scale, local effects, like: a
handheld torch, ... a bonfire with smoke".

### Fake-3D tricks (M§4.10)

-   Detail flow mapping: sim velocity advects static noise, and sim
    density is the mask.
-   Parallax occlusion mapping on inverted density.
-   "2D Raymarching" on raw sim density toward the main light (fake
    self-shadow).

### Volumetrics (M§11)

-   The 2D sim drives the volume as follows:
    -   Sim density acts as a **height field**: higher density gives a
        thicker volume.
    -   Terrain elevation can modulate it.
    -   Velocity advects 3D noise, which is subtracted from the density as
        flow detail.
-   Volumes are surface-aligned or camera-facing.
-   Generic volume function features:
    -   "extruded and cross-section based 3D density"
    -   velocity flow mapping
    -   fades
    -   "emissive from volume density, using blackbody-temperature
        function"

| Type                                                                   | Manual's description                                                                                                                           |
| ---------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| FVOL (UE fog volume, needs Exponential Height Fog with volumetric fog) | "Low performance cost, low resolution, low responsivity (high latency) ... no self-shadows, supports multiple lightsources"                    |
| CVOL (cloud volume)                                                    | Medium cost, self-shadows, directional light                                                                                                   |
| HVOL (UE Heterogeneous Volume)                                         | "High performance cost, high resolution, high responsivity ... supports self-shadows and multiple lightsources"                                |
| SVOL ("Smoke Volume", custom ninja tech predating UE HVOL)             | "Similar to HVOL, but supports UNLIT mode - ideal to deliver volumetrics on low-end hardware! Limitation: supports only a single light source" |

### Published costs (M§12, RTX 3080)

-   A camera-facing bonfire smoke setup:
    -   512×512 sim: **0.2 ms**
    -   512×512×128 HVOL render with self-shadows: **2 ms**
-   Cheap alternative: "mapping the sim on a camera facing plane with
    translucent material (instead of using volumetrics)" runs 4 sim
    instances at 300 FPS on a GTX 1070.
-   Water example: 2048² sim 1.2 ms, rendering 3 ms.
-   Rule: visualization, not simulation, sets the cost.
-   Self-shadowed HVOL and CVOL "tank performance". FVOL without
    self-shadows is "highly optimal".

### Sleep and LOD (M§12.3–12.4)

-   Proximity activation volume.
-   `PauseSimWhenNotVisible` ("a torch mounted on a wall - paused if the
    player looks away").
-   Fewer sim cycles with distance.

### Particles (M§2.1, §14)

-   No internal particle rendering. Sparks for fire come from an external
    Niagara system.
-   "particle rendering is in the prototype phase".

### Mapping to Skyrim

-   FVOL corresponds to EHF froxels (c1). HVOL and SVOL correspond to c2.
-   "Camera-facing translucent plane" corresponds to the vanilla smoke and
    fire cards plus flow distortion (b).
-   Blackbody emissive corresponds to the vanilla greyscale-palette fire
    (`GrayscaleToColor`), which already exists.
