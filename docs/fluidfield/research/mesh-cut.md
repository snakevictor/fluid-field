# Mesh cut: water-line cross-sections of static meshes on the CPU

Feasibility spike. Date: 2026-10-14. Phase 1, follow-up to R1 (D7 stage 4) and
the D8 water slice. Status: **recommendation, awaiting a decision.**

**Question:** can Fluid Field build its obstacle mask by cutting the vanilla
water plane(s) against the static meshes near the player on the CPU, instead of
the Havok raycasts that misread the Riverwood bridge arches
(`reports/water-slice-1-round-3.md` section 4)?

**Short answer:** yes. The visual triangles of static `BSTriShape`s are very
likely still in system memory at runtime (`BSGraphics::TriShape::rawVertexData`
/ `rawIndexData`), Community Shaders already reads that pointer, and another
SKSE plugin (HIGGS) builds triangle lists from it. The cut is cheap (well under
1 ms per reference, done once per reference). Collision is a second CPU source
whose triangles can be queried for just the water-line slab. A GPU staging
readback is a last resort only.

**Conventions:** paths are relative to the repo root; `CL/` is
`extern/CommonLibSSE-NG/include/RE/`. **UNVERIFIED** marks anything inferred
rather than read in source, headers or a primary document. No figure here was
measured.

---

## 1. Finding candidate meshes

**Enumeration.**

-   `TES::ForEachReference` and `TES::ForEachReferenceInRange` (`CL/T/TES.h:81-82`)
    walk the attached grid cells, or the interior cell, plus the worldspace sky
    cell (`extern/CommonLibSSE-NG/src/RE/T/TES.cpp:83-157`).
-   Two problems with using them as is:
    -   They call `gridCells->GetCell` without the 1.7.99+ terrain-cell lock.
        Fluid Field already has the locked wrapper
        (`src/Features/FluidField/EngineGrid.h:8-16`). Iterate the grid ourselves
        with it and call `TESObjectCELL::ForEachReference` per attached cell.
    -   The "in range" test is on the reference **origin**
        (`src/RE/T/TESObjectCELL.cpp:24-33`). A long bridge whose origin is far
        from the player would be missed. Use the 3D world bound instead (below).
-   `TESObjectCELL::ForEachReference` holds the cell's `spinLock` for the whole
    walk (`src/RE/T/TESObjectCELL.cpp:13-22`; `CL/T/TESObjectCELL.h:233,239`).
    Do no mesh work inside the callback: collect
    `NiPointer<TESObjectREFR>` candidates, release the lock, then process.
-   Existing CS pattern for the same walk and the marker filter:
    `src/CSEditor/LightPicker.cpp:21-26,153-158`.

**Cheap filters (in order).**

1. `ref->IsDisabled()`, `IsDeleted()` (`CL/T/TESObjectREFR.h:452`); skip.
2. Base form type (`ref->GetBaseObject()`, `CL/T/TESObjectREFR.h:384`;
   `CL/F/FormTypes.h:174-178`):
    - keep `Static` (STAT), `MovableStatic` (MSTT), `Tree` (TREE), and possibly
      `Activator`/`Furniture` with collision (docks, mills; UNVERIFIED how many
      there are near water);
    - skip actors, `Misc`, weapons, armour, ingredients, `Light`, `Grass`, `Flora`,
      `SoundMarker`, `IdleMarker`, and STATs with the `kIsMarker` flag
      (`CL/T/TESObjectSTAT.h:49`, as `LightPicker.cpp:21-26` does).
    - `StaticCollection` (SCOL): its references expand to a single 3D tree at
      runtime (UNVERIFIED), so it can be treated like STAT.
3. `NiAVObject* root = ref->Get3D()` (`CL/T/TESObjectREFR.h:376`); skip null.
   Skip roots with `kHidden` (`CL/N/NiAVObject.h:67`).
