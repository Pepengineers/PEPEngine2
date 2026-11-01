# Particle sources

`ffx_parallelsort.h` is copied without changes from the FidelityFX SDK pinned at
`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`,
`sdk/include/FidelityFX/gpu/parallelsort/ffx_parallelsort.h`. Its AMD MIT notice is
retained in the file. `ParticleSort.hlsl` supplies only HLSL type/atomic aliases,
buffer callbacks and dispatch entry points for these real count/reduce/scan/scatter
kernels; it does not replace the sort with a CPU implementation.

Pool architecture reference: [AMD GPUParticles11](https://github.com/GPUOpen-LibrariesAndSDKs/GPUParticles11),
MIT, copyright 2016 AMD; license retained alongside this file. Simulation is an
independent D3D12 implementation using explicit bounded reservations, a ping-pong
alive list and an indexed dead list instead of D3D11 hidden append/consume counts.
No DXUT, sample application, textures or demo scene are imported.
