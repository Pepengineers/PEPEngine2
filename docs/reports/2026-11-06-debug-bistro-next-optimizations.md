# Следующая итерация Debug Bistro

6 ноября 2026. Основа — `5a088ee` / код `908457b`, текущий checkout
`codex/debug-frame-hotspots`. План: `docs/plans/2026-11-06-0142-debug-bistro-next-optimizations-plan.md`.

Две оптимизации renderer подтверждены обычными A/B-запусками.
Устойчивый realtime с исходной collision fidelity не достигнут.

## Условия и метод

Ryzen 7 5800HS / RTX 3060 Laptop 6 GiB, Debug x64. Обычный App,
1280×800, стартовая камера, `Render.NumFrames=3`, Bounds=false,
particle scale=1 / budget=250000. INI SHA256:
`53c782371018e3e7e00f43d7d41594de0112be828a89277c076651a86b16e245`.
Оригинальный executable сохранён как `NextBaseline.exe` до пересборки;
SHA256 `a3b817b00582a890f8b2d2c9e083bba15cb7b8a9101c5e97753da0afb9ce124e`.
Старый `Baseline.exe` из предыдущей итерации не используется.

Для обычного App каждый scored run содержит 120 кадров после первого кадра:
`-Loading.BenchmarkOutput=<absolute-json> -Loading.BenchmarkFrames=121`.
Каждый оставленный вариант сравнивается тремя чередующимися парами.
Проверяются fingerprints и параметры сцены; сборки, тесты и captures
не пересекаются со scored runs. Медиана в таблицах — медиана средних frame time
трёх запусков, а не медиана отдельных кадров. Критерий экспериментов:
выигрыш в каждой паре выше 2% от медианы baseline (`decide.mjs`, paired).

Подробные scopes и physics counters находятся в диагностических копиях TUs,
обычный App не содержит instrumentation. Их времена нельзя смешивать
с обычным executable или складывать как независимые слагаемые.
CSV позволяет сопоставлять simulated time, fixed ticks и seed.
Frame p95 диагностического executable указан отдельно от scored frame means.
Частоты не фиксировались; nvidia-smi записывает GPU temperature/clocks/P-state
перед и после запусков. Между сериями есть заметный thermal/clock drift,
поэтому выигрыши разных серий не складываются.

## Что оставлено

- Shadow visibility сравнивает текущие static bounds/signatures с предыдущим
  набором и кэширует результат по каждому light view. Динамика проверяется
  каждый кадр. Static/dynamic signatures объединяются в определённом порядке;
  удаления, exits и смена classification обновляют AllCasters.
  Radiance не влияет на ключ visibility, position/range/cone/type/matrices
  и включение shadow slice влияют. Общая lighting revision для этого не нужна.
- Snapshot хранит полную signature вместе с primitive index/generation.
  Проверки CPU mirrors, material/mesh revisions и прямых edits сохранены.
  Caster и capture-only min/max накапливаются по одному разу и затем объединяются.
  Double extrema и округление float extents наружу сохраняют тонкие bounds далеко
  от начала координат. Удаление крайнего contributor сужает bounds;
  пустая сцена получает bounds по умолчанию вместо старого значения.

Разрешение и необходимые грани теней, эффекты, состав сцены, частота анимации,
fixed step, четыре collision steps, CCD, solver settings, impulses, Debug CRT,
assertions и iterator checks сохранены. Новых зависимостей и настроек нет.

## Пять измеренных вариантов

| Вариант | Baseline → candidate | Решение |
|---|---|---|
| Static shadow visibility | 122,483 → 89,193 мс, −27,18% | Оставлен |
| Fold single-hull shape wrapper | 17,613 → 17,674 мс/tick | Удалён: выигрыша нет |
| Snapshot с двумя bounds reductions | 71,433 → 70,559 мс, −1,22% | Не принят: внутри шума |
| Snapshot с одним накоплением и Merge | 71,749 → 67,297 мс, −6,20% | Оставлен |
| Общий static aggregation cache | 67,929 → 65,202 мс, −4,01% | Удалён: одна пара дала лишь 0,86% |

Для shape wrapper — одна exploratory headless пара: по 1827 fixed ticks,
три полные seeded waves, 379 dynamic props + 2 static meshes, 16 threads.
Последовательности active-body counts совпали на каждом тике;
ms/tick p95 — 31,480 → 31,541. Импульсы 500/700 cm/s, peak speed и все
проверки совпали. В обеих версиях 2 props вышли за modeled geometry;
это существующее ограничение containment, не исправленное этой итерацией.
Дополнительная серия не выполнялась: источник восстановлен.