4. **Vertical overlap test** with `root->worldBound` (`CL/N/NiAVObject.h:171`,
   centre + radius): keep the reference only if
   `|centre.z − waterZ| ≤ radius` for some water plane, and the bound's XY disc
   overlaps that water object's `multiBounds` AABB (`CL/T/TESWaterObject.h:27-36`)
   and the simulation grid. This is one compare per reference and removes nearly
   everything (rooftops, hillside rocks, interiors of houses).
5. Optional "is solid" gate: the reference has a collision object on a static
   layer (`root->GetCollisionObject()`, `CL/N/NiAVObject.h:133`; layers as in
   `reports/r1-route-run.md`). Reeds, lily pads and most decorative foliage have
   no collision (UNVERIFIED but typical of vanilla), so this one test removes the
   "alpha-tested foliage blocks water" problem (section 5).

Distant LOD is not under `ref->Get3D()` of loaded references (it lives in the
terrain LOD nodes), so walking loaded references' 3D skips it by construction.
Effects (waterfall spray, splash meshes) are usually MSTT or ACTI with
`BSEffectShaderProperty`; skip geometry whose shader property is not
`BSLightingShaderProperty` (`CL/B/BSGeometry.h:105-116`).

## 2. Getting triangles on the CPU

### 2.1 What CommonLib exposes

-   `BSGeometry::GetGeometryRuntimeData()` (`CL/B/BSGeometry.h:58-70,101`):
    `alphaProperty`, `shaderProperty`, `skinInstance`,
    `BSGraphics::TriShape* rendererData` (0x18) and `vertexDesc` (0x28).
-   `BSGeometry::GetType()` (`CL/B/BSGeometry.h:25-42,103`) distinguishes
    `kTriShape`, `kDynamicTriShape`, `kMultiIndexTriShape`, `kMeshLODTriShape`
    and so on.
-   `BSTriShape::GetTrishapeRuntimeData()`: `triangleCount`, `vertexCount`
    (`CL/B/BSTriShape.h:15-24,38`; SE offset 0x158). Both are `uint16`.
-   `BSGraphics::TriShape` (declared in `CL/N/NiSkinPartition.h:14-24`):

    | Offset | Field           | Meaning                                    |
    | ------ | --------------- | ------------------------------------------ |
    | 0x00   | `vertexBuffer`  | `ID3D11Buffer*`                            |
    | 0x08   | `indexBuffer`   | `ID3D11Buffer*`                            |
    | 0x10   | `vertexDesc`    | packed `BSGraphics::VertexDesc`            |
    | 0x18   | `refCount`      |                                            |
    | 0x20   | `rawVertexData` | `uint8_t*`, CPU copy of the vertex block   |
    | 0x28   | `rawIndexData`  | `uint16_t*`, CPU triangle list (3 per tri) |

    SKSE64's own reverse-engineered `BSGeometryData` has the same layout, with
    `vertices` at 0x20 and `triangles` at 0x28 (`skse64/NiGeometry.h`,
    ianpatt/skse64).

-   `BSGraphics::VertexDesc` (`CL/V/VertexDesc.h:54-134`): `HasFlag`,
    `GetFlags` (flags at bit 44), `GetAttributeOffset(VA_POSITION)`.
    The vertex **stride** is nibble 0 × 4; CS already decodes it that way
    (`src/Features/GrassOptimizations/GrassMeshLibrary.h:7`). Prefer it over
    `VertexDesc::GetSize()`, which hard-codes a 16-byte position
    (`CL/V/VertexDesc.h:97-128`).
-   `BSDynamicTriShape::GetDynamicTrishapeRuntimeData().dynamicData`
    (`CL/B/BSDynamicTriShape.h:15-27,39`): CPU positions (16 bytes each) under a
    `lock`. Used for faces and morphs, not statics; can be skipped.
-   `BSMultiIndexTriShape` (`CL/B/BSMultiIndexTriShape.h:20-60`) derives from
    `BSTriShape` and uses the same `rendererData`; its extra index buffer is for
    the snow/moss material projection. Treat it as a `BSTriShape`.
