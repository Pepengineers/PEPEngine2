# MGPU fountain flipbook

`mgpu-fountain-atlas.png` packs the existing 64 MGPU particle frames without
resampling or recoloring. Source: the user's local
`D:/Project/Multi-GPU-Render-DX12/Data/Common/Particles/tile (1).png` through
`tile (64).png`, also used by that project's `Common/ParticleEmitter.cpp`.

Layout: chronological, left to right then top to bottom, 8 columns × 8 rows.
Each original RGBA frame is 128×128; each 136×136 atlas cell has a 4-pixel
transparent border. Atlas dimensions: 1088×1088. Playback is once over lifetime.
Packing copies straight RGBA bytes directly; all 1,048,576 source pixels across
the 64 decoded PNG frames were compared and match exactly, including alpha.
No new third-party texture source was introduced. The upstream authorship/license
of these already-present MGPU images has not been independently established.
