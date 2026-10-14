# Wade In Water Redone: study for Fluid Field

Oct 14, 2026 · read-only report (Phase 2 input research)

Source: [doodlum/skyrim-wade-in-water](https://github.com/doodlum/skyrim-wade-in-water), `main` at `afab5ce` (2026-08-22). Abbreviated below as `WiW@afab5ce`. Our repo is cited as `CS@4e1addd` (`feature/fluid-field`). CommonLibSSE-NG paths are from our submodule (`extern/CommonLibSSE-NG`). Anything inferred rather than read in code is marked UNVERIFIED.

## Summary

Wade In Water Redone is tiny: 2 source files, about 110 lines of logic. It does **not** hook the submerged-level check, despite what `ROADMAP.md` "What to borrow" says about the original Loki mod. It **calls** the engine's `Actor::GetSubmergedLevel` (RelocationID 36452 / 37448) and **hooks one call site** that returns an actor's maximum movement speed (inside RelocationID 37013 / 37943). It scales that speed down by submersion and adds or removes one of three "slowdown" spells from the original `WadeInWater.esp`. It never looks up water height itself: it relies on the engine, which reads a per-reference cached water height.

For Fluid Field the useful parts are the engine call and its thresholds, not the code. `Actor::GetSubmergedLevel` is already in CommonLibSSE-NG (`Actor.h:593`), so Fluid Field can use it without any new address or hook. Fluid Field and Wade In Water do not conflict today.

## 1. What it does and how

### Hook

| What                                   | Where                                        | Detail                                                                                                                                                                                                                                                                                                                                                                                                    |
| -------------------------------------- | -------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Call-site hook (5-byte `call` rewrite) | `WiW@afab5ce:src/ActorUpdateHandler.h:46-49` | `stl::write_thunk_call<Actor_SetMaximumMovementSpeed>(REL::RelocationID(37013, 37943).address() + REL::Relocate(0x1A, 0x51))`. It uses a 14-byte trampoline (`WiW@afab5ce:include/PCH.h:47-54`).                                                                                                                                                                                                          |
| Thunk                                  | `WiW@afab5ce:src/ActorUpdateHandler.h:35-43` | `float thunk(RE::Actor*)`: calls the original, returns `original * GetWaterMultiplier(actor)`. The struct is named `SetMaximumMovementSpeed`, but the hooked callee takes an actor and returns a float, so it is more likely a "compute max speed" getter inside the function that sets it. Which engine function 37013 is, and what the callee is, is UNVERIFIED (not named in CommonLib; needs Ghidra). |
| Engine call (not hooked)               | `WiW@afab5ce:src/ActorUpdateHandler.h:17-27` | `GetSubmergedLevel_Impl(actor, zPos, cell)` via `REL::RelocationID(36452, 37448)` (comment: SE `1405E1510`), called with `actor->GetPositionZ()` and `actor->GetParentCell()`. The same ID is wrapped by CommonLib as `Actor::GetSubmergedLevel` (`CS@4e1addd:extern/CommonLibSSE-NG/src/RE/A/Actor.cpp:694-699`, `include/RE/A/Actor.h:593`).                                                            |
| Vtable hooks                           | none                                         | `write_vfunc` helpers exist in the PCH (`WiW@afab5ce:include/PCH.h:56-68`) but are unused.                                                                                                                                                                                                                                                                                                                |
| Plugin entry                           | `WiW@afab5ce:src/XSEPlugin.cpp:3-6, 36-51`   | Installs the hook directly in `SKSEPlugin_Load`. No messaging listener, no settings, no INI.                                                                                                                                                                                                                                                                                                              |

### What it computes

`GetWaterMultiplier` (`WiW@afab5ce:src/ActorUpdateHandler.cpp:3-50`):

-   `submergedLevel` is a fraction from 0 to 1 of the actor's height under water (UNVERIFIED for the engine function; CommonLib's reimplementation for references, `TESObjectREFR::GetSubmergeLevel`, computes `min((waterHeight - z) / GetHeight(), 1)`, `CS@4e1addd:extern/CommonLibSSE-NG/src/RE/T/TESObjectREFR.cpp:609-622`).
-   Bands (`cpp:21-47`):