-   Legacy `NiTriShape` (`CL/N/NiTriShape.h:7`) keeps CPU arrays in
    `NiGeometryData` (`vertex` at 0x28, `CL/N/NiGeometryData.h` members). SSE
    meshes are `BSTriShape`; whether old-format statics are converted to
    `BSTriShape` at load is UNVERIFIED, so handle `NiTriShape` only if the
    prototype ever meets one.

### 2.2 Are the CPU copies kept for static meshes?

Evidence they are:

-   **Community Shaders reads `rawVertexData` at runtime** in Light Limit Fix
    (`src/Features/LightLimitFix.cpp:990-999`, decoder at `:820-850`). It guards
    against null, and only for particle-light effect meshes, so this proves the
    field is valid in SE/AE but not that every static keeps it.
-   **HIGGS** (adamhynek/higgs, Skyrim VR, same engine family) builds world-space
    triangle lists for grabbed objects directly from `geometryData->vertices` and
    `->triangles`, with stride `(vertexDesc & 0xF) * 4` and
    `GetVertexAttributeOffset(VA_POSITION)`, then applies `m_worldTransform`
    (`src/math_utils.cpp` `UpdateTriangles` / `GetTriangles`, about lines
    1824-1900). It treats a null `geometryData` as "probably skinned", and reads
    skinned meshes through `NiSkinPartition::Partition::buffData` instead.
-   The engine itself places blood and impact decals on static geometry, which
    needs CPU triangles (UNVERIFIED that the decal code reads `rawVertexData`).

**Verdict:** very likely kept for non-skinned `BSTriShape`s, still UNVERIFIED
for every static. Prototype stage 0 (section 6) counts null pointers on real
references before anything else is built.

### 2.3 Decoding positions

-   SSE NIFs always store the position as a full `Vector3` plus a float
    (bitangent X or unused W) when `VF_VERTEX` is set. Only the Fallout 4 format
    honours the full-precision flag 0x400 and uses half floats (nif.xml
    `BSVertexDataSSE`, lines 2132-2140, versus `BSVertexData` lines 2111-2119,
    niftools/nifxml). So on SSE/AE: `pos = *(NiPoint3*)(raw + i*stride + posOffset)`.
-   Defensive check: if `VF_FULLPREC` (`CL/V/VertexDesc.h:39`) is clear **and**
    the stride implies an 8-byte position, decode half floats instead
    (UNVERIFIED that this ever happens at runtime).
-   Positions are local to the geometry node; transform by `geom->world`
    (`CL/N/NiAVObject.h:169`, rotation × scale + translation).
-   **Skinned meshes** (`skinInstance` non-null) need bone transforms; skip them.
    Statics are not skinned.

### 2.4 GPU staging readback (only if 2.2 fails)

-   `rendererData->vertexBuffer` / `indexBuffer` are the engine's
    `ID3D11Buffer`s; CS already takes them for its own draws
    (`src/Features/GrassOptimizations/GrassMeshLibrary.cpp:97-99`).
-   `CopyResource` / `CopySubresourceRegion` into a `D3D11_USAGE_STAGING` buffer
    with `CPU_ACCESS_READ` works whether the source is `DEFAULT` or `IMMUTABLE`;
    only the destination may not be immutable. Then `Map(D3D11_MAP_READ)`.
-   Pitfalls:
    -   It must run on the thread that owns the immediate context (the render
        submission thread, which is the main thread in SE/AE; UNVERIFIED for
        every hook point), never from a worker.
    -   `Map` stalls until the copy completes; issue the copy one frame and map
        it two or three frames later (a small ring of staging buffers).
    -   One copy per shape; a 10k-vertex shape at 28-48 bytes per vertex is
        about 0.3-0.5 MB, so the bandwidth is fine but the latency is frames.
    -   Same layout decoding as 2.3.
