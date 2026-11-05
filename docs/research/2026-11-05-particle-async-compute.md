# Particle Async Compute: первая реализация

Дата: 2026-11-05. Первичная реализация и исходные измерения относятся к `4b5097e` поверх `e173c28`, direct-разделение принято на `9411599`. Повторная GBV-проверка после исправления SSSR и последующий shared-depth путь описаны отдельно ниже. Исходные A/B samples и captures относятся к первоначальному conservative join.

`Render.Particles.AsyncCompute=false` остаётся значением по умолчанию. При ручном включении только simulation выполняется на COMPUTE; initialization/reset до графа и поздние cull/sort/draw/composite остаются на DIRECT. Неудачная инициализация COMPUTE и dual-GPU режим сохраняют DIRECT scheduling. Shader path, controls и текущая opaque prepass depth для collision сохранены.

## Первичный консервативный путь (`4b5097e`)

В `4b5097e` из [плана](../tasks/async-compute-rollout.md) реализованы particle-only очередь, physical buffer accesses, GPU fork/join, retirement, fallback, diagnostics и A/B harness. Совместное чтение одной depth texture из GBuffer и COMPUTE тогда было **отложено**. Первичная схема консервативно держала texture ownership на одной очереди до join:

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
| `BistroRenderGraphTests --gpu-validation` | Исходный run остановился на SSSR stride error; повтор после SDK patch — exit 0, все 18 HDR cases и заключительные SDK effects/resize/Native→XeSS прошли, см. follow-up ниже |
| `BistroRenderGraphTests --timing` | Exit 0; 360 measured samples после прогрева |

Particle fixtures проверяют независимые position/age/lifetime и collision oracles, hidden simulation/expiry, pause/master disable, pending Emit/Reset, quality/capacity/removal, три blend modes, mesh fireflies, точный refill 250000/1000000 и queued DIRECT/async/DIRECT кадры с simulation-only участками. Recording-failure fixture проверяет завершение уже отправленного COMPUTE до явных cleanup flushes вызывающего кода.

**Исходный сбой полного Bistro GBV в `4b5097e`:** `SSSR_SpdAtomicCounter`, DIRECT `SSSR-DEPTH_DOWNSAMPLE`: UAV `StructureByteStride=1`, shader ожидает 4. В локальном FidelityFX SDK `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55` описание counter в `sdk/src/components/sssr/ffx_sssr.cpp` задавало stride 1, а `ffx_sssr_callbacks_hlsl.h` объявлял `RWStructuredBuffer<FfxUInt32>`. В первичной async-реализации SDK не менялся, и первоначальный run не является полностью прошедшим. Исправление и чистый повтор зафиксированы ниже.

Логи: `Build/async-baseline-{cpu,gpu-configured,gbv-configured}.log`, `Build/async-final-root-build.log`, `Build/async-final-graph*.log`, `Build/async-final-second-*.log`, `Build/async-final-bistro-{debug,gbv}.log`, `Build/async-final-timing.log`. Ранние shared-read прогоны и native repro находятся под `Build/AsyncComputeVerify-9411599/Build/async-*.log`; они не являются приёмкой финального пути.

Сборка использовала MSBuild 18.8.2, Release/x64, `BuildProjectReferences=false`, `_CL_='/Z7 /GL-'` и существующий `Build/ParticleSplit.targets`, отключающий linker PDB. Это обход локального C1902 toolset/PDB mismatch. Сохранились существующие conversion и third-party warnings. Отдельного lint runner для C++ здесь не запускалось; task-scoped `git diff --check` проверен.

## Исправление SSSR counter и повторный GBV, 2026-11-05

Pinned SDK задавал SPD counter как buffer размером 1 byte, stride 1 и initialization size 1. `FfxInternalResourceDescription::width/height` означают для buffers size/stride; DX12 backend переносит stride в UAV, тогда как HLSL читает `RWStructuredBuffer<FfxUInt32>`. [Патч](../../External/FidelityFX/sssr-spd-counter.patch) заменяет все три значения на `sizeof(uint32_t)`: один полностью инициализированный 4-byte element. Общий DX12 backend и shader path не менялись. Патч зарегистрирован в build script и MSBuild inputs, проверен на чистом pinned archive и обратной проверкой уже применённого SDK.

