# Waterfall flow at HEAD c3a3e12b3 (D59): what causes the back-and-forth and the chaos

Read-only analysis. Sources: FallGround.hlsli, Lighting.hlsl, Effect.hlsl, FluidField.cpp/.h, FallSites.cpp, FallStream.cpp, ogfalls/main.md (vanilla controller keys), and the five bake dumps in `My Games/.../SKSE/FluidField_bake*.bin`. The dumps came from a D59 dev build at 11:05, so their rock rule may be older than HEAD's. The statistics script is `research/bakestats.py`.

## 1. CONFIRMED (main cause of the back-and-forth): the base scroll is `forward * Time` with a forward that changes over time

-   `FallGround.hlsli:272`: `forward = Scroll.y * InverseScale.y`.
-   `FallGround.hlsli:279`: `uvBase = uvStill - (0, frac(forward * Time))`.
-   `Time` is `fallSurfaceTime`. It only ever grows (`FluidField.cpp:848`) and has no wrap.
-   Any change dF in forward therefore moves the whole curtain by `dF * Time` repeats. The visible speed is `F + Time * dF/dt`, not F.

**a. Vanilla's V Scale wobble goes straight into forward.** `InverseScale.y` comes from `1/texCoordScale.y` (`FluidField.cpp:1587`). On every lit body (Tall Inner, Slope, Markarth WATER) a V Scale controller loops over 6.467 s. Its quadratic keys (main.md:76-77) are 1, 0.98945 at 0.5 s, 0.99926 at 1.533 s, 0.9946, 0.9989, 0.9916, then 1.

-   On Tall Inner (r = 0.12/s), forward swings by dF = 0.12 × 0.01066 = 0.00128 /s within 0.5 s. On Slope (r = 0.1714/s) it is 0.00183 /s.
-   The resulting phase shift is dF × T:

| Time T | Shift (Tall) | Shift (Slope) |
| ------ | ------------ | ------------- |
| 60 s   | 0.08 repeats |               |
| 300 s  | 0.38 repeats | 0.55 repeats  |
| 1000 s | 1.28 repeats | 1.83 repeats  |

