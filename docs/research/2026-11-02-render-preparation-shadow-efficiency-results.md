# Bistro: DEFAULT shadow inputs и подготовка до render graph

Задание: [render-preparation-and-shadow-efficiency.md](../tasks/render-preparation-and-shadow-efficiency.md).
Работа начата 2026-11-02. Ветка: `codex/render-preparation-shadow-efficiency`.

## Результат и границы приёмки

GPU-размещение дало воспроизводимый выигрыш без изменения HLSL, сцены или качества:
медиана полного кадра в трёх коротких Nsight запусках снизилась с **39,680 до 33,819 мс**,
local-shadow cull scopes — с **8,327 до 1,972 мс**. Финальный вариант с CPU preparation
и исправленным BeginFrame сохранил результат: **33,754 мс** за отдельное 12-frame окно.
Обычный запуск без присоединённого профилировщика дал среднее **32,1563 → 21,3029 мс**
на 180 кадрах после первого готового кадра. Это дополнительная проверка направления эффекта,
а не полноценный прогретый p95 benchmark.

CPU light grid и решения о dirty local-shadow views вынесены из Execute. Построение light grid
перекрывается с shadow visibility; зависимая подготовка начинается после Complete. Скорость
финального CPU-пути проверена 2026-11-03: девять независимых PIX captures, без потерь ETW.
Относительно DEFAULT до CPU-рефакторинга median/p95 записи графа уменьшились с
**4,100 / 6,997 до 3,188 / 4,280 мс**, подготовки вместе с записью — с
**5,171 / 8,261 до 4,125 / 5,472 мс**. Это сокращение CPU critical path;
дополнительный прирост FPS от CPU-этапа не подтверждён.

Приёмка закрыта на доступной RTX 3060 Laptop. FrameGenerationTests теперь проходит с
debug layer после каждого кадра; исправлены состояния входа и выхода общего tone-map pass.
Также прошли AO/DoF/motion-blur/SSR и повторные reflection/Bistro проверки. Ограничения
коротких GPU traces, неподдерживаемого DLSS-G и отсутствующей второй GPU сохранены ниже.

## Сопоставимая база

- Исходный HEAD: `8ae2ab0503333c92037a2c046ca386fe1df23e2c`, после merge `33ca359`.
- Profiling baseline: `471d276` — только opt-in scopes, без изменения алгоритма.
- DEFAULT: `8e6f342` — пять shadow DEFAULT mirrors на frame slot и staging templates.
- Lifecycle: `81fdb36` — настоящий Bistro Update вызывает BeginFrame; отдельная red/green регрессия.
- CPU preparation: `73d8066`; полные SHA также записаны в локальном
  `Build/Captures/2026-11-02-render-preparation/manifest.json`.
- Tone-map state correctness и per-frame FG debug regression: `7e10358`.

Свежие EXE и PDB собраны MSVC **14.51.36231**, v145, `Release|x64`, `/O2 /GL /MD /Zi`,
`PEPEnableProfiling=true` (`PROFILE_BUILD`, без `_DEBUG`). Runtime INI для App во всех сравнениях:

```ini
[ConsoleVariables]
Render.NumFrames = 3
```

RTX 3060 Laptop, драйвер 610.88, 1280×800, неподвижная камера у входа. Bistro simulation
продолжается: 346 benchmark props и бюджет 250000 частиц. Shadow resolution 256, lighting,
upscaler XeSS, эффекты и видимые casters не отключались. Для performance это одинаковый
динамический сценарий, но не одинаковые положения объектов в каждый момент: более быстрый
вариант достигает frame 180 раньше. Для correctness использованы фиксированные GPU fixtures.

