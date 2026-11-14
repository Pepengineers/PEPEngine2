# Local DIRECT smoke — 2026-11-14

Base: `207f6385` (`codex/bistro-rendering`).

- Debug x64 App.MetaRenderGraph built after restoring the missing SceneShadows texture-resource include and correcting the obsolete water-runtime startup assertion. Both fixes were already identified on MisSpec.
- Ordinary application launch, without `--smoke` or test fixtures. Primary adapter: NVIDIA GeForce RTX 3060 Laptop GPU. `Meta.Async.Queues=false`, cross-adapter and water offload disabled, Native output and Frame Generation disabled.
- Window displayed animated procedural planes and grass; application exited normally with code 0. Runtime log contains one GPU adapter initialization and no `[ERROR]` entries.
- This is the procedural scene constructed in `MetaApplication::Initialize` from `Configs/SmokeWorld.yaml`. It does not load the Amazon Lumberyard Bistro assets used by `Apps/App/src/App.cpp`. Reusing rendering techniques does not mean that the Bistro scene is integrated.
- No numerical comparisons, edge-case suites, profiling or performance measurements were run. This check establishes only that the current scene renders on the selected DIRECT path.

Local artifacts: `Build/Verification/meta-local-direct/{scene.png,runtime.log,effective-startup.ini,build.log}`. The generated Startup.ini override was restored after the run; tracked defaults were not changed to DIRECT.
