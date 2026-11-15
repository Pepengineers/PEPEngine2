# Meta local shadows: receiver selection, depth-only draws and caching

This implements the first three recommendations from the shadow-cost diagnosis.
The earlier agreement to defer temporal local-shadow caching is superseded by
the user's explicit request. Sun-shadow optimization is outside this change.

## Receiver and face selection

A shared scene BVH contains opaque and masked geometry, including receivers
that do not cast shadows. A receiver job clips each bounding box against the
camera frustum. Intersections use edges of both convex volumes; wholly contained
boxes take a fast path and uncertain numerical cases retain the original box.
Light jobs query visible receiver bounds against range and face frusta. Bounds
are expanded for the largest normal offset used by `LocalVisibility`, including
point-light face selection after bias. A GPU face mask prevents sampling slices
that were not selected. The opt-in flag leaves production App's full-map path
unchanged.

This is conservative geometry-bounds selection, not exact visible-pixel or
occlusion culling. It can retain extra lights/faces. Casters for needed faces
are gathered separately across the entire BVH, so off-camera objects still cast
into visible receivers. No GPU-to-CPU transfer is introduced.

## Opaque and masked shadow draws

Each cached face has separate opaque and masked command lists. Opaque uses a
vertex-only depth PSO; its shader variant omits UV/material pixel outputs and
there is no pixel shader. Masked geometry retains the original alpha-test PSO.
Both groups draw into the same face after one clear. Bias and PCF stay unchanged.

## Cache ownership and invalidation

Each frame slot owns its array, stable light slice allocations and CPU command
lists. Camera movement only changes needed-face masks. Cached maps need no
clear, draw or command upload. Newly needed faces are prepared before sampling.
Map validity advances only after successful frame submission; slot reuse still
waits for its existing frame fence. Atlas recreation invalidates stored maps.

Light generation, shadow revision, projection or type changes invalidate that
light's lists/maps. Explicit EveryFrame mode forces map updates. Scene mesh,
instance, material, relevant world matrix and geometry revisions invalidate
scene caches conservatively. The shared TransformCache also contains cameras;
comparing only mesh-referenced matrices avoids rebaking maps on camera movement
or previous-matrix settling. Material dirty revisions and texture-resource
rebinding invalidate maps even when GPU material constants remain equal.
In-place texture uploads must mark the material dirty or invalidate the light;
resource identity alone cannot detect writes inside the same allocation.

Lighting publishes stable slices and selected face bits through the existing
light structure. Arrays reserve capacity for authored shadow faces, up to the
D3D12 2048-slice limit. DSVs are retained only for faces updated in the current
slot use, so visiting additional cached lights does not retain more descriptors. If residency
fills while visiting different lights, allocation resets and currently needed
maps refresh from retained CPU lists. A simultaneously visible set beyond that
limit retains the existing explicit error instead of silently dropping shadows.
Light-set topology or light-type changes reset mappings. Resolution changes recreate maps.

## Validation

Release x64 dependencies were rebuilt and the final executable build passed.
SDK DXC compiled the opaque depth VS, masked VS/PS and opaque deferred CS variants.
`git diff --check` passed. Source review found no actionable issues.
Verification is limited to source review and these builds;
the user will inspect the image and request PIX capture in the diagnosis chat.
No tests were added or expanded, and no automated runtime checks, performance
measurements or captures are part of this implementation. The existing
LocalLightingTests ownership of light revision/update rules was inspected;
those shared semantics remain in `ShouldRenderLocalShadow`.

The final build also exposed an existing build-script problem: preparing Agility
SDK tried to overwrite identical DLLs held by the other chat's renamed preview.
`Prepare-AgilitySDK.ps1` now compares SHA256 before copying, using its existing
.NET hashing approach. Changed runtimes still require a successful copy. The
previously blocked build completed without closing that preview.

The other chat concurrently replaces Bistro point fixtures with paired spots
in `SceneManagerModule.cpp`. That edit is outside this task and its commit.
Consequently a later capture also reflects that separate scene change; it
cannot isolate these implementation changes against the earlier point-light
capture without an explicitly controlled comparison.
