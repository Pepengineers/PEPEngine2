# U5 lighting and reflections — source implementation

Status (2026-11-14): **реализовано, тесты и runtime validation отложены пользователем**.
Accepted base: `71f6f4258f26dc4d61ce8800f935a5ce9423d7f0` (U4), MidSpec,
branch `codex/meta-render-graph-u5`. Task HEAD and source/evidence SHA256 are in the
local `Build/Verification/meta-render-graph-u5/handoff.json`.

The final source has NOT been compiled or executed. During U5.1 the user prohibited
all further builds, CPU/GPU tests, runtime/visual validation, profiling and benchmarks.
All known validation sessions had finished before that instruction. Subsequent work
was implementation, source formatting and static source/diff review only. U5 is not
accepted; originating-chat independent code review precedes a separately authorized
validation session. U6 was not started. No push, PR, merge or other agent was used.

## Source contracts

The pipeline retains one device / DIRECT queue and the U3/U4 CPU recording executor.
No adapter calls `GDX12RenderGraph::Execute`. Pass resource/pipeline creation and
CPU parameter/scheduler algorithms are reused; native callbacks use unchanged
engine technique shaders. The adapters prepare resources, descriptors, constant
ranges, view selection and command descriptions before workers start. Workers do
not call GDX resource-state getters/setters, queue submission or CPU fence waits.

| Group | Packets / real accesses | Ownership, history and fallback |
|---|---|---|
| U5.1 `SceneEnvironment` | DDS upload, cloud source, atmosphere transmittance/MS/capture sky view, source cube mips, irradiance, GGX mips, BRDF, visible sky view and aerial volumes | Persistent source/IBL/LUTs return to COMMON. Uploaded asset memory is retained through submitted fences. Off writes opaque black cubes and retains BRDF. `Meta.Clouds.Enabled` opts into the local source; world pause/time scale resolves simulation delta before freezing. Source discontinuities and lighting revisions retain the original distinction. |
| U5.2 `SceneReflections` | Original probe CPU scheduler and cache selection; selected sun-shadow and HDR faces; real GPU culling and skeletal batching; interleaved mesh/grass views; baked color/depth reuse; GGX filtering; atlas publication copy; metadata upload with publication dependencies | Two independent serial/parallel cache states. Static-only and dynamic capture rules, face masks, update intervals, rebake pending and published bounds are retained. CPU-prepared metadata can describe a same-frame result only because GPU consumers depend on its publication copy. Public readiness is gated by a submitted frame fence; a prior completed cache remains ready during update. Disabled probes upload zero-intensity metadata and an initial defined black fallback. |
| U5.3 `SceneAO` / deferred lighting | Original SSAO/HBAO/XeGTAO/CACAO kernels, full/half grids, denoise/resolve/upsample; neutral AO when disabled. Early mode produces mesh+grass prepass normals and orders AO before both GBuffer producers. Late mode reads GBuffer normals. Light-grid upload and deferred reads include GBuffer, AO, packed U4 shadows, lights, IBL/LUTs and probe texture/metadata | Per-slot AO scratch/context/output, frame-global AO temporal sample index. Settings, light-grid policy and reflection settings are frozen once per frame. Early ordering remaps scheduling IDs while retaining immutable callback IDs. Native light-grid preparation uses the existing builder with frozen settings. |
| U5.4 SSR / composition | Original SSSR prepare, AMD SSSR + reflections denoiser, specular resolve, disabled HDR copy-through, original opaque aerial/WBOIT composition shader with neutral transparency, HDR readback | Separate serial/parallel persistent SDK histories. Camera/history/environment/settings and extent discontinuities recreate contexts on owner; rotating frame-local GBuffer addresses do not reset history. Reset requests apply at the next Freeze. Opaque aerial perspective is applied once. A minimal diagnostic display uses the existing tone-map shader; no reconstruction, output-provider, frame-generation or full post-chain migration is claimed. |

`SceneGeometry::Plan` exposes append hooks and complete physical bindings. Early AO
moves the original mesh GBuffer callback after mesh+grass depth and AO; redundant
initial diagnostic copies are omitted in that ordering. Final geometry readbacks
remain after completed views. `SceneGrass::Append` can append additional views
without advancing wind/interaction a second time. It retains stable callback IDs
and adds the actual capture lighting reads. Prepared grass constants remain in
retained blocks; owner view expansion does not invalidate previously frozen ranges.

