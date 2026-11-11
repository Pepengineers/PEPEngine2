# Runtime bottlenecks: проверка 12 ноября 2026

## Результат

**Устойчивый кадровый bottleneck пока не установлен.** Свежая штатная Debug-сборка
успешна, но первый холодный запуск Bistro не дошёл до кадрового benchmark за
420 секунд. CPU/GPU frame samples, FPS, GPU pass timings и сравнение вариантов
не получены. Нельзя утверждать, что текущий кадр ограничен водой, освещением,
физикой, submit, Present, upscaler, occupancy или bandwidth.

Наблюдаемый blocker этой попытки — CPU-активная подготовка сцены до первого кадра.
Последние сообщения — упрощение collision meshes. Статический путь согласуется
с последующим холодным convex decomposition, но стеков исполнения нет: это
гипотеза локализации, а не доказанная стоимость конкретной функции.

## Сборка и изоляция

- CPU: AMD Ryzen 9 5950X, имя прочитано из реестра.
- GPU: NVIDIA GeForce RTX 4080 SUPER, `nvidia-smi` сообщает 16376 MiB,
  драйвер 610.88. DXGI log приложения сообщает dedicated memory 16047 MiB;
  это разные источники показателя.
- MSBuild VS 2026: 18.12.2.51402; bundled CMake: 4.3.1-msvc1.
  Прежние ошибки `System.Memory` и отсутствия генератора VS18 в этой проверке
  не воспроизводятся. Изменения сборочных скриптов не потребовались.
- Первая попытка сборки внутри sandbox остановилась на доступе к
  `C:\Users\raein\AppData\Local\Microsoft SDKs`. Та же сборка с разрешённым
  доступом к SDK прошла за 82.14 s; инкрементальная — за 3.22 s.
- Команда: `MSBuild.exe Apps/App/App.vcxproj /t:Build /p:Configuration=Debug
  /p:Platform=x64 /m:4 /nr:false /v:minimal`, executable MSBuild из
  `C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/amd64/`.
- Параллельные изменения исходников были обнаружены хешированием. Для запуска
  создана обычная копия 574 собственных source/config/tool files в
  `Build/RuntimeBottlenecks/frozen`, без Git worktree и переключения ветки.
  Изменений во время копирования не было. `External`, `Assets` и
  `Build/Dependencies` подключены directory junction к общим данным.
  Исходники внешних библиотек и импортированные ассеты не редактировались.
- Полная сборка этой копии прошла за 57.33 s. У неё собственные obj/bin,
  а compile-time shader paths указывают на замороженные шейдеры.
  Debug CRT/assertions сохранены; штатный оптимизированный Debug Jolt собран
  обычным скриптом. Новая инструментализация в этот exe не включена.
- Snapshot включает промежуточное состояние параллельных работ. Он **не является
  проверкой точного финального quality commit `9cdd897d`** или последующей
  гибридной сортировки воды. Точный состав — `frozen-source-manifest.json`.

## Протокол и полученные данные

Все артефакты этой работы находятся в `Build/RuntimeBottlenecks/`.

| Поле | Значение |
|---|---|
| Executable | `frozen/Build/bin/x64/Debug/App/App.exe` |
| EXE SHA256 | `a6f046fd531da32712803787768316faaaab9b33f6a396ce7d031bef7a7fd39d` |
| Runtime INI SHA256 | `33c42e1c84eef0dce66829ecadcff65bf749380b1ec44e6d6ce670ba55f130cc` |
| Config | копия `Config/Engine.ini`, добавлены только output dimensions 1280×800 |
| Камера | штатная stationary: position (-300, 200, 20), rotation (0, 117, 0) |
| Seed | штатный loading benchmark: 12345 |
| Time behavior | реальное wall time, fixed physics step 1/60; debt сохраняется |
| Smoke | 10 warmup frames, 10 measured frames (`BenchmarkFrames=11`) |
| План baseline | 120 warmup + 1200 measured frames, три отдельных повторения |
| Исполнено | один cold-start smoke; запланированный baseline не начался |
| Hash checks | 110 exe/DLL/config/shader inputs, до/после совпадают |
| Изменение общего INI | нет |

Количество объектов и effective reconstruction не успели быть экспортированы.
Count/quality controls сохранены как в frozen source и INI, без уменьшения нагрузки.
Статический default upscaler — XeSS, frame generation — Off, swapchain VSync — Off;
effective runtime state не подтверждён завершённым кадром. Поэтому значения нельзя
подменять утверждением о подтверждённом Native/FG-Off benchmark.

Runner использован из `tools/agent-skills/peengine-perf/scripts/run_job.py` через
artifact wrapper `run_baseline.py`. Wrapper удаляет унаследованные переменные
`PERF_*` и `DEBT_*`, пишет точную команду и параллельно опрашивает `nvidia-smi` раз
в секунду. Полный запуск — `smoke-launch.json`, состояние — `smoke.json`.

Журнал начинается 22:29:56.911 UTC 11 ноября (01:29:56.911 12 ноября в Istanbul).
Последняя запись — collision simplification в 22:33:42.076 UTC, примерно +225 s.
На промежуточной проверке окно показывало `Preparing textures 214/308`.
В самом журнале CPU mesh import занимает 137.255 s, CPU texture decoding —
72.9639 s; первые примерно 210 s нельзя целиком приписывать текстурам.
После превышения лимита — `Uploading materials 67/67`, суммарное CPU time процесса
5895.0625 s, затем 6634.71875 s. Это накопленное время всех потоков, **не время кадра**.

Набор из 438 system-wide GPU sensor samples за 01:29:56–01:37:17:

| Показатель | Minimum | Median | Maximum |
|---|---:|---:|---:|
| GPU utilization, % | 1 | 8 | 100 |
| GPU memory used, MiB | 4048 | 5244.5 | 7090 |
| GPU temperature, °C | 32 | 33 | 36 |
| Board power, W | 27.23 | 27.72 | 68.90 |
| Graphics clock, MHz | 210 | 210 | 2835 |

Это данные подготовки сцены и всего адаптера, включая другие приложения.
Они показывают преимущественно низкую GPU-активность в данной попытке; не доказывают
отсутствие VRAM pressure, не измеряют residency процесса и не описывают игровой кадр.
Низкие частоты здесь не являются доказательством thermal throttling.

На 420 s runner попытался завершить owned process tree. Ожидание child после
`taskkill` превысило ещё 10 s; итоговое состояние `failed`, elapsed 440.73 s.
Вывод `taskkill` штатный runner подавляет, поэтому конкретный отказ завершения
не установлен. Процесс некоторое время оставался жив, затем исчез; код его
завершения не получен. `smoke-result.json` отсутствует. Принимается только
**censored/failed evidence**, никакие frame numbers из этого запуска не используются.
Перед передачей GPU/BUILD WINDOW чату воды отсутствие App/MSBuild/cl/link проверено.

## Локализация загрузки и следующий профиль

`WorldLoader.cpp` вызывает `BuildObjMaterialSlots`, затем `SpawnObjScenePart`.
В Bistro `SceneMeshFactory` переходит к collision jobs в `BistroProps.h` и ждёт
их завершения. Между завершением загрузки материалов и этими jobs нет нового
loading progress message: заголовок `Uploading materials 67/67` не доказывает
ожидание upload fence.

В `Apps/App.Base/src/Physics/CollisionGeometry.cpp` сообщение об упрощении
предшествует синхронному VHACD convex decomposition: resolution 100000–1000000,
recursion depth 8, максимум 32 vertices/hull; Bistro задаёт 4–12 hulls и ошибку 1 см.
В логе 31 сообщение simplification, но нет завершённого decomposition или cache hit.
Для этой TU tlog подтверждает `/O2 /Ob2 /MDd`: объяснять стоимость просто
неоптимизированным Debug VHACD нельзя. Без sampled CPU stacks нельзя
исключить другие стадии после последней записи. Попытка использовать CDB по
Windows Kits path не состоялась: executable отсутствует. Attach/capture
не выполнены.

Collision cache зависит от compile-time `SOLUTION_DIR`, поэтому у snapshot свой
пустой `frozen/Build/AssetCache/Collision`. Запись выполняется после завершения
bake; остановка до него не создаёт warm cache. Ключ включает Origin, геометрию,
ошибку, hull budget и версию алгоритма. Это нужно учитывать при сравнении
cold/warm loading, не менять ключ или качество ради ускорения этой диагностики.

Следующая проверка должна начинаться с загрузки:

1. На том же frozen exe/config запустить bounded cold preparation с достаточным
   startup budget отдельно от frame benchmark. Запускать runner с правами,
   позволяющими завершить его child tree; сохранить результат завершения.
2. Пока стадия загрузки активна, снять несколько CPU stack samples с символами,
   записать main-thread wait и работающие jobs. Установить конкретный hotspot
   до изменения collision алгоритма, качества или числа объектов.
3. Дождаться корректного окончания bake и проверить cache writes; сравнить
   следующий warm load на том же входе. Не отключать физику ради получения FPS.
4. Выполнить три ordinary повторения 120+1200 кадров с неизменными hashes.
   Записать probe readiness, actual dimensions, effective upscaler/FG,
   counts, physics requested time/ticks/debt, loading и frame distributions.
5. Отдельно выполнить диагностический CPU/GPU run и trace. PIX GPU capture
   доступен; PIX timing capture требует admin. Nsight Graphics 2025.5 найден,
   но capture в этой итерации не запускался. Сначала разделить GPU work,
   starvation, queue waits и CPU recording; occupancy/bandwidth требуют
   hardware counters. Water sort/densities/integrate выделить отдельно.

## Подготовленная телеметрия и ограничения

`profile/` содержит artifact-only copied-source harness: main-thread CPU scopes
для update/render/physics/backend/dynamic sync/jobs completion, command acquire,
graph recording, submit, Present и fence waits. Метрики thread-local и включаются
только на main thread; worker CPU time не измеряется. GPU aggregate берётся из
завершённых timestamp totals с drains на границах и проверкой sample count.

Вложенные scopes нельзя суммировать. `Game update` включает `BeginFrame` и его
fence wait. Обычный `frame_ms` включает финальный graphics flush, отдельные
`frame_samples_ms` — нет. `GetGPUFrameTimeMS()` может повторять прошлое значение;
для среднего нужны totals/count. Primary GPU interval заканчивается до
frame-generation/Present и может содержать queue dependencies/starvation.

Harness не заменяет обычный baseline: его дополнительные drains и CPU clock
reads меняют pacing. На момент этой записи диагностический executable не собран
и не запущен. Preparation self-check и генерация всех семи compile response files
и link response по завершённым frozen tlogs прошли. Проверены единственный owner
и App.obj в многострочном link tlog, единственный `/OUT`, SHA256 копируемых
исходников и изолированные пути diagnostic objects/PDB.

Изменений production renderer, воды, lighting/effects, quality search,
third-party и общих config в этой работе нет. Новые коммиты переданы отдельному
ревьюеру согласно последнему поручению пользователя. Отсутствие численного
профиля не является доказательством выигрыша или регрессии соседних изменений.