| `submergedLevel` | Spell kept on the actor        | Speed multiplier                                           |
| ---------------- | ------------------------------ | ---------------------------------------------------------- |
| ≥ 0.69           | `WaterSlowdownSwim` (`0xD67`)  | `1 - swim.magnitude / 100`                                 |
| 0.4 to 0.69      | `WaterSlowdownLarge` (`0xD65`) | `1 - lerp(small, large, (level - 0.2) / (1 - 0.49)) / 100` |
| 0.2 to 0.4       | `WaterSlowdownSmall` (`0xD64`) | same formula as above                                      |
| < 0.2            | all three removed              | `1 - lerp(0, small, level * 5) / 100`                      |

-   The spells come from `WadeInWater.esp` (the original Loki plugin), looked up once (`cpp:5-16`). The Redone package ships `WadeInWaterRedone.esp`, which masters `Skyrim.esm` and `WadeInWater.esp` and overrides the `WaterSlowdown` MGEF (string dump of `WiW@afab5ce:package/WadeInWaterRedone.esp`; commit `9b0b7c8` "Changed MGEF to stop HP reduction bug").
-   The spell magnitudes are the tuning knobs. The speed multiplier is applied in the hook; the spells appear to exist for their visible effect and other side effects (UNVERIFIED).

### Thread and cost

-   Thread: whichever thread runs function 37013. Actor movement updates for NPCs may run on worker threads (UNVERIFIED; needs Ghidra or a thread-ID log as in `LEARNINGS.md` "Threading"). If so, `AddSpell` / `RemoveSpell` from there would be a thread-safety concern.
-   Per call, per actor: one engine `GetSubmergedLevel`, one or more `HasSpell`, and in the dry branch (`cpp:42-46`) **three unguarded `RemoveSpell` calls every time**. Dry actors are the common case, so this is the main cost. Not measured; how often 37013 runs per actor is UNVERIFIED.
-   Robustness issues (read in code, effect in game UNVERIFIED):
    -   If `WadeInWater.esp` is missing, the three spell pointers are null and `WaterSlowdownSwim->effects[0]` (`cpp:27, 34, 41, 46`) dereferences null. The plugin requires both ESPs.
    -   `dataHandler` and the spell pointers are lazily initialised function statics without synchronisation (`cpp:5-16`).
    -   No null check on `GetParentCell()` before passing it to the engine (`ActorUpdateHandler.h:26`); whether the engine tolerates null is UNVERIFIED.
    -   The interpolation parameter `(level - 0.2) / (1 - 0.49)` is shared by the 0.2 and 0.4 bands and does not reach 1 at 0.69. It looks like a tuning artefact, not a design.

## 2. How it gets the water surface height

It does not query any water system. The engine function does that internally. What the engine reads is visible in CommonLib's reimplementations of the reference-level helpers:

-   `TESObjectREFR::GetWaterHeight()` returns `loadedData->relevantWaterHeight` if it is not −inf, otherwise `parentCell->GetExteriorWaterHeight()` (`CS@4e1addd:extern/CommonLibSSE-NG/src/RE/T/TESObjectREFR.cpp:631-643`).
-   `LOADED_REF_DATA` holds `currentWaterType` (`TESWaterForm*`), `relevantWaterHeight` and `underwaterCount` (`CS@4e1addd:extern/CommonLibSSE-NG/include/RE/T/TESObjectREFR.h:82-91`). The engine keeps these up to date per loaded reference (how, and on which thread, UNVERIFIED).
-   `IsInWater()` is `GetWaterHeight() > GetPositionZ()` (`TESObjectREFR.cpp:857-860`); `IsPointSubmergedMoreThan` wraps `GetSubmergeLevel` (`:882-885`).
-   Whether the native `Actor::GetSubmergedLevel` (36452) uses exactly this logic is UNVERIFIED. Its signature (actor, z, cell) matches `TESObjectREFR::GetSubmergeLevel`.