Для последнего cache-варианта snapshot scope снизился до 8,57 мс
(aggregation 3,36 мс), но короткая ordinary серия не прошла выбранный paired
критерий. Этот дополнительный кэш не включён в итоговый код.

Shadow-пары: baseline 104,895 / 122,483 / 123,649 мс;
candidate 85,468 / 89,193 / 89,244 мс. CV 8,98% / 2,46%.
Snapshot Merge-пары: baseline 69,879 / 71,749 / 72,198 мс;
candidate 67,297 / 66,689 / 68,872 мс. CV 1,73% / 1,67%.
Эти промежуточные серии не являются окончательным суммарным сравнением.

## Атрибуция

В отдельных diagnostic profiles полный shadow scan выполнял 3 249 040 range
tests/кадр, cached вариант — 102 735. Face tests приблизительно 274 тыс. → 35–36 тыс.
Сумма длительностей shadow jobs — около 311 → 14–18 мс; warm static rebuilds=0.
Это сумма wall durations jobs, включая возможное вытеснение потоков, а не
exclusive CPU time и не wall time кадра. Ожидание снизилось с ~19 мс
до ~0,17–0,23 мс. Поэтому grid/BVH не добавлялся.

После snapshot Merge: scene snapshot ~11,80 мс, extraction ~4,96,
aggregation ~6,63, cascades ~0,032. Это отдельный профиль; предыдущий
отчёт и profiles были сняты при других частотах/состояниях симуляции.

CPU local shadow scopes в последнем исследовательском профиле:
clear 0,92, cull 0,43, draw/bindings 3,79, copy 0,38 мс;
dirty static faces=0, dynamic≈253, InputCommands=24837.
Shader по-прежнему просматривает полные templates по каждой dirty face.
Компактные static/dynamic buffers и batching не добавлены без подтверждения
их влияния на общий frame time. Свежая GPU атрибуция приведена ниже.

BeginFrame: UI ~5,10 мс, frame-generation/token/pacing ~0,017 мс.
Удалять ожидание/pacing оснований нет. Skeletal prepare ~4,81 мс;
15 pose calls и 15 evaluations/кадр, bounds jobs в сумме ~3,66 мс CPU,
hash/publish ~0,123 мс. В этой медленной realtime нагрузке поза обновляется
каждый кадр; кэш между pose updates не уменьшает её обычную работу.
Частота анимации и motion history не менялись.

## Физический прогресс до изменений

10 минут обычного realtime Run при TimeScale=1: requested=600,044 с,
simulated=90,750 с, конечный debt=509,294 с. Это не только startup debt.
После первой минуты каждая следующая минута добавляла примерно 49–53 с долга.
Jolt занимал 23–40 мс/tick в поздних интервалах; ECS post sync в среднем
~0,37 мс/tick, pre ~0,09. Средний CPU render/update frame time после первых
10 кадров — 120,485 мс, p95=161,320 мс.

Встроенный профиль Jolt показывает collision/contact work: compound/mesh/convex
и solver, включая тысячи manifold/constraint операций за fixed tick.
Подготовка jobs и sleeping-body synchronization не являются главным резервом.
Оптимизированный Debug Jolt `/O2 /Ob2`, Debug DLL CRT и assertions уже были
на месте. Повторно засчитывать их как оптимизацию нельзя.
Accumulator, wall budget и clock не сбрасывались и не менялись.

## Изоляция от параллельной работы

Во время финальных проверок другой чат изменил `BistroProps.h` и
`BistroPhysicsTests.cpp`, затем создал `f5f7d91` с box proxies. Эти изменения
сохранены; они не принадлежат данной оптимизации renderer. Все пять
исследовательских A/B-серий завершились до начала этого чата.
Дополнительный поздний headless thread probe пересёкся с его нагрузкой:
его timings исключены из выводов о масштабировании.

Финальный обычный `NextFinal.exe` собран с текущими renderer libraries,
но `App.cpp` и все application headers взяты из `5a088ee` в ignored build
directory. Проверена подстановка именно frozen `App.obj` в link response.
Диагностический `NextProfile.exe` использует те же исходные scene headers.
Manifest: `frozen-inputs.json`. Это сохраняет исходные convex-коллайдеры,
массу, инерцию, число props, четыре substeps и CCD при сравнении renderer.
Общие Debug/Release проекты собраны также в совместном checkout с `f5f7d91`.
Выигрыш box proxies не включён в результаты renderer.

## Итоговая приёмка

### Обычный executable

