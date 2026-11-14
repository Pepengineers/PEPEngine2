# Postprocessing, UI and presentation — source implementation

Date: 2026-11-14. Local checkout: `D:\Project\PEPEngine2`.
Branch: `codex/meta-render-graph-u7`.
Base: `214c4ff19dd5047a941d9e31dde19ab4b7d0ca25`, the local ordinary merge of
`dd012c0f1aee54913dd65d811c45e73680eee0dc` and corrected water/particle work
`bf4096e02ea6b2c5970595d411bdafd4fbad1bd1`. The resulting commit is the commit
containing this report; resolve it with `git log --format=%H --follow --` on this
file. The base is intentionally newer than remote `codex/bistro-rendering`.

**Source connected, unbuilt and unrun. No runtime acceptance.** The user's direct
prohibition covers builds, tests, shader compilation, applications, validation and
GPU workloads through implementation/review of the water MGPU stage. It overrides
the plan's intermediate run gates. This handoff stops after the post/UI work;
async queues and later stages are not implemented or authorized by this report.

## Connected path

`ScenePostUI` extends the existing native plan, without legacy graph Execute:

| Operation / boundary | Declared resources and behavior |
|---|---|
| DoF | Final water/near-transparency/SortedAlpha HDR and water-combined depth; original FidelityFX DoF and copy-through disabled/small-grid behavior. Persistent SDK context is private to this adapter. |
| Motion blur | DoF HDR, combined depth/motion, particle feature motion and current camera constants; original complete-motion merge, velocity, tile/neighbor maxima and gather/copy kernels. All five output/scratch textures are declared. |
| Reconstruction | Combined depth, complete motion and blurred HDR; existing Native/XeSS/FSR/DLSS/NIS bindings and unsupported-provider fallback/status. Native links HDR directly; duplicate physical inputs are deduplicated. |
| Output | Original HDR tone-map/display encoding after reconstruction, at the current output-grid swapchain extent. No new HDR10 transform or color algorithm. |
| World/offscreen UI and posters | Existing Bistro canvas frame/effect generator and offscreen raster, always DIRECT in this executable. Per-frame effect/target resources are explicit; generated material targets are read by the scene packets. |
| Screen Engine.UI | Capture the owner-thread screen draw data, explicitly record font/texture uploads and raster into a per-slot linear premultiplied RGBA8 image, then existing ScreenComposite over the display target. Retain draw/raster/composite owners until that frame slot retires. |
| Debug collision view | Shared original Jolt renderer; collect triangles and fill its upload on the owner before recording; upload + display accesses are explicit. `Meta.Debug.Collisions` selects the collision-only display and suspends FG. Returning to scene requests history reset. |
| FG / SDK tagging | Existing primary-DIRECT `GDX12FrameGeneration::Prepare`, with combined depth and complete motion, ordered after output. Off/unsupported/runtime-failure behavior remains in the existing FG implementation. |
| Frame end / Present | Explicit display access in PRESENT and GPU timer finish in a plan packet. Executor submits/signals and returns the actual retirement fence; RenderModule then calls existing EndRendering and Present on the owner. Swapchain BeforePresent/AfterPresent and AMD SDK callback remain SDK-owned presentation boundaries on their existing queue. |

The post adapter is installed before initial scene target creation. Reconstruction
Setup/ApplySettings/QueryRenderTargetResolution run before geometry resize and
Freeze. Main scene attachments use DownscaledWidth/Height; screen UI/output use
WindowWidth/Height. Existing camera jitter and flags, history revision,
camera-cut/reset and provider restart semantics are retained. Provider input links
are rebound to the actual current frame slot, including cached FSR/DLSS/NIS input
members, before initialization/resize/recording.

Posters prepare before material uploads. Their owner-only recording operations are
appended with stable callback IDs, then moved to the beginning of the compiled
operation order. Material reads are conservatively added to scene consumers,
including depth/shadow/probe/transparent paths, so uploads/raster complete before
sampling. This may overorder non-material work; it never invokes COMPUTE/COPY or
an offload worker. Production placement defaults remain unchanged.

The old diagnostic albedo-to-backbuffer copies are omitted when a full scene
continuation supplies output. Besides avoiding overwriting output, this prevents
an invalid full-resource copy between different render/output extents. Fixture
callers without a continuation retain the old diagnostic path.

## State, lifetime and recording ownership

