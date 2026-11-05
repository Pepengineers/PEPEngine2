# Оптимизация Debug Bistro

Исходная версия: `4b76470`. Все измерения ниже сделаны заново после исправления
collision geometry, с одним и тем же runtime INI обычного Debug App.
Данные старого нагруженного прогона на 1,5 с/кадр не используются как baseline.

## Почему было 4620 узлов

Проверен DOT исходного графа. В нём было по **1530** узлов трёх типов:
`Geometry view mesh batches`, `Bistro grass expansion`, `Geometry source batches`.
Оставшиеся 30 узлов включали техники, их завершение и общую подготовку:

```
4620 = 30 + 1530 × (mesh view + grass expansion + grass draw)
```

Это количество потенциальных view slots, а не отдельных техник отображения.
Узлы создавались заранее и для неактивных видов; их Execute затем мог ничего
не делать. Каждый всё равно проходил общий executor, собирал access lists и markers.

Теперь граф содержит **25 узлов**. Одна geometry technique сама обходит активные
виды и участки: mesh draw, expansion, немедленное потребление общей arena, затем
EndGeometry. Запись inputs/outputs техники и её общий порядок сохранены.
Изменение числа видов/участков больше не перестраивает topology графа.

## Выполненные изменения

- Удалён безусловный temporal reset при каждом кадре дольше 250 мс. Реальные
  suspend/resume transitions сбрасывают историю однократно независимо от наличия
  frame generation. Camera cut, resize, resource/environment resets сохранены.
  Уничтожение SSR context по-прежнему корректно ожидает GPU.
- Grass patch проверяется по консервативным bounds и frustum **его текущего вида**,
  включая shadow/probe views. Main-camera visibility не используется для удаления
  off-camera casters. Для отсечённых patches не создаются draw constants,
  expansion и draw не записываются.
- Подготовленные grass outputs индексируются по view key/stage/patch. Константы
  выделяются блоками по 64 записи на frame slot; декларации draw reads перечисляют
  эти buffers, а не constants каждого отдельного output.
- Неизменные static mesh instances переиспользуют extracted primitive и signature.
  Кэш проверяет mesh handle/content revision, submesh, material identity/revision,
  classification/visibility, instance data и world matrix. Динамические объекты
  обновляются каждый кадр; удаление primitive и generational handles сохранены.
- Для перестраиваемого skeletal entity сначала загружаются source meshes, затем
  одним резервированием обеспечивается место под его output vertices. Существующий
  allocator распределяет части из свободного диапазона вместо повторного роста
  vertex buffer для каждой части. Per-part source upload в CreateMesh сохранён.

При полноценном запуске обнаружена и исправлена интеграционная ошибка объединения:
mesh draw должен работать в split mode, иначе он автоматически потреблял grass
до её expansion. В BistroRenderGraphTests теперь устанавливается настоящая трава;
тест не ограничивается пайплайном без application geometry.

## Измерения

Ryzen 7 5800HS, RTX 3060 Laptop 6 GiB, NVIDIA 617.42, Debug x64. Стартовое окно
1280×800, стандартная камера Bistro у входа, `Render.NumFrames=3`, bounds off,
particle scale 1 / budget 250000. Качество эффектов и collision settings не снижались.
Каждый запуск — 61 кадр; первый кадр и uploads исключаются штатным loading benchmark.
Во время измеренных прогонов не выполнялись наши сборки или другие тесты.

| Метрика | До | После |
|---|---:|---:|
| Среднее следующих 60 кадров, мс | 365,388 | **173,789** |
| Производная частота, FPS | 2,74 | **5,75** |
| Узлы render graph | 4620 | **25** |
| Initialize, с | 35,183 | 26,387 |
| Готовность первого полного кадра, с | 59,649 | **36,377** |
| GPU allocations | 2473 | **1145** |
| GPU blocks | 1895 | **567** |
| GPU allocation bytes, MiB | 3390,26 | 3307,26 |
| Transient logical/physical textures | 16 / 10 | 16 / 10 |

Ускорение в этом сценарии — **2,10 раза**, frame time уменьшился на 52,4%.
Оба App завершились с exit 0. Это отдельные короткие прогоны, не статистика
обхода всей сцены. Частоты/температура не фиксировались; первые physics ticks,
probe bake и SDK preparation входят в соответствующие стартовые окна.
Рост FPS сам по себе не доказывает отсутствие physics backlog.

Артефакты: `Build/RenderPerf-20261105/before.json`, `after.json`, matching DOT,
`before-exe.json`, build/test logs. Build игнорируется Git.

Воспроизведение из корня репозитория:

```powershell
Build\bin\x64\Debug\App\App.exe `
  -Loading.BenchmarkOutput=D:\Project\PEPEngine2\Build\render-performance.json `
  -Loading.BenchmarkFrames=61
```

## Проверка

- App и App.Editor собраны в Debug/Release; профиль обычного Debug App завершается с exit 0.
- RenderPipelineTests: primitive/batch contracts, preparation phases, application
  composition, links и lifetime.
- ReconstructionTests: реальные pause/resume transitions через RenderModule,
  отсутствие повторного reset при неизменном состоянии.
- ObjectDataJobsTests: actual GPU instance/transform/material uploads, 1/4 потока;
  static snapshot warm reuse, material/transform/visibility/classification changes,
  same-handle reimport, removal/replacement и три последовательных кадра >250 мс
  без повторной invalidation history. Вместо открытой треугольной оболочки fixture
  bottle использует замкнутый тетраэдр, совместимый с действующим collision baking.
- SecondGroupTests: GPU skinning/current+previous poses, reserved output ranges,
  grass view culling/arena ordering, 65 prepared views с разными проекциями во всех
  frame slots, wind, water, particles, direct/async/direct. Проверяются реальные
  depth readbacks с непустым покрытием и D3D12 debug-layer errors.
- SSRTests: hits/HDR/fallback/roughness/history/Off/resize/jitter/motion/queued slots.
- BistroRenderGraphTests с травой: 25 узлов, идентичный HDR в pooled/dedicated и
  direct/async режимах, обе позиции AO, resize и Native/XeSS transitions.

Все перечисленные проверки выполнены в Debug. В Release дополнительно прошли
RenderPipelineTests, полный ReconstructionTests, SecondGroupTests и
BistroRenderGraphTests. Усиленная проверка 65 views с непустым покрытием обеих
проекций повторно прошла в Debug и Release. SecondGroupTests, SSRTests и
BistroRenderGraphTests завершились без ошибок D3D12 debug layer;
проверка whitespace diff также прошла.

`ce-code-review`, status `complete`, run `20261105-render-perf-4b76470`: correctness,
стандарты AGENTS.md, testing, maintainability, performance и adversarial, включая
повторную проверку добавленных регрессий. Подтверждённых замечаний нет.
Внешний peer не запускался; adversarial выполнен локальным reviewer.
Receipt: `Build/RenderPerf-20261105/review/20261105-render-perf-4b76470/review.json`.

Сборки сохраняют существующие предупреждения C4267 и отсутствующие vendor PDB.
Оптимизации не отключают assertions, Debug CRT или D3D12 validation в тестах.

Не проверены длительный soak, scale 16 и другое оборудование. Оставшееся время
кадра требует отдельного профиля; результат не означает достижение 60 FPS.
