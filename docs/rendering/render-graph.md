# Render graph

`GDX12RenderGraph` schedules the primary rendering pipeline. Every primary pass derives from `GDX12RenderPass` and implements `RecordRenderGraph(builder)` alongside `Execute(commands)`. `AddPass(pass)` records the class directly; there are no execution callbacks or captured pipeline-building lambdas.

`RenderModule::ConfigureRenderPipeline` creates the pass objects and connects their input links. `RebuildRenderGraph` imports the scene/swapchain, enqueues those objects (moving AO after the depth prepass when requested), and compiles. Each pass itself declares its reads, writes, transient outputs, hidden cache dependencies and conditional forwarding. Input links are the resource identity, so the renderer no longer maintains a second list of the same resource connections.

## Adding a pass

```cpp
class MyPass final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("My pass");
        graph.ReadInputs(_inputs, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        graph.Write(_outputs[0], "My output", Lifetime::Transient,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    void Execute(GDX12CommandList* commands) override
    {
        // Bind the pipeline, descriptors and constants, then dispatch.
        // Declared input/output transitions are supplied by the graph.
    }
};

// After Setup/Initialize and connecting the input/output links:
graph.AddPass(myPass);
graph.Compile();
// Every frame:
commands = graph.Execute(commands);
```

The pass owns its pipelines and logical texture/view objects. The graph owns the scheduling and physical transient pool. `Initialize` creates texture descriptions/views as before; first allocation is still eager. Passes must outlive the graph, and changing their links requires rebuilding it. Adding the same pass instance twice or a pass without a graph contract is rejected. Failed recording rolls back its resource registrations.

`Lifetime::Transient` permits reuse of a fixed engine-owned texture. `Persistent` excludes a fixed texture from pooling; `Dynamic` resolves a link at execution time for rotating swapchain images, SDK-owned outputs, configurable caches and passthroughs. A composition pass supplies its own storage separately because its public output link can forward the background.

All 22 primary nodes now use these class contracts, including frame-generation preparation. Deferred lighting and tone mapping receive their input/output entry barriers from the graph; their `Execute` methods require graph execution. SDKs, simulation, captures and effects with internal dispatch sequences retain internal barriers. The old cross-adapter utility passes and private reconstruction providers are not independently enqueued in the primary graph.

## Contracts

* `Import` describes initialized external resources or state. Writing an import is an observable side effect.
* `Create` describes a produced resource. `Read` or `ReadWrite` before its first `Write` is rejected. `Write` must fully initialize the contents that subsequent readers use; `ReadWrite` preserves previous contents.
* Access declarations establish RAW, WAR and WAW edges in declaration order. `DependsOn` adds non-resource ordering. Compilation validates cycles, retains exports/side effects and their predecessors, then performs a stable topological sort. Passes are not implicitly reordered ahead of their declared resource versions.
* `AddFallback(output, input)` declares that an output may forward an input. Every downstream read extends the fallback resource's lifetime and retains its producer. Water/no-water and front-composition bypasses use this contract.
* `Export` extends lifetime through the end of graph execution. Resources retained across frames or used asynchronously outside the graph must be non-transient. Frame-generation depth/motion and temporal histories are kept out of the transient pool.
* An access can specify a D3D12 state; the executor inserts its transition. SDK, capture and simulation nodes still manage their internal subpass transitions. Logical tokens cover their persistent resources without exposing them to pooling.

## Transient textures

The compiler computes first/last use in the compiled order. Two textures can occupy one physical slot only when their intervals do not overlap and their native descriptions, device and optimized clear values match. This is reuse of the same `ID3D12Resource`, not overlapping heaps containing differently shaped resources.

Each logical texture keeps its descriptor slots. Compatible textures share a `GDX12TextureResource` and therefore one barrier state tracker and D3D12MA allocation. UAV ordering is inserted at the start of each logical lifetime. The pass still performs the required clear/copy/discard/full UAV write. Existing physical resources seed the pool; recompilation allocates a new resource only when a previously shared slot must be split.

Compilation drains the owning queue before changing bindings. Resize replaces a texture's resource rather than mutating a shared state object. Execution rejects stale bindings. The application rebuilds after resize, upscaler changes, AO placement changes or toggling pooling. Ordinary frames do not compile, allocate pooled textures or rewrite their descriptors. Multiple queued frames remain ordered on the same direct queue.

`Render.Graph.ReuseTextures` defaults to true. Set it to false to keep the same graph and allocate separate textures for comparison. It is a runtime diagnostic, and changing it drains/recompiles the graph.

