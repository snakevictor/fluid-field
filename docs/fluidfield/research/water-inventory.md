# Water inventory: what Fluid Field covers, and the gaps

Date: 2026-10-26. Brief: section 4 of `D:\Fluid Field Pre-Ship Notes 2026-10-26.md`.
Status: study only. No code was changed. To be reviewed with Victor before any fixes.

## How this was made

-   **Game data.** Skyrim.esm, Update.esm, Dawnguard.esm, HearthFires.esm and Dragonborn.esm, read with small Python parsers. They read WATR, CELL, WRLD and REFR records, plus every base object whose model path looks like water. NIF headers were read from `Skyrim - Meshes0/1.bsa` to count lit (`L`), effect (`E`) and particle (`P`) blocks per model.
-   **Parsers.** They live in the session scratchpad, not the repo: `C:\Users\victa\AppData\Local\Temp\claude\C--dev\d607eb62-cbde-467b-ada7-a91a434086b8\scratchpad\inventory\`. They are `bsa.py`, `esm.py` (writes `inventory.pkl`), `report.py`, `usage.py`, `flowcells.py`, `classify.py` (applies our path filters to every placed model), `places.py` (finds places) and `hiddencover.py`.
-   **Ref counts.** These are placed references across the five masters, deleted ones excluded. `INT` means interior cells.
-   **Flowmap flag.** CommonLibSSE's `TESWaterForm::Flag::kEnableFlowmap` is bit 3 (`1 << 3`) of the WATR `FNAM` byte. The bit after it, `kBlendNormals`, is bit 4.
-   **Positions.** These are the REFR `DATA` positions. Exterior grid = floor(pos / 4096).
-   **Code refs.** These are on `feature/fluid-field` as of today. `FS` = `src/Features/FluidField/FallSites.cpp`. `FF` = `src/Features/FluidField.cpp`. Shaders are in `features/Fluid Field/Shaders/FluidField/`.

## The filters, in one place

| Filter                              | Where                                  | Matches (lowercase substring)                                                                                                                                                                                        | Effect                                                                                                                                                                                          |
| ----------------------------------- | -------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Classify`                          | FS:279-301                             | Needs `effects\`; skips `salmon`. Body: `waterfallbody`/`waterfallthin`/`waterfalltall`/`waterwall`. Skirt: `waterfallskirt`. Rapids: `rapids`. Splash: `splash`/`churn`. Mist: (`waterfall`+`mist`) or `mistblast`. | Collected into fall sites. Every collected ref is hidden by `RefreshHiddenEffects` (FS:415-449).                                                                                                |
| Hiding, non-Body                    | FS:424-431 (D81)                       | All Classify classes except Body                                                                                                                                                                                     | Every geometry without a lit shader is app-culled.                                                                                                                                              |
| Hiding, Body                        | FS:432-440, `LayerOf` FS:402-413       | Body                                                                                                                                                                                                                 | Hides any layer not in `FallLayerMask`. The default is `0x1`, the lit inner surface only (`FluidField.h:93`). The user's settings also have 1. Effect sheets, streaks and particles are hidden. |
| Lit / effect split                  | FS:811-831                             | Body geometry with `BSLightingShaderProperty`                                                                                                                                                                        | Only lit shapes get a dense copy (D73/D172), a rock clip and a bake. Plunges, spray, mist cards and pools all come from bakes (D78).                                                            |
| `VanillaFoam::IsFoamModel`          | `VanillaFoam.cpp:57-68`                | Needs `effects\`. Keeps `mistlow`, `waterfallbody`, `waterfallthin`, `waterfalltall`. Hides `rapids`, `splash`, `churn`, `foam`, `spray`, `mistblast`, `skirt`, `waterfallmist`, `ripple`.                           | Hides non-lit parts inside 85% of the simulated area, within -512..+1024 of a water surface (FF:839-843).                                                                                       |
| `WaterfallSources::IsWaterfallBody` | `WaterfallSources.cpp:47`              | `effects\` and (`waterfallbody` or `waterfallthin`)                                                                                                                                                                  | Falls push current and make foam in the sim (D22). Strength is the same for every fall (`OUTFLOW_SPEED` 60, `FOAM_RATE` 0.6, `WaterfallSources.cpp:27-35, 279-282`). Width has a minimum of 96. |
| `MeshCut::IsFallingWater`           | `MeshCut.cpp:50-59` (D176)             | `effects\` and (`waterfall`/`waterwall`/`rapids`)                                                                                                                                                                    | Never a sim obstacle.                                                                                                                                                                           |
| `FallCapture::IsFallingWater`       | `FallCapture.cpp:24` (D56)             | `fxwaterfall` or `fxrapids`                                                                                                                                                                                          | Its collision is not counted as rock.                                                                                                                                                           |
| Flow per water type                 | `WaterSurfaces.cpp:68`                 | WATR `kEnableFlowmap`                                                                                                                                                                                                | flowScale 1 or 0. Rest current = Unified Water flowmap x RiverSpeed x flowScale (FF:1401-1412). There is no per-type list. Cell-water fallback gets 0 (`WaterSurfaces.cpp:147-160`).            |
| Sloped planes                       | `WaterSurfaces.cpp:10, 78-81`          | A water bound with half-height > 10                                                                                                                                                                                  | Skipped: not simulated.                                                                                                                                                                         |
| Flowmap texture                     | `UnifiedWater/Flowmap.cpp:99, 187-195` | Tamriel only                                                                                                                                                                                                         | Other worldspaces have no flowmap.                                                                                                                                                              |
| Ground fog                          | `GroundHeightMap.cpp:74-76`, FF:1236   | Any water surface above the ground                                                                                                                                                                                   | The fog base is lifted to the water. It is off in interiors. It is also off in the user's settings (`InteractiveFog: false`).                                                                   |

## Data summary

**Flowing water types.** Only four WATR forms have `kEnableFlowmap`. All four are in Update.esm.

| Form     | EditorID              | FNAM | Used by                                                                                                                         |
| -------- | --------------------- | ---- | ------------------------------------------------------------------------------------------------------------------------------- |
| 01001232 | RiverWaterFlow        | 8    | 612 Tamriel exterior cells (XCWT). This is nearly all inland water: rivers **and lakes**.                                       |
| 01001235 | DefaultWaterFlowBlend | 24   | 14 Tamriel cells: the Sea of Ghosts coast at Katariah/Icerunner (-14..-7, 26), 39..42,14 near Windhelm, plus 23,-15 and 25,-12. |
| 01001233 | RiverWaterFlowBlend   | 24   | Not used by any cell or placed water.                                                                                           |
| 01001234 | DefaultWaterFlow      | 8    | Not used.                                                                                                                       |

-   No placed water activator (ACTI with WNAM) uses a flowmap type.
-   No interior and no other worldspace uses one. That includes Solstheim, Blackreach, the Forgotten Vale, and the city worldspaces Whiterun, Markarth and Riften.
-   Tamriel's default water (NAM2) is `DefaultWater` 00000018, which has no flowmap.

**RiverWaterFlow cells grouped by cell water height.** Same-height clusters are lakes or river reaches.

| Height   | Cells | Grid                 | What it is                                                         |
| -------- | ----- | -------------------- | ------------------------------------------------------------------ |
| 10624    | 84    | x 17..42, y -26..-15 | Lake Honrich, the Riften rivers, Heartwood Mill, Geirmund's Hall   |
| 600      | 47    | x -12..0, y -16..-12 | **Lake Ilinalta** (IlinaltasDeepExterior01-04 at -8..-7, -13..-12) |
| -7700    | 29    | x -17..-5, y 0..7    | Hjaalmarch / Whiterun tundra rivers                                |
| -250     | 13    | x 2..7, y -13..-9    | Riverwood river (Riverwood01/02/06)                                |
| -9550    | 12    | incl. 26,2           | Includes Mara's Eye Pond, a pond                                   |
| none set | 186   | all over             | Cells that take the plane from the worldspace                      |

**Older "flow" types without the flowmap bit.** These scroll in vanilla through the WATR linear velocity (NAM0). Fluid Field gives them no current.

| Type                           | Linear velocity                | Where                                                                                                                                                |
| ------------------------------ | ------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| RiverWaterFlowNE 000E717C      | (2.54, 1.35)                   | 20 Tamriel cells: Riverwood03 (6,-11), Riverwood04 (5,-12), Tamriel 8,-6 below Whiterun, Ivarstead, Darkwater. Also 41 placed planes, Markarth gate. |
| RiverWaterFlowNW / SE          | (-1.96, 2.0) / (1.8, -2.2)     | 28 placed planes. Interiors: Arkngthamz, Forgotten Vale caves, Bthalft.                                                                              |
| CreekWaterFlowSE / SW          | (0.70, -0.72) / (-0.98, -0.69) | 30 placed planes. Falkreath creeks (e.g. -13,-17), Bthardamz02, Solstheim.                                                                           |
| CreekWaterFlow 00015429        | (0, 0)                         | The Whiterun tundra stream planes (TundraStream\*WaterA/B, 31 refs, e.g. Tamriel 3,-2).                                                              |
| WRWaterFlow, MarkarthWaterFlow | (0, 0)                         | Whiterun city streams and Markarth's stream. These are sloped meshes.                                                                                |
| DLC1WaterFastFlow              | (3, -3)                        | Darkfall Cave                                                                                                                                        |
| DLC1FrostreachWaterFlowNE/SE   | (1.75, 0.75) / (1.8, -2.2)     | Forgotten Vale, 27 cells                                                                                                                             |
| DefaultRiverWater              | (-0.5, 1.29)                   | Solstheim, 4 cells (15..16, 14..15: Thirsk, Stone Beast)                                                                                             |
| DefaultIntWaterWaistHeightFlow | (0, 0)                         | 7 interiors (Bleak Falls Barrow 02, Alftand, Brinewater)                                                                                             |

**Placed water-like models, and which of our filters catch them.** From `classify.py`. Ambient fog mists (`fxmistlow*`, about 4,500 refs) are left out: they are not water.

| Model                                                            | Refs                            | NIF       | Classify | Notes                                  |
| ---------------------------------------------------------------- | ------------------------------- | --------- | -------- | -------------------------------------- |
| FXWaterfallThin512x128                                           | 228                             | L0 E3 P1  | Body     | 131 interior                           |
| FXWaterfallThin2048x128                                          | 118                             | L0 E4 P1  | Body     | 62 interior                            |
| FXWaterfallThinSpray2048                                         | 68                              | L0 E5 P1  | Body     |                                        |
| FXWaterfallThin4096x256 / 3096x256 / 2048x512                    | 54 / 29 / 53                    | L0        | Body     | Blackreach, Solstheim                  |
| FXWaterfallThin128x128 / 256x128 / 512x64 / Leak / LeakSmall     | 38 / 17 / 10 / 32 / 3           | L0        | Body     | Whiterun Wind District                 |
| FXWaterfallThinSheet                                             | 7                               | **L1** E3 | Body     | The only lit thin model                |
| FXWaterfallBodySlope / Tall / Tall02 / Tall02MovLOD / noLOD      | 98 / 117 / 25 / 9 / 7           | L1        | Body     | The big falls tested so far            |
| FXRapids / 02 / Big01 / FallsLine01 / FallsTop / RingHeavy       | 131 / 81 / 160 / 264 / 10 / 258 | L0        | Rapids   |                                        |
| FXrapidsRocks01                                                  | 128                             | L6 E3 P1  | Rapids   | Lit boulders stay (D81)                |
| DLC1FXFasterRapids01/02                                          | 10                              | L0        | Rapids   | Darkfall Cave                          |
| DLC1FXFasterFallsLine01                                          | 3                               | L0        | **none** | No "rapids" in the name                |
| FXSplashLargeChurnNoRapids                                       | 95                              | L0        | Rapids   | Name contains "rapids"                 |
| FXSplashLargeChurn / SmallParticles / SmallParticlesLong / Heavy | 217 / 498 / 608 / 20            | L0        | Splash   |                                        |
| **FXDripsSplash01 / 02, FXFloorSplash\***                        | 574 / 24 / 30                   | L0        | Splash   | Cave drip splashes, mostly interior    |
| FXWaterfallSkirtSlope / TallFront / TallFrontSlope               | 135 / 121 / 8                   | L0        | Skirt    |                                        |
| FXWaterfallMistBlast / MistBlastLite                             | 124 / 368                       | L0        | Mist     |                                        |
| **FXCreekFlatLong01**                                            | 382                             | L3 E3 P1  | **none** | 234 interior, 90 Tamriel, 28 Solstheim |
| **FXCreekFlatLarge**                                             | 29                              | L4 E1     | **none** |                                        |
| **Water\TundraStreamTransition01** (+Rocks, Ice, Snow)           | 137                             | L3 E1     | **none** | Creek-mouth meshes                     |
| Landscape\Tundra\TundraStream\*                                  | 108                             | L8        | none     | Stream-bed statics                     |
| **MarkarthWaterfallCliff02/03/04, WRTerWind01Waterfall01**       | 4                               | L1 E1-7   | **none** | City falls. Path has no `effects\`.    |
| WRRapidRocks01                                                   | 6                               | L1 E1     | none     | Whiterun                               |
| Water\DLC2OceanWave\*                                            | 18                              | L1 E7 P4  | none     | Solstheim surf                         |
| Water\MineralPool\*Water                                         | 75                              | L1        | none     | Eastmarch hot springs, Ancestor Glade  |
| FXDrips01\* (all)                                                | about 1,250                     | L0        | none     | Drips. Not water on the ground.        |

Water planes placed as activators (ACTI + WNAM) number 1,100 refs over 60 bases. The main ones are:

-   Water1024 (DefaultWater): 199
-   Water1024Ankle/WaistStill (interior): 150
-   DLC2WaterApocrypha: 137
-   WaterPuddle (PuddleWater): 87
-   Water1024NoReflections: 79
-   HorseTrough: 48
-   RiverFlowNE/NW: 61
-   CreekFlowSE/SW: 34
-   TundraStream planes: 31

---

## 1. Thin, low-flow falls (`FXWaterfallThin*`)

**Examples.**

-   Models: the twelve `FXWaterfallThin*` models above, about 650 refs. About 300 of them are in interiors. This is about **71% of all falling-water refs**. Only FXWaterfallThinSheet (7 refs) has a lit shape.
-   WhiterunWorld 6,-1 (`WhiterunWindDistrict03`): Thin256x128 x6 at (25929, -233, -1920), Thin128x128, Thin512x128.
-   HelgenKeep01 (interior): Thin512x128 x5 and Thin128x128 x3 at about (-1754, -4326, -1473).
-   Blackreach 3,5: Thin4096x256 x2 at (14411, 21344, 5387), Thin2048x128 x3, ThinSpray2048 x2.
-   Solstheim 9,21 (`DLC2CastleKarstaagRuinsExterior02`): Thin2048x512 x5 at (37469, 86321, 6509).
-   Also White River falls, Tamriel 7,-8: Thin512x128 x5 at (30132, -31615, -1213), next to the lit falls.

**Current treatment.**

-   **Classify:** Body (FS:288).
-   **Dense copy and bake:** none. There is no lit shape (FS:811-831).
-   **Hidden:** all of it. The effect layers are Sheet or Streaks, the particles are Particles, and none of these are in `FallLayerMask` 0x1 (FS:432-440, `FluidField.h:93`). The code hides them whenever Waterfalls is on (FF:846-852, `hideFallEffects` default true, `FluidField.h:396`). Nothing draws them in their place. _Needs in-game check:_ they are probably invisible today, not "vanilla unchanged".
-   **Sim:** WaterfallSources still treats each one as a full fall (`WaterfallSources.cpp:47`). It pushes current at `OUTFLOW_SPEED` and foams at `FOAM_RATE`, the same as a big fall, with a radius from a width of at least 96 (`:27, :279-282`). So a hidden trickle can leave a churning foam patch with no fall above it.
-   **VanillaFoam:** kept (`VanillaFoam.cpp:63-64`).
-   **MeshCut:** not an obstacle.

**Right look.**

-   A thin, translucent ribbon with vanilla's streak scroll.
-   At most a small ring of ripples and a little white where it lands.
-   No big plunge foam, no spray cloud, no mist cards.
-   Indoors, it must keep showing.

**Smallest change.**

1. Hide Body layers only on models that have a lit shape (a dense copy or a refractive draw replaces them). Effect-only Body models stay as vanilla draws them. This is one check in the Body branch of `RefreshHiddenEffects`.
2. Scale each WaterfallSources emitter by the fall's size: width x drop height, or the mesh's scale. Effect-only models should get a fraction of the push and foam. Use `tools/bakestats.py`-style data before picking the factor (data-driven-thresholds rule).
3. Later, and optional: drive the thin streak UV scroll from the fall's drop speed, like D67's shared scroll, instead of a bake.

## 2. Creeks and rapids effects

**Examples.**

-   Rapids: FXRapids/02/Big01/FallsLine01/RingHeavy/Rocks01, about 1,030 refs. FXCreekFlatLong01 (382), FXCreekFlatLarge (29) and TundraStreamTransition01 (137).
-   **Riverwood**, Tamriel 4..6, -13..-11: RiverWaterFlow. Riverwood03 and 04 use RiverWaterFlowNE, which has no flowmap (_needs in-game check_ on which reach is which). The user's `FluidField_Rapids` save is here.
-   **Tamriel 8,-6** (White River below Whiterun): FXRapids x2 at (33910, -22670, -3746), FXrapidsBig01 x3, FallsLine01 x3, BodySlope_static. The cell water is **RiverWaterFlowNE (no flowmap)**.
-   **Tamriel 3,-2** (`WhiterunExterior13`), a tundra creek: TundraStreamBend01WaterB (CreekWaterFlow) at (13653, -6564, -4800), FXCreekFlatLong01 x2 at (15680, -5952, -4396), RingHeavy, Transition01.
-   **HelgenKeep01:** FXCreekFlatLong01 x22 at (-659, -3987, -1497). **StonyCreekCave01:** x16 at (-1293, 1068, -670).
-   **DLC1DarkfallCave01:** DLC1FXFasterRapids01 x5 at (-3245, -2158, -2084), FXCreekFlatLong01 x17, DLC1WaterFastFlow planes.

**Current treatment.**

-   **FXRapids\*:** Classify Rapids.
    -   Non-lit layers are hidden (D81, FS:424-431). They are also hidden by VanillaFoam inside the sim.
    -   FXrapidsRocks01's boulders stay.
    -   A site with no Body becomes `Kind::Rapids`, which leaves the sim and its foam to carry it (FS:6079-6084).
    -   They are excluded from MeshCut obstacles and from FallCapture rock.
-   **FXCreekFlatLong01, FXCreekFlatLarge, TundraStreamTransition01, DLC1FXFasterFallsLine01:** no filter catches them.
    -   Vanilla draws them unchanged, effect layers included.
    -   D81 names FXCreekFlatLong01, but the code never collects it, so D81 does nothing to it.
-   **Coverage:** the sim makes foam only from current: shear, Froude, walls (`WaterFoamCS.hlsl:147-154`). Current exists only on flowmap water. `hiddencover.py` estimates how many hidden rapids refs sit over water with **no flowmap**:
    -   Rapids: 667 of 1,137
    -   Splash: 1,692 of 2,015
    -   Skirt: 171 of 265
    -   Mist: 420 of 492
    -   That covers every interior, Solstheim, the Forgotten Vale, Blackreach, the cities and Tamriel non-flowmap reaches such as 8,-6.
    -   There the vanilla foam is hidden and nothing replaces it. _Needs in-game check._

**Right look.**

-   Rapids: white broken water over rocks, carried downstream. Riverwood is the target.
-   A creek is calmer. It has thin lace around stones and a gentle drift, not Riverwood's white water.
-   Where there is no simulated current, vanilla's rapids should stay.

**Smallest change.**

1. Hide a rapids, splash, skirt or mist ref only when the water under it has flowScale > 0, or when a bake replaces it. Otherwise leave vanilla. The water map already carries flowScale per texel (`WaterMapBuilder.cpp:86`).
2. For FXCreekFlatLong01 and FXCreekFlatLarge, apply the same rule: add `creekflat` to the VanillaFoam patterns (non-lit parts only), but only on flowing water. They then match FXRapids' handling on rivers and stay vanilla in caves.
3. Add `fallsline` to Classify Rapids so DLC1FXFasterFallsLine01 joins FXRapidsFallsLine01.

## 3. Very shallow water

**Examples.**

-   **IlinaltasDeep01** (interior): WaterPuddle (PuddleWater) x28 at (3067, 5256, -186), scale up to 3.7. FXDripsSplash01 x66, FXFloorSplash_Large x4.
-   **Tamriel 3,-2** (`WhiterunExterior13`): the TundraStream planes (CreekWaterFlow) are ankle-deep creeks. The Whiterun tundra has 31 such planes.
-   **GallowsRock01:** WaterPuddleDefaultIntAnkleStill x7 at (4374, -4460, 340). **FrostflowAbyss:** Water2048DefaultIntAnkleStill x6 at (-3776, 2672, -1664).
-   PuddleWaterFlow / WaterPuddleLongFlow: Alftand01 at (4643, 3124, 28), Labyrinthian01.
-   Anywhere a RiverWaterFlow cell plane lies just above the terrain on a shore.

**Current treatment.**

-   **Solver:** a cell with depth <= `wetThreshold` (1 unit) is dry (`ShallowWaterSolver.h:32`). There is no rest current, no foam and no height offset there (`WaterFoamCS.hlsl:112-115`).
-   **Bed:** the bed is the landscape or a ray. Where it is unknown, it is set 1024 below the plane (`WaterMapBuilder.cpp:15, 79`).
-   **Foam (D71):** shear > 4/s, impact > 90, wall rush > 320, Froude > 2.5.
    -   Froude uses `max(depth, 1)` (`WaterFoamCS.hlsl:150`).
    -   So 2 units of water at RiverSpeed 150 gives Froude 4.0, which counts as breaking. Shallow flowing water foams all over. Nothing else fades foam with depth.
    -   This only matters on flowmap water. Puddles have no current.
-   **Wakes:** speed >= 20, feet within 32 of the surface (`WakeSources.cpp:13, 17`). There is no depth test on the CPU. The GPU uses depth > WetThreshold.
    -   A puddle 2 units deep gets the full wake. _Needs in-game check:_ a full V-wake in a puddle looks wrong.
-   **Ground fog:** lifted to any water surface above the ground, even a few millimetres (`GroundHeightMap.cpp:74-76`). It is off indoors (FF:1236) and off in the user's settings.

**Right look.**

-   Puddles and creek films: small rings and ripples around feet. No travelling V-wake, no white foam.
-   Shore films: the colour of wet ground, no foam unless the water truly breaks.
-   Fog should sit on the ground, not on a 2-unit film.

**Smallest change.**

1. In `WaterFoamCS`, scale the breaking and shear terms by a depth ramp, e.g. `saturate((depth - a) / b)`. Set a and b from bakestats-style data on real shore depths, not by guess.
2. Scale the wake and splash emitter strength by the same depth ramp, so shallow water shows rings rather than wakes.
3. In `GroundHeightMap`, lift the fog base to the water only where the water is more than a few units deep.

## 4. Water planes by type (flowmap flow)

**Examples.**

-   **Lake Ilinalta:** RiverWaterFlow, cell height 600, 47 cells. `IlinaltasDeepExterior01` is at -8,-12; the lake centre is about (-30700, -47100), with water at z 600.
-   **Lake Honrich:** RiverWaterFlow, height 10624, 84 cells with the Riften rivers. `HeartwoodMillExterior` is at 32,-24.
-   **Mara's Eye Pond** (`MarasEyePondExterior01`, 26,2): RiverWaterFlow, a still pond.
-   **Sea coast:** `KatariahExterior01` (-13,26) and `IcerunnerExterior01` (-8,26) use DefaultWaterFlowBlend. This is flowing sea.
-   **Riverwood** (4..6, -13..-11): the river this flow was tuned on.

**Current treatment.**

-   The rest current is the Unified Water flowmap x RiverSpeed (150) x flowScale (`WaterSurfaces.cpp:68`, FF:1401-1412).
-   Where planes overlap, the highest one wins (`WaterSurfaces.cpp:138-146`).
-   D20 tried telling lakes apart by width and withdrew it. The flowmap is as strong under Ilinalta (0.84) as under the rapids. Still open.
-   Every other "flow" type (table above) gets no current, though vanilla scrolls it by its linear velocity.

**Right look.**

-   Rivers flow.
-   Lakes and ponds are still or drift weakly.
-   The sea has waves, not a river current.
-   Placed creek planes flow in vanilla's own direction.

**Smallest change.** Both options use vanilla data.

1. **Lakes.**
    - Make a small table of "still" RiverWaterFlow planes, keyed by cell water height plus the cell cluster: Ilinalta 600, Mara's Eye -9550, Honrich 10624 minus its river cells, and so on.
    - Generate it offline from the ESM (`flowcells.py` already groups them) and review it by hand.
    - Apply flowScale 0 there.
    - Alternative, if the flowmap texture has a usable speed channel under lakes: scale by that. _Needs a look at the texture._
2. **Non-flowmap flow types.**
    - Use the WATR's own linear velocity as a uniform current. This is direction only; speed comes from `RiverSpeed` x a small factor.
    - `WaterSurfaces` already logs `linearVelocity` (`:72-74`).
    - This gives Riverwood03/04, Tamriel 8,-6, the Falkreath creeks, the Forgotten Vale, Solstheim's rivers and the interior streams a drift in vanilla's direction.
    - Keep 0 for types whose velocity is (0, 0).
3. Sea: DefaultWaterFlowBlend on the coast should probably not get river current. Same table as lakes. _Needs in-game check_ on how it looks today.

## 5. Skirts, mist and splashes

**Examples.**

-   **White River falls,** Tamriel 7,-7 / 7,-8, the main test site:
    -   FXWaterfallSkirtSlope x5 at (30847, -26830, -1953)
    -   MistBlastLite at (30504, -26133, -2139)
    -   FXLargeSplashChurn x4, SplashSmallParticlesLong x13
-   **DragonBridgeExterior03** (-25,19): MistBlastLite x4 at (-101721, 79206, -13203).
-   **BleakFallsBarrow02:** FXWaterfallSkirtTallFront x3 at (-7403, -3252, 75), over DefaultIntWaterWaistHeightFlow (no flowmap).
-   **Eastmarch hot springs:** `POIVolcanicTundra05` (30,-2), FXLargeSplashChurnWater x5 at (126963, -5620, -10950), DefaultVolcanicWater. These are bubbling springs, not falls.

**Current treatment.**

-   Skirt, Mist and Splash classes: everything non-lit is hidden (FS:424-431, D81). VanillaFoam does the same inside the sim.
-   At lit falls, Fluid Field's own spray, mist cards (`MistAmount`) and plunge foam replace them, all from lit-mesh bakes (D78).
-   Elsewhere nothing replaces them:
    -   near thin falls
    -   in caves
    -   at hot springs, where the churn models are the bubbling
    -   on still water
-   **False positives:** FXDripsSplash01/02 and FXFloorSplash\* (about 630 refs, mostly cave floors) match `splash`. They are hidden too.

**Right look.**

-   At big lit falls, today's replacement is the agreed target (falls-target-look).
-   Elsewhere, vanilla's skirts, mist and splashes should stay until something replaces them.
-   Cave drips and hot-spring bubbles should be left alone.

**Smallest change.**

1. Same rule as gap 2: hide only where a bake (plunge or spray from a lit mesh) or simulated current covers the ref.
2. Exclude `drip` and `floorsplash` from both Classify and VanillaFoam.
3. Exclude `churnwater`, or any ref over a non-flowing type, so the hot-spring bubbles stay.

## 6. Anything else

| Kind                                   | Where                                                                                                                                                                               | Today                                                                                                                                  | Right look / change                                                                                                                                    |
| -------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| City falls on architecture paths       | MarkarthWorld -43,1 (`MarkarthOrigin`): MarkarthWaterfallCliff02/03 at (-175680, 4608, -2944); -43,0: Cliff04. WhiterunWorld 6,-1: WRTerWind01Waterfall01 at (25208, -1440, -3072). | No filter: the paths have no `effects\`. They are lit (L1) plus effect layers. Vanilla.                                                | Very visible in Markarth. Add their names to Classify Body so they get a dense copy and bake. _Needs in-game check_ on whether their UVs bake cleanly. |
| City streams (sloped water meshes)     | Whiterun: WRStairsStream L/R, WRMainRoadPlainsRiver01 (WRWaterFlow). Markarth: MarkarthWaterSystemStream (MarkarthWaterFlow).                                                       | Their bounds are probably > 10 tall, so they are skipped as sloped (`WaterSurfaces.cpp:78-81`). _Check the "sloped" count in the log._ | Leave vanilla. Possibly treat them as falls later.                                                                                                     |
| Interior water                         | 7 interiors with WaistHeightFlow; ankle and waist planes in about 60 cells; Blackreach water in DLC2RRBulwarkCave (26 planes)                                                       | The sim runs on the planes. No current anywhere (no flowmap). Thin falls and rapids are hidden (gaps 1 and 2).                         | Keep vanilla effects; give a drift from linear velocity (gap 4).                                                                                       |
| Blackreach                             | Blackreach 3,5: Thin4096x256 / Thin2048x128, FXrapidsRocks01 x2 at (14232, 21489, 1811), SkirtTallFront x2. BlackreachWater (no flowmap).                                           | As above. Its tall thin falls are hidden.                                                                                              | As gap 1.                                                                                                                                              |
| Forgotten Vale (Dawnguard)             | `DLC1FalmerValleyPOI06` (0,-1): FXWaterfallBodyTall02MovLOD/noLOD at (94, -2129, 2983), lit. FrostreachWaterFlowNE/SE (no flowmap).                                                 | The lit falls are baked. Rapids and splashes are hidden with no current under them.                                                    | Gap 2 rule, gap 4 drift.                                                                                                                               |
| Solstheim                              | River at 15,15 / 16,14 (DefaultRiverWater): FXRapids02 at (64677, 65111, 4804), FXrapidsBig01. Placed CreekFlowSE / RiverFlowNE planes.                                             | No flowmap types, and the Unified Water flowmap is Tamriel-only (`Flowmap.cpp:187`). The sim has no current, but rapids are hidden.    | Gap 2 rule, gap 4 drift.                                                                                                                               |
| Solstheim surf                         | DLC2OceanWaveCurveOuter / WavesStraight, 18 refs (e.g. 20,13 at (83736, 56042, 256))                                                                                                | No filter. Vanilla.                                                                                                                    | Fine as is. _Needs in-game check_ that sim waves don't fight it.                                                                                       |
| Hot springs and mineral pools          | DefaultVolcanicWater, 39 cells (26..35, -9..-1). Water\MineralPool\* (75, lit static "water").                                                                                      | The sim runs, still. The churn bubbles are hidden. The lit pool meshes are untouched.                                                  | Keep the bubbles (gap 5).                                                                                                                              |
| Lit thin sheet / small-scale lit falls | FXWaterfallThinSheet x2 in IlinaltasDeep01 at (353, 315, 445), scale 0.44                                                                                                           | Full big-fall treatment: dense copy, bake, spray, plunge, mist cards.                                                                  | Scale spray, mist and plunge by the mesh's world size. Low priority.                                                                                   |
| Apocrypha water                        | DLC2ApocryphaWater (FNAM 1, damaging), 137 planes                                                                                                                                   | The sim runs.                                                                                                                          | Probably fine. Not checked.                                                                                                                            |
| Drips                                  | FXDrips01\*, about 1,250 refs                                                                                                                                                       | Nothing (correct). Their floor splashes are hidden (gap 5).                                                                            | Leave them.                                                                                                                                            |

---

## Gap list, ranked by how much players see it

| #   | Gap                                                                                                                                                                 | Refs / scale                                                                                                | Where first seen                                                                      | Smallest change                                                                                 |
| --- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------- |
| 1   | **Effect-only thin falls are hidden and not replaced.** Their sim source still churns and foams at full-fall strength. (Code says so; confirm in game.)             | About 650 refs, 71% of falling-water refs. Nearly every cave and many exteriors.                            | Helgen Keep (the first dungeon), Bleak Falls Barrow, Whiterun Wind District           | Hide Body layers only on models with a lit shape. Scale WaterfallSources by fall size.          |
| 2   | **Rapids, splash, skirt and mist are hidden over water with no simulated current.** Nothing replaces them. This includes cave drip splashes and hot-spring bubbles. | About 2,950 of 3,900 classified non-Body refs                                                               | Every cave; Solstheim; Forgotten Vale; Tamriel 8,-6 below Whiterun; Eastmarch springs | Hide only where flowScale > 0 or a bake covers it. Exclude `drip`, `floorsplash`, `churnwater`. |
| 3   | **Lakes and ponds on RiverWaterFlow run at full river current.**                                                                                                    | Ilinalta 47 cells, Honrich about 84, Mara's Eye, plus the sea coast on DefaultWaterFlowBlend                | Lake Ilinalta (known), Riften                                                         | An offline-generated, hand-reviewed table of still planes (flowcells.py). flowScale 0 there.    |
| 4   | **Non-flowmap "flow" water has no current.** Vanilla scrolls it by linear velocity.                                                                                 | 20 Tamriel cells including Riverwood03/04, about 90 placed planes, all interiors, Solstheim, Forgotten Vale | Riverwood's NE reach, Tamriel 8,-6, Falkreath creeks                                  | Use the WATR linear velocity as a uniform current.                                              |
| 5   | **Shallow films foam and wake like deep water.** Froude uses max(depth, 1). The fog lifts onto mm films (fog is off by default).                                    | Every shore and creek film on flowing water; all puddles for wakes                                          | Riverwood banks, tundra creeks                                                        | A depth ramp on breaking, shear, wakes and fog lift, set from measured data.                    |
| 6   | City falls on architecture paths are not treated.                                                                                                                   | 4 refs, very visible                                                                                        | Markarth entrance, Whiterun Wind District                                             | Add them to Classify Body.                                                                      |
| 7   | FXCreekFlatLong01 / Large / TundraStreamTransition01 are untouched. They can double up with sim foam on flowing water.                                              | About 550 refs                                                                                              | Whiterun tundra creeks, Falkreath                                                     | VanillaFoam `creekflat`/`streamtransition`, only on flowing water.                              |
| 8   | Sloped water meshes (city streams) are not simulated.                                                                                                               | About 10 refs                                                                                               | Whiterun, Markarth                                                                    | Probably leave. Check the sloped count in the log.                                              |
| 9   | Small lit sheets get the full spray, mist and plunge.                                                                                                               | 7 + small-scale lit bodies                                                                                  | Ilinalta's Deep                                                                       | Scale the extras by the mesh's world size.                                                      |
| 10  | Solstheim surf, Apocrypha water and mineral pools are not reviewed.                                                                                                 | 18 / 137 / 75                                                                                               | Solstheim coast                                                                       | In-game check only.                                                                             |

---

## Game round plan

-   **Launch.** One launch.
-   **Before the first stop:** `set timescale to 0`, `set gamehour to 13.5`, `fw 81a` (clear). Repeat `fw 81a` after each worldspace change.
-   **At each stop:** wait for the full load, take the vanilla shot with Fluid Field off (the menu's Enabled), then the Fluid Field shot from the same pose.
-   **z values.** Treat them as approximate; a `player.setpos z` a little above the stated value avoids falling through.
-   **Exteriors without an EditorID.** Use `cow <worldspace> X Y` first.

| #   | Command                                           | Then                                                              | Shows (category)                                                                          |
| --- | ------------------------------------------------- | ----------------------------------------------------------------- | ----------------------------------------------------------------------------------------- |
| 1   | `coc HelgenKeep01`                                | `player.setpos x -1700`, `y -4300`, `z -1400`                     | Thin falls (1); FXCreekFlatLong01 (2); splashes (5); interior water                       |
| 2   | `coc BleakFallsBarrow02`                          | `setpos -7300 -2900 250`                                          | Interior flowing type with no flowmap (4); thin falls; SkirtTallFront; RingHeavy (5)      |
| 3   | `coc IlinaltasDeep01`                             | `setpos 3067 5256 -150`                                           | Puddles and shallow water (3); drip splashes (5); the lit ThinSheet (6)                   |
| 4   | `coc IlinaltasDeepExterior01`                     | `setpos -30700 -47100 700`                                        | **Lake Ilinalta** on RiverWaterFlow (4)                                                   |
| 5   | `coc Riverwood` (or the `FluidField_Rapids` save) | Bridge, then the Riverwood03/04 reach east                        | Rapids target (2); flowmap versus RiverWaterFlowNE reaches (4)                            |
| 6   | `cow Tamriel 7 -8` (or the falls save at 12:00)   | `setpos 30300 -31500 -1100`                                       | White River falls: lit falls, thin falls beside them, skirt, MistBlastLite, churn (1, 5)  |
| 7   | `cow Tamriel 8 -6`                                | `setpos 33900 -22700 -3650`                                       | Rapids over RiverWaterFlowNE (no flowmap) (2, 4)                                          |
| 8   | `coc WhiterunExterior13`                          | `setpos 13300 -8000 -4700`                                        | Tundra creek: CreekWaterFlow planes, FXCreekFlatLong01, Transition01; shallow film (2, 3) |
| 9   | `coc WhiterunWindDistrict03`                      | `setpos 25300 -1300 -2600`                                        | Thin256x128 falls; WR sloped streams; WRTerWind01Waterfall01 (1, 6)                       |
| 10  | `coc MarkarthOrigin`                              | Look at the cliff falls by the gate, about (-175700, 4600, -2900) | Architecture-path falls; Markarth stream (6)                                              |
| 11  | `coc POIVolcanicTundra05`                         | `setpos 126900 -5600 -10900`                                      | Hot springs: churn bubbles, DefaultVolcanicWater (5, 6)                                   |
| 12  | `coc IcerunnerExterior01`                         | Shore                                                             | Sea on DefaultWaterFlowBlend (4)                                                          |
| 13  | `cow Blackreach 3 5`                              | `setpos 13600 22000 2100`                                         | Tall thin falls, rapids rocks, SkirtTallFront in Blackreach (1, 2, 6)                     |
| 14  | `coc DLC1FalmerValleyPOI06`                       | `setpos 300 -2300 3000`                                           | Dawnguard lit falls (Tall02MovLOD); Frostreach flow type (4, 5)                           |
| 15  | `coc DLC1DarkfallCave01`                          | `setpos -3200 -2200 -2050`                                        | DLC1 faster rapids; DLC1WaterFastFlow; creek (2, 4)                                       |
| 16  | `cow DLC2SolstheimWorld 15 15`                    | `setpos 64900 65000 4900`                                         | Solstheim river: DefaultRiverWater, FXRapids02/Big01 (2, 4)                               |
| 17  | `coc DLC2CastleKarstaagRuinsExterior02`           | `setpos 37400 86200 5100`                                         | Solstheim thin 2048x512 falls; snow creek (1, 2)                                          |
| 18  | `cow DLC2SolstheimWorld 20 13`                    | `setpos 83700 56000 300`                                          | Solstheim surf meshes (6)                                                                 |

That is 18 stops in one round. The order runs from interiors near Helgen and Riverwood, through Whiterun and the Reach, to Eastmarch and the coast, then the DLC areas.

-   Stops 1, 3, 6 and 9 settle gap 1.
-   Stops 2, 7, 11 and 15 settle gap 2.
-   Stop 4 settles gap 3.
-   Stops 5 and 7 settle gap 4.
-   Stops 3 and 8 settle gap 5.
