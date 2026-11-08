# P1: DIRECT / COMPUTE / COPY queues and producer timelines

2026-11-08. Worktree: `D:\Project\PEPEngine2\.tmp\mgpu-feature-2026-11-08\p1`, branch `codex/mgpu-p1`, base `79c6dc3fde0688715c3669375b09b71c7bd85b16`.

**Status: validated.** The lead granted owner=P1, state=granted for this worktree. All build/model/queue/graph correctness checks below completed sequentially with a lease check before every workload. The lease was not modified. Local commit follows validation; no push.

## Scope and integration API

Changed only `GDX12Device.h/.cpp`, `GDX12CommandQueue.h/.cpp`, necessary `GDX12CommandList.h/.cpp`, new `Apps/App/tests/MGPUQueueTests.cpp`, new `Apps/App/MGPUQueueTests.vcxproj` and this report. `GDX12Submission.h`, existing projects/filters/solution, graph/runtime/pipelines and CVars are untouched. No technique switches or placeholder controls added.

Implemented the P0 signatures unchanged:

```cpp
AdapterId GDX12Device::GetSubmissionAdapterId() const;
QueueId GDX12CommandQueue::GetIdentity() const;
TimelineId GDX12CommandQueue::GetTimelineIdentity() const;
CompletionPoint GDX12CommandQueue::GetLastCompletionPoint() const;
CompletionPoint GDX12CommandQueue::SignalCompletion();
FenceProgress GDX12CommandQueue::GetCompletionProgress() const;
void GDX12CommandQueue::WaitFor(const CompletionPoint&);
```

Additional P1 API:

```cpp
GDX12CommandQueue* GDX12Device::TryGetCopyQueue();
GDX12CommandQueue(GDX12Device*, D3D12_COMMAND_LIST_TYPE = D3D12_COMMAND_LIST_TYPE_DIRECT,
                  bool crossAdapterFence = false);
TimelineId GDX12CommandQueue::ImportTimeline(GDX12CommandQueue& producer);
FenceProgress GDX12CommandQueue::GetCompletionProgress(TimelineId producer) const;
```

Names above use `GDX12Submission` types. Local queues are lazily accessed through the existing DIRECT/COMPUTE methods and new COPY accessor. Queue creation failure logs the actual HRESULT exception and leaves DIRECT available; device removal propagates. Explicitly constructing a queue propagates creation errors to its caller.

For P3b, an exporting transport owns an explicit queue constructed with `crossAdapterFence=true`, then the consumer calls `ImportTimeline(producer)` before waiting on its points. Local DIRECT/COMPUTE/COPY defaults use `D3D12_FENCE_FLAG_NONE`; explicit exporters use SHARED | SHARED_CROSS_ADAPTER. Sharing is decided at construction, never silently replaces an in-use fence/timeline. All supported queue types can export. Import requires the actual producer queue, rather than accepting an unverified `(HANDLE, TimelineId)` pair. This deliberately avoids arbitrary handle/timeline binding. Same-device timelines resolve through the owning device registry without handles. Remote imports keep the original identity and a native local view; temporary Win32 handles close on success and exception. Repeated imports reuse their view, expired imports are pruned.

**Threading discipline:** device initialization, queue submissions/observations, import and reset require external serialization by their device owner, including source access during cross-device import. Identity issuance uses checked atomic counters; queue counters, active state, lists and registry are not a concurrent API. P1 adds no worker/polling thread. The fixture watchdog is only a bounded correctness gate.

## Correctness and lifetime

