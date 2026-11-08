# MGPU: интеграция первой волны

2026-11-08. **P0/P1/P2a/P3a интегрированы и совместно проверены.** Worktree: `D:\Project\PEPEngine2\.tmp\mgpu-feature-2026-11-08\integrator`, branch `codex/mgpu-integrator`. Основной checkout не изменялся; push не выполнялся.

## Принятые commits и границы

| Пакет | Source commit | Commit в integrator | Отчёт |
| --- | --- | --- | --- |
| P0 | `79c6dc3fde0688715c3669375b09b71c7bd85b16` | тот же | [контракт/baseline](2026-11-08-mgpu-p0-contract.md) |
| P1 | `31a1cf29710994bc6d02b8506931f1a8ee55cf9d` | `a237b2bff6349cee40e2f53530f181c2eaaefd3c` | [очереди](2026-11-08-mgpu-p1-queues.md) |
| P2a | `1eb0b1487abc85c012315963da5b0f99373d5c6f` | `fc54e765d1d3119c05df261ad51c8eee160741e5` | [compiler](2026-11-08-mgpu-p2a-compiler.md) |
| P3a | `babda879ace09907b6a556eb11c62f2de0116b81` | `0de278a04d25729554a85f45fa5e056b75f0edc5` | [snapshot model](2026-11-08-mgpu-p3a-snapshots.md) |

Все cherry-picks без конфликтов. Task-file содержимое совпадает с принятыми source commits; после P1 совпало всё дерево. Промежуточный doc-only commit `559a020992b47d330494374609f7894ed564734c` записал статическую интеграцию P1 без workloads, пока lease принадлежал workers.

Интегратор добавил только регистрацию `Include\Engine.RendererDX12\GDX12SnapshotProtocol.h` / `src\GDX12SnapshotProtocol.cpp` в `Engine.RendererDX12.vcxproj` и соответствующие Header/Source Files filters. Standalone `MGPUSnapshotProtocolTests.vcxproj` сохранён: он компилирует protocol cpp напрямую, **не линкуется с renderer** и не смешивает fake глобальный queue issuer с настоящим `GDX12CommandQueue`. Новый protocol cpp компилируется также как production renderer member; model test cpp в production не добавлен. Solution/runtime/pipelines не менялись.

## Review/test fixes принятых пакетов

- P1 сохранил сигнатуры P0 и старых callers. Completion points выдаются после queue Signal, imports сохраняют producer provenance, allocator retirement не доверяет внешнему `FenceValue`. Shared fence export остаётся явным opt-in. Dormant legacy raw-fence rendezvous не включён.
- P2a сохраняет внешний Exact fallback в `CompiledPlan::Inputs`, включая conditional-only consumer; проверка не маскируется вторым прямым reader. Разные версии, записываемые одним pass в одну allocation, отклоняются. План содержит symbolic batch references, не выдуманные CompletionPoints; legacy Execute отвергает новые explicit plans до P2b.
- P3a исправил найденный при model test starvation: optional imports сохраняют newest completed compatible candidate до выбора render frame. Exact reservation сохраняет приоритет. Первоначальный failure, исправление и unchanged rate fixtures описаны в отчёте P3a.
- Дополнительных production fixes в интеграции не потребовалось. Scoped diff checks прошли.

## Совместные проверки

Перед **каждым** workload повторно прочитан `D:\Project\PEPEngine2\.tmp\mgpu-feature-2026-11-08\validation-lease.json`: `owner=Integrator`, `state=granted`, точный integrator worktree. Все builds/tests/generator запускались последовательно через peengine-perf `run_job.py`, с timeout, отдельными stdout/stderr и SHA256 fingerprints до/после. Lease не изменялся. Runner timings здесь не используются как performance evidence.

Все paths ниже относительно integrator worktree; `.stdout.log` / `.stderr.log` имеют тот же basename, что `.json`:

| Workload | Результат | State |
| --- | --- | --- |
| Debug/x64 RenderGraphTests build | PASS | `Build/Integration/build-graph.json` |
| Debug/x64 MGPUQueueTests build | PASS | `Build/Integration/build-queue.json` |
| Standalone Debug/x64 snapshot build | PASS, `/W4 /WX` | `Build/Integration/build-snapshot.json` |
| Обычный Debug/x64 App build, все production callers | PASS; приложение не запускалось | `Build/Integration/build-app.json` |
| Snapshot CPU suite | PASS | `Build/Integration/snapshot-cpu.json` |
| Graph `--compiler-only` | PASS, без GPU fixture | `Build/Integration/compiler-cpu.json` |
| Queue `--gpu-validation --require-two-adapters` | PASS, physical two-GPU обязательна | `Build/Integration/queue-gbv.json` |
| Graph `--gpu-validation` | PASS, clean debug layer | `Build/Integration/graph-gbv.json` |
| Compile database generation | PASS, 36 projects / 127 translation units; ignored local output | `Build/Integration/generate-compdb.json` |

Queue GBV подтвердил single-device DIRECT→COMPUTE→COPY→DIRECT, provenance/negative commands/retirement, положительный COMPUTE SetPipelineState1, и physical adapters NVIDIA RTX 3060 Laptop `LUID 0:62942` → AMD Radeon Graphics `LUID 0:57700`: cross-adapter Signal/Wait, 24 payload generations, stale imports, bounded handles. Итог: `Physical two-GPU=PASS`, clean D3D12 debug layer.