После отдельной прогревочной пары, исключённой из итоговой статистики, — три чередующиеся пары,
по 120 измеренных кадров. Исходные коллайдеры и настройки сохранены.

| | Baseline | Финальный renderer |
|---|---:|---:|
| Парные frame means, мс | 90,472 / 92,286 / 94,865 | 63,566 / 64,378 / 65,763 |
| Медиана frame mean, мс | 92,286 | 64,378 |
| 1000 / медиана, FPS | 10,84 | 15,53 |
| CV средних запусков | 2,39% | 1,72% |

Frame time ниже на 30,24%, каждая пара прошла выбранный порог 2%.
Короткая серия достигла промежуточных 66,7 мс; цель 50 мс не достигнута.
Это FPS обычных rendered frames, без FG. Это не обещание устойчивых 15 FPS
на поздних волнах. GPU clocks не фиксировались, температура в scored серии
71–78 °C; alternating order уменьшает, но не устраняет thermal drift.
SHA256 финального frozen-input executable:
`d44198ceb88c01cd5d20ed6c6218a71d07a2f35fd43ae36917ff4c8436f2d4a0`.

### Отложенный двор и сопоставление симуляции

Чистые диагностические запуски по 121 кадру, первые 11 кадров исключены
из frame statistics: среднее 89,601 → 65,763 мс, p95 98,948 → 72,777 мс.
За весь запуск выполнено 281 → 303 fixed ticks (4,683 → 5,050 с симуляции).
Здесь нельзя приписывать разность Jolt ms/tick оптимизации физики: состояния
и realtime интервалы различаются.

В одинаковом simulated-time окне 2..3 с: 24 / 21 кадр и среднее active bodies
230,083 / 228,667, total bodies=381; frame mean 87,327 → 64,450 мс,
shadow wait 17,341 → 0,160 мс, snapshot 16,759 → 12,199 мс.
Это контроль после выбора вариантов, не дополнительный scored A/B с тремя парами.
Предварительные `courtyard-*.csv` включили Jolt profiling из-за ошибки очистки env
и исключены из итоговой статистики; используются повторные `courtyard-clean-*`.

### Свежий GPU Trace

Nsight Graphics 2026.4 / driver 617.42, 3 кадра, 298,353 мс суммарного
frame interval, Throughput Metrics, single pass, Time Every Action,
unaltered clocks. DIRECT queue, 5283 actions и 5052 barriers.
GR Engine Active=34,58%; сумма неперекрывающихся верхних GPU markers
в среднем 34,47 мс/кадр. Frame intervals 102,319 / 98,494 / 97,541 мс
включают CPU gaps, это не стоимость одних GPU shaders.

| GPU marker | Среднее, мс/кадр |
|---|---:|
| Local shadows | 14,185 |
| Sun shadows | 6,453 |
| GBuffer | 1,892 |
| Fluid surface | 1,877 |
| Screen-space reflections | 1,770 |
| Clustered deferred lighting | 0,806 |

Внутри local shadows: copy 1,062, clear 0,642, cull 2,652, draw 8,035 мс;
около 302 записей clear/cull/draw на кадр. Эти дочерние scopes уже входят
в родительские 14,185 мс. На первой local-shadow области SM throughput=9,27%,
VRAM throughput=36,33% от peak sustained; это не доказательство конкретной
единственной причины stalls. Следующий эксперимент — уменьшение числа
per-face dispatch/draw/барьеров и входных templates с отдельным измерением
grass expansion, сохраняя все необходимые грани и depth/HDR equivalence.

Native launch дважды завершился `NVPA_STATUS_INTERNAL_ERROR` в sampler.
Fallback CLI записал пригодный trace: native reader его открыл и прочитал
все три кадра. Старый wrapper ошибочно искал экспорт `BASE`, фактический
каталог — `BASE_UNLOCKED`; поэтому его ненулевой exit не принят за провал
самого capture. Есть одно предупреждение о трёх отсутствующих shader metrics;
их значения не используются. PC sampling выключен, shader stall claims нет.
Capture — позднее состояние активных waves с исходными 381 телом,
а не сравнение со старой GPU цифрой 16,86 мс.

Второй capture добавляет только диагностические `Grass expansion` markers
и wave counters. Trigger — frame 200; trace сообщает start frame 199.
UTC-корреляция CSV с trace, включая предшествующие 300 мс возможных CPU
producer frames, показывает simulated time 6,733..6,833 с, первую волну,
переход Lift→Fall и 379 active dynamic bodies. Это подтверждает активную волну,
а не пустой/спящий snapshot. Три кадра, 301,490 мс общего frame interval.

