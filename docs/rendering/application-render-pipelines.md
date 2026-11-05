# Application-owned render pipelines

An application passes its pipeline to the existing `RenderModule` type. The
module owns GPU devices, queues, swapchain, frame fences and graph execution;
the pipeline owns pass instances and their composition. The two-argument
constructor selects `DefaultRenderPipeline`, which has no Bistro techniques.

```cpp
auto scenes = std::make_shared<SceneManagerModule>(scenePath);
auto pipeline = std::make_unique<BistroRenderPipeline>(scenes);
auto renderer = std::make_shared<RenderModule>(window, timer, std::move(pipeline));
locator.RegisterModule(renderer);
locator.RegisterModule(scenes);
```

`IRenderPipeline::Initialize` creates and sets up passes, links their inputs and
initializes resources. `BuildGraph` adds those existing passes to the existing
`GDX12RenderGraph`; it chooses their links, dependencies and outputs. Changing
an application pipeline requires compiling that application's C++ code, but
does not require adding a stage hook to the engine.

Each pass declares logical reads/writes, fallbacks and side effects through
`RecordRenderGraph`. Its `GetBufferAccesses` declares current physical buffers,
which can change with frame slots or resizing. Use explicit graph dependencies
for required order that resource declarations do not express. The pipeline
does not assign inter-queue fence values; the existing executor remains
responsible for supported queue synchronization.

`GetBindings().PrimaryPasses` and `SecondaryPasses` list the pipeline-owned
techniques whose flags drive common camera/geometry/material allocations.
Include a custom pass there when it requires those services, and perform its
setup, initialization, resize and dependency cleanup in the pipeline lifecycle.
Adding it only to `BuildGraph` does not initialize its resources or propagate
its flags. The bindings also declare reconstruction/FG capabilities explicitly.

For example, an application-owned pass can sit between an HDR producer and a
post-process consumer. Its implementation owns the shader/resources and
declares those accesses; composition only selects links:

```cpp
applicationPass.SetInputs({ lighting.GetOutput(0) });
postProcess.SetInputs({ applicationPass.GetOutput(0), opaque.GetOutput(2) });
graph.AddPass(lighting);
graph.AddPass(applicationPass);
graph.AddPass(postProcess);
```

Moving the `AddPass` call alone does not change the resource read by a consumer.
AO similarly requires depth/normals rebinding before the CPU view preparation
for that frame, as well as the corresponding graph order.

`UpdateFrameSettings` runs before frame/camera uploads, so a reconstruction
change and resize apply to the same frame. After the frame-slot wait and scene
update, `PrepareFrame` prepares techniques and cache metadata and can request a
graph rebuild. Building or rebuilding the graph never advances simulation.
Resize waits for the existing GPU work and destroys the graph before changing
pass resources. Shutdown retires GPU work, then destroys graph, pipeline state
and finally devices. Pass objects and links must outlive their compiled graph.

`CommonRenderPasses` shares ownership/lifecycle of common techniques. It does
not determine Bistro's pass order: Default and Bistro each link and add their
passes explicitly. AO placement belongs to those builders; changing it also
rebinds depth/normals before the lighting consumer. Reconstruction/FG queries
use explicit pipeline capabilities rather than pass names or a Bistro cast.

Water, grass and their wind state belong to Bistro. Common particles, sky,
AO/SSR, skinning, shadows and post-processing remain reusable engine
techniques. Each project chooses its own simulation policy; physics does not
depend on render visibility.

## Geometry sources and prepared views

`GDX12RenderPrimitive` records bounds, current/previous transform entry,
material/instance range, participation flags and revisions used by common
visibility and shadow/probe caches. Handles contain a generation, so removing
an owner and reusing its slot cannot revive a stale primitive. Sources publish
or update these records before common scene/cache preparation and remove them
when their owner disappears:

```cpp
GDX12RenderPrimitive primitive;
primitive.Source = &source;
primitive.Bounds = worldBounds;
primitive.Dynamic = true;
auto handle = resources.Primitives.Add(primitive);
resources.GeometrySources.push_back(&source);
// Later snapshots update the same handle; shutdown removes it and the borrowed source.
resources.Primitives.Update(handle, primitive);
resources.Primitives.Remove(handle);
```

An `IRenderGeometrySource` gathers CPU batch descriptions for a supplied
`GDX12GeometryContext`. It respects the view stage and static/dynamic filter,
including independent shadow and reflection participation. Its batch contains
geometry or explicit vertex/index views, topology, instance range, graphics
binding and direct/indexed/indirect arguments. `GDX12GraphicsBindings` describes
the actual supported stage, RTV/DSV formats, root resources and indirect layout;
the shared emitter validates compatibility and issues the draw. A binding may
set graphics state/resources only. Gathering and binding do not dispatch or
draw. Existing static and skeletal meshes retain their grouped GPU-culling
streams and use this same emitter; publication does not create one draw per
primitive or instance.