- Successful native queue Signal publishes a point on that exact queue/fence timeline. Execute keeps its existing void API and emits one signal per batch. Failed Signal publishes no new point.
- Fence values never issue zero or UINT64_MAX; overflow is rejected before executing a command batch. Identity counters check overflow and never wrap/reuse across recreation. Legacy public FenceValue remains an observation; external mutation is rejected on submission.
- `WaitFor` resolves only a live local timeline or a specifically imported remote producer. Unknown, reset, invalid or unimported points are rejected before native GPU Wait. Wait HRESULT is checked. No local submission number substitutes for remote completion.
- Existing `WaitForQueue(producer, value)` is retained, checks the value was actually issued by that producer and delegates to the same validation. Zero/future values are rejected.
- `GetCompletionProgress` retains producer provenance. UINT64_MAX remains an observation of device removal and never retires allocators; Wait/GetCommandList/CPU wait reject removal.
- Allocator retirement stores a private queue-issued point and its native fence, independently of public FenceValue. List/queue type and queue ownership, duplicate/null/empty batches and re-execution are checked. A submitted list whose Signal fails cannot be recycled without retirement.
- Queue reset drains native work, invalidates its identity/import provenance, unregisters and releases resources. Device reset also resets caller-owned registered export queues before native device release. Recreated devices/queues receive new IDs. Explicit Reset propagates errors after cleanup; destructors preserve native error text in debugger output because they cannot throw.
- COPY uses native COPY allocators/lists. Wrapper graphics/compute/DXR commands and unsupported COPY state/UAV/split barriers are rejected before recording. Allowed legacy COPY transitions are COMMON/COPY_SOURCE/COPY_DEST. Raw `GetCommandList()` remains a backend escape hatch whose commands require D3D12 validation.
- DXR SetPipelineState1/DispatchRays/BuildRaytracingAccelerationStructure remain permitted on DIRECT and COMPUTE. SetPipelineState1 invalidates the cached ordinary PSO to allow subsequent rebinding.

The dormant legacy `RenderModule::ShareFences()` / symmetric numeric `WaitForOtherFence` path is **not migrated or enabled**. Its raw mutable GetOtherFence cannot establish provenance. WaitForOtherFence accepts only a view established by ImportTimeline and an issued producer value; otherwise it fails before native Wait. P4 must replace the dormant helper rather than enable its known mismatched-counter behavior. Current active single-GPU graph callers use WaitForQueue and retain their signatures.

Queue Wait is a GPU operation that returns immediately to the CPU ([Microsoft API contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-wait)). Explicit cross-adapter fence flags follow the [fence flags contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_fence_flags). The fixture uses placed buffers on an explicit shared cross-adapter heap, matching [shared heap restrictions](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps), and buffer COMMON promotion/decay at submission boundaries per [legacy state synchronization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12). COMPUTE DXR binding is supported by [SetPipelineState1](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist4-setpipelinestate1).

## Implemented checks — executed successfully

`MGPUQueueTests --model-only`: provenance type assertions, invalid token and device-removal sentinel behavior, null/uninitialized queue/list/device, unsupported BUNDLE and queue-before-initialization rejection. No fake global queue class, forged valid token or production test backdoor.

Native fixture:

- Required single physical GPU DIRECT -> COMPUTE -> COPY -> DIRECT buffer payload chain, exact per-queue signals, equal numeric values on distinct timelines and imported/local progress provenance.
- Invalid/unknown timeline/token, future/zero native values, counter sentinel/overflow, wrong queue ownership/type, duplicate/null/empty batch, invalid COPY graphics/compute/DXR commands and COPY UAV/shader-state transitions. Invalid inputs must throw C++ validation errors, not merely fail inside D3D12.
- A fixture-only CPU-signaled gate delays issued COMPUTE work. DIRECT Wait must return while the producer remains incomplete; pending allocator Reset/re-execution and reuse are rejected; another available allocator can submit; completed allocators are subsequently reused. A five-second watchdog releases the test gate on failure so this negative case does not leave a permanent queue hang. This is a correctness timeout, not a performance benchmark.
- Positive COMPUTE SetPipelineState1 with a minimal real DXR pipeline when the physical adapter supports DXR; unsupported DXR reports an explicit fixture subcheck SKIP. COPY DXR rejection is unconditional.
- Two-GPU enumeration compares physical adapter LUIDs and excludes software adapters. Cross-adapter Signal/Wait, a producer with the same numeric token on a different timeline, unimported/nonshared/wrong-view/stale imports, 24 changing payload generations, slot release via the consumer's own completion and bounded handles over 32 recreated producer timelines.
- The physical two-GPU fixture SKIPs only without a second compatible adapter (DXGI_ERROR_UNSUPPORTED/E_NOTIMPL capability failure); unexpected HRESULT, E_INVALIDARG, debug error and data mismatch fail. `--require-two-adapters` makes unfulfilled two-GPU acceptance return failure. One device with two queues is never reported as MGPU.
- Explicit queue/device shutdown, caller-owned queue invalidation and device recreation with new identities. D3D12 debug ERROR/CORRUPTION messages fail before and after reset.

Actual hardware device removal is not induced. UINT64_MAX behavior is checked through the value model and the production retirement predicates; fault-injected native Signal/handle/identity exhaustion is not implemented.

