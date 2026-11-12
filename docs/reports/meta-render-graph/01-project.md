# U1 — isolated experimental application

Date: 2026-11-13, host `MidSpec` (task referred to this machine as MisSpec).
Checkout: `C:\Users\MidSpec\Documents\PEPEngine2`.

## Source and scope

Original checkout HEAD: `d04ea7d037609751d8ca9c764a89e047370b8d7a`, branch `codex/bistro-rendering`. Initial tracked/index state was clean; only untracked `Apps/App/App.vcxproj.user` existed. Ordinary sandbox status could not access an inih submodule; unrestricted status confirmed the same state. `git fetch origin` updated `origin/codex/bistro-rendering` to `dd012c0f1aee54913dd65d811c45e73680eee0dc`, with no newer remote commits. The complete 13-file plan package is present, including plan commit `9b64feb48ef0b435c7437dc2d55ac6b32f87c3ac` and current particle implementation in dd012c0f.

Implementation base: `dd012c0f1aee54913dd65d811c45e73680eee0dc`. Work branch: `codex/meta-render-graph-u1`. The original branch retains its three diagnostic commits (`3681fcb6`, `3659d521`, `d04ea7d0`). No reset, clean, forced checkout, push, PR, merge, driver change or global setting change was performed. `BistroAdditionsTests.cpp` was not edited. The user file remains untracked and excluded.

Result HEAD is the local U1 commit containing this report; resolve with `git log -1 --format=%H -- docs/reports/meta-render-graph/01-project.md`. The exact result hash and both binary patches are also recorded in the untracked local handoff manifest at `Build/Verification/meta-render-graph-u1/handoff.json` after commit. The patch from original checkout HEAD intentionally includes the upstream plan/particle changes as well; the base-to-result patch isolates U1.

U1 is implemented and its hosting/isolation smoke passed. This executable is explicitly **reference only** and still calls `GDX12RenderGraph::Execute`. No executor, recording-job API or U2 work has been implemented. No independent review was run in this chat.

## Files and ownership

- `Apps/App.MetaRenderGraph/App.MetaRenderGraph.vcxproj`: independent GUID, Debug/Release x64, library project references to App.Base/Core/RendererDX12/UI/External.Libraries. No App/Bistro source or App entry point is imported.
- `src/App.cpp`: its own Windows entry point; `--smoke` selects bounded runtime verification.
- `src/MetaApplication.h`, `src/MetaApplication.cpp`: shared experimental host, startup isolation and tiny procedural reference scene; existing DefaultRenderPipeline remains the composition.
- `Configs/Startup.ini`, `Configs/SmokeWorld.yaml`: isolated deployment settings and camera/world.
- `ApplicationSmokeTests.vcxproj`, `tests/ApplicationSmokeTests.cpp`: minimal Console test target using exactly the same host, with a separate entry point and output directory. It imports the experimental project and removes its Windows entry source.
- `tests/Invoke-Smoke.ps1`: bounded runtime runner with hostile generated INI, initialized-device count, offload/error checks, production INI hashes and local evidence export.
- `PEPEngine2.sln`: registers the experimental project under Apps, with Debug/Release x64 mappings. The smoke target can be built directly.
- This report and `technique-inventory.md`: exact current Default/Bistro technique families, variants, owners, inputs/outputs, hidden submits/waits, state risks and U4–U12 ownership.

There are no shared Engine/App.Base implementation changes in U1. Source sharing consists of library references, without copying the Bistro application or creating duplicate technique implementations. Project settings follow current App build/link/runtime deployment conventions and keep v145/installed Windows SDK.

## Startup isolation and paths

GlobalSettings.props already assigns `Build/bin/x64/<Configuration>/<ProjectName>/` and `Build/obj/x64/<Configuration>/<ProjectName>/`. Therefore App, App.MetaRenderGraph and ApplicationSmokeTests have distinct executable/PDB/object/config/log locations. Shared library artifacts keep their existing owning project directories.

Before constructing BenchmarkEngine/App (the constructor loads and locks startup CVars), PrepareStartup replaces **only its own executable-directory** `Config/Engine.ini` with deployed `Configs/Startup.ini`, initializes ConsoleModule and checks invariants. It does not load repository or App output INI. The generated experimental INI is intentionally disposable: settings UI edits do not survive restart. Missing startup file or invalid safety settings fail startup.

Primary adapter is fixed to hardware-list index 0; particles secondary adapter is -1, AO/particles async are false; native reconstruction and frame generation Off are explicit. Bistro water code is not linked, and runtime asserts its secondary-adapter CVar is absent. An eligible SharedSimulation particle emitter still runs on the primary GPU. Until water is introduced later, absence of that runtime is the water-offload exclusion. Future stages must add an explicit disabled water adapter policy when linking water, rather than assuming particle adapter -1 also disables inherited water selection.

