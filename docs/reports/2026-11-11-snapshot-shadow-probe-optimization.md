# Инкрементальный snapshot Bistro, тени и reflection probes

Реализация сохранена локальным коммитом `e016329f`. Работа выполнена в исходном checkout, чужая правка BistroAdditionsTests.cpp сохранена. Измерения и сборки завершены, runtime-ресурс передан исходному чату для независимой проверки. PIX-after остаётся открытой частью этой проверки.

## Изменения

Snapshot хранит входы по renderer и прежние записи по GPU instance. При совпадении mesh generation, material inputs, выбора submesh, видимости, текущего instance mirror и общей world matrix кэш сохраняет существующие primitive handles. Этот путь читает входы renderer и непрерывный диапазон instance mirror. Полный обход submeshes, повторное вычисление подписей и обновление примитивов выполняются только для групп с изменившимися входами и для обычного пути динамических и preview-примитивов.

Инвалидация учитывает следующие источники.

- События ECS создают и удаляют GPU instances. Sweep удаляет отсутствующие primitives, включая renderer без transform. Повторно использованный entity ID получает другой primitive handle. World reset очищает кэши.
- Render prep публикует данные текущего frame slot. Снимок читает этот mirror, включая прямые `CopyData` записи. `Visible`, mesh handle, material bindings и submesh selection проверяются при сборе. Флаги материала и `GetRevision()` обнаруживают изменения кастеров и материалов.
- `GDX12GeometryBuffer` меняет revision при замене mesh cache entry. Скелетные и application primitives продолжают поступать через общий реестр. Для последних учитываются изменения через mutable `Get`.
- При изменении static membership или данных пересчитываются статическая подпись и объединённые bounds. В частности, удаление крайнего объекта сужает bounds. Membership revision реестра разрешает пакетное сохранение ранее проверенных диапазонов и обновляет сохранённый список dynamic/application primitives. Свет, окружение и параметры probe по-прежнему собираются каждый кадр и участвуют в существующей инвалидации.
- `GDX12DeviceResources::LocalShadowCasters` содержит текущих динамических и остальных кастеров. Полный набор включает `StaticMeshShadowCasters`. `ShadowVisibilitySystem` принимает статический mesh список и его revision, добавляет его к cached static набору при инвалидации, затем на каждом кадре дополняет результат текущей динамикой. Standalone callers могут по-прежнему передавать единый список. Baked static, frozen и dynamic probe policies остались прежними.

Изменённые contracts охватывают `GDX12RenderPrimitiveRegistry::GetMembershipRevision`, `GDX12DeviceResources::RetainKnownMeshPrimitives`, `GDX12GeometryBuffer::GetMeshCacheRevision` и дополнительные аргументы `ShadowVisibilitySystem::Schedule`. Production caller `RenderPipelineContext::PrepareScene` передаёт обе части caster snapshot. Legacy standalone callers используют прежние аргументы. ObjectData reference scan учитывает оба списка. Политика GPU shadow rendering и reflection capture исполняется прежними проходами.

`RenderModuleSnapshot.cpp` собирается с оптимизацией в Debug по примеру существующих горячих translation units. Это меняет пошаговую отладку этой функции. Числа ниже показывают общий вклад алгоритма и компиляции. Отдельный замер со старым алгоритмом и новым флагом компиляции не проводился.

## CPU scopes

Один профиль, стартовая камера Bistro, seed 12345, 1280×800, XeSS, MGPU выключен, полный набор объектов и real-time physics. Baseline PIX содержит 137 полных snapshot scopes в окне 1000–10000 мс, PID 12960, TID 31336. Кандидат измерен на 600 кадрах после 120 warmup через локальный QPC diagnostic build. Он заменяет маркеры только в четырёх translation units через `ForceImportBeforeCppTargets`, пишет накопленные строки в CSV при выходе и не участвует в обычной серии FPS. Сырые числа позволяют сопоставить стоимость этапов. Различие инструментирования и запусков ограничивает точность сравнения между столбцами.

