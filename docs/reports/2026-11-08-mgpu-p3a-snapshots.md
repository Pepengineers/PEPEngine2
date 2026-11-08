# P3a: bounded snapshot protocol

2026-11-08. Worktree: `D:\Project\PEPEngine2\.tmp\mgpu-feature-2026-11-08\p3a`.
Branch: `codex/mgpu-p3a`; base: `79c6dc3fde0688715c3669375b09b71c7bd85b16`.
Status: **checks complete**; Debug/Release standalone builds and CPU tests passed under P3a's granted lease. No native/GPU workloads were run.

## Scope and integration

New `GDX12SnapshotProtocol.h/.cpp` implement a CPU-only lifetime/selection model using the unchanged P0 `TransferVersion`, `CompletionPoint`, `FenceProgress`, and frame/epoch identities. No D3D, native resources, queue implementation, runtime/pipeline/graph changes, technique payload/format abstraction, settings, or CVars are introduced. Defaults remain unchanged.

`Apps/App/MGPUSnapshotProtocolTests.vcxproj` is a standalone C++17 CPU application: its only translation units are the protocol cpp and `tests/MGPUSnapshotProtocolTests.cpp`. It has no project references or renderer/native queue linkage. The test-only global `GDX12CommandQueue` is the sole model issuer, using the existing P0 private friendship. **Never add that test source to a target containing the real queue.** No production test backdoor was added. Integrator later adds the protocol cpp/header to the renderer project; P3a does not edit existing projects/filters/solution files.

The new target uses v145, `/W4 /WX`, `/Z7`, and no linker PDB, following the known local toolchain limitation in P0. Both Debug and Release run the same always-on `CHECK` checks; Release does not compile them out.

## State machine and invariants

1. `TryReserve(version, mode)` accepts one new producer invocation per stream. Accepted invocation IDs strictly increase within the active epoch. Latest reserves a shared credit; Exact reserves shared **and** local credits in one mutex-protected transaction. A successful reservation is only a symbolic ticket, never a fence value.
2. `BindProducer(ticket, producerReady)` records a real successful producer Signal. `BeginExport` reserves the export stage and returns that point for native queue ordering. `BindExport` separately records export-ready and source-copy-complete points. Merely completing the producer does not permit overwriting its source.
3. `BeginImport` requires an issued export token. Latest additionally requires an observed completed export, avoiding an import queue wait for a future optional publication. Exact may return an import plan for an incomplete but already signalled export; its local credit was reserved earlier. `BindImport` records local import-ready and shared-read release separately.
4. `SelectLatest(frame)` chooses only a completed LOCAL import of the active compatibility epoch. With newer export/import busy it repeats the compatible displayed snapshot. It returns immutable value metadata for current and previous displayed versions, holding both slots. Repeated selection for the same frame cannot advance to a newly completed version. Frame IDs never derive from producer invocations and are not reset with the epoch.
5. `AcquireReader(ticket)` reserves one bounded reader credit and returns the local slot, exact version, and real import-ready point. It can precede import completion only while Exact is pinned; native readers must wait on that issued point. Optional/Latest tickets require an observed completed local import even for direct acquisition outside SelectLatest. Regression checks reject pending optional acquisition without consuming reader credit, retain the completed displayed snapshot, and allow acquisition once the new local import completes. Multiple passes/queues acquire distinct reader credits. `BindReader` records each reader's actual release; `CancelReader` only releases an unsubmitted reader. `ReleaseExact` transfers the demanded-version hold to at least one already reserved reader. Retain the Exact pin until all intended readers are acquired.
6. Shared reuse requires completed producer, export, source copy, import, and shared-release as applicable. Local reuse requires completed import, every reader release, and absence of Exact/current/previous holds. Completed unpinned local snapshots may be evicted only when taking a local credit. Optional BeginImport additionally preserves the newest compatible completed candidate between frame selections, preventing a fast producer from replacing every available completed result with in-flight imports. Exact reservation may evict an unpinned completed candidate; its demanded-generation priority is unchanged. There is no modulo overwrite. Shared/local indices and their rates are independent.

On Exact exhaustion `Deferred` contains no ticket/slot/token and consumes no partial credits. One deferred Exact demand per stream is retained, blocking later optional/new demands until the same generation is retried successfully, explicitly cancelled by `CancelDeferredExact`, or invalidated by reset. This follows the plan's initial single sequential producer per technique; callers keep additional demands outside the model and retry them. Required generations are never counted as optional drops. Optional completed writer publications can be explicitly discarded by `DropOptional`; in-flight writers, Exact pins, pending imports, display pins, and readers prevent dropping.

