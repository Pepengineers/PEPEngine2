# DirectX 12 Agility runtime

Renderer applications export `D3D12SDKVersion=619` and an app-relative
`D3D12SDKPath` pointing to `.\D3D12\`.
`tools/Prepare-AgilitySDK.ps1` downloads Microsoft's **1.619.6** NuGet package,
checks its pinned SHA256 and caches it under `Build/Dependencies`. The renderer's
public MSBuild props deploy the matching runtime, debug layer, state-object
compiler and license into each application's `D3D12` directory, including tests.
Ship that directory beside the executable. The first build needs network access;
later builds use the cache. This does not install or replace the system runtime.

The Windows system runtime 10.0.26100.9549 reported cross-queue texture ownership
errors in the queued DIRECT/COMPUTE read/read stress test. Agility 1.619.6 passes
the same test with synchronized queue validation enabled. See the
[particle validation report](../../docs/research/2026-11-05-particle-async-compute.md).

The bundled DXC 1.8 compiler signs retail shaders through SM 6.8. Engine shader
targets remain capped at 6.8 even when the runtime advertises a higher model.

Sources: [Microsoft release notes](https://devblogs.microsoft.com/directx/directx12agility/),
[pinned package](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.619.6).