В `SSRTests` добавлен `--gpu-validation`: до patch он воспроизвёл точный stride 1/4 error; после пересборки SDK прошёл с exit 0 и без debug/GBV warnings/errors. Проверены hits/HDR/fallback/roughness/history/Off/resize/jitter/motion/frame slots. Этот test проверяет настоящий SDK dispatch, но сам не проверяет particle scheduling.

**Полный `BistroRenderGraphTests --gpu-validation` завершился с exit 0**, без D3D12/GBV errors: 18 HDR cases (две reference readbacks и 16 byte-identical сравнений), DIRECT/async/DIRECT, pooling off/on/off, обе AO placements, затем все effects, odd resize и Native→XeSS с включённым particle async. Тест не ослаблялся: добавлены отметки заключительных стадий и дополнительная проверка messages после resize до переключения XeSS. Проверка текущего консервативного async-пути на RTX 3060 Laptop закрыта; shared-depth read/read scheduling остаётся отложенным.

Один повтор был прерван до окончания и не считается приёмкой. Финальная XeSS GBV-стадия заняла несколько минут; stack snapshot активного CPU worker показал `nvgpucomp64`/`nvwgf2umx` (NVIDIA shader compilation). Окно в этот момент может не отвечать, поэтому отсутствие UI response само по себе не означает зависание GPU fence.

Логи follow-up: `Build/sssr-stride-red-build.log`, `Build/sssr-stride-red.log`, `Build/sssr-stride-fix-build.log`, `Build/sssr-stride-green-gbv.log`, `Build/sssr-stride-bistro-rebuild.log`, **`Build/sssr-stride-bistro-gbv-complete.log`**. `Build/sssr-stride-bistro-gbv.log` относится к прерванному повтору. Обычный SSR test раньше не включал GBV, поэтому buffer/shader stride mismatch оставался незамеченным. A/B samples и captures ниже сняты до этого SDK patch и в follow-up не переснимались.

## Совместное чтение depth: follow-up 2026-11-05

Этот раздел заменяет ограничение exclusive-depth пути выше. DIRECT публикует после prepass `DEPTH_READ | PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE`; COMPUTE не меняет depth state. На время ветки `BeginConcurrentRead` сохраняет опубликованное объединение во всех resource helpers и отклоняет запись, UAV barrier, release в COMMON и стороннюю смену состояния. После GPU join ограничение снимается. Совместное чтение разрешено для явных read-accesses того же physical state owner; writer, unmanaged access, несовместимый state или другой state owner требуют join. Буферные hazards, whole-graph texture lifetimes, exception drain и final DIRECT retirement сохранены.

GBuffer использует read-only DSV и `DEPTH_READ`, включая grass EQUAL/no-write. View пересоздаётся после resize через существующий descriptor API. AO/CACAO и deferred lighting явно объявляют SRV reads и сохраняют общее состояние. В Bistro join сдвинут с GBuffer/early AO на **`Reflection probe meshes`**, первый объявленный depth writer после GBuffer, AO и lighting. Cull/sort/draw и остальные техники остаются DIRECT; новых GPU-копий, CPU waits и задержки на кадр нет. `Render.Particles.AsyncCompute=false` и DIRECT fallback не менялись.

### Runtime и воспроизводимость

Системный `D3D12Core.dll` **10.0.26100.9549** давал `Non-simultaneous-access Texture Resource ... read|write|transition_barrier` и на native fixture без graph, pooling, timer и recycling. Создание новых command lists внутри каждого кадра часто скрывало конфликт за CPU-задержкой. После предварительного создания всех списков ошибка воспроизводилась в 400 queued frames и с legacy read union, и с Enhanced Barriers. Поэтому ранние нулевые результаты медленного repro не доказывали корректность overlap.

Тот же native prepared-list repro с **Agility SDK 1.619.6** завершился без ошибок и с правильными меняющимися данными в обоих readback buffers. Разница связана с версией runtime/debug layer; причину в GPU driver этот опыт не устанавливает. Новая реализация использует обычные legacy barriers и read-state union, без нового Enhanced Barriers API. Synchronized command queue validation сохраняет стандартное включённое состояние.