-   Not needed unless stage 0 shows null `rawVertexData` on most statics.

## 3. Collision geometry as a fallback

Collision is CPU-resident by nature (Havok runs on the CPU), and CommonLib
exposes enough to walk it:

-   Path to the shape: `root->GetCollisionObject()` → `bhkNiCollisionObject::body`
    → `bhkRigidBody` → `hkpRigidBody` (`referencedObject`) →
    `collidable.GetShape()`. CS already walks this path in
    `src/Utils/ActorUtils.cpp:7-24` (note the `hkpListShape` cast bug recorded
    in `LEARNINGS.md`; do not copy it).
-   **The useful API is the BV tree query.** `hkpBvTreeShape::QueryAabb(aabb, hits)`
    (`CL/H/hkpBvTreeShape.h:30-31`) returns the shape keys of just the triangles
    whose bounds overlap an AABB. Query with a thin slab
    `[x0,x1]×[y0,y1]×[waterZ−δ, waterZ+δ]` (in Havok units, ×0.0142875 as
    `src/CSEditor/LightPicker.cpp:17` uses) and get only the water-line
    triangles. `hkpMoppBvTreeShape` (`CL/H/hkpMoppBvTreeShape.h:8-31`) is the
    usual wrapper for static meshes.
-   Fetch each triangle with `GetContainer()->GetChildShape(key, buffer)`
    (`CL/H/hkpShape.h:109`; `CL/H/hkpShapeContainer.h:22-27`;
    `CL/H/hkpShapeBuffer.h`). That returns an `hkpTriangleShape`, which has
    **no CommonLib header**; its vertices are three `hkVector4` at 0x30 in Havok
    2010 (UNVERIFIED offset; must be checked once in the debugger).
-   Direct decoding is also possible: `hkpCompressedMeshShape`
    (`CL/H/hkpCompressedMeshShape.h:30-110`) has `chunks` (quantised `uint16`
    vertices × `error` + chunk `offset`, strip `indices`, `transformIndex`),
    `bigVertices`/`bigTriangles` and `convexPieces`. The NIF-side copy is
    `bhkCompressedMeshShapeData` (`CL/B/bhkCompressedMeshShapeData.h:26-50`),
    kept by `bhkCompressedMeshShape::data` (`CL/B/bhkCompressedMeshShape.h:37`).
-   Primitive shapes: `hkpBoxShape::halfExtents` (`CL/H/hkpBoxShape.h:31`),
    `hkpConvexVerticesShape::rotatedVertices` / `planeEquations`
    (`CL/H/hkpConvexVerticesShape.h:37-43`; the plane equations give an exact
    convex cross-section), `hkpListShape` children, and
    `kConvexTransform` / `kTransform` wrappers (`CL/H/hkpShapeType.h:12-35`).
-   Thread safety: read under `bhkWorld::worldLock` (as for raycasts,
    `LEARNINGS.md` Threading).

Caveat: collision is often simplified. Bridge arches are usually modelled
(players walk under them), but thin posts may be boxes or missing, and some
meshes have collision that differs from the visuals. It answers "can the
player pass here", which is close to "can water pass here", and it ignores
reeds automatically.

## 4. The cut and the rasterisation

**Per reference, per water plane at height h** (all in world space):

1. For each shape under the root (recursive `NiNode` walk; for `NiSwitchNode`
   only the active child; skip `kHidden`, decals `kDecal`/`kDynamicDecal`
   (`CL/B/BSShaderProperty.h:80-81`), effect shaders, skinned shapes):
    - transform each vertex's **Z only** first (one dot product), mark vertices
      above/below `h`;
    - for every triangle with vertices on both sides, transform X/Y and emit the
      segment where it crosses `h` (two edge interpolations);
    - triangles lying inside the slab `[h−δ, h+δ]` (flat decks at the water
      line) are emitted as filled triangles instead (δ about 2 units).
