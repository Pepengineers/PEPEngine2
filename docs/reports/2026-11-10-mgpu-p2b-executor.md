# MGPU P2b native graph executor

2026-11-10. Status **validated**. Debug/x64, compiler-only, debug layer and GPU-based validation passed. The worktree is p2b on codex/mgpu-p2b at 97f4fac4e4c4767c2495a0544c3cbce506780857.

## Execution and ownership

GDX12RenderGraph::PlanExecutor(maxInFlight) consumes an explicit CompiledPlan with one recorded pass per batch. The runtime supplies QueueBinding pairs, ViewBinding identities with native resource owners, and ExactInputBinding reservations with issued completion points. TrySubmit returns an ExecutionReceipt indexed by PlannedBatchId. An empty optional reports backpressure. CollectCompleted polls outstanding work and Drain handles teardown. One externally serialized executor domain covers all placed invocations that share these resources. Queues and devices outlive that domain.

Preflight checks live device and queue identity, native resource ownership, binding epochs, resource flags, fixed heap states, allocation aliases, required wait paths, and exact versions before GPU work can be issued. The executor records and closes every command list before the first Wait or Execute. RecordPlacedCommands receives only the native resource IDs declared by the pass, including read fallback candidates. The recorder chooses its conditional input and records against that device. The executor restores bound host resource handles and state bookkeeping if a recorder changes them and recording fails.

The executor submits its own command lists and allocators through the native queues. P1 SignalCompletion issues each receipt point after the corresponding list is submitted. P1 WaitFor consumes real points immediately before dependent batches. The executor checks the expanded order when adding dependencies from earlier batches and earlier invocations. External wait points must already have been issued. Waits between these invocations therefore point to work already enqueued. FIFO stays intact within a placement, and independent DIRECT work can progress while a producer waits elsewhere.

Whole-resource handoffs use COMMON between batches. DIRECT and COMPUTE record explicit state transitions. Each COPY batch uses implicit promotion from COMMON and decay on its own ExecuteCommandLists boundary. UPLOAD and READBACK keep their fixed states. Transitions out of UAV state order the next use. Accesses to the same backing are conservatively serialized across queues, including readers whose transitions would overlap. The executor accepts a single native object per backing per device and cross-adapter aliases of shared buffers with compatible width. The transport provides the actual shared heap, placed views, and allocation provenance. D3D12 barriers and shared heap requirements follow the Microsoft contracts. [Resource barriers](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12), [Shared heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps), [Heap properties](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-getheapproperties).

A local Exact snapshot binds local import-ready. A cross-adapter shared view may bind a remote COPY export through SharedProducer and SharedSource. The executor checks the live issuing timeline and matching source adapter, allocation, stream and epoch. The transport supplies the binding between the publication and the bytes. A queued writer of a different version on that allocation invalidates an older Exact input.

Invocation owners, resource handles and command allocators remain live until every submitted timeline is proven complete. Wait or Signal failure after native submission stops new submissions and keeps the pending record. Drain observes issued dependencies while waiting and can issue a recovery signal for a list whose initial signal failed. Fatal teardown that cannot prove GPU retirement quarantines the bounded remaining owners through process exit. That path needs the higher-level device loss recovery policy.

## P3b and P4 seam

P3b prepares reservations, native per-device slot views, and copy-only recorders. RecordPlacedCommands only records commands. P4 maps its graph batch IDs to the returned producer/export/import/reader points, then binds those to the reserved transport publication in order. Partial native success requires quarantine until retirement is proven. An issued token from an earlier invocation may enter through ExactInputBinding.

P4 migrates individual techniques after identifying all physical accesses and per-invocation GPU owners. Existing passes with undeclared resources use the legacy Execute path. A staged graph can hand off to a migrated technique with a retained resource binding and an issued readiness point. One placed plan requires complete declarations throughout. The control directory contains the reviewed P2b-contract.md API handoff.

## Fixture coverage

The existing RenderGraphTests project runs legacy tests by default and keeps --compiler-only independent of GPU initialization. --placed adds the local placed fixtures. --require-two-adapters adds local fixtures and requires distinct physical adapter LUIDs.

- Local DIRECT → COMPUTE → COPY → DIRECT payload round trip and queue timeline verification.
- Rejection before submission for missing queues, wrong epoch, duplicate native aliases, foreign native resources, invalid UAV flags, non-exportable remote timelines, and missing, stale or unowned Exact bindings.
- Late recorder failure rolls back a changed host resource state without submitting an earlier recorded prefix.
- Issued pending local Exact, independent DIRECT prefix and frame tail, dependent partial join, bounded capacity, and per-invocation owner retirement with gated GPU progress.
- COMPUTE UAV texture overwrite followed by COPY footprint readback and descriptor retirement.
- Physical two-adapter HAO-shaped round trip with two queued generations. A separate gated shared-buffer test verifies remote issued Exact provenance, pending reader WAR, stale generation rejection and intact readbacks for both versions.

## Validation receipt

Each workload ran sequentially under the exact P2b lease through peengine-perf run_job.py. The ready manifest records the executed build and test command lines and their timeouts. The GPU fixture commands both included --require-two-adapters. State, stdout and stderr files share the basename shown below.

| Workload | Result | State |
| --- | --- | --- |
| VS v145 Debug/x64 build with local /Z7 target | PASS | Build/P2b/build-debug-r3.json |
| RenderGraphTests --compiler-only | PASS | Build/P2b/compiler-only-r2.json |
| Legacy and native graph fixture, debug layer, required physical adapters | PASS | Build/P2b/native-debug-r2.json |
| Legacy and native graph fixture, GBV, required physical adapters | PASS | Build/P2b/native-gbv.json |
| Scoped diff check | PASS | Five task files |
| Own process audit | PASS with OwnWorkloadCount=0 and ChecksComplete=true | Build/P2b/processes-final.json |

Both GPU runs passed on NVIDIA GeForce RTX 3060 Laptop GPU LUID 0:62869 and AMD Radeon(TM) Graphics LUID 0:57666. They included the 400-frame legacy reader fixture and all placed cases above. Both had clean D3D12 error checks and empty stderr. The final build fingerprints matched all four source files, and all final tests used executable SHA-256 `ebd23d086f959f1c543f01fb27c00119e7cd48027477587c10759b7bcfe1a6da`. Each GPU run began from the immutable local INI seed SHA-256 `33395d44fd4eda9d2a0c6a0c7940663987c2e3ef91b52484884276c440b9cb8c`. Separate runtime outputs `native-debug-r2.Engine.after.ini` and `native-gbv.Engine.after.ini` matched that seed.

The first build compiled graph and test sources then failed linking an LFS pointer in place of AkSoundEngine.lib. Local sources supplied 56 exact objects verified against their pointer sizes and SHA-256 values. Build/P2b/lfs-hydration.json records the objects. The first successful link used unchanged code. The hydrated files were local build dependencies and are outside the commit.

The first physical debug fixture read back every expected payload but failed the final InfoQueue check. CopyTextureRegion expected COMMON and found an explicit COPY_SOURCE transition on its COPY list. The corrected executor leaves the COPY default resource in COMMON for promotion and decay, while preserving explicit DIRECT/COMPUTE transitions and all validation checks. The same UAV texture readback then passed both debug and GBV. The failed evidence remains in Build/P2b/native-debug.json and its logs. The central contract carries this COPY boundary for P3b and P4. [D3D12 multi-queue resource access](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization#multi-queue-resource-access).

Application wiring and a production legacy-segment bridge are separate P4 work. GPU device-loss and failed native Signal quarantine were reviewed without fault injection. Performance, long soak and technique parity were not measured.
