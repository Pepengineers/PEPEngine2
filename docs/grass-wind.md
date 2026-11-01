# Grass renderer и wind field

Актуальная интеграция: код объединён в корневую ветку и прошёл CPU/GPU/GBV smoke checks. [Результаты и непроверенные области](rendering/second-group-integration.md). Авторский статус ниже — исторический, до центральной интеграции.


Статус автора: **НЕПРОВЕРЕНО**. Код добавлен без сборок, запуска HLSL, тестов,
GPU fixtures, readbacks, benchmarks и отдельного review/cleanup-прохода по
прямому указанию пользователя. Ни визуальное качество, ни производительность,
ни прохождение debug layer не утверждаются. Bistro и assets не изменены.

## Владение и API

`GrassRenderComponent` и `WindFieldComponent` входят в `WorldECS`. Их нет в
сцене автоматически. Оба требуют `TransformComponent`; wind использует только
world XZ location, его rotation/scale не масштабируют горизонтальное поле.
Grass использует весь world transform для roots и формы, а bend применяется в
world XZ, в сантиметрах. Transform должен быть finite и невырожденным
(`abs(determinant) >= 1e-9`).

Компоненты содержат авторские поля `GDX12GrassSettings` / `GDX12WindSettings`.
`SetSettings(settings)` проверяет диапазоны, копирует поля и запрашивает reset.
Прямая запись полей также поддержана: renderer валидирует snapshot перед
использованием и обнаруживает изменение настроек. Изменять компоненты нужно
на main thread между кадрами; снимок неизменяем в течение всех проходов кадра.
`GrassRenderComponent::ResetHistory()` и `WindFieldComponent::Reset()` меняют
revision. Поля `_renderOwner` принадлежат RenderModule, вручную их менять нельзя.

`GrassRenderComponent::Material` — обязательная ссылка на материал, полученный
через существующий `RenderModule::CreateMaterial/GetMaterialByName`. Принимаются
только принадлежащие модулю opaque/masked materials, transparent отклоняется.
Texture, normal/specular/roughness/emissive maps и alpha-test остаются в штатном
material/texture cache; новый asset registry не создаётся. `RootsCm` — собственная
копия точек в локальных сантиметрах: приложение может взять точки из своего
SceneAsset/MeshRegistry и передать их через settings, без владения чужой GPU
геометрией. Загрузка новых моделей и scatter по треугольникам сюда не добавлены.
WorldLoader по-прежнему загружает существующие scene/mesh assets; компоненты
создаются клиентским C++ API, собственного формата файлов у них нет.

`WindEntity` выбирает конкретный `WindFieldComponent` в том же World.
`InvalidEntity`, отсутствующий, выключенный или не имеющий transform wind
даёт нулевой bend. Несколько grass-компонентов могут использовать одно поле,
а разные поля имеют отдельные параметры, скорость времени и GPU state.
`Enabled` каждого компонента локальный. `Paused` wind замораживает его время;
учитываются также `World::IsPaused()` и `World::GetTimeScale()`.

Пример настройки клиентского кода (не добавлен в Bistro):

```cpp
auto& field = windEntity.AddComponent<WindFieldComponent>();
GDX12WindSettings wind;
wind.Implementation = GDX12WindImplementation::ProjectedGrid;
wind.Direction = Vector2(0.8f, 0.6f);
wind.SpeedCmPerSecond = 180;
field.SetSettings(wind);

auto& patch = grassEntity.AddComponent<GrassRenderComponent>();
GDX12GrassSettings grass;
grass.DensityPerSquareMetre = 80;
grass.Implementation = GDX12GrassImplementation::CrossedRibbons;
patch.SetSettings(grass);
patch.Material = grassMaterial;
patch.WindEntity = windEntity;
```

## Глобальные controls и inheritance

В `GDX12GrassWind.cpp` до startup INI зарегистрированы только четыре CVar:
`Render.Grass.Enabled=true`, `Render.Grass.Quality=2`,
`Render.Wind.Enabled=true`, `Render.Wind.Quality=2`.
Все имеют `Default`, runtime запись через `ConsoleModule` и startup
`Config/Engine.ini`. Quality принимает 0 Off, 1 Low, 2 Mid, 3 High.
Off исключает технику; Wind Off оставляет неподвижную траву. Grass Off исключает
также её wind consumers. Enabled/Quality читаются один раз для кадрового snapshot.

