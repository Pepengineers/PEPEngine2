# Particle Async Compute: первая реализация

Дата: 2026-11-05. Реализация и проверки относятся к исходникам этого коммита поверх `e173c28`, direct-разделение принято на `9411599`.

`Render.Particles.AsyncCompute=false` остаётся значением по умолчанию. При ручном включении только simulation выполняется на COMPUTE; initialization/reset до графа и поздние cull/sort/draw/composite остаются на DIRECT. Неудачная инициализация COMPUTE и dual-GPU режим сохраняют DIRECT scheduling. Shader path, controls и текущая opaque prepass depth для collision сохранены.

## Уточнение исходного плана

Из [плана](../tasks/async-compute-rollout.md) реализованы particle-only очередь, physical buffer accesses, GPU fork/join, retirement, fallback, diagnostics и A/B harness. Совместное чтение одной depth texture из GBuffer и COMPUTE **отложено**. Итоговая схема консервативно держит texture ownership на одной очереди до join:

1. DIRECT выполняет `InitializePools`, полный opaque depth prepass и переход depth в `NON_PIXEL_SHADER_RESOURCE`; persistent particle buffers выпускаются через COMMON.
2. COMPUTE ждёт fence DIRECT, выполняет prepare/simulate/spawn/publish, выпускает buffers через COMMON и сигналит собственный fence.
3. DIRECT может выполнять не использующие эту texture тени/probes. Любое последующее обращение DIRECT к той же физической texture, включая чтение, сначала делает GPU join. В измеренной late-AO конфигурации join стоит перед GBuffer; early AO сокращает окно до своего первого depth access.
4. Также проверяются логические зависимости и physical buffer RAW/WAR/WAW. Без draw-потребителя выполняется обязательный join перед возвратом из графа.
5. Caller отправляет возвращённый tail-list. Его DIRECT fence защищает frame slots, uploads, descriptors и persistent pools транзитивно после COMPUTE. Allocator/list и timestamp slots COMPUTE используют собственный fence. Следующий DIRECT prefix следует за предыдущими graphics readers; дополнительного кадра задержки симуляции нет.

Async texture inputs и их fallback sets исключены из линейного transient reuse консервативным whole-graph lifetime. Нет новых копий depth/particle state, read-only DSV изменений, глобального изменения resource helpers, CPU wait на fork/join или запрета recycling списков. Имеющиеся CPU frame-slot/timestamp waits сохраняются.

Причина отклонения: конкурентный read/read текстуры давал `Non-simultaneous-access Texture Resource ... still referenced by read|write|transition_barrier ... on another Command Queue`. Ошибка воспроизвелась и на native D3D12 без графа, pooling, таймеров и повторного использования allocator/list: DIRECT clear → read transition → signal; COMPUTE wait/read/signal; DIRECT read/wait → следующий producer. В 400-кадровом native опыте новые queues/fences дали 6 сообщений; существующие queues с новыми fences — 396. Это **не устанавливает** причину в драйвере или debug layer. Совместное чтение не включено в принятую реализацию. Групповой retirement, дополнительные COMMON transitions, изменение fence flags и отключение recycling не дали доказанного исправления и удалены.

## Проверки

Сначала повторно приняты direct CPU/GPU/GBV particle fixtures. Финальные проверки выполнялись на основной рабочей копии, а не только на диагностическом source snapshot.

| Проверка Release/x64 | Результат |
| --- | --- |
| Core, RendererDX12, App.Base и три test projects | Сборки успешны |
| `RenderGraphTests`, затем `--streamline` | Exit 0, чистый debug layer; 400 texture ownership handoffs, buffer hazards, fallback, queued submissions, missing draw consumer, final fence и catch-drain после ошибки записи |
| `SecondGroupTests --cpu` | Exit 0 |
| `SecondGroupTests`, `--async` | Exit 0, чистый debug layer |
| `SecondGroupTests --gpu-validation`, `--async-gpu-validation` | Exit 0, без GBV/debug errors |
| `BistroRenderGraphTests` | Exit 0, чистый debug layer; 18 HDR cases (две reference readbacks и 16 byte-identical сравнений), pooling off/on/off, DIRECT/async/DIRECT, обе AO placements, odd resize и Native/XeSS switch |
| `BistroRenderGraphTests --gpu-validation` | Все 18 HDR cases прошли без ошибок; финальная стадия с включёнными SDK effects завершилась ошибкой SSSR, см. ниже |
| `BistroRenderGraphTests --timing` | Exit 0; 360 measured samples после прогрева |

Particle fixtures проверяют независимые position/age/lifetime и collision oracles, hidden simulation/expiry, pause/master disable, pending Emit/Reset, quality/capacity/removal, три blend modes, mesh fireflies, точный refill 250000/1000000 и queued DIRECT/async/DIRECT кадры с simulation-only участками. Recording-failure fixture проверяет завершение уже отправленного COMPUTE до явных cleanup flushes вызывающего кода.

**Ограничение полного Bistro GBV:** `SSSR_SpdAtomicCounter`, DIRECT `SSSR-DEPTH_DOWNSAMPLE`: UAV `StructureByteStride=1`, shader ожидает 4. В локальном FidelityFX SDK `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55` описание counter в `sdk/src/components/sssr/ffx_sssr.cpp` действительно задаёт stride 1, а `ffx_sssr_callbacks_hlsl.h` объявляет `RWStructuredBuffer<FfxUInt32>`. Эти SDK-файлы и SSSR production pass в этой задаче не менялись. Ошибка сохранена; тест не ослаблялся и не объявляется полностью прошедшим. Отдельный чистый baseline этого полного GBV SDK-run не получен.

