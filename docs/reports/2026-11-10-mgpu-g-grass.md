# MGPU G. Grass fields and solver handoff

2026-11-10. Base 22559a86f30d472cb8a4353dd10a34127962931b. The core uses the shared P4 runtime and P3b snapshot banks. The separate G2 shadow replica draft and production Bistro graph wiring are outside this change.

The placed producer packs current wind, interaction trail and Firefly presence. Primary expansion still runs per view. The handoff seeds the next solver with the last authoritative checkpoint and its clocks. It retains queued actor inputs and per-wind debt. A paused wind field preserves its bytes, clock and debt while actor presence can update. Each placed invocation processes up to four ordered input ticks. Display selection stages current and previous reader leases. Only the issued primary tail commits the display. A no-submit bridge refusal can cancel and retry the same frame with completed-only selection, preserving the pending Exact demand. Publication metadata follows the occupied import slot. Latest holds a compatible completed field during backpressure and reports age without changing placement or quality.

| Verification | Result | Receipt in Build/G |
| --- | --- | --- |
| MGPUGrassTests Debug/x64 build | PASS | build-mgpu-grass-debug-rev4.json |
| MGPUGrassTests two physical adapters, debug layer and GBV | PASS | mgpu-grass-gbv-rev4.json |
| SecondGroupTests Debug/x64 build | PASS | build-second-group-debug.json |
| SecondGroupTests CPU | PASS | second-group-cpu-retry.json |
| SecondGroupTests grass/Firefly isolation, GBV | PASS | grass-interaction-gbv-native-bin.json |

The native route used NVIDIA GeForce RTX 3060 Laptop GPU and AMD Radeon Graphics. It exercised primary seed, Latest import admission, age, manual cadence, pending Exact cancellation and same-frame fallback, next Exact retry, previous display ownership, return and bank reuse, and the retained completed candidate while three newer exports were gated. The local fixture checked paused/resumed wind debt for both modes, byte parity of packed fields and local DIRECT/COMPUTE Firefly and grass interaction. The standalone route observes completed Exact readiness before its successful legacy fixture recording. The live Bistro Exact bridge, S2 raster and combined water/AO remain integration checks.

Builds used the pinned local SDKs and validation-only P0 /Z7 override. The CPU fixture uses the three verified Khronos Fox files. The isolated GBV run used the native C: target of the Build/bin junction so its atomic CVar save operated on one volume. The final runner fingerprints matched before and after every recorded job. No performance workload ran.
