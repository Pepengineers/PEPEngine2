# U4 — scene geometry, shadows and grass

Date: 2026-11-13 (Europe/Moscow). Host: MidSpec (requested as MisSpec).
Checkout: `C:/Users/MidSpec/Documents/PEPEngine2`.

## Base and result

Accepted base and initial HEAD: `64f0ab1ad31dfea307495d9cfcf6c87e139bca79`, branch `codex/meta-render-graph-u3`. U2 `0ba202a335750fc3f7f1b40ede3253ce2894abd0` is an ancestor. Work branch: `codex/meta-render-graph-u4`. The initial superproject status, ignoring submodules, contained only untracked `Apps/App/App.vcxproj.user`; it is preserved and excluded. Its final SHA256 is recorded in handoff.json. No reset, clean, push, PR, merge or reviewer agent was used.

U4.1–U4.4 are implemented and the checks below passed. Originating-chat review remains the acceptance gate. U5 was not started. First local subgroup commit: `575f2722` (scene snapshots, geometry and skinning). Final result is the local commit containing this report: `git log -1 --format=%H -- docs/reports/meta-render-graph/04-geometry-shadows.md`. Exact base/result HEADs, source SHA256s, binary base-to-U4 patch and artifact manifest are in ignored `Build/Verification/meta-render-graph-u4/handoff.json` and `base-to-u4.patch`.

## Implemented subgroups and APIs

| Subgroup | New packet/access path | GPU evidence |
|---|---|---|
| U4.1 | `Techniques/SceneGeometry`: immutable instance/template/material/transform/main/camera snapshots; four disjoint upload buckets; count reset, unchanged `Culling.hlsl`; static and GPU-batched `ExecuteIndirect`; reversed-Z depth and five GBuffer outputs from unchanged `OpaquePass.hlsl` | Actual ECS meshes, exact snapshots/images, motion, invisible/far/empty visibility; packed draw count and two instances |
| U4.2 | `FreezeSkeletalMeshes` / `GDX12Skinning::Freeze`; explicit palette copy and deformation operations using unchanged `Skinning.hlsl`; source bindings deduplicated, geometry write/read dependencies declared | Two instances share immutable mesh data but use independent palettes/times; expected GPU vertex positions within 0.001; exact serial/parallel vertices |
| U4.3 | `SceneShadows`: four cascade matrices from the existing cascade builder and existing local-light matrices; private constants, static/dynamic counters/commands and depth per waited frame-slot/view | Four cascades, six point faces, spot; independent camera silhouettes, movement/animation, empty local maps; exact raw depth/count comparison |
| U4.4 | `SceneGrass`: existing Bistro ECS collection, BeginFrame/PrepareView, grass settings/LOD, projected/analytic wind and interaction algorithms; frozen native roots/copies/dispatches/expansion/draws with explicit accesses | Grass in main depth/GBuffer, shadow views and six diagnostic probe geometry faces; Ribbon/CrossedRibbons, moving source/patch, pause/reset/no patches, GPU field versions/history |

The application now consumes actual ECS scene data and the engine geometry buffer, rather than the U2/U3 scalar/full-screen fixture. The deterministic registered meshes are small integration fixtures; this is not full Bistro scene acceptance. No CPU draw loop substitutes for ExecuteIndirect. The inherited U3 fixture still runs separately during smoke for its executor/error/fence coverage.

Shared seams:

- `IRenderPipeline::OwnsScenePreparation` defaults to false; only Meta opts in. Legacy RenderModule dispatch is unchanged for other pipelines. The Meta owner freezes skinning work after slot retirement instead of recording the old pre-graph skin dispatch.
- `GDX12GeometryBuffer::FreezeRecordingBinding` exposes read-only VB/IB/resource bindings. Zero geometry returns initialized empty views; Meta supplies separate valid dummy buffers.
- `GDX12SkinningWork` freezes palette/source addresses and PSO/root ownership. `GDX12InstanceBatcher::ReserveRecording`, `RecordingResources` and `RecordStage` separate reservation from native command recording, reusing `InstanceBatching.hlsl`.
- Main-view batching uses a private counter copy, preserving the original culling results for readback. Shadow views consume individual GPU-generated commands with the opt-in `SHADOW_UNBATCHED` shader define; production shader compilation retains the original path.
- Grass friendship permits the experimental adapter to freeze existing prepared jobs/outputs. `PrepareView` optionally accepts the experimental material descriptor. The optional buffer-descriptor start retains the production default. Meta uses index 105000, after the existing texture/cubemap ranges, to avoid exhausting the small render-target SRV range.

## Ownership, states and ordering