| CPU scope | Baseline PIX mean / p95, мс | Кандидат QPC mean / p95, мс |
|---|---:|---:|
| Scene snapshot | 10,137 / 14,844 | 1,868 / 2,419 |
| Scene preparation | 16,069 / 23,459 | 4,909 / 5,972 |
| Shadow visibility wait | 0,455 / 0,953 | 0,480 / 0,759 |
| Snapshot world collection | — | 1,513 / 2,005 |
| Snapshot primitive retirement | — | 0,034 / 0,045 |
| Snapshot primitive reduction | — | 0,164 / 0,201 |
| Local shadow pass preparation | — | 0,757 / 0,905 |
| Local shadow instance preparation | — | 0,248 / 0,297 |
| Reflection probe preparation | — | 0,015 / 0,019 |
| Объединённые local shadow и reflection preparation | — | 1,019 / 1,212 |

Значения preparation для shadow/probe посчитаны по объединению интервалов одного кадра. Вложенный static pass и повторные no-op вызовы не учитываются дважды. Shadow visibility wait измеряется отдельно. GPU execution в эти значения не входит.

За каждый из 600 кадров кэш пропускал полный обход **23 032** submeshes, а обычный путь посещал **1 752**. В реестре было **23 036** статических mesh primitives, статический список не перестраивался. В среднем публиковалось **1 000** mesh primitives за кадр, p95 **1 748**. Первая итерация дала snapshot 3,485 мс mean и 4,070 мс p95, вторая — 3,260 / 3,892 мс.

Выборочная атрибуция на кадрах 200, 300, 400 и 500 второй итерации обнаружила три cached renderer, 201 проверенный material binding, 185 реально использованных slots и 201 уникальный material pointer. Проверка материалов занимала 0,012–0,021 мс. Полный путь динамических и preview renderer занимал 1,30–1,74 мс. Основной остаточный расход находился вне material comparison.

Следующая итерация сохраняет проверенные диапазоны mesh instances пакетно и обслуживает список dynamic/application primitive handles по membership revision. QPC подтверждает достижение ориентира mean <=2 мс. Остаются bulk comparison входов изменяемых renderer, epoch marking и обработка динамических записей. Они сохраняют поддержку прямых edits текущего frame slot и public fields. Полный static submesh scan и обход всей статической части registry в steady state отсутствуют. При изменении topology или static metadata выполняется полный корректирующий проход. Объединённая стоимость подготовки теней/probes около 1 мс на кадр. Локальная spatial invalidation для lights/probes в этот патч не добавлялась.

Baseline PIX сохранён в `Build/BoundAnalysis/bistro-timing.wpix`. Для кандидата два attach завершились тайм-аутом, а PIX launch завершился `0x800704c7` после запуска приложения. Файл timing capture кандидата не появился. Логи попыток, диагностические исходники, `qpc3-scopes.csv`, `qpc3-summary.json`, `qpc3-state.json` и обычные результаты находятся в `Build/SnapshotOptimization/`.

## Обычные прогоны

Финальная последовательная серия B/A/B использует обычный executable без QPC override. Перед каждым запуском выполнялось ограниченное ожидание охлаждения, фактическая начальная температура GPU была 65 / 66 / 68°C. По 120 warmup и 600 samples. Все runner exit codes равны 0, hashes executable, INI и overlay совпали до и после каждого прогона.

| Порядок | Raw samples mean / p95, мс | FPS | Physics requested / simulated, с | Ticks | Debt начало → конец, с |
|---|---:|---:|---:|---:|---:|
| B1, cool-b1 | 47,067 / 53,279 | 21,25 | 28,2333 / 27,5000 | 1650 | 0,0101 → 0,7433 |
| A, cool-a | 58,251 / 67,468 | 17,17 | 34,9448 / 28,3333 | 1700 | 0,0183 → 6,6298 |
| B2, cool-b2 | 48,672 / 57,772 | 20,55 | 29,1944 / 26,7833 | 1607 | 0,0059 → 2,4171 |

Среднее двух B около 47,87 мс, на 17,8% ниже A. Разброс B по среднему 3,35%. Рост physics debt составил 0,73 / 6,61 / 2,41 с. Физика исполнялась с прежними 60 Hz, CCD и solver settings.