| Артефакт | EXE SHA-256 | PDB SHA-256 |
| --- | --- | --- |
| baseline | `5A8B37CAC43A59B7C8EA766326E9255C1F5FDA331133FEC6F3FEAAF7B2321492` | `E6F410A11BD8A844F049450CB93F7D3EF5D8E2CD2997ECF3895344860B5373F2` |
| DEFAULT, runs 1/2 | `A8050919AFC757C33D5516DDD77419837ACAC456AB0FC4A9A40E3248ECD02EE1` | `1880779EEB6244656EDF29753DA822678CE42A210B86409B4AF313478DB207DE` |
| DEFAULT, run 3 | `6E6C9D62D1FD43F8867446FA429156C2B46A90532923E07F194A466F4321BB95` | `837D654A30223D87B8A29E6CF4EA30F00D6EAA26CE1365C80920A9CA9CD15F15` |
| final prepared | `449BC8D9498B8CAA0673E297686DC8BDCB5F82A4B9D66182ED2987530FB6C8AD` | `7D2BB1C88187618F9A6CEA4A98A8E2960CCB373642C985290979971B57B035FC` |
| final acceptance, 2026-11-03 PIX | `7121D886841F960CF80607EFEF09AEDD2E3FF26C8F7F707DA184AE9CFC8A02D2` | `7CF8C60ABF5159D87C045224F691A9B766FC6602179478F689268E02B3127680` |

DEFAULT run 3 пересобран после эквивалентного упрощения scalar CopyData через bulk overload;
исходные resource/index guards сохранены. Runs 1/2 относятся к предшествующему эквивалентному
варианту; отдельные hashes не смешаны с финальным EXE.

## GPU A/B

Native Nsight live schemas проверены перед сбором. В каждом независимом запуске:
trigger frame **180**, **12 frames**, Ampere GA10x, **Top-Level Triage**, PM bandwidth **1000**,
PC sampling off, terminate-after-session. GPC Frequency во всех проверенных запусках около **900 MHz**.
Это интервалы под Nsight с включёнными scopes, не обычный FPS и не p95 benchmark.

| Метрика | UPLOAD baseline, 36 кадров | DEFAULT, 36 кадров | final prepared, 12 кадров |
| --- | ---: | ---: | ---: |
| Full-frame median, мс | 39,680 | 33,819 | 33,754 |
| Full-frame min–max, мс | 36,269–50,251 | 33,222–34,522 | 33,141–34,473 |
| Local-shadow sequence median, мс | 16,873 | 10,201 | 10,137 |
| Clear scopes median/frame, мс | 0,503 | 0,511 | 0,504 |
| Cull scopes median/frame, мс | 8,327 | 1,972 | 1,957 |
| Draw scopes median/frame, мс | 5,153 | 5,148 | 5,112 |
| Texture-copy scopes median/frame, мс | 2,584 | 2,262 | 2,254 |
| New scene-upload scope median/frame, мс | — | 0,275 | 0,279 |
| Dirty views / cull dispatches per frame | 167–168 | 169–170 | 169–170 |
| PCIe throughput, % peak, whole trace | 11,38 / 15,25 / 12,65 | 6,993 / 6,999 / 6,999 | 7,003 |

Каждая dirty view по-прежнему записывает clear → cull → barriers → ExecuteIndirect;
dynamic views также копируют static depth. Scopes включают заключённые переходы/setup;
это не отдельный hardware shader-only counter. Число input templates осталось **24777**.
Небольшая разница views соответствует разным фазам движущейся сцены, а не уменьшению качества.

Медианы полного кадра по отдельным запускам:

| Run | Baseline, мс | DEFAULT, мс |
| --- | ---: | ---: |
| 1 | 37,495 | 33,828 |
| 2 | 46,348 | 33,781 |
| 3 | 39,945 | 33,826 |

В baseline повторялся трёхкадровый ритм, особенно около 50/46/40 мс во втором запуске.
В DEFAULT и final окнах такого выраженного ритма нет; остаётся плавное изменение нагрузки.
Эксперимент подтверждает эффект набора DEFAULT copies, но не локализует исходный ритм
до одного буфера, физической страницы или конкретного frame slot.