Graph GBV подтвердил compiler HAO/independent invocations/version/alias/FIFO cases вместе с прежними pooling/resize/DIRECT fallback, multiple async producer RAW/WAR/WAW, UAV texture prefix/alias/retirement/failure checks и **400 changing frames concurrent texture readers/readbacks**. CPU compiler-only завершился до GPU initialization. Оба GPU stderr пусты.

Snapshot CPU suite повторил все три 420-tick rate fixtures: producer/consumer 1/5, 7/1, 2/3; selected/repeated — 83/27, 410/351, 132/104; first→last invocation — 3→392, 1→59, 1→191. Это bounded ownership/liveness/payload model checks, не GPU transport и не benchmark.

App build выдал один C4244 diagnostic (с continuation lines): int→float через `std::make_unique` в неизменённом `BistroBounds.h:90`; ошибок сборки нет. Эта строка не исправлялась вне области MGPU. Остальные targets не выдали compiler/linker warnings/errors.

Worker Release receipt P3a принят без повтора Release: protocol header/cpp и test cpp **побайтно** совпали с `p3a/Build/P3a/release-build.json`; project text идентичен после LF/CRLF normalization. Первоначальное raw-project hash расхождение сохранено в `Build/Integration/worker-release-source-check.json`, доказательство только line endings — в `worker-release-source-equivalence.json`. C++/project semantics не менялись.

`Build/Integration/validation-summary.json` фиксирует все 9 successful exit states и повторную сверку их fingerprints с текущими inputs. Builds имели 21–22 fingerprints, CPU/GPU tests — 23, generator — 3; изменений validated inputs нет.

### Точные команды и setup

Для восьми основных workloads полные команды, cwd, lease snapshot, timeout и fingerprint paths записаны в `Build/Integration/<job>.command.json`; это exact executed argv, включая runner. `Build/Integration/run_validation.ps1` — ignored task-local launcher с проверкой lease перед каждым запуском. У всех builds `/m:1 /nr:false`, timeout 900; tests timeout 300, runner heartbeat 25.

MSBuild: `C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe`. Общие build flags: `/p:Configuration=Debug /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /nr:false /nologo /v:minimal`. Projects: `Apps/App/RenderGraphTests.vcxproj`, `Apps/App/MGPUQueueTests.vcxproj`, `Apps/App/MGPUSnapshotProtocolTests.vcxproj`, `Apps/App/App.vcxproj`.

Graph/queue/App builds дополнительно использовали `/p:ForceImportBeforeCppTargets=D:\Project\PEPEngine2\.tmp\mgpu-feature-2026-11-08\integrator\Build\P0\Validation.targets`. Этот **локальный validation-only `/Z7` override** заменяет `/Zi` (включая per-file flags), отключает linker PDB и использует проверенную локальную копию Jolt archive вместо пересборки dependency. Tracked optimization/CRT/defines не менялись. Standalone snapshot project сам задаёт `/Z7`, без этого override. Dependency copies и причина PDB workaround — в P0 report; outputs общих dependencies не менялись.

Test executable paths: `Build/bin/x64/Debug/<target>/<target>.exe`, cwd — его output directory. Snapshot — без аргументов; RenderGraphTests — `--compiler-only` и отдельно `--gpu-validation`; MGPUQueueTests — `--gpu-validation --require-two-adapters`. Только output fixture INI у graph/queue содержит `[ConsoleVariables]` без настроек, чтобы избежать известного unknown `App.Water.HeightScale`. Original output configs сохранены (`Build/P0/Engine.original.ini`, `Build/Integration/queue.Engine.original.ini`); исходный `Config/Engine.ini` не менялся. Runner fingerprinted executable/fixture INI вместе с source inputs.

Compile database обновлена командой `python tools/Generate-CompileCommands.py --msbuild "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe"` через тот же runner (`--cwd . --state Build/Integration/generate-compdb.json --timeout 180 --heartbeat 25`, fingerprints renderer/queue/snapshot projects). Она уже выполнялась к моменту запрета ведущего на новые workloads; после этого новых запусков/повторов не было. Общий Serena project не переактивировался.

## Конфигурация, передача и следующие границы

Реальные MGPU techniques пока **не wired**; новых technique CVars и no-op switches нет. Existing async CVars `Render.Fluid.AsyncCompute`, `Render.Grass.AsyncCompute`, `Render.Particles.AsyncCompute`, `Render.AO.AsyncCompute` сохраняют `false`. При дальнейшем подключении каждая MGPU technique switch обязана иметь `false` default; correctness fixtures включают её явно и восстанавливают default при teardown. Автоматической смены качества/частоты нет.

`Build/Integration/processes-final.json`: **`OwnWorkloadCount=0`, `ChecksComplete=true`**. Все девять runner child roots завершились; Win32 Toolhelp audit проверил parent relationships и новые workload processes относительно сохранённого baseline. Audit только читает процессы; PID **30988** явно исключён, чужие процессы не останавливались. Слот можно передать, lease остаётся у ведущего.

Следующая разрешаемая ведущим волна: P2b (P1+P2a) — native executor/barriers/queue-aware lifetime и binding symbolic batches к реальным points; P3b (P1+P3a) — DX12 snapshot transport, shared allocations/footprints/import-ready/release. Затем P4 — runtime/application wiring. Здесь эти пакеты и техники не начинались. Physical queue fixture не доказывает native snapshot transport или полный MGPU runtime. Не запускались Bistro, полный IBL suite, soak, captures или performance benchmarks; **performance claim отсутствует**.
