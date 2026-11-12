# Перенос отмеченных частиц на дополнительную GPU

Дата: 2026-11-13. База: `f82180289b04a9b49d6530709e27ed095a6f6de9`,
ветка `codex/bistro-rendering`.

## Поведение и устройство

- `GDX12ParticleEmitterSettings::SharedSimulation` — явная отметка, по умолчанию
  выключенная. Настройки уже копируются компонентом и `CollectParticleEmitters`,
  поэтому отдельной второй отметки в ECS нет.
- Централизованная проверка `CanOffloadParticleEmitter` допускает только отмеченные
  эмиттеры с `DepthCollision=false`. Все допустимые отмеченные эмиттеры переносятся.
  `DepthCollision=true` сохраняет локальную симуляцию и коллизии, с сообщением при
  входе в это состояние. Soft particles, box/volume и interaction не запрещают перенос.
- Один `GDX12ParticleOffload` владеет вторичным device, queue, worker, постоянными
  solver pools всех перенесённых эмиттеров и общим трёхслотовым обменом. Вода и
  частицы выбирают одну дополнительную карту; конфликт индексов отклоняется.
  Отдельных offload-контекстов на каждый эмиттер нет.
- `Render.Particles.SecondaryAdapter` выбирается при старте. `-1` следует выбранному
  `Render.Fluid.SecondaryAdapter`; без него остаётся обычный локальный путь.
  Отсутствующая или основная карта сохраняет локальный путь.
- Вторичная карта исполняет те же шесть solver stages. Вариант шейдера без depth
  не содержит depth collision и не использует camera/depth bindings. Solver root
  не содержит render texture range. Первичная симуляция пропускает перенесённые IDs.
- На принятый неизменяемый снимок выполняется один шаг с его фактическими dt,
  transform, settings, pause/time scale, reset revision и cumulative burst total.
  Пока worker занят, кадры не накапливаются в долг; продвижение может замедлиться.
  Повторного шага с тем же принятым снимком нет. Burst учитывается по разности
  cumulative totals; пауза не потребляет его. Reset отменяет предыдущую ревизию.
- Общий настоящий interaction field снимается на основной один раз на принятый
  batch, затем импортируется на вторичную один раз для всех потребителей. Параметры
  области/высоты/разрешения относятся к этому же снимку; исходный borrowed resource
  вторичным потоком не разыменовывается. GPU-сигнал готовности отправляется после
  graph prefix, не после остальной сцены. Без field worker стартует по CPU-снимку.
- Основная импортирует только целиком завершённые публикации. Ready/free fences
  запрещают перезапись читаемого слота; обычный кадр не ждёт solver на CPU.
  Реальные display/received/interaction buffer accesses объявлены графу. Обмен
  расположен перед opaque depth. Выделение/смена состава/reset/shutdown могут ждать.

## Общий payload и история

Размер публикации: `32 + 32 * emitterCount + 36 * sum(capacity)` байт.
Передаются header с sequence/layout generation, ID/reset revision/offset/capacity/
alive count/display base каждого эмиттера и фиксированные диапазоны render data.
Данные частицы: position, age, lifetime, rotation, size random и 64-битный birth ID.
Velocity, solver Previous, alive/dead lists, scratch и solver counters не передаются.
Передаётся выделенная ёмкость, включая мёртвые slots; синхронного live-count readback нет.

На публикацию записывается одна `CopyBufferRegion` packed -> shared и одна
shared -> received. Число GPU packing/history dispatches может зависеть от числа
эмиттеров. Копии общего interaction field и локальные remap-копии при изменении
раскладки учитываются отдельно и не выдаются за transport payload copies.

Отображение реконструирует предыдущие position/age/rotation по предыдущему
**показанному** состоянию и birth ID. Повторный показ даёт previous=current;
новая частица в старом slot не получает историю умершей. Пустые/умершие состояния
убирают частицы. Camera-history reset не стирает показанные позиции. Culling,
общая SortedAlpha сортировка, текстуры, все blend/geometry modes и raster depth
остаются на основной и используют смесь локальных и импортированных частиц.

