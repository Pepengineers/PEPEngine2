# Hillaire 2020 atmosphere reference

Official author source: https://github.com/sebh/UnrealEngineSkyAtmosphere

Pinned revision: `183ead5bdacc701b3b626347a680a2f3cd3d4fbd`.
License: MIT, copyright 2020 Epic Games, Inc. (`LICENSE` in this directory).

`RenderSkyCommon.hlsl`, `RenderSkyRayMarching.hlsl` and `SkyAtmosphereCommon.hlsl`
are unchanged reference files downloaded from that revision's `Resources/` directory.
They are retained as the licensed algorithm source, not compiled as the DX11 sample.

The engine adaptation is `Engine/Engine.RendererDX12/Shaders/SkyAtmosphere.hlsl`,
`SkyParameters.hlsli` and `SkyRendering.hlsli`: D3D12 compute dispatches, Y-up RH
coordinates, cm-to-km boundary, colored aerial transmittance, frame-slot constants,
Hillaire transmittance parameterization and isotropic multiple-scattering geometric
series. No code was restored from the cancelled PEPEngine2 implementation.
The donor MGPU-Atmosphere was read for context; its framework was not imported.