2. Rasterise into a per-reference local mask at the grid spacing (8-16 units),
   covering the reference's XY bound plus one cell:
    - **boundary**: mark every cell a segment passes through (conservative
      line rasterisation, e.g. Amanatides-Woo), dilated by one cell so gaps
      smaller than a cell (unwelded seams, non-closed meshes) are sealed;
    - **interior**: flood-fill "open" from the border of the local mask
      (always outside the reference) through unmarked cells. Every cell not
      reached is solid. This needs no closed loops or consistent winding, unlike
      parity scanline fill, and an open arch is open as long as its opening is
      wider than about two cells (16-32 units), which vanilla arches are.
    - Optional loop building (hash segment endpoints, chain into polygons) only
      for the debug log and overlay, not for the mask.
3. Composite into the toroidal obstacle layer: obstacle = OR of all
   reference masks; bed and surface are unchanged (landscape bed stays exact
   from `TESObjectLAND`, `LEARNINGS.md`).

**Multiple water heights.** Cut each candidate against each water object whose
`multiBounds` AABB overlaps its bound (`CL/T/TESWaterObject.h:27-36`), and store
the mask per (reference, plane-height) pair. Within one water object the plane is
flat. To be safe at undersides that sit just under the surface, cut at `h` and
at `h − 8` and OR them; an arch whose lowest edge dips below the surface then
blocks only where it really touches water.

**Riverbed meshes below the surface** are not obstacles; their tops still come
from the raycasts (or later from a downward cut). This approach replaces the
"overhead or solid" decision, not the bed depth.

**Cost estimate (UNVERIFIED, not measured).**

-   Vertex Z pass: one multiply-add triplet per vertex; a 10k-triangle bridge
    (about 6k vertices) is roughly 10-20 µs.
-   Segment emission: only the few hundred triangles that cross `h`.
-   Rasterisation: a bridge's cross-section perimeter of about 2000 units is
    about 250 boundary cells at 8 units; the flood fill of a 600×200-unit local
    mask at 8 units is 1875 cells. Microseconds.
-   Per cell: after the bound test, perhaps 5-30 references touch the water
    (riverside rocks, piers, a bridge, mills). Total roughly 0.1-0.5 ms per
    newly attached cell, sliced across frames under the same time limit as the
    raycasts, then **zero per frame**.

**Caching and invalidation.**

-   Key: `ref->formID` + `Get3D()` pointer + quantised plane height. Store the
    world-space mask tile (or its segments), which never changes for statics.
-   On grid change (the existing poll), drop entries whose cell detached, and
    re-validate entries whose `Get3D()` pointer changed or became null
    (model reload, disable/enable).
-   MSTT with animations (water wheels, mill gears) are cut once at rest; their
    motion is ignored (acceptable for a flow mask).

## 5. Risks

