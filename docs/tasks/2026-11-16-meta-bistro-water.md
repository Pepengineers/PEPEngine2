# Bistro: вода / SPH и поверхность жидкости

Репозиторий `D:\Project\PEPEngine2`, обычный `App.MetaRenderGraph`.
Пользователь добавил воду следующим этапом 2026-11-16. Основа при подготовке —
`9fe0fed8`: opaque, тени, AO, SSSR, стекло, трава/ветер, частицы, персонажи,
Motion Blur, reconstruction и screen UI. Probes в этом HEAD ещё не перенесены.
Перед работой сверить HEAD/status; сохранить чужие staged/unstaged изменения.
Это задание на реализацию, при его подготовке код рендера не менялся.

## 1. Сцена, настройки и время

Переиспользовать `Apps/App/src/BistroWater.h`, `BistroWaterLayout.h`,
`Components/FluidRenderComponent.h`, `BistroWaterComponents.h` и
`Rendering/Water/BistroWaterRendering.*`. Создать существующий бассейн и его
каменный контейнер, без остальных Bistro additions. Текущая раскладка:
Center=(-1297,65.3,-425), HalfExtent=(260,38,132), yaw=135°, единицы сцены — см.
Сохранить исходные материалы, placement и общий с BistroSparkRain центр;
дождь уже существует, второй emitter не создавать.

Сверить App source/overrides и явно записать Meta Startup.ini: App.Water.Count=1,
ParticleCount=100000, PresetParticleCount=0, HeightScale=2, WaveSpeed=6,
WaveAcceleration=4, WavePeriod=6, ParticleRadius=0.15, FilterRadiusPixels=6,
Render.Fluid.Enabled=true, Quality=Mid. Фактическое число seed particles вычисляется
сеткой CreateFluidSource и может отличаться от target; сообщить оба числа.
Не снижать воду/частицы/персонажей или соседние quality ради FPS.

Сохранить ReferenceModel, seed, массу, pressure/near-pressure, viscosity,
container-local solver units и преобразование в мир. HeightScale меняет также
offset для сохранения дна; существующие виртуальные стенки выше борта — часть
App поведения, открытый перелив в этом этапе не добавлять.

BistroWaterRendering::CollectScene сейчас обнуляет ClockScale при paused world.
Дать обычному Meta явный opt-in продвижения только воды, сохранив world time scale,
component TimeScale, SimulationEnabled/RenderEnabled, ResetRevision и bounded
PlanSteps/MaxStep/MaxSubsteps. Default App pause semantics не менять. Собирать
snapshot до записи; один кадр продвигает симуляцию один раз с нужными substeps,
независимо от views/slots.
Физический мир/Jolt остаётся paused; взаимодействие тел с SPH не добавлять.

Результат шага: исходная вода действительно симулируется при paused остальной
сцене, с тем же численным контрактом App и самостоятельными simulation/render controls.

## 2. Solver и Meta ownership

Переиспользовать `Rendering/Water/GDX12FluidSolver.h`, `GDX12Fluid.cpp`,
`GDX12FluidPass.*`, shaders FluidSolver/FluidSurface и recording из
`Techniques/SceneWaterParticles.*`. Использовать существующие hash/sort/density/
integrate, surface/thickness, NarrowRange filtering и merge depth/motion.
Это перенос работающего solver, а не новый алгоритм воды.

Первый ordinary перенос local-only на primary GPU: Render.Fluid.AsyncCompute=false,
Render.Fluid.SecondaryAdapter=-1, Meta.Water/particle offload выключены; запись воды
на DIRECT через Meta Executor. Существующие очереди остальных техник сохраняются.
Не включать `_opaqueOnly=false`: diagnostic путь активирует другие техники.

Адаптировать минимально нужный water/composition owner. Старый SceneWaterParticles
читает отсутствующий ordinary `geometry._shadows`, владеет отдельной particle
симуляцией и временно подменяет глобального owner. Подать sun/local shadows и
environment явно от текущих owners. Сохранить одного владельца обычных particle
pools, BurstTotal, culling/sort и draw; параллельные SceneParticles и diagnostic
particle pipeline не должны оба исполнять один эффект.

Seed upload только при создании/reset/replacement; persistent DEFAULT pools,
solver head и предыдущие отображённые позиции сохраняются между кадрами.
Объявить реальные ресурсы по стадиям simulation/surface/resolve, а не весь список
ReadWrite в каждом packet. Owner recording сохраняется для stateful helpers.
Нет private submit/Flush в recording, CPU particle readback и per-frame обхода
статического Bistro. Изменения контейнера идут через существующие dirty journals.
Завершение GPU и замена resources/descriptors управляются Meta fences/retirement.

## 3. Композиция воды, стекла и частиц