Grass expansion: 109 вызовов / 3 кадра, суммарно 2,833 мс, то есть
36,33 вызова и 0,944 мс/кадр. Из них 0,799 мс внутри local shadows,
0,146 мс внутри sun shadows; camera-view expansions в этом capture отсутствуют.
`Bistro wind and grass roots`=0 мс соответствует отсутствию GPU-работы
в этом warm analytic-wind preparation pass и не означает отключение grass.
Общие GPU roots здесь 27,719 мс/кадр, local shadows 12,436, sun 5,957,
lighting 0,541, SSR 1,154 мс. Эти числа не сравниваются с первым trace как
ускорение: отличаются фаза волны и динамические частоты/температура GPU.
Grass marker уже входит в parent shadow timings. Основной резерв local
shadows на этой камере не объясняется одной grass expansion.

### Длительная проверка

Выполнены отдельные 600 с TimeScale=1 и 600 с pause/TimeScale на маршруте.

Чистый `final-timescale1`: requested=600,089 с, 7512 ticks / 125,200 с
симуляции; 17 начатых и 16 завершённых seeded waves, все три фазы наблюдались.
TimeScale всегда 1, paused frames=0, total bodies всегда 381.
Конечный debt=474,889 с. После прогрева он продолжал добавлять примерно
46–49 с за минуту. Среднее frame time=87,983 мс, p95=116,813 мс;
поздние Jolt intervals около 22–33 мс/tick. Короткие 15,53 FPS не описывают
устойчивую позднюю нагрузку. Clock continuity проверена по всем кадрам,
requested/real-frame-delta median=0,999992. Jolt profiling выключен.

D3D12MA allocation count=1145 и bytes=3 467 910 000 оставались постоянными
на периодических samples. Working set 2,299..2,374 GB,
private bytes 5,979..6,114 GB; monotonic allocation growth в пределах прогона
не наблюдается. NVML memory относится ко всему GPU, а не только процессу.
Это ограниченная десятиминутная проверка, не доказательство отсутствия утечек.
Исходный baseline soak шёл со стартовой камерой, финальный — с маршрутом;
разность физического прогресса этих двух запусков не является чистой A/B-оценкой.

Цель стабильного долга с исходной collision fidelity не достигнута.
На поздних контактах один Jolt tick часто дороже 16,67 мс сам по себе.
Clock, fixed step и debt не изменялись ради достижения frame-time цели.

Чистый `controls-soak`: requested=600,087 с, 7183 ticks / 119,717 с
симуляции, конечный debt=480,370 с. Среднее frame time после прогрева
88,122 мс, p95=125,775 мс. На 162 paused frames нет ни одного fixed tick;
simulated time постоянен (39,6 с), debt постоянен (140,373 с).
Максимальный разрыв между предыдущим debt-after и следующим debt-before —
0 мс в точности CSV. Медиана requested/real-frame-delta для TimeScale
0,5 / 1 / 2 равна 0,499965 / 0,999990 / 2,000148.
Это корректность запрашиваемого времени, не обещание удвоенного фактического
прогресса при перегруженной физике. Jolt profiling выключен.
Сохранение большого долга подтверждено; его устойчивость не достигнута.

Первый файл `final-soak.csv` исключён из timing-приёмки: пустые env-переменные
Windows сохранились, и проверка существования ошибочно включила Jolt profiling
и controls. В harness исправлено удаление отсутствующих flags через
`Remove-Item Env:...`; production-код не менялся. Обычные scored executable
не содержат этих флагов. Повторные запуски сохраняются под другими именами.

### Совместный checkout с отдельной физической правкой

Дополнительно выполнен `combined-timescale1` с renderer этой итерации и
`BistroProps.h` из `f5f7d91`; остальные frozen scene inputs те же.
Это отдельная проверка уже разрешённых в другом чате box proxies, а не
подтверждение сохранения исходной collision fidelity и не вклад renderer.

600,048 с requested, 23437 ticks / 390,617 с симуляции,
51 начатая / 50 завершённых waves, 381 тело, пауз нет, TimeScale=1.
Clock discontinuity=0 мс, конечный debt=209,431 с.
Frame mean=83,748 мс, p95=107,356 мс; Jolt=5,616 мс/tick в среднем
по этому run, physics=20,528 мс/кадр. Состояния контактов отличаются от
исходных shapes: это не paired ms/tick сравнение одинаковых контактов.

