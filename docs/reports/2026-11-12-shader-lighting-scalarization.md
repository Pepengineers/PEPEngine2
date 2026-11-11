# Lighting/material/visibility shaders: uniformity и лишняя работа

Дата: 12 ноября 2026. Checkout `I:\PEPEngine2`, ветка `codex/bistro-rendering`.
Повторный аудит текущих исходников после `a04aeb41`. Проверены все перечисленные
в задаче shaders и связанные light-grid/graphics bindings. Два независимых агента
проверили material/visibility и shadow/probe части; запись файлов выполнял один агент.

## Изменения

### Baked forward больше не обходит local lights

В `PBR.hlsli::ShadePBR` добавлен возврат `s.Albedo` после `SamplePBRSurface`, если
`s.Baked != 0`. Оба вызывающих пути — `TransparentPass` и `ReflectionCapture`.
Существующая семантика baked уже была: `ShadeSunAndIBL` возвращает albedo,
а каждый `ShadeLocalLight` возвращает ноль. Однако последний проверяет baked
только после передачи/загрузки структуры источника и вычисления расстояния.

В baseline `transparent-before.ll` реально присутствуют девять вызовов загрузки
полей light header на итерацию перед проверкой baked. Это подтверждённая
кодогенерацией лишняя работа. Новый выход обходит весь lighting path. Проверка
расположена после material sampling, чтобы не переносить texture samples/ddx/ddy
за новую ветку. Alpha clip, aerial perspective, WBOIT и capture outputs сохранены.

### Убраны неиспользуемые VS-выходы

В `OpaquePass` и `TransparentPass` удалены выходной tangent и его преобразование/
нормализация; в transparent также удалены linear depth и расчёт view-space Z.
Все их PS-entry points прочитаны: эти поля не используются. `SampleSurfaceNormal`
строит tangent frame через производные position/UV. Vertex input layout и формат
skinning buffer сохранены; нормаль и motion-vector outputs не менялись.

DXC SM6.0, оптимизация O3, до/после:

| VS | Scalar `storeOutput` calls | `Rsqrt` calls | SSA definitions |
|---|---:|---:|---:|
| Opaque | 24 → 21 | 2 → 1 | 335 → 307 |
| Transparent | 17 → 13 | 2 → 1 | 223 → 185 |

Это размеры и операции промежуточного DXIL, не ISA instruction count и не
измерение времени GPU. Драйвер мог удалять часть неиспользуемых exports и раньше.

### Нулевые draw-команды отсекаются в cascaded shadow culling

В `ShadowPass::CS` добавлен тот же early return по `InstanceCount == 0`, который
уже есть в main culling, local shadow culling и probe culling. Его вход —
`ShadowTemplates`, связанный в `GDX12CascadedShadowPass`. Отключённая команда
больше не читает instance/material, не проходит четыре frustum-test и не
увеличивает счётчики видимых команд. В DXIL проверка count действительно стоит
перед загрузками instance и перед atomics. Проверки активных команд сохранены.

### Починен SM5.1 probe culling

FXC отклонил `ReflectionCapture` с `PROBE_CULLING=1`: локальная переменная
`matrix` конфликтовала с именем типа. Переименована в `viewProj`.
Исходная ошибка воспроизведена для `ClearCS` и `CullCS`; после правки оба
компилируются. Логика выбора shadow/camera matrix не менялась.

## Uniformity: что уже работает

| Данные/условие | Область одинакового значения | Решение |
|---|---|---|
| CBMain sun flags/direction/radiance, grid dimensions | Dispatch/draw uniform | Сохранены обычные ветки |
| Shadow/probe culling selectors, light/face IDs | Dispatch uniform | Не заменять pixel/command frustum rejects |
| Material flags/texture IDs | Один материал draw; текущие skeletal batches группируются по material ID | PS получает varying index; это не доказательство scalar ISA |
| Instance transform/skinning data | Могут отличаться между instances | Не подменять через wave-first |
| `PS` / `PSFar` / `PSNear` partition | Compile-time constant | Уже специализированы entry points |
| Deferred `word`, merged mask, выбранный light index | Wave uniform среди участвующих lanes | Уже есть WaveActiveBitOr + WaveReadLaneFirst |
| Fallback deferred light index | Может отличаться между lanes | Сохранён SM5.1 обход собственной маски |
| Local light type/shadow flags | Uniform в forward и wave-union deferred; varying в fallback | DXIL уже содержит ветки вокруг spot/point/shadow работы |
| Range, N·L, cone value, cascade/blend, cube face, shadow bounds | Per-pixel divergent | Сохранены early rejects |
| Probe index/intensity в общем цикле | Uniform среди активных lanes | Bounds и blend weight остаются per-pixel |
| Alpha clip, separator depth, normal-frame degeneracy | Per-pixel divergent, чувствительны к derivatives | Не преобразовывались в lerp/step |

Deferred union объединяет bitmask конкретных источников, а не просто IDs ячеек.
В baseline DXIL `WaveReadLaneFirst` стоит до проверки членства конкретного пикселя;
загрузки света используют его результат внутри ветки. Это уже обеспечивает общий
индекс для участвующих lanes. Из этого нельзя заключить, что драйвер выбрал
SGPR/SMEM/SALU либо конкретные scalar/vector операции. Shader не фиксирует размер
wave; нынешняя схема не вводит предположение wave32 вместо wave64. Новых wave
intrinsics в pixel shader не добавлено.

## Проверенные и отклонённые маленькие «оптимизации»

