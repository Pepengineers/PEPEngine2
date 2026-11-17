# App renderer replacement — 2026-11-17

## Result

The ordinary Meta Bistro renderer replaces the previous App renderer. The solution
has one application project, `Apps/App/App.vcxproj`, retaining its existing GUID.
Engine/UI/App.Base/External projects remain libraries. App.MetaRenderGraph,
App.Editor and App.Benchmark application projects are removed.

The former Meta host is `Apps/App/src/BistroApplication.cpp`; the sole pipeline is
`Apps/App/src/Rendering/BistroRenderPipeline`. The packet compiler, recording,
submission and technique sources moved into App's `src/Graph` and `src/Techniques`.
There is no legacy GDX12RenderGraph/default pipeline fallback.

## Removed duplication

- Old graph registration/compiler/execution and default render-pipeline wrappers.
- Duplicate opaque/culling/shadow/AO/SSR/environment/probe/post/particle recording
  paths; shared resource preparation, GPU kernels and SDK integrations remain.
- Procedural diagnostic scene, direct fixture, A/B histories, readbacks and retired
  experimental particle/water transports. The ordinary cloud-picture transport stays.
- Old per-technique async routing switches. The packet graph owns queue placement.
- Unused shader files and entries after their last renderer consumer was removed.
- Standalone fixtures exclusively coupled to deleted renderers. Independent
  allocator/physics/content/kernel checks remain; MotionBlur and DoF fixtures now
  call the explicit preparation/recording API used by the sole renderer.

## Preserved behavior

Original Bistro scene setup, sun and point lights, local/sun/probe shadows,
reflections, grass, particles, fluid, skinning, physics, UI and native TAA use the
ordinary Meta path. Reconstruction/FG input ownership and state publication,
executor retirement, recording error handling and presentation order are retained.
Native TAA provider 5/quality 0 and FG provider 0 are the startup defaults.
FSR FG/XeFG integrations retain the previously accepted SDK swapchain fix.

Startup.ini is copied to the App output and replaces Config/Engine.ini at launch,
as the former ordinary Meta host did. Historical reports retain historical paths.

## Verification

- All retained Apps/Engine project XML parses; explicit file/project items resolve.
- Source review compared the ordinary Meta path with the replacement, including
  compacted geometry resource IDs, skinning bindings, FG lifetime and submission order.
- Final Release x64 App build passed (MSBuild exit 0). Only existing missing Jolt
  PDB/debug-symbol linker warnings remained. Log: `Temp/unified-app-final-release.log`.
- Only a short ordinary App smoke is required. No accuracy/mode matrix, repeated
  FG acceptance, profiling or long soak is part of this replacement.

- Ordinary `Build/bin/x64/Release/App/App.exe` smoke passed on NVIDIA GeForce
  RTX 3060 Laptop GPU (5994 MiB), 1280x800. Scene, reflections and UI were visible;
  the reconstruction overlay reported Native TAA. Startup/runtime INI confirmed
  upscaler provider 5 and FG provider 0. The cloud picture published frames.
- No ERROR/FATAL/device-removed entries in the smoke log. The title showed advancing
  frame statistics; this was a functional smoke, not a performance measurement.
  Closing the window logged `Application resources released` and removed the window.
- Local evidence: `Temp/unified-app-smoke.jpg` and
  `Build/bin/x64/Release/App/Logs/Engine.log` (ignored build artifacts).
- `git diff --check` passed. Retained standalone test binaries were not rebuilt/run;
  no Debug, FG mode matrix, cross-adapter hardware acceptance or long soak was run.