The old unused depth-clear resource/pass was removed. Behind/front transparency now has separate logical targets; graph lifetimes replace the manually configured sharing in the application's pipeline.

## Diagnostics and checks

The loading benchmark writes a `.json.dot` dependency graph beside its JSON report. JSON includes executed/culled passes, logical/physical transient texture counts and their D3D12 allocation sizes. These sizes are not total resident VRAM; D3D12MA may retain spare heap capacity.

Build the following projects with MSBuild (`/p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /nr:false`), then run their executables from the repository root:

* `Apps/App/RenderGraphTests.vcxproj`: class recording, linked-input validation, failed-recording rollback, dependency hazards, cycles, dead passes, bad accesses, pooled/dedicated pixel identity, queued frames, shared states, descriptor stability, recompilation, resize, fallback/export lifetimes and allocation cleanup, with the D3D12 debug layer.
* `Apps/App/BistroRenderGraphTests.vcxproj`: complete Bistro HDR readbacks match byte-for-byte with pooling off/on/off, in both AO placements. Temporal effects are disabled for this deterministic comparison, then re-enabled for odd resize and Native/XeSS switching under the debug layer.

Particle simulation, fluid simulation and AO can opt into COMPUTE independently; other production passes remain on DIRECT. Cross-adapter async scheduling, pooling of internal SDK resources and buffers, lazy resource creation before pass initialization, and heap aliasing between incompatible texture descriptions are not implemented. New passes must declare hidden reads/writes and conditional forwarding before opting their outputs into pooling.

## Particle async scheduling

`builder.AsyncCompute()` marks eligibility. `Execute(commands, computeQueue)` returns the final DIRECT list, which the caller must submit; omitting the queue preserves sequential DIRECT execution. Each list/allocator belongs to its queue and is recycled by that queue's fence. The presentation/Streamline queue remains DIRECT.

The initial executor supports explicitly declared read-only texture inputs and persistent buffers returned by `GetBufferAccesses()` each frame. This lets particle pools be replaced during pre-graph preparation without caching obsolete physical addresses. Buffer handoffs release to COMMON on their producer and signal/wait the owning fences. Logical dependencies and physical buffer RAW/WAR/WAW force a join. Texture readers can overlap when their explicit read states are subsets of the state published before the fork. A writer, unmanaged access, or incompatible state on the same physical texture joins first. Independent graphics work continues on DIRECT. A side-effect-only branch still joins before execution returns; the caller's final DIRECT submission fence transitively retires both queues' frame inputs. Failed recording drains submitted compute work before resource owners unwind.

Opaque depth transitions to `DEPTH_READ | PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE` on DIRECT before the fork. `BeginConcurrentRead` locks that union: both queues' resource helpers preserve it without emitting read-to-read transitions, and reject writes or layout release until the join. GBuffer binds a read-only DSV, refreshed after resize. AO/CACAO and deferred lighting declare their read states and can share depth with simulation. In Bistro the first declared depth writer is `Reflection probe meshes`, so the join follows GBuffer, AO and deferred lighting. Other unmanaged SDK accesses remain conservative join boundaries. Async resources and their fallback sets have conservative whole-graph texture lifetimes; no aliasing benefit is assumed from linear pass indices across queues.

DOT diagnostics include actual queue selection, physical buffer access identities and fork/compute fence values plus the join boundary. Statistics expose async pass count and COMPUTE duration using its own frequency/query/readback slots; `RenderModule::GetGPUFrameTimeMS()` measures the full DIRECT frame interval including waits. These are durations on their respective queues, not a calibrated cross-queue timeline.

`SecondGroupTests --async` and `--async-gpu-validation` run the particle oracles through this path. Both ordinary and async runs include direct/async/direct queued frames and branches without a draw consumer. `BistroRenderGraphTests` compares HDR across queue/pooling toggles and AO placements; `--gpu-validation` enables GBV. Its separate `--timing` mode disables validation, freezes dt/camera, seeds 250000 particles and records 24 warm-up plus 120 measured frames per direct/async/direct series in `Build/async-bistro-timing.csv`, with DOT files beside it. A per-frame CPU drain makes timer readback unambiguous; this timing mode does not replace the queued-frame correctness tests or establish general FPS gains.

[2026-11-05 validation and A/B report](../research/2026-11-05-particle-async-compute.md) separates the initial exclusive-depth measurements from the shared-depth follow-up. Applications deploy [Agility SDK 1.619.6](../../External/AgilitySDK/README.md) for correct cross-queue read/read validation; the bundled DXC keeps engine shaders at retail SM 6.8. Async remains default-off with DIRECT fallback.

