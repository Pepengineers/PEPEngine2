# Перенос только solver воды: реализация и границы проверки

Дата: 2026-11-12. База: `4801cd88e811d7433dec857f8e83b29cc91123e6`, ветка `codex/bistro-rendering`.
Задание: [water-only restart](../plans/2026-11-12-water-only-mgpu-restart.md).

## Статус

Реализован новый путь воды. **Двухкарточная работоспособность и ускорение пока не подтверждены.**
Локально доступна только NVIDIA GeForce RTX 4080 SUPER. На MisSpec по SSH подтверждены NVIDIA RTX 5050
и Intel Arc B570. Пользователь поручил передать локальный коммит в существующий репозиторий MisSpec;
для проверки используется отдельный worktree. Аппаратные результаты ниже дополняются после прогона.

## Реализация

- `GDX12WaterOffload` владеет постоянным вторичным устройством, очередью, heap дескрипторов, существующим
  `GDX12FluidSolver` и его буферами. Это отдельный путь только воды, без старого MGPU runtime.
- Фоновый поток последовательно запускает тот же `Record`, `PlanSteps`, hash/sort/density/integrate.
  Численные kernels `FluidSolver.hlsl` не изменены. Инициализация частиц выделена в общий `MakeSeed`.
- Вход каждого опубликованного результата содержит settings, transform, batch `Count/Dt` и накопленное
  число реально выполненных шагов. Параметры копируются в immutable на время batch constant buffer.
  Live worker использует прошедшее время собственного цикла, масштаб времени мира и воды; пауза
  останавливает новые шаги. Обновления входов принимаются между batch, без привязки к Present.
- Передаются только позиции `float3`: **12 × число частиц байт** на результат. Скорости, плотность,
  ключи, offsets, ping-pong и предыдущие позиции solver остаются на второй карте.
- Три отдельных shared cross-adapter heaps содержат placed buffers. Вторичная карта делает pack/copy,
  возвращает ресурс в COMMON и сигналит shared ready fence. Основная принимает только завершённый
  результат, копирует его в локальный буфер и после submission сигналит shared consumed fence.
  Повторная запись слота разрешена после consumed fence; вторичная очередь также ставит GPU wait.
- Пока кольцо заполнено, solver продолжает считать без публикации. Освобождение слота позволяет
  опубликовать следующее состояние с фактическим накопленным числом шагов.
- Основная карта выполняет небольшой `AdvanceDisplay`: previous position берётся из предыдущего
  показанного состояния. Без новой публикации previous=current. Начальные позиции доступны до первого
  вторичного результата. Для локальной модели сохранена предыдущая матрица контейнера при вычислении
  движения поверхности. Поверхность, освещение и композиция остаются на основной карте.
- CPU-ожидание вторичной очереди находится только в worker, для повторного использования его constants
  и ограничения числа незавершённых command lists. Основной поток в steady state его не ожидает.
  Reset/recreate/shutdown останавливают поток и дожидаются использования освобождаемых ресурсов.
- При остановке в лог выводятся реально выполненные шаги, число публикаций, последняя принятая
  публикация и payload bytes/result. Это счётчики всей жизни экземпляра, включая прогрев.

Схема shared heaps и fence-зависимостей сверена с
[Microsoft D3D12 Shared heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps).
Checkout `E:\Study\DX12` прочитан как пример организации обмена и не изменён.

## Выбор пути

Индексы соответствуют `GDX12DeviceFactory::GetDeviceDescriptors`, отфильтрованному списку аппаратных GPU.
Доступный список печатает `WaterOffloadGpuTests.exe --list`.

Обычный путь по умолчанию:

```ini
[ConsoleVariables]
Render.PrimaryAdapter = 0
Render.Fluid.SecondaryAdapter = -1
```

Перенос воды (пример индексов, перед запуском сверить список):

```ini
[ConsoleVariables]
Render.PrimaryAdapter = 0
Render.Fluid.SecondaryAdapter = 1
```

Параметры startup-only. Для App можно использовать существующий `-Quality.Overlay=<path.ini>`.
Одинаковый адаптер в обеих ролях и отсутствующий индекс отклоняются. Третий GPU не создаётся.
Нет Performance/Quality политики переноса, целевого FPS, freshness/debt проверок, cadence/autosearch.
Существующие настройки качества поверхности и самого solver сохранены.

## Выполненные проверки

