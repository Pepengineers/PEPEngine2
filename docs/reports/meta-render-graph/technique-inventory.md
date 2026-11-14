# Current async single-GPU source status — 2026-11-14

Base: local `codex/bistro-rendering` at
`c444d58a30b0921c2d126dbf252ce116a0e6d196`, including the reviewed post/UI merge.
This section supersedes earlier queue/authorization status statements below.
**Source implemented; unbuilt/unrun, no runtime acceptance.**

| Technique / boundary | Queue placement and ownership | Deferred evidence |
|---|---|---|
| Minimal existing buffer fixture | `DirectFixture::Plan(..., true)` adds independent DIRECT clear, COPY upload, COMPUTE transforms, DIRECT/COMPUTE readers and DIRECT draw. Default fixture callers stay DIRECT. | Source only; no invocation/test scenario added or run. |
| Scene/material/frame and skin uploads | Four immutable scene upload buckets and palettes COPY; actual culling/count reset and skin deformation COMPUTE; depth/GBuffer ExecuteIndirect DIRECT. | Bytes, scene output and GPU timeline pending. |
| Particle simulation groups | Depth-independent A DIRECT; local depth-collision B COMPUTE; physically separate per-emitter pools and actual depth/interaction dependency. CPU preparation/recording stays owner-only. | Seeds/pause/reset/bursts/collisions and mixed output pending. |
| Decomposed particle consumer | Independent DIRECT clear/cull A excludes B pools and unused depth/atlas acquisitions. B append joins later; one global SortedAlpha sort, original combined drawing and one near WBOIT resolve. Particle draws remain after the join. | Water on/off independence, sorted alpha, WBOIT and motion pending. |
| Water/transparency | Existing owner-only water solver/surface/optics on DIRECT; mesh transparency can precede B consumer join. Original shaders and resolve boundaries retained. | No water async/parallel CPU claim. |
| Geometry/grass/shadows/environment/probes/AO/lighting | Existing native worker recorders retained; unselected work remains DIRECT. | Current source has no new runtime validation. |
| Post/reconstruction/FG/UI/posters | SDK/UI/post owner-only DIRECT, original per-slot provider links and retained FG state exports. Poster texture material reads restricted to DIRECT consumers. | All providers/FG/history/visuals pending; no SDK queue migration claim. |
| Retirement/lifecycle/errors | Actual queue/list identities and issued per-queue fence tokens; selected slot retired before material/owner preparation; terminal DIRECT joins queues; previous-frame GPU waits protect persistent resources even without prefix. Shutdown owner drain includes executor queues. | No per-frame global CPU drain/Flush introduced. Partial submit aborts; unknown completion quarantines. |
| Heap/state compiler | Explicit physical DEFAULT/UPLOAD/READBACK contracts; fixed upload/readback states; buffer decay and COMMON texture releases; queue-legal union states/UAV barriers and all-reader WAR dependencies. | Cross-queue DEFAULT readers are conservatively ordered, not advertised as concurrent. |

`Meta.Async.Queues=false` retains the single-DIRECT placement reference in the
experimental application. [08-async-single-gpu.md](08-async-single-gpu.md) records
coverage, errors, retirement, source review and remaining limits. No secondary
device/transport/offload worker, production pipeline switch, PR or Bistro merge.
No overlap, correctness, timings, memory figure or speedup has been measured.

**NOT RUN:** builds, C++/HLSL compilation, CPU/GPU/unit/integration/regression
tests, smoke/validation scripts, applications, GBV/debug-layer checks, readbacks,
images/visual checks, benchmarks or profiling. Existing test files received only
heap declarations; the reusable DirectFixture gained opt-in queue placement.
No new executable test was added.
Independent originating-chat review and later authorized runtime acceptance remain.

---

# Current post/UI source status — 2026-11-14

Authorized implementation base: local `codex/bistro-rendering`
`214c4ff19dd5047a941d9e31dde19ab4b7d0ca25` (corrected water/particle merge).
The source paths below are connected but **unbuilt/unrun; no runtime acceptance**.
This current section supersedes earlier authorization/status statements; historical
report bytes and evidence below do not validate the new code.

