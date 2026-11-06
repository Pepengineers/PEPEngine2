# Agent tooling trial — 2026-11-07

Installed the global `bounded-tool-output` skill, Serena 2.1.0, RTK 1.1.0, and
ccusage 20.0.48. The separate `peengine-perf` task was not changed.

## Configuration

- Skill source: `tools/agent-skills/bounded-tool-output/SKILL.md`; installed copy:
  `C:/Users/Asus/.codex/skills/bounded-tool-output/SKILL.md`.
- Serena: installed with uv in the user profile using the existing Python 3.14.
  Registered as global Codex MCP server `serena`, stdio, `--context codex`,
  `--agent-interface repl`, `--project-from-cwd`. Dashboard/browser and GUI log
  windows are disabled. Startup timeout is 60 seconds; client tool timeout is
  300 seconds (Serena's own default is 240 seconds).
- `.serena/project.yml` configures C++ and excludes generated/vendor trees from
  project searches. Compiler include paths still resolve dependencies there.
- Serena's bundled clangd 19.1.2 failed the installed MSVC STL version check
  (`STL1000: expected Clang 20 or newer`). Installed official clangd 23.1.0 at
  `C:/Users/Asus/.local/share/clangd/clangd_23.1.0/bin/clangd.exe` and selected it
  in the ignored `.serena/project.local.yml` under `ls_specific_settings.cpp.ls_path`.
- RTK: `C:/Users/Asus/.local/bin/rtk.exe`; user PATH includes this directory.
  Installed for explicit trial commands; automatic rewrite hooks are not enabled.
- ccusage: installed in the existing user npm prefix, without install scripts.

New MCP/skill discovery and the changed PATH may require a new Codex session or
application restart. MCP behavior was tested directly through its stdio protocol;
availability inside an already-running Codex chat was not established.

## Regenerate the C++ database

```powershell
python tools/Generate-CompileCommands.py
python tools/tests/test_compile_commands.py
```

The exporter evaluates the 31 first-party Visual Studio projects with MSBuild
without building targets. The initial Debug/x64 database contains 121 unique
translation units, with evaluated includes, definitions, language standard,
runtime-library flags and forced includes. Shared sources prefer production
entries over test variants. This is a navigation database for one configuration,
not a replacement for the MSVC build. Regenerate after project settings or source
lists change. `--configuration Release` selects Release instead.

The generated root `compile_commands.json`, clangd cache and local Serena files
are ignored. Portable `.serena/project.yml` is tracked. HLSL is not indexed by
this C++ configuration. On another machine, install compatible clangd and set
its local path; the user-specific path is intentionally not committed.

## Observed checks

| Check | Result |
| --- | --- |
| Skill validator and installed/source comparison | Passed |
| Exporter test | Preserved spaced arguments, runtime/standard/include/define/forced-include flags; skipped excluded source; rejected unresolved expressions and missing source |
| Real MSBuild export | 31 projects, 121 translation units |
| clangd 23 `--check` | `GDX12CommandQueue.cpp` and `GDX12TextureResource.cpp`: exit 0 |
| Serena class overview | 378 characters |
| Serena `GDX12CommandQueue::Flush` body | 455 characters, about 0.10 s in the tested cached session; whole source file is 5,195 characters |
| Serena references to `GDX12CommandQueue/Flush` | 135 references in 30 files; all returned line positions checked against source; both `GDX12DepthOfFieldPass.cpp` calls found |
| First reference query | 192.86 s; 41,309-character detailed result reduced by Serena to a 10,291-character location list at the requested limit |
| Registered REPL interface | Initialization and symbol-body query passed through MCP |
| Bounded tool discovery example | Same 15 tool names: 542 characters instead of 23,909 characters of full metadata; schema retrieval remains a separate step |
| RTK `git log -10` | stdout 2,379 → 1,365 bytes (42.6% smaller) |
| RTK C++ file read | stdout 5,357 → 5,195 bytes (3.0% smaller) |
| RTK failing test fixture | Failure text retained; exit code 7 retained |
| ccusage offline report | November 5–6, Europe/Moscow, JSON with costs disabled; daily accounting identities passed |

Read-only trial artifacts are under ignored `Build/AgentTools-20261107/`.
Large reference lookups remain expensive; scoped symbol queries are the strongest
observed Serena benefit. Returned references were checked, but an exhaustive
cross-configuration reference oracle and an end-to-end task-token A/B were not run.
Character/byte reductions above are not measured token or subscription savings.
ccusage's model fallback labels are estimates, not proof of the backend model.

Useful explicit trial commands:

```powershell
rtk git log -10
ccusage codex daily --offline --no-cost --timezone Europe/Moscow
```

Sources: [Serena installation](https://oraios.github.io/serena/02-usage/010_installation.html),
[C++ setup](https://oraios.github.io/serena/03-special-guides/cpp_setup.html),
[Codex MCP configuration](https://learn.chatgpt.com/docs/extend/mcp?surface=cli),
[RTK](https://github.com/rtk-ai/rtk),
[ccusage Codex support](https://github.com/ccusage/ccusage/blob/main/docs/guide/codex/index.md).
