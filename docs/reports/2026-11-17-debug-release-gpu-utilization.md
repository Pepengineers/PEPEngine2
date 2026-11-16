# GPU utilization: current Release and Debug build differences

Source HEAD `66b9a7d6`, existing Release executable dated 2026-11-17 01:01 MSK.
No production code/config edits, rebuilds or quality changes during diagnosis.
Existing PaintingSurfaces and BistroAdditionsTests work is unrelated and untouched.
Evidence lives in `Build/gpu-idle-audit-2026-11-17/`.

## Live observations

Initially no renderer process was running. The old log's flower-pot failure
predates the current successful physics build and was not reproduced.

The current Release was launched first through PIX, then ordinarily without the
GPU capture virtualizer. NVIDIA utilization snapshots in the warmed-up runs were
97-98%, with ordinary window-title rates around 23-24 FPS. The user also confirmed
100% GPU utilization in the ordinary Release run. This does not establish the
exact cause of the earlier Debug screenshot by itself.

`steady.wpix` is a six-second PIX-instrumented timing capture. The union of all
recorded DIRECT/COMPUTE/COPY GPU-work parallel levels over capture time 2000-8000 ms
was 3230.885 ms, 53.85% of that interval; largest work gaps were about 34 ms.
This number is specific to GPU-capture instrumentation, not the ordinary Release
or Debug's utilization. NVIDIA snapshots outside the capture used different
sampling windows/metrics. A single GPU work lane alone reported only 24.87%; that
incomplete figure was rejected after collecting every parallel level.

PIX's timing query categories did not become callable through the client after
activation. The same official installed `PixTimingCapturesMcp.exe` was queried
over local stdio instead; its full schemas, raw work intervals and CPU stacks
were saved. GPU PIX event scopes were absent, but GPU API work timestamps and CPU
sampling were available. PDB symbolization used the matching executable/base.

`ordinary-cpu.wpix` is a separate six-second CPU sampling capture of the ordinary
Release process. `ordinary-module.json` confirms no WinPixGpuCapturer module in
that process. In an exploratory subset of 450 decoded distinct main-thread stacks:

| Inclusive application path | PIX-instrumented | Ordinary Release |
| --- | ---: | ---: |
| QueueSubmission::Submit | 208 | 141 |
| UpdatePhysics | 97 | 97 |
| SceneLocalShadows | 12 | 30 |

These are selected decoded stack counts, not unbiased CPU-time percentages and
not frame timings. Physics/child scheduling and command submission merit attention
in ordinary Release too, but instrumentation materially increases the submission
cost. No controlled ordinary A/B performance improvement was measured.

## Confirmed build difference

Read actual `CL.command.1.tlog` commands, not assumed Visual Studio defaults:

| Projects: App.MetaRenderGraph, App.Base, Engine.RendererDX12 | Flags |
| --- | --- |
| Debug | `/ZI /JMC /Od /D _DEBUG /RTC1 /MDd` |
| Release | `/Zi /O2 /GL /D NDEBUG /MD` |

All three Debug modules have disabled C++ optimization, runtime checks, Debug CRT
and Just My Code instrumentation. C++ container/iterator checks also follow the
Debug runtime defaults; they were not individually cost-isolated here.

Shader compilation is shared: `GDX12ShaderCompiler.cpp:93` always requests `-Zi`
and `-O3`, independent of C++ configuration. Normal Debug does not automatically
enable the D3D12 debug layer: `RenderModule.cpp:103` requires `-d3ddebug`, while
`MetaApplication.cpp:461` enables it for the explicit smoke harness. No production
call enabling GPU-based validation was found in this ordinary path. Do not blame
default GPU validation without checking the actual launch/external DX settings.

Thus Debug can move the frame's limiting work to CPU preparation, physics, command
recording and submission, leaving optimized GPU shaders waiting for the next work.
The exact Debug bottleneck split remains unprofiled; Debug was not launched or
rebuilt during this investigation, so the user's open Release was not interrupted
for another comparison.

## Proposed corrections

1. Use current Release for performance diagnosis: it already has `/Zi` and PDBs,
   as demonstrated by the resolved CPU stacks. Keep full Debug for source stepping
   and checks. There is no need to add another build configuration just for symbols.
2. Reduce executor overhead independently of build configuration. Today every
   packet reserves/resets/records `Lists+2` command lists, including empty barrier
   prologue/epilogue lists (`MetaGraphExecutor.cpp:155,169`). Omit empty barrier
   lists first; preserve the existing submission boundaries for a small change.
3. Then consider grouping adjacent same-queue packets to reduce
   ExecuteCommandLists/Signal calls (`QueueSubmission.cpp:121`). This is not a safe
   one-line batching change: `MetaGraphCompiler.cpp:110` assumes DEFAULT-buffer
   decay to COMMON at **each packet submission**. State tracking/barriers must be
   compiled for the real submission batches, and cross-queue publication/fence
   boundaries preserved. No CPU Flush or lost dependencies are acceptable.
4. Profile physics job granularity next. App::UpdatePhysics performs multiple
   Schedule/Complete phases per fixed step; WakeThreadsForNewTasks was frequent
   in the decoded stacks. Preserve ticks, elapsed simulated time and debt when
   evaluating changes; reducing physics fidelity is not a performance fix.

No renderer change is committed in this diagnosis. Short timing captures and
ordinary launch succeeded; no tests, long soak or Debug/Release A/B matrix ran.
The ordinary Release window is left open for the user.
