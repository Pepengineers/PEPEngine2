# U2 — single DIRECT execution plan

Date: 2026-11-13 (Europe/Moscow). Host: `MidSpec` (requested as MisSpec).
Checkout: `C:/Users/MidSpec/Documents/PEPEngine2`.

## Source and result

Initial HEAD and accepted implementation base: `40f986588d36f47fdda49b982bd4f7d00571fceb` (U1). Work branch: `codex/meta-render-graph-u2`, created from that exact local commit. U1 remains on `codex/meta-render-graph-u1`; no remote branch was substituted for the local base. The only initial untracked file, `Apps/App/App.vcxproj.user`, is preserved and excluded. There were no pre-existing staged changes.

Result HEAD is the local U2 commit containing this report: resolve with `git log -1 --format=%H -- docs/reports/meta-render-graph/02-direct-plan.md`. The exact hash, file manifest, SHA256 values and base-to-U2 binary patch are saved after commit in ignored `Build/Verification/meta-render-graph-u2/handoff.json` and `base-to-u2.patch`. The report avoids embedding its own commit hash, which would change that hash.

Only U2 is implemented. No U3 jobs, technique migration, extra device, async compute, offload worker, new aliasing allocator, push, PR or merge. Independent review is reserved for the originating chat; no review subagent was launched.

## APIs and execution paths

- `Apps/App.MetaRenderGraph/src/Graph/MetaGraph.h`: CPU-only `Meta::Graph`, resources with imported initial/final states, operations, whole-resource accesses, explicit producer IDs, side effects, packet list counts, barriers and `Schedule`. `MetaGraphCompiler.cpp` implements `Compile` and textual `Schedule::Write`.
- `Compile` builds RAW/WAR/WAW edges in declaration order, joins **all** readers before a writer or a read-state change, checks unknown dependencies/resources, duplicates and cycles, and chooses the lowest available operation ID for stable topological ordering. Side effects have declaration-order edges and no operation is culled. Empty operations still have a command list and completion. A terminal packet joins every packet and emits final-state transitions.
- UAV accesses at the unchanged `UNORDERED_ACCESS` state receive a UAV barrier whenever either the preceding access or the new access writes. Read/read accesses at the same state need no UAV barrier. A transition and a dependency are separate concepts. CPU tests cover the read/read/write UAV case as well as dependent writers.
- `MetaGraphExecutor.h/.cpp`: `Meta::Executor(device, slotCount, listCapacity)`, `Begin(slot)`, `Execute(schedule, bindings, recorder, prefix, suffix)`, `Tokens`, `Retirement` and `Drain`. All command allocators/lists are allocated in its constructor. The application reserves 16 native lists per each of 3 frame slots; its schedule consumes 11 lists (9 operations, including a two-list upload, plus a terminal packet).
- `DirectFixture.h/.cpp`: reusable real upload/compute/diamond/draw/readback recorder, shared by the application and GPU tests. It creates fixed per-slot buffers and two pipeline states/root signature before recording.
- `MetaRenderPipeline.h/.cpp`: owns the fixture/executor, compiles the plan at rebuild, fetches the current backbuffer every frame and calls this executor. Its legacy `BuildGraph` throws. It does not construct or execute a legacy graph.
- `App.Base/Rendering/IRenderPipeline.h`: opt-in `OwnsExecution`, `RebuildExecution`, `ExecuteFrame` and `WriteExecution`. Existing pipelines default to the legacy path.
- `App.Base/src/RenderModule.cpp`: branches both graph construction and execution. Pre-graph commands/timer start are passed as the prefix. UI/timer stop are recorded in a suffix. The actual suffix fence becomes frame-constant-slot completion and timer notification; SDK Begin/EndRendering and Present retain their existing brackets. Experimental drain does not create/signal an unused COMPUTE queue.
- `GDX12RenderGraph::GetExecuteCallCount()` is a process-wide diagnostic counter incremented on entry to the old Execute. Experimental smoke asserts and logs zero; legacy default/custom tests assert at least three calls. This is runtime evidence of the selected path, beyond the seam's source structure.
- `tests/PlanTests.cpp`, `DirectExecutionGpuTests.cpp`, their `.vcxproj` targets, and `Invoke-Build.ps1`, `Invoke-Smoke.ps1`, `Invoke-Legacy.ps1` supply bounded reproducible checks. `BaseRenderPipelineTests.cpp` also fixes a stale pre-existing three-argument Setup call referencing removed `context.Secondary`, so the affected regression target can compile against the accepted base API.

