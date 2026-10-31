# Sky atmosphere

Adapted LUT parameterization, participating media and multiple-scattering
integration from Sébastien Hillaire's MIT reference implementation:
https://github.com/sebh/UnrealEngineSkyAtmosphere

Pinned upstream revision: `183ead5bdacc701b3b626347a680a2f3cd3d4fbd`.
The accessible framework donor's `MGPU-Atmosphere/Shaders/SkyAtmosphere` provided
the selected transmittance / multiscattering / sky-view / aerial-volume path.
The port uses a compact native ABI and keeps the upstream MIT notice in LICENSE.txt.

Paper: Hillaire, *A Scalable and Production Ready Sky and Atmosphere Rendering
Technique*, EGSR 2020, https://sebh.github.io/publications/egsr2020.pdf .

Engine world centimetres/Y-up map to kilometres/Z-up as `(x,-z,y)*1e-5`.
Rayleigh and Mie scale heights are kilometres; optical coefficients are per km.
The existing directional light supplies solar irradiance and direction. The solar
disk uses irradiance divided by its projected solid angle, clamped to finite
RGBA16F radiance. It is excluded from IBL to avoid counting the direct light twice.
Aerial perspective stores RGB scattering and RGB transmittance in separate volumes.
Sky-view is nonlinear in view zenith and relative sun azimuth; a stable projected
dot product handles zenith and a vertical sun without normalizing zero vectors.

Quality selects LUT resolution and integration steps; source identity resets
temporal history, while continuous lighting/altitude changes refresh the LUTs/IBL
without restarting screen-space temporal history or partial probe capture cycles.