- Debug x64 MSBuild 18: `App.vcxproj`, `WaterOffloadGpuTests.vcxproj`, `FluidSortGpuTests.vcxproj`,
  `BistroRenderGraphTests.vcxproj` собраны. Предупреждения линковщика об отсутствующих PDB сторонних
  DirectXTK/DirectXTex библиотек и `/EDITANDCONTINUE` не являются ошибками сборки.
- `FluidSortGpuTests.exe --validate`: успешный GPU-прогон на RTX 4080 SUPER с debug layer/GBV.
  Sort oracle от 1 до 1 048 576 элементов; production solver точно совпадает с полным sort для
  3/2049/51975 частиц, обоих моделей, нескольких batches и переключения DIRECT/COMPUTE/DIRECT.
- `BistroRenderGraphTests.exe`: успешный однокарточный GPU-прогон графа, pooled/dedicated HDR identity,
  async compute, обе точки AO, нечётный resize и native/XeSS switch.
- `WaterOffloadGpuTests.exe --local --primary 0`: успешный GPU-прогон production `Pack` и
  `AdvanceDisplay` с GBV на 257 частицах. Packed позиции совпадают побитно, новые позиции сохраняют
  предыдущие показанные позиции, повторный показ даёт previous=current. Межкарточного обмена здесь нет.
- `SecondGroupTests.exe`: успешный GPU-прогон SPH/NRF/resolve, quality, simulation-only, clear/reset,
  водной композиции и интеграции шести компонентов без ошибок debug layer.
- `git diff --check` проходит для правок задачи.
- Независимое ревью по correctness, reliability, performance, testing, maintainability и правилам проекта
  завершено. Исправлено P2: ожидание worker в тесте теперь ограничено deadline и фактической публикацией,
  вместо фиксированных 100 мс. Готовность к завершению зависит от двухкарточной проверки ниже.

Логи находятся в игнорируемой `Build/`: `water-app-build.log`, `water-offload-build.log`,
`water-sort-build.log`, `water-sort-validation.log`, `water-graph-build.log`, `water-graph-validation.log`.
Локальные kernels: `water-transfer-local.log`.
Интеграция воды: `water-second-group-build.log`, `water-second-group-validation.log`.

## Воспроизводимая двухкарточная проверка

```text
WaterOffloadGpuTests.exe --list
WaterOffloadGpuTests.exe --local --primary <N>
WaterOffloadGpuTests.exe --validate --primary <N> --secondary <M>
```

Проверка включает оба численных режима, 257/1025 частиц, разные transform/acceleration/waves/`dt`,
несколько публикаций и recreate. Численное сравнение после одинаковых реально выполненных шагов:
по каждой компоненте позиции/скорости `abs error <= 1e-4 + 1e-4 * max(abs(a),abs(b))`, только для
короткого воспроизводимого теста. Такой допуск допускает различие FP contraction/округления разных
GPU; он не является доказательством долгосрочной эквивалентности турбулентной симуляции.
Допуск не применяется к транспорту: отправленные и полученные позиции сравниваются побитно.

Отдельно заполняются все три слота без отображения, выполняется ещё один расчёт при заполненном кольце,
сравнивается полный solver state, затем принимаются все результаты FIFO с проверкой bytes и displayed
history. Циклы повторяются. Есть повторный показ без новых результатов, начальное состояние,
уничтожение с непотреблённой публикацией и Start/Stop без вызовов Present.

Эти двухкарточные тесты написаны и собраны, **но пока не выполнены на паре GPU**.

## Не подтверждено

- Численное соответствие между RTX 5050 и Arc B570 и точный межкарточный транспорт на этих драйверах.
- Реальное прохождение shared heap/fence пути, несколько оборотов кольца и lifecycle на двух GPU.
- Изображение новой воды, освещение и motion vectors при повторном показе/движении контейнера.
- GPU timeline одновременной работы secondary solver и остальных primary passes. Добавлены PIX
  markers `Water secondary solver + position export` / `Water ready import + displayed history`.
- FPS и полное GPU-время основной карты до/после при одинаковой сцене/разрешении/числе частиц.
  **Измеренного прироста FPS нет.** Нельзя считать успешную сборку или локальные регрессии подтверждением переноса.
  Текущий общий GPU timer начинается в frame command list, а импорт подаётся отдельным submission
  в PrepareFrame; для полного времени основной карты при сравнении нужно включить импорт по timeline.

Push не выполняется. Посторонние изменения `BistroAdditionsTests.cpp`, terra-validation report,
`imgui.ini` и `tools/tests/__pycache__` сохранены.