### Compared with Fluid Field's `WaterSurfaces`

`CS@4e1addd:src/Features/FluidField/WaterSurfaces.cpp:133-199` walks `TESWaterSystem::waterObjects` under the system lock, takes each flat bound's centre Z, and falls back to per-cell water (`GetExteriorWaterHeight`) or the interior's `waterHeight`. `SampleHeight` (`:201-227`) takes the highest covering surface.

| Aspect             | Wade In Water (engine per-actor value)                                                      | Fluid Field `WaterSurfaces`                                                                                  | Note for us                                                                                                                                                      |
| ------------------ | ------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Scope              | One value per actor, at its position                                                        | A field: any (x, y) in the grid                                                                              | Different jobs. We need the field for the depth map; per-actor values are a cheap cross-check and the right source for actor inputs.                             |
| Which surface wins | The engine's "relevant" water for that reference                                            | Highest bound covering the point (`:203-211`, open question for R3)                                          | Where water objects overlap, we may disagree with the engine. Logging `actor->GetWaterHeight()` against `SampleHeight` at the player would measure this cheaply. |
| Freshness          | Updated by the engine as the actor moves (UNVERIFIED cadence)                               | Rebuilt every 30 updates (`WaterSurfaces.h:70`, `.cpp:123-131`)                                              | Up to 30 frames stale after water objects change (Unified Water swaps, cell attach). Acceptable for the map, worse for actor inputs.                             |
| Sloped water       | Presumably whatever water the engine assigns (UNVERIFIED)                                   | Bounds with half-height > 10 are skipped (`:96-97, 155-158`)                                                 | An actor in rapids may have an engine water height where our map has none. `LEARNINGS.md` notes no sloped bounds were seen on the test route.                    |
| Height value       | `relevantWaterHeight` (engine)                                                              | Bound centre Z, with the plane-derived value only used to log the difference (`:150-151, 241-243`)           | Same choice as `Skin::GetWaterHeight` (`CS@4e1addd:src/Features/Skin.cpp:294-329`). `Verify()` already checks this against `TESObjectCELL::GetWaterHeight`.      |
| Locking            | None needed by the mod; the engine reads its own cached value                               | `TESWaterSystem::lock` held while iterating (`:144-147`); terrain-cell lock on 1.7.99+ (`EngineGrid.h:8-16`) | We are stricter than both WiW and Skin. Side finding: `Skin::GetWaterHeight` iterates `waterObjects` **without** the lock (`Skin.cpp:298-325`).                  |
| Null checks        | Spell pointers and parent cell unchecked                                                    | Every pointer checked; unset cell heights rejected (`:98-99, 171, 183`)                                      | Nothing to learn from WiW here.                                                                                                                                  |
| Interiors          | `relevantWaterHeight` should work in interiors; `GetExteriorWaterHeight` returns −inf there | Reads `interior->GetRuntimeData().waterHeight` with `kHasWater` (`:169-174`)                                 | Interior handling is equivalent in intent. Whether `relevantWaterHeight` is set in interiors with placed water but no cell water is UNVERIFIED.                  |
| Water type         | Not used                                                                                    | Not read                                                                                                     | `loadedData->currentWaterType` gives the actor's `TESWaterForm`, a free hint for per-water-type tuning later (flow, foam). Not used by WiW.                      |

## 3. What Fluid Field can reuse or learn

### (a) Phase 2 actor inputs

