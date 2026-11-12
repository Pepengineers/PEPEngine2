# Bistro: атрибуция CPU/GPU-стоимости теней и кандидаты

Дата: 12 ноября 2026. Статус: диагностический результат для ревью. Production
изменений и принятой оптимизации нет. Ускорение >=20% не подтверждено.

## Результат и область

Сохранённый Nsight capture подтверждает значимую стоимость parent ranges:
Sun shadows — **3,147705 мс**, Local shadows — **4,645171 мс** в среднем по восьми
кадрам. В Local найдены 208 строк каждого вида clear/cull/draw. Это основание
для проверки последовательного сброса/отбора команд, но не доказательство того,
что изменение сброса ускорит весь проход. Самый малый кандидат — использовать
существующий `LocalShadowPass.hlsl::ClearCS` вместо копирования четырёх байтов
через COPY_DEST перед каждой гранью. Следующий, более дорогой кандидат —
независимые counter/command ranges и пакетная подготовка граней.

Результат относится к frozen accepted snapshot
`7acaf95089541909925d8e305fa3b0ba55ff5cc8`, предоставленному perf-координатором.
Он не характеризует текущий dirty MGPU renderer. При начальном осмотре общий
HEAD был `485daaa38b11bda8151dfae7b1f38b72284830e8`; оба shared shadow headers
уже содержали чужие изменения policy/display snapshots. Их не редактировали.
Реальный солнечный проход — `GDX12CascadedShadowPass.h`; файла
`GDX12ShadowPass.h` здесь нет.

Пользователь разрешил продолжить диагностику в Build, подготовить отчёт и
передать его на ревью, оставив общие файлы теней до согласования после MGPU.
Координатор отдельно ограничил текущий этап диагностикой и private Build copies.
Build/GPU window этому чату не выдавался. Сборки, C++/shader tests, приложение и
capture в этой итерации не запускались. GPU/build ресурс этим чатом не занят.

## Входы и воспроизведение

Raw trace: `Build/PerformanceAnalysis/gputrace/BistroProfile_2026_11_12_02_34_05.ngfx-gputrace`.
Экспорты `BASE_UNLOCKED/*.xls` — TSV. `REPRO_INFO.xls` подтверждает Ryzen 9 5950X,
RTX 4080 SUPER / AD103, driver 610.88, Windows build 26200, HAGS on.
По зафиксированным входам координатора: Release /O2 с PIX diagnostic markers,
1280×800 output, effective XeSS 985×616, FG Off, VSync Off; неподвижная камера
(-300,200,20)/(0,117,0), seed 12345; 949 bodies, 10 people, 5 foxes,
100980 water particles, particle capacity 500000, полная сцена.
Эти counts и settings повторно в приложении здесь не измерялись.

Самостоятельный read-only разбор:

```powershell
python Build/ShadowPerformance/analyze.py
```

Выход — `Build/ShadowPerformance/attribution.json`: полные per-frame samples,
номера строк, число marker rows, исключения и SHA-256 пяти экспортов, trace и
четырёх accepted shadow sources. Исходные evidence и accepted sources читаются,
не изменяются. Входной trace имеет SHA-256
`63df80cab9cce74449441d9ead9fc10f5c41d3bc5b0aa875a3df3788769b667a`.
Экспорт D3DPERF_EVENTS —
`882f1c2df49f09db92f34eb20cac8e55d3238940cd55ab5bf2ff15dd204c28d6`.

## GPU: что действительно выделено

| Parent / marker | Строк в экспорте | Среднее, мс | Min–max по восьми кадрам, мс |
|---|---:|---:|---:|
| Sun shadows parent | 1 | 3,147705 | 3,134270–3,154210 |
| Sun / Cascaded shadow maps | 1 | 0,016212 | 0,015968–0,016928 |
| Sun / Bistro grass expansion | 3 | 0,295924 | 0,294080–0,297600 |
| Local shadows parent | 1 | 4,645171 | 4,623100–4,675330 |
| Local / Local shadow copy | 1 | 0,189880 | 0,186336–0,195391 |
| Local / Local shadow clear | 208 | 0,625762 | 0,375072–0,770902 |
| Local / Local shadow cull | 208 | 0,481682 | 0,474534–0,487424 |
| Local / Local shadow draw, без строки 665 | 208, валидных 207 | 2,481912 | 2,432863–2,535041 |
| Local / Bistro grass expansion | 22 | 0,694960 | 0,525121–1,021984 |

