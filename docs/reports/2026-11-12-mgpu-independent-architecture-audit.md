# Независимый аудит MGPU: граф, транспорт и симуляция воды

Дата: 12 ноября 2026. Исследованный PEPEngine2: `codex/bistro-rendering`, `2af62375c7311e2a27a0fa00da02278c4825ec66`. Сравнение: `E:/Study/DX12`, HEAD `d17e45bcfef0a16af0f7ee74e936c162845bd221`, с существующей локальной правкой `MGPU-Reflection/Scene.cpp`. Оба дерева исследованы в их фактическом состоянии; исходники не изменялись.

## Вывод

Подозрение на фундаментальную проблему организации графа и межадаптерного обмена обосновано. PEPEngine2 умеет исполнять команды на второй GPU и передавать ресурсы, но композиция этих возможностей ограничивает параллельную полезную работу. Общий граф разделён на legacy-сегменты и отдельные placed-вызовы. Границы собирают незавершённые primary COMPUTE-работы; новые маршруты частиц и отражений дополнительно блокируют CPU до завершения небольших графов. Асинхронная вода использует другой путь, однако её следующий расчёт и продвижение Latest-передачи зависят от обслуживания CPU-кадра.

Есть также самостоятельные дефекты: FPS в заголовке переживает сброс часов; вода обрабатывает максимум два логических шага за вызов и при низком FPS неизбежно отстаёт; отложенный Exact может оставить кадр без воды. Эти проблемы нельзя исправить новым перебором размещений.

Наблюдение пользователя — вода в DX12 устойчиво симулируется и загружает обе видеокарты — принято как runtime-свидетельство. В этом аудите DX12 не запускался. Сравнение ниже объясняет различия в подаче работы и обмене; оно не опровергает наблюдаемую работоспособность DX12. Отдельные ограничения контрактов примеров не являются объяснением дефектов PEPEngine2.

## Границы доказательств

- **Код:** воспроизводимая логическая цепочка на указанной ревизии, без нового исполнения.
- **Артефакт:** значение непосредственно из уже сохранённого JSON/журнала. Диагностический запуск с монитором не считается обычным сравнительным бенчмарком.
- **Гипотеза о симптоме:** механизм в коде существует, но его срабатывание в момент пользовательского наблюдения ещё не привязано к конкретному кадру.

Прочитаны `AGENTS.md`, `CONTEXT.md`, утверждённый план `docs/plans/2026-11-12-mgpu-quality-search-design.md`, ADR-0013, исходный `docs/reports/2026-11-11-misspec-bottleneck-review.md`, отчёты реализации и незавершённой проверки. Исторический запрет исполнения из плана не трактуется как актуальный; действует новый прямой запрет пользователя на сборки, проверки с запуском приложения, GPU-нагрузки, UI-автоматизацию и production-правки. Здесь выполнены чтение исходников и разбор существующих артефактов.

## Что происходило в последнем запуске

Источник: `Build/mgpu-quality/quality-foreground-diagnostic-01/trial-{1,2,3}.json`. Пара RTX 5050, LUID 47217 / Arc B570, LUID 59621; Native 1280×720, FG Off, Release без debug layer. Запуск имел дополнительный монитор окна. Все три JSON содержат `visual_quality_verified=false`.

| Trial | Размещение воды | FPS из полного 100-секундного участка | Максимальный долг воды, с | Кадры с нарушением свежести воды |
|---|---|---:|---:|---:|
| 1 | Primary | 13,8077 | 59,6553 | 1381 / 1381 |
| 2 | Secondary Exact | 14,7293 | 62,4921 | 1474 / 1474 |
| 3 | Secondary Latest | 14,5686 | 56,9346 | 1457 / 1457 |

Во втором и третьем trial на Arc переносилась **только вода**. Grass, particles, sun/local shadows и reflection faces оставались primary. Поэтому большой reflection shadow atlas не является причиной поведения этих двух trial. `synchronous_transfer_calls=0` также не означает отсутствие обмена: этот счётчик охватывает `CrossAdapterWork::Transfer`, а вода идёт через native async transport. Место учёта: `Apps/App.Base/src/Rendering/CrossAdapterWork.cpp:237`; отдельные water export/import counters: `Apps/App/src/Rendering/Water/CrossAdapterWater.cpp:516-517,573,602`.