Baseline: 792–796 actions и 990–994 barriers/frame. DEFAULT: около 804 actions и 1006 barriers/frame;
final: 804,67 и 1006,67. Counts включают всю сцену, поэтому зависят и от dirty views.
В установившемся состоянии добавлены четыре CopyBufferRegion/frame и их переходы:
instances, materials, lights, transforms. Templates не копируются, пока содержимое не изменилось.
Command buffers остаются 12 на окно, submits — 24 с учётом Present.
На view остаётся один ExecuteIndirect с прежним MaxCommandCount. Число реально вышедших
GPU commands в каждом counter не считывалось; counts API actions нельзя считать этим числом.

Все семь коротких trace прочитаны native analyzer, overflow warnings отсутствуют. Единственный
warning во всех окнах — удалённые отсутствующие shader metrics трёх shaders; они не использованы.
Rust WRPV v11 path для численных выводов не использован.

## Цена памяти и решение по следующему culling этапу

181-frame обычный прогон, первый готовый кадр отделён от следующих 180, RenderDoc replay
закрыт и его GPU-ресурсы освобождены. GPU clocks в этом обычном прогоне не записывались:

| Метрика | baseline | final |
| --- | ---: | ---: |
| Среднее sampled-frame wall time, мс | 32,1563 | 21,3029 |
| D3D12MA allocation count | 520 | 538 |
| D3D12MA allocated bytes | 2978328576 | 2986586112 |
| D3D12MA reserved block bytes | 3258253312 | 3258449920 |
| Graph physical textures | 9/16 | 9/16 |
| Particle budget | 250000 | 250000 |

Приращение — **8257536 bytes = 7,875 MiB**, 18 allocations: 15 DEFAULT mirrors и три
frame-local UPLOAD template staging buffers. Из этой суммы 1,875 MiB приходится на staging
templates (24777 × 24 bytes, округление allocation до 655360 bytes на слот); оставшиеся
**6 MiB** — DEFAULT allocations. Это расчёт из allocator delta и известных новых ресурсов,
не измерение DXGI resident VRAM. Reserved blocks в чистом прогоне выросли на **0,1875 MiB**:
новые allocations в значительной степени разместились в существующих blocks. Эти totals включают
UPLOAD heaps и allocator slack, поэтому их нельзя называть дополнительной resident VRAM.
CPU mirror templates требует ещё 594648 bytes, без учёта vector capacity/allocator overhead.

Первый обычный прогон был сделан при открытом RenderDoc replay и дал 36,7485 → 31,8872 мс,
а разница reserved blocks составила 29,609 MiB. Он исключён из основного сравнения: resident
replay resources могли влиять на VRAM/тайминги, а allocator layout зависит от порядка загрузки.
Оба набора JSON сохранены; таблица использует файлы `*-memory-clean.json`.

Full-list culling оставлен: его стоимость стала около 2 мс, примерно 6% кадра; внутри local
shadows теперь преобладают draw и texture copies. Compact static/dynamic streams и пакетирование
dirty faces не добавлены. Следующий эксперимент имеет ограниченный потолок выигрыша около
оставшихся 2 мс и должен отдельно доказать пользу дополнительного хранения и dispatches.
Переиспользуемые Commands/Counter не переставлялись между faces; overflow/truncation casters не введены.

## CPU preparation и lifecycle

После завершения simulation/object-data jobs и ожидания слота сохраняется следующий порядок:

1. Обновление материалов, skeletal/particle/grass данных и сбор текущего scene/caster snapshot.
2. Запуск shadow visibility job. Пока она выполняется — CPU light-grid build/upload preparation.
3. Complete visibility, включая обязательное дренирование handle при исключении light-grid preparation.
4. Prepare dirty local-shadow faces и probe-owned static shadows; snapshot шаблонов текущего слота.
5. GPU scene copies, затем graph recording, GPU culling/barriers/ExecuteIndirect и submit.

