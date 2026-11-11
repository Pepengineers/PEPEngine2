# Live FPS degradation investigation

Observed 2026-11-11, Europe/Moscow. Diagnosis only; engine and runtime settings were not changed.

## Target and method

- Existing PID 22040, `Build/bin/x64/Debug/App/App.exe`, started at 19:19:26; executable last written at 19:19:24. Rider was running. No debugger pause or application restart was performed.
- Checkout HEAD `4182964a`, branch `codex/bistro-rendering`. The executable's exact source provenance was not established. Pre-existing edits in `Apps/App/tests/BistroAdditionsTests.cpp` were preserved.
- Interior Bistro view with two reflective spheres; window approximately 1280 x 800. Camera and quality settings were not changed. The application was brought to the foreground before the measurement.
- 60 samples between 19:28:22 and 19:30:26: process memory, handles, threads, FPS reported in the window title, and NVIDIA device metrics. Sampling interval approximately 2.1 seconds. Raw data: [samples](2026-11-11-live-fps-degradation.samples.jsonl).
- FPS values are sparse samples of the application's displayed statistic, **not** per-frame measurements or aggregate throughput. They do not establish p99 frame times or exact spike frequency.

## Findings

| Metric | Observed |
| --- | --- |
| GPU | RTX 3060 Laptop, 6144 MiB |
| Temperature | 85–88 C, average 86.52 C |
| GPU graphics clock | 900–1972 MHz, average 1352 MHz |
| GPU utilization | 64–100%, average 93.4% |
| SW Thermal Slowdown | Active in 59/60 samples |
| Thermal slowdown counter | Increased from 364,959,312 to 447,062,270 microseconds: 82.10 seconds during the 124-second observation |
| Device VRAM used | Constant 3614 MiB |
| Process private memory | 5764.676–5764.738 MiB; first-to-last increase 12 KiB |
| Process working set | 2134.395–2134.441 MiB |
| Threads | 55 at both endpoints |
| Handles | 955 at both endpoints |
| Displayed FPS samples | 5.97–18.40; arithmetic mean 13.52 |
| Mean displayed FPS, consecutive groups of 20 | 14.70 / 13.05 / 12.80 |

At 19:26:31, `nvidia-smi -q -d PERFORMANCE,TEMPERATURE,CLOCK,POWER` already reported active SW Thermal Slowdown, a target temperature of 87 C, and graphics clock 1327 MHz. SW Power Cap and HW Slowdown were not active at that instant. Approximately 14 GiB physical memory was free.

**Confirmed:** the GPU driver was thermally limiting clocks during this running session, alongside variable frame rate. This is a concrete contributor worth controlling before interpreting long-run rendering performance. Neither a cold baseline nor a controlled cooling comparison was recorded, so the proportion of FPS loss attributable to it is not established.

**Not observed:** increasing process/private memory or device VRAM over this two-minute interval. This does not exclude a slower leak, resource churn with constant occupancy, or GPU residency stalls.

**Unresolved:** which CPU/GPU operation causes individual long frames; whether software workload accumulates independently of temperature. The measurements do not prove that thermal limiting explains every spike.

## PIX recording and limits

A 15-second attached timing capture was saved locally to `Build/Diagnostics/2026-11-11-degradation/live.wpix` (192,782,336 bytes). The recording command took approximately 25 seconds and completed before the 60-sample measurement. The capture opened successfully in the separate PIX timing MCP analyzer.

The standard PIX analysis bridge was disabled. The separate timing analyzer reported the capture loaded and accepted enabling memory/metrics categories, but their query tools and the advertised capture-manifest tool did not appear in the exposed tool catalog. Consequently no CPU stack, GPU residency, or event-duration conclusions were extracted. Attaching after D3D12 device creation also does not provide the same instrumented GPU scopes as launching through PIX. The binary trace is intentionally left outside Git.

## Next discriminating check

Repeat a fixed-camera, fixed-quality workload from startup through the slowdown while recording per-frame timing and clock/thermal data. Compare under cooling conditions that keep SW Thermal Slowdown inactive, without changing rendering settings. If long frames persist, launch through PIX for GPU instrumentation and inspect the slow frames' queue gaps, residency events, and CPU waits. A source change or regression test should target the mechanism that this comparison establishes; none is justified by the current evidence alone.

Validation: live process sampling and a successfully saved/opened PIX trace. No build or code tests were run because implementation was unchanged.
