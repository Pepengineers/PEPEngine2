# Grass constants and steady-frame transfer audit

## Change

`GDX12GrassRenderer::PrepareView` now sets the camera HiZ dimensions from
`view.SceneDepth` before its single `parameters->CopyData(element, p)`.
The second 384-byte write in `SceneGrass::Append` and the temporary
`Output::Settings` copy were removed. No new Upload or Readback resources
were introduced by this change. Existing per-frame parameter pools remain.

## Capture conditions

Release x64 build passed. Ordinary Bistro was warmed up before capture,
without smoke flags or quality overrides. The current source base includes
`d42d15f9` (GPU particles), so particles are present in these captures.
Existing uncommitted point-light restoration was included in the executable
and left uncommitted; `BistroAdditionsTests.cpp` was untouched.

Executable and both runtime INI hashes were identical before and after the
profiling runs (`Build/upload-readback-audit/fingerprints-{before,after}.json`).
PIX and Nsight ran separately. The camera moved during the final PIX session;
the captures are transfer audits, not an A/B performance comparison.

Artifacts:

- PIX: `Build/upload-readback-audit/steady.wpix`, final capture at 21:51:05
  Moscow time; session `pix-4`.
- Nsight: `C:/Users/Asus/Documents/NVIDIA Nsight Graphics/GraphicsCaptures/App.MetaRenderGraph_2026_11_15_21_49_43.ngfx-capture`.
- Nsight API export and decoded arguments/resource/heap properties:
  `Build/upload-readback-audit/nsight-transfers.json` and
  `Build/upload-readback-audit/nsight-transfer-details.json`.
- PIX EOP-to-EOP CSV:
  `C:/Users/Asus/AppData/Local/Temp/pix-mcp-cache/918b8c4cb3413749/events__EOP-to-EOP-Duration-ns-f5bc286d.csv`.

The Nsight wrapper returned failure because it searched the requested output
directory while Nsight saved to its Documents directory. The actual capture
opened successfully and its metadata and API parameters were inspected.
PIX's analysis bridge is disabled; PIX measurements used pixtool replay counters.

## Observed transfers

Both profilers reported one CopyBufferRegion, four CopyTextureRegion calls
and one ResolveQueryData per captured frame. Nsight decoded their arguments
and memory properties; PIX supplied the replay durations below.

| Operation | Direction and payload | PIX EOP-to-EOP sum |
| --- | --- | ---: |
| CopyBufferRegion | Upload to Default, 10,920 bytes of local-shadow draw commands | 0.005120 ms |
| Four CopyTextureRegion | Default to Default textures; two RGBA16F, one R8_UNORM and one R32_FLOAT, all 1280x800 | 0.002304 ms |
| ResolveQueryData | Two timestamp queries to a Readback buffer: 16 bytes | 0.016384 ms |
| Total | Six transfer/query-resolution calls | 0.023808 ms |

The command upload matches `SceneLocalShadows::Append`'s packed-command copy.
The four texture copies occur in the SSSR/denoiser region of the command stream;
their Default heap types confirm that they are GPU-local copies, not CPU
uploads or image readbacks. Their payload is about 20.5 MiB of GPU-local data.
The readback matches `GDX12GPUTimer::Stop`, whose QueryCount is two.

Nsight also recorded 72 CopyDescriptorsSimple calls. These copy CPU descriptors,
not buffer or image payloads. There were no captured Map/Unmap calls, which is
consistent with persistent mappings; it does not establish the absence of CPU
writes through those mappings.

No bulk static-scene, grass-root, grass-geometry, light-snapshot or diagnostic
image readback copy was present in these steady frames. The ordinary graph
removes diagnostic Readback operations in `SceneGeometry::Plan`; grass
diagnostic readback is also gated by `_diagnostics`.

## Conclusion and limits

The redundant grass constant write is removed at its source. The observed
explicit uploads/readbacks are small and do not look like a significant
steady-frame transfer bottleneck. The remaining shadow-command upload serves
the selected shadow updates; removing it was outside this change.

Upload/Readback are heap types, not necessarily separate D3D12 queues. This
audit classifies actual resource transfers and does not measure PCIe bandwidth
or isolate queue-wide scheduling stalls. GPU frame captures do not record
ordinary CPU memcpy into persistently mapped Upload memory. Direct GPU reads
of Upload constant buffers and SDK constant rings remain; their total traffic
and CPU write cost were not measured. No occupancy or FPS improvement is claimed.