[Подготовка Agility](../../External/AgilitySDK/README.md) закреплена в публичных renderer MSBuild props: SHA256-проверенный NuGet archive в `Build/Dependencies`, version export **619**, runtime/debug layer/state-object compiler/license в `D3D12` рядом с каждым EXE. Этот каталог нужен и для поставки приложения с DIRECT по умолчанию. Установка системного runtime не меняется. Для первого скачивания нужен интернет; проверены свежая распаковка и повторная подготовка через Windows PowerShell, deployment x64 и Win32.

Новый runtime сообщает SM 6.9, а комплектный DXC **1.8.2505.32** создаёт для этого target preview shader. Red particle run остановился на `CreateComputePipelineState` с unsigned/preview shader. Engine target ограничен retail **SM 6.8**, как на прежнем системном runtime; compiler и shader algorithm не обновлялись.

Диагностика: `Build/depth-share-probe-prepared-1050612.log` — старый runtime, 400 frames, ownership errors при правильных readbacks; `Build/depth-share-probe-agility619-{1048832,1050612,1048584}.log` — prepared-list color/depth variants, errors=0 и bad_readbacks=0. Первые эксперименты с Agility 1.620.1 не являются выбранной зависимостью: 1.619.6 устраняет исходную проблему без миграции feature-query API.

### Проверки финального shared-depth пути

Финальные проверки Release/x64 выполнены с Agility 1.619.6; логи используют префикс `Build/depth-share-619-`.

| Проверка | Результат |
| --- | --- |
| RendererDX12, App.Base, App, RenderGraphTests, SecondGroupTests, BistroRenderGraphTests, SSRTests | Сборки успешны; тот же локальный `/Z7 /GL-` и `ParticleSplit.targets` workaround |
| RenderGraphTests: native / Streamline, с GBV и без GBV | Exit 0; 400 меняющихся кадров, оба readback buffers, guard против сужения/записи/release, writer/unmanaged joins, fallback, retirement и clean debug layer |
| SecondGroupTests: CPU, DIRECT, async, DIRECT GBV, async GBV | Все пять вариантов exit 0; queued toggles, hidden simulation, expiry/refill, collision, controls и frame slots сохранены |
| SSRTests --gpu-validation | Exit 0, строгая проверка warnings/errors; SDK hits/HDR/fallback/history/resize/jitter/motion/frame slots прошли |
| BistroRenderGraphTests | Exit 0; 18 HDR cases, восемь активных AO cases, odd resize и Native→XeSS |
| **BistroRenderGraphTests --gpu-validation** | **Exit 0**; те же 18 HDR cases и восемь active AO cases, все эффекты, odd resize и Native→XeSS с async, без D3D12/GBV errors |

В обеих Bistro-проверках HDR identity проверяется при pooling off/on/off и DIRECT/async/DIRECT в обеих AO placements. SSAO/HBAO/XeGTAO/CACAO дополнительно исполняются по четыре кадра в каждой позиции с активным async и проверкой join перед `Reflection probe meshes`. HDR byte-identity не заявляется для этих восьми дополнительных AO cases. Заключительный GBV запущен в 03:16:34 и завершён в 03:27:10 по Москве. Медленная XeSS-стадия вновь показала активный NVIDIA shader compiler в stack snapshot (`Build/depth-share-xess-stacks.log`); процесс завершился самостоятельно.

Code review: targeted manual due to unrelated branch work. Проверены state-owner lifetime, совпадение деклараций и локальных barriers, read-only DSV при resize, fallback/default, runtime deployment и SM 6.8. Task-scoped `git diff --check` чистый. Параллельные physics/Bistro правки не входят в этот follow-up.

Свежие A/B timings и GPU captures для расширенного окна не снимались; ускорение не установлено. Другие GPU/драйверы, Win32 GPU execution, реальный dual-GPU, принудительный отказ создания COMPUTE, все SR/FG providers и длительный soak остаются непроверенными.

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