Execute deferred lighting больше не строит light grid и не пишет CPU upload data. Local-shadow
Execute использует подготовленные light/cache/dirty-face snapshots и command count; cache меняется
после записи. Подготовлены также static shadows, которыми владеет reflection probe pass.
Сохранены invalidation, static/dynamic permutations, masked materials, удаления и per-slot fence.
Глобальный scene hashing оставлен: замена его revision-моделью без полной invalidation не оправдана.
ECS System::Prepare не использован для зависимой работы.

Новые PIX attach captures 2026-11-03: по три свежих процесса каждого варианта, 30 s прогрева,
6 s capture, одинаковый benchmark loop с продолжающейся simulation. Порядок:
baseline, DEFAULT, prepared, prepared, baseline, DEFAULT, DEFAULT, prepared, baseline.
Во всех девяти manifest **lostEtwEvents=0, lostEtwBuffers=0**; длительность trace 7,008–7,325 s.
Анализируется внутреннее окно **1000–6000 ms**, только полностью завершённые scopes нужного PID.
p95 — nearest rank по объединённым кадрам трёх повторов. Нулевые padding rows PIX API
исключены проверкой eventId/processId; сырые ответы сохранены. Первые captures 2026-11-02
сохранены как исторические артефакты, в новые числа не смешаны.

| CPU scope / cadence, мс median / p95 | UPLOAD baseline | DEFAULT до CPU prepare | final CPU prepare |
| --- | ---: | ---: | ---: |
| CPU full-frame interval между Game update starts | 30,837 / 33,683 (495) | 28,683 / 29,687 (548) | 29,149 / 29,900 (513) |
| Game update | 0,397 / 0,660 | 0,440 / 0,776 | 0,412 / 0,706 |
| Frame-slot wait/upload scope | 0,008 / 0,013 | 0,008 / 0,013 | 0,006 / 0,012 |
| Scene snapshot | 0,646 / 0,785 | 0,729 / 0,968 | 0,629 / 0,781 |
| Shadow visibility, Schedule + Complete | 0,238 / 0,334 | 0,281 / 0,424 | перекрывается с grid; отдельный wait ниже |
| Shadow visibility wait после grid | — | — | 0,084 / 0,148 |
| CPU light grid | 0,132 / 0,174 | 0,146 / 0,216 | 0,098 / 0,146 |
| Scene preparation, включая snapshot/grid/wait/dirty views | отдельного scope нет | отдельного scope нет | 0,894 / 1,127 |
| Graph recording | 3,580 / 4,673 | 4,100 / 6,997 | 3,188 / 4,280 |
| Snapshot start → graph recording end | 4,466 / 5,687 | 5,171 / 8,261 | 4,125 / 5,472 |
| Submit | 0,266 / 0,441 | 0,295 / 0,487 | 0,253 / 0,385 |

Число graph/preparation-span samples: 497 / 551 / 516. Строка Snapshot start → graph end включает
подготовку, промежуточный upload и запись графа, поэтому измеряет больше, чем перенос grid
между методами. От DEFAULT до final median этого span уменьшилась на **20,24%**, p95 — на
**33,77%**; graph recording — на **22,24% / 38,84%**. Медианы отдельных повторов:

| Вариант | Graph recording, три median, мс | Preparation + recording, три median, мс |
| --- | --- | --- |
| baseline | 3,566 / 3,352 / 3,848 | 4,433 / 4,221 / 4,771 |
| DEFAULT | 3,968 / 4,334 / 4,083 | 4,912 / 5,463 / 5,150 |
| prepared | 3,230 / 3,277 / 3,093 | 4,159 / 4,231 / 4,028 |

В baseline и DEFAULT grid вложен в graph recording во всех 497/551 полностью проверенных
graph scopes; в final — **0 из 516**, все grid/snapshot scopes находятся внутри Scene preparation
до соответствующего graph scope. Это подтверждает границу фаз профилем, а не только исходниками.
Shadow wait и прежний Schedule+Complete имеют разные границы: их разность сама по себе не
является точным временем сэкономленной работы. Все длительности inclusive wall time,
включают scheduling/driver calls и могут перекрываться; сумма строк не равна CPU work.