Источник порядка — `BistroRenderPipeline::LinkInputs/BuildGraph`, готовые
far/near WBOIT и water.Resolve. Ordinary SceneTransparency сейчас использует All:
при активной воде перейти к App far/near partition, не рисовать оба пути вместе.

1. Opaque mesh/grass depth и GBuffer готовы; SPH формирует surface/thickness и
   объединённые depth/motion, сохраняя opaque inputs.
2. Opaque lighting + SSSR остаются на opaque GBuffer/depth, как App.
3. Far mesh WBOIT и его resolve формируют HDR за водой.
4. Water optics resolve использует этот HDR для refraction/absorption/scattering,
   Fresnel и отражений по существующему контракту.
5. Near mesh WBOIT; WeightedAlpha particles дописывают near targets; один near resolve.
6. Global SortedAlpha/additive particles → Motion Blur/complete motion → SR/native
   → display → screen UI → единственный PRESENT.

Far и near targets очищаются по одному разу на соответствующую фазу; частицы
не очищают их повторно. Сохранить global SortedAlpha join и существующие blends,
soft particles, wave compaction, DEFAULT Firefly VB и раздельные resource accesses.
Не вводить новую сортировку или независимые near/far particle simulations.

Разделить два depth-контракта: particle simulation/depth collision читает opaque
mesh/grass depth, как текущий App; particle raster/soft fade читает merged water
depth. SceneParticles сейчас принимает одну depth для обеих задач — этот seam
нужно адаптировать. Не добавлять двустороннюю связь дождь↔SPH или новые коллизии.

Merged depth воды — R32_FLOAT SRV/UAV, не DSV. Нельзя подавать её в включённый
SetReadOnlyDepthEnabled(true). Использовать совместимый raster вариант с shader
depth test; аппаратный test по отдельной opaque DSV допустим только с явным
разделением источников, сохранив shader rejection по merged depth и запрет depth writes.
PSO/DSV mode нельзя менять после инициализации без безопасного пересоздания.

При disabled воде/Count=0/RenderEnabled=false сохранить соответствующую симуляцию
по App semantics, а display возвращает прежние opaque depth/motion и путь без воды;
не читать старые surface targets. Bypass не добавляет water resolve/copy/filter.
Необходимый HDR copy внутри существующего активного Resolve допустим: это меняющийся
фон для композиции, не повторная загрузка статической сцены.

## 4. Освещение, postprocessing и lifetime

В текущем ordinary HEAD probes отсутствуют. Water Resolve всё же обращается к
probe radiance/metadata: обеспечить корректный static-IBL shader path или валидные
neutral bindings/count=0. Не включать probes/atmosphere ради незаполненного SRV.
Если probes уже интегрированы к старту работы, переиспользовать их published
resources и Meta dependencies, не возвращать освещение на static-only.

Передать merged depth/motion воды в ScenePostUI; feature motion частиц добавить
один раз существующим Prepare. Сохранить previous displayed water transform и
particle positions, unjittered velocity convention, reset/cut/re-enable semantics.
Temporal SR получает complete motion даже при отключённом blur; Native/NIS bypass
сохраняет оптимизации предыдущего этапа. Вода не требует включать DoF/FG.

Surface targets следуют render grid выбранного provider, UI остаётся output-sized.
После resize/provider change сначала дренировать, связать актуальные inputs,
пересоздать только screen-sized targets и сбросить screen history. Изменение
разрешения не должно само по себе пересоздавать seed/pools или начинать SPH заново:
старый SceneWaterParticles::Resize заменяет Path целиком, не копировать это поведение
слепо в обычный путь. Предыдущие targets/descriptors живут до последнего reader fence.

Результат: вода, стекло и частицы композятся как в App, камера/SR не ломают depth,
history или симуляцию; ни одна уже перенесённая техника не отключена ради воды.

## Приёмка и передача

AGENTS.md и Verification Contract в `docs/plans/2026-11-13-meta-render-graph/00-meta-plan.md`:
Release-сборка и один короткий обычный запуск без `--smoke`/`--water-smoke`.
Подойти к исходному бассейну, увидеть движущуюся поверхность, преломление/окклюзию
контейнера, существующий дождь и работающий UI; кадр представлен, явных ошибок нет.
Зафиксировать effective SR и render/output размеры. Успешный smoke не повторять.
Матрицы режимов, точность solver, новые тесты, stress/длинные прогоны и профилирование
в этот этап не входят. Runtime pause/reset/resize, не проверенные этим запуском,
честно отметить как source-reviewed; не обещать измеренный speedup.

Обновить README, migration plan и отчёт: count/seed/settings, время/substeps,
composition order, два depth-контракта, ownership/fences, bypass и ограничения.
Создать локальный task-owned коммит, без push; сообщить hash и executable.
Остановиться на пользовательскую проверку нагрузки/FPS. Reflection probes остаются
отдельным подготовленным заданием; сравнение App/Meta автоматически не начинать.
