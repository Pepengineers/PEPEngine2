# Bistro App

The scene includes four frozen local reflection cubemaps, one entrance grass
patch with wind, one small water basin, four walking human NPCs from the MGPU
sample and one walking Khronos fox inside. Application-only runtime CVars control
the populations; zero removes the corresponding objects:

| CVar | Default | Range |
|---|---:|---:|
| `App.ReflectionProbes.Count` | 4 | 0..32 |
| `App.Grass.Count` | 1 | 0..16 |
| `App.Water.Count` | 1 | 0..8 |
| `App.NPC.Count` | 4 | 0..16 |
| `App.Fox.Count` | 1 | 0..8 |

Reflection probes follow the terrace tables. `App.ReflectionProbes.ShowSpheres`
shows their baked cubemaps on mirrored spheres; `App.ReflectionProbes.ShowBounds`
independently shows the colored influence boxes and labels. Preview spheres are
excluded from captures and shadow casting. Both switches default to true. Each
sphere samples only its probe's published cubemap, starting at the first populated
mip; enabling previews does not trigger a bake. The HDR preview pass runs after
SSSR, respects opaque depth and skips probes without a published map.
These debug spheres do not write depth or motion, so depth-based post effects
still use the underlying scene.

Grass bounds and appearance are controlled by `App.Grass.ExtentsXCm`,
`ExtentsZCm`, `DensityPerSquareMetre`, `MaxBlades`, `HeightCm`, `WidthCm` and the
other [grass/wind settings](../../docs/rendering/bistro-grass-controls.md).
For particles, `App.Particles.Count=-1` preserves the existing `CountScale`, `0`
disables the effects, and positive values select the exact total. The
[particle settings](../../docs/rendering/bistro-particle-controls.md) also expose
the simulation integrator and a bounded emitter-local box with configurable
X/Y/Z half-extents.

Use the console/settings controls or the App executable's own
`[ConsoleVariables]` INI section. Search by `App.Grass.`, `App.Particles.` or
`App.ReflectionProbes.` to find these controls. These names are
registered by Bistro App, so keep them out of other executables' shared INI files.
Reflection maps bake during initial warmup and remain static until explicitly
invalidated or their capture settings/topology change. They exclude moving props
and characters and do not repeatedly capture the scene. This is an in-memory
bake, without an offline disk cache. Character routes are simple bounded loops;
they respect world pause/time scale and use the existing GPU skinning pipeline.
Human asset provenance is in `Assets/Models/MGPUSoldier/SOURCE.md`.

Separate development tasks are in `docs/tasks/bistro-additions/README.md`.
Build `BistroAdditionsTests.vcxproj` for the integrated count, animation and GPU
visibility checks; images are written under `Build/BistroAdditions`.

Set **App** as the startup project in `PEPEngine2.sln`, select **Release | x64**, and run.
Build from a Visual Studio developer prompt:

```powershell
msbuild Apps/App/App.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

Executable: `Build/bin/x64/Release/App/App.exe`. The existing asset-folder setting
resolves assets independently of the working directory.

The **Settings** ImGui window opens with the application; **F1** hides or
reopens it. Search by CVar name, expand an effect group, and hover a control for
its description and valid range. Checkboxes, numeric fields and enum lists apply
immediately. Rejected values leave the previous setting intact and show an error.
Startup-only settings are locked and marked **restart**. Changes last for this
session; edit `Config/Engine.ini` beside
the executable to keep them. Camera controls yield to ImGui while it captures input.
All registered CVars are discovered every frame. Controls follow their declared
types; groups use everything before the last dot in the name. Enum classes ending
in `Count` get a numeric
dropdown automatically. New settings need no UI edits; use descriptions for help.

Eight particle effects share the App-owned runtime CVar `App.Particles.CountScale`
(default 1; <=0 also uses 1). Use
`App.exe -App.Particles.CountScale=4` for four times the particles in every
emitter, or set the CVar in App.exe's own `Config/Engine.ini`. GPU bursts refill
dead slots to maintain the requested live simulation count from the first frame.
Changing the count resets the pools and waits for GPU work; exclude that frame
from timing. Fractional scales are accepted (0.5 gives 125000); the upper limit
is 4194304/250000. The JSON includes `particle_count_scale` and `particle_budget`.
Fireflies use a 120-vertex volumetric instanced mesh with animated wings; the
other effects use larger billboard sprites.
See [effects and benchmark commands](../../docs/rendering/scene-techniques.md).

Scene loading prepares independent textures through the App's shared JobSystem,
then publishes assets/materials/entities on the owner thread and batches texture
uploads. Rendering starts after the complete scene is ready. OBJ import remains
Assimp-based on a cache miss. Converted engine `Mesh` objects (`*.pepmesh`) and BC7
textures are cached under `Build/AssetCache`. A warm geometry load reads engine vertices,
indices, bounds, materials and skin data directly, without creating an Assimp importer
or converting its scene. Source/MTL size and modification time plus importer options,
Assimp version and native format version invalidate the mesh cache. Writes are atomic;
the reader checks record counts, indices and SHA-256 integrity and rebuilds invalid files.
The old `.assbin-*` caches are no longer used. Native caches are shared by Debug/Release.
Deleting this directory is safe, but the next launch must prepare it again.
BC7 keeps texture dimensions and mip levels; diffuse/opacity inputs that need CPU
alpha merging stay uncompressed. Uploaded CPU texture pixels are released after
the GPU fence completes. The loading window pumps messages, shows its current
stage, and supports Esc/close cancellation while worker tasks finish.

Local shadows cache static geometry separately and compose moving casters over it.
All original lights and shadow resolutions remain enabled. Debug optimizes the
mesh importer and native-cache translation units (with PDBs); other Debug code remains unoptimized.

`Loading.TransientBudgetMiB` defaults to 1024. Set it in the executable's
`Config/Engine.ini`, under `[ConsoleVariables]`, or override it for one launch:

```powershell
Build/bin/x64/Release/App/App.exe -Loading.TransientBudgetMiB=1024
```

The value must be a positive integer and is captured once per scene load. It limits
estimated temporary loading work and pending GPU uploads, rather than total RAM,
resident CPU assets or VRAM. An oversized resource runs alone. With the default
budget, Bistro's two large OBJ files import sequentially; texture preparation is
parallel. Larger budgets can allow both independent imports to run together.

For a reproducible loading measurement, the App can initialize the complete scene,
render frames, wait for GPU completion, write JSON counters and exit:

```powershell
Build/bin/x64/Release/App/App.exe -Loading.TransientBudgetMiB=1024 -Loading.BenchmarkOutput=Build/loading.json -Loading.BenchmarkFrames=121
msbuild Apps/App/LoadingTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
Build/bin/x64/Release/LoadingTests/LoadingTests.exe
```

The report includes initialization/ready times, CPU preparation stages, current and
peak process memory, and average time of frames after the first. The sampled frames
use the normal CPU/GPU pipeline; the measurement waits for the last GPU frame to finish.
`Loading.BenchmarkFrames` accepts 1 (default) through 10000. Output paths are relative
to the launch directory. The tests check CVar validation, bounded/oversized batches,
and identical pixels/mips from serial and parallel WIC decoding. Measurements and
limitations are recorded in `docs/reports/2026-11-01-bistro-loading.md`.

The App loads Exterior and Interior into one world, including their submeshes and
materials, and shares texture uploads by path. Loading fails if either part is
missing or cannot be imported. No benchmark objects or camera splines are added.
The camera starts at eye height just outside the main entrance, facing the Interior.

The existing renderer uses shared vertex/index buffers and bindless material
textures. GPU frustum culling builds separate opaque/masked and transparent
command buffers. One ExecuteIndirect call draws each list; the existing OIT
composition feeds the existing XeSS/output pipeline. Interior now splits 346
existing props (304 bottles, 32 chairs, 10 tables) into independently moving ECS
entities. All material sections of a prop share its transform; geometry and
materials still use the original shared mesh. Exterior and the remaining Interior
geometry stay in their original positions.
Instance and indirect buffers grow once per scene part, rather than once per submesh.

The App owns `FlyingPropComponent`, prop partitioning and scene setup under `src`.
`ConfigureBistroProps` registers its component when a world is created and installs
the world's `SceneMeshFactory` before import. Without this optional factory the
shared loader creates one entity per mesh. The renderer uses the existing
`StaticMeshRenderComponent::ReflectionDynamic` flag for these props.
Register additional ECS component types on the owner thread before scheduling jobs.
Their pools participate in normal queries, entity destruction and world clearing.

The bottle glass/wine materials have explicit opacity in both scene MTL files;
caps and labels retain their source materials. Glass opacity is an artistic
approximation. The shared OIT passes use floating-point weighted accumulation,
multiplicative revealage, and normalized composition for overlapping bottles.
OIT approximates blended transparency; it does not simulate glass refraction.

Opaque geometry writes a G-buffer (albedo/metallic, world normal/roughness,
F0/baked flag, emissive, motion and sampled reverse-Z depth). A CPU job fills a
regular frustum grid, independently of G-buffer depth, with global light masks.
Its Z slabs are built in parallel on the shared JobSystem. The deferred compute
pass selects each pixel's cell and scalarizes the wave's union of sources using
SM6 intrinsics; other devices use a per-lane mask loop. Grid size/depth are startup
ReadOnly CVars in Engine.ini. See [clustered lighting](../../docs/rendering/clustered-lighting.md) for
the algorithm, GPU reference checks and limits. The source cubemap supplies the visible background.
Transparent bottles retain forward OIT and receive the same local-light PBR and PCF shadows.

Both lighting paths use Cook-Torrance PBR with GGX distribution, correlated
Smith visibility, and Schlick Fresnel. Materials use metallic/roughness factors,
RGB specular maps, normal maps, red-channel roughness maps, and emissive maps/colors.
OBJ shininess supplies roughness when there is no roughness factor; Bistro's
`_ddn`/`_ddna` bump slots are imported as RGB tangent-space normal maps.
Normal mapping derives its tangent frame from UV/position derivatives so mirrored
UVs do not need an extra tangent-sign vertex attribute.

Color textures use sRGB views; normal and roughness maps stay linear. Opaque color
and OIT accumulation use floating-point HDR targets. Composition and XeSS retain
linear HDR; the final output pass applies Reinhard and sRGB encoding once.
AMD SSSR and Reflections Denoiser replace opaque indirect specular before OIT;
[controls, conventions and verification](../../docs/sssr.md) describe the integration.
Default directional-light values are in `GDX12MainConstants`.
Baked lighting is preserved without relighting.

The directional light uses four stabilized cascades in a 4096x4096 depth atlas
(2048x2048 per cascade). Logarithmic/uniform splits cover up to 6000 scene units.
Receiver frusta overlap, transitions blend over their last 10%, and the final
cascade fades out. Matrices use the unjittered camera and a light-space texel grid.
GPU culling builds four independent lists of opaque/masked shadow casters from
the complete scene, including off-camera objects. Each cascade uses ExecuteIndirect;
masked materials alpha-test in the depth pass. Transparent bottles receive shadows
and IBL, but do not cast opaque shadows. Shadows use depth/slope/normal bias and
3x3 comparison-sampler PCF, constrained to each atlas tile.

IBL consists of diffuse irradiance, an eight-mip GGX-prefiltered radiance cubemap,
and a split-sum BRDF LUT using the same Smith/Fresnel model as direct lighting.
These resources are generated once on the GPU from `Assets/Textures/Environment/grasscube1024.dds`,
copied from the Multi-GPU-Render-DX12 project's `Data/Common/Textures` folder.
The source is an sRGB BC1 cubemap with six 1024x1024 faces and eleven mip levels.
This is global environment lighting, without local probes or environment occlusion.
Reinhard + sRGB now runs once after HDR DoF and XeSS. Opaque/OIT composition,
FidelityFX DoF and XeSS retain linear RGBA16F HDR. See
[the DoF contract](../../docs/rendering/depth-of-field.md) for settings and checks.

Bistro contains 135 ECS fixture lights (124 point and 11 spot), placed from actual
bulb/glass/lens geometry, including street lamps, string bulbs, chandeliers, wall
lamps and fan globes. `BistroLightPlacements.h` retains the original fixture names.
`TransformComponent` supplies position/rotation; `LightComponent` supplies type,
linear color, intensity, range, spot half-angles, enable/shadow flags and update mode.
Directional light is also an ECS entity. Systems update lights before GPU data upload.

Each light selects `ShadowUpdateMode::Cached` or `EveryFrame`. Cached maps bake on
first use and rebake when the light moves or its shadow projection changes; changes
to intensity/color reuse depth. A system calls `light.InvalidateShadow()` after
changing static casters or when an explicit refresh is desired. EveryFrame lights
render on every frame. Point lights have six maps and spots one, currently 256x256
per view with PCF. Transparent meshes receive shadows but do not cast opaque shadows.
CSM caching also refreshes when the camera-fitted cascade matrices change.
Bistro selects EveryFrame for its lights because its casters move. This increases
shadow rendering work, especially for point lights with six views.

`LocalLightingTests.cpp` checks ECS parameter updates and cache invalidation; the
GPU smoke verifies initial point/spot baking, cached reuse, moving a spot, and
every-frame point updates with actual shadow depth readback.

Controls: right mouse to look; W/A/S/D to fly; Q/E down/up; Shift to move faster;
mouse wheel to change speed; Esc to exit.

Run scene/material and CPU OIT-reference checks with:

```powershell
python Apps/App/verify_scene.py
```

These checks do not replace running the renderer on a DirectX 12 GPU.

`tests/ShadowCascadeTests.cpp` checks the actual cascade matrices, receiver coverage,
depth range, and whole-texel stabilization. Build with the renderer's public include
directories, `/MD`, and `DirectXTK12.lib`.

`tests/LightingGpuSmoke.cpp` runs without loading Bistro or opening a window. It
first checks replacement/removal of an uninitialized renderer, then creates the
CSM/PBR pipeline states, renders one synthetic shadow caster through
GPU culling and ExecuteIndirect, generates IBL on the GPU, and reads back the atlas,
irradiance, radiance mips, and BRDF LUT under the DX12 debug layer. Build it with
the App's include/link settings and the console subsystem.

The BRDF tests compile the same scalar functions as the shaders. In a Visual Studio
developer prompt:

```powershell
cl /nologo /std:c++17 /EHsc /W4 /FoBuild/PBRMathTests.obj /FeBuild/PBRMathTests.exe Apps/App/tests/PBRMathTests.cpp
Build/PBRMathTests.exe
```

## Jobs and update order

The C++17 API is in `Engine.Core/Jobs/JobSystem.h`, backed by the vendored enkiTS
scheduler. Derive from `Job` for a synchronous `Execute()` on the calling thread,
or from `ParallelJob` for independent `Execute(size_t index)` operations:

```cpp
struct Move final : ParallelJob {
    std::vector<float>& positions;
    explicit Move(std::vector<float>& values) : positions(values) {}
    void Execute(size_t i) override { positions[i] += 1.0f; }
};
JobSystem jobs; // Hardware thread count, including the caller; JobSystem(1) is serial.
std::vector<float> positions(4096);
Move move(positions);
auto moved = move.Schedule(jobs, positions.size()); // Pool default, initially 64 elements.
moved.Complete();
move.Schedule(jobs, positions.size(), 32).Complete(); // Override for this invocation.
```

The application's shared pool reads `Jobs.ThreadCount` from `[ConsoleVariables]`
in `Config/Engine.ini` beside the executable. Values `<= 0` select hardware
concurrency; positive values specify the total thread count, including the caller
(`4` means three workers and the caller). The default is `0`. This setting applies
to ECS/object jobs and scene-loading jobs using the same application pool. It is
read-only after startup; restart the application to change the pool size.

`Jobs.BatchSize` sets the application's default minimum batch size in elements
(default `64`, positive integers only). Set it in the same INI and restart to tune
it without rebuilding. `Schedule(jobs, count, dependency)` and
`system.Schedule(jobs, world, dt, dependency)` use the pool default; inserting an
explicit batch-size argument before the dependency overrides it for that call.
Standalone pools can choose a default with `JobSystem(threadCount, batchSize)`.
The ECS/Bistro update chain and initial indirect-template build use the configured
default. Scene import and texture decode retain explicit batches of one item.
The CPU measurement test also uses this configured default and prints it.

enkiTS uses the batch size as a minimum range, except for the final remainder; it
can group more indices into a range. This is scheduling granularity, not byte
alignment. Cache-line length alone cannot determine the best batch size: false
sharing also depends on the base alignment, element strides, actual writes and
the scheduler's range boundaries. The scheduler does not promise cache-aligned
partitions. Choose a batch size for each workload and measure; padding or aligning
data is a separate change when profiling shows cache-line contention.

Pass a preceding `JobHandle` as the last Schedule argument to start a system only
after every batch of its predecessor finishes. `JobHandle::CombineDependencies`
joins handles; `Job::Run(handle)` waits before its synchronous Execute. Complete
propagates job exceptions; failed dependencies skip their dependents. Schedule and
Complete belong to the thread that created the JobSystem. Separate schedulers
cannot share dependencies.

`BistroApp::Update` explicitly schedules movement -> circle -> spline -> flying
props -> look-at -> lights -> GPU object data, then completes the final handle
before rendering. Each system's batches can run across workers; systems retain
their specified order. `App::Update` supplies the default chain for the other apps.
Custom systems can be inserted at the required point in either application's code.
`System::Tick` remains available as a sequential baseline; SceneManager no longer
owns system execution or implicit ordering.

The scheduler passes each complete batch to `ParallelJob::ExecuteRange(begin, end)`.
Its default implementation calls `Execute(index)` for each item, preserving per-index
jobs. `System` overrides this entry point to call `TickRange(world, dt, begin, end)`
once per batch; the ECS query then iterates the objects inside that range.

Keep job objects, component storage and referenced data alive until Complete.
Do not mutate a scheduled job, schedule it again while active, or add/remove
entities/components while jobs use their pools. Each index must write its own
data; dependencies declare cross-job ordering manually. `System::Prepare` runs
immediately on the caller and must not read unfinished predecessor output; put
such work in TickRange. An ECS range uses the dense index of its first component
pool and checks the other sparse components. There is no automatic component
access analysis or system sorting.

At scene initialization, `IndirectCommandBuildSystem` fills all candidate draw
templates in parallel before any frame is submitted. Each template holds draw
arguments and an instance index, including candidates that GPU culling will reject.
It is separate from the device command signature that describes their layout.
Movement updates per-frame transforms, bounds and instance data without rewriting
the shared templates. The GPU generates camera and shadow visible lists separately
and uses ExecuteIndirect for both, including off-camera shadow casters. Command
list recording remains sequential. Changing scene topology requires completing
CPU jobs, waiting for GPU use to finish, resizing/reserving slots and rebuilding
the templates before rendering resumes; per-frame topology edits are outside this
fixed-scene benchmark.

Run the scheduler and sparse ECS checks from the repository root in a Visual Studio
developer prompt (create `Build/tests/job-runtime` and `Build/tests/ecs-jobs` first):

```powershell
cl /nologo /std:c++17 /EHsc /O2 /I Engine/Engine.Core/Include /I External/External.Libraries/enkiTS Engine/Engine.Core/tests/JobSystemTests.cpp Engine/Engine.Core/src/Jobs/JobSystem.cpp External/External.Libraries/enkiTS/TaskScheduler.cpp /FoBuild/tests/job-runtime/ /FeBuild/tests/job-runtime/JobSystemTests.exe
Build/tests/job-runtime/JobSystemTests.exe
cl /nologo /std:c++17 /EHsc /O2 /I Engine/Engine.Core/Include /I External/External.Libraries/enkiTS Engine/Engine.Core/tests/ECSJobTests.cpp Engine/Engine.Core/src/Jobs/JobSystem.cpp External/External.Libraries/enkiTS/TaskScheduler.cpp /FoBuild/tests/ecs-jobs/ /FeBuild/tests/ecs-jobs/ECSJobTests.exe
Build/tests/ecs-jobs/ECSJobTests.exe
msbuild Apps/App/ObjectDataJobsTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m:1
Build/bin/x64/Debug/ObjectDataJobsTests/ObjectDataJobsTests.exe
```

`BistroPropsTests.cpp` checks importer node names, grouping/coverage and bounded
motion. Build with App.Base/Core/Assimp/DirectXTK include directories, MeshImporter.cpp
and MeshTypes.cpp, `/MD`, and release Assimp/DirectXTK libraries. Pass an output
directory and optionally the actual Interior OBJ to verify its 2381 sections are
partitioned exactly once. `ObjectDataJobsTests` loads a small real DX12 scene and
checks selected indirect slots, motion and bounds across all frame slots with
one and four threads, and that movement leaves source templates unchanged.
Its `--check-job-settings` argument verifies the configured thread count, batch
default and startup-only settings without opening a window or initializing the
renderer (`--check-job-threads` remains an alias).
Pass the actual Bistro scene directory as its single argument for a CPU-only
measurement of the real motion and object-data systems (256 samples after warmup,
one/four/hardware threads). Loading still creates the real GPU resources; no draw
or Present is included in those CPU timings. They include Schedule/Complete cost
and do not measure FPS or shadow-rendering cost.

The projects target Visual Studio 2026's v145 toolset.
