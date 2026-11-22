# Bistro Exact light grid on MidSpec

`App.LightGrid.Adapter = -1` keeps construction on the scene adapter. Selecting a secondary DXGI index places the existing culling shader on that adapter with one packed sphere per light. Position and radius come from the CPU mirror of the same frozen, ordered light snapshot that feeds deferred lighting on GPU0. Camera view, projection coefficients, far plane and grid layout are recorded as root constants on the builder. GPU0 still uploads its full lighting data locally.

The owner publishes a single mask buffer for the current frame. The GPU0 import waits on the owner export, and the first mask read in clustered deferred lighting depends on the import. The local mask dispatch is omitted for a remote owner. A primary frame slot retires before its mask buffer can be reused, and the owner retires its slot before replacing its uploads, mask buffer or transfer allocation. Placement changes drain both sides before releasing the previous owner.

## Verification

Release x64 built successfully with the light-grid change on base `bd97296b`. One ordinary Bistro run selected `Render.PrimaryAdapter = 0` and `App.LightGrid.Adapter = 1`. The Intel Arc B570 built the grid, and the GeForce RTX 5050 ran deferred lighting, post and presentation. The run kept TAAU Balanced, FG off and the game HUD on the scene adapter.

| Data on frame 1 | Recorded bytes |
| --- | ---: |
| CPU → Arc, 135 position/radius records | 2,160 |
| CPU → Arc, root constants including grid layout | 144 |
| RTX → Arc, GPU resource transfers | 0 |
| Arc → RTX, 12 × 8 × 32 cells with 5 mask words per cell | 61,440 |

The recorded frame reached `Present = S_OK`. Arc DIRECT build and export fences were 2 and 3. RTX COPY import fence was 3, RTX DIRECT deferred fence was 497, and RTX DIRECT frame fence 518 was complete when the evidence was logged. The runtime log contained no error or critical entries. The runner stopped the hidden process after collecting the result.

Local logs are `Build/bistro-mgpu-light-grid-release.log` and `Build/bistro-mgpu-light-grid-smoke.log`. Release x64 also built after integrating SDK and submodule update `90bdbea2`, with the build log at `Build/bistro-mgpu-light-grid-integrated-release.log`. Placement changes and the GPU0 dispatch path were inspected in code.
