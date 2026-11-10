# Packet C cloud environment publication

The packet adds a cloud producer and an App.Base controller on the P4 runtime at `50ee64f`. The integrator owns Bistro graph wiring, the App.Base project registration, and the production settings profile. The fixture compiles the controller directly and exercises the actual P2c and P4 submission paths.

The producer captures the cloud parameters, seed, simulation phase, sun, quality and revisions before submission. Direct and Compute use the same cloud and IBL kernels. One RGBA16F payload carries all faces and mips of the irradiance, GGX and source cubemaps. Primary Direct installs a complete version on the existing output links. The DFG stays on the display adapter.

Exact admits the producer, two copies and Direct reader together. If admission fails before submission, the frame uses Direct. Latest advances each copy only after the preceding stage completes and can display the previous compatible version while work is pending. Layout or source changes invalidate the previous epoch. The controller reports the effective route, selected generation, age, repeats and admission counters.

`Render.Clouds.Bistro`, `Render.Clouds.AsyncCompute` and `Render.Clouds.MultiGPU` default to false. `Render.Clouds.Latest` defaults to true. An update rate of zero leaves dirty production uncapped. A maximum age of zero disables the age diagnostic. A nonzero age threshold records a crossing without changing selection.

## Legacy Off source

The original `SecondGroupTests --environment-gpu-validation` alternates Off, atmosphere and HDRI over 128 cycles. The initial pre-C run at `f067926` passed. A replay of that binary in the C runtime with the same seed INI, DDS and Agility SDK layers failed at step 258. The initial C candidate failed at step 498 and its archived uninstrumented replay failed at step 378. All three failed on the first Off dispatch after SkyBox. GBV reported COMMON for the 1×1 source cube's UAV at root parameter 9 and descriptor 111.

A passive trace reproduced the error at step 288. The same native resource was recorded in the mip-zero UAV descriptor, the COMMON to UAV transition for all subresources, the dispatch binding and the UAV to SRV transition. GBV reported all six faces in COMMON. The allocation had reused a retired heap range. Two uninstrumented changes, a committed Off source cube and explicit transitions for each face, both failed at step 438. The cause of the disagreement between the recorded barrier and GBV remains unresolved.

Off now initializes its three 1×1 cubemaps by copying the same opaque black RGBA16F texel to each face. The former source and filter shaders produced these constants for Off. The copy uses the existing resource-state tracker for COPY_DEST and SRV and leaves the DFG on its existing device-local cache path. Atmosphere, HDRI and the active cloud producer still execute their existing kernels. Both the original source-switch fixture and the focused fixture run with unfiltered GBV.

## Frozen cloud input and comparisons

The focused native fixture used physical adapter LUIDs `0:62869` and `0:57666`, simulation time 0.25 s, phase (0.005, 0.0025), seed 0, quality 2, five octaves, IBL source size 32, 16 filter samples and intensity 1. Sun direction was (−0.35, 1, −0.25) and radiance was (4, 3.8, 3.5).

Primary Compute matched primary Direct on every component. Secondary Exact and Latest matched the secondary Direct control on every transferred component, and both imports preserved all 1,228,800 packed bytes. The device-local DFG matched primary Direct. The earlier comparison of secondary output to primary Direct exceeded 0.015. Independent Direct runs on the two adapters reproduced the differences below with the same input and quality.

| Output | Maximum absolute error | Mean absolute error | RMS | P99 | Components above 0.015 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Irradiance | 0.181641 | 0.0367854 | 0.0511768 | 0.125 | 15,318 / 24,576 |
| GGX | 0.0698242 | 0.00177547 | 0.0042245 | 0.0185547 | 9,784 / 524,280 |
| Source | 0.000976562 | 0.000273457 | 0.000485964 | 0.000976562 | 0 / 32,760 |

The saved contact sheet uses fixed tone mapping and shows the six faces in each row. The integrator inspected the aligned source, irradiance and GGX and their amplified differences. The comparison covers this input and adapter pair.

## Validation receipts

Packet worktree receipts are under `Build/C`. The saved comparisons are `r5-adapter-controls.bin` and `cloud-adapter-comparison.png`. The passive trace attribution is `r8c-attribution.json`. The archived failures include `r6-environment-pre-C-replay.json`, `r8d-environment-plain-control.json`, `r11-environment-candidate-gbv.json` and `r12-ibl-per-face-plain-gbv.json`.

| Final Debug x64 check | Result | Receipt |
| --- | --- | --- |
| Build the environment regression and cloud fixture in sequence | Both passed | `r13-build-environment-neutral.json`, `r13-build-cloud-fixture.json` |
| CPU inputs, defaults and cadence | Passed | `r13-cloud-inputs-cadence.json` |
| Native two-adapter cloud fixture with GBV | Passed with empty stderr | `r13-cloud-placed-gbv.json` |
| Legacy environment regression with GBV, two fresh processes using the same binary | 128 cycles each, empty stderr in both | `r13-environment-neutral-gbv.json`, `r13b-environment-neutral-gbv.json` |

The final fixture checks immutable inputs, default switches, 110 updates and 10 compatible repeats over two seconds at 55 Hz on 60 frames/s, Exact capacity fallback and recovery, Latest progress while work is pending, resized epoch selection and live route changes. Its Off readback covers all 18 faces and compares all DFG components against the original readback.

Production Bistro startup, its profile settings and performance are outside this packet's standalone verification.