| Inventory position | Explicit packet/access path | Status and deferred evidence |
|---|---|---|
| DoF, motion blur, complete particle/water motion | `ScenePostUI.*`: original DoF SDK; complete-motion/velocity/tile/neighbor/gather or copy; camera and all scratch accesses | Source only. Owner preparation and recording; native output/history/image checks deferred. |
| Native and XeSS/FSR/DLSS/NIS reconstruction | Existing provider bindings in explicit DIRECT SDK boundary; current per-slot depth/motion/HDR links; render grid set before scene resize/freeze | Source only. Existing support fallback retained; every SDK/hardware path runtime pending. |
| HDR output/display | Existing OutputToScreen tone-map pass consumes reconstructed HDR and writes current output-grid backbuffer | Source only. Diagnostic full-resource copies removed from continued scene path; color/resize checks deferred. |
| UI offscreen/posters/world canvas | `BistroUIPosters` local-DIRECT effects/raster prelude, explicit effect/target writes and material reads including capture/shadows | Source only. Owner-only internal scratch/upload section; fonts/animation/world visibility/lifetime checks deferred. |
| Screen Engine.UI | Explicit capture/font upload/offscreen raster and ScreenComposite packets; per-slot target and retained recording owners | Source only. Existing screen content has no external image bindings; arbitrary future external images require the existing TextureBinding contract. |
| Frame generation / SDK / Present | Primary-DIRECT prepare/tag boundary after display; exported depth/motion stay NON_PIXEL_SHADER_RESOURCE through terminal packet and Present; owner EndRendering/Present callbacks | Source only. All Off/AMD/NVIDIA/Intel paths runtime pending; unsupported fallback remains existing SDK behavior. |
| Skinning and preparation outside legacy graph | Existing explicit palette/deformation and immutable scene uploads from geometry stage remain in use | No new parallelism/runtime claim. |
| Debug/collision-only variants | Shared original Jolt renderer, owner triangle/upload preparation, explicit display/upload packet; `Meta.Debug.Collisions` suspends FG and resets history | Source only. Existing intermediate HDR/geometry diagnostic paths retained for fixture callers; diagnostic A/B does not validate post/water/particles. |

See [07-post-ui.md](07-post-ui.md) for state exports, lifecycle, source review and
the exact list of checks not run. Geometry/lighting retain their existing worker
recording; post/UI/SDK and water/particles are explicitly owner-only CPU sections.
Production App remains on its existing pipeline. No async/MGPU/offload workers.
All testing/builds/application runs remain prohibited through implementation and
review of the water MGPU stage. Stop here for originating-chat static review;
no next stage is started by this inventory update.

---

# Current U6 source status - 2026-11-14

U6 explicitly authorized on U5 `a322796512233fc303fed4d54860081ca6a62767`.
Implemented in source; builds/tests/acceptance deferred by the user. Independent
originating-chat static review is pending. U5/U4 sections below are historical:
their older authorization statements and binaries do not validate U6.

| U6 position | New packet/access path | Status / ownership |
|---|---|---|
| Water solver/surface/resolve | `SceneWaterParticles.*`, App-owned fluid pass, original SPH/surface/NRF/optics kernels | Source connected, unbuilt/unrun. Local-only; private diagnostic histories; combined depth/motion feed downstream consumers. |
| Particle pools/controls/collision | Explicit initialization and simulation/dispatch packets, opaque depth and current grass field | Source connected, unbuilt/unrun. Original seeds/pause/reset/bursts/time/collision; depth-dependent input groups. |
| Particle render/global sort/motion | All-emitter culling/radix sort, weighted/feature-motion, SortedAlpha/additive packets | Source connected, unbuilt/unrun. One global sort across groups; no offload workers/private submits. |
| Transparency/WBOIT | Original far/near shaders on U4 immutable geometry; far resolve -> water optics -> near mesh+particles resolve | Source connected, unbuilt/unrun. Clear once per far/near boundary; opaque aerial once from ReflectedHDR; final HDR displayed. |

Owner-only U6 recording is an explicit KTD5 sequential CPU section. A strict
recording-local state scope restores declared packet boundaries. No U6 CPU
parallelism or GPU async claim. See [06-water-particles.md](06-water-particles.md)
for accesses/history/fallback/reset/resize, future payload/input fields and unrun
acceptance. U7 remains unauthorized. Existing inventory bytes below are preserved.

---

# U1 technique inventory

## Current U5 source status � 2026-11-14

