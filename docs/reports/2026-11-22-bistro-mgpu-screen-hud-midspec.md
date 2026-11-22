# Bistro MGPU screen HUD on MidSpec

## Review follow-up

Release x64 rebuilt with zero errors. One ordinary Exact run repeated the initial `98fcfcc2` settings below. The RTX 5050 completed the frame after importing the Arc B570 HUD at fence 511 and compositing at fence 513. The final frame fence was 515. Arc raster and export retired at fences 4 and 6. The layer remained 1280 × 800 premultiplied RGBA8_UNORM, 4,096,000 transferred bytes at row pitch 5,120, with zero scene bytes sent to Arc. The log reported no errors. The runner stopped the hidden process after observing completion.

Source review traced a partial HUD submission through the owner's executor retirement fences. Submission failure and presentation drain include retained HUD owners. The primary import joins owner work in the normal path. Slot reuse checks the owner before releasing any recording, and a generation whose queue cannot be drained retains its resources. There were no forced-failure or resize runs.

The remote icon atlas reloads when `RenderModule::SetTextureReplicaSource` replaces the source registered for `BistroDemoHUD`. Other registrations leave it resident. Recorded draws retain their atlas texture handles. This path uses explicit source registration. Editing an asset on disk alone does not trigger a reload. The font atlas keeps its existing ImGui snapshot mechanism. Live icon replacement was reviewed in code.

`ScenePostUI::OwnerHudLayer()` yields a pure layer for a secondary HUD owner. The GPU0 path renders the game HUD and debug UI into the same texture. A future GPU1 presentation path with a GPU0 HUD needs a read point before debug UI. The normal GPU0 path has no extra copy for that case.

Review logs are `Build/bistro-mgpu-hud-review-release.log` and `Build/bistro-mgpu-hud-review-smoke.log`.

## Initial smoke at `98fcfcc2`

Release x64 built successfully. The ordinary Bistro run rendered the game HUD and procedural health bar with GPU sparks on Intel Arc B570. NVIDIA GeForce RTX 5050 imported that HUD layer, drew the information and debug UI above it, applied the combined UI to the displayed frame and reached the final frame fence.

```ini
Render.PrimaryAdapter = 0
App.UI.Game.Adapter = 1
Render.UI.Game.Enabled = 1
Render.UI.Game.Quality = 1
```

| Evidence | Completed frame 1 |
| --- | --- |
| HUD owner | [1] Intel Arc B570 |
| Scene and display | [0] NVIDIA GeForce RTX 5050 |
| HUD layer | 1280 × 800, linear premultiplied RGBA8_UNORM |
| Cross-adapter footprint | 4,096,000 bytes, row pitch 5,120 bytes |
| Scene color, depth and motion sent to HUD owner | 0 bytes |
| Owner raster / export fence | 4 / 6 |
| Display import / composite / final frame fence | 511 / 513 / 515 |
| Information and debug UI above game HUD | yes |

The renderer uploads the captured ImGui draw lists and font pixels from the CPU to the owner. The owner reuses the immutable font atlas and loads the Bistro icon atlas from the same asset. The backend derives framebuffer scale from the physical HUD target and the captured logical viewport, then applies the captured clip rectangles. The imported layer initializes the existing display UI texture for the current frame. Overlay raster preserves the game layer underneath. Screen composition uses premultiplied source-over blending before Present.

The ordinary default remains `App.UI.Game.Adapter = -1`. If the HUD is disabled, there is no layer import, and any information or debug UI clears its target before rendering. A size change replaces display-size targets and their footprints, and leaves the device-local atlas and pipeline reusable. Frame slots retain recordings until the display frame retires. Prior owner generations remain alive through their last slot. `ScenePostUI::OwnerHudLayer()` exposes the pure game HUD texture on the secondary device and its completion token after `SubmitHud`. It is intended for a same-device consumer in the next stage.

The runtime log contained no error or critical entries. The runner used Stop-Process after observing completion. It ran with a hidden window and captured no screenshot. Disabling, adapter changes, DPI changes and resize were reviewed in code. The run did not exercise those transitions separately.

Local logs are `Build/bistro-mgpu-hud-release.log` and `Build/bistro-mgpu-hud-smoke.log`. The reference uses a separate UI render target on the second device, a shared surface and display-side composition. [Reference HUD render and copy](https://github.com/Mikhanil/Multi-GPU-Render-DX12/blob/d17e45bcfef0a16af0f7ee74e936c162845bd221/MGPU-IMGUI/HybridUIApp.cpp#L238-L314)