Batch physical reads must also appear in the source's
`GetGeometryBufferAccesses(stage)` union for the current frame. GPU preparation
uses ordinary graph passes with their own real buffer accesses. Resources,
bindings and constant uploads remain alive until every referencing submission
retires; CPU descriptions do not transfer their ownership to the graph.

`GDX12GeometryViewPass` exposes the actual views selected during CPU
preparation. `GetGeometryView(slot)` returns null for an inactive/cache-hit
slot. Its normal `Execute` performs Begin, selected views and End. A pipeline
can split that lifecycle into a head, `GDX12GeometryViewJobPass`, source
preparation and `GDX12GeometryBatchPass` jobs, then
`GDX12GeometryFinalizePass`. The containing technique owns target clearing,
viewport/scissor and final cache publication. Finalization republishes its
logical outputs after all source ranges finish, so downstream consumers cannot
run after only the mesh portion.

Bistro grass prepares a frozen CPU frame snapshot, roots/wind GPU work and
per-view/per-patch expansion. It reuses one direct-queue geometry arena in the
order `expand A -> consume A -> expand B -> consume B`; dependencies protect
the earlier consumer before the next write. Constants and roots retain their
frame-slot lifetime. The graph keeps stable slots for view topology and patch
count; changing dirty faces or a probe's face budget does not by itself rebuild
the graph. Standalone technique execution remains available without the
split jobs. A different application's source that needs GPU preparation is
responsible for composing that work before its consumers.

Graph replacement builds the new graph while the old one can still reference
its job objects. Bistro retains those old objects until the old graph has been
destroyed. Neither rebuilding nor collecting batches advances simulation.

## Bistro water composition

`BistroWaterRendering` owns fluid snapshots, GPU simulation/surface/resolve,
Far transparency and its HDR composition. Its component pool is registered
explicitly through `RegisterBistroWaterComponents`; standalone Bistro world
fixtures call this before accessing that pool. World listeners assign a fresh
GPU owner identity on creation/replacement, so a reused entity with the same
immutable particle seed cannot retain the previous owner's simulation. Pause
and world time scale determine the collected delta.

Bistro connects the common techniques itself:

```text
opaque depth/motion -> fluid surface -> selected scene depth/motion
lighting/SSR -> Far transparency -> composition -> water resolve
water resolve -> Near transparency -> particles -> final composition -> post
```

SSR continues reading opaque depth/normals/motion. Particle drawing, DOF,
motion blur, reconstruction and frame-generation inputs use the selected scene
depth/motion. Common WBOIT exposes `DepthPartition::All/Far/Near` and an optional
separator depth; it does not identify a water component.

The Far composition applies opaque aerial perspective. Only final composition
receives the particles pass, and Bistro disables repeated opaque aerial
perspective there. The two HDR targets are distinct: when water is empty or
disabled, resolve aliases the Far HDR, which must not also be the final pass's
write target. The Near pass still executes when particle output is required.

Surface outputs include selected depth/motion and optional geometry/owner
links. Resolve explicitly declares these graph reads, original opaque depth
and the reflection cache; no app-specific shared graph token is required.
`SetInstances` and CPU-only `PrepareFrame` allocate/retire state before physical
buffer accesses are queried. GPU simulation remains in surface execution.
Resolve refreshes its forwarding alias during preparation, including after
surface target release. Empty internal links remain graph dependencies without
becoming mandatory initialized inputs to the common pass lifecycle.

## Existing output configurations

The shared `Config/Engine.ini` contains only engine/base CVars. Bistro retains
its C++ defaults and accepts existing `Render.Grass`, `Render.Wind` and
`Render.Fluid` values. Strict unknown-key validation and the build's
copy-if-absent behavior remain unchanged.

Close a non-Bistro application before migrating an old output config:

```powershell
./tools/Remove-BistroRenderDefaults.ps1 -Path ./Build/bin/x64/Debug/App.Editor/Config/Engine.ini
```

The command removes only `Enabled` and `Quality` for those three Bistro
techniques. It first saves `Engine.ini.bistro-backup` and refuses to overwrite
an existing backup. Other settings, comments and line endings are retained.
Restore the backup to undo the migration. Leave Bistro configs alone when
retaining their custom effect settings.

See [validation record](../reports/2026-11-05-app-render-pipelines-validation.md)
for the exact checkout, imported execution baseline and observed checks.