## Owner-thread, states, submission and retirement contract for U3

Device/queue access, `Begin`, compilation/binding, submission and drain run on the renderer owner thread. Executor checks that thread ID. U2 invokes every recording callback sequentially on that thread. The gate-release test thread only signals its independent native fence; it does not call an engine queue/device API. CPU-ready lists and GPU-complete tokens are distinct.

Plan state lives in compiler-local vectors and immutable packet barriers, never in `GDX12Resource::_currentState`. Native callbacks receive `ID3D12GraphicsCommandList*`; they issue Copy/Dispatch/Draw/Clear/root-binding calls and do not call wrapper transition helpers. Required before/after states are fixed before recording. Internal state changes must be separate declared operations. One packet's prologue is in its first list, and its epilogue in its last list; child lists do not independently transition the shared destination. Resource IDs must refer to distinct physical resources with truthful initial states; callers retain resources/descriptors through slot completion.

U2 deliberately coalesces the ready plan packets into **one native ExecuteCommandLists batch**, preserving list order. Every packet token resolves to that batch's successful queue-owned Signal fence. These are conservative completion points, not independent early-completion claims. The prefix and suffix are separate queue-wrapper submissions orchestrated by the executor; the suffix fence retires the whole frame slot. There is no interqueue Wait in production U2 because the only queue is DIRECT. Same-queue ordering plus compiled transitions/UAV barriers lower the dependencies.

This batching boundary matters: D3D12 buffers decay at Execute boundaries. The fixture starts and ends its data buffer in COMMON and keeps planned states across lists within the batch. U3 must not split this batch into separate Execute calls while assuming those intermediate buffer states survive. Earlier packet submission would require explicit decay-aware lowering; parallel CPU recording alone does not require splitting it.

`Begin(slot)` waits for that slot's last successful retirement fence before upload mutation or allocator reset. Each native allocator is used by only one list. RTV descriptors are borrowed from the backbuffer heap; fixture root views use no transient shader descriptors. Fixed fixture buffers stay alive with the pipeline. Each application slot requests 336 logical buffer bytes (upload/data/three readbacks: 5x16, plus 256-byte pixel readback), 1008 bytes for three slots before D3D12 allocation alignment/heap overhead. The 48 native allocators/lists, PSOs/root signature and existing queue-pooled prefix/suffix lists add driver/heap memory; no measured peak-memory or performance claim is made. Test resources are destroyed after their executor drains.

The executor records and closes the entire native batch **before submitting the prefix or any plan work**. Callback/Close/validation failure leaves packet tokens zero, preserves the previous slot retirement, and sends no partial batch or dependent draw. Already-recorded native lists are CPU-only and can be reset after the prior slot fence; the recovery test proves that reuse. Queue wrappers for an abandoned prefix/suffix are retained by the existing queue until destruction; they are never returned to its available pool without submission retirement. Frame failure propagates to the application; arbitrary legacy pre-graph state mutations are not automatically rolled back, so this is not a general frame-retry API for migrated techniques.

If execution occurs but Signal fails, no successful token for that batch is published, and the slot is poisoned so `Begin` cannot reset it. Native list/allocator references are retained if destructor drain cannot establish safe retirement. A successfully signaled graph token remains valid if a later suffix fails, but `Execute` then throws and publishes no successful frame completion. Device-removal/Signal failures were not fault-injected; recording failure was exercised on hardware.