В каждом trial недопустима и свежесть травы; максимальный долг 9,3113 / 2,9607 / 3,8578 с. Это не равно CPU physics debt: физика в первом и третьем trial имеет `valid=true`, во втором есть шесть превышений бюджета. Нельзя называть все эти результаты одной «проблемой физики CPU».

`quality-foreground-diagnostic-01.run.json` и `.user-cancelled.json` фиксируют остановку пользователем, 401,71 с, отсутствие принятого победителя и неизменные before/after fingerprints. Скриншот пользователя показывает CPU 9%, RTX 89%, Arc 4%; это один момент без привязки к номеру trial/кадра. Он согласуется с недостаточной подачей полезной работы на secondary, но не измеряет длительности конкретных очередей.

## 1. Граф создаёт более широкие зависимости, чем требует потребитель

**Подтверждено кодом.** Здесь нет единого исполняемого DAG всего кадра на двух адаптерах. `PlanExecutor::TrySubmit` принимает только entirely explicitly placed plan, отвергая legacy primary batches (`Engine/Engine.RendererDX12/src/GDX12RenderGraph.cpp:985-987`). Основной legacy-граф вызывает замену на границе и возвращается к записи оставшейся части кадра (`:1498-1533`). Вода, тени, частицы и отражения подключены такими границами (`Apps/App/src/Rendering/BistroRenderPipeline.cpp:741-756`).

На конце **каждого** `RecordLegacyRange` выполняется `join(pending.size(), ... "legacy split boundary")` (`GDX12RenderGraph.cpp:1746`). Сам join отправляет DIRECT-prefix и вставляет GPU wait на конец всех накопленных primary COMPUTE-вызовов (`:1590-1599`). Это не CPU wait и не доказательство полной остановки второй GPU. Но любая новая граница обрезает существующее окно async overlap, даже если её потребителю не нужны все эти COMPUTE-результаты. Следующий async fork также зависит от целого отправленного DIRECT-prefix (`:1722-1723`).

В placed-executor есть дополнительное упорядочивание по backing allocation (`:1119-1155`). Все обращения к одному backing ставятся в цепочку; перед новым обращением добавляются waits на незавершённые обращения других очередей, **без условия, что хотя бы одно из них пишет**. Это намеренно защищает whole-resource transitions через COMMON. Его цена — сериализация read/read и независимых диапазонов или subresources одного ресурса. Например, `CrossAdapterWork::Copy` умеет выбирать одну грань texture (`CrossAdapterWork.cpp:354-371`), но graph use регистрируется для всего resource (`:336-339`).

Следствие: схема размещения может содержать независимые faces/lights, а физическая единица зависимости остаётся крупнее. Сам факт наличия per-face политики не доказывает per-face overlap. Цена этих лишних рёбер в миллисекундах в текущих артефактах не выделена.

**Что не обнаружено:** executor не вставляет безусловный глобальный CPU wait после каждого placed-submit. Он отправляет queue waits и command lists (`GDX12RenderGraph.cpp:1271-1284`); `GDX12CommandQueue::WaitFor` вызывает `_commandQueue->Wait` (`src/GDX12CommandQueue.cpp:186-193`). Общая блокировка возникает в вызывающем коде и на избыточных зависимостях, а не потому, что любой D3D12 Wait здесь реализован через CPU.

Canonical identity проверяет adapter/resource/epoch/allocation и shared aliasing. `RenderMGPURuntime.cpp:49-73` переиспользует идентичность владельца, `GDX12RenderGraph.cpp:1461-1479` отвергает противоречивые legacy owners. Удалять эту проверку для ускорения нельзя: она не заменяет планировщик и защищает реальную память. Доказательства актуального ошибочного смешивания identity воды с ресурсами другой техники не найдено.

## 2. Latest-транспорт зависит от CPU между стадиями

