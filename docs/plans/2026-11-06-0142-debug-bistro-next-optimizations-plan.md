# Дальнейшая оптимизация Debug Bistro
Created: 2026-11-06

Основа: HEAD `908457b`, отчёт `docs/reports/2026-11-06-debug-frame-hotspots.md`.
Статус итерации 2026-11-06: кэш static shadow visibility и однопроходный сбор bounds
реализованы и проверены. [Результаты измерений и ограничения](../reports/2026-11-06-debug-bistro-next-optimizations.md).
Стабильный физический долг с исходными коллайдерами, snapshot <=9 мс,
graph recording <=8 мс и общий frame time <=50 мс остаются открытыми целями.

## Цель и исходные данные

Увеличить реальную частоту кадров без сокращения сцены, физической работы,
качества теней, травы, отражений и постобработки. Сохранить Debug CRT и assertions.
Промежуточные ориентиры: 66,7 мс (15 FPS), затем 50 мс (20 FPS).
Это критерии следующих итераций, не обещанный результат или сумма прогнозов.

Текущая обычная Debug-сборка: медиана 104,657 мс, около 9,56 FPS,
три запуска по 120 измеренных кадров. Данные отдельных scopes получены
в диагностическом executable: это ориентиры для выбора работ, а не разложение
104,657 мс на точные независимые слагаемые.

| Участок | Измерено | Что уточнить / сократить |
|---|---:|---|
| Physics update | 21,85 мс | Число тиков, цена одного тика, долг симуляции |
| Shadow visibility wait | 19,91 мс | Полная работа jobs и повторная обработка статики |
| Scene snapshot | 16,99 мс | Агрегация ~9,28 мс, extraction ~5,09 мс, cascades ~2,42 мс |
| Graph recording | 13,89 мс | Локальные тени внутри него ~9,47 мс |
| BeginFrame | 5,78 мс | CPU-подготовка SDK либо корректное ожидание/pacing |
| Skeletal preparation | 4,68 мс | Bounds, palettes и signatures между обновлениями позы |
| GPU timestamp interval | 30,11 мс | Нужен новый capture после CPU-правок на сопоставимом состоянии |

Scopes вложены, jobs могут выполняться в других ожиданиях, CPU и GPU перекрываются.
Эти строки нельзя складывать или вычитать из frame time для прогноза FPS.

## 0. Сделать нагрузку и физический прогресс сопоставимыми

**Первое действие:** дополнить имеющиеся измерения, без нового общего profiler framework.

- Для каждого кадра записывать число fixed ticks, accumulator до/после,
  запрошенное и фактически пройденное время симуляции, срабатывание лимитов.
- Разделить один тик на FixedPrePhysics, BeforePhysicsStep / waves,
  Jolt Update и FixedPostPhysics; отдельно считать active/sleeping bodies.
- Измерить полную работу shadow visibility jobs и число range/face tests,
  candidate casters и invalidated faces. Wall time ожидания не равен всей работе.
- Сохранить обычный realtime benchmark с camera/INI/seed текущего отчёта.
  Добавить проверочный маршрут камеры: интерьер, внешний двор, активная волна.
- Для сопоставления содержимого использовать одинаковые точки simulated time
  и seed; deterministic replay дополняет realtime замер, не заменяет его
  дешёвым сценарием с одним physics tick на кадр.
- Отличить долг, накопленный при первом тяжёлом render/loading кадре,
  от постоянной нехватки производительности после прогрева. Не списывать
  accumulator и не менять dt ради улучшения результата benchmark.

**Почему это первым:** `App::UpdatePhysics` выполняет до 8 шагов по 1/60 с,
но также выходит после достижения wall budget 1/60 с. Поэтому измеренные
~22 мс нельзя считать стоимостью одного шага Jolt. Ускорение рендера может
увеличить количество выполняемых physics ticks и скрыть часть выигрыша в FPS.

**Готово, когда:** известна цена тика, видно изменение долга за 10 минут
realtime при TimeScale=1, получена разбивка по фазам и контрольный набор сцен.
Растущий долг фиксируется как проблема корректности/производительности,
даже если FPS вырос. Новый профиль сопоставлен с обычным executable.

Владельцы: `Apps/App.Base/src/App.cpp`, `Apps/App.Base/src/Physics/JoltPhysicsBackend.cpp`,
`Apps/App.Base/Include/App.Base/Systems/ShadowVisibilitySystem.h`, существующие
benchmark/scopes в `Build/DebugPerf-20261106/` и `Apps/App/src/App.cpp`.

## 1. Убрать повторные проверки статических casters для каждого света

**Приоритет: высокий; текущий видимый scope ~20 мс ожидания.**