Accepted U4 base: `71f6f4258f26dc4d61ce8800f935a5ce9423d7f0`.
**�����������, ����� � runtime validation �������� �������������.** The final U5
source has not been built or executed; independent originating-chat review and a
new validation authorization are required. Historical U5.1 checks cover an earlier
partial binary only. See [05-lighting-reflections.md](05-lighting-reflections.md)
for exact contracts, earlier evidence, memory/SDK costs and pending acceptance.

| U5 position | New packet/access path | Validation status / concrete ownership |
|---|---|---|
| Environment IBL, off/DDS/atmosphere, optional local clouds | `SceneEnvironment.*`; explicit upload/cloud/LUT/source/filter/BRDF/aerial packets; persistent output fences | Historical five-case original-pass baseline + eight-step cloud lifecycle passed before the ban. Current refinements are unvalidated. Cold allocation/descriptor work stays on owner. |
| Reflection probes | `SceneReflections.*`; original scheduler/cache CPU preparation; native mesh culling/batching and interleaved grass capture, GGX filter, publication copy + dependent metadata upload | Source connected. Public readiness uses a submitted GPU fence. Separate A/B caches; original static/dynamic/frozen rules; valid-mip readbacks and new fixture/check source prepared, not run. |
| AO / deferred lighting | `SceneAO.*`, `SceneLighting.*`; prepass normals and early ordering, late GBuffer inputs, SSAO/HBAO/XeGTAO/CACAO, clustered grid upload, actual lighting/shadow/IBL/probe accesses | Source connected, unbuilt/unrun. Neutral AO fallback. Per-slot AO state and global temporal index. Original U4 shadow depth is packed into lighting layouts; static-only local views feed baked probes. |
| SSR / opaque composition | `SceneLighting.*`; original SSSR prepare + SDK + resolve, disabled copy-through, opaque aerial composition with neutral transparency, diagnostic HDR display | Source connected, unbuilt/unrun. Separate persistent A/B SDK histories; real reset/extent keys; frame-slot imports retain history. First lazy-ring SDK dispatch is explicitly owner-recorded. |

One GPU / DIRECT and existing parallel CPU recording remain the source contract.
U5 is not accepted. U6 and subsequent stages are not authorized. The older U4
section below documents the accepted prerequisite and its own evidence.

## Current U4 migration status — 2026-11-13

Accepted base: `64f0ab1` (U3). U4.1–U4.4 are implemented; originating-chat review was accepted at `71f6f425`.
See [04-geometry-shadows.md](04-geometry-shadows.md) for exact commands, artifacts,
hardware, API changes, measured CPU traces, memory and limits. U5.1 progress is recorded above.

| U4 position | New packet/access path | Verified result / temporary parts |
|---|---|---|
| Object/material/frame uploads, GPU culling, ExecuteIndirect | `Apps/App.MetaRenderGraph/src/Techniques/SceneGeometry.*`: private immutable instance/template/material/transform/main/camera snapshots; four upload buckets; count reset/culling; native instance-batching stages | Debug/Release exact GPU bytes/images; five objects become four draws with two instances in one batch. CPU scene preparation/source freezing remains on owner. |
| Skeletal pose/deformation | `RenderModule::FreezeSkeletalMeshes`, `GDX12SkinningWork`; explicit palette copy and deformation packets | Two animated instances share immutable mesh data and retain separate times/palettes; expected GPU vertex positions and exact A/B. Cold source creation/loading remains owner-sequential. |
| Depth/GBuffer, mesh/grass views | `SceneGeometry` and `SceneGrass`: explicit attachments/accesses, native depth/GBuffer PSOs and indirect draws | Motion, invisible/far/empty visibility, all five GBuffer outputs and depth compared; three frame slots, resize and queued frames. Diagnostic albedo output. |
| Cascaded/local shadows, static/dynamic/update/cache variants | `SceneShadows.*`: private frame-slot/view matrices, descriptors, static/dynamic culling children, depth and readback packets; existing cascade/local matrix and shader algorithms | Four cascades, six point faces, spot, camera isolation and moving/animated casters. Diagnostic per-view targets replace packed atlas; selected cached views currently redraw. Shadow draws remain per-instance GPU commands. |
| Grass/wind/interaction; depth/GBuffer/shadow/probe expansion | `SceneGrass.*`: existing Bistro ECS snapshots/settings/LOD/prepared jobs; explicit root uploads, interaction versions, wind stages, argument clear, expansion and draw accesses | Ribbon/CrossedRibbons and projected/analytic wind; pause/reset/transform/no patches; previous-field/version bytes; six diagnostic probe geometry/GBuffer faces. Shared expansion arena has GPU WAR/RAW edges. Full probe capture lighting/readiness remains U5. |