**Подтверждено кодом.** `Protocol::BeginImport` требует для Latest не просто известный export fence, а `ExportDone`, ранее замеченный CPU (`Engine/Engine.RendererDX12/src/GDX12SnapshotProtocol.cpp:192-207`). Для Exact это условие не действует. `Transport::Poll` наблюдает producer/export/import completion (`src/GDX12SnapshotTransport.cpp:700-711`). Latest выбирается только после `ImportDone` (`GDX12SnapshotProtocol.cpp:257-270`).

Вода вызывает `AdvanceOutputs` из `Tick` (`CrossAdapterWater.cpp:721,730`). Там export может быть отправлен сразу с GPU dependency, а import откладывается до следующего обслуживания, если CPU ещё не наблюдал завершение export (`:550-615`). Регулярный runtime poll находится в `RenderModule::OnUpdate` (`Apps/App.Base/src/RenderModule.cpp:1071-1078`). Poll из других путей не продолжает автоматически water `AdvanceOutputs`.

Таким образом, быстрая GPU-копия может ждать следующего CPU-кадра перед запуском следующей копии; затем готовый импорт ждёт выбора снимка. Это дополнительная задержка управления, а не PCIe bandwidth. При кадре 70 мс даже одно такое ожидание уже превосходит весь Quality budget 66,67 мс. Точное число потерянных интервалов зависит от момента завершения и poll; нельзя объявлять фиксированную задержку ровно в два кадра.

Три transport slots (`GDX12SnapshotTransport.h:10`) обеспечивают хранение и lifetime. Они не устраняют host-gated запуск стадий и не означают три одновременно обслуживаемых расчёта симуляции.

## 3. Новые particles/reflections исполняют синхронные цепочки

**Подтверждено кодом.** `CrossAdapterWork::Run` всегда выбирает DIRECT, затем вызывает `Wait(done)`; `Wait` использует `CPUWaitForFenceValue` (`Apps/App.Base/src/Rendering/CrossAdapterWork.cpp:87-94,125-132`). `Transfer` отправляет четыре отдельных batch: DIRECT pack → producer COPY export → consumer COPY import → consumer DIRECT unpack, затем CPU ждёт последнего (`:212-236`). `Copy` дополнительно пакует исходные buffers/textures и распаковывает результат (`:375-401`). Это не прямое чтение GPU-результата в CPU RAM, но несколько GPU-копирований с синхронным завершением на CPU.

У reflections последовательность особенно явна:

```text
CPU ждёт primary prefix
  → зеркалит scene buffers, изменяемые textures, environment и shadow atlas
  → запускает secondary capture/filter и ждёт его
  → возвращает raw faces / опубликованную cubemap и ждёт копий
  → выполняет primary publication и ждёт
  → возвращается к остальному кадру
```

Источник: `CrossAdapterReflections.cpp:309-395`. Отдельные queues здесь не дают независимой работе следующего сегмента опередить эту цепочку. Native water/shadow paths не следует автоматически относить к этому синхронному пути.

Частицы передают depth/camera/interaction, выполняют simulation, затем переносят полный checkpoint pools (`CrossAdapterParticles.cpp:291-325,336-406`). Даже primary-кандидат с `Scheduled=true` идёт через синхронные `Run` для input capture, simulation и истории отображения (`:310,358,476,518`). Preset включает scheduled particles (`Apps/App/src/BistroQualityPresets.h:269`). Поэтому «все работы primary» внутри нового поиска не обязательно равнозначно прежнему обычному single-GPU pipeline. Количество synchronous transfers не отражает эти локальные CPU waits.

**Необходимое изменение:** возвращать completion/ownership без немедленного ожидания, отправлять независимый primary prefix пока secondary работает, соединять только перед реальным чтением. Простое удаление `Wait` небезопасно: нужны живущие до completion constants, input snapshots, mutable scene state и slots; нынешняя синхронность скрывает эти требования к lifetime.

## 4. Вода получает недостаточно шагов даже при свободной GPU