| Risk                                                                                                                                                                                                                      | Mitigation                                                                                                                                                                                              |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **3D detached or replaced while reading.** Model loading is queued to background threads, but attach and detach are done on the main thread (UNVERIFIED). `FluidField::Prepass` runs on the main thread (`LEARNINGS.md`). | Do the cut synchronously inside Prepass. Hold `NiPointer` on the root while reading; `rendererData` also has its own `refCount`. Never cache raw vertex pointers across frames, only the computed mask. |
| **Cell lock held too long**                                                                                                                                                                                               | Collect references under `spinLock`, cut after releasing it.                                                                                                                                            |
| **`rawVertexData` null on some or all statics**                                                                                                                                                                           | Stage 0 measures it. Fall back per shape to collision (section 3), then to GPU staging (2.4).                                                                                                           |
| **Huge meshes** (mod landscapes as meshes, big rock or cliff meshes, merged "static collections" from optimisation tools)                                                                                                 | The Z pass is linear and cheap; cap triangles per frame and resume (time-slicing), and clip to the grid extent before rasterising. Per-shape `uint16` counts cap a shape at 65535 triangles.            |
| **Alpha-tested foliage** (reeds, lilies, hanging moss) has no volume but cuts the plane                                                                                                                                   | Skip geometry with alpha testing (`NiAlphaProperty::GetAlphaTesting`, `CL/N/NiAlphaProperty.h:59`) **and** references without collision; trees keep only trunk shapes if their leaves are alpha-tested. |
| **Decals and BSOrderedNode overlays**                                                                                                                                                                                     | Skip `kDecal` / `kDynamicDecal` shader flags (as HIGGS does).                                                                                                                                           |
| **Effect meshes** (waterfall, splash, mist)                                                                                                                                                                               | Skip non-`BSLightingShaderProperty` geometry.                                                                                                                                                           |
| **Non-closed meshes** (pillars open at the bottom, back faces culled away)                                                                                                                                                | Cross-sections need only the side walls; the dilated boundary + outside flood fill (section 4) tolerates gaps.                                                                                          |
| **Visual vs collision mismatch**                                                                                                                                                                                          | Compare both on the bridge in stage 0; choose per data source.                                                                                                                                          |
| **Memory**                                                                                                                                                                                                                | Masks are bit-packed tiles of a few KB per reference; a few hundred KB per loaded area.                                                                                                                 |
| **Mod compatibility**                                                                                                                                                                                                     | Mods that replace the bridge mesh change the result automatically, which is the point. Engine Fixes or other mods that free CPU geometry would break the visual path (UNVERIFIED that any do).          |

## 6. Recommendation

**Viable.** Use the visual mesh's CPU copy (`rendererData->rawVertexData` /
`rawIndexData`) as the primary source, gated by "has static collision" and the
filters in section 1. Keep collision (BV tree slab query) as the fallback and
cross-check; keep GPU staging readback as the last resort only. Keep the
landscape bed from `TESObjectLAND`, and keep the raycasts only for riverbed
depth under water (or drop them later if a downward cut proves enough).

**Staged prototype plan**, each behind a debug setting with a log check:

0. **Data probe (no simulation changes).** On a key press or once after load,
   walk the attached cells and log per reference: base type, shape count,
   `rawVertexData`/`rawIndexData` null or not, `vertexDesc` flags and stride,
   triangle count, whether it has collision and its root shape type. Pass:
   most STAT shapes near water have non-null CPU data.
1. **Riverwood bridge cross-section.** For reference `00013307`, cut at the
   Riverwood water height and log the segment count, the chained loops (number,
   vertex count, bounding boxes) and the time taken. Also cut its collision with
   a slab `QueryAabb`. Pass: one closed loop per pillar, nothing across the
   arch openings, both sources agreeing within a cell or two.
2. **Overlay only.** Rasterise all candidate references into a new obstacle
   view in the layer overlay next to the Raycasts image; compare with the
   orange/purple raycast classification under each arch (one screenshot per
   arch).
3. **Feed the solver.** Replace the raycast "overhead vs solid" decision with
   the mesh-cut obstacle mask; keep raycasts for bed depth. Re-run the round-3
   checks: water runs unbroken under the arches and reflects off the pillars.
4. **Cache and invalidation.** Per-reference cache with the grid-change poll,
   time-sliced cutting after a load, Tracy zones; check the cost on the
   Riverwood–Whiterun route.
5. **Hardening.** Foliage and decal filters, collision fallback for null CPU
   data, multiple water planes, interiors.

---

Sources:

-   [ianpatt/skse64 `skse64/NiGeometry.h`](https://github.com/ianpatt/skse64/blob/master/skse64/NiGeometry.h) (`BSGeometryData` layout)
-   [adamhynek/higgs `src/math_utils.cpp`](https://github.com/adamhynek/higgs/blob/master/src/math_utils.cpp) (`UpdateTriangles`, `GetTriangles`, `IsIgnorableGeometry`)
-   [niftools/nifxml `nif.xml`](https://github.com/niftools/nifxml/blob/develop/nif.xml) (`BSVertexDataSSE`, `BSVertexData`)
