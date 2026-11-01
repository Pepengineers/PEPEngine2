# Component sky and atmosphere

Актуальная интеграция: код объединён в корневую ветку и прошёл CPU/GPU/GBV smoke checks. [Результаты и непроверенные области](rendering/second-group-integration.md). Авторский статус ниже — исторический, до центральной интеграции.


Implementation-only delivery, **UNVERIFIED**. No build, shader compilation, tests,
GPU fixture, benchmark or separate verification/review pass was run by this author.
The [official MIT reference](https://github.com/sebh/UnrealEngineSkyAtmosphere/tree/183ead5bdacc701b3b626347a680a2f3cd3d4fbd)
is retained under `External/SkyAtmosphere`; the engine shaders are an adaptation.

## Ownership and API

`World` registers separate `SkyBoxComponent` and `SkyAtmosphereComponent` pools.
Each instance owns its complete settings. `GetSettings()` returns a const reference;
edit a copy and pass it to `SetSettings()` for finite/range validation and a revision
increment. Replacing a component through ECS assignment also increments its revision.
These APIs are used on the scene/update thread, before lighting and camera upload.
No global registry or singleton holds author settings.

```cpp
// Attach to an existing entity; this example does not create a scene object.
auto& atmosphere = world.GetECS().Add<SkyAtmosphereComponent>(existingEntity);
auto settings = atmosphere.GetSettings();
settings.DirectionToSun = Vector3(-0.3f, 0.8f, 0.2f);
settings.IBL.Intensity = 0.8f;
settings.SkySamples = 48; // explicit override, independent of global quality
atmosphere.SetSettings(settings);
world.SetSkySource(SkySource::SkyAtmosphere, existingEntity);

// Another component may keep completely different settings, selected explicitly.
world.SetSkySource(SkySource::SkyBox, existingSkyBoxEntity);
world.SetSkySource(SkySource::Off);
```

Selection belongs to the active camera's world. `SetSkySource` validates the enum
and the presence of the selected component; removal of a selected component causes
a diagnostic exception on the next snapshot. Explicitly select Off before removal,
or select another component. Destroying the selected world clears the renderer
snapshot. No implicit component search, priority, automatic HDRI fallback or source
mixing occurs. A disabled selected component or quality Off produces a black sky
and zero global IBL, retaining the world's independent direct/local lighting.

The existing Bistro Sun entity receives a default `SkyBoxComponent` for the existing
`grasscube1024.dds` asset. No new entity, material, model or demo scene is added.
Other loaded worlds default to Off until explicitly selected. The current OBJ world
loader/saver has no component serialization support; this API does not invent one.

## Global controls

Exactly these runtime `Default` CVar exist, registered once in `GDX12Sky.cpp` before
startup INI loading. Both ConsoleModule C++ writes and `Config/Engine.ini` work:

| CVar | Default | Values |
| --- | --- | --- |
| `Render.SkyBox.Enabled` | true | bool |
| `Render.SkyBox.Quality` | 3 | 0 Off, 1 Low, 2 Mid, 3 High |
| `Render.SkyAtmosphere.Enabled` | true | bool |
| `Render.SkyAtmosphere.Quality` | 2 | 0 Off, 1 Low, 2 Mid, 3 High |

There is no Source CVar. Source, assets, optics, sun and all budgets belong to the
world/component. Quality resolves a copy of the selected component settings; it
never writes the component. Budget zero inherits the preset, and a positive value
remains explicit across quality changes. Setting that field back to zero restores
inheritance. Enabled/Quality are sampled with the component before camera/shadows
are uploaded, not read independently by every consumer.

## Units, defaults and validation

Scene positions are RH Y-up centimetres. Optical integration and radii use km;
coefficients are inverse km. Sun direction points **towards** the sun; the existing
main directional-light convention points away, so its normalized negative is used.
Atmosphere selection supplies the one directional sun before CSM matrices are
computed. Its TOA illuminance is multiplied by LUT transmittance at each shaded
surface in deferred, transparency and probe captures; CSM/local visibility is kept.

RGB `SunIlluminanceLux` represents top-of-atmosphere solar illuminance, with defaults
(120000, 114000, 105000). `LuminanceScale` (default 1e-5) converts physical luminance
and illuminance to the engine's scene-linear HDR units. The analytic disk uses
`E / (pi * sin(angularRadius)^2)`, with the same solar attenuation. This is an RGB
photometric approximation, not a spectral simulation. The disk is deliberately
excluded from the IBL cube, which already has a matching direct BRDF sun light.
There is no second solar disk/specular term hidden in IBL.

| Component field | Default | Accepted range / interpretation |
| --- | --- | --- |
| Both `Enabled` | true | bool, per instance |
| Box `Cubemap` | `Textures/Environment/grasscube1024.dds` | nonempty DDS cubemap path; relative to Assets or absolute |
| Box `RadianceMultiplier` | 1 | finite 0..100, affects visible sky and IBL |
| Box `RotationRadians` | 0 | finite -10000..10000, Y-axis sampling rotation |
| Both `IBL.Intensity` | 1 | finite 0..100, affects diffuse/specular global IBL only |
| Both `IBL.CubeResolution` | 0 | inherit, or 16..1024 pixels per face |
| Both `IBL.FilterSamples` | 0 | inherit, or 16..1024 samples per diffuse/GGX texel |
| Atmosphere `PlanetCenterCm` | (0,-636000000,0) | finite components -1e11..1e11 cm |
| `PlanetRadiusKm` | 6360 | 100..100000 km |
| `AtmosphereHeightKm` | 100 | 1..1000 km above the radius |
| `RayleighScatteringPerKm` | (.005802,.013558,.033100) | each channel 0..10 /km |
| `RayleighScaleHeightKm` | 8 | .01..1000 km, exponential density |
| `MieScatteringPerKm` | (.003996,.003996,.003996) | each channel 0..10 /km |
| `MieAbsorptionPerKm` | (.000444,.000444,.000444) | each channel 0..10 /km, independent absorption |
| `MieScaleHeightKm` | 1.2 | .01..1000 km, exponential density |
| `MieAnisotropy` | .8 | -.95...95, Cornette-Shanks phase |
| `OzoneAbsorptionPerKm` | (.000650,.001881,.000085) | each channel 0..10 /km |
| `OzoneCenterKm` | 25 | 0..AtmosphereHeightKm |
| `OzoneHalfWidthKm` | 15 | .01..1000 km, triangular density |
| `GroundAlbedo` | (.3,.3,.3) | each linear reflectance channel 0...99 |
| `DirectionToSun` | (-.35,1,-.25) | finite components -1e6..1e6, length squared >=1e-12; normalized in snapshot |
| `SunIlluminanceLux` | above | each channel 0..200000 lux |
| `SunAngularRadiusRadians` | .004675 | .001...1 rad |
| `LuminanceScale` | .00001 | 1e-7..1; unattenuated disk must stay <=60000 in every RGB channel for RGBA16F |
| `SunCastsShadows` | true | existing CSM/probe sun shadows; every-frame update |
| `AerialDistanceKm` | 96 | .01..1000 km; frustum volume integrates to this authored distance, clamps beyond it |
| `IBLCapturePositionCm` | (0,200,0) | finite components -1e11..1e11 cm, outside/on planet surface |

Asset upload retains cubemap validation: six square faces, nonzero dimensions,
complete mip subresources and sufficient row data. Existing AssetManager owns CPU
asset caching; DDS upload uses the existing committed-resource/queue path. sRGB DDS
is decoded by the SRV; HDR DDS remains linear. The scene-linear output cube is
RGBA16F, with a full mip chain sampled from the original DDS mip chain for SkyBox.
Source cube resolution may resample the old DDS; default High is 512 pixels/face.
It does not alter the asset payload.

Every atmosphere LUT/sample budget below defaults to **zero (inherit)**:

| Field | Low / Mid / High | Explicit bounds |
| --- | --- | --- |
| `TransmittanceWidth` | 128 / 256 / 512 | 16..512 |
| `TransmittanceHeight` | 32 / 64 / 128 | 8..128 |
| `MultipleScatteringSize` | 16 / 32 / 32 | 8..64 |
| `SkyViewWidth` | 128 / 256 / 512 | 32..1024 |
| `SkyViewHeight` | 64 / 128 / 256 | 16..512 |
| `AerialWidth`, `AerialHeight` | 16 / 32 / 32 | 8..64 each |
| `AerialDepth` | 16 / 32 / 64 | 8..64 |
| `TransmittanceSamples` | 32 / 64 / 128 | 8..256 |
| `SkySamples` | 16 / 32 / 64 | 8..256 |
| `MultipleScatteringDirections` | 16 / 32 / 64 | 8..128 |
| `MultipleScatteringSamples` | 16 / 20 / 32 | 8..128 |
| `AerialSamples` | 8 / 16 / 24 | 4..128 |
| Both `IBL.CubeResolution` | 128 / 256 / 512 | 16..1024 |
| Both `IBL.FilterSamples` | 32 / 64 / 128 | 16..1024 |

Diffuse irradiance stays 32 pixels/face and the published GGX cube stays 128 with
8 mips to preserve the existing roughness*7 contract. Quality and component sample
budgets change actual convolution work. DFG stays 128 square with its existing 256
samples; it is the shared PBR BRDF integral, independent of sky optics/source.

## Render and lifetime seams

`LightUpdateSystem -> RenderModule::SubmitSky` copies the selected component and
both controls once for the active world. `SubmitLights` applies atmospheric sun
settings before `GPUDataUpdateSystem` builds cascades and camera history. The
component state is consumed by `GDX12EnvironmentIBLPass`, before depth prepass and
shadows. No new opaque/masked draw path or input layout exists: depth-only late-AO
prepass, normals early-AO prepass, G-buffer Equal/no-write, main shadows, motion and
indirect geometry draws keep their existing paths.

`GDX12EnvironmentIBLPass` publishes its same four links: diffuse irradiance, GGX
prefilter, DFG and source sky cube. Deferred lighting, local probe captures,
transparency and SSSR continue to use these links. The existing local-probe miss
fallback and unified specular resolve are retained.

Atmosphere LUTs are transmittance (Hillaire's radius/distance parameterization),
isotropic multiple scattering (spherical direction integration and geometric
series), a horizon-focused full-sphere sky-view LUT, and frustum aerial perspective.
Aerial radiance and **RGB** transmittance occupy separate RGBA16F 3D textures.
The segment integral uses `(1-exp(-sigma*distance))/sigma`, with its finite zero
extinction limit. Ray segments clip against atmosphere entry/exit and ground,
including views from outside the atmosphere. Camera positions below the surface
are clamped to a 1m shell for sky lookup/integration; the planet center is rejected.

The IBL cube represents `IBLCapturePositionCm`, independent of camera movement.
On source/author/quality changes, sky-view is first evaluated at that capture point,
then converted to the source mip chain and convolved. The visible sky-view is then
rebuilt at the current camera every frame; aerial perspective uses the current
jittered inverse view-projection. Two constant-buffer elements per frame slot keep
the capture and current-camera parameters immutable while their commands execute.

HDR ordering is deferred -> SSSR plus its one specular resolve -> opaque aerial
perspective inside WBOIT composition -> WBOIT HDR -> DoF -> motion blur -> HDR XeSS
-> one tone mapping. Transparent surfaces receive aerial perspective before
weighted accumulation, at their own distance, so opaque fog is not added to them
again. The visible background is already an atmospheric path integral and is not
fogged again. Its analytic solar disk uses the same transmittance and units.

Composition adds optional fourth input: opaque reverse-Z depth. Existing three-input
callers remain neutral for Off/SkyBox; atmosphere requires depth plus active camera.
Deferred root adds b3/t15/t16; transparency adds b4/t11..t13 and moves its material
texture range to t14; probe capture adds b4/t10 and moves that range to t11;
composition adds camera b0, sky b1, depth t3, transmittance t4 and aerial t5/t6.
Sampler s7 is linear clamp. `PBRLighting.hlsli` applies atmospheric solar
transmittance only when the sky bindings are present.

Source/author/quality revisions invalidate `ReflectionEnvironmentRevision`, probes,
SSSR and temporal/upscaler histories. Continuous camera movement does not invalidate
IBL. Sky rotational prevNDC-currentNDC motion remains the existing motion-blur
completion path, excluding camera translation/jitter. Resize uses the newly jittered
camera and render dimensions; LUTs depend on component budgets, not viewport size.

No atmosphere textures or DDS are loaded for Off. Neutral output cubes are 1x1;
only the shared DFG remains full size. Atmosphere LUTs allocate lazily on selection
and release on a switch to HDRI/Off. Source cube, diffuse and prefilter allocations
also shrink on Off. Descriptor/resource replacements wait for the device queue
before rewriting descriptors or releasing resources. Ordinary frame updates reuse
resources on the ordered graphics queue, and CB uploads use the existing waited
frame slot. Asset replacement reuses its descriptor slot.

## Unverified scope and central acceptance

No success claim is made for C++/HLSL compilation, runtime switching, GPU correctness,
visual quality, timings, memory, debug-layer behavior or long-run stability. No new
tests/fixtures/benchmark infrastructure was added or run. The two existing environment
fixture callers now explicitly select SkyBox with nonzero quality; the lighting
fixture retains its explicit 1024-face budget and expects the generated linear cube
only after the existing Execute call. These caller edits were not run. Standalone
pass users set `resources.Sky` explicitly and increment state revision or call
`Invalidate()` after edits; this is an intentional ownership/output-contract change.

The later central checker owns the package's Off/HDRI identity, LUT/Beer-Lambert,
zero coefficients, ground/zenith/space camera, sun, IBL, history, resize, frame slots,
quality and timing checks. The source mip chain is sampled from the sky-view LUT
for atmosphere, so its lowest mips approximate solid-angle filtering; assess horizon
aliasing. Multiple scattering is Hillaire's isotropic approximation with a finite
near-unit-feedback bound. Aerial perspective clamps to its authored distance and
does not include terrain volumetric shadows. Global IBL and probe backgrounds use
the fixed component capture position; large altitude variation needs a suitable
capture position/local probes. Multi-GPU, SSAA and a new demo scene are outside scope.
