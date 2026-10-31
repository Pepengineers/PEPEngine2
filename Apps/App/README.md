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

Opaque geometry writes a G-buffer (albedo/metallic, world normal/roughness,
F0/baked flag, emissive, motion and sampled reverse-Z depth). A 16x16 tiled compute
pass reconstructs world position, rejects local-light spheres against each tile's
frustum and depth range, and evaluates lighting into HDR. Tile-list overflow falls
back to the complete light list. The source cubemap supplies the visible background.
Transparent bottles retain forward OIT and receive the same local-light PBR and PCF shadows.

Both lighting paths use Cook-Torrance PBR with GGX distribution, correlated
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

The directional light uses four stabilized cascades in a 4096x4096 depth atlas
(2048x2048 per cascade). Logarithmic/uniform splits cover up to 6000 scene units.
Receiver frusta overlap, transitions blend over their last 10%, and the final
cascade fades out. Matrices use the unjittered camera and a light-space texel grid.
GPU culling builds four independent lists of opaque/masked shadow casters from
the complete scene, including off-camera objects. Each cascade uses ExecuteIndirect;
masked materials alpha-test in the depth pass. Transparent bottles receive shadows
and IBL, but do not cast opaque shadows. Shadows use depth/slope/normal bias and
3x3 comparison-sampler PCF, constrained to each atlas tile.

IBL consists of diffuse irradiance, an eight-mip GGX-prefiltered radiance cubemap,
and a split-sum BRDF LUT using the same Smith/Fresnel model as direct lighting.
These resources are generated once on the GPU from `Assets/Textures/Environment/grasscube1024.dds`,
copied from the Multi-GPU-Render-DX12 project's `Data/Common/Textures` folder.
The source is an sRGB BC1 cubemap with six 1024x1024 faces and eleven mip levels.
This is global environment lighting, without local probes or environment occlusion.
Reinhard remains the tone mapper after linear-HDR opaque/OIT composition.

Bistro contains 135 ECS fixture lights (124 point and 11 spot), placed from actual
bulb/glass/lens geometry, including street lamps, string bulbs, chandeliers, wall
lamps and fan globes. `BistroLightPlacements.h` retains the original fixture names.
`TransformComponent` supplies position/rotation; `LightComponent` supplies type,
linear color, intensity, range, spot half-angles, enable/shadow flags and update mode.
Directional light is also an ECS entity. Systems update lights before GPU data upload.

Each light selects `ShadowUpdateMode::Cached` or `EveryFrame`. Cached maps bake on
first use and rebake when the light moves or its shadow projection changes; changes
to intensity/color reuse depth. A system calls `light.InvalidateShadow()` after
changing static casters or when an explicit refresh is desired. EveryFrame lights
render on every frame. Point lights have six maps and spots one, currently 256x256
per view with PCF. Transparent meshes receive shadows but do not cast opaque shadows.
CSM caching also refreshes when the camera-fitted cascade matrices change.

`LocalLightingTests.cpp` checks ECS parameter updates and cache invalidation; the
GPU smoke verifies initial point/spot baking, cached reuse, moving a spot, and
every-frame point updates with actual shadow depth readback.

Controls: right mouse to look; W/A/S/D to fly; Q/E down/up; Shift to move faster;
mouse wheel to change speed; Esc to exit.

Run scene/material and CPU OIT-reference checks with:

```powershell
python Apps/App/verify_scene.py
```

These checks do not replace running the renderer on a DirectX 12 GPU.

`tests/ShadowCascadeTests.cpp` checks the actual cascade matrices, receiver coverage,
depth range, and whole-texel stabilization. Build with the renderer's public include
directories, `/MD`, and `DirectXTK12.lib`.

`tests/LightingGpuSmoke.cpp` runs without loading Bistro or opening a window. It
first checks replacement/removal of an uninitialized renderer, then creates the
CSM/PBR pipeline states, renders one synthetic shadow caster through
GPU culling and ExecuteIndirect, generates IBL on the GPU, and reads back the atlas,
irradiance, radiance mips, and BRDF LUT under the DX12 debug layer. Build it with
the App's include/link settings and the console subsystem.

The BRDF tests compile the same scalar functions as the shaders. In a Visual Studio
developer prompt:

```powershell
cl /nologo /std:c++17 /EHsc /W4 /FoBuild/PBRMathTests.obj /FeBuild/PBRMathTests.exe Apps/App/tests/PBRMathTests.cpp
Build/PBRMathTests.exe
```

The projects target Visual Studio 2026's v145 toolset.