Wait/upload — весь OnUpdate, не чистое ожидание fence. CPU cadence включает GPU/Present waits:
между DEFAULT и prepared она **не улучшилась** (28,683 → 29,149 ms median). Не заявляется
снижение полного CPU work или дополнительный FPS от этапа 4. CPU/GPU clocks не фиксировались;
схема питания Turbo, ноутбук под длительной нагрузкой. Частичная NVIDIA telemetry для последних
повторов сохранена отдельно; её нельзя распространить на первые повторы. Основной вывод —
воспроизводимое сокращение измеренной CPU preparation/recording critical path при прежней сцене.

### PIX timeout diagnosis

Четыре прежние попытки через MCP (60–90 s timeout) не оставили файлов и подробного журнала;
восстановить точную причину тех зависаний нельзя. Новый контроль использует прямой
`pixtool 2610.28`, verbose logs, свежий PID, гарантированно исполняющий кадры benchmark loop,
отдельный timeout процесса и проверку файла/manifest. Для 6 s записи полное время CLI:
**11,54–51,27 s**; все девять завершились exit 0. В длинном prepared-1 журнал показывает
Starting New Timing Capture на 2,1 s, Stopping на 49,9 s, saved на 51,2 s, но сам trace —
7,010 s: задержка находится в live setup/start-stop пути PIX, вне измеряемого окна,
а не в анализаторе и не является 48-секундным кадром приложения.

Повтор исходного MCP `pix_capture_timing` на свежем прогретом final PID также успешен:
3 s capture за **9,53 s**, 60 252 160 bytes. Отдельный обычный `App.Run`, без benchmark,
после 30 s прогрева записал 3 s trace за **8,0 s**, exit 0; файл открыт официальным
анализатором, ETW losses=0. Таким образом, результат не зависит только от benchmark entry point.
Ошибка не воспроизводится устойчиво ни как
дефект CPU preparation, ни как постоянная неисправность MCP. Изменять renderer или чужой
PIX plugin по неподтверждённой причине не требуется. Рабочий CLI workflow и все журналы
сохранены; увеличение timeout само по себе не использовалось как доказательство исправления.

BeginFrame исправлен отдельно. Регрессия переключает Intel FG → Off через настоящий
BistroApp::BenchmarkFrame/Update: до исправления падала с `Bistro Update must begin the frame...`,
после исправления прошла. Тест не вызывает BeginFrame вместо приложения. Он также проверяет
90 кадров, изменения particle budget, focus/background, minimize и resize с debug layer.

## Correctness и проверки

Успешно собраны/выполнены Release x64:

- LightingGpuTests: baseline, DEFAULT и prepared; fixed radiance/depth readback, static/dynamic
  перестановки, masked/deleted/out-of-frustum casters и light-grid brute-force сравнение до 769 lights.
- Новая ShadowUploadSlots GPU-регрессия: DEFAULT heap, рост 1→9 templates, одинаковая запись
  не меняет revision, изменения/удаления доходят до каждого из трёх слотов, GPU readback.
- ReflectionProbeTests `--debug`: static bake, cache invalidation, queued frame-slot publication,
  два настоящих probes, resize/mode switch, depth/radiance/PBR/XeSS readback.
- RenderGraphTests: pooling, hazards, queued frames, descriptors, shared states, resize, teardown.
- ObjectDataJobsTests: actual material upload через Render во всех слотах; immutable templates,
  selected indirect slots и moving bounds, 1/4 threads.
- BistroStartupTests и BistroRenderGraphTests: настоящий lifecycle; статичный pooled/dedicated HDR
  побайтно одинаков для обоих AO placements, odd resize и native/XeSS switch.