## Actual validation results

Machine-readable record: `D:\Project\PEPEngine2\.tmp\mgpu-feature-2026-11-08\P1-ready.json`. Its original sequential commands remain available alongside final results and fingerprints. Before each build/test, the command read the lease and checked owner=P1, state=granted and the exact worktree. No other coordinated MGPU worker ran workloads during the P1 lease; this is not a global claim about every process on the machine. Performance benchmarks/FPS/preset tuning were not run.

All paths in the following table are relative to this worktree. Each workload has a JSON state with the exact command, exit code, stdout/stderr and before/after input SHA256. All successful rows returned exit 0, unchanged fingerprints and empty stderr.

| Check | Result | State/log prefix |
| --- | --- | --- |
| Local P0 dependency copies and /Z7 override | PASS, outputs confined to this worktree | `Build/P1/prepare.log` |
| Debug/x64 MGPUQueueTests, VS v145 | PASS | `Build/P1/queue-build3` |
| CPU/model-only invalid-input checks | PASS | `Build/P1/queue-model` |
| Queue fixture, D3D12 debug layer | PASS, physical two-GPU PASS | `Build/P1/queue-debug` |
| Queue fixture, debug layer + GBV + --require-two-adapters | PASS, physical two-GPU PASS | `Build/P1/queue-gbv` |
| Existing RenderGraphTests Debug/x64 build | PASS | `Build/P1/graph-build` |
| Existing RenderGraphTests debug layer | PASS | `Build/P1/graph-debug` |
| Existing RenderGraphTests debug layer + GBV | PASS | `Build/P1/graph-gbv` |
| Local Debug compile database regeneration | PASS, 35 projects / 125 translation units | `Build/P1/compile-commands` |
| Final task diff whitespace review | PASS | `git diff --check` |

Hardware actually exercised: NVIDIA GeForce RTX 3060 Laptop GPU, LUID **0:62942**, and AMD Radeon(TM) Graphics, LUID **0:57700**. Adapter names are recorded in `Build/bin/x64/Debug/MGPUQueueTests/Logs/Engine.log`; both queue run stdout logs record the distinct LUIDs. Neither physical two-GPU fixture skipped. Positive COMPUTE DXR SetPipelineState1 also passed. Each run reported the single-device chain, negative command/provenance checks, allocator retirement, 24 cross-adapter payload generations, stale import rejection, bounded handles and a clean debug layer.

Both existing graph runs reported scheduling/cycles, recording/rollback, queued frames/pooling/resize/fallback, multiple async producers, UAV texture aliases/prefix/retirement, RAW/WAR/WAW and **400 changing frames with concurrent texture readers**, followed by `Render graph tests passed; clean D3D12 debug layer.`

The fixture INIs contain only `[ConsoleVariables]`, following P0's documented startup workaround for the unrelated unknown `App.Water.HeightScale` setting. Original copies are `Build/P1/queue-Engine.original.ini` and `Build/P1/graph-Engine.original.ini`; tracked `Config/Engine.ini` is unchanged.

Two compile defects were found and fixed before successful execution: `Build/P1/queue-build` caught CD3DX12_RESOURCE_BARRIER's static Transition helper hiding the base Transition field (the COPY validator now reads the base native structure); `Build/P1/queue-build2` caught the new fixture's missing DXC include path (added only to its new project). Both failed logs remain available; `queue-build3` builds the final sources. No GPU fixture failure occurred and no production source changed after the successful queue build.

Final executable/source SHA256 values and per-run fingerprints are in the manifest/state files. The P0 header remains unchanged. Existing projects/filters/solution/runtime/graph sources and defaults remain untouched. No new requested/active technique CVars were added. Unrelated pre-existing LFS dirty files were preserved and excluded from the task commit.

Limits: no full Bistro/IBL suite, captures, long soak, fault-injected native failures, actual device removal or performance measurements. The two-GPU test covers P1's placed-buffer queue synchronization fixture; P3b/P4 transport/runtime and technique acceptance remain separate work packets. The queue/device API requires the documented external serialization; concurrent API safety is not claimed. Final process inventory (`Build/P1/processes-final.json`) found zero workloads with this P1 worktree in their command line. A pre-existing unrelated MSBuild PID 30988, created before P1 validation and targeting a temporary project, was preserved; global MSBuild absence is not claimed. All P1 runner child processes exited. The lead remains the only lease writer.