**Подтверждено кодом и сохранённым долгом.** `GDX12FluidClock::TicksPerInvocation=2`; `Prepare` больше не выдаёт (`Apps/App/src/Rendering/Water/GDX12FluidClock.h:15-16,86-111`). Логический шаг не больше `WorldTimeScale*TimeScale/60` (`GDX12Fluid.cpp:190-194`). Secondary запрещает следующую подготовку при действующем `SolverDone`, а его завершение обрабатывает `Collect` из очередного `Tick` (`CrossAdapterWater.cpp:205-213,373-383,687`). Новые inputs и submit поступают из `BistroRenderPipeline::PrepareFrame`, один раз на кадр (`:575-583`).

При обычном масштабе времени верхняя граница продвижения — `2/60` секунды симуляции на invocation. При 14,73 кадра/с и не более одного invocation на кадр это максимум около 0,491 секунды симуляции за секунду реального времени. Даже бесконечно быстрая Arc не может компенсировать остальное, потому что ей не отправляют дополнительные шаги. Безопасный solver step может сделать предел ещё ниже. На 30 FPS нет резерва для догоняния потерянных шагов.

Primary scheduled-вода содержит тот же предел `recorded < 2` (`GDX12FluidPass.cpp:430-474`). Поэтому долг trial 1 — ожидаемое следствие этой политики, а не свидетельство плохого PCIe. Secondary Exact/Latest не исправляют её.

Три transport slots не меняют наличие одного `SolverSlot`, одного `SolverDone` и одного publication Source (`CrossAdapterWater.cpp:160-168,325-347`). Source нельзя переиспользовать до завершения его export (`:395-399`). Latest при нехватке publication credit способен продолжить solver без публикации (`:403-407`), но всё ещё ограничен обслуживанием кадров. Нет независимого цикла, который продолжает кормить secondary, пока primary занят рендером.

Размер water payload — `Seed.size()*sizeof(Particle)`; `Particle` содержит три `XMFLOAT3` (`GDX12FluidSolver.h:49`). Для 49 776 частиц, указанных в trial, это 1 791 936 байт на снимок, если весь этот набор принадлежит routed owner. При одной публикации на каждый из 14,73 кадров/с — около 26,4 МБ/с полезных снимков в одном направлении; это оценка по размеру и максимальной частоте, не измеренный PCIe throughput. Здесь надо искать задержки запуска/передачи и недостаток solver work, а не автоматически переносить диагноз 198-МБ reflection atlas на воду.

Долг сохраняется честно; стирать его, увеличивать freshness budget или просто присваивать `Completed=Desired` нельзя. Нужен планировщик необходимого объёма работы с ограничением очереди и проверкой устойчивой пропускной способности. Solver должен сохранить исходные dt, входы и физические параметры; увеличение одного dt вместо недостающих шагов не является эквивалентной правкой.

Для ветра травы также есть сохранение debt при ограниченном временном бюджете (`Apps/App/src/Rendering/Grass/GDX12GrassWind.cpp:554-561,596-603`), включая ограничение времени аналитической ветки (`:562-578`). Его отдельная оптимизация не доказана; артефакты уже показывают, что local-вариант не выдерживает текущий freshness gate.

## 5. Exact может убрать воду из кадра; измеритель этого не замечает

**Доказанная ветка кода; связь с конкретным наблюдаемым миганием требует покадрового подтверждения.**

1. `Tick` сбрасывает `Selected` (`CrossAdapterWater.cpp:677`). При `DemandExact` обычный `SelectFrame` возвращается, не выбрав старый снимок (`:736`).
2. `DemandExact` может быть установлен при незавершённом прошлом solve и наличии debt (`:724-726`). Позже `PrepareSolve` возвращает false, пока `SolverDone` действителен (`:375`), даже если GPU успела завершиться после последнего `Collect`.
3. `SubmitExact` в этом случае только увеличивает `ExactDeferrals`, пишет `exact-deferred` и возвращает пустой receipt (`:750-763`). Старый display не восстанавливается. `BistroWaterRendering::SubmitExact` обновляет display только при наличии receipt (`:86-90`).
4. Routed owner по-прежнему исключён из локальной симуляции (`GDX12FluidPass.cpp:422`), а `drawing` для routed owner без display равен false (`:518-523`); surface draw не выполняется (`:554-567`). История отображения также теряет последовательность (`CrossAdapterWater.cpp:769-772`).