RenderModule's existing shared DIRECT flush covers native executor submissions for drain, reset/world removal and resize. `OnResize` drains before replacing swapchain buffers, then rebuilds the experimental schedule even though there is no `_primaryGraph`. The plan stores no old backbuffer pointer: bindings/RTV/dimensions are fetched again for each frame, also accommodating BeginFrame presentation-resource replacement. Pipeline ownership remains inside RenderModule's GPU-domain quarantine. UI is the only called tracker-mutating barrier helper in the suffix: the plan returns the target to PRESENT, its tracker is explicitly checked at that boundary, and UI records PRESENT->RT->PRESENT sequentially. Timer helpers have no hidden graph-resource transitions. Native plan recording never changes this tracker.

## Fixture and hardware

The actual experimental image is a full-screen procedural triangle. Four uints (`seed+i`) are uploaded by two lists copying disjoint 8-byte halves. Two dependent DIRECT UAV dispatches produce `4*x+3`; two readers independently copy all four values. A third dispatch waits on both readers and produces `8*x+7`. The vertex shader reads this final buffer; the draw's red channel is its first value divided by 255, with exact green/blue endpoints 0/1. A buffer readback and center-pixel texture copy are checked. A Clear initializes every render target before draw, including newly allocated non-zeroed D3D12MA memory.

GPU test: 32x32 target, two slots, eight frames with seeds 1..8, then an injected failure in operation 2, then successful reuse with seed 11 and a DIRECT queue gated for 150 ms. Both pre-transform readers, final buffers and RGBA pixel bytes are compared. A separate immutable `0x12345678` value is copied by the executor-owned prefix/suffix and read back, proving those commands survive the seam. The failure test verifies unchanged readbacks, no draw callback, no queue-fence advance, unchanged retirement and all-zero unsubmitted tokens. Delayed-slot completion is observed incomplete before reuse; `Begin` actually blocks about 151 ms before data validation.

Application smoke: same fixture/executor, three slots, eight presented frames (four before/four after SetWindowPos), seed sequence 1..8, timer dt=0, initial client 1280x800, measured resized client 784x561. The camera/world and authored eligible particle emitter from U1 remain loaded, but no particle technique executes in U2; particle migration is U6. This is not a Bistro image comparison.

Hardware: primary NVIDIA GeForce RTX 5050, 7910 MiB logged dedicated memory, driver `32.0.16.2114`. Other installed adapters (including Intel Arc B570) were not used as rendering devices. Runtime logs contain exactly one initialized engine device. Toolchain: VS 18 Professional MSBuild 18.9.1, v145 14.51.36231, Windows SDK 10.0.26100.0, x64. No compiler/toolset, driver or global Git settings changed. Startup remains isolated in each executable output directory: primary=0, particle secondary=-1, AO/particle async=false, native output, frame generation Off, no linked Bistro water runtime.

## Commands and results

Run from checkout root in PowerShell. `Invoke-Build.ps1` uses the installed MSBuild with `/p:Configuration=<configuration> /p:Platform=x64 /m:2 /nr:false /v:quiet /fl`, per-target compact stdout/stderr and detailed file logs, and a 600-second timeout (kills its own build tree on expiry). Runtime scripts impose 60 seconds per process. Heavy builds and GPU runs were sequential.

