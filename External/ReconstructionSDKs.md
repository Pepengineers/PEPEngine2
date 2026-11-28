# Rendering reconstruction SDK packages

The deployed reconstruction DLLs are unmodified vendor binaries. `CopyRuntimeSDKFiles` copies the runtimes, NIS shaders and `External/Runtime/Licenses` beside App and the test executables that import its project.

| Package | Source | Deployed DX12 components |
| --- | --- | --- |
| NVIDIA Streamline 2.14.16 | [Official release](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16) | Interposer, common, DLSS Frame Generation 310.9.16.0, Reflex and PCL from production `bin/x64` |
| NVIDIA DLSS SDK 310.9.16 | [Official tag](https://github.com/NVIDIA/DLSS/tree/v310.9.16) | Direct NGX for DLSS Super Resolution on the selected post device. Uses `nvngx_dlss.dll` 310.9.16.0 |
| NVIDIA Image Scaling SDK 1.0.3 | [Official tag](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling/tree/v1.0.3) | DXC compiles the vendor compute shader from `Shaders/NIS` for the selected post device |
| AMD FSR SDK 2.4.0 | [Official tag](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.4.0) | Loader 2.3.0.4555, Upscaling 4.1.2.4555, Frame Generation 4.0.2.4555 from `Kits/FidelityFX/signedbin` |
| Intel XeSS SDK 3.0.2 | [Official release](https://github.com/intel/xess/releases/tag/v3.0.2) | SR 2.0.2.68, XeFG 1.3.1.78, XeLL 1.3.2.10; package version differs from component versions |

The 2026-11-22 audit compared the runtime DLLs, SDK headers and import libraries against the corresponding releases, accounting for Git line endings and the AMD include paths described below. The bundled direct NGX files, including the existing `nvngx_dlss.dll`, match the DLSS 310.9.16 tag.

The Streamline release supplies `sl.interposer.lib`. The other `sl.*.lib` files in the vendor directory are outside the release package and the application loads Streamline through the interposer DLL. Optional FG libraries are loaded from the executable directory. The interposer stays resident while proxy COM objects may still exist.

## Reproduction

Intel archive: `XeSS_SDK_3.0.2.zip`, SHA256 `88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990`. NVIDIA archive: `streamline-sdk-v2.14.16.zip`, SHA256 `32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b`.

Intel `inc` maps to `External.Libraries/xess/Include`, and `lib` to its `lib` directory. NVIDIA `include` maps to `External.Libraries/nvidia-sdk/Include/nvidia-sdk`. Their selected runtime DLLs map to `External/Runtime`.

DLSS SDK 310.9.16, commit `9981c6350dc58c0ce21f024fd65e31e851c31689`, supplies the NGX headers in `External.Libraries/ngx/Include/ngx`. The DLL CRT libraries from `lib/Windows_x86_64/x64` map to `ngx/lib/Release/nvsdk_ngx_d.lib` and `ngx/lib/Debug/nvsdk_ngx_d_dbg.lib`. The NVIDIA driver supplies the NGX core. The application ships the signed `lib/Windows_x86_64/rel/nvngx_dlss.dll` with `Licenses/NVIDIA/nvngx_dlss.license.txt`.

NIS SDK 1.0.3 supplies `NIS_Config.h`, `NIS_Main.hlsl` and `NIS_Scaler.h` in `External.Libraries/nis`. The two shader files and `Licenses/NVIDIA/nis.license.txt` accompany the executable.

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

## Other engine dependencies checked on 2026-11-22

| Dependency | Selected version | Integration |
| --- | --- | --- |
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics/releases/tag/v5.7.0) | 5.7.0 | `Build-Jolt.ps1` builds the pinned submodule for the selected configuration. Optimized Debug uses the debug DLL CRT. |
| [rapidyaml](https://github.com/biojppm/rapidyaml/releases/tag/v0.16.0) | 0.16.0 | The release includes c4core 0.6.0 and fast_float 8.2.10 in its source tree. |
| [inih](https://github.com/benhoyt/inih/releases/tag/r63) | r63 | Pinned submodule. |
| [Agility SDK](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.620.2) and [DirectX-Headers](https://github.com/microsoft/DirectX-Headers/releases/tag/v1.620.2) | 1.620.2 | The executable exports the SDK version from the bundled headers. The runtime and debug layer ship in the application's D3D12 directory. |
| [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609) | 1.9.2609 | x64 compiler and validator DLLs report 1.9.2609.5. The engine uses shader profiles through 6.8. |
| [DirectXTex](https://www.nuget.org/packages/directxtex_desktop_win10/2026.5.8.1), [DirectXMesh](https://www.nuget.org/packages/directxmesh_desktop_win10/2026.5.8.1), [DirectXTK12](https://www.nuget.org/packages/directxtk12_desktop_win10/2026.5.8.1) | 2026.5.8.1 | Desktop x64 libraries from the May release, selected by build configuration. |
| [D3D12 Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator/releases/tag/v3.3.0) | 3.3.0 | The renderer compiles the allocator from source. |
| [WinPixEventRuntime](https://www.nuget.org/packages/WinPixEventRuntime/1.0.240308001) | 1.0.240308001 | Official x64 release DLL and import library. |
| [DirectStorage](https://www.nuget.org/packages/Microsoft.Direct3D.DirectStorage/1.3.0) | 1.3.0 | Both bundled x64 runtime DLLs match the stable package byte for byte. |
| [Dear ImGui](https://github.com/ocornut/imgui/tree/v1.93.0-docking) | 1.93.0 docking | Core, Win32 and DX12 backends from the same docking tag. |
| [Assimp](https://github.com/assimp/assimp/releases/tag/v6.0.6) | 6.0.6 | Built from tag `a79ff6bb` with VS 2022 v143, shared libraries and bundled zlib. Both x64 configurations use matching headers and import libraries. The public overflow check uses parenthesized `max` for Windows macro compatibility. |

Wwise stays at 2025.1.5.9095 with its matching plug-ins. spdlog 1.17.0 and meshoptimizer 1.3 match their stable releases. The enkiTS pin `404a3bf` includes the v1.12 release and subsequent scheduler fixes. The pinned V-HACD, standalone CACAO, XeGTAO and sky atmosphere revisions match their respective upstream default branches at the audit date.

The engine projects select the installed Windows 10.0 SDK through MSBuild and use the v145 C++ toolset. The x64 builds for this audit used Windows SDK 10.0.26100.0.