- `cascade < 3 ? SampleCascade(...) : 1`: DXC уже создаёт настоящий branch перед
  следующим sample. Последний cascade не обращается к несуществующему следующему.
- `pow(x, 2)` в attenuation уже свёрнут в `fmul`; текстовая замена не даёт нового
  выигрыша кодогенерации.
- Local shadow data не читается сразу шестью матрицами: DXIL загружает четыре
  строки только выбранной face matrix после проверок результата и shadow flags.
  Разделение структуры на light/shadow buffers не обосновано таким предположением.
- Probe intensity и bounds уже имеют отдельные ветки после оптимизации DXC.
  Разбивать исходный `if` только ради предполагаемой uniform branch незачем.
- Предварительное вычисление N·V вне local-light loop возможно, но увеличивает
  lifetime значения и меняет общий интерфейс BRDF. Нужен ISA/register-pressure
  эксперимент перед включением. То же относится к force-unroll и branch hints.

## Forward light grid: конкретный следующий шаг

Для обычных lit-материалов forward/captures по-прежнему обходят `LocalLightCount`.
Безопасная область первого переноса — основной WBOIT вид со всеми тремя depth
partitions. CPU grid в `GDX12LightGridBuildSystem` строится по frustum и сферам
источников, без occupied-depth mask, поэтому может включать свет для прозрачных
поверхностей, отсутствующих в opaque depth. Выбирать Z необходимо по позиции
самого прозрачного фрагмента, а не по `SceneDepth`.

Это не только замена shader loop. Сейчас frame uploads/masks/constants приватны
в `GDX12DeferredLightingPass`; копирование masks записывается внутри `Execute`.
Прозрачность не объявляет зависимость от этой сетки. Нужные изменения:

1. Выделить общий snapshot grid для конкретных frame slot, camera/projection и
   render resolution. Один build/upload, два потребителя. Объявить upload/read
   зависимость и lifetime в render graph; сохранить fence-safe reuse и resize.
   Текущий `GetSRVBarrier` уже включает pixel и non-pixel состояния.
2. Связать snapshot только с соответствующим WBOIT видом. Использовать тот же
   XY viewport/размер и `-mul(float4(position,1), View).z`, те же logarithmic Z
   constants/clamp и покрытие последнего слоя. Проверить jitter, разные resolution,
   пустой список света, near/far planes, границы tiles/slices и off-axis projection.
3. В `GDX12WBOITTransparencyPass` добавить CB grid и SRV masks. Сейчас b0 — draw
   constants, b1–b4 заняты, t0–t15 заняты, а Texture2DCache начинается с t16.
   Простой вариант — grid b5/t16, texture range перенести на t17 одновременно в
   HLSL и root/binding-коде; проверить все paths `EmitGeometry` и root budget.
4. Начать с обхода bitmask без wave intrinsics, общего для SM5.1/SM6.
   Затем сравнить с wave-union pixel permutation отдельно: helper lanes, discard,
   derivative semantics и wave32/64 требуют самостоятельной GPU-проверки.
5. Capture views имеют свои камеры: основную grid им не передавать. Оставить
   all-lights loop либо построить отдельные snapshots. Grass использует собственный
   forward shader/bindings; он также не получит grid автоматически.

Приёмка следующего изменения: изображение против all-lights reference для всех
partitions/captures, lights на границах и вне opaque geometry, debug layer,
SM5.1 + SM6 wave32/64 при наличии оборудования. Замерять отдельно время
прозрачности, CPU build/upload и весь кадр, с фиксированными сценой/камерой/quality.
В эту задачу непроверенная перестройка владения grid не включена.

Другой отдельный эксперимент — wave-prefix compaction command buffers: один atomic
на wave вместо одного на видимую команду. Сохранить fallback, capacity, payload и
проверки disabled draws; учитывать, что порядок atomic append уже не детерминирован.
Не внедрено без runtime сравнения.

## Проверки и ограничения

- 98 успешных offline-компиляций SDK 10.0.26100.0: 33 DXC SM6.0, 33 DXC SM6.8,
  32 FXC SM5.1. Включены opaque VS/PS/prepass/depth, transparency VS/PS/Far/Near,
  reflection VS/PS/shadow/sky/culling, shadow VS/PS/culling/replica,
  local shadow VS/PS/static/dynamic culling, main culling и deferred
  reference/mask/wave. Wave variant закономерно не компилировался под SM5.1.
- `python -B -m unittest tools.tests.test_lighting_shaders -v`: 3 теста,
  6 подслучаев, без skips. Для запуска задать DXC/FXC путями к SDK executable.
  Fixture вызывает настоящий `ShadePBR` с инструментированными surface/lighting
  функциями: baked оставляет только вызов surface, lit — surface/sun/local.
  Baseline PBR из `a04aeb41` оставляет 3 atomic call sites даже при baked=1;
  исправленный — 1. Это compile-time contract, не GPU/численный тест освещения.
- Проверены output signatures VS и SM5.1 probe culling. `git diff --check`
  выполняется перед коммитом только по файлам задачи.
- Baseline/after DXIL, DXBC, compile logs, matrix script и JSON результатов:
  `Build/ShaderLightingScalarization/` (локальные артефакты, не в коммите).

Полная сборка, приложение, GPU benchmark/capture, сравнение изображений, аппаратные
wave32/64, ISA и register pressure не выполнялись: ими в общем checkout владеет
чат диагностики. Не заявляется измеренное ускорение кадра. Остаточный риск —
runtime PSO/driver поведение и визуальная интеграция ещё требуют проверки.
Вода, third-party, импортированные assets, Engine.ini и чужие правки не менялись.