-   **Use the engine value, not a new lookup.** For each tracked actor, `actor->GetWaterHeight()` (inline in CommonLib, no relocation) gives the water height; depth is `waterHeight - z`. `actor->GetSubmergedLevel(z, cell)` gives the 0 to 1 fraction WiW uses; it is already in CommonLib, so it adds no new address. Both are main-thread reads of per-actor data and should fit the "32 sources < 0.1 ms" gate easily (UNVERIFIED until measured).
-   **Wading detection:** WiW's bands give a ready tuning table: below 0.2 ankle-deep, 0.2 to 0.4 knee, 0.4 to 0.69 waist, 0.69 and up swimming. For "is swimming", prefer the engine state `ActorState::IsSwimming()` (`CS@4e1addd:extern/CommonLibSSE-NG/include/RE/A/ActorState.h:202`) over a threshold.
-   **Bones:** per-bone submersion is simply `boneZ < waterHeight` with the same per-actor water height, so bone inputs need no extra water queries.
-   **Which actors:** WiW has no selection; it runs for every actor that passes through function 37013. Keep Grass Collision's candidate list (`highActorHandles` plus player, distance-sorted; `reports/grass-collision.md` §2.2) and add the submersion test as a cheap filter: actors with no water at their position contribute nothing to the water layer.
-   **Cross-check:** compare `actor->GetWaterHeight()` with `WaterSurfaces::SampleHeight` at the player each rebuild and add it to `VerifyStats`. This catches overlap-rule and staleness errors in the map.

### (b) Wake strength by submersion

WiW only scales speed, but its curve suggests a wake profile (inference, to tune in game): impulse ∝ horizontal speed × f(level), with f rising from 0 at dry to a peak around waist depth (0.4 to 0.6) and dropping when swimming, where only the head and shoulders cut the surface. The actual water-plane cross-section is what matters: at the surface, the impulse source should be the bones or collision spheres that straddle `waterHeight` (Grass Collision already gives the spheres, `Util::GetShapeBound`). Note that WiW also changes the actor's speed, so with WiW installed actors move more slowly in water and Fluid Field's velocity-based wakes weaken automatically. That is correct behaviour and needs no special handling.

### (c) Buoyancy and submersion in Phase 6

-   `GetSubmergedLevel` is for actors. For loose references, CommonLib's `TESObjectREFR::GetSubmergeLevel` (pure C++, `TESObjectREFR.cpp:609-622`) does the same with `GetHeight()`. Both use a flat water plane and a bounding height, which is too coarse for bobbing. Phase 6 needs the simulated surface (readback, R6) against the body's actual shape.
-   The useful lesson is architectural: WiW changes behaviour by multiplying an engine result at one call site, which is cheap and composes with other mods. If R7 finds the vanilla Havok buoyancy path, the same pattern (hook the call that fetches water height for buoyancy and add the simulated offset) may be safer than applying our own forces. UNVERIFIED; needs R7.

### Conflicts

-   **No shared hooks today.** Community Shaders hooks neither 36452 / 37448 nor 37013 / 37943 (grep of `src/` found no use of either ID; CommonLib only wraps 36452 as a callable). Fluid Field's plan reads data in `Prepass` and does not hook actor updates. Both can be installed together.
-   **Behavioural overlap:** none visible. WiW changes speed and spells; Fluid Field changes visuals. The vanilla wading ripples that D8 keeps are independent of WiW.

### Feeding simulated water height back

-   **To WiW (and other callers of 36452):** WiW calls the engine function through its address (`ActorUpdateHandler.h:20`). A detour on the **entry** of 36452 would therefore change WiW's input with no cooperation from WiW; a call-site hook would not. That detour would also change every engine caller (swimming state, possibly AI and breathing; UNVERIFIED which), so it is a gameplay change, not a visual one.
-   **Alternative:** write `loadedData->relevantWaterHeight` per actor. The engine probably rewrites it each update (UNVERIFIED), so this would race with the engine.
-   **Risks:** the simulated height is on the GPU and reaches the CPU only through Phase 6's readback, a few frames late. Wave crests near the swim threshold (0.69 for WiW, and whatever the engine uses) could toggle swim state and spells every few frames. If ever done, clamp the offset (for example ±10 units), apply it only while wading (below the swim threshold), and keep it behind its own setting.
-   **Recommendation (inference):** out of scope until Phase 6. Ghidra work on 36452 (what it reads, who calls it) should be part of R7.

