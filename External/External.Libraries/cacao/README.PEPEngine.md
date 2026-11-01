# AMD FidelityFX CACAO

Official standalone DX12 reference integration, commit
`0ddca95e6714727a252ead345591ca8f2598f261` from
[GPUOpen-Effects/FidelityFX-CACAO](https://github.com/GPUOpen-Effects/FidelityFX-CACAO/tree/0ddca95e6714727a252ead345591ca8f2598f261).
MIT license: `license.txt` and source notices.

SDK v1.1.4 also contains CACAO 1.4.0. This integration uses the official standalone
implementation to avoid importing the SDK's generic backend and permutation
generator for one effect. No Cauldron source, sample assets or framework is used.
`MSBuild/BuildCacaoShaders.ps1` compiles the 33 entries in AMD's DXIL build manifest
using Windows SDK DXC, into the renderer's own intermediate directory. Generated
headers are build products, not vendored source.

Local changes:

- Add an explicit UAV barrier after clearing the adaptive load counter, before
  later atomic increments use it.
- Clamp input normal/depth `Load` coordinates at odd-sized viewport edges;
  deinterleaving otherwise reads zero in the missing half of the last 2x2 quad.
- Disable the reference implementation's unconditional Cauldron marker define.
- Add `maxFramesInFlight` to DX12 context initialization and size the upload ring
  accordingly, rather than assuming five in-flight frames.
- Include projection row 2 offsets in `NDCToViewAdd` for the engine's jittered RH
  projection. Input normals are already transformed and encoded by the engine.

All depth deinterleave/mips, generation quality levels (including adaptive Q4),
importance maps, edge-sensitive blur and apply shaders remain AMD's implementation.