Для повторяющихся имён таблица показывает сумму **только одноимённых** marker
rows в каждом кадре, затем статистику восьми сумм. Разные строки таблицы нельзя
складывать: scopes могут быть вложены. В частности, expansion может принадлежать
draw; таблица не строит exclusive decomposition и не вычисляет остаток вычитанием.
Число marker rows не равно числу GPU-команд, visible casters или dirty faces:
пустые dynamic faces выходят до clear/cull/draw, а static и dynamic paths могут
записывать одинаковые маркеры. Их разбиение экспорт не даёт.

Строка 665 `Local shadow draw` содержит 137,649–225,739 мс в последних четырёх
столбцах при полном кадре 25,669–37,916 мс и parent около 4,6 мс. Исключена
**вся строка**, включая первые четыре правдоподобных значения. Поэтому subtotal
draw неполный. Остальные nested samples не превышают соответствующий parent,
но эта проверка сама по себе не доказывает точность timestamps.

Capture использует Time Every Action и пересекается с внешними CPU builds.
Наблюдаемый full frame 25,6689–37,9164 мс не принимается за ordinary FPS/clean bound.
Сумма parent ranges около 16,3 мс согласуется с hardware graphics-active estimate
16,15–16,28 мс из предыдущего разбора координатора; это sanity check, а не
независимая верификация query timings. Перед A/B нужен clean повтор capture и
отдельная сверка shadow GPU timestamps. В этой итерации их нет.

Sun DRAM throughput около 75% peak, Local около 32% в исходном
`trace-summary.json`. Они не устанавливают причинный bandwidth bottleneck.
Выводов об occupancy, stalls или эффективности memory layout этот отчёт не делает.

## Связь с кодом и недостающая CPU-атрибуция

В accepted `GDX12LocalShadowPass.h`:

- `Prepare` (строка 96) вычисляет dirty/empty masks и готовит static child.
  `ShouldRenderLocalShadow` (вызов около 160) сохраняет EveryFrame contract.
- `BeginGeometry` (223) очищает новые карты и готовит bindings.
- `ExecuteGeometryView` (299) после empty-face early return переводит counter в
  COPY_DEST, копирует UINT zero, возвращает его в UAV, запускает CS culling,
  при необходимости skeletal batching, затем переводит commands/counter в
  INDIRECT_ARGUMENT и вызывает EmitGeometry. Это повторяется для граней.
- `BeginDynamicGeometry` (411) уже группирует static-depth copies. Повторное
  «пакетирование copy» не является новой оптимизацией: соответствующая работа
  принята раньше, см. `docs/research/2026-11-04-shadow-copy-batching.md`.

В accepted `GDX12CascadedShadowPass.h` `BeginGeometry` (129) сбрасывает четыре
counter за один dispatch и отбирает четыре cascade в одном CS. Scope `Cascaded
shadow maps` заканчивается с BeginGeometry, не охватывает все последующие draws.
`ExecuteGeometryView` (200) отдельно записывает draw каждого каскада. Поэтому
0,016212 мс этого маркера не объясняют parent 3,147705 мс: для следующего шага
нужны отдельные markers mesh draw / application draw / depth clear.

Восемь исходных CPU stack snapshots показывают LocalShadow::ExecuteGeometryView,
driver и graph recording. Они не выделяют длительность shadow CPU critical path.
Целиком приписывать graph_record теням нельзя. Для CPU-кандидата нужны scopes
Prepare/Begin/Bind/clear/cull/batcher/EmitGeometry/End, без двойного учёта static
child; счётчики dirty/static/dynamic/empty faces, dispatch, ExecuteIndirect,
depth clears, texture copies и фактически записанных transitions. Отдельно нужны
visibility cache invalidations и queue submission/idle intervals. Такие counters
не добавлены в production в этой итерации.

## Кандидаты после MGPU freeze

1. **Сброс counter существующим ClearCS.** Только local header: добавить PSO
   entry `ClearCS` с теми же SHADOW_CULLING/STATIC_ONLY defines, заменить
   CopyBufferRegion на Dispatch(1,1,1) в UAV state и UAV barrier перед cull,
   удалить ненужный upload zero buffer. `LocalShadowPass.hlsl:14` уже содержит
   этот entry; новый shader code не требуется. Следует измерить reset вместе
   с переходами и весь Local parent: compute dispatch/PSO change может оказаться
   дороже copy, поэтому это гипотеза, а не рекомендуемый production patch.
   В текущем trace clear subtotal равен примерно 13,5% Local parent. Даже
   гипотетическое устранение всего этого subtotal не обосновывает 20% выигрыша
   parent; эта оценка также ограничена overhead/вложенностью capture.