Следовательно, «не готов новый Exact» может означать не ожидание согласованного нового результата и не сохранение старой воды, а отсутствие воды в кадре. Это дефект отображения независимо от возможной дополнительной ошибки транспорта. При строгом Exact требуется надлежащая GPU-зависимость на нужную версию; при разрешённом сохранении старого display это должно быть явным режимом с честным возрастом, не скрытой подменой Exact.

Есть пробел измерения: `DisplayedSimulationTime` после первого успешного выбора остаётся в counters; в начале кадра он не сбрасывается. `GetDiagnostics` продолжает вычислять положительный `SourceAgeSeconds` по этому прежнему времени (`CrossAdapterWater.cpp:812-814`), а quality `Ready` проверяет только `SourceAgeSeconds>=0` (`BistroRenderPipeline.cpp:241-245`). Поэтому `missing_frames=0` в trial JSON не доказывает, что вода рисовалась в каждом кадре. Нужен отдельный признак выбранного/показанного состояния текущего frame, с его version и причиной пропуска.

## 6. Отображение воды не интерполирует отставшую симуляцию

Production water route передаёт particles GPU→GPU. CPU readback частиц не является обязательной стадией отображения этого пути. Metadata содержит immutable input/settings, version, simulation time и capture time (`CrossAdapterWater.cpp:92-98,508-509`); display удерживает leases до primary tail (`:625-668,766-783`). Это полезная основа согласованности.

Current/Previous используются для motion vectors: `FluidSurface.hlsl:110-115` вычитает предыдущую показанную позицию из текущей. Это не интерполяция положения частиц к текущему времени. Если новых снимков мало, вода удерживает прежнее состояние и затем скачком принимает новое. Wave phase тоже продвигается только solver dt (`GDX12Fluid.cpp:366-370`); большой debt означает замедленную волну.

Anchor, найденный в grass, — отдельный локальный checkpoint при передаче владения (`CrossAdapterGrass.cpp:327-330`). Выбор grass сохраняет `Shown` и принимает `Pending` со своими readers (`:502-570`). Это не water interpolation, и исправление grass anchor identity не устраняет исчезновение воды. После несовместимых inputs water clock может заполнить 16 spans и зафиксировать terminal missing-input state (`GDX12FluidClock.h:55-83`); это отдельная возможная причина остановки публикаций, не установленная по текущим JSON причина пользовательского мигания.

## 7. FPS в заголовке действительно может остаться от прошлого trial

**Подтверждено кодом; журнал согласуется.** `App::CalculateFrameStats` хранит `static FrameCount` и `static TimeElapsed`, обновляя заголовок только при `Timer.TotalTime()-TimeElapsed>=1` (`Apps/App.Base/src/App.cpp:42-72`). Поиск сбрасывает `Timer` при restart, calibration и готовности trial (`Apps/App/src/App.cpp:137,644,799`), но не сбрасывает эти статические значения. `GameTimer::Reset` обнуляет базу и delta (`Engine/Engine.Core/src/GameTimer.cpp:57-68`).

После trial примерно на 110 секунд новый trial начинает время с нуля и почти целиком не проходит условие обновления. Счётчик кадров при этом растёт, поэтому последующее показание также может быть неверным. Это не вывод среднего предыдущего trial по дизайну UI, а ошибка использования сбрасываемых часов для текущего FPS. Длинный блокирующий Render дополнительно задерживает обновление: CalculateFrameStats вызывается только после него (`App.Base/src/App.cpp:139-141`).

В `.foreground.jsonl:145` последнее показание — `13.181203 FPS`, monotonic `81605.4258453`; следующая запись `:146` через 244,524 с уже показывает отсутствие окна приложения. Монитор сохраняет только изменения состояния (`run-interactive.py:74-84`). Между этими событиями нет записанных изменений заголовка, хотя сохранены следующие полные trials. Это согласуется с дефектом; из журнала нельзя восстановить содержимое каждого показанного кадра.

Benchmark FPS в JSON вычисляется отдельным `BistroBenchmarkFlight` по `QualityTime()`; ошибка заголовка сама по себе не обесценивает арифметику `sample_count/seconds`. Исправление: независимые монотонные часы статистики и сброс её окна при смене режима, явные current FPS / trial result / phase.