Среди секунд с GPU utilization >=70% максимальная температура во всех трёх прогонах 87°C, медианная graphics clock 1612 / 1736 / 1658 MHz. Software thermal slowdown отмечен в 0 из 35, 6 из 34 и 7 из 36 таких samples, hardware thermal slowdown отсутствовал. GPU telemetry охватывает загрузку, warmup и measured frames. Более ранние A/B/A и B/A/B без предварительного охлаждения показали сильный thermal drift, вплоть до baseline p95 296 мс. Они сохранены как отвергнутая для итоговой оценки серия. Даже финальные значения характеризуют этот Debug workload и его температурную историю. Разные real-time dt дают разные физические траектории и число ticks.

Raw JSON, per-frame samples, DOT и telemetry сохранены в `Build/SnapshotOptimization/cool-{b1,a,b2}*`. Первый baseline executable имеет SHA256 `eb5de81696b4c7a4d5aceb68507ed160bfdfdba86b41e2009a404aff5df6385c`. Окончательный `App.SnapshotOptimization3.exe` и обычный `App.exe` имеют SHA256 `5f2972a1aad2dd58afdcf3f1ad671254a629e380bbb3fb9647f278ed49382382`. Сравнение касается CPU-подготовки и whole-frame темпа. Актуальный GPU PIX-after остаётся частью независимой проверки.

## Проверки

Debug x64, MSVC v145. Команда сборки из корня checkout, с указанием проекта.

```powershell
$msbuild = 'C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/amd64/MSBuild.exe'
& $msbuild Apps/App/App.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /nr:false /nologo /v:minimal
```

Первый sandbox build получил C1902. Следующий incremental build оставил старые objects с прежним C++ layout. Полный последовательный `/t:Rebuild /p:BuildProjectReferences=false` для Engine.RendererDX12, Engine.UI, App.Base и App с доступом к серверу PDB устранил проблему. Обычная финальная сборка выполнена без локального QPC override.

- `RenderPipelineTests.exe` — primitive records, bounds, prepared views, generation-aware range retention и graph contracts. Фикстура существующего теста вызывала подготовку атласа с неинициализированными D3D12 heaps. Проверка с выключенным солнцем остаётся без устройства, а проверка реальных каскадов создаёт устройство и малый тестовый атлас.
- `ObjectDataJobsTests.exe` — slot mirrors и реальные рендер-кадры с full-scan reference, unchanged reuse, pause, изменения трансформа, материалов, visibility и классификации, reimport, bounds shrink и реальное повторное использование entity ID.
- `LightingGpuTests.exe` — debug layer, точное совпадение static/dynamic light signatures с полным caster scan, первый кадр с revision 0, изменения light position/range/matrices, статических и динамических кастеров, GPU shadow readback.
- `ReflectionProbeTests.exe` — GBV, baked/dynamic/frozen modes, off-camera caster, environment/settings, slot publication и смена режимов.
- `BistroStartupTests.exe --reset-only` — A/B/A и повтор A, воспроизводимость начальных counters, transforms и camera slot.

Тестовые проекты собирались теми же flags с `/p:BuildProjectReferences=false` после обновления библиотек. Исполняемые файлы запускались из своих runtime directories через `peengine-perf/scripts/run_job.py`. Для ObjectData и Reflection нужен минимальный INI `[ConsoleVariables]` и `Render.NumFrames = 3`: эти тесты не регистрируют `App.Water.HeightScale` из Bistro INI. Оригинальные runtime INI восстановлены. Полные логи и коды завершения сохранены рядом с benchmarks.

На окончательном коде все пять сборок и пять запусков завершились с exit 0. Сводка находится в `final3-test-exits.log`, individual runner states — в `final3-test-*.json`. ReflectionProbeTests выполнялся с GPU-based validation, LightingGpuTests с D3D12 debug layer. Локальный `baseline_native.py` повторно прочитал Scene preparation baseline и сохранил исполнения в `baseline-cpu57.json`. Сборки сохраняют существующие C4244, LNK4075 и LNK4099 warnings от BistroBounds и сторонних PDB.

Release, физические MGPU-маршруты и покадровое визуальное сравнение полной сцены не проверялись. Baseline PIX показывает около 13 мс у GPU fluid simulation и local shadows. Для этих GPU этапов новых timing данных нет.