Before workers start, the owner reserves lists, resources, descriptors, source ranges, per-view parameters and output slots. Scene uploads, including padded main/camera constants, are copied to private per-slot staging/default buffers. Workers issue native commands and do not allocate descriptors, access ECS, submit queues, wait for GPU or mutate shared resource trackers. One renderer owner submits all frame packets on one DIRECT queue. Existing cold mesh/texture/immutable skin-source initialization remains sequential on that owner and can submit/flush during resource creation; it is not a worker or hidden legacy graph execution.

DEFAULT buffers declare decay to COMMON at every ExecuteCommandLists boundary. Textures have explicit terminal COMMON transitions, and the swapchain boundary is PRESENT/COMMON before the existing UI/timer suffix. Frame/source/descriptor lifetimes remain protected by actual GPU fences. CPU-ready and GPU-complete remain separate. U3's partial-error policy is unchanged: Begin is rejected after partial execution until Drain and recreation of execution/resources. The inherited GPU tests retain late error/cancellation, fence retention and true delayed slot reuse coverage.

Each grass view has private immutable parameters. The current expansion arena is shared, so RAW/WAR/WAW edges order expansion, consumption and subsequent reuse on GPU. Independent native recording still overlaps on CPU. Scene collection, source copying into frozen staging, allocation/PSO creation, pose/view preparation and deterministic submission stay on the owner. ECS system order was not changed.

## Fixture and comparison contract

Camera: `(1399.6,100,969)` cm, rotation zero, FOV 60, near 1, far 10000. Three static quad entities plus two skeletal entities share registered mesh/scene assets and a colored texture. Skeletal animation seeks advance by 0.1 s per compared frame; the second instance has a 0.25 s offset. Seeking follows the existing history-reset contract; natural continuous-animation timing performance is not claimed. A moving static object separately exercises previous transforms/motion attachments.

A 256-root grass patch, seed 13, uses projected wind (16x16, four pressure iterations, one substep) and then analytic wind. Ribbon changes to CrossedRibbons. A swept interaction source moves 20 cm per frame. Host dt is fixed at zero; compared wind/interaction dt is 1/60. Pause is exercised on frame 2; reset/settings change and returning patches reset interaction history on frames 4/7. Patch transforms change and patches are absent on frames 3/6. Quality resolution uses existing algorithms/CVars, with sun 512 and local 128. Startup explicitly disables async/offload/frame generation; one engine device initializes.

Each of eight frames compares serial and parallel recording with identical frozen scene/camera/input data. Grass uses independent reference/parallel GPU state, so the second run does not advance the first run's simulation. Comparison is bit-exact for valid source snapshots (including main/camera), skinned vertices, canonicalized culling/packed commands and visible instance sets, all pixels of five GBuffer attachments plus depth, eleven shadow depths/counts, grass fields and six probe faces' geometry attachments. Row padding and unwritten command tails are excluded; atomic append order and the representative instance ID of a batch are normalized. Expected skinned vertex coordinates additionally use a 0.001 absolute tolerance.

Observed visible object counts: `5,5,4,0,5,4,0,5`; packed draws: `4,4,3,0,4,3,0,4`, maximum instance count 2. Frame 0 has 305992 main depth pixels, 22245 sun-shadow pixels across cascades, 18869 local-shadow pixels, and 872 pixels in the forward diagnostic probe face. Empty local views/probe faces retain no old draws. GPU interaction versions are 1..8; previous field bytes match the preceding completed publication except documented reset frames, when previous equals current.

After eight A/B pairs, four more geometry frames submit without intermediate Drain. The final drain validates readbacks from all three reused slots. Both configurations present 12 frames, perform an actual odd resize to 784x561, close and destroy normally, report zero legacy Execute calls and zero D3D12 ERROR/CORRUPTION messages. Production INI hashes are unchanged by Meta smoke.

## Commands and results

All final combinations below passed (exit 0). `Invoke-Build.ps1` bounds each build to 600 seconds; GPU/legacy runners bound each process to 60 seconds. Heavy builds and GPU runs were sequential.