- Сначала разделить static/dynamic contributions в расчёте visibility.
  Повторно использовать static signatures по свету/грани, пока их входы не изменились;
  динамические bounds проверять на каждом нужном кадре.
- Начать с существующих revision/handle механизмов. Отдельную геометрическую
  revision вводить только если счётчики покажут лишние invalidations от общего
  lighting/environment hash. Ключ света должен учитывать position/range,
  type, projection/cone и участие в shadow maps.
- Сохранить детерминированное объединение static/dynamic signatures и обработку
  выходов/удалений. Нельзя просто пропустить статику и оставить старый AllCasters.
- Если после кэширования всё ещё доминирует полный перебор light × caster,
  добавить узкую broad phase для render bounds. Выбрать grid или BVH по реальным
  размерам/распределению bounds; не строить заранее универсальную spatial system.
  Физическая broad phase не покрывает все render primitives и application geometry.

Рабочая цель scope: <=8 мс ожидания на стартовом сценарии, при снижении полной
работы jobs и общего frame time. Реальный выигрыш пока неизвестен; ~20 мс —
размер измеренного scope, а не обещанная экономия.

Проверки: старый полный перебор как oracle; moved/removed/static-to-dynamic caster,
moving light, range/cone edit, material alpha/classification, same-handle reimport,
выключение/включение shadows, cube seams, off-camera casters, grass/wind,
reflection views, 1/4 потоков. Сравнить depth/HDR, не только hashes.

Владельцы: `Apps/App.Base/Include/App.Base/Systems/ShadowVisibilitySystem.h`,
`Engine/Engine.RendererDX12/Include/Engine.RendererDX12/GDX12LocalLights.h`.
Основная проверка: `Apps/App/tests/LightingGpuSmoke.cpp`.

## 2. Сократить агрегацию snapshot и повторное объединение bounds

**Приоритет: высокий; snapshot ~17 мс, из них агрегация ~9,3 мс.**

- Начать с дешёвого изменения: вычислять общие reflection/caster min/max
  в одном обходе, не пересобирать caster bounds вторым проходом перед cascades.
  Проверить консервативность и точность на больших/тонких bounds.
- Кэшировать signature вместе с generational primitive handle, чтобы неизменная
  статика не проходила повторное byte hashing тех же полей и идентификаторов.
- Переиспользовать агрегированные static bounds/metadata при неизменной статике;
  обновлять dynamic contributions каждый кадр. После удаления/сжатия границ
  выполнять корректное восстановление bounds: union-only кэш здесь непригоден.
- Не полагаться только на ECS events: существующие пользователи и тесты меняют
  transform/material/visibility напрямую. Сохранить действующие проверки revisions
  до доказательства полного покрытия всех путей изменений.
- Параллелить reduction только если последовательные кэши оставляют существенный
  остаток; сохранить стабильный порядок вычисления scene/probe signatures.

Рабочая цель: snapshot <=9 мс на стартовой камере. Сначала держать отдельно
измерения hashing, bounds и публикации, чтобы не принять перенос работы за выигрыш.

Проверки: текущий ObjectDataJobsTests плюс удаление крайнего caster, изменение
static bounds, смена world, пустая сцена, добавление application primitive,
material/visibility/classification edit и reimport во всех frame slots.

Владельцы: `Apps/App.Base/src/RenderModule.cpp`,
`Engine/Engine.RendererDX12/Include/Engine.RendererDX12/GDX12RenderGeometry.h`.

## 3. Оптимизировать физику по цене тика и пропускной способности

**Приоритет: высокий; конкретные правки выбираются после этапа 0.**

- Если доминирует ECS synchronization: измерить per-body locks, обход sleeping
  bodies, quaternion/Euler conversions, scheduling и повторную подготовку систем.
  Менять только измеренный дорогой участок; waking/teleport/kinematic изменения
  должны оставаться видны renderer в нужном тике.
- Если доминирует Jolt: собрать стоимость collision detection / contacts / solver
  и число collision steps. Проверить актуальные collision shapes, фильтры пар
  и избыточную работу со sleeping bodies. Кэширование/baking shapes и оптимизация
  Debug Jolt уже сделаны — повторно не считать их новой гипотезой.
- Если долг возникает только при startup, определить корректную границу начала
  gameplay clock для обычного App и benchmark. Не исправлять только benchmark.
- Не менять fixed step, CCD, collision fidelity, solver iterations, wave impulses
  и не выбрасывать долг симуляции ради FPS.

Приёмка: снижение ms/tick при сопоставимом числе активных тел/контактов,
стабильный долг после прогрева и сохранённые physics/wave invariants.
Снижение суммарных ~22 мс не является достаточным доказательством.

