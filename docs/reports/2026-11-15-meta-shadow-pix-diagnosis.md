# Meta Bistro: PIX diagnosis of shadow cost

## Scope and evidence

Diagnosis only, source HEAD `e9dfd792`. Existing unrelated modification:
`Apps/App/tests/BistroAdditionsTests.cpp`; left untouched.

Executable: `Build/bin/x64/Release/App.MetaRenderGraph/App.MetaRenderGraph.exe`,
ordinary Bistro, default startup camera, no smoke flags or quality overrides.
Existing Release binary was used, without rebuilding. PIX 2610.28, NVIDIA
GeForce RTX 3060 Laptop GPU. Exact binary/source equivalence was not rebuilt
or independently established.

Single-frame GPU capture: `Temp/meta-shadow-one.wpix` (1,413,836,296 bytes).
PIX session `pix-1`. Capture creation succeeded in 32.16 seconds.
The initial attempt mistakenly requested 240 captured frames, not a delay;
that attempt was stopped and produced no usable capture. Its window FPS is
not an ordinary-run measurement.

PIX analysis bridge is disabled by `PIX_MCP_BRIDGE_DISABLED`. Analysis used
pixtool event export and replay counters instead. No CPU timing attribution
is available from this capture.

## Measured work

PIX exported 96,024 DrawIndexedInstanced calls, 452 ExecuteIndirect calls and
448 ClearDepthStencilView calls. The ordinary operation order and the first
six indirect calls identify depth, GBuffer, then four sun cascades; subsequent
indirect calls belong to local maps.

| Work | ExecuteIndirect | Expanded draws | EOP-to-EOP replay sum |
| --- | ---: | ---: | ---: |
| Scene depth + GBuffer | 2 | 4,964 | 3.132416 ms |
| Four sun cascades | 4 | 31,341 | 8.128512 ms |
| Local shadow maps | 446 | 59,719 | 46.898176 ms |
| Local depth clears | — | 446 clears | 3.020800 ms |

All root events with measured EOP-to-EOP values sum to 64.033472 ms;
local draws plus clears account for about 78% of that accounting total.
Deferred-lighting Dispatch global ID 97070 costs 1.398784 ms.
These are single-frame replay counter sums, not an ordinary FPS measurement
or a precise cross-queue frame critical path. Expanded child draws are counted
but their times are not added to parent ExecuteIndirect times.
TOP-to-EOP values overlap and were not used as additive pass totals.

Counter exports are retained in:

```text
C:/Users/Asus/AppData/Local/Temp/pix-mcp-cache/53bbe33b91c086c9/
  events__default.csv
  events__EOP-to-EOP-Duration-ns-f5bc286d.csv
  events__TOP-to-EOP-Duration-ns-109ad27f.csv
```

## Causal chain in source

1. Ordinary opaque rendering creates both sun and local shadow techniques:
   `Apps/App.MetaRenderGraph/src/Techniques/SceneLighting.cpp:27-30`.
2. Every frame schedules local culling and packing, prepares sun shadows, then
   waits for local completion before proceeding:
   `SceneLighting.cpp:91-99`, `SceneLocalShadows.cpp:169-190`.
   Jobs parallelize CPU preparation, not GPU map drawing.
3. Light selection rejects a light only when its range AABB misses the camera
   frustum (`SceneLocalShadows.cpp:135-136`). Gather additionally checks caster
   range and light-face frusta (`:105-125`), but does not test whether a face
   contributes to camera-visible receivers or is occluded by scene geometry.
4. A retained point light allocates six views; a spot allocates one
   (`Engine/Engine.RendererDX12/Include/Engine.RendererDX12/GDX12LocalLights.h:110`).
   Packing retains all faces once any face has casters
   (`SceneLocalShadows.cpp:154-164`).
5. Packed commands are uploaded every frame (`SceneLocalShadows.cpp:258-268`).
   The draw loop clears each selected slice and executes its caster list
   (`:305-313`). Atlas allocation is reused (`:212-248`), but map contents and
   caster lists are not temporally reused when lights and scene are static.
6. The captured frame consequently renders 59,719 local-shadow draws, versus
   4,964 for the two camera geometry passes combined. Local drawing and clears
   dominate measured GPU replay cost. This is a work-volume problem, not
   evidence that CPU JobSystem scheduling itself caused the regression.

PCF adds nine comparison samples for each contributing shadowed local light
(`Engine/Engine.RendererDX12/Shaders/LocalShadows.hlsli:24-28`), but measured
deferred lighting is much cheaper than drawing local maps in this frame.

## Recommendations and limits

The first implementation target should be reducing repeated local-map drawing,
not moving more preparation to jobs. Static-map reuse with correct invalidation
for lights, caster transforms/material alpha and scene changes addresses the
observed cost directly. This changes the current deliberately uncached policy
and needs a separate implementation request. Camera-relevant receiver/face
selection can reduce retained work while preserving off-camera casters that
cast shadows into the visible scene. Simply discarding off-camera casters is
incorrect. Reducing resolution alone does not remove the repeated geometry work.

A cheap diagnostic would expose selected lights, faces, packed draw count and
command-upload bytes. Existing correctness tests cannot enforce a GPU time
budget; no new tests were added. Validate a future change with a short ordinary
run and a comparable PIX frame, checking map updates after moving a light or
caster only when that additional validation is explicitly authorized.

No runtime fix, rebuild, A/B shadow-disable run, CPU timing capture, historical
binary comparison or sustained benchmark was performed. The captured GPU cost
is established; the exact user's FPS delta and any additional CPU bottleneck
remain unmeasured. The large capture stays local and is not committed.