```powershell
foreach ($target in @('PlanTests','RecordingJobsTests','App.MetaRenderGraph','App','BaseRenderPipelineTests','RenderPipelineTests')) {
    & Apps/App.MetaRenderGraph/tests/Invoke-Build.ps1 -Configuration Debug -Target $target
}
foreach ($target in @('App.MetaRenderGraph','App')) {
    & Apps/App.MetaRenderGraph/tests/Invoke-Build.ps1 -Configuration Release -Target $target
}
& Build/bin/x64/Debug/PlanTests/PlanTests.exe
& Build/bin/x64/Debug/RecordingJobsTests/RecordingJobsTests.exe
& Apps/App.MetaRenderGraph/tests/Invoke-Smoke.ps1 -Configuration Debug -Target App.MetaRenderGraph
& Apps/App.MetaRenderGraph/tests/Invoke-Smoke.ps1 -Configuration Release -Target App.MetaRenderGraph
& Apps/App.MetaRenderGraph/tests/Invoke-Legacy.ps1 -Target BaseRenderPipelineTests
& Apps/App.MetaRenderGraph/tests/Invoke-Legacy.ps1 -Target BaseRenderPipelineTests -Custom
& Build/bin/x64/Debug/RenderPipelineTests/RenderPipelineTests.exe
python Apps/App.MetaRenderGraph/tests/Export-U4Evidence.py --configuration Debug
python Apps/App.MetaRenderGraph/tests/Export-U4Evidence.py --configuration Release
```

Planner/recording CPU suites retain U2/U3 dependencies, decay, nested jobs, deterministic assembly, cancellation and shutdown coverage. Composition tests passed primitive/batch/pipeline/AO/rebuild/grass-lifetime checks. Legacy default/custom GPU tests retain the old execution path. Production App builds passed Debug and Release; full Bistro runtime/image comparison was not run.

Earlier failures were corrected and rerun: initial fixture templates were not populated after entity creation; an absent diffuse texture made the first albedo black; 8-bit linear shadow depth hid near-one values (exports now include occupancy silhouettes and raw floats); low generic descriptor allocations collided with the texture range; the interaction-history test initially ignored reset semantics. The legacy runner initially copied an App-owned grass CVar into its App.Base-only test config; the runner now filters those test-only entries. Sandbox MSBuild produced C1902/PDB-manager failures; native retries used the same project/compiler settings. Existing LNK4075/LNK4099 warnings remain. Final diff --check passes.

## Hardware, trace and memory

RTX 5050, current `nvidia-smi` driver 621.14 and 8151 MiB total (DXGI logs 7910 MiB); Windows driver naming 32.0.16.2114 agrees with the accepted base. VS18 MSBuild 18.9.1, v145/14.51.36231, SDK 10.0.26100.0, x64. Debug and Release smoke both enable the D3D12 debug layer. No driver/toolset/global settings changed.

All eight traces in both configurations show native technique callbacks on workers 1/2/3 and peak simultaneous recording on three workers. Frame 0 has 170 native payload callbacks; total callback spans are 13692 us Debug and 17584 us Release. These totals sum overlapping callbacks and include setup/Close; they are not frame times or a benchmark. Four real upload buckets are scheduled, but their tiny payload can execute on one worker; concurrency evidence is in meaningful mesh/shadow/grass callbacks, not artificial repeated copies. Frozen owner preparation is outside these spans. These are CPU traces, not PIX captures or GPU-overlap evidence.

D3D12MA resource allocations measured identically in final Debug/Release runs: before resize 333533184 bytes (318.08 MiB), 698 allocations, 386531328 block bytes; after queued frames 204427264 bytes (194.96 MiB), 717 allocations, 344064000 block bytes. These include diagnostic outputs/readbacks and two grass states. The fixed pool reserves 512 native lists/allocators per each of three slots. Driver command allocator/descriptor-heap memory is not included in those D3D12MA numbers; no universal memory/performance bound is claimed.

## Artifacts and limits

`Build/Verification/meta-render-graph-u4/Debug-App.MetaRenderGraph/` and the corresponding Release directory contain runtime/config hashes, `geometry-metrics.json`, `geometry-trace-summary.json`, and `geometry/` with 1031 raw files per configuration: scene/skin/field/image readbacks, per-view metrics, PPM/PGM/PNG screenshots, operation maps and schedules, native CPU traces, and queued-slot results. The inherited `direct-schedule.txt` / `recording-cpu-trace.json` belong to the U3 executor fixture; geometry traces are named `geometry-N-cpu.json`. Export uses Python standard libraries only. Artifacts are local/ignored and must be rerun on another host.

Current diagnostic shadow targets are individual per view, not the production packed atlas. Selected cached views are redrawn each frame; cache/update optimization is deferred, while geometry/filter/matrix results are retained. Main-view skeletal draws are batched; shadow draws stay per-instance on GPU. Probe faces test grass geometry/GBuffer and CaptureReflections filtering; full reflection capture lighting/readiness belongs to U5 and is not claimed. Readbacks and duplicate grass state remain diagnostic costs in this experimental implementation. Structure/capacities are fixed for this fixture; general hot streaming and automatic partial-frame recovery are absent. No lighting/post U5–U7, GPU async or mGPU was introduced.

Stop here for originating-chat review. U5 requires a separate decision.
