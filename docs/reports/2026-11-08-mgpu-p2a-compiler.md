# MGPU P2a — graph compiler and symbolic queue plan

Status: **validated / ready for integration**, 2026-11-08. Debug/x64 build, compiler-only CPU tests and both legacy GPU correctness runs passed under the granted P2a lease. No benchmarks, application workload or captures were run.

Worktree: `D:\Project\PEPEngine2\.tmp\mgpu-feature-2026-11-08\p2a`; branch `codex/mgpu-p2a`; base `79c6dc3fde0688715c3669375b09b71c7bd85b16`.

## Scope and preserved behavior

Only graph header/implementation, the pass queue-support hook, this report and `RenderGraphTests.cpp` change. P0 `GDX12Submission.h`, queue/device/runtime files and project files are unchanged. No MGPU switches or new defaults. No dependency on P1's uncommitted API.

Existing `Compile(bool)` and DIRECT/optional COMPUTE `Execute` remain the legacy path. Explicit placement plans are **compile-only in P2a**: legacy Execute rejects them before inspecting command lists or submitting anything, including explicit primary DIRECT/COMPUTE placement. This conservative restriction avoids claiming an AdapterId maps to the legacy device before P1/P2b supply native identity/capability routing. COPY, cross-device and unresolved exact inputs therefore cannot silently execute on primary. `CompilePlan()` also forbids legacy Execute until `Compile()` prepares native resources.

## Declaration API for P2b

All new shared value types below come from the unchanged `GDX12Submission` namespace.

```cpp
// Graph resource handles still identify logical graph views, not native pointers.
void RegisterResource(Resource view, ResourceIdentity identity, bool crossAdapter = false);
void ImportVersion(Resource importedView, TransferVersion exactVersion);
void CompilePlan();                    // no GPU work/flush/allocation/rebinding
const CompiledPlan& GetCompiledPlan() const;

// Pass RecordRenderGraph builder additions:
void SetPlacement(Placement location);  // explicit adapter + Direct/Compute/Copy
void DeclareCompleteAccesses();         // physical access contract, not a hidden-buffer exemption
void UseVersion(Resource view, Mode usage, TransferVersion version, D3D12_RESOURCE_STATES state);

// Pass opt-in; default accepts Direct only. Existing AsyncCompute() remains compatible.
virtual bool SupportsGraphQueue(QueueKind queue) const;
```

Use `Create`/`Import` to obtain each physical buffer/texture view's graph handle, then `RegisterResource` once. Distinct views require distinct nonzero ResourceId; the same backing allocation requires the same AllocationId even across adapters, transfer streams and epochs. Cross-adapter registrations and all transfer-bearing registrations require a nonzero AllocationId. A logical `SharedResource::Scene/Environment/...` token cannot become a registered physical view.

Explicit placement requires complete declarations, supported queue kind, registered device-local views on the chosen adapter, explicit resource states and no legacy `GetBufferAccesses()` entries. COPY accepts only COPY_SOURCE reads / COPY_DEST writes, not ReadWrite; COMPUTE rejects graphics-only states. Physical access modes must agree with read/write state flags. Resource capability/layout/footprint/format validation and real command-list recording remain native executor/transport responsibilities.

Every transfer-bearing access declares an exact `(Transfer, Epoch, Invocation)` matching its registered view. Reads require a matching prior resource/allocation writer or explicitly imported version; imported means symbolic external readiness, **not completed/available memory**. Runtime must hold a P3a reservation and supply local import-ready when binding external inputs before dependent submission. The compiler does not fabricate a reservation, CompletionPoint or fence value.

Several writes of one allocation in the same pass must declare identical versions; differing invocation or stream versions are rejected rather than using the last access. Version checks include fallback views. Backing allocation hazards are independent of transfer/version identities.

## Exact plan layout

Types live inside `GDX12RenderGraph`:

- `enum class PlannedBatchId : UINT { Invalid = UINT_MAX }`: vector index valid only inside this compiled plan. Invalidated by mutation/recompile; never usable as a runtime/fence token.
- `PlannedBatch { Id, Location, LegacyPrimary, vector<Pass> Passes, vector<Access> Accesses }`: one pass per batch preserves all possible wait boundaries. `Access` now appends optional `TransferVersion Version` after existing Id/Usage/State, preserving legacy aggregate initializers. Legacy Location.Adapter stays Invalid deliberately; its queue is DIRECT or COMPUTE-eligible, resolved by existing runtime policy.
- `PlannedView { View, ResourceIdentity Identity, Imported, CrossAdapter, LogicalToken, vector<Resource> Fallbacks }`: indexed by graph Resource. Legacy logical/unregistered entries retain invalid identity.
- `BatchDependency { Producer, Consumer }`: symbolic local edges for resource RAW/WAR/WAW, explicit dependencies and queue FIFO, including same-queue edges. P2b translates cross-queue edges to waits only after producer submission has actually signaled.
- `VersionInput { View, TransferVersion Version, Consumer }`: external exact read inputs, including expanded conditional fallback views. Deduplicated by `(View, Version, Consumer)`; no implicit edge from matching numeric invocation values or TransferId alone.
- `CompiledPlan { Batches, Views, Dependencies, Inputs, RequiresNewExecutor }`: Batches are in deterministic expanded topological order. Dependencies sorted by producer/consumer batch ID. Inputs stable in batch/access/fallback order. Getter rejects uncompiled/failed/stale plans.

Resource ownership, exports/lifetimes, textures/links and pass instances remain in the existing graph's ResourceInfo/Node for the successor P2b owner. They are not copied into a second renderer. Source/description access is available to P2b inside the same graph implementation.

## Scheduling and lifetime boundaries

Existing registration-order RAW/WAR/WAW and fallback expansion remain. Physical allocation hazards add the same protections across registered aliases, independent of device, transfer stream and epoch. Culling/export retention includes the actual alias writer. Distinct allocations/streams do not imply physical aliases.

For explicitly placed queues, registration order defines enqueue FIFO among live passes with the same full Placement. Legacy queue FIFO follows the existing topological order. A second topological check validates the expanded pass/wait/FIFO graph before producing batches. Backward same-queue waits and FIFO-induced cross-queue cycles reject. Same QueueKind on different AdapterIds does not merge queues. Slot-release credit dependencies may be represented as explicit graph edges; dynamic reservation/credit graphs spanning separate invocations must be validated by P2b/P3 binding before submit. No claim is made that a local compiled plan validates unavailable external credit graphs.

Explicit plans never pool or rebind resources on the legacy device; the compiler-only path never flushes or allocates. Legacy COMPUTE lifetime widening and existing pooling behavior remain. Queue-aware resource retirement, COMMON release/acquire transitions, UAV barriers, native waits and completion binding belong to P2b. Independent producer templates carry no compiler-inserted Present/frame join; retaining producer-owned resources across frames remains a runtime obligation.

`WriteDot` now shows explicit adapter/queue labels and the expanded plan edges, without pretending symbolic IDs are fences.

## Authored checks (not yet run)

`RenderGraphTests.exe --compiler-only` returns before TestApp construction, D3D12 debug interface/device creation and the GPU fixtures. Existing CPU scheduling/link/rollback tests still run; the executable still links the existing renderer and runtime DLLs. No separate native MGPU implementation or new project file is required.

New cases cover:

- HAO-shaped D0 -> Copy0 -> Copy1 -> Compute1 -> Copy1 -> Copy0 -> D0 with distinct device views/shared backing, exact input/output versions, deterministic recompilation and diagnostics.
- Independent producer templates at invocation 7 and 19, exact symbolic external imports and no implicit frame/Present dependency.
- Equal numeric invocation values in different streams/queues do not fabricate dependencies; explicit cross-queue dependencies do.
- Allocation RAW/WAR/WAW across devices, streams and epochs; alias cycles, backward wait and FIFO-induced cycles.
- Missing allocation registration, invalid/duplicate identity, wrong-device view, unsupported/invalid placement, incomplete access declarations, unmanaged resource/buffer, illegal COPY/COMPUTE state/mode, missing/stale/wrong-stream/wrong-epoch versions and stale plan access.
- Conditional written output with an externally imported exact fallback: a conditional-only consumer must retain external readiness; a separate consumer with a direct external read must deduplicate it. Conflicting writes to one allocation in one pass reject.
- Culling, forwarding, exports and legacy DIRECT/COMPUTE plan compatibility. Existing GPU correctness suite remains scheduled separately after lease.

