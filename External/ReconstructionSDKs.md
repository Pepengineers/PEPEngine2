# Rendering reconstruction SDK packages

Updated 2026-11-01/02 from public stable vendor releases. Runtime files are unmodified vendor binaries; headers/import libraries match their package. SDK runtimes and `External/Runtime/Licenses` are copied beside App and imported test executables by `CopyReconstructionSDKFiles`, including when a vendor archive carries older timestamps.

| Package | Source | Deployed DX12 components |
| --- | --- | --- |
| NVIDIA Streamline 2.14.16 | [Official release](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16) | Streamline 2.14.16, DLSS SR/FG 310.9.16.0, NIS, Reflex/PCL; production `bin/x64`, not development binaries |
| AMD FSR SDK 2.4.0 | [Official tag](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.4.0) | Loader 2.3.0.4555, Upscaling 4.1.2.4555, Frame Generation 4.0.2.4555 from `Kits/FidelityFX/signedbin` |
| Intel XeSS SDK 3.0.2 | [Official release](https://github.com/intel/xess/releases/tag/v3.0.2) | SR 2.0.2.68, XeFG 1.3.1.78, XeLL 1.3.2.10; package version differs from component versions |

The Intel SR DLL already matched this release and remains byte-identical. NVIDIA's matching `sl.interposer.lib` also remains byte-identical. Optional FG libraries are loaded from the executable directory; the interposer remains resident while proxy COM objects may still exist.

## Reproduction

Intel archive: `XeSS_SDK_3.0.2.zip`, SHA256 `88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990`. NVIDIA archive: `streamline-sdk-v2.14.16.zip`, SHA256 `32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b`.

Intel `inc` maps to `External.Libraries/xess/Include`, and `lib` to its `lib` directory. NVIDIA `include` maps to `External.Libraries/nvidia-sdk/Include/nvidia-sdk`. Their selected runtime DLLs map to `External/Runtime`.

AMD public `api/include`, `upscalers/include` and `framegeneration/include` are flattened into the existing `External.Libraries/ffx-api/Include/ffx-api` layout. Only their package-relative include paths are adapted: top-level `../../api/include/` becomes local, and FG DX12/Vulkan `../../../api/include/dx12/` / `vk/` becomes local inside the corresponding backend directory. No algorithmic SDK changes are applied. Loader import library maps to `ffx-api/lib`; preserve AMD's complete `docs/license.md`.

| Runtime file | SHA256 |
| --- | --- |
| `amd_fidelityfx_loader_dx12.dll` | `e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb` |
| `amd_fidelityfx_upscaler_dx12.dll` | `aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace` |
| `amd_fidelityfx_framegeneration_dx12.dll` | `e6843252f58b0341a0d80f6a20bc0d840643ee95bc48c22cac08745344577bb6` |
| `sl.interposer.dll` | `b1d7395aebd7b9722841d64dcf7433680020b6a8e421f851c16d5ba0c2182edf` |
| `nvngx_dlss.dll` | `d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e` |
| `nvngx_dlssg.dll` | `8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7` |
| `libxess_fg.dll` | `ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27` |
| `libxell.dll` | `d2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7` |

The separate patched FidelityFX SDK 1.1.4 used by SSSR/DoF/denoiser remains pinned by [its existing build](../tools/Build-FidelityFX.ps1) and [instructions](FidelityFX/README.md). FSR SDK 2.4.0 is not a drop-in replacement for those static effects.
