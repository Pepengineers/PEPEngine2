# Bistro MGPU shadows on MisSpec

Release x64 built successfully from `Apps/App/App.vcxproj`. The ordinary Bistro smoke used the NVIDIA GeForce RTX 5050 as GPU0 and the Intel Arc B570 as GPU1, with four active CSM cascades and a split after cascade 1. Both the sun and the selected point light used `Exact`.

```ini
Render.PrimaryAdapter = 0
App.Shadows.Sun.Adapter = 1
App.Shadows.Sun.NearCascades = 2
App.Shadows.Sun.CrossSync = 1
App.Shadows.Local.Adapter.Paris_Building_01_paris_building_01_bottom_44 = 1
App.Shadows.Local.CrossSync.Paris_Building_01_paris_building_01_bottom_44 = 1
```

The Arc B570 rendered all six faces for point light entity `986` and CSM cascades 2–3. The RTX 5050 rendered cascades 0–1. The first recorded GPU1 export fence was `18`, the first GPU0 import fence was `383`, and the completed GPU0 frame that used both imports retired at fence `514`. The following recorded consumer retired at fence `715`. The smoke log records masks `3` (`0b0011`) and `12` (`0b1100`). The runner stopped the application after observing the completed consumer. There were no error entries in that run.

GPU0 mirrors the frozen mesh, material, transform, camera and vegetation inputs to the owner. The depth pass uses the existing culling and opaque/masked draws. The CSM import updates the far rectangles in the existing atlas and preserves the near rectangles. Deferred lighting and transparency sample the published atlases. Local static reflection captures bind their own shadow atlas and matching light metadata. Remote reflection capture retains its shadow fallback.

`Latest`, fully remote CSM, and spot-light placement were reviewed against the code. The smoke exercised hybrid CSM and a point light in Exact. Latest holds the last sampled image while an owner submission is pending and uses Exact when the projection or publication layout is incompatible. No visual accuracy, timing, or stress measurements were collected.

Local logs under `C:\Users\MidSpec\Documents\PEPEngine2\Build`:

- `bistro-mgpu-shadows-local-release.log`
- `bistro-mgpu-shadows-smoke.log`
- `bistro-mgpu-shadows-smoke-attempts.log` contains earlier attempts, including the cold-load timeout, absent secondary camera binding, and oversized transfer before filtering to masked diffuse textures.
