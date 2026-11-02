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
финального CPU-пути **не подтверждена измерениями**: четыре попытки записи PIX завершились timeout.
Эта часть сохранена как исправление требуемой границы подготовки; отдельный CPU speedup не заявляется.
Следовательно, численная CPU-часть performance-приёмки остаётся открытой.

11 целевых исполняемых проверок прошли. Standalone FrameGenerationTests завершился ошибкой
debug layer на baseline и candidate; это существующая проблема FG, описанная ниже.
Полностью зелёная FG-приёмка не заявляется.

## Сопоставимая база

- Исходный HEAD: `8ae2ab0503333c92037a2c046ca386fe1df23e2c`, после merge `33ca359`.
- Profiling baseline: `471d276` — только opt-in scopes, без изменения алгоритма.
- DEFAULT: `8e6f342` — пять shadow DEFAULT mirrors на frame slot и staging templates.
- Lifecycle: `81fdb36` — настоящий Bistro Update вызывает BeginFrame; отдельная red/green регрессия.
- CPU preparation: `73d8066`; полные SHA также записаны в локальном
  `Build/Captures/2026-11-02-render-preparation/manifest.json`.

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

Baseline и DEFAULT PIX attach captures успешно записаны с нулём lost ETW events/buffers.
Время — wall duration scopes, включая scheduling/driver calls. Диапазон анализа 500–6500 мс;
p95 — nearest rank. Это не inclusive CPU sampling и не сумма процентов стеков.

| CPU scope / cadence, мс median / p95 | baseline | DEFAULT | final CPU prepare |
| --- | ---: | ---: | --- |
| CPU full-frame interval между Game update starts | 25,507 / 29,883 (224) | 23,713 / 24,222 (252) | нет данных |
| Game update | 0,339 / 0,504 | 0,381 / 0,692 | нет данных |
| Frame-slot wait/upload scope | 0,006 / 0,012 | 0,007 / 0,012 | нет данных |
| Scene snapshot | 0,501 / 0,625 | 0,612 / 0,768 | нет данных |
| Shadow visibility | 0,214 / 0,272 | 0,227 / 0,323 | заменён подготовкой и отдельным wait scope; нет замера |
| CPU light grid | 0,112 / 0,153 | 0,117 / 0,167 | до graph Execute; нет замера |
| Graph recording | 2,897 / 3,490 | 3,130 / 4,666 | нет данных |
| Submit | 0,214 / 0,723 | 0,246 / 0,374 | нет данных |

Wait/upload — весь OnUpdate scope, не чистое ожидание fence. CPU cadence включает update,
render, Present и ожидания; её нельзя считать чистым CPU execution time. PIX GPU clocks
не фиксировались/не измерялись, поэтому эта таблица не изолирует эффект CPU-рефакторинга.

Final PIX попытки: warm attach 6 s / timeout 90 s; fresh attach 3 s / timeout 60 s;
launch 30 s / timeout 90 s; fresh CPU-only attach 3 s / timeout 60 s. Файлы не получены,
эти попытки исключены из численных выводов. Не заявляется уменьшение CPU median/p95 или
суммарного CPU work. Из recording phase устранено само построение grid, а независимые jobs
теперь перекрываются; количественная проверка новой critical path остаётся следующей проверкой.

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

Standalone **FrameGenerationTests не прошёл**: `DiscardResource` получает COMMON/PRESENT
вместо RENDER_TARGET. С теми же исходными baseline renderer headers и baseline libraries
тест воспроизвёл ту же ошибку. AMD успел сгенерировать 33 кадра, Intel — 32; NVIDIA DLSS-G
недоступен на RTX 3060. Это не доказательство исправности FG SDK integration и не новая
регрессия данного shadow/CPU-патча; исправление provider/state interoperability сюда не включено.

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
