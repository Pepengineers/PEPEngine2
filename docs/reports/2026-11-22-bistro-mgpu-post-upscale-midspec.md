# Bistro MGPU post and presentation on MidSpec

Release x64 built with zero errors. One ordinary Bistro smoke used the RTX 5050 for the scene and the Intel Arc B570 for the post stack, game HUD, screen UI composition and swapchain. The actual provider was TAAU Balanced at a 752 × 470 render grid and a 1280 × 800 display grid. Frame generation was off.

```ini
Render.PrimaryAdapter = 0
App.Post.Adapter = 1
App.UI.Game.Adapter = 1
Render.Upscaler.Provider = 5
Render.Upscaler.Quality = 3
Render.FrameGeneration.Provider = 0
Render.UI.Game.Enabled = 1
Render.UI.Game.Quality = 1
```

| Transfer on the observed frame | Format or source | Footprint (bytes) |
| --- | --- | ---: |
| RTX → Arc, composed scene-linear HDR | RGBA16_FLOAT | 2,887,552 |
| RTX → Arc, final post depth | R32_FLOAT view of the depth resource | 1,443,776 |
| RTX → Arc, final post motion | RG16_FLOAT | 1,443,776 |
| RTX → Arc, separate feature motion | Absent in this Bistro path | 0 |
| RTX → Arc, alignment between inputs | Packed shared buffer | 192 |
| RTX → Arc, total scene payload | Same-frame bundle | 5,775,296 |
| HUD or inspector between adapters | HUD rendered on Arc, inspector disabled | 0 |
| Arc → RTX, display color | Direct Arc swapchain | 0 |

The CPU writes the current camera constants to Arc and passes the same frame's jitter, sizes and reset state to the stack. Arc keeps complete motion, reconstruction history, and post intermediates on its device. Its game HUD and informational overlay share a local screen layer, with the information and debug UI above the game HUD. The display transform writes to the actual Arc back buffer. The scene timer ends on the RTX DIRECT queue before the owner submits its display work.

The first recorded frame returned `Present = S_OK` on the Arc swapchain. The RTX scene handoff fence was 512 and the Arc terminal fence was 12. The application confirmed completion of the Arc fence before logging the result. The runner stopped the hidden process after capturing it. The runtime log contained no error or critical entries.

The code uses a pure game HUD layer when the HUD has another owner. GPU0 can publish its game layer before debug UI, and a separate secondary HUD owner can export the layer directly to the display GPU. The optional scene texture inspector atlas travels only when active. If direct presentation fails with FG off, GPU0 can import one finished display-color texture and compose UI locally. If the requested reconstruction or FG cannot run on the post adapter, the selection returns to the local stack and logs the reason. Compatible direct FG uses the imported depth and owner-generated complete motion, with both retained on the presentation device until the existing FG lifetime ends. It adds no scene input to the bundle. Those paths, resize and owner transitions were checked in code. The smoke covered the direct Arc route with FG off. No screenshots, performance runs or configuration matrix were collected.

Local logs are `Build/bistro-mgpu-post-release.log` and `Build/bistro-mgpu-post-smoke.log`. The reference rebinds the window and back-buffer RTVs to the selected presentation device before drawing and presenting on it. [Reference presentation selection and draw](https://github.com/Mikhanil/Multi-GPU-Render-DX12/blob/d17e45bcfef0a16af0f7ee74e936c162845bd221/MGPU-Reflection/ReflectionRenderer.cpp#L876-L975)
