# W. Cross-adapter water validation

Validated on November 10, 2026 in `codex/mgpu-water`, with P4 `50ee64fb` and P2d `d49e5f6e` applied at `a230cd9cf1e6fff6b579fb7f05c3ac0a00db126a`.

| Check | Result | Receipt under `Build/W` |
| --- | --- | --- |
| Clock Debug build and run | Pass | `clock-build-debug-r2.json`, `clock-run-debug.json` |
| Clock Release build and run | Pass | `clock-build-release.json`, `clock-run-release.json` |
| Native Debug build | Pass | `native-build-debug.json` |
| Two physical adapters, D3D12 debug layer | Pass | `native-two-adapters-debug-layer.json` |
| Two physical adapters, GPU-based validation | Pass | `native-two-adapters-gbv.json` |

The native fixture verifies distinct adapter LUIDs and checks both D3D12 info queues for errors. It reads back the current and previous particle payloads, compares Exact and Latest at the same logical step, and compares two 60 Hz steps with one 30 Hz producer invocation. Delayed COPY tests cover primary progress, held publication storage, completed-only Latest selection, Exact ordering and reset during import. With Surface absent, both modes advance the solver without displaying a snapshot. The first visible frame resets motion continuity.

The initial Clock Debug build hit MSVC C4459 under warnings-as-errors because the fixture constant shadowed a clock template parameter. Renaming the fixture constant kept the solver clock unchanged. The initial failure and successful retry have separate receipts.

| Executable | SHA-256 |
| --- | --- |
| Clock Debug | `1810d1d7c4b5d2e2cda6166d860e9c0cc4d6d6ced4a89bef9bc01655e730c7a4` |
| Clock Release | `5e48e700c0a2852700bb2abc3d90732e25ea7c2a902dc7ee04da41b5ce985098` |
| Native Debug | `ed0f4cf7349dcdd5ce9d5fcc1cc57cd212bdfb74135e33a92a036a08bb5e4240` |

`Build/W/processes-final.json` records zero active workloads or descendants from the eight recorded process roots, including the initial failed build. The preexisting Rider project-evaluation process was identified separately.

The fixture validates the W controller and particle transport. Production Bistro graph registration, the Exact cut, actual Surface motion, and the Collision View caller still require the integrator's validation. No performance run was performed.