| Inherit field (`0`) | Low | Mid | High |
| --- | ---: | ---: | ---: |
| Grass MaxBlades | 16384 | 65536 | 262144 |
| Grass Segments | 1 | 3 | 5 |
| Wind GridResolution | 32 | 64 | 128 |
| Wind PressureIterations | 8 | 20 | 40 |

Пресеты разрешаются в копии: авторские поля не перезаписываются, явный override
живёт независимо от смены Quality. Запись `0` в соответствующее поле возвращает
inherit. Implementation не зависит от Quality. При полностью явных budgets
Quality не меняет эти budgets. Нет глобальных density/force/implementation CVar.

## Grass fields

| Поле | Default | Диапазон / смысл |
| --- | --- | --- |
| Implementation | Ribbon (0) | 0 одна ribbon, 1 CrossedRibbons: две перпендикулярные ribbon |
| ExtentsCm | (500,500) | локальные XZ полуразмеры, каждое 1..1e6 cm |
| RootsCm | empty | до 262144 finite локальных точек, координаты ±1e7 cm |
| DensityPerSquareMetre | 0 | 0..100000 blades/m²; используется для rectangle при пустом RootsCm |
| MaxBlades | 0 inherit | 1..262144; ограничивает кандидатов, не переписывает density |
| Segments | 0 inherit | 1..8 сегментов ribbon |
| Seed | 1 | uint32, стабильный hash распределения и ориентации |
| HeightCm | 40 | 0.1..10000 cm, local Y |
| HeightVariation | 0.25 | 0..1, симметричное отклонение от height; min height 0.01 cm |
| WidthCm | 3 | 0.01..1000 cm |
| BendSeconds | 0.15 | 0..10 s, художественный коэффициент velocity → displacement |
| MaxBendCm | 30 | 0..10000 cm, ограничивает world displacement |
| LodDistanceCm | 1500 | 1..1e7 cm; дальше число сегментов делится на 2 (минимум 1) |
| FarCardDistanceCm | 3000 | LodDistanceCm..1e7; дальше main/capture uses one facing textured quad |
| MaxDistanceCm | 6000 | FarCardDistanceCm..1e7; main/capture root cutoff, не shadow cutoff |
| CastShadows / CaptureReflections | true / true | локальные opt-outs для shadow/capture проходов |

Непустой `RootsCm` выбирает point source: по одной blade на точку, до MaxBlades;
density тогда не используется. Rectangle count = ceil(4*extentX*extentZ/10000*density).
Пустой point source с density=0 не имеет GPU-потребителей. Высокие budgets могут
потребовать значительную память: capacity vertices = count*2*(segments+1)*ribbons,
по 44 bytes; indices = count*6*segments*ribbons, по 4 bytes. Scratch общий для всех
компонентов/видов, размер по максимуму одного draw, а не по сумме сцены.

## Wind fields

| Поле | Default | Диапазон / смысл |
| --- | --- | --- |
| Implementation | ProjectedGrid (1) | 0 Analytic gust, 1 semi-Lagrangian advection + divergence/Jacobi/projection |
| ExtentsCm | (1000,1000) | world XZ полуразмеры, 1..1e6 cm |
| Direction | (1,0) | finite компоненты -1..1, length² > 1e-8; нормализуется в snapshot |
| SpeedCmPerSecond | 100 | 0..MaxSpeedCmPerSecond |
| GustAmplitude | 0.5 | 0..1, синусоидальная модуляция скорости |
| GustFrequencyHz | 0.5 | 0..100 Hz |
| GustWavelengthCm | 500 | 1..1e6 cm |
| ForcePerSecond | 2 | 0..100 /s, приближение grid velocity к gust target |
| DampingPerSecond | 0.2 | 0..100 /s, экспоненциальное затухание при advection |
| MaxSpeedCmPerSecond | 500 | 0.01..10000 cm/s; clamp перед/после projection, также analytic clamp |
| TimeScale | 1 | 0..10, множитель world-scaled dt |
| MaxStepSeconds | 1/60 | 0.0001..0.1 s |
| MaxSubsteps | 4 | 1..16 |
| GridResolution | 0 inherit | 8..256, квадратная grid |
| PressureIterations | 0 inherit | 1..128 |
| ResetRevision | 0 | uint32, Reset() увеличивает revision |

