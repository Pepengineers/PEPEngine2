# U12 implementation handoff

Date: 2026-11-14. Reviewed source head: `eef2fe07` on local
`codex/bistro-rendering`, merging U11 `11657349a1acb7dca4044df8a3b1dbf25d7408ac`
after runtime integration `1fc3b05f` and `11753b2d`.

**Source implementation complete; combined runtime not run.** U12 closes the
source review and documentation handoff under the user's implementation-only
instruction. It does not claim hardware acceptance of the merged code.

The user requested completion of the implementation on the existing procedural
scene. Bistro migration is deferred. This phase permits source inspection and
local integration, with no builds, tests, application runs or captures.

## Current source coverage

| Requirements | Connected implementation | Status |
|---|---|---|
| R1–R3 | Separate executable and startup files; `IRenderPipeline::OwnsExecution` selects Meta `ExecuteFrame` instead of legacy graph execution. Existing technique kernels are reused. | Source connected. Procedural scene, not Bistro. |
| R4–R6 | `MetaGraphRecorder` nested children/continuations; `MetaGraphExecutor` waits for and submits individual packets through `QueueSubmission`; CPU-ready and GPU tokens remain distinct. | Source connected; some technique recording stays owner-only. |
| R7–R8 | Compiler access dependencies/barriers, per-device queue tokens, frame/producer slot retirement and resource ownership. | Source connected; no new runtime result in this phase. |
| R9–R10 | Local water/particle rendering, common transparency/history, selectable single-DIRECT versus local COMPUTE/COPY placement. | Source connected. Ordinary scene now creates water and four continuous emitter groups near the camera. |
| R11 | Independent cross-adapter/water producer contexts, StrictPublication and LatestCompleted, common submissions and bounded publication lifetimes. | Source connected; no local MGPU run. |
| R12–R13 | U11 eligible particle offload, one particle payload export/import, real interaction input and common `SimulationDomain` device/submission ownership with water. DepthCollision stays primary; local/imported particles share sorting and resolve. | Source implementation complete and reviewed; not run. |
| R14 | Explicit placement and independent work; no automatic quality scheduler or required FPS/utilization target. | No overlap or performance claim. |

## Integrated corrections

`1fc3b05f` imports the completed MisSpec runtime corrections while preserving
U9/U10 stop/retirement hooks: swapchain/provider drain and UI ownership,
descriptor allocation, probe/AO/SSR bindings, radiance selection and render
extent handling. `11753b2d` removes diagnostic readback copies from ordinary
plans while preserving the existing diagnostic host. Diagnostic buffer
allocations remain. See [runtime-fixes-integration-2026-11-14.md](runtime-fixes-integration-2026-11-14.md).

U11 adds particles-only and joint water/particle modes using
`Meta.Particles.Enabled`, `.SecondaryAdapter` and `.Selection`. With water active,
particle adapter `-1` shares its device; explicit different devices are rejected.
Each producer advances independently. The particle batch contains emitter
identity/count/range metadata and render data; its export/import count does not
grow with emitter count. Real interaction cells use a separate shared input
snapshot. Initial/repeated display packets do not read a pending received buffer.
See [11-mgpu-particles.md](11-mgpu-particles.md).

## Evidence and limits

The [earlier local DIRECT smoke](local-direct-smoke-2026-11-14.md), delivered in
`aabb2744`, displayed procedural planes and grass on the RTX 3060 Laptop GPU and
exited with code 0. It covered that source/configuration only, before the
integration commits above. The original MisSpec runs likewise apply to their
recorded branch/configurations, not automatically to this combined source.

Current integrated-source result: **not run**, by instruction. Source review
covered project/caller connections, shared device/submission ownership, particle
eligibility, batch transfer, display history and retained water hooks. The parent
and an independent reviewer also reviewed U11 before its merge. Native and FSR
are in scope; FG and other reconstruction providers are deferred. Production App
remains on its old pipeline. The new procedural content has not been visually
checked, and Bistro has not been migrated.

The [README](../../../Apps/App.MetaRenderGraph/README.md) describes ordinary
launch and startup configuration. Historical reports/inventory entries retain
their original results; their old numerical, edge-case, timeline and matrix
requirements are superseded by the current
[Verification Contract](../../plans/2026-11-13-meta-render-graph/00-meta-plan.md#verification-contract).

No tests, builds, shader compilation, GPU runs or profiling were performed for
this handoff. Documentation received a scoped diff check. A later authorized
runtime acceptance needs only the short ordinary smoke specified by the current
contract; it does not require the historical expanded suites.