Логи: `Build/async-baseline-{cpu,gpu-configured,gbv-configured}.log`, `Build/async-final-root-build.log`, `Build/async-final-graph*.log`, `Build/async-final-second-*.log`, `Build/async-final-bistro-{debug,gbv}.log`, `Build/async-final-timing.log`. Ранние shared-read прогоны и native repro находятся под `Build/AsyncComputeVerify-9411599/Build/async-*.log`; они не являются приёмкой финального пути.

Сборка использовала MSBuild 18.8.2, Release/x64, `BuildProjectReferences=false`, `_CL_='/Z7 /GL-'` и существующий `Build/ParticleSplit.targets`, отключающий linker PDB. Это обход локального C1902 toolset/PDB mismatch. Сохранились существующие conversion и third-party warnings. Отдельного lint runner для C++ здесь не запускалось; task-scoped `git diff --check` проверен.

## A/B timing

RTX 3060 Laptop GPU (GA106, 6144 MiB), driver 617.42; Ryzen 7 5800HS, Windows 11 build 26300. Bistro, 1280×800 Native, FG off, camera и dt=0 фиксированы, 250000 live particles, исходный seed/capacity восстанавливаются перед каждой серией. Late AO, остальные техники из `Config/Engine.ini`, nominal frames in flight 3. На серию 24 warm-up + 120 measured frames. Перед чтением timestamp результата каждый measured frame завершается CPU drain; это отдельный режим измерения, а не тест нормального pipelining/FPS.

| Серия | GPU median, ms | GPU p95, ms | GPU stdev, ms | CPU submit median, ms |
| --- | ---: | ---: | ---: | ---: |
| DIRECT до | 8.676 | 8.947 | 0.143 | 5.054 |
| Async | 8.741 | 8.891 | 0.130 | 6.209 |
| DIRECT после | 8.970 | 9.835 | 0.523 | 5.017 |

COMPUTE simulation interval: median **0.299 ms**, p95 **0.310 ms**. Полный GPU interval измеряется DIRECT timer от начала кадра до конца графа, включая queue waits, без presentation/FG. COMPUTE duration использует частоту своей очереди; эти два таймера сами по себе не дают общей временной шкалы.

Async median на 0.75% выше первой DIRECT серии; две DIRECT серии различаются на 3.39%. CPU submit p95 составляет 10.408 / 8.537 / 47.805 ms. При таком дрейфе и единичном запуске ускорение не установлено. Фиксация dt=0 делает этот опыт воспроизводимым steady workload, но не заменяет измерение движущейся сцены, expiry/spawn и thermal soak.

[Все raw samples](2026-11-05-particle-async-samples.csv), [исполненный async graph с physical buffer identities и fence values](2026-11-05-particle-async-graph.dot). Профилированные GPU Trace кадры не смешиваются с этим CSV.

## GPU timelines

Парные GPU Trace captures записаны Nsight Graphics 2026.4.0, `Throughput Metrics`, `--time-every-action`, duration 200 ms, clocks unaltered, validation off:

- DIRECT: `Build/async-timelines/direct/2026-11-05-01-41-23-090/BistroRenderGraphTests_2026_11_05_01_41_57.ngfx-gputrace`, 17 кадров, только DIRECT, 125 actions/frame.
- Async: `Build/async-timelines/async-numeric/2026-11-05-01-44-51-202/BistroRenderGraphTests_2026_11_05_01_45_23.ngfx-gputrace`, 18 кадров, 85 DIRECT + 40 COMPUTE actions/frame. Восемь emitters × пять simulation stages соответствуют 40 перенесённым actions; draw stages остались на DIRECT.

Native GTAPI открыл оба файла. Все **18 из 18 COMPUTE command-buffer timestamp intervals** полностью лежат внутри независимых DIRECT intervals; сумма пересечений равна всей сумме COMPUTE intervals, 6.175 ms. Первый кадр относительно trace start: DIRECT prefix 0–1.477 ms, independent DIRECT 1.488–3.934 ms, COMPUTE 1.566–1.906 ms, DIRECT tail 3.948–8.658 ms. Это подтверждает перекрытие интервалов очередей, но не доказывает одновременный issue инструкций на SM. [Машиночитаемое evidence](2026-11-05-particle-async-timeline.json) содержит queue counts, интервалы и quality limits.

Оба captures предупреждают об отсутствующих per-shader metrics; выводов о shader occupancy/throughput по ним нет. В Release нет pass markers. CLI завершил ngfx с rc=0 и успешно экспортировал `BASE_UNLOCKED`; старая Python-обёртка ошибочно ожидала `BASE` и вернула failure. Rust analyzer отверг новый WRPV 11, поэтому timing/queues прочитаны через version-matched native GTAPI. Capture startup/trigger retries не смешивались с чистым timing CSV.

Baseline запущен через `gputrace-capture --start-after-hotkey`; async использовал `--start-after-frames 200`, после чего фактические COMPUTE queue/actions проверены в самом capture. Остальные флаги: `--architecture "Ampere GA10x" --metric-set-name "Throughput Metrics" --time-every-action --max-duration-ms 200 --set-gpu-clocks unaltered --args=--timing`. Host-доступ понадобился для attach к процессу/loopback. Перед профилированием runtime INI копировался из `Config/Engine.ini` в папку test executable.

## Остаточные проверки

Не проверялись другие GPU/драйверы, реальный dual-GPU adapter, принудительный отказ создания COMPUTE, все SR/FG providers и длительный soak. Параллельные physics/Bistro правки не входят в async-коммит. Остальные техники на COMPUTE не переносились; автоматической hardware policy нет. Включение async по умолчанию не предлагается.