**Смена GPU размещения явно сбрасывает живые частицы затронутого эмиттера.**
Диагностика сообщает `placement reset`. Уже потреблённые bursts переносятся как
CPU accounting baseline; они не воспроизводятся повторно. Pending burst сохраняется.
Увеличение capacity сбрасывает только соответствующий pool. При удалении, reset
или смене раскладки других эмиттеров их постоянные secondary pools сохраняются;
последнее показанное состояние неизменённых эмиттеров локально переносится в новые
диапазоны. Старые slots/offsets после смены поколения не импортируются.

## Подтверждённые проверки

Локально, NVIDIA RTX 4080 SUPER:

- Debug x64 сборки App, renderer, `ParticleOffloadGpuTests`, `SecondGroupTests`,
  `BistroRenderGraphTests`, `WaterOffloadBistroTests` — успешны.
- `ParticleOffloadGpuTests.exe --local --primary 0`: выбор/копирование настроек,
  compact payload и независимый metadata oracle, repeat-display motion, reuse birth
  identity, смерть/пустое состояние, camera reset и отклонение старого поколения —
  пройдены с D3D12 debug layer и GPU-based validation.
- `SecondGroupTests.exe --gpu-validation`: пройден после изменения solver root;
  интеграторы, pending burst/reset/pause, depth/box/volume/interaction, DIRECT/COMPUTE,
  spawn/refill до миллиона частиц, все существующие blend/geometry и вода сохранены.
- `BistroRenderGraphTests.exe --gpu-validation`: pooled/dedicated HDR identity,
  оба положения AO, odd resize и переключение Native/XeSS — пройдены.

MisSpec, **NVIDIA GeForce RTX 5050**, index 0 / LUID `0:47217` primary;
**Intel Arc B570**, index 1 / LUID `0:59621` secondary:

```text
ParticleOffloadGpuTests.exe --validate --primary 0 --secondary 1
```

Release x64, D3D12 debug layer и GPU-based validation включены, exit 0.
AMD Radeon, присутствующая в hardware list, не используется для расчёта.

| Проверка | Результат |
|---|---|
| AnalyticDrag и SemiImplicitEuler, 3 эмиттера, capacity 17/257/33 | По 12 реальных одинаковых шагов на обеих картах |
| Реальные box/volume/interaction, dt, seed, transform, pause/time scale, reset/burst | Пройдены; alive state сравнивается по 64-битному birth ID |
| Position/velocity/age/lifetime/rotation | Конечны и укладываются в заданный abs+rel допуск |
| Общий payload конкретных публикаций, включая metadata | Побитное совпадение отправленного и принятого, без допуска |
| Слоты | 12 публикаций на режим; каждый из трёх слотов использован 4 раза |
| Copies для трёх эмиттеров | 12 export + 12 import на 12 публикаций, по одной на сторону |
| Размер публикации | 11 180 байт |
| Interaction field | 12 общих снимков на 12 batches, без копии на эмиттер |
| Смена состава/диапазонов | Unaffected solver pool побитно сохранён |
| Production placement | 0/1/много, mixed local/imported, soft/box/volume, depth toggle, opt-out, reset/capacity/removal — пройдены |
| Busy worker | 100 кадров без CPU wait между ними, pause и reset; итоговые pending burst counts строго 100/100, локальных pools нет |
| D3D12 messages на обеих картах | Ошибок не обнаружено |

Допуск для математики: `abs(a-b) <= 1e-4 + 1e-4 * max(abs(a),abs(b))`.
Это ограничение короткой одинаковой последовательности межвендорных FP-вычислений
(включая exp/trig); транспорт сравнивается строго побитно. Совпадение unordered
alive-list order не требуется. Ни максимум ошибки, ни долгая численная устойчивость
этим отчётом не заявляются.