`SceneShadows` retains U4's individual diagnostic depth targets and additionally
renders static-only depth from the original static culling results. Real mesh/grass
shadow values are copied into the original CSM atlas / local-array layouts by
`Shaders/ShadowAtlasCopy.hlsl`. This layout-copy helper preserves R32 depth values;
shadow evaluation still uses the original geometry, filters, matrices and shaders.
Static probe receivers use the static-only local views; grass remains dynamic as
in the existing reflection implementation. Packed outputs have defined white
fallbacks, private CPU clear descriptors and frame-slot ownership.

Shared renderer changes are restricted to adapter friendship and optional external
capture shadows/settings in the reflection CPU preparation pass. Null external
pointers retain the production path. The existing App/default pipeline is not
switched to Meta execution.

## SDK and sequential ownership

Cold allocation/descriptor replacement remains owner-sequential: environment
source/LUT changes, probe topology/quality/capture allocation, actual HDR resize,
SDK context creation/recreation, and slot/list capacity growth. These sections are
concrete; ordinary mesh/grass/environment/AO/lighting recording remains parallel.

CACAO owns its private scratch, descriptors and constant ring; context/screen
resources/settings are created on owner. One exclusive SDK dispatch imports
planned depth/normals/output and restores its output to the declared UAV boundary.
The next explicit AO packet transitions it for resolve. Its internal SDK resources
are not split into synthetic dispatches.

The bundled FidelityFX DX12 backend creates its fallback constant ring lazily in
`FallbackConstantAllocator` during first GPU-job recording. `Operation::OwnerRecording`
marks exactly that first real SSSR dispatch after context creation/recreation.
Executor records and closes that body on owner before workers/submissions, while
its prologue/epilogue and dependency/submission remain normal planned packets.
The schedule names this cold owner section. Later SSSR dispatches use the existing
private ring from workers. SDK-private scratch/history transitions remain inside
this exclusive SDK packet; imported resource states are restored by the SDK.
Contexts are independent between reference and parallel modes. An owner drain
handles frame-slot counts beyond `FFX_MAX_QUEUED_FRAMES` (4 in the bundled headers).
Owner-prepared body no-ops are named separately in CPU traces and should not be
counted as worker-native recording spans.

`Executor::Reserve` explicitly grows the selected slot only after `Begin` waited
its retirement fence. Execute still rejects an unreserved plan. The pipeline
reserves the compiled list count before launching workers; the initial pool is
1024 allocator/list pairs per each of three slots and can retain a higher water
mark for scheduled probe work. Resize/shutdown drain before replacing resources.
No automatic recovery of partially submitted frames or partially advanced SDK
CPU context state is added.

## Prepared evidence and consumer boundaries

The scene fixture now has an actual cached reflection probe in addition to U4's
mesh, independent skins, lights and grass. Native verification code reads AO,
deferred HDR, SSR HDR, composed HDR, GPU probe metadata and only logically valid
published probe mip ranges. Pending/unpublished ranges are excluded from readback.
`ReflectionGpuTests.cpp` checks native serial/parallel metadata/mips and finite lit
radiance. `LightingGpuTests.cpp` retains the original environment baseline and
cloud lifecycle fixture. These sources were prepared; the final U5 fixtures were
not run. They are not a substitute for the pending full baseline/temporal matrix.

`SceneLighting::LitHDR`, `ReflectedHDR`, `ComposedHDR` and `SceneGeometry::Depth` /
`GBuffer` expose same-frame graph inputs. Resource pointers are not completion
publications. Future water/transparency consumers must append their real reads in
the same plan and honor the returned completion/lifetimes. They should start from
reflected HDR before opaque aerial composition; composed HDR already includes
that aerial term. IBL, packed shadows and probe texture/metadata must be declared
when shading those consumers. No U6 technique was connected.

Diagnostic costs are explicit: duplicate probe and SSSR histories for A/B, per-slot
AO scratch/contexts, static depth and packed shadow views, four lighting readback
planes and valid probe mips, retained upload memory, plus allocator/list high water
marks. Final D3D12MA/driver/SDK memory and CPU recording overlap are unmeasured after
the validation ban. No Bistro acceptance, performance improvement, PIX capture,
GPU async or MGPU result is claimed.