`Observe` checks each point against its original full timeline identity. Progress on another queue, adapter, replacement fence, too-small value, invalid point, or `UINT64_MAX` does not satisfy it. Points on different timelines are never reduced to one maximum value. Same-timeline backward stage/release points are rejected. Correct cross-timeline Wait edges remain the native transport/graph's responsibility.

`Reset(newEpoch)` requires a strictly increasing epoch, clears deferred demands and displayed compatibility, and stops new stages/readers for old publications. It does **not** clear pools or completion dependencies. Already-begun unbound producer/export/import and reader work holds its lifetime until a late successful `Bind*` or explicit cancellation. Late bindings can retire old storage but cannot republish it into the new epoch. New-epoch reservations reuse the existing bounded allocations only after those credits retire; no extra pool is allocated on reset.

`Cancel` certifies that still-unbound work has not been submitted, marks the publication retiring, cancels its pending stages, and retains all already-bound completion points and readers. Do not call it as a shortcut after a native submission whose Signal/binding has not been resolved. Cancelled Exact dependencies must be removed before dependent submission. Device removal leaves incomplete storage retained; the caller stops the MGPU path and handles native teardown rather than pretending removal is successful completion.

Tickets have a private issuer and shared owner identity, scoped to a Protocol instance. Serial IDs never recycle across reset; an owner identity survives as long as copied tickets do. `SourceReusable` remains valid after `Collect` removes a completed publication: only an opaque ticket minted by this owner with a previously issued serial can prove retired source lifetime. Default/foreign tickets return false, including another Protocol with identical TransferId/epoch/serial. No per-publication tombstone journal is retained.

Memory is bounded by shared slots + local slots publication records, their two fixed owner arrays, a fixed reader-credit array, and one owner identity. Defaults are 3 shared, 3 local, 32 outstanding readers. Capacity, ID exhaustion, duplicate/older invocation, invalid transitions and stale epochs fail without overwriting live storage. P3b owns physical allocation aliases and source buffer identities; a slot index is not an AllocationId or a native resource state.

## P3b handoff

Construct one Protocol for each logical transfer stream and one destination-local pool. Map returned shared/local indices to the resources and per-device views owned by P3b. Technique payload, dimensions, revisions, simulation time and checksums must be bound as one immutable bundle to the ticket/version; readers cannot assemble metadata from another ticket. Additional incompatible technique revisions start a new epoch.

Sequence native work as follows:

```text
TryReserve -> producer submit/Signal -> BindProducer
BeginExport -> queue Wait(producerReady), copy/Signal -> BindExport
BeginImport -> queue Wait(exportReady), copy/Signal -> BindImport
AcquireReader -> queue Wait(importReady), read/Signal -> BindReader
Observe each original fence timeline; keep polling issued points even if seen before Bind
```

`BeginExport`/`BeginImport` mark unresolved submission windows before touching native queues. Serialize native submit-and-bind with Reset/Cancel in the transport owner; the model's mutex protects credit transactions, not native calls outside the object. On failed/unsubmitted work, cancel it explicitly. On partial native success, retain/resolve issued completion rather than erasing its reservation. Runtime calls SelectLatest once per frame and acquires readers for all current/previous passes before advancing frames. Hold an extra reader when previous displayed data must outlive that selection window.

The graph executor still validates FIFO and cross-invocation/slot-release edges; this model does not manufacture points for planned batches and cannot prove arbitrary graph acyclicity. Physical shared heap flags, alias registration, texture footprints/format validation, source-resource protection across multiple publications, imported fences/handles, and GPU tests belong to P3b/P2b. Protocol lifetime alone is not proof that an arbitrary technique source buffer is safe to overwrite; gate reuse by its ticket's source-copy completion and retain any additional technique users.

## Authored tests

The executable includes negative transition/identity tests, invalid/backward points, Exact shared and local exhaustion with no partial reservation, demanded-generation retry/priority, unavailable import token before producer/export, Exact in-flight import dependency, multi-reader/multi-timeline release, bounded reader credits, source-vs-export readiness, too-small/wrong/replaced timelines, device removal, writer-protected optional drops, displayed/previous/repeated snapshots and same-frame immutability, incompatible epochs, reset during each unresolved submission stage, explicit cancellation, stale late result retirement, and source-reuse query after reclamation with foreign-ticket rejection.