Both final Meta configurations passed 12 presented frames: eight serial/parallel pairs
and four queued frames, zero legacy graph Execute and zero D3D12 ERROR/CORRUPTION.
Production App Debug/Release builds, legacy default/custom GPU checks and composition/
grass-lifetime tests passed. CPU planner/recording tests and inherited U3 GPU/error/
retirement checks passed. Every frame trace shows meaningful native recording on
three workers. This is a small real ECS/geometry integration fixture, not full Bistro
scene acceptance, a PIX capture, GPU async/mGPU, or a performance claim. Readbacks
and duplicate reference grass state are explicit diagnostic costs.

Baseline source: `dd012c0f` (2026-11-13). This is a source inventory of both current Default and Bistro compositions, not a claim that the U1 procedural fixture exercises Bistro. U1 links only App.Base/Engine. File names below are relative to the repository root; `DX/` means `Engine/Engine.RendererDX12/src/`, `Pass/` means `DX/` with the `GDX12` pass file named in the row, and `Bistro/` means `Apps/App/src/Rendering/`.

All legacy passes share `GDX12Resource::_currentState`, descriptor heaps, `GDX12DeviceResources`, frame constants and common camera/history state. `GetTransitionBarrier` mutates state during recording. No row is safe for arbitrary parallel recording merely because its geometry is split. Until U8, compute shaders must be recorded on DIRECT. Existing fixture names refer to `Apps/App/tests/`; they are starting points, not checks run in U1.

