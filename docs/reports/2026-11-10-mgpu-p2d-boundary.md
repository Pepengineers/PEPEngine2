# MGPU P2d external views at the legacy boundary

2026-11-10. Status **validated**. Worktree p2b on codex/mgpu-p2d, base eddd473db4a9e86408c41dbe57fda8211d088023. P2d is a separate commit after accepted P2c. Older reports and validation logs stay in Build/P2b and Build/P2c. P2d setup archived the verified P2c executable before changing the common build output.

## Contract and implementation

The optional resolver on SetLegacySplit supplies the current whole-resource external views and their canonical owners, identities, modes and placed states. A buffer-only replacement can use the existing graph order and explicit legacy edges. A configured resolver that returns empty leaves the original pass in the legacy range. Texture links and their existing pinning remain on the prior P2c path.

The bridge checks actual native ownership, heap, declared state, canonical identity and stable handle before lending a view. DEFAULT loans release to COMMON after the legacy segment's GPU join. UPLOAD stays fixed GENERIC_READ and read-only. The replacement uses the real same-primary DIRECT sequence. Successful replacement reacquires the declared legacy state or the captured post-prefix state for reads. Proven no-submit fallback reacquires the captured post-prefix state for every loan. Mutated native handles, inconsistent declarations, stale receipts and uncertain submission reject.

Each frame retains canonical resource owners and copies of the original native allocation handles across its splits. The primary and local COMPUTE lists keep this ownership, including the open final DIRECT list that P4 submits. P1 retirement protects reset and release. Completed idle lists may retain ownership until their next reset or queue teardown. P4 still binds transport reader leases to their actual placed or legacy tail receipts.

Geometry vertex and index buffers now expose ShareVertexResource and ShareIndexResource. Their owners and state trackers are shared by resize, skinning and IA. Geometry replacement allocates a new tracker and preserves earlier retained handles. The UPLOAD tracker initializes to the actual native GENERIC_READ state. Public BeginSkinning, EndSkinning and GetVertexAddress callers keep their signatures. The audit found no private raw geometry buffer caller requiring a RenderModuleSkinning patch.

P4 must supply a complete per-frame inventory and a real shared owner for each external resource. The first bridge path uses primary DIRECT for borrowed resources. Incoming bank views use existing transport readiness and reader leases. P4 owns multiple-technique dispatch and frame/descriptor retirement. S2's caster and material inventory and W/G's display binding remain separate production work. See the central P2d-boundary-contract.md for the exact API and constraints.

## Fixture coverage

- A CPU case accepts a buffer-only split with an explicit resolver and rejects a boundary-less split without one.
- Native mesh growth retains both prior vertex and index handles. Skinning and IA update the canonical state.
- Two external-view points share an allocation. An issued gated placed writer releases its view, legacy reacquires the success state, and a later no-submit fallback restores the captured state. Legacy COMPUTE and readback check both payloads.
- Fixed UPLOAD and missing resume, duplicate identity and mutated native handle negatives exercise preflight and no unsafe fallback.
- A separate gate holds the actual final DIRECT submit. The fixture retires the placed work and recycles earlier lists, then checks that the final list still owns the loan. Reusing the completed tail releases it.
- The existing P2b physical two-GPU and P2c texture bridge fixtures run alongside the new local tests.

## Validation receipt

All four workloads ran sequentially under the exact P2d lease through peengine-perf run_job.py. P2d-ready.json records the exact commands and timeouts. The build used the host v145 toolset, Debug/x64, /m:1 /nr:false, local dependencies and the validation-only /Z7 override. Frozen source and executable fingerprints matched before and after their workloads.

| Check | Result | State |
| --- | --- | --- |
| Debug/x64 RenderGraphTests build | PASS with no warnings or errors | Build/P2d/build-debug.json |
| RenderGraphTests --compiler-only | PASS with CPU declaration coverage | Build/P2d/compiler-only.json |
| External boundary, legacy, placed and physical two-GPU debug layer | PASS with clean debug layer | Build/P2d/boundary-debug.json |
| External boundary, legacy, placed and physical two-GPU GBV | PASS with clean debug layer | Build/P2d/boundary-gbv.json |
| Scoped diff check | PASS | Nine task files |
| Own process audit | PASS with OwnWorkloadCount=0 and ChecksComplete=true | Build/P2d/processes-final.json |

The new fixture validated canonical mesh ownership through growth, buffer-only split ordering, fixed UPLOAD, state release/reacquire on success and fallback, and ownership on the submitted gated DIRECT tail after earlier lists were recycled. Both GPU runs also passed the P2c early/late texture bridge and the P2b physical HAO cases on NVIDIA RTX 3060 Laptop GPU LUID 0:62869 and AMD Radeon Graphics LUID 0:57666. CPU and GPU stderr were empty.

The final executable SHA-256 is `8309d5ae9481831ae0f97d89912a322e60209de99718b2c0bee548ee9954df7a`. Build/P2d/P2c.accepted.RenderGraphTests.exe has the preserved P2c SHA-256 `c3b356bc7184ea2b0ca47ee463fb742b193200c5e4e1840aa1053be7b36246d4`. The immutable INI seed SHA-256 is `33395d44fd4eda9d2a0c6a0c7940663987c2e3ef91b52484884276c440b9cb8c`. The captured debug and GBV runtime INIs have SHA-256 `33395d44fd4eda9d2a0c6a0c7940663987c2e3ef91b52484884276c440b9cb8c` and `33395d44fd4eda9d2a0c6a0c7940663987c2e3ef91b52484884276c440b9cb8c`, respectively.

The test covers the boundary mechanism and local/physical graph regression. Actual S2/W/G/U2 pipeline wiring, production parity, benchmarks and long soaks remain outside this validation.