- SecondGroupTests, LoadingTests, MemoryAllocatorTests.
- Core JobSystemTests и ECSJobTests: dependency chains/fan-in, exceptions/reuse, ordered sparse
  system batches на 1/4 threads; пересобраны текущим MSVC против свежей Engine.Core.lib.
- `git diff --check`.

Два старых fixture ожидания исправлены без ослабления assertions: material upload проверялся
раньше OnRender; manual-frame fixtures имели неинициализированный GameTimer с DeltaTime=-1.
Для HDR identity timer теперь явно остановлен, а frame lifecycle выполняется. Profiling marker
test condition включает PROFILE_BUILD и больше не подаёт намеренный nullptr активному маркеру.
GPU runtime tests запускались последовательно; один конкурентный Bistro запуск вышел с 2173,
повторный отдельный запуск прошёл. Debug layer не включался в timing runs.

Standalone **FrameGenerationTests исправлен и прошёл 2026-11-03**. Первоначальная ошибка
`DiscardResource` COMMON/PRESENT вместо RENDER_TARGET воспроизводилась также с baseline
headers/libraries. Проверка debug layer после каждого кадра дополнительно обнаружила
отсутствующий SRV transition входа уже на первом FG-Off/native кадре, до включения SDK providers.
Причина — общий GDX12OutputToScreenPass полагался на barriers графа, тогда как FG и несколько
effect tests вызывают его Execute напрямую. SetRenderTargets только привязывает RTV.

Общий проход теперь переводит input в ALL_SHADER_RESOURCE и output в RENDER_TARGET перед
bind/discard. На graph path helper отфильтровывает одинаковые before/after states, дополнительных
GPU transitions не появляется. Discard сохранён: это требование инициализации placed RT;
RT state требуется самим [D3D12 DiscardResource API](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-discardresource).
Регрессия оставляет direct Execute и проверяет info queue после каждого Flush и teardown.
Red: первый FG-Off кадр падает с SRV state error; green: весь provider/restart/pause/resize/SR
цикл проходит без warning/error. AMD генерирует 33 кадра, Intel — 32; NVIDIA DLSS-G корректно
сообщает unsupported на RTX 3060, его генерация этим оборудованием не проверяется.

После fix заново собраны и последовательно прошли AOTests, DepthOfFieldTests, MotionBlurTests,
SSRTests, ReflectionProbeTests `--debug`, BistroRenderGraphTests и BistroStartupTests. Эти проверки
покрывают direct consumers, graph states, HDR readback, runtime switches и настоящий app lifecycle.
Вместе с предыдущими проверками задачи — 16 целевых исполняемых тестов; 8 повторены после fix.
Независимый targeted review двух изменённых файлов и barrier helpers замечаний не обнаружил.

Свежий `prepared.rdc` открыт RenderDoc: 43826 draw-событий, 46370 action-tree событий,
17 именованных проходов, debug log пуст. Проверены compute/graphics bindings local shadows
и экспортированный 1280×800 tone-map target (`rt_95146_0.png`). Также визуально просмотрены
native Nsight screenshots baseline/final с движущимися частицами. Фазы анимации отличаются;
pixel-exact Bistro A/B, физическая secondary GPU и длительный soak не проверены.

## Воспроизведение и файлы

Все raw artifacts локально, исключены из Git:
`D:\Project\PEPEngine2\Build\Captures\2026-11-02-render-preparation`.
Там находятся `gpu-summary.json`, CPU JSON/responses, `pix_query.py`, runtime/memory reports,
`manifest.json`, `baseline-{1,2,3}.ngfx-gputrace`, `default-{1,2,3}.ngfx-gputrace`,
`prepared-1.ngfx-gputrace`, два успешных warm PIX capture и `prepared.rdc`.
Build roots: `Build/RenderPreparation/{baseline,default,prepared}`.
Изолированные output/include/library props: `Build/render-preparation-*.props`.