Во время подготовки fixture исправлен его `ActiveCameraCBufferIndex`: значение
по умолчанию `UINT_MAX` вызывало debug-layer `SET_ROOT_CONSTANT_BUFFER_VIEW_INVALID`
в однокарточном reference Execute. Cdb показал именно неверный CBV oracle; после
установки индекса 0 полный двухкарточный тест прошёл. Ранние exit 2173 не являются
результатом проверки транспорта и не доказывают дефект драйвера Arc.

Логи: локальные `Build/particle-mgpu-*.log`; MisSpec
`C:/Users/MidSpec/Documents/PEPEngine2-water-only-mgpu/Build/particle-evidence/validation.log`.
Использован отдельный тестовый checkout; пользовательский основной checkout MisSpec
не переключался. Передача файлов и одноразовая интерактивная задача разрешены пользователем.

## Оставшаяся проверка и пределы

Совместный Release `WaterOffloadBistroTests --particles` на той же паре завершился
с exit 0 и обычным D3D12 debug layer. Вода содержит 100 980 частиц. Подтверждены:

- Видимые и конечные color/motion outputs смеси локальных и импортированных
  частиц. Локальный и два импортированных SortedAlpha эмиттера используют общий
  sort stream; это smoke отображения, не независимый oracle порядка каждого пикселя.
- Пять isolated imported-only случаев: billboard additive/sorted, Firefly mesh
  weighted/additive и WaterDrop sorted. Для каждого локальные pools отсутствуют,
  есть видимые particle coverage и конечные motion vectors.
- Настоящий interaction field из COMPUTE grass preparation, затем переключение
  producer обратно на DIRECT. Soft particles и локальный отмеченный depth emitter
  присутствуют в смешанном режиме.
- Смена DepthCollision, reset, capacity и disable частицы вместе с работающей водой.
  Production routing/data outcomes отдельно проверены основным GPU harness.
- Water pause: 85 686 водных пикселей, движущихся 0. При движении контейнера:
  110 364 водных пикселя с конечными ненулевыми motion. Reset/recreate/odd resize/
  disable/reenable и queued shutdown пройдены; ошибок debug layer не обнаружено.

Полный Bistro-прогон с GPU-based instrumentation остановлен: неинвазивный стек
показывал затраты в неизменённом DoF/FFX execution. Ни отказ драйвера, ни deadlock
offload этим наблюдением не установлены. Итоговый scene smoke использует обычный
debug layer; основной math/transport/placement harness прошёл с GBV на обеих GPU.
Контрольный HDR-preview просмотрен; исходники кадров и readbacks остались в
`Build/particle-evidence/bistro-images` тестового checkout. Финальные логи также
скопированы локально в `Build/particle-evidence/`. Разрешённая одноразовая задача
`CodexParticleGpuValidation` удалена; тестовые/debugger процессы завершены.

Не заявляются сравнительный FPS, calibrated GPU timeline overlap, длительный soak,
устойчивость birth counter на 2^64 рождениях или поведение других пар адаптеров.
Принудительное насыщение всех output slots одновременно отдельно не воспроизводилось;
проверены четыре оборота каждого слота и сохранность payload/fences в этих прогонах.
Время primary exchange prefix входит в graph execution; отдельного обещания экономии
полного GPU-времени кадра нет. Автоподбор качества/частоты, freshness/debt и прежний
MGPU runtime не восстанавливались.

Ревью: `ce-code-review`, артефакт `Build/particle-review/20261113-particles`,
correctness/standards/testing/maintainability/reliability/performance; подтверждённых
замечаний к коду нет. Проверки ревью не заменяют аппаратные результаты выше.
Посторонние `BistroAdditionsTests.cpp`, terra-validation report, `imgui.ini` и
`tools/tests/__pycache__` не включаются в коммит. Push не выполняется.