## Historical U5.1 checks before the prohibition

Earlier partial Debug/Release x64 builds and smoke runs passed on RTX 5050 using
VS18 v145 and SDK 10.0.26100.0. The partial executable enabled the D3D12 debug layer,
retained U3 executor checks and U4's 8 serial/parallel pairs + 4 queued frames,
three slots, resize and shutdown. Logs reported legacy graph Execute = 0 and
D3D12 ERROR/CORRUPTION = 0. Those binaries predate the final U5.2–U5.4 implementation
and later U5.1 source refinements; they do not validate current HEAD.

Five fresh environment cases (off, DDS, atmosphere, clouds, advanced phase) compared
all output subresources against the original `GDX12EnvironmentIBLPass` and between
serial/parallel recording. Padding was excluded. Max raw difference was zero;
byte counts were 131216 off and 1245152 enabled. An eight-step persistent source
fixture covered pause/cache reuse, advance, reset, off/on and 1x/2x deltas, with
zero original-baseline and serial/parallel differences at each step. A delayed
DIRECT fence rejected environment readiness before GPU completion. Original pass
execution was an isolated baseline, not old render-graph execution.

Historical commands (NOT permission to run them now):

```powershell
& Apps/App.MetaRenderGraph/tests/Invoke-Build.ps1 -Configuration Debug -Target App.MetaRenderGraph
& Apps/App.MetaRenderGraph/tests/Invoke-Smoke.ps1 -Configuration Debug -Target App.MetaRenderGraph
& Apps/App.MetaRenderGraph/tests/Invoke-Build.ps1 -Configuration Release -Target App.MetaRenderGraph
& Apps/App.MetaRenderGraph/tests/Invoke-Smoke.ps1 -Configuration Release -Target App.MetaRenderGraph
python Apps/App.MetaRenderGraph/tests/Export-U5Evidence.py --configuration Debug
python Apps/App.MetaRenderGraph/tests/Export-U5Evidence.py --configuration Release
```

Historical raw files are local/ignored under
`Build/Verification/meta-render-graph-u5/{Debug,Release}-App.MetaRenderGraph/`:
environment serial/parallel/baseline binaries, lifecycle binaries/metrics,
CPU traces, schedule/operation maps where available, source-cube PNG previews,
runtime/config hashes and build logs. The updated exporter can collect full native
geometry/lighting/probe files in a future authorized run; it was not rerun after
the prohibition. No final lighting/probe images, traces or baseline metrics exist.

The first sandbox build failed with C1902/PDB-manager errors and its 600s runner
limit. Taskkill reported a credential error. The later approved native retry
succeeded without toolset/SDK changes. The inherited runner initially overwrote
`meta-render-graph-u4/App.MetaRenderGraph-Debug-{build,stdout,stderr}.log` before its
evidence root was changed to U5. U4 runtime/readback evidence and tracked sources
were preserved, but those three historical build log hashes are no longer intact.
Existing historical LNK4075/LNK4099 warnings remain; final compiler/link status is
unknown because current sources were not built.

## Pending authorized acceptance

After independent code review and a new user authorization, perform Debug/Release
Meta + production App builds, planner/recording regressions including owner cold
recording/cancellation, and relevant legacy default/custom checks. Then exercise:

- Original-pass baselines at matching settings for full lighting/probes/AO/SSR,
  with explicit image/readback metrics and justified tolerances.
- All AO implementations, full/half, early/late and switching; actual depth and
  shading normal correspondence.
- Probe first GPU readiness with a delayed fence, unchanged-scene cache reuse,
  scene/environment change, static/dynamic/frozen capture and scheduling policies.
- Clouds off/on/pause/reset/time scale and source changes in the combined pipeline.
- SSR off/on, camera cut/world reset, resize, multiple queued slots, SDK private
  lifetime/initialization and absence of stale/uninitialized shader reads.
- Combined resize/shutdown, debug-layer cleanliness, native CPU recording traces
  and measured resource/list/SDK memory. No validation is implied by static review.

Stop after U5 implementation handoff. U6 requires a separate decision.