Доведение приёмки 2026-11-03:
`D:\Project\PEPEngine2\Build\Captures\2026-11-03-acceptance`.
Там `capture-cpu.ps1`, `analyze-cpu.py`, `capture-runs.json`, `cpu-summary.json`, per-event JSON,
девять `{baseline,default,prepared}-{1,2,3}.wpix`, verbose PIX logs, MCP контроль и
`manifest.json` с SHA-256 артефактов и EXE/PDB. `probe.wpix` записан у конца benchmark process
и не включён в A/B. FG red/green logs — `Build/fg-{checkpoint-red,fix-green}.log`;
дополнительные build/runtime logs — `Build/acceptance-*-{build,run}.log` и
`Build/acceptance-test-results.json`.

Повтор CPU captures/анализа (последовательно, без других GPU runtimes):

```powershell
& Build/Captures/2026-11-03-acceptance/capture-cpu.ps1
& C:/Users/Asus/.codex/vendor_imports/gpu-debug/pix-mcp/.venv/Scripts/python.exe Build/Captures/2026-11-03-acceptance/analyze-cpu.py
```

Внутри capture script после 30 s прогрева выполняется фактическая CLI команда:

```powershell
& 'C:\Program Files\Microsoft PIX\2610.28\pixtool.exe' --output=verbose --log=verbose --log-file=<log> attach <PID> take-new-timing-capture <capture.wpix> --duration=6000
```

Пример фактически использованных build/check команд (PowerShell; MSBuild 18 Professional):

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild Apps/App/App.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 /p:PEPEnableProfiling=true /p:ForceImportBeforeCppTargets=D:\Project\PEPEngine2\Build\render-preparation-prepared.props /m:2 /nr:false
& $msbuild Apps/App/LightingGpuTests.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 /p:PEPEnableProfiling=true /p:ForceImportBeforeCppTargets=D:\Project\PEPEngine2\Build\render-preparation-prepared.props /m:1 /nr:false
& Build/RenderPreparation/prepared/bin/x64/Release/LightingGpuTests/LightingGpuTests.exe
& Build/RenderPreparation/prepared/bin/x64/Release/ReflectionProbeTests/ReflectionProbeTests.exe --debug
& Build/RenderPreparation/prepared/bin/x64/Release/BistroStartupTests/BistroStartupTests.exe
& Build/RenderPreparation/prepared/bin/x64/Release/BistroRenderGraphTests/BistroRenderGraphTests.exe
& Build/RenderPreparation/prepared/bin/x64/Release/ObjectDataJobsTests/ObjectDataJobsTests.exe
git -c diff.ignoreSubmodules=all diff --check
```

У остальных `.vcxproj` в списке проверок тот же build invocation с заменой имени. Бинарники
лежат в `bin/x64/Release/<ProjectName>`. Полные build/runtime logs — `Build/render-preparation-*`.
Sandbox MSVC выдавал C1902 (PDB service); рабочие сборки выполнены через host MSBuild.
Существующие compiler/third-party PDB warnings не объявляются исправленными этой задачей.

Контрольные SHA-256:

| Capture | SHA-256 |
| --- | --- |
| baseline-1.ngfx-gputrace | `D7A8B737653A5D7CE1C039F611B6EF9325051ED614753CEFD55F926FDF4F669F` |
| default-3.ngfx-gputrace | `2EDA7AEDE74A76361179EFDAC58B15AD0E6B9B69B74A5D4CAB61BF14FCBF6C30` |
| prepared-1.ngfx-gputrace | `A257771BF6A242C3644FC0ED40423F3EBEF678132F8718DBBB276B5EA24C9837` |
| prepared.rdc | `6758AA51D7AED0B5FD4B54ECD0DE17351FE9CA7F966C85A17A00DB18F2FF1ADF` |

Чужие изменения `Apps/App/tests/BistroPropsTests.cpp` и
`docs/research/2026-11-02-cpp-exceptions-simd.md` сохранены вне коммитов задачи. Push не выполнялся.
