# XeGTAO

Intel XeGTAO sources from the local Multi-GPU-Render-DX12/MGPU-SSAO/Shaders donor.
Upstream: https://github.com/GameTechDev/XeGTAO

The copyright and SPDX notices are preserved. The depth-pyramid reduction masks
in XeGTAO.hlsli were corrected from scalar bool truncation to bool2.
PEPEngine2's AOXeGTAO.hlsl supplies native DX12 bindings and a single-GPU wrapper.