| Technique / current variants | Owning files | Inputs → outputs | Hidden submission / CPU waits and mutable ownership | Fixtures / migration owner |
|---|---|---|---|---|
| Object/instance/material/camera upload, GPU culling, ExecuteIndirect | `Apps/App.Base/src/RenderModule.cpp`, `RenderModuleSnapshot.cpp`, `Apps/App.Base/Include/App.Base/Systems/IndirectCommandBuildSystem.h`; `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12GPUCullingPass.h`, `GDX12GeometryBuffer.cpp` | ECS snapshots, meshes, frame constants → upload ranges, indirect buffers/counters | Frame slot CPU fence wait in OnUpdate; growth/upload paths flush; shared snapshots/counters and indirect cache ranges. Preserve GPU-driven indirect calls. | `ObjectDataJobsTests.cpp`, `RenderPipelineTests.cpp`; **U4** |
| Skeletal pose preparation and skinning | `Apps/App.Base/src/RenderModuleSkinning.cpp`, `DX/GDX12Skinning.cpp` | poses/current+previous transforms → skinned vertices/bounds/history | CPU preparation plus dispatch before legacy graph Execute; history and palette/frame slots need explicit lifetime | `SkinPlacementChecks.h`; **U4**, same-GPU scheduling **U8** |
| Depth prepass + GBuffer, split mesh/grass geometry views | `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12OpaquePass.h`, `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12GeometryViewPass.h`; `Bistro/BistroRenderPipeline.cpp` | indirect commands/materials/geometry → depth, normal/material/albedo/motion attachments | BeginGeometry/EndGeometry surround view writes; shared view preparation, output texture states and previous transforms | `BaseRenderPipelineTests.cpp`, `RenderPipelineTests.cpp`; **U4** |
| Cascaded directional and local point/spot shadows; update/cache variants | `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12CascadedShadowPass.h`, `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12LocalShadowPass.h`; `Apps/App.Base/Include/App.Base/Systems/ShadowVisibilitySystem.h` | lights/camera/casters → CSM and atlas, shadow metadata | `CommonRenderPasses::PrepareFrame` schedules scene visibility then Complete; atlas/cache revisions and prepared view arrays mutate | `ShadowCascadeTests.cpp`, `LocalLightingTests.cpp`; **U4** |
| Grass wind + interaction field + per-view expansion; grass in depth, GBuffer, shadows, probes | `Bistro/Grass/BistroGrassGeometry.cpp`, `GDX12GrassWind.cpp`, `GDX12GrassRenderer.cpp`; `Bistro/BistroRenderPipeline.cpp` | patches/wind/colliders → interaction field, expanded geometry, indirect draws | Reuses expansion buffers by view/patch, mutable preparations; async switch exists; interaction also feeds particles | `BistroGrassChecks.h`, `GrassInteractionChecks.h`, `PreparedGrassViewChecks.h`, `GrassPlacementChecks.h`; **U4**, scheduling **U8**, exported particle interaction **U11** |
| Environment IBL, skybox/atmosphere/off, procedural cloud source | `DX/GDX12EnvironmentIBLPass.cpp`, `GDX12Sky.cpp`, `GDX12CloudComponent.cpp`; `Bistro/BistroRenderPipeline.cpp` | world-selected sky/cloud component → radiance/irradiance/prefilter/BRDF | Asset/reallocation and initialization paths flush; initial IBL has internal ExecuteCommandList+Flush; persistent environment/LUT/cache revisions | `RenderPipelineTests.cpp`, `BistroReflectionChecks.h`; **U5** |
| Reflection probes (static/dynamic capture, readiness/history) | `DX/GDX12ReflectionProbePass.cpp`, `GDX12ReflectionProbes.cpp`; `Bistro/Grass/BistroGrassGeometry.cpp` | selected faces + scene + local shadows + IBL → probe atlas/mips and readiness | Resource resize/rebuild/reset flushes; mutable face budgets, cache revision, capture targets; grass shares expansion scratch | `BistroReflectionChecks.h`; **U5** |
| AO early after depth / late after GBuffer; SSAO, HBAO, XeGTAO, CACAO; full/half resolution | `DX/GDX12AmbientOcclusionPass.cpp`, `GDX12AmbientOcclusion.cpp`; `Apps/App.Base/src/Rendering/CommonRenderPasses.cpp` | depth + normals/history → AO | SDK scratch/context and histories; execution point changes graph; async flag selects compute in legacy executor | `AOTests.cpp`; **U5**, async **U8** |
| Deferred lighting / clustered CPU light grid | `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12DeferredLightingPass.h`, `GDX12LightGrid.cpp` | GBuffer, AO, shadows, sky/IBL/probes → lit HDR | CPU grid preparation in PrepareFrame; mutable per-frame light/grid uploads and constants | `ClusteredLightingChecks.h`, `LocalLightingTests.cpp`, `PBRMathTests.cpp`; **U5** |
| SSSR + reflections denoiser, disabled passthrough | `DX/GDX12SSSRPass.cpp`, `GDX12SSRSettings.cpp` | HDR, depth/normals/roughness, motion, environment → reflected HDR | SDK resources, counters and temporal history; transitions remain legacy tracked | `SSRTests.cpp`, `SSRBistroTests.cpp`; **U5** |
| Water solver/SPH sort, surface/thickness/normals/depth, far/near transparency partitions, composite/resolve | `Bistro/Water/GDX12Fluid.cpp`, `GDX12FluidPass.cpp`, `BistroWaterRendering.cpp`, `GDX12FluidSolver.h` | fixed-step fluid inputs/particles + opaque HDR/depth → solver state and resolved water HDR | Mutable accumulator/ping-pong buffers/partitions; reallocations flush; offload Start is currently owned by fluid pass and is forbidden in experiment until U10 | `FluidSortGpuTests.cpp`, `FluidSimulationChecks.h`, `BistroWaterChecks.h`, `WaterCompositionChecks.h`; **U6**, same-GPU async **U8**, MGPU **U10** |
| Particle simulation: AnalyticDrag / SemiImplicitEuler, pause/reset/burst, depth collision, interaction; primary/shared eligibility | `DX/GDX12ParticleSimulation.cpp`, `GDX12Particles.cpp`; `Apps/App.Base/src/RenderModule.cpp` CollectParticleEmitters | immutable emitter snapshot + depth/interaction → alive pool/indirect args/history | BeginFrame allocates pools and may create offload; InitializePools records before graph; IDs, burst/reset revisions, slots mutate | `ParticleTimeTests.cpp`, `ParticleSimulationControlsChecks.h`, `ParticlePlacementChecks.h`; **U6**, async **U8**, MGPU **U11** |
| Particle drawing: WeightedAlpha, Additive, global SortedAlpha; billboard, firefly, water-drop, atlas/material | `DX/GDX12ParticlesPass.cpp` | alive buffers/depth/materials → WBOIT or HDR + motion | Sorting covers all relevant emitters; mutable sort scratch, draw buffers/history; do not sort independently per worker | `ParticlePlacementChecks.h`, `BistroParticleControlsChecks.h`; **U6** |
| Mesh transparency + WBOIT resolve | `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12WBOITTransparencyPass.h`, `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12WBOITCompositionPass.h`; `Bistro/Water/BistroWaterRendering.cpp` | opaque depth, materials, lights/shadows → accumulation/revealage → composition HDR | All/far/near depth partition differs between Default/Bistro; shared attachments must remain ordered with water and particles | `WaterCompositionChecks.h`, `RenderPipelineTests.cpp`; **U6** |
| DoF; motion blur and output motion handling | `DX/GDX12DepthOfFieldPass.cpp`, `GDX12DepthOfField.cpp`, `GDX12MotionBlurPass.cpp`, `GDX12MotionBlurSettings.cpp` | composed HDR/depth/motion → filtered HDR/motion | SDK context and scratch, temporal/camera history, disable/quality branches | `DepthOfFieldTests.cpp`, `BistroDofSmoke.cpp`, `MotionBlurTests.cpp`; **U7** |
| Reconstruction Native / XeSS / FSR / DLSS / NIS; quality/fallback/restart | `DX/GDX12Reconstruction.cpp`, `GDX12ReconstructionPass.cpp` | render HDR/depth/motion → output grid | ApplySettings may call context.Resize before frame slot update; SDK contexts, history and render dimensions; capability-specific branches retained | `ReconstructionTests.cpp`; **U7** |
| Output/present; frame generation Off / AMD / NVIDIA / Intel | `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12OutputToScreenPass.h`, `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12FrameGenerationPass.h`, `GDX12FrameGeneration.cpp`, `GDX12SwapChain.cpp`, `GDX12StreamlineSDK.cpp`; `Apps/App.Base/src/RenderModule.cpp` | final HDR/depth/motion → swapchain + SDK callbacks/present | BeginFrame/BeginRendering/EndRendering bracket work; SDK queue callbacks/submissions and swapchain replacement require explicit ownership. UI occurs after graph, before final submit. | `FrameGenerationTests.cpp`; **U7**, queue audit **U8** |
| World/offscreen UI, atlas/screen composition, Bistro posters | `Engine/Engine.UI/src/WorldCanvas.cpp`, `Engine/Engine.UI/Include/Engine.UI/OffscreenUI.h`, `UIScreenComposite.cpp`; `Bistro/UI/BistroUIPosters.cpp`, `Apps/App/src/BistroUICanvases.h` | UI tree/material pages → UI textures, world surfaces/posters, screen output | Mutable atlas/descriptors/frame buffers, poster material reads declared in graph; UI async flag and quality; Engine::UI final overlay outside graph | `RenderPipelineTests.cpp`, `BistroAdditionsTests.cpp`; **U7**, async **U8** |
| Debug collision view alternate pipeline | `Bistro/BistroRenderPipeline.cpp` BistroCollisionPass | Jolt triangles/camera → swapchain | Debug-only short graph replaces normal techniques; per-frame upload rings; keep explicit alternate selection | `CollisionGeometryTests.cpp`; **U7** |
| Existing MGPU water transport/worker | `Bistro/Water/GDX12WaterOffload.cpp`, `GDX12FluidPass.cpp` | secondary solver packed positions → shared copy/import/display | Own worker, DIRECT/COMPUTE/COPY submissions, waits, ready/consumed slots, readback waits; cannot run beside new executor | `WaterOffloadGpuTests.cpp`, `WaterOffloadBistroTests.cpp`; **U9** transport fixture, **U10** integration |
| Existing MGPU particle transport/worker (dd012c0f) | `DX/GDX12ParticleOffload.cpp`, `GDX12ParticleSimulation.cpp` | eligible depth-independent emitters + interaction field → shared packed payload/metadata → primary display | Own worker, submits/CPU waits at allocation and per secondary iteration, ready/consumed state. Adapter -1 may inherit water selection; both must be disabled. Common payload, no per-emitter transport copies. | `ParticleOffloadGpuTests.cpp`; **U9** transport fixture, **U11** integration |

U2's single-DIRECT executor and U3's recording jobs are accepted prerequisites. Current U4 migration evidence is listed above; U5–U12 ownership in the baseline table remains future work. U12 must revisit every row against actual runtime evidence. Source-specific tests are not automatically acceptance of a new executor path.