2. **Пакетная подготовка независимых face command ranges.** Один counter reset
   на набор граней, затем cull и draws без повторной перезаписи одного counter.
   Возможная область — local header + `LocalShadowPass.hlsl`, после отдельного
   согласования. Это потребует offsets, bounds/capacity checks, per-frame lifetime
   и контроля памяти; не создавать полные command buffers на все slices без
   измерения VRAM. Сохранить число кандидатов/кастеров, frustum tests, ordering
   application geometry и static→dynamic copy. Это более широкий эксперимент;
   сначала нужен результат кандидата 1 и точная атрибуция barriers/driver cost.

3. **Sun: сначала детализация draw.** Parent дорог, а уже пакетный clear/cull
   marker мал. Проверить mesh/application draws, depth clear и bandwidth/stalls
   с соответствующими counters. Конкретный shader/PSO patch пока не выбран.
   Сокращение разрешения, casters, grass или частоты обновления не подходит задаче.

Отклонено как выход за текущие условия: убрать EveryFrame force при совпадении
caster hash, уменьшить число граней/cascades/lights, обновлять тени реже или
переиспользовать старый dynamic depth. `LightingGpuSmoke.cpp` явно проверяет
продолжение EveryFrame обновлений и сохранение static cache. Кэш-signature сам
по себе не даёт права менять этот контракт.

## Проверка будущей итерации и зависимые callers

Изменение header достигает `CommonRenderPasses.h`, `RenderShadowRoutes.cpp`,
`GDX12ReflectionProbePass.h` (static local capture), LightingGpuSmoke,
PreparedShadowViewChecks и ReflectionProbeTests. Перекомпилировать нужно все
затронутые TU согласованного snapshot: relink одной старой библиотеки не проверяет
ODR/layout consistency inline-класса. Shared RenderModuleSnapshot,
ReflectionProbePass, Bistro/GrassRoute и water solver остаются у их владельцев.
Новый config/CVar/export для кандидата 1 не нужен.

После завершения/ревью MGPU согласовать точные frozen пути, SHA-256 и режим
primary/secondary с координатором. Проверять новую итерацию поверх этого состояния;
старые accepted sources служат диагностическим объяснением, не доказательством
пригодности текущему HEAD. Окно build/GPU должно быть выдано явно и сериализовано.

Приёмка требует не менее трёх paired A/B repeats с одинаковой timeline, seed,
camera, fixed ticks, warmup и counts. Порядок чередовать; process monitor должен
покрывать загрузку и measured interval. Хешировать exe/DLL/shaders/config и inputs
до/после; contaminated/timeout samples исключать с сохранением raw evidence.
Ordinary throughput измерять отдельно от stage instrumentation. Для выбранного
scope требовать >=20% сокращения без большего переноса стоимости; весь parent,
whole frame mean/p95, simulated seconds/ticks/debt фиксировать отдельно. Выигрыш
одного reset scope не выдавать за достижение 20% всего shadow pass или FPS.

Повторно использовать RenderPipelineTests, ObjectDataJobsTests и LightingGpuTests
(source `LightingGpuSmoke.cpp`), а для static local capture — ReflectionProbeTests.
В Lighting уже есть point+spot readbacks, unchanged/EveryFrame, light move,
generation/slot reuse, static/dynamic reclassification, caster removal, reference
full scan и face movement; это проверено чтением теста, не его запуском здесь.
Для нового кандидата нужны точные baseline/candidate depth readbacks, static и
moving geometry/lights, empty/zero-command paths, границы local faces/near/far,
alpha cutouts и skeletal/application geometry. Для Sun — cascade boundaries.
GBV/debug-layer correctness прогонять отдельно от performance. Покадровое
сравнение всей Bistro и проверка свежести MGPU snapshots остаются обязательными
за пределами покрытия локальной smoke fixture.

## Проверки и передача

`python Build/ShadowPerformance/analyze.py` завершился с exit 0: parser self-check
проверил исключение целой corrupt row, раздельную группировку parents и отказ
out-of-frame parent. На реальном экспорте assertions подтвердили 8 кадров, anomaly
row 665 и 208 local rows каждого вида. Сохранены все samples и input hashes. Дополнительно четыре accepted shadow sources
сверены с `git show 7acaf950:<path>` после нормализации CRLF; все совпали.
Production tests/builds/GPU runs не выполнялись; измеренного A/B нет.

На ревью передаются только этот отчёт и ignored Build evidence. Локальный коммит
отчёта создаёт общий ревьюер `01a1ccc0-05f1-7180-8756-b146dc187627` после READY;
исполнитель не коммитит и не выполняет push. Shared files не frozen этим отчётом
и не принадлежат данной итерации. Отчёт — диагностический, реализация и проверка
оптимизации зависят от последующего согласования MGPU state и выдачи окна.