## 8. Передача данных отражений остаётся слишком крупной

**Код + отдельно обозначенная история измерений.** Исправления reuse и skin ranges присутствуют: immutable geometry копируется при смене allocation/revision, иначе передаются активные skinned ranges (`CrossAdapterReflections.cpp:146-179`). Прежнее пересоздание банков и копирование 703 892 012 B каждый frame нельзя выдавать за текущий код.

Однако при каждом secondary capture всё ещё обновляются environment textures и весь `CaptureLocalShadows()` (`:189-208`), через отдельные synchronous `MirrorTexture` calls (`:76-79`). Даже размещение одной грани probe несёт эти общие входы. Последующее уменьшение result cubemap не устраняет цену входного пакета.

Сохранённый незавершённый validation report, строки 59-63, сообщает после исправления ranges: geometry 8 182 720 B; environment+shadow 215 989 264 B; atlas 256×256×755, 197 918 720 B и примерно 78,85 мс CPU wall. Это пересказ существующего диагностического результата, не новый замер и не изолированное GPU copy time. Локальный `reflection-profile-01.log` относится к предыдущей, уже исправленной стадии и не используется для текущих чисел.

Непосредственно доступная startup calibration последнего запуска даёт для 197 918 720 B primary→secondary median 71,2901 мс, обратное направление 61,2240 мс (`quality-foreground-diagnostic-01/transfer-calibration.json`). Это текущий synchronous pack/export/import/unpack path, не физический предел PCIe. Один такой синхронный входной пакет уже больше Quality budget; ускорение только capture/filter не решает критический путь. Число 755 — назначенные layers; доказательства, что весь этот объём можно без изменения качества отбросить как неиспользуемую capacity, нет.

Нужны revision-based dirty slices и сохранение shadow/environment replica либо вычисление необходимых зависимостей на secondary. Выбор между ними требует учёта стоимости и одинакового lighting результата. Копировать тот же atlas целиком реже — изменение временного качества, которое надо учитывать явно.

## Сравнение механизмов E:/Study/DX12 и PEPEngine2

Пути в первом столбце ниже относительны `E:/Study/DX12`. Изучены горячие пути, queue implementation и shared-resource allocation, а не только перечисление адаптеров.