Grid — локальный художественный ветер с закрытыми стенками (нормальная скорость
нулевая, свободное тангенциальное скольжение), Neumann pressure и закреплённой одной
ячейкой. Поле снаружи bounds даёт нулевой bend, внутри bilinear sampling.
Projection приближённая, это не полноценная физика стебля или атмосферы.
Analytic не создаёт grid и вычисляет travelling sinusoidal gust в expansion.
`ForcePerSecond`, `DampingPerSecond`, grid/iterations относятся только к ProjectedGrid.

Dt=0 не продвигает время и не запускает solver. Предыдущее velocity копируется из
текущего, чтобы остановленная трава не сохраняла старую motion. Для grid каждый
substep ограничен min(MaxStepSeconds, 0.5*cellSize/MaxSpeed). Общее число шагов
ограничено MaxSubsteps; лишнее время отбрасывается, без catch-up spiral. Analytic
ограничивает интервал MaxSubsteps*MaxStepSeconds. Reset/settings/owner replacement
сбрасывают время и velocity history; camera reset сбрасывает motion, сохраняя simulation.

## Renderer seams

- `RenderModule::CollectGrassScene` собирает main-thread snapshots после ожидания
  frame-slot fence, до проходов. Fresh owner tokens выдаются событиями создания /
  замены ECS-компонента и при подписке World; reused entity ID не наследует state.
  Невалидные finite/range/enum/material/world данные отклоняются исключением.
- `GDX12GrassRenderer::BeginFrame` исполняет wind один раз, сохраняет предыдущее
  состояние последнего реально отрисованного кадра, а не старое содержимое frame slot.
  Grid buffers только у реально используемых ProjectedGrid wind. Пустая сцена не
  создаёт renderer/buffers; удалённые state/scratch/descriptors удерживаются до
  завершения fence. Upload buffers по frame slot, view parameters отдельно на каждый
  draw: shadows/probe faces не перезаписывают уже записанные constants.
- `GrassExpand.hlsl` culls conservative bent-blade AABB в матрице конкретного вида,
  atomically compacts vertices + indexed triangles + DRAW_INDEXED arguments.
  `ExecuteIndirect` читает GPU count без CPU count readback. Far-card переход
  сбрасывает vertex history для сменившего представление стебля.
- `GDX12OpaquePass`: early AO path пишет depth+normal/roughness; late AO path — только
  DepthVS/DepthPS + masked alpha-test. GBuffer использует ту же raster geometry,
  depth Equal, без writes, и заполняет штатные PBR/motion outputs. RH/reverse-Z,
  положительный raster jitter и motion prevNDC-currentNDC сохранены. Камера,
  resize/history reset поступают из существующего RenderPipelineCommonData.
- `GDX12CascadedShadowPass` и `GDX12LocalShadowPass`: отдельное light-frustum culling,
  normal-Z shadow depth. Camera distance не выключает caster. CSM depth interval
  дополнительно охватывает conservative world bounds grass, включая сильный bend
  и off-camera поля. Grass revision инвалидирует cached CSM и local shadow cache;
  paused неизменный компонент не требует постоянной инвалидизации.
- `GDX12ReflectionProbePass`: grass входит в dynamic/all capture geometry и полный
  sun-shadow capture; существующий static bake его исключает. Capture использует
  те же PBR materials, direct/local shadows и global IBL в linear HDR. Существующее
  ограничение mixed probes сохраняется: static baked receivers имеют static-only
  shadows; для движущейся травы на таких receivers нужен dynamic capture.
- В HDR chain изменений нет: grass G-buffer → deferred → SSSR/denoiser + единый
  specular resolve/local fallback → WBOIT HDR → DoF → motion blur → HDR XeSS → один
  tone mapping. Grass получает тот же AO, local reflections и blur через штатные
  depth/normal/motion outputs; отдельная specular добавка не введена.

Прямое использование renderer требует того же direct queue, BeginFrame один раз
после ожидания текущего frame slot и destruction после GPU drain, как у других
render passes. PIX regions отмечают wind и expansion/draw/shadow; численных timings
автор не получал. Создание CPU/GPU verification harness и readback fixtures здесь
не выполнялось по указанию пользователя; это работа центрального проверяющего.

Донор для концепции: read-only `Multi-GPU-Render-DX12/MGPU-Grass/ComputeGrass`
(SingleExpanded) и `Common/WindFluidSimulator`. Код написан заново в текущей базе;
старые отменённые feature commits не восстановлены, зависимости не добавлены.