## 4. Licence, runtime support, last update

-   **Licence:** two files disagree.
    -   `WiW@afab5ce:LICENSE` is MIT, "Copyright (c) 2022 Tim", from the initial commit `30b081b`. GitHub's API reports the repository as MIT.
    -   Commit `d1da317` (2026-08-22) added `COPYING` (GPL-3.0) and `EXCEPTIONS.md` (Modding Exception and GPL-3.0 Linking Exception, the same pair Community Shaders uses), and `vcpkg.json` now says `"license": "GPL-3.0-or-later"` (`WiW@afab5ce:vcpkg.json:8`).
    -   Either licence is compatible with Community Shaders' GPL-3.0-or-later. In any case there is little code worth copying: Fluid Field would call `Actor::GetSubmergedLevel` from CommonLib, not WiW's code. Credit WiW for the thresholds if they are reused.
-   **Runtime:**
    -   Built on CommonLibSSE-NG (submodule `8b48fb1`; the `support/skyrim-1.7.99` branch pins v6.7.0 at `3226c2e`, 2026-08-24) with SE, AE and VR enabled (`WiW@afab5ce:cmake/XSEPlugin.cmake:1-6`). Commit `d1da317` is titled "Add Skyrim 1.7.99 support".
    -   Addresses are SE/AE `RelocationID` pairs with SE/AE offsets only. VR would resolve through the SE ID and SE offset; whether that is valid in VR is UNVERIFIED. `Build Release.bat` builds only the `skyrim` preset; a `vr` preset exists (`CMakePresets.json`).
    -   Whether AE IDs 37448 and 37943 exist in the 1.7.104 Address Library is UNVERIFIED (CommonLib's own use of 37448 suggests 36452 at least is mapped).
    -   `SKSEPlugin_Version` declares `UsesAddressLibrary()` and `UsesUpdatedStructs()` (`WiW@afab5ce:src/XSEPlugin.cpp:53-60`).
-   **Last update:** `main` 2026-08-22 (`afab5ce`), branch `support/skyrim-1.7.99` 2026-08-24. No GitHub releases; distribution is Nexus SSE 71418 (`research/existing-mods.md:90`). Before August 2026 the last change was 2022-10-26 (`3a908f5`, "1.6.629+ support").

## 5. Open questions

1. What are functions 37013 / 37943 and the callee at +0x1A / +0x51? Is it called once per actor per frame, and on which thread? (Ghidra with the annotated archives.)
2. Does native `Actor::GetSubmergedLevel` (36452) read `loadedData->relevantWaterHeight` as CommonLib's reference version does, and what calls it (swim state, breathing, AI)? This decides whether a feedback detour is feasible (R7).
3. Where and how often does the engine update `relevantWaterHeight`, and does it handle sloped water and interior placed water? A log of `actor->GetWaterHeight()` against `WaterSurfaces::SampleHeight` on the test routes would answer the practical part.
4. At what submersion does vanilla switch to swimming? WiW's 0.69 might be chosen to match it (UNVERIFIED).
5. Is the WiW licence MIT or GPL-3.0-or-later going forward? It does not block us, but it decides the credit wording.
6. Should `Skin::GetWaterHeight` take `TESWaterSystem::lock` like `WaterSurfaces` does? Outside Fluid Field's scope; worth a separate upstream issue.
7. Does WiW work on VR with SE offsets, and on 1.7.104? Relevant only for the compatibility matrix.