| Механизм | DX12 | PEPEngine2 и следствие |
|---|---|---|
| Fluid submission | `MGPU-FluidSimulation/HybridParticleApp.cpp:297-322` отправляет secondary compute в начале Draw, накапливает dt между отправками, затем записывает primary normal/AO/shadows/forward (`:338-354`) | Latest отправляется в PrepareFrame, но следующий solve разрешается только после CPU Collect. Exact запускается у surface boundary. Нет независимого обслуживания накопившегося water debt |
| Fluid time | `Engine/Common/SharedFluidParticleEmitter.cpp:359-370` в подпроекте fluid делит принятый dt на `IterationsPerFrame`, defaults 3 / MaxTimeStepFps 60 (`.h:197-198`) | Два фиксированных logical ticks максимум за кадр. Разные правила продвижения времени; одинаковое название SPH и визуальная похожесть не обеспечивают одинаковый workload |
| Результат fluid | Копируются positions и velocities из persistent secondary resources в shared и затем в local primary; нет универсальной дополнительной pack/unpack-схемы (`HybridParticleApp.cpp:313-322`) | Water тоже имеет отдельный native путь, но с publication source, export/import, metadata и leases. Для новых техник поверх транспорта добавлен дополнительный pack/unpack |
| Buffering/pacing | Общая база имеет три FrameResources (`Utils/d3dUtil.h:278`); CPU waits преимущественно при повторном использовании frame slot (`MGPU-FluidSimulation/HybridParticleApp.cpp:48-75`) | Три transfer slots; один water solver invocation и source. В Work waits после каждой операции, независимо от свободных frame slots |
| Баланс нагрузки fluid demo | `MGPU-FluidSimulation/HybridParticleApp.cpp:1228-1328` подбирает количество частиц по secondary GPU timing, затем отдельно primary render quality (`:1333-1496`). Во время первой фазы метрика берётся из secondary compute timestamp (`:1516-1537`) | В последнем water trial число частиц фиксировано качеством, а producer rate ограничен CPU-кадрами. Высокая занятость обеих карт в demo дополнительно поддерживается балансировкой самостоятельных workloads; одна настройка качества здесь не устраняет недостаток подачи шагов |
| Shared allocation | `Graphics/GCrossAdapterResource.cpp:21-59`: persistent shared cross-adapter heap, две views | Transport также использует shared heap/views; аппаратная возможность обмена существует. Разница в организации стадий, versions, lifetime и числе копий |
| Reflection scene | Локальная scene на каждой GPU; secondary строит собственную shadow map и captures (`MGPU-Reflection/ReflectionRenderer.cpp:1335-1436`) | Capture получает зеркала текущей primary scene и shadow atlas; большая обратная зависимость перед secondary работой |
| Reflection scheduling | Primary и secondary Draw отправляются раздельно (`ReflectionRenderer.cpp:998-1077`); результаты faces импортируются в primary (`:1440-1463`) | CPU дожидается всей цепочки secondary до продолжения legacy кадра. Near/far и face placement пока не означают независимый pipeline |
| Grass | `MGPU-Grass/HybridGrassApp.cpp:814-857` отправляет новый batch после завершения своего предыдущего, primary продолжает рисовать local output (`:861-891`) | Native grass содержит anchor/current/previous, ordered inputs и leases; отражения могут дополнительно потребовать его checkpoint. Сам протокол корректности не создаёт постоянной подачи работы |
| SFR | Обе части изображения отправлены до их соединения (`MGPU-SFR/SFRApp.cpp:1430-1446`); есть queue shared fence и CPU wait secondary у композиции | В Bistro выбран offload техник. Нет причин переносить SFR как обязательную новую функцию; полезен принцип позднего join после двух независимых ветвей |
| Noise/clouds | `MGPU-Noise/HybridNoiseApp.cpp:324-369` содержит варианты с queue-side shared Signal и polling; Draw отдельно отправляет clouds и scene (`:649-664`) | Похожая техника не должна наследовать синхронность Work только ради общего API. Проверять конкретную зависимость inputs/results |
| UI / atmosphere | `MGPU-IMGUI/HybridUIApp.cpp:248-286`, `MGPU-Atmosphere/HybridAtmosphereApp.cpp:245-315`: самостоятельная secondary работа, shared output и независимая primary работа | Эти техники не перенесены в последних water trials; их существование не доказывает overlap воды |

В DX12 полезно заимствовать **раннюю отправку самостоятельной работы, persistent local state, ограниченный пакет результата и позднее соединение**, сохранив контракт свежести PEPEngine2. Реальный throughput обеих карт и визуальная корректность DX12 являются пользовательским наблюдением; новой измеренной equivalence двух сцен здесь нет.

Техническая оговорка к буквальному копированию: несколько примеров не показывают законченный протокол публикации shared data. Fluid Draw не вставляет межадаптерный wait между новым secondary export и primary copy; shadow/UI проверяют старые fences разных очередей отдельно. `MGPU-Particles/HybridParticleApp.cpp:291,328` вызывает `Signal` непосредственно на `ComPtr<ID3D12Fence>` (`.h:108-114`), тогда как queue-issued Signal реализован отдельно в `Graphics/GCommandQueue.cpp:123-125`. В reflection импорт faces не имеет epoch/coherent-cubemap протокола PEPEngine2. Это статические ограничения гарантий данных путей, не утверждение о воспроизведённом сбое DX12 и не оправдание проблем нашего графа.

## Что реализовано и что ещё не выполнено

Модель кандидатов действительно содержит пару, water/grass, sun/local policies, particles и независимые reflection axes (`BistroMGPUQuality.h:17-81`). Native water/grass/shadow transfers, canonical identities, snapshot lifetime, coherent reflection publication и фильтрация неприемлемой свежести — реальный код, не пустые переключатели. `FreshnessMeasurements::Valid` отклоняет fallback/missing/over-age и растущий тренд debt (`:114-151`); последний запуск не даёт оснований объявлять winner.