Владельцы: `Apps/App.Base/src/App.cpp`, `Apps/App.Base/src/Physics/JoltPhysicsBackend.cpp`,
`Apps/App.Base/src/Physics/JoltJobSystem.cpp`, системы Kinematic/DynamicBodyUpdate.
Проверки: PhysicsTests, BistroPhysicsTests, CollisionGeometryTests, pause/TimeScale,
wake/teleport и несколько полных seeded waves; scale 16 отдельно как stress test.

## 4. Уменьшить запись и GPU-работу локальных теней

**Приоритет: следующий; CPU local shadows ~9,5 мс внутри graph recording.**

- Сначала измерить clear/cull/draw/copy и число dirty static/dynamic faces.
- Сейчас `LocalShadowPass.hlsl` просматривает полный InputCommands для каждой
  грязной грани и затем отбрасывает неподходящий static/dynamic класс.
  Проверить компактные списки templates/candidates для двух классов, сохраняя
  настоящий InstanceID и время жизни buffers/descriptors для queued frames.
- Если основную цену даёт множество мелких dispatches/барьеров, рассмотреть
  пакетную подготовку нескольких граней. Это следующий эксперимент, не обязательная
  часть первого изменения. Зависимости clear -> cull -> indirect draw сохранить.
- Не снижать разрешение, число необходимых граней или частоту обновления
  видимо меняющихся shadows. Копирование только dirty slices уже реализовано.

Рабочая цель: graph recording <=8 мс. GPU-выигрыш неизвестен до нового capture;
старые 16,86 мс GPU local shadows относятся к исходной версии, не к новому baseline.

Проверки: cached/every-frame lights, static/dynamic overlays, alpha-tested meshes,
пустой список, рост capacity, удаление caster, queued frame slots, reflection capture,
Debug layer и HDR/depth equivalence. Затем BistroRenderGraphTests direct/async.

Владельцы: `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12LocalShadowPass.h`,
`Engine/Engine.RendererDX12/Shaders/LocalShadowPass.hlsl`,
`Engine/Engine.RendererDX12/src/GDX12DeviceResources.cpp`.

## 5. Проверить меньшие CPU-затраты и заново определить GPU bottleneck

- BeginFrame ~5,8 мс: разделить UI, Streamline frame token/markers, Reflex/XeLL
  sleep и recreate. Fence wait расположен отдельно в RenderModule::OnUpdate;
  его нельзя автоматически приписывать BeginFrame. Корректное pacing ожидание
  нельзя удалять как бесполезную CPU-работу. FrameGenerationTests обязательны.
- Skeletal preparation ~4,7 мс: проверить работу между обновлениями позы и копии
  Pose/palettes, повторный hash и bounds. Кэшировать только с учётом animation
  revision, transform/material edits, reset, pause и previous-frame motion.
  Частоту анимации и число персонажей сохранить; SecondGroupTests обязательны.
- После CPU-изменений снять свежий Nsight GPU Trace на сопоставимой сцене/волне.
  Проверить local/sun shadows, grass expansion, lighting и SSR, выбирать следующий
  GPU-эксперимент по новому capture. Включение async/FG само по себе не считать
  ускорением реального simulation/render кадра.
- Для 30 FPS нужен CPU/GPU бюджет <=33,3 мс, для 60 FPS <=16,7 мс.
  Последний GPU interval ~30 мс показывает необходимость отдельной GPU-работы
  для 60 FPS, но не предсказывает предел FPS на других состояниях сцены.

## Общая приёмка и порядок исполнения

1. Выполнить этап 0; затем по одному эксперименту: shadow visibility -> snapshot.
   Физическую оптимизацию поднять раньше, если измерен растущий долг.
2. После каждой сохранённой правки повторно определить критический путь;
   перейти к local shadow recording, затем меньшим CPU scopes и новому GPU capture.
3. Для сравнения обычного App — не менее трёх чередующихся baseline/candidate пар,
   одинаковые executable/config fingerprints, camera/seed и сопоставимые состояния.
   Без параллельных сборок, GPU-тестов и captures. Указывать медиану, разброс,
   p95 кадров и simulated time/tick counts. Не складывать выигрыши разных стадий.
4. Проверить стартовую камеру, интерьер, двор и активную волну; один сценарий
   оставить для финального подтверждения, не использовать для выбора вариантов.
5. Принимать изменение при измеренном снижении целевого scope и общего frame time
   выше шума измерений, без регрессий других сцен, visual/physics invariants
   и Release. Проверять только затронутые suites, затем общий Bistro smoke.
6. Завершить 10-минутным realtime прогоном с waves, pause/resume и TimeScale;
   отдельно записать динамику physics debt, VRAM, allocations и p95.

Не входят: снижение настроек ради числа FPS, отключение Debug checks,
широкая смена ECS/job scheduler, новый renderer, новые зависимости без доказанной
необходимости. Сначала сокращается повторная работа в существующем pipeline.
