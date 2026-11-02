# CPU frustum grid versus depth-adaptive GPU clustering

Date: 2026-11-02. GPU: NVIDIA GeForce RTX 3060 Laptop GPU. Release x64 `/O2`,
`_CL_=/Z7 /GL-` for the local MSVC PDB-server issue. Grid startup settings: 64px,
32 slices, FarDepthCm=100000. One-thread and four-thread JobSystem instances.

The reproducible baseline is `Apps/App/tests/shaders/DepthAdaptiveLighting.hlsl`,
copied from the implementation present before this rewrite. Both implementations
use identical light records, G-buffers, PBR/shadow functions and shader optimization.
The comparison is against the depth-adaptive clustered shader, not the older tiled
shader from the first experiment.

Run `Build/bin/x64/Release/LightingGpuTests/LightingGpuTests.exe --benchmark`
from the repository root. This disables the D3D12 debug layer for measurements.
The default invocation enables the layer for correctness checks. In both modes,
325 HDR fixtures are compared against brute force; CPU checks include coverage,
INI startup locking and identical masks with one/four threads.

Each number below is the median of five warmed batches of 16 dispatches. CPU
includes building the grid, waiting for its jobs, writing staging data and recording
commands. GPU includes the new copy/barriers and shading, or the baseline's GPU
list construction and shading. Frame-slot buffers are warmed before measurement.
CPU/GPU values are separate measurements; adding them does not measure application
frame time because real CPU/GPU work can overlap.

| Size | Sources | Fixture | Previous CPU ms | Previous GPU ms | New CPU, 1 thread ms | New CPU, 4 threads ms | New GPU, 4 threads ms |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 320x180 | 135 | Dense | 0.00160 | 0.31552 | 0.17327 | 0.10060 | 0.36256 |
| 320x180 | 512 | Dense | 0.00170 | 0.97402 | 0.57166 | 0.27085 | 1.10272 |
| 320x180 | 135 | Layered | 0.00125 | 0.05568 | 0.04458 | 0.03868 | 0.05779 |
| 320x180 | 512 | Layered | 0.00157 | 0.11878 | 0.13323 | 0.08244 | 0.13888 |
| 1280x720 | 135 | Dense | 0.00172 | 4.39315 | 0.56698 | 0.28688 | 4.46739 |
| 1280x720 | 512 | Dense | 0.00168 | 16.2106 | 2.18287 | 0.89230 | 16.3870 |
| 1280x720 | 135 | Layered | 0.00167 | 0.64832 | 0.10012 | 0.06608 | 0.64774 |
| 1280x720 | 512 | Layered | 0.00173 | 1.74688 | 0.34532 | 0.15914 | 1.76102 |

The rewrite does **not** show an overall speedup in these fixtures. GPU time is
similar at 720p and worse at the smaller resolution, while CPU work increases.
Four threads reduce the new CPU cost by about 1.5–2.5x in the 720p cases. The
architectural gains are independence from visible depth, reusable spatial light
assignment, no group synchronization during deferred shading, and removal of the
512-light tile overflow path. They do not prove a frame-rate improvement.

Dense fixtures deliberately overlap every source over the surface; layered
fixtures alternate many depths within small screen regions. Neither is a Bistro
FPS measurement. Laptop clocks/load are not fixed. Smaller XY cells can tighten
selection while increasing CPU/upload costs; the ReadOnly CVars allow that tradeoff
without recompiling. No CPU-cache or unchanged-frame shortcut was used.

Verification completed:

- LightingGpuTests: all 325 fixtures pass in Debug and Release with the debug layer.
- A separate Release run with INI tile size 40, 24 slices and FarDepthCm=50000
  also passes all 325 fixtures; the executable configuration was restored afterward.
- ReflectionProbeTests `--debug` passes, including opaque/transparent PBR,
  both prepass modes, publication, resize, XeSS and runtime switches.
- BistroRenderGraphTests passes pooled/dedicated HDR identity, both AO placements,
  odd resize and Native/XeSS switching with particles disabled in its temporary
  executable configuration, as in the earlier lighting validation. It was restored.
- App builds in Debug. The running normal App.exe prevented Release relinking;
  the same source/project was compiled and linked to an isolated Release directory
  (`Build/FrustumLightGrid/AppRelease`) with a temporary import wrapper, then the
  wrapper was removed. The running application was not restarted.
- DXC SM6.0 wave/non-wave variants compile with `-WX`; FXC SM5.1 compiles with
  existing X4000 warnings in shared helpers. Non-WaveOps hardware was not tested.

Local raw logs: `Build/FrustumLightGrid/benchmark-release.log`,
`gpu-final-release.log`, `gpu-final-debug.log`, `gpu-custom-grid.log`,
`reflection-gpu.log`, and `bistro-gpu.log`. Build artifacts are not committed.