Даже здесь `wall_limit` срабатывал на 99,4% warm frames, а за последние
минуты долг продолжал расти. При ~84 мс render/update кадре нужно в среднем
около пяти fixed ticks/кадр для realtime; фактически около 3,37.
Следующий throughput-разбор должен рассматривать вместе renderer cost,
стоимость полного fixed tick и существующий wall budget 1/60 с,
сохраняя fixed dt и накопленный долг. Увеличение budget в этой итерации
не проверялось и не вводилось.

GPU allocation count/bytes также постоянны: 1145 / 3 467 910 000.
Эта проверка показывает, что headless-ускорение physics нельзя напрямую
перевести в выигрыш общего FPS или обещание realtime.

### Проверки

Все 20 сборок финальной матрицы завершились с exit=0:
Debug App/Editor и затронутые suites, Release App и соответствующие suites.
`RenderPipelineTests`, `ObjectDataJobsTests`, `LightingGpuTests`,
`PhysicsTests`, `BistroPhysicsTests`, `CollisionGeometryTests`,
`FrameGenerationTests`, `SecondGroupTests` прошли в Debug и Release.
`BistroRenderGraphTests` прошёл в Debug: direct/async, pooled/dedicated,
AO rebind, odd resize и фактические grass HDR readbacks.

После последней правки точности bounds повторно выполнены Debug/Release
RenderPipeline/ObjectData/Lighting suites. В Lighting включён D3D12 Debug Layer.
CPU oracle сравнивает независимые холодные и прогретые кэши на 1/4 потоках,
набор со 128 дополнительными случайными casters и 65 point/spot lights, edits/removal/classification,
range/cone/projection и отключение/возврат shadows. GPU fixture сравнивает
точные depth и HDR cached/full-scan результатов для static и dynamic caster.
ObjectData проверяет все frame slots, reimport, extreme-caster removal,
прямой bounds/classification edit application primitive и generation reuse.
Bounds unit test покрывает порядок reduction, пустой набор, объединение классов
и тонкий extent при center=1e20, который мог теряться даже в double endpoints.

Первый запуск SecondGroup завершился ошибкой asset path из runtime CWD;
повтор из корня репозитория прошёл в обеих конфигурациях. Production-fix не нужен.
DLSSG на RTX 3060 недоступен: проверен ожидаемый fallback; работа FG на другом
поддерживаемом GPU здесь не проверялась. Контейнмент props не исправлен:
исходные BistroPhysics tests сохраняют 2 escaped props Debug / 5 Release.

Отдельный original-fidelity headless stress `BistroPhysicsBaseline.exe 16 16 1`
прошёл: 6064 dynamic bodies + 2 static meshes, 891 ticks, одна полная seeded
wave, peak speed=3000, минимальные launch/scatter=500/700 cm/s.
Elapsed=188,924 с. Это fixed-tick stress без renderer и без realtime-утверждения.
Выход за modeled geometry наблюдался у 381 props, static chord candidates=428;
последнее является диагностикой кандидатов, а не доказанным числом туннелирований.
Containment не входит в результат этой оптимизации и остаётся проблемой.
Долгий GPU/realtime stress ×16 не выполнялся.

Финальный совместный checkout дополнительно прошёл обычный App smoke в
Debug и Release: по 120 измеренных кадров, 25 graph passes и particle
budget=250000, exit=0. Это единичные smoke, не performance A/B.
Debug INI fingerprint после всех запусков совпадает с исходным.

Не проверялись другие GPU, Release performance A/B и полный pixel-by-pixel
Bistro replay во всех фазах. Depth/HDR equivalence относится к указанным
GPU fixtures. Snapshot <=9 мс и graph recording <=8 мс также не достигнуты;
отклонённый более сложный snapshot cache не возвращён ради отдельного scope.

## Локальные коммиты

- `fb84482` — static shadow visibility cache и CPU/GPU oracle tests.
- `ca21c3d` — static signatures, однопроходные conservative bounds и regression tests.

Коммиты этой итерации не включают физику `f5f7d91`, созданную другим чатом.
Push и PR не выполнялись. Из пяти code variants оставлены два, три удалены
как отрицательные или не прошедшие выбранный paired критерий. Остальные
плановые цели явно оставлены открытыми, включая стабильный physics debt.

## Артефакты

`Build/DebugNext-20261106/` (Git-ignored): spec/log, original/candidate
fingerprints, `measure-next.ps1`, `prepare-next.py`, build/test logs,
paired JSON, CSV scopes, physics fixed-tick comparison и summaries.
Диагностические build scripts читают MSBuild tlogs после завершения сборок.
Пересечение чтения tlog с финальной сборкой привело к ошибке подготовки;
частичная diagnostic сборка исключена, итоговые copies готовятся последовательно.