-   One repeat is about 800-4000 world units, so each of these shifts is a large, visible movement.
-   The eased key makes dF/dt peak at about 3·dF per second. At T = 300 s the texture surges at about 1.15 rep/s (roughly 10 times vanilla's 0.12). From 0.5 s to 1.5 s it then runs **backward** at about 0.4 rep/s. The pattern repeats every 6.5 s, with smaller wiggles in between, and grows linearly with session time.
-   D58 had the identical line, which explains why it looked clean early in a session and showed back-and-forth "once".

**b. Noise in the rate measurement is multiplied by T in the same way** (`FluidField.cpp:1566-1580`).

-   The clock is `fallSurfaceTime`, which adds `min(dt, 1/20)` per frame (`FluidField.cpp:702`, `FluidField.h:50`). Vanilla's controllers run on unclamped time.
-   One 100 ms hitch inside a 0.25 s window makes the measured rate about 20% too high. After the 0.3 smoothing that is about 6%, or dF ≈ 0.007, which is about 2 repeats at T = 300. The curtain jumps, then slides back over about 1 s as the smoothing relaxes.
-   Frame rates that hover around 20 fps produce this continuously.
-   When a material is first seen, `DEFAULT_SCROLL` (0.15) is replaced by the measured 0.12 after 0.25 s. That is a one-time jump of 0.03 × T.
-   The buffer index matches what the vertex shader used (TruePBR.cpp:1110 reads the same `textureTransformCurrentBuffer`). It is not a cause.

**Fix:** do not multiply a rate by absolute time.

-   Simplest: use vanilla's own integrated phase. The V offset already equals −r·t mod 1, and its whole-repeat wraps are invisible on a wrapped texture. In `Flow`: `uvBase = float2(uvStill.x * S0.x, uvStill.y * S0.y + TexOffset.y)`. This keeps the static scale S0 and drops only the U-offset wobble and the scale wobble.
-   S0 is the material's nominal scale. A long average of `texCoordScale` on the CPU (about 10 s) works.
-   Alternative: keep a per-material `phase = frac(phase + rate*dt)` on the CPU, with rate taken without `InverseScale`, and pass the phase in the constant buffer.
-   Either way, the measured rate then only scales the deviation. Noise there is harmless because it is not multiplied by T.

## 2. MOSTLY REFUTED: vanilla's offset and scale wobble leaking into the sampled coordinates

-   `uvStill = (a_uv - O) * (1/S)` (`FallGround.hlsli:271`) exactly inverts `uv = uv0*S + O` (`Lighting.hlsl:198`) with the same buffer index. The U-offset wobble (±0.036) and the scale wobble therefore cancel at the sample position.
-   The wobble leaks only through `forward × T` (finding 1a).
-   Side effect: dividing by S removes the **static** scale as well. The Effect layers use scale (2,3) or (3.04,3), so they now show their texture 2-3 times coarser than vanilla. This changes how they look, not how they move. The S0 in fix 1 restores it.

## 3. CONFIRMED (main cause of the chaos at boulders): the bake field near rock is discontinuous and too strong, and the two-phase scheme magnifies it

Dump statistics, per mesh:

| Measure                                                       | Value                         |
| ------------------------------------------------------------- | ----------------------------- |
| Rock islands                                                  | 30-118                        |
| Islands of 4 texels or fewer                                  | about 60% (median 1-3 texels) |
| Speed p50 / p99 / max                                         | 0.83-0.97 / 1.3-1.9 / 3.5-5.5 |
| Water texels clamped to `vy = 0.3`                            | 4-9%                          |
| Neighbour pairs whose down component jumps by more than 0.5   | 340-1330                      |
| Neighbour pairs whose across component jumps by more than 0.5 | 100-320                       |

Causes:

-   **Hard edges at rock.** Non-water texels are written as (0,1,0,0) (`FallSites.cpp:2097`). `BakeAt` interpolates bilinearly into them (`FallGround.hlsli:257-264`), so a stalled texel (0.3) or a squeezed one (3-5) sits beside "vanilla forward" with no deviation, one texel (10-20 units) away. Foam and run time also drop to 0 at rock edges.
-   **No upper clamp.** Only `down >= 0.3` and `|across| <= 1.7·down` are applied (`FallSites.cpp:2103-2104`). Singular corner speeds pass through: a texel with vy = 5 gives a deviation of 4·forward ≈ 0.48 uv/s, a travel of 0.24 repeats per 0.5 s cycle, beside a texel with 0.
-   **The across component also reaches the cap.** It is scaled by `acrossToU` (`FallSites.cpp:2133`) and lands at the 0.3 cap (`DEVIATION_MOST`, `FallGround.hlsli:58, 302-305`).
-   **Only one 3×3 smoothing pass** (`FallSites.cpp:2108-2131`), over water only.
-   **The velocity is halved next to walls.** Per-cell velocity is the average of the face fluxes, and wall faces carry 0 (`FallStream.cpp:234-235`). This creates stall bands that the clamp then holds at 0.3.

Result: within each 0.5 s cycle, neighbouring pixels slide up to 0.24-0.3 repeats apart (hundreds of world units of texture over about 20 units of surface), then reset and cross-fade. That reads as chaos.

The cross-fade also produces a ghost jump everywhere: each phase shows a copy offset by travel/2 (`FallGround.hlsli:312-314`), a pulse of up to 0.15 repeats at 2 Hz. It is small on curtains (p95 travel about 0.015) and large near rock.

Fixes, in order:

1. Open small rock islands of 6 texels or fewer and treat them as water (they are noise from the parity test).
2. Fill non-water texels with the nearest water value by dilation instead of (0,1).
3. Clamp the speed (`down` at most about 1.6, `|v|` at most 1.6, turn at most about 45°).
4. Blur the deviation with a Gaussian of 3-4 texels, or several passes.
5. Lower `DEVIATION_MOST` to about 0.08 repeats. Target: Δtravel between pixels 20 units apart below about 0.02 repeats.

## 4. Other findings

-   **CONFIRMED: secondary textures are sampled at `uvA` only.** The env mask (`Lighting.hlsl:1233/1314`; the falls use the Environment Map shader, main.md:75), the mesh terrain-variation lattice (`uvOriginal`, `Lighting.hlsl:1058-1067`) and any glow or backlight maps are sampled at `uvA` without the cross-fade to `uvB`. They saw-tooth: they move with the deviation and snap back by the full travel every 0.5 s. **Fix:** sample them at `uvBase` (no deviation).
-   **SUSPECTED: inflow and outflow cells along the sides.** About 74-95 inflow and 104-109 outflow cells per mesh sit away from the top and bottom rows (`FallSites.cpp:2085` marks any texel with an off-mesh texel above or below). They are pressure-zero sources and sinks along the jagged UV sides, which skews the flow sideways near edges. **Fix:** mark only the first and last covered run of each column.
-   **SUSPECTED: `fallScrolls` is keyed by material.** If materials are shared across refs whose controllers update at different times, the rate gets noisier. Check by logging the number of materials against the number of geometries and the spread of the rate. Fix 1 removes the sensitivity anyway.
-   **REFUTED: float precision of Time.** It is harmless below about 10⁴ s. At about 10⁵ s (around 28 h of session) the per-frame increment starts to quantise.
-   **REFUTED: phase and cross-fade logic.** The phase and blend logic is correct: each reset is hidden at weight 0, and the stagger is smooth over about 1700 units.