Three 420-tick deterministic rate fixtures use separate producer/frame counters and fake sequence/epoch/checksum bundles: producer/consumer periods 1/5, 7/1, and 2/3; independently delayed export/import/readers. They verify shared payload before import, every current/previous selection, and each held reader's local payload before and after new writes. Every tick checks pool/reader/publication bounds. The original `selected > 10` assertion remains unchanged; `lastSelected > firstSelected` additionally rejects perpetual repetition of the first result. At least two distinct versions over 420 model ticks is a deliberately minimal liveness requirement for these finite delays, not an FPS/cadence target. The printed distinct count is `selected - repeated`. These are model correctness fixtures, not benchmarks or GPU copies.

## Validation results

The first Debug build passed, but the first CPU run **failed**, `line 537: selected > 10`. Its original state and logs remain in `Build/P3a/debug-test.json`, `.stdout.log`, and `.stderr.log`. The first 1/5 fixture could evict each completed unpinned local result into another pending import before the next frame selected it. Pool bounds and payload checks alone did not prove selection liveness. Fix: preserve the newest completed compatible candidate only for optional local-credit acquisition. A focused regression now verifies Deferred instead of evicting the sole pre-frame candidate; a separate Exact case proves demanded reservation can still evict an unpinned candidate. The unchanged rate schedules also verify ongoing advancement after first selection. Intermediate `debug-build-r2.json` is a build-only artifact; it is not counted as a test pass.

Final acceptance, sequential and with an exact `owner=P3a`, `state=granted`, worktree check immediately before every workload:

| Check | Result | State (stdout/stderr use the same basename) |
| --- | --- | --- |
| Debug/x64 v145 standalone build, `/W4 /WX /Z7` | PASS, no compiler warnings | `Build/P3a/debug-build-r3.json` |
| Debug CPU suite | PASS, exit 0 | `Build/P3a/debug-test-r3.json` |
| Release/x64 v145 standalone build, `/W4 /WX /Z7` | PASS, no compiler warnings | `Build/P3a/release-build.json` |
| Release CPU suite | PASS, exit 0 | `Build/P3a/release-test.json` |

Both CPU configurations reported the same deterministic progress:

| Producer/consumer period | Export/import/reader delay | Selected | Repeated | Distinct | Deferred publications | First→last selected invocation |
| --- | --- | --- | --- | --- | --- | --- |
| 1/5 | 2/3/9 | 83 | 27 | 56 | 357 | 3→392 |
| 7/1 | 2/2/5 | 410 | 351 | 59 | 0 | 1→59 |
| 2/3 | 11/13/17 | 132 | 104 | 28 | 178 | 1→191 |

The runner checked SHA256 inputs before/after every workload; final accepted states are `passed`, not censored. Exact executed commands, 180-second build / 60-second test timeouts, fingerprints and paths are recorded in `D:\Project\PEPEngine2\.tmp\mgpu-feature-2026-11-08\P3a-ready.json`. MSBuild used `/p:PreferredToolArchitecture=x64 /m:1 /nr:false`; outputs were worktree-local. No checks were added or repeated after final PASS.

Process ownership audit: CIM access was unavailable, so ignored `Build/P3a/process_audit.py` used Win32 Toolhelp parent relationships, runner root PIDs, and a pre-validation process baseline. It only inventories processes; it never kills unrelated workloads. No foreign PID, including the lead's previously noted PID 30988, was terminated. `Build/P3a/processes-final.json` records `OwnWorkloadCount=0`, `ChecksComplete=true`. P3a reported checks complete and left the lease unchanged for the lead to transfer.

Static review covered the state machine, API/ODR ownership, and all five task files. Whitespace checks passed; `core.safecrlf=false` only suppresses the repository's expected vcxproj LF/CRLF conversion notice. The local commit contains only the protocol header/cpp, standalone project/test source and this report. Existing tracked files, the common header, main checkout, other worktrees, and runtime defaults were not modified. No push.

Unverified: native/D3D transport and integration, physical allocation/fence correctness, GPU correctness and performance. Those belong to subsequent packets; CPU model PASS makes no native or FPS claim. Known P3a blockers: none.
