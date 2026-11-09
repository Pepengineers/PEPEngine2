# MGPU P2c legacy graph bridge

2026-11-10. Status **validated**. Worktree p2b on codex/mgpu-p2b, base 4dec8d64c3275944272a4a689cc4293f7f6ac4b1. P2c extends the accepted P2b commit in the same worktree. P2b validation logs remain in Build/P2b. The P2c setup archived the accepted P2b executable by its verified hash before the shared build output changed.

## Bridge

SetLegacySplit accepts an existing legacy pass and ordered input and output links. Each pass has at most one point. Updating a point validates the complete replacement before changing the graph. The compiler checks that each point is live and widens every boundary texture and forwarding candidate to the full graph lifetime. This prevents a pooled resource slot from being reassigned while a boundary is exposed.

ExecuteLegacySplit records ranges of the original legacy loop, in compiled execution order. Each boundary retains the same physical texture and tracker owners, joins that segment's local COMPUTE work on DIRECT, releases the views to COMMON and submits the prefix. The callback receives the actual issued DIRECT point and the exact views for that pass. On success the callback returns a newer issued point on the same primary DIRECT timeline. FIFO then orders the next legacy range, including its COMPUTE forks. The bridge keeps the existing tracker and validates native identity and COMMON state on return.

A callback may return nullopt after proven no-submit. That point runs its original legacy pass. Earlier successful replacements and frame statistics remain in effect. The bridge rejects a stale receipt or a fallback after primary DIRECT advanced. IsSubmissionFaulted reports whether the placed executor encountered a potentially submitted failure. P4 owns any failed transaction recovery and retains the frame resources and descriptors through the final DIRECT receipt. ExecuteLegacySplit returns the open final list for the existing UI and Present path.

The external contract supports multiple points and multiple outputs. P4 owns route selection and the transport transaction. The scope covers stable linked DEFAULT textures, per-segment joins, and same-primary-DIRECT placement at the boundaries. P4 supplies initialized views and actual Finalize receipts and owns the replaced pass's logical writes or other side effects. All physical shared inputs and outputs of the placed plan are declared. Exclusive per-invocation private resources can stay in retained owners as documented in the private domain contract.

## Fixture coverage

- CPU rejection for missing reads and writes, unknown links, duplicate outputs, wrong access modes and split points on culled passes.
- Pooling control without a split, followed by disjoint physical resources when the input, output and fallback candidate are pinned.
- Early and late link selection, two split points configured out of order, a successful two-output placed replacement and an independent no-submit fallback. A gated COPY lets the issued DIRECT prefix complete before Finalize. COMPUTE reads the frame inputs before the split and checks the placed and fallback outputs after it. The final DIRECT receipt covers both local joins.
- Prefix receipt rejection and rejection of a nullopt callback that advanced primary DIRECT.
- Existing legacy tests and the P2b native physical two-adapter fixture continue to run in both GPU validation modes.

## Validation receipt

Workloads ran sequentially under the P2c lease through peengine-perf run_job.py. Each used the command and timeout in P2c-ready.json. Source and executable fingerprints matched before and after each workload.

| Check | Result | State |
| --- | --- | --- |
| VS v145 Debug/x64 build with local /Z7 target | PASS, no warnings or errors | Build/P2c/build-debug.json |
| RenderGraphTests --compiler-only | PASS, no GPU fixture initialized | Build/P2c/compiler-only.json |
| Debug layer, bridge and P2b physical regression | PASS, clean debug layer | Build/P2c/bridge-debug.json |
| GPU-based validation, bridge and P2b physical regression | PASS, clean debug layer | Build/P2c/bridge-gbv.json |
| Scoped diff check | PASS | Four task files |
| Process audit | PASS, OwnWorkloadCount=0 and ChecksComplete=true | Build/P2c/processes-final.json |

Both GPU runs exercised the P2c bridge on primary and the P2b native HAO round trip on two physical adapters, NVIDIA RTX 3060 Laptop GPU LUID 0:62869 and AMD Radeon Graphics LUID 0:57666. The bridge fixtures verified two replacement points, two outputs at the first point, fallback at the second, legacy COMPUTE joins on either side, and full texture readbacks. Legacy regression also completed 400 changing frames with concurrent texture readers. CPU and GPU stderr were empty.

Final executable SHA-256 is `c3b356bc7184ea2b0ca47ee463fb742b193200c5e4e1840aa1053be7b36246d4`. The archived P2b executable in Build/P2c/P2b.accepted.RenderGraphTests.exe has SHA-256 `ebd23d086f959f1c543f01fb27c00119e7cd48027477587c10759b7bcfe1a6da`. Each GPU job started with the immutable minimal INI seed SHA-256 `33395d44fd4eda9d2a0c6a0c7940663987c2e3ef91b52484884276c440b9cb8c`. The separately saved debug and GBV runtime INIs had SHA-256 `33395d44fd4eda9d2a0c6a0c7940663987c2e3ef91b52484884276c440b9cb8c` and `33395d44fd4eda9d2a0c6a0c7940663987c2e3ef91b52484884276c440b9cb8c`.

P4 production wiring and technique parity remain separate. The bridge fixture uses a local gated COPY dependency. The physical two-adapter test covers the fully placed P2b route. Benchmarks, soak and device-loss injection were outside this validation.
