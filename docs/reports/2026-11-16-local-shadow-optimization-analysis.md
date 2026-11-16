# Local shadows: cost and quality-preserving optimization

Analysis at `7403eb7d`; no rendering implementation or settings changed.
Uses the two existing user PIX captures from
`Build/frame-audit-2026-11-16-post-water/`. Point lights, face coverage,
resolution, geometry and shadow filtering remain in scope to preserve.

## Evidence

| Local mesh shadow work | A | B |
| --- | ---: | ---: |
| Refreshed depth faces | 67 | 222 |
| Expanded indexed draws | 22,733 | 25,363 |
| ExecuteIndirect GPU replay time | 6.84 ms | 16.64 ms |
| Depth clears, previous counter extraction | 0.43 ms | 1.42 ms |
| VS invocations | not collected | 106,807,394 |
| PS invocations | 494,633 | 6,658,169 |

These are replay counters, not ordinary frame latency. B's additional VS counter
collection replays the capture again, so its 16.64 ms differs slightly from the
earlier 16.66 ms. Counts include opaque/masked mesh draws, exclude the separately
scheduled grass draws. Data and per-face event IDs are in
`Build/frame-audit-2026-11-16-post-water/local-shadow-analysis.json`.

The work is distributed across many faces, rather than a single pathological
draw. B's most expensive mesh face is about 0.188 ms. Large vertex work and many
small indirect draws make geometry submission/vertex processing a stronger
candidate than optimizing only pixel shading; counters alone do not establish
the hardware limiting unit. Opaque depth-only PSO already has no pixel shader
(`SceneLocalShadows.cpp:104`).

## Why geometry is repeated

`LightCache` has one `MapsValid` mask and one opaque/masked command list per face
(`SceneLocalShadows.h:49`). There is no separate static depth in this Meta path.

1. Mesh movement/deformation or a material edit reaches `UpdateCasters`.
2. `Invalidate` clears both list and depth validity in every frame slot whose
   light/face intersects the old/new caster bounds (`SceneLocalShadows.cpp:190`).
3. Grass deformation clears depth validity for intersecting faces (`:206`).
4. `PackJob` selects needed invalid faces and packs their entire caster lists
   (`:475`), without separating static and dynamic casters.
5. `Append` clears the full face and executes its opaque/masked lists (`:700`).
   Thus one changed character can cause static walls/furniture in the same face
   to be rendered again. Grass is drawn afterward into that face.

This is face/bounds-specific invalidation, not unconditional full-atlas redraw.
Camera receiver selection and a BVH already exist. `Freeze` clears the
EveryFrame flag (`:516`), so that flag is not the cause of ordinary repeated work.
Three frame slots each own their atlas/cache (`Config/Engine.ini:11`); cached
static depth and its warmup are currently duplicated across slots.

## Recommended order

### 1. Make invalidation describe shadow changes

`UpdateCasters` compares entire Instance/Material structures and treats any
material journal entry as dirty (`SceneLocalShadows.cpp:282`).
`GDX12DeviceResources::UpdateMaterialCB` explicitly marks dirty material entries
even if their constants are unchanged, to support updated masked alpha contents.
This is necessary for masked materials, but opaque shadow depth does not consume
roughness, metallic, emissive, color or diffuse contents.

Use a shadow-specific comparison: geometry/index range, current world transform,
caster/preview/layer eligibility and deformation; for masked casters also alpha
texture binding and alpha-content revision. Keep reference bookkeeping updated
when ignoring shading-only edits. Do not suppress dirty alpha-content events.
Animated opaque UI textures are a possible beneficiary; the current captures do
not establish their share of dirty faces.

Separate membership/list invalidation from depth invalidation. A pose-only change
with unchanged bounds/command does not require re-traversing the BVH and rebuilding
the static command list. Bounds, eligibility or index-range changes still do.
This principally saves CPU preparation; reducing unnecessary map invalidation
also saves GPU work. No measured saving is claimed yet.

### 2. Cache static depth independently, then draw dynamic casters

For each needed face:

- Rebuild static depth only when its static geometry, masked alpha, light matrix,
  resolution or allocation changes.
- If dynamics changed, copy valid static D32 depth into the existing final face,
  then render all current dynamic casters and grass using the existing PSOs.
- If neither changed, reuse the final face. Restore from static depth when a
  dynamic caster leaves, to remove its previous shadow.

This keeps the same depth minimum before the existing PCF sampling, rather than
combining already filtered visibility values. Preserve draw matrices, bias,
resolution, alpha tests and full point-light coverage. It requires correct
static/dynamic classification and invalidation on every shadow-relevant change;
an object marked static may still be edited and must invalidate the cache.

There is an existing implementation pattern to reuse:
`GDX12LocalShadowPass.h:389` / `BeginDynamicGeometry` copies static subresources
before dynamic drawing. Reuse that algorithm in Meta's explicit graph; do not
invoke its legacy Execute path or add private queue submissions/CPU Flush.

Tradeoff: extra GPU depth storage and copy bandwidth. At 256x256 D32, a face is
256 KiB; copying 222 faces moves about 55.5 MiB of depth payload (read plus write
traffic is larger). Avoid blindly adding a full extra atlas to each of three
slots. Evaluate a shared static cache with executor-owned lifetime/state ordering
or a compact allocation for used faces. Exact gain depends on the as-yet unmeasured
static/dynamic geometry share; do not quote all 16.64 ms as recoverable.

### 3. Batch remaining compatible geometry per face

Both local PSOs use `SHADOW_UNBATCHED`; each command still represents a separate
draw. ExecuteIndirect reduces CPU API calls, not the GPU's draw count. Reuse
existing instanced-geometry/visible-instance machinery for compatible repeated
mesh/index ranges. Preserve per-face visibility, transforms, materials and skin
outputs. This saves draw overhead and repeated instance setup; it does not remove
the vertex work for each distinct shadowed instance.

## What not to change

No point-to-spot replacement, resolution reduction, shadow update throttling,
shorter light range, extra shadow LOD or camera-only caster culling. Off-camera
geometry can cast onto visible receivers. No claim that opaque pixel shaders can
be disabled again; that optimization is already present.

## Next measurement and verification boundary

Before choosing cache size, collect compact per-frame dirty-reason and
static/dynamic command counts per light/face (no GPU readback required). The
current PIX bridge is disabled and these captures do not expose the instance
buffers through the available Windows binding tools, so exact responsible entity
names and static/dynamic percentages were not recovered. New light/face labels
would also make subsequent PIX attribution direct.

No new build, scene run, quality matrix or rendering change was necessary for
this diagnosis. The recommended first implementation is narrow invalidation,
followed by the static-depth split, with ordinary smoke and focused paired
measurements after each independently reviewable stage.
