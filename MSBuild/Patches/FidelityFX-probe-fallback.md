The patch applies to FidelityFX SDK v1.1.4, commit
`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`. SDK files retain their AMD MIT notices.
The pinned license is preserved in [LICENSE-FidelityFX.txt](LICENSE-FidelityFX.txt).

SSSR material parameters use RGB for optional, prefiltered local-probe fallback
radiance and A for perceptual roughness. A negative RGB value selects the original
environment-map sampling path. Both intersection misses and rough-surface fallback
read the same pixel's value; hit confidence and the denoiser remain upstream.
The prepared fallback is a split-sum probe approximation rather than a stochastic
sample in each individual ray direction. The final PBR resolve applies DFG once.

DX12 and Vulkan shader callbacks/bindings are updated together, without changing
the public SDK host API. PEPEngine builds and validates the DX12 backend only.

`MSBuild/BuildFidelityFX.ps1` applies this patch before configuring/building
shader permutations. It uses `git apply --reverse --check` to detect an
already-applied patch.