Logs are `<TargetDir>/Logs/Engine.log`; captured reference graph is `<TargetDir>/Logs/reference.dot`. Deployment includes runtime DLLs and SDK notices through the project targets. Working directory in Visual Studio is the repository root; startup/world/config paths are executable-relative.

## Exact U2 extension boundary (proposed, not implemented)

Current lifecycle in `Apps/App.Base/src/RenderModule.cpp`:

1. Initialize creates one primary device/resources/timer, swapchain, calls pipeline Initialize, refreshes flags, then RebuildRenderGraph. UI initializes and scene subscriptions follow.
2. OnUpdate (line 870 at base) applies UpdateFrameSettings before uploads; a changed render grid can invoke Resize here. It selects the frame-constant slot, CPU-waits that slot's DIRECT fence, then updates main constants. This wait must stay before reuse of uploads/descriptors.
3. OnRender (line 904) binds common Jobs, calls frame-generation BeginRendering, prepares material/skeletal/particle snapshots, acquires the primary list and starts GPU timing. DispatchSkeletalMeshes and pipeline PrepareFrame currently record into that list before graph execution. PrepareFrame may request a graph rebuild.
4. The **narrow execution insertion point** is the `_primaryGraph->Execute(primarycmdList, compute)` call at base line 930. Its returned list receives final UI, timer stop, ExecuteCommandList, frame-slot FenceValue/timer notification, SDK EndRendering and Present. Replacing this call alone is insufficient if new execution performs its own submissions or consumes pre-graph commands.
5. RebuildRenderGraph (line 1124) flushes DIRECT, imports Scene and Swapchain, calls the old `IRenderPipeline::BuildGraph(GDX12RenderGraph&, ...)`, compiles and installs the graph, then invalidates history. This construction must branch together with execution.
6. OnResize (line 160) invalidates history, flushes DIRECT, resizes swapchain, updates common dimensions, drops old graph, calls pipeline Resize and rebuilds. GDX12SwapChain::Resize recreates presentation resources; BeginFrame can also recreate presentation for frame-generation settings. All experimental resources referencing old buffers must retire before these callbacks.
7. DrainFrameWork calls WaitForUploads (DIRECT flush) and flushes an existing COMPUTE queue. Destructor drains before graph/pipeline/swapchain destruction; world removal and reset also require lifetime handling. U2 must include its work in drain/resize/reset ownership.

Minimal proposed API direction: add an explicitly selected execution owner to RenderModule's existing opt-in constructor, while default constructor preserves legacy behavior. The owner needs build/rebuild/backbuffer binding, frame execution returning the actual completion point for the frame slot, and drain/resize/reset retirement. Keep pipeline lifecycle/material bindings, camera/constant preparation, UI, timer, SDK Begin/EndRendering and Present contracts visible. Extend the pipeline with a separate explicit plan-building contract; do not disguise a call to BuildGraph/Execute as the new plan. U2's selected mode must reject or deliberately include pre-graph list work (skeletal dispatch, particle initialization, shadow uploads) in its one-DIRECT submission ownership; it must not lose those commands or publish a false fence. It can start with the real buffer fixtures and procedural scene instead of migrating every Default pass ahead of U4–U7.

A switch only inside IRenderPipeline cannot replace the executor, because its BuildGraph signature requires GDX12RenderGraph and RenderModule owns construction, execution, final submit and completion. No speculative interfaces or empty future classes were added here. Resource state compilation/local recording contexts and command allocator retirement belong to U2/U3; shared `_currentState` cannot be updated by worker finish order.

## Baseline manifest