## Multiple producers and fluid simulation

`IsAsyncComputeEnabled()` selects each eligible pass at runtime. Providing a COMPUTE queue does not enable a technique whose own flag is false. The executor retains all pending producers and physical accesses; another COMPUTE pass uses queue order rather than forcing a DIRECT join. Consumers retire only the needed ordered prefix, waiting on its last fence. Later shared-read owners remain locked until their own join. Conflicting shared-texture layouts still join before DIRECT publishes a new state. DEFAULT buffer read/read accesses join conservatively except explicitly declared CBV readers; their shared state remains locked until the last reader retires. Immutable upload reads may overlap. Upload/readback heaps retain their fixed states.

`Render.Fluid.AsyncCompute` defaults to false. Bistro runs the solver before geometry and consumes its same-frame particle state at `Fluid surface`; raster/filter/resolve stay DIRECT. Hidden simulation still retires through the final join. Separate solver/render constant records preserve late history updates, and seed uploads retire before collecting buffer-access pointers. Per-producer DOT fences and `AsyncJoins` expose the actual boundaries; `ComputeTimeMS` sums per-pass timers.

The [water report](../research/2026-11-07-water-async-compute.md) documents controls, lifetime, correctness commands and the full-frame `--matrix` harness. It uses dt=1/60, explicit basin creation, fixed clock/camera, per-series wind/water/particle reset and W/P forward/reverse rows. Its labelled CPU drain is distinct from queued-throughput testing.

`Render.AO.AsyncCompute` defaults to false. Explicit UAV texture outputs hand off through COMMON; their producers and fallback sets retain conservative whole-graph lifetimes. Private AO scratch reads use NON_PIXEL states, and CACAO selects a COMPUTE-compatible final output state. AO settings/history exclude the queue toggle. Water surface declares compatible SRV/CBV reads and publishes actual depth/motion storage in PrepareFrame, so it can retire W while later AO readers continue; resolve stays unmanaged because it copies its background. The [AO report](../research/2026-11-07-ao-async-compute.md) covers contracts, validation and `--matrix --matrix-ao` (W=1, P=2, A=4). That matrix preserves late XeGTAO/quality 2 in every row.

## Class API migration checks, 2026-11-02

After moving all primary contracts into the pass classes, App and RenderGraphTests build in Debug/Release x64. RenderGraphTests passes in both configurations. BistroRenderGraphTests passes again with byte-identical HDR for pooling off/on/off, both AO placements, odd resize and Native/XeSS switching. The pool still uses 9 physical textures for 16 logical transient textures. LightingGpuTests and ReflectionProbeTests (`--debug`) pass after converting their lighting/tone-map calls to graph execution; no D3D12 debug-layer errors were reported. Build/test logs use the `Build/graph-classes-*` prefix.

## Initial implementation measurements, 2026-11-02

App and RenderGraphTests build in Debug/Release x64. Graph CPU/GPU tests pass in both configurations. BistroRenderGraphTests passes with exact HDR identity and no debug-layer errors. The existing Release MemoryAllocatorTests, ReflectionProbeTests (`--debug`), AOTests, SSRTests, DepthOfFieldTests, MotionBlurTests, SecondGroupTests and ReconstructionTests also pass.

The final default Bistro run completed 121 frames (120 measured), exit 0:

| Metric | Result |
| --- | --- |
| Executed graph nodes | 22, including frame-generation preparation |
| Logical / physical transient textures | 16 / 9 |
| Logical / physical transient allocation size | 65.875 / 35.375 MiB |
| Saved transient allocation size | 30.5 MiB |
| All tracked live allocations, versus D3D12MA-only baseline | 227 → 221; 28.75 MiB fewer live bytes |
| D3D12MA heap reservation, versus baseline | 8 MiB larger; allocator spare capacity is retained |
| Mean frame time, versus baseline | 20.2672 → 20.1788 ms; single runs do not establish a speedup |

The off/on comparison comes from the runtime toggles in BistroRenderGraphTests. The application does not parse arbitrary CVar command-line arguments; use the console/INI for `Render.Graph.ReuseTextures`. Logs are `Build/render-graph-*-build.log`, `Build/render-graph-*-test.log`, `Build/render-graph-tests-debug.log` and `Build/render-graph-bistro-tests.log`. The final JSON and dependency graph are `Build/render-graph-final-true.json` and `Build/render-graph-final-true.json.dot`.

No cross-adapter run, asynchronous queue scheduling, prolonged stability test or actual frame-generation-provider validation was performed. Existing conversion and third-party PDB warnings remain.