## Воспроизведение локальных измерений

Артефакты лежат в ignored каталоге `Build/SnapshotOptimization`. `baseline-hashes.json` и `BaselineRuntime/` сохраняют исходный executable, PDB, DLL и конфиг. `measure.py` содержит команды последовательных ordinary прогонов. `ScopeDiagnostics.h`, `ScopeDiagnostics.targets` и `qpc_stats.py` сохраняют резервный CPU measurement и объединение вложенных интервалов. Для QPC build применяется дополнительный локальный параметр, после которого требуется обычная пересборка.

```powershell
/p:ForceImportBeforeCppTargets=D:/Project/PEPEngine2/Build/SnapshotOptimization/ScopeDiagnostics.targets
```

Обычные команды используют `App.SnapshotOptimization3.exe` и frozen `App.BoundAnalysis.exe`, runtime `Build/bin/x64/Debug/App`, overlay `Build/SnapshotOptimization/high.ini` и аргументы ниже.

```powershell
'-Quality.Overlay=D:/Project/PEPEngine2/Build/SnapshotOptimization/high.ini'
'-Loading.BenchmarkWarmupFrames=120'
'-Loading.BenchmarkFrames=601'
'-Loading.BenchmarkOutput=D:/Project/PEPEngine2/Build/SnapshotOptimization/<run>.json'
```

`BenchmarkFrames=601` даёт 600 per-frame samples. Камера и seed задаются существующим benchmark entry point. Runtime DLL и INI исходного baseline после сборок побайтно совпадают с сохранённой копией. В обоих вариантах 308 prepared textures, 29 graph passes, physics count scale 1, particle count scale 2 и particle budget 500 000. Кандидат сохраняет 24 784 static-mesh submesh records по CPU-счётчикам.

## Независимая приёмка основным чатом

Проверена реализация `e016329f` и отчёт `d16f5f7f`. По замечаниям проверки исполнитель исправил первое заполнение static visibility при revision 0 и добавил regression assertion. После промежуточных результатов 3,48 и 3,26 мс потребована дополнительная итерация: пакетное сохранение проверенных instance ranges и сохранение списка dynamic/application primitives устранили повторный steady-state обход статической части registry.

Независимый пересчёт `qpc3-scopes.csv` по 600 полным Scene preparation интервалам подтвердил snapshot mean 1,867561 мс / p95 2,418600 мс и Scene preparation 4,909049 / 5,971900 мс. Каждый выбранный кадр содержит ровно один snapshot и counters, 24 784 reused + visited records и ноль static rebuilds. Объединение shadow/probe интервалов, полностью содержащихся в Scene preparation, составляет 1,017611 / 1,210800 мс; небольшая разница с таблицей объясняется границей внешнего scope. Независимый пересчёт `frame_samples_ms` обычной B/A/B серии подтвердил 47,066654 / 58,251026 / 48,671515 мс mean и 53,2790 / 67,4678 / 57,7716 мс p95.

Проверены конечные state JSON всех пяти suites: passed, exit 0, fingerprints до и после совпадают. После передачи ресурса исполнителем основной чат отдельно повторил RenderPipelineTests и ObjectDataJobsTests: оба passed / exit 0 (2,03 и 10,24 с). Исходный INI восстановлен в finally. Состояния и логи: `Build/SnapshotOptimization/reviewer-RenderPipelineTests.json` и `reviewer-ObjectDataJobsTests.json`. Проверены production callers разделённых caster lists и `git diff bde36a6b..HEAD --check`; ошибок не обнаружено. Посторонняя правка BistroAdditionsTests.cpp сохранена вне коммитов задачи.

Приёмка ограничена CPU snapshot и подготовкой shadow/probe в измеренном Bistro workload: ориентир mean <=2 мс достигнут, подтверждённых блокирующих дефектов в проверенном патче не осталось. Это не утверждение об отсутствии других bottlenecks движка. Долг физики продолжает расти, GPU drawing shadows/fluid отдельно не оптимизировался. PIX-after отсутствует; baseline PIX и candidate QPC не являются одинаковым инструментированием. Release, физический MGPU, длительный thermal soak и визуальное A/B полной сцены не проверены.
