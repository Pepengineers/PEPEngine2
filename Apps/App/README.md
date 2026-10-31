# Bistro App

Set **App** as the startup project in `PEPEngine2.sln`, select **Release | x64**, and run.
Build from a Visual Studio developer prompt:

```powershell
msbuild Apps/App/App.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

Executable: `Build/bin/x64/Release/App/App.exe`. The existing asset-folder setting
resolves assets independently of the working directory.

The App loads Exterior and Interior into one world, including their submeshes and
materials, and shares texture uploads by path. Loading fails if either part is
missing or cannot be imported. No benchmark objects or camera splines are added.

The existing renderer uses shared vertex/index buffers and bindless material
textures. GPU frustum culling builds separate opaque/masked and transparent
command buffers. One ExecuteIndirect call draws each list; the existing OIT
composition feeds the existing XeSS/output pipeline. The scene stays static;
only the camera needs continuous updates.
Instance and indirect buffers grow once per scene part, rather than once per submesh.

The bottle glass/wine materials have explicit opacity in both scene MTL files;
caps and labels retain their source materials. Glass opacity is an artistic
approximation. The shared OIT passes use floating-point weighted accumulation,
multiplicative revealage, and normalized composition for overlapping bottles.
OIT approximates blended transparency; it does not simulate glass refraction.

Both geometry passes use Cook-Torrance PBR with GGX distribution, correlated
Smith visibility, and Schlick Fresnel. Materials use metallic/roughness factors,
RGB specular maps, normal maps, red-channel roughness maps, and emissive maps/colors.
OBJ shininess supplies roughness when there is no roughness factor; Bistro's
`_ddn`/`_ddna` bump slots are imported as RGB tangent-space normal maps.
Normal mapping derives its tangent frame from UV/position derivatives so mirrored
UVs do not need an extra tangent-sign vertex attribute.

Color textures use sRGB views; normal and roughness maps stay linear. Opaque color
and OIT accumulation use floating-point HDR targets, then the composition pass
applies Reinhard and sRGB encoding before the existing LDR XeSS/output path.
Default directional-light values are in `GDX12MainConstants`.
Baked lighting is preserved without relighting.

Ambient lighting is constant diffuse illumination; CSM and IBL are not implemented.

Controls: right mouse to look; W/A/S/D to fly; Q/E down/up; Shift to move faster;
mouse wheel to change speed; Esc to exit.

Run scene/material and CPU OIT-reference checks with:

```powershell
python Apps/App/verify_scene.py
```

These checks do not replace running the renderer on a DirectX 12 GPU.

The BRDF tests compile the same scalar functions as the shaders. In a Visual Studio
developer prompt:

```powershell
cl /nologo /std:c++17 /EHsc /W4 /FoBuild/PBRMathTests.obj /FeBuild/PBRMathTests.exe Apps/App/tests/PBRMathTests.cpp
Build/PBRMathTests.exe
```

The projects target Visual Studio 2026's v145 toolset.
