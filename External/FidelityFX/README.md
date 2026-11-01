# FidelityFX SSSR dependency

AMD FidelityFX SDK **v1.1.4**, commit
`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55` from
[GPUOpen's official repository](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55).
The archive SHA256 is
`eff7e8e371b2d461b980f8c75583133f771e940ed7aa00779dbbaf6dd2f88746`.

The renderer's MSBuild target invokes `tools/Build-FidelityFX.ps1`, which downloads
the pinned archive once, verifies its hash, and extracts only `sdk` and its MIT
license into this checkout's ignored `Build/Dependencies`. It builds the DX12
backend, SSSR, Denoiser and DoF static libraries and their shader permutations with
the upstream CMake build. No Cauldron or samples are compiled or linked. The SDK's
SSSR component already dispatches the three Reflections Denoiser stages.

Prerequisites: VS 2026 C++/Windows SDK and CMake component, curl and tar. To prepare
the dependency explicitly: `powershell -File tools/Build-FidelityFX.ps1 -Configuration Release`.
Debug uses the SDK's `d` library postfix. `cold-history.patch` corrects the denoiser's
average-radiance binding: prefilter/temporal resolve must read the average produced
by current-frame reprojection, not the previous ping-pong surface (zero at startup).
GPU tests reproduce black first-frame reflections without this one-binding fix.
The patch is checked and applied idempotently by the build script. All upstream
notices remain; [LICENSE.txt](LICENSE.txt) is retained here for distribution.

`probe-fallback.patch` adapts only the SSSR environment callback: the engine prepares the same per-pixel prefiltered local/global incident radiance used by PBR, and the SDK loads it at the ray origin for misses. The input named environmentMap is a 2D RGBA16F receiver-grid image in this build. The real AMD ray marcher, confidence weighting and denoiser are retained. See docs/sssr.md and the centralized review report. All patches are checked against the pinned source before application.