```powershell
& Apps/App.MetaRenderGraph/tests/Invoke-Build.ps1 -Configuration Debug -Target PlanTests
& Build/bin/x64/Debug/PlanTests/PlanTests.exe
& Apps/App.MetaRenderGraph/tests/Invoke-Build.ps1 -Configuration Debug -Target DirectExecutionGpuTests
& Apps/App.MetaRenderGraph/tests/Invoke-Smoke.ps1 -Configuration Debug -Target DirectExecutionGpuTests
& Apps/App.MetaRenderGraph/tests/Invoke-Build.ps1 -Configuration Debug -Target App.MetaRenderGraph
& Apps/App.MetaRenderGraph/tests/Invoke-Smoke.ps1 -Configuration Debug -Target App.MetaRenderGraph
& Apps/App.MetaRenderGraph/tests/Invoke-Build.ps1 -Configuration Release -Target App.MetaRenderGraph
& Apps/App.MetaRenderGraph/tests/Invoke-Smoke.ps1 -Configuration Release -Target App.MetaRenderGraph
& Apps/App.MetaRenderGraph/tests/Invoke-Build.ps1 -Configuration Debug -Target BaseRenderPipelineTests
& Apps/App.MetaRenderGraph/tests/Invoke-Legacy.ps1 -Target BaseRenderPipelineTests
& Apps/App.MetaRenderGraph/tests/Invoke-Legacy.ps1 -Target BaseRenderPipelineTests -Custom
& Apps/App.MetaRenderGraph/tests/Invoke-Build.ps1 -Configuration Debug -Target RenderGraphTests
& Apps/App.MetaRenderGraph/tests/Invoke-Legacy.ps1 -Target RenderGraphTests
& Apps/App.MetaRenderGraph/tests/Invoke-Build.ps1 -Configuration Debug -Target App
& Apps/App.MetaRenderGraph/tests/Invoke-Build.ps1 -Configuration Release -Target App
```

All commands above passed (exit 0). CPU tests cover RAW/WAR/WAW, two-reader diamond, same-state UAV including WAR, all-reader state transitions, stable scheduling, forward explicit producer, cycle/unknown producer/resource, invalid list count, empty operation/graph, independent multi-list packet and multi-input packet. GPU tests pass real values/pixels, repeated slots, failure recovery and delayed retirement. Both application configurations and the dedicated Debug GPU executable pass hardware smoke with D3D12 debug layer, actual resize, normal window close/destruction, zero legacy Execute calls and zero ERROR/CORRUPTION messages. The GPU test target is not separately run in Release; its same test code runs in Release application smoke.

Affected legacy regressions pass: BaseRenderPipelineTests default and custom (three real frames, resize, debug validation and legacy Execute counter), and RenderGraphTests (compiler/pass tests, physical RAW/WAR, DIRECT fallback, queue allocator retirement, async joins, final fences and 400 changing frames with concurrent texture readers). Normal App builds in Debug and Release. Full Bistro runtime, image A/B and performance comparisons were not run; no Bistro technique was migrated.

An initial sandbox build failed C1902 (PDB manager, as in U1); the native build succeeded without changing compiler flags. Initial hardware fixtures exposed fractional UNORM interpolation rounding (blue 127 vs expected 128 for 0.5) and the non-zeroed RT initialization rule. These were fixed using flat color/exact endpoints and explicit Clear, then all final runs were repeated successfully. Existing LNK4075, third-party LNK4099 missing PDB and Bistro conversion warnings remain.

Evidence is local under ignored `Build/Verification/meta-render-graph-u2/`: `*-build.log`, `*-stdout.log`, `*-stderr.log`, `PlanTests-Debug-runtime.log`, legacy stdout/stderr, and per-smoke `runtime.log`, `direct-schedule.txt`, `reference.dot`, `effective.ini`, `protected-config-hashes.json`. The compatibility filename `reference.dot` contains a plain CPU schedule for U2, not DOT/PIX. These are executable hardware checks and readbacks, not PIX captures. Production INI SHA256 stayed `8196CA973D3BD39C030B6596B39F5BC62176D5D4F9D877E968E67447EE9B929F` (repository) and `2DDAD8833D4143F4C263BD5D03DC3B759088D82A453087C6BD38F5F8B54C94BF` (App output). The smoke runner checks exact before/after hashes and hostile generated INI restoration.

## Handoff limits

The one-DIRECT, whole-resource, fixed-capacity implementation is a sequential diagnostic baseline. There is no scheduler/worker integration, streaming, aliasing, resource-range analysis, independent packet early submission, async/MGPU execution, general legacy-helper state rollback, Signal-failure injection, full-scene visual equivalence or throughput claim. Unused operations are retained. Recording callbacks must honor the declared boundary and cannot hide submissions or state transitions. U3 must preserve slot retirement, deterministic indexed assembly and the buffer-decay/batch contract while introducing recording jobs. U3 was not started.