## Validation receipt

Every workload ran sequentially from this worktree after checking `owner=P2a`, `state=granted` and the exact worktree path. Local dependency copies and `/Z7` override follow the P0 setup; no tracked build flags/project files changed. Build source fingerprints matched before/after; executable/fixture INI fingerprints also matched before/after each test run. No code changed after the successful build.

| Check | Result | State and logs relative to worktree |
| --- | --- | --- |
| VS v145 Debug/x64 build, `/m:1 /nr:false`, local `/Z7` target | PASS, child exit 0; no compiler/linker warnings or errors found | `Build/P2a/build-debug.json`, `.stdout.log`, `.stderr.log` |
| `RenderGraphTests.exe --compiler-only` | PASS, child exit 0; returned before GPU fixture initialization | `Build/P2a/compiler-only.json`, `.stdout.log`, `.stderr.log` |
| Legacy GPU correctness, D3D12 debug layer | PASS, child exit 0; clean debug layer | `Build/P2a/legacy-debug.json`, `.stdout.log`, `.stderr.log` |
| Legacy GPU correctness, debug layer + GPU-based validation | PASS, child exit 0; clean debug layer | `Build/P2a/legacy-gbv.json`, `.stdout.log`, `.stderr.log` |
| Scoped `git diff --check` | PASS | Task files only; no LFS filtering or index mutation |
| Owned workload process audit | PASS, `OwnWorkloadCount=0`; no process terminated | `Build/P2a/process-audit.json`, `Build/P2a/processes-final.json` |

Both GPU runs reported pooled/dedicated identity, queued frames, shared states/descriptors, resize/export/fallback lifetimes, multiple async producer RAW/WAR/WAW and retirement, async UAV texture prefix/read-owner/alias/failure checks, final joins, and **400 changing frames with concurrent texture readers and readbacks**. Both also ran the new CPU compiler cases. Test stderr was empty. The fixture explicitly created COMPUTE and exercised existing DIRECT fallback; no native MGPU executor was used or claimed.

Successful executable SHA256: `77b7f3efcba99afec2cd12d7d4d2bc1f431d8034ff75a5cef727d27619175a49`.

Only the output fixture INI contains `[ConsoleVariables]` to avoid P0's known baseline unknown `App.Water.HeightScale`; original output INI is saved in `Build/P2a/Engine.original.ini`. Tracked configuration, defaults, quality and simulation parameters are unchanged.

Final audit recorded owned root PIDs 37216 (MSBuild), 5484 (CPU), 28504 (debug), 36472 (GBV), all exited. No matching MSBuild/cl/link/RenderGraphTests process was visible in the final scoped inspection. The lead's unrelated historical PID 30988 was explicitly excluded from ownership and was never stopped or changed. CIM process inspection was unavailable in the sandbox; fallback used `Get-Process`, recorded runner PIDs, successful child exit states and absence of visible workload children. No further workloads are needed; the lead can transfer the validation slot while the local commit is finalized.

Exact sequential setup/build/CPU/debug/GBV commands, timeouts and absolute log paths remain in `D:\Project\PEPEngine2\.tmp\mgpu-feature-2026-11-08\P2a-ready.json`. Validation used local-only dependency copies; no P1 uncommitted source, project/runtime/P0 edits, push, benchmarks or preset tuning.

Unrelated LFS/runtime/image worktree differences are excluded. Git commands use scoped paths and command-local `-c filter.lfs.process= -c filter.lfs.required=false` to avoid sandbox git-lfs subprocess failures; tracked LFS configuration is unchanged.

## Handoff limits

P2a provides validated compiler/plan declarations and legacy regression coverage. P2b must implement native device/queue routing, bind planned producers to actual post-Signal CompletionPoints, retain external Exact reservations/import readiness, validate cross-invocation credit graphs, emit transitions/waits and own queue-aware retirement. All explicitly placed plans remain rejected by legacy Execute until then, including primary-only explicit placement. Single-device legacy regression passes do not establish native COPY/two-adapter correctness or performance. No full Bistro/IBL suite, soak, captures or benchmarks were run.