DoF context allocation/retirement and motion tile allocation are separated from
command recording; legacy Execute still calls preparation then recording.
Post calls those owner preparation methods before compiling/submitting its plan.
Motion scratch, camera input and all externally visible SDK inputs/outputs have
whole-resource declarations. Each command callback borrows the executor's DIRECT
list, uses strict recording-local states and restores its declared boundary.
It rejects private submission signals. No new callback submits, signals or waits
an engine queue. Internal SDK scratch is an exclusive opaque SDK section;
CanvasEffects/OffscreenRenderer private scratch and upload owners are retained by
their existing recording handles. Their internal raw barriers stay within that
owner-only section, rather than pretending to be independently schedulable jobs.

FG depth and complete motion end in NON_PIXEL_SHADER_RESOURCE, matching the
existing NVIDIA eValidUntilPresent tagging promise (also valid for the existing
AMD/Intel prepare/tag calls). No terminal COMMON transition invalidates those
tags before Present. Successful submission publishes retained physical-resource
handles per frame slot; subsequent plans import that state even when the motion
texture is declared only inside the post adapter. New allocations start COMMON.
Pending exports are separate from submitted exports. Retained COM handles prevent
pointer-reuse aliasing; the bounded slot array avoids an ever-growing state map.

Resize drains the primary queue/executor before replacing scene/UI/post resources.
The adapter clears old exports on resize, rebinds scene links and resizes post
outputs/providers before their next record. Existing swapchain fencing, SDK
presentation lifetime and device quarantine remain authoritative. There is no
new recovery from partially submitted/aborted executor work.

Post/UI/SDK recording is **owner-only CPU recording**, as allowed for techniques
whose mutable internals have not been split. Water/particles retain their existing
explicit owner-only section; neither is reported as parallel. Native geometry,
grass and lighting groups retain their existing worker recording. All engine
packets still submit to one primary DIRECT queue. No CPU/GPU overlap or speedup
has been measured.

The inherited diagnostic serial/parallel mode still compares geometry/lighting
readbacks only. Stateful post/SDK and captured screen UI execute once, on the
parallel displayed frame, and FG is suspended in that diagnostic mode. It is not
a numerical or temporal oracle for post, water or particles. Persistent post
scratch and per-slot screen images/recording handles add memory; no memory figure
or performance result is asserted.

## Static review and verification limits

Read: meta plan and post/UI plan; predecessor reports/inventory and their source
contracts; RenderModule callers, frame flags/material/camera/skinning lifecycle;
geometry plan reorder/compilation and stable IDs; lighting continuation; water
outputs; original DoF/motion/reconstruction/provider/output/FG implementations;
Bistro UI/poster/collision path; Engine.UI capture/offscreen/canvas/composite
recorders; existing DepthOfFieldTests, MotionBlurTests, ReconstructionTests and
FrameGenerationTests as source patterns only. NIS uses its declared input/output
wrapper barriers; no additional wrapper scratch resource was found in that pass.

Own static review covered per-slot provider links, first frame/resize imports,
retained FG states, cold allocations, callback captures, shader/provider fallback,
poster writer-before-reader ordering, removed diagnostic copies, UI ownership and
production defaults. `git diff --check --ignore-submodules=all` passed. This is a
text/diff hygiene check, not a test. Full status cannot traverse existing inih
submodule metadata in the sandbox; scoped status ignores submodules. No submodule
was edited. The foreign `Apps/App/tests/BistroAdditionsTests.cpp` remains excluded,
with SHA256 `BAF2D4E7B6ED94E634DA4821EFACD62743AF55FBEE86A81504C9AC6665F84EE4`.

**NOT RUN:** Debug/Release builds; C++ or HLSL/shader compilation; CPU/planner/unit,
integration/regression/GPU tests; smoke/validation/evidence-export scripts;
App or App.MetaRenderGraph; debug layer/GBV; readback/image/visual validation;
benchmarks, performance/timing captures or profiling. No old binary or old capture
was used as evidence for new code. No new tests were executed; source invariants
reject mismatched grids, missing owner preparation and private submissions.

After the separately authorized testing phase, acceptance still needs native and
available provider image/history checks; first frame and multiple queued slots;
DoF/motion toggles, jitter/reset/camera cut/pause/world reload; render/output resize
and provider changes; posters/screens/fonts/world capture; collision mode return;
FG supported/unsupported/failure paths and actual Present lifetime; legacy App
regressions; complete serial/parallel scene comparison, CPU trace, DIRECT-only GPU
timeline and measured memory/time. Unavailable providers must be identified then,
not guessed from installed DLLs. Independent originating-chat review is pending.
No async stage, MGPU transport or merge into `codex/bistro-rendering` is performed.