При этом не завершены сквозная визуальная приёмка, подтверждённое ускорение, стабильный winner и replay. `visual_quality_verified=false` выводится прямо кодом (`Apps/App/src/App.cpp:1039`); автоматическое принятие trial использует freshness, не анализ изображения (`:853-858`). Следовательно, runtime capability/fixture PASS не равен выполнению согласованного плана. Гранулярное размещение реализовано шире, чем гранулярное асинхронное исполнение.

## Приоритет исправлений

1. **Сначала исполнение и корректность воды.** Отделить запуск необходимого количества solver steps от частоты отображения; отправлять работу secondary рано, обслуживать completion без ожидания следующего полного кадра. Сохранить ограниченную очередь, immutable inputs и проверку debt. Устранить пустой display на Exact deferral и добавить в измерение текущий displayed version/наличие draw. Параллельно исправить часы FPS — это небольшая независимая ошибка диагностики.
2. **Перестать разрывать GPU-цепочки через CPU.** В Latest резервировать безопасный local slot и ставить import на export fence без требования предварительного CPU-наблюдения ExportDone; выбирать для отображения только действительно готовую версию. Это требует изменения протокола reservation/cancellation/lifetime и соответствующих проверок, а не удаления одного `if`.
3. **Сузить границы графа.** Заменить join всех primary COMPUTE на join необходимых producers; перенести ранний secondary producer и поздний consumer в общий план зависимостей либо расширить существующий executor до такой композиции. Разрешать concurrent reads только при совместимых состояниях; для independent ranges/faces нужны реальные subresource/range contracts. Не ослаблять canonical identity и не убирать защитные barriers вслепую.
4. **Перевести particles/reflections с синхронного Work на отложенные completion.** Переиспользовать существующие queues/transport, добавить достаточное хранение inputs и результатов. Для particles сохранить depth/camera/interaction одного исходного состояния, для probes — coherent epoch и расписание. Сократить reflection входной пакет по фактическим revisions/slices либо производить нужные зависимости локально на secondary.
5. **После отдельного разрешения на запуски — короткая проверка причин, затем поиск.** Сначала одинаковый water-only workload с реальным dt и непрерывным отображением, затем combined workload. Проверять GPU timeline обеих карт, producer/export/import/display timestamps, очередь шагов/debt и кадры без воды. Зафиксировать одинаковые параметры primary/secondary и реальные progress clocks. Лишь после устранения причин возвращаться к 10+100-секундным trials, повтору финалиста и визуальному replay.

Пункт 5 — порядок будущей проверки, не запуск и не запрос возобновить остановленные задачи. Увеличение utilization не является критерием успеха: нужны одинаковое качество, допустимая свежесть, отсутствие растущего долга и выигрыш времени кадра.

## Неустановленное

- Не определено, какие именно кадры пользовательского мигания попали в Exact deferral, нехватку display credits, terminal input gap либо другую ошибку surface rendering. Текущая диагностическая готовность недостаточна, чтобы это исключить.
- Не измерены отдельно стоимость лишних graph edges, latency CPU polling и длительность простоя Arc в последнем water trial. Код подтверждает механизм; процент вклада требует timeline.
- Не доказана текущая порча GPU-данных из-за неверного fence/transition или canonical identity в PEPEngine2. Найденные ограничения подачи работы достаточны для накопления debt без порчи данных.
- Не установлена причина внешних смен foreground. Reset порядка 0,1 с из прежней диагностики не превращается здесь в «пятиминутное зависание»; отсутствие обновления title имеет отдельную подтверждённую причину.
- Старые ordinary smoke-числа из validation report (0,6126→2,5171 FPS secondary, 20,9910 FPS primary) относятся к другой стадии/профилю. Они не объединены с последними диагностическими water trials в единое A/B.

Проверка этого изменения: просмотр исходников и существующих JSON/логов, проверка ссылок и diff документа. Новые сборки, тесты, GPU-запуски, профилирование и UI-автоматизация не выполнялись. Посторонние изменения оставлены без изменений.