- Scene: tiny procedural reference, **not Bistro validation**. Camera from SmokeWorld.yaml: position (0,0,-400) cm, rotation (0,0,0) degrees, FOV 60, near 1, far 10000 cm. No imported geometry, grass, water, probes, clouds or world UI.
- One point emitter at origin, analytic drag, additive billboard, capacity 256, seed 13, burst 64, no continuous emission, SharedSimulation=true, DepthCollision=false; other author settings are current GDX12ParticleEmitterSettings defaults in dd012c0f.
- Smoke: Timer.Reset then Stop; dt=0, eight explicitly rendered frames (four before/four after actual SetWindowPos). Interactive execution has ordinary wall-clock updates. This tests hosting and resource lifetime, not numeric simulation convergence or pixel equivalence.
- Initial window 1280x800 client; SetWindowPos uses 800x600 outer window, measured final client 784x561 on this machine.
- Hardware used: NVIDIA GeForce RTX 5050, logged 7910 MiB dedicated memory; Windows driver 32.0.16.2114. Other installed adapters were not initialized as render devices in the smoke log. Device factory capability probes are not counted as initialized rendering devices.
- Toolchain: Visual Studio 18 Professional MSBuild 18.9.1, v145 (14.51.36231), SDK 10.0.26100.0 selected by existing configuration. No toolset override.
- Local baseline evidence directories: `Build/Verification/meta-render-graph-u1/Debug-App.MetaRenderGraph/`, `Release-App.MetaRenderGraph/`, `Debug-ApplicationSmokeTests/`. Each has runtime.log, reference.dot, effective.ini, protected-config-hashes.json. These are runtime graph/log captures, **not PIX captures or image readbacks**. No full-scene pixel/performance baseline is claimed; later technique migration needs its own image/readback/capture comparisons.
- Source fixture and startup config are tracked. Local executable/config/graph SHA256 values and result HEAD are in handoff.json. Artifacts remain local under ignored Build; another machine must rerun the commands.

## Verification commands and results

From repository root, use MSBuild at `C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe` (abbreviated MSBuild below):

```powershell
MSBuild Apps/App/App.vcxproj /p:Configuration=Debug /p:Platform=x64 /m:2 /nr:false /v:minimal
MSBuild Apps/App/App.vcxproj /p:Configuration=Release /p:Platform=x64 /m:2 /nr:false /v:quiet
MSBuild Apps/App.MetaRenderGraph/App.MetaRenderGraph.vcxproj /p:Configuration=Debug /p:Platform=x64 /m:2 /nr:false /v:minimal
MSBuild Apps/App.MetaRenderGraph/App.MetaRenderGraph.vcxproj /p:Configuration=Release /p:Platform=x64 /m:2 /nr:false /v:minimal
MSBuild Apps/App.MetaRenderGraph/ApplicationSmokeTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m:2 /nr:false /v:quiet
./Apps/App.MetaRenderGraph/tests/Invoke-Smoke.ps1 -Configuration Debug -Target ApplicationSmokeTests
./Apps/App.MetaRenderGraph/tests/Invoke-Smoke.ps1 -Configuration Debug -Target App.MetaRenderGraph
./Apps/App.MetaRenderGraph/tests/Invoke-Smoke.ps1 -Configuration Release -Target App.MetaRenderGraph
```

All five build combinations succeeded (exit 0). Logs are `app-debug.log`, `app-release.log`, `meta-debug-native.log` + `meta-debug-final.log`, `meta-release.log`, `smoke-debug.log` + `smoke-debug-final.log` in `Build/Verification/meta-render-graph-u1`. Existing C4244 (Bistro), LNK4075 and third-party LNK4099 missing PDB warnings remain. First sandbox Debug build failed C1902 (program database manager); unrestricted build succeeded without changing compiler settings. First smoke found an invalid camera-less YAML; corrected fixture was rerun successfully.

All three final runtime commands succeeded (exit 0), each with a 60-second limit. Each ran the real renderer/present path with D3D12 debug layer, 8 frames, actual resize, completed GPU work, no D3D12 ERROR/CORRUPTION messages, normal WM_CLOSE/WM_QUIT path and application destruction. Each restored the generated INI despite poisoned adapter indices 999, water selection and async=true; logged exactly one initialized GPU adapter; emitted no offload log or runtime error; and preserved hashes of repository Config/Engine.ini and the corresponding App output Config/Engine.ini. The fixture has an explicitly offload-eligible emitter, so the particle exclusion is exercised with a consumer present.

The initial Debug App output INI SHA256 during final testing is `2DDAD8833D4143F4C263BD5D03DC3B759088D82A453087C6BD38F5F8B54C94BF`; repository INI is `8196CA973D3BD39C030B6596B39F5BC62176D5D4F9D877E968E67447EE9B929F`. Hashes per run also remain in the evidence directories. Runtime evidence is not replaced by source inspection.

## Limits and handoff

Only x64 was added. The minimum test target was built/run in Debug; Release runtime coverage is through the actual experimental Windows executable with the same host. Normal App was built in both configurations, but full Bistro runtime/regression testing was not run because U1 changes none of its runtime sources. There is no image readback, PIX timeline, performance claim, MGPU validation, new graph execution or parallel recording evidence. The inventory includes all current composition families and does not mark their future implementations accepted.

Next task is U2, `docs/plans/2026-11-13-meta-render-graph/02-direct-plan.md`; it is not started. Independent review is reserved for the originating chat. Use the base-to-result patch for U1 code review and the original-to-result patch for reproducing the checkout transition; preserve the original diagnostic branch and untracked user file.
