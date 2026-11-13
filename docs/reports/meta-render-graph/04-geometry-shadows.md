# U4 — scene geometry and shadows (in progress)

Host MidSpec, 2026-11-13. Accepted base/initial HEAD: `64f0ab1ad31dfea307495d9cfcf6c87e139bca79` on `codex/meta-render-graph-u3`; U2 `0ba202a335750fc3f7f1b40ede3253ce2894abd0` is an ancestor. Work branch `codex/meta-render-graph-u4`. Initial status contained only untracked `Apps/App/App.vcxproj.user`, preserved and excluded. No push/PR/merge/reviewer agents.

U4 is **not complete**. U4.1 and U4.2 experimental Debug GPU checks passed; U4.3 and U4.4 remain mandatory. Final production/shared-caller and Release validation remain pending.

## U4.1 result

`Techniques/SceneGeometry` freezes private upload snapshots of actual ECS instance, transform, material and indirect-template buffers after frame-slot retirement. Four buckets record disjoint typed element ranges with plan-controlled packet transitions. Camera/main uploads are immutable in the existing frame slot. DEFAULT buffers declare COMMON decay boundaries to the U3 compiler.

Separate packets reset both visible counts, run unchanged `Culling.hlsl`, and consume GPU-generated opaque commands with `ExecuteIndirect`. Unchanged `OpaquePass.hlsl` supplies reversed-Z depth and five GBuffer outputs, including motion. Diagnostic output copies albedo; no lighting/post. Three ECS entities share one registered quad mesh. This is real scene/geometry integration, not Bistro acceptance. No CPU draw-loop replacement for ExecuteIndirect.

Geometry-buffer API adds a read-only VB/IB/native-resource binding snapshot. Upload destinations, descriptors, ranges, views and dimensions exist before recording. Workers call native commands and never allocate descriptors or mutate trackers. Scene CPU preparation/staging copies remain sequential. ECS system order is unchanged.

Completed Debug commands (exit 0):

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe' Apps/App.MetaRenderGraph/App.MetaRenderGraph.vcxproj /p:Configuration=Debug /p:Platform=x64 /m:2 /nr:false /v:quiet /fl /flp:logfile=Build/Verification/meta-render-graph-u4/meta-debug-native.log
& Apps/App.MetaRenderGraph/tests/Invoke-Smoke.ps1 -Configuration Debug -Target App.MetaRenderGraph
```

Eight serial/parallel pairs used identical frozen inputs/camera. Exact snapshot bytes, canonical opaque command prefixes, and six image attachments matched (row padding excluded). Movement changed image bytes. Visible counts were `3,3,2,0,3,2,0,3`; frames 3/6 had zero depth coverage, frame 0 had 153740 covered pixels, frame 5 had 46878. Actual resize after frame 3, three slots, shutdown, one hardware device, zero legacy Execute calls, zero D3D12 ERROR/CORRUPTION. Inherited U3 GPU tests passed in the same smoke. Initial missing indirect-template initialization was fixed. Visual inspection exposed an absent diffuse texture (black albedo); an explicit colored texture was added and awaits the next run.

Evidence currently resides in `Build/bin/x64/Debug/App.MetaRenderGraph/Logs/geometry-*`: serial/parallel scene bins, full image bins, albedo PPMs, metrics and CPU trace. Smoke copies logs/config evidence to `Build/Verification/meta-render-graph-u4/Debug-App.MetaRenderGraph`. Native upload/geometry callbacks overlap across worker tids 1/2/3. This is CPU trace/readback evidence, not PIX or an FPS claim.

Hardware/toolchain: RTX 5050, 7910 MiB, driver 32.0.16.2114, VS18 v145, SDK 10.0.26100.0, Debug x64/debug layer. Existing LNK4075/LNK4099 warnings. Sandbox MSBuild/PDB stalled; native retry passed without compiler-setting changes. Release and production/shared-callers checks remain pending.

## U4.2 result

`OwnsScenePreparation` defaults to false independently of execution ownership; only the experimental pipeline opts in. Existing `DispatchSkeletalMeshes` callers retain their path. `RenderModule::FreezeSkeletalMeshes` and `GDX12Skinning::Freeze` freeze palette staging, immutable source bindings and PSO/root ownership before recording. Two explicit operations upload palettes then deform shared geometry. Shared source bindings are deduplicated in the graph, and geometry read/write dependencies and buffer decay are explicit. Skin output ranges are fixed by existing allocation. Native worker recording uses the unchanged `Skinning.hlsl` algorithm.

A registered scene asset/animation clip drives a four-vertex skinned ECS mesh. Eight Debug serial/parallel pairs passed with visible counts `4,4,3,0,4,3,0,4`. Skinned readback matches the expected pose translation (10 units per frame on x/y) within 0.001, and matches exactly between recording modes. Frame 0 has 206870 depth pixels. Colored albedo was visually inspected at `Build/Verification/meta-render-graph-u4/u4-2-albedo.png`. Resize/three slots/debug layer/zero legacy calls/shutdown passed. This fixture changes animation time explicitly at dt=0 and is deterministic. Default skeletal instancing remains false; the batched variant still needs explicit coverage.

## Remaining

U4.2: explicit skinning/pre-graph operations and animated fixture. U4.3: isolated cascaded/local views. U4.4: grass wind/interaction/per-view geometry. Then shadow/grass image/version checks, final builds, artifacts/patch/SHA256/result HEAD and handoff. U5 is not authorized.
