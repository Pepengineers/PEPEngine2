# Аудит шейдеров эффектов — 12 ноября 2026

## Область и результат

Проверены собственные шейдеры частиц, skinning, instance batching, AO, motion blur,
SSSR, атмосферы, облаков, IBL, canvas и травы, а также соответствующие точки вызова.
Исходная точка сравнения — `a04aeb41`. Анализ частиц, травы и атмосферы выполнен
независимыми агентами; изменения собраны и повторно скомпилированы в общем checkout.
Вода, общий PBR/lighting/culling, сторонние библиотеки и импортированные ассеты
не изменялись. Настройки качества, число samples, размеры dispatch и ресурсы сохранены.

Удаление лишней работы подтверждено оптимизированным DXIL. Это не замер времени
на GPU и не доказательство использования аппаратных scalar ALU: драйвер может
дополнительно оптимизировать DXIL, а время жизни значений влияет на регистры.

## Изменения

### Частицы: диапазон новых частиц без атомика на каждую запись

`ParticleSimulation.hlsl`, `Spawn` / `PublishAlive`.

`PrepareSpawn` уже резервирует число новых частиц в `Counters[3]`, ограничивая его
свободными слотами dead-list. Все прошедшие guard потоки Spawn создают ровно одну
частицу; в Spawn нет дополнительного отбрасывания. Поэтому они записывают
`AliveOut[Counters[2] + id]`, где `Counters[2]` — число переживших Simulate частиц.
PublishAlive публикует сумму survivors + spawn.

Проверены оба вызова PublishAlive, включая инициализацию с нулевыми счётчиками,
сброс счётчика следующей симуляцией и существующие UAV-барьеры. Сумма не превышает
Capacity. Изменение не требует wave intrinsics, дополнительных dispatch или барьеров.
В DXIL Spawn число `atomicBinOp` уменьшилось с 1 до 0. Атомики Simulate сохранены:
его потоки могут погибнуть и действительно требуют уплотнения.

Порядок AliveOut до изменения определялся атомарным резервированием и не был
стабильным. Новый диапазон упорядочен по spawn id; соответствие id → particle
и генерация случайных значений сохранены. Последующий Cull также уплотняет
draws атомарно. Визуальная проверка OIT и одинаковых глубин всё равно нужна.

### Частицы: восстановлена компиляция Simulate в SM5.1

FXC отвергал исходный Simulate с X3511: динамическая запись `localNormal[axis]`
в FindBoxContact приводила к обязательному разворачиванию внешнего динамического
цикла Substeps. Простое `[loop]` не помогало, давая X3531.

Обе записи заменены построением полного вектора из выбранной оси и прежнего знака.
Выбор оси, обработка параллельных slabs, времени контакта и ранние выходы сохранены.
Теперь все шесть entry points ParticleSimulation компилируются DXC SM6.0/6.8
и FXC SM5.1. Это исправление совместимости, не заявление об ускорении Simulate.

### IBL: базис вычисляется один раз на texel

`EnvironmentIBL.hlsl`: SampleBasis строит tangent/bitangent/normal до циклов
IrradianceCS и PrefilterCS. Все вызовы ToWorld/SampleGGX находятся в этом файле
и обновлены. Явное выражение преобразования сохраняет порядок прежней арифметики.
Для IntegrateBRDFCS базис константный и сворачивается компилятором.

В default PrefilterCS SM6.0 тело цикла уменьшилось с 112 до 88 DXIL-инструкций;
один `rsqrt` построения базиса перенесён из цикла в блок перед ним. Ранний выход
при нулевой roughness сохранён. Базис различается между texels; это вынесение
инварианта цикла, а не uniform значение всей группы.

### Облака: uniform ранний выход для Coverage 0/1

`Clouds.hlsl`: при нулевом и полном покрытии прежний код вычислял три fractal loops,
затем заменял opacity константой. Теперь endpoint возвращается до расчёта noise.
Проверка Enabled остаётся первой: выключенный источник сохраняет alpha=0,
включённый endpoint — alpha=1. CPU допускает и валидирует оба endpoint.
DXIL подтверждает ветвление по Coverage до входа в noise loops.

### Трава: одна diffuse-выборка для alpha test и цвета

`GrassRender.hlsl`, Surface: материал и diffuse читаются один раз. Clip остаётся
перед normal/roughness/другими картами. Отдельный DepthPS сохраняет AlphaTest.

| SM6.0 DXIL | До | После |
|---|---:|---:|
| PS: обычные sample-инструкции | 6 | 5 |
| PS: structured buffer loads | 12 | 11 |
| PS: cbuffer loads | 2 | 1 |
| CapturePS: обычные sample-инструкции | 6 | 5 |
| PrepassPS: sample-инструкции | 3 | 3 |
| DepthPS: sample-инструкции | 1 | 1 |

Проверен control flow PrepassPS: DXC оставляет diffuse sample внутри RenderLayer==1,
поэтому непрозрачный prepass не получает лишнюю выборку. Производные и discard
не заменены branchless выражениями.

### Ветер: центр сетки уже прошёл проверку границ

`WindField.hlsl`: индекс центра вычисляется непосредственно после bounds guard;
повторный clamp в Cell удалён только для центра. Соседние координаты и Bilinear
по-прежнему ограничиваются. Существующая специализация WIND_STAGE сохранена.
Число integer min/max в DXIL stages 0/5/6: 4→0; stage 1: 12→8.
Stages 2/3/4 остаются 12→12: компилятор уже использует общие ограничения соседей.

### HBAO, motion blur и подготовка SSSR

- HBAOCS запрашивает размеры Depth один раз и использует их в цикле; количество
  статических `getDimensions` уменьшилось 2→1, запрос из внутреннего цикла удалён.
  Uniform шаг угла также перенесён перед внешним циклом. Все bounds/depth/radius
  ранние отсеки и порядок накопления оставлены.
- MotionBlur Gather вычисляет Halton(sequence,2) после выхода для extent≤0.5;
  выбор соседнего tile с Halton(sequence,3) оставлен до выхода, поскольку влияет
  на него. Uniform jitterScale вынесен из sample loop. Оба переноса видны в DXIL;
  Tiles, group barriers, фильтр и значения jitter не изменены.
- SSSR Prepare восстанавливает позицию, нормализует направления и строит отражённый
  луч внутри существующей проверки reflective. Ранее эта работа выполнялась
  также для неба и baked-поверхностей. DXIL подтверждает положение матричной
  арифметики после ветвления. Outputs roughness, fallback и motion сохранены.

## Проверки и регрессии

Основной повторный прогон текущих production-файлов, SDK 10.0.26100.0 x64,
DXC/FXC `-O3`: **131 из 144 компиляций успешны**.

- Все изменённые entry points MotionBlur, SSSR, ParticleSimulation, Clouds,
  EnvironmentIBL (default/GLOBAL_SKY_FILTER/LOCAL_PROBE_FILTER), GrassRender
  (включая CapturePS) и все семь WIND_STAGE прошли SM6.0, SM6.8 и SM5.1.
- Все 13 entry points AmbientOcclusion прошли SM6.0 и SM6.8.
- 13 попыток AmbientOcclusion SM5.1 упираются в прежний синтаксис `xx` в
  `External/External.Libraries/XeGTAO/XeGTAO.hlsli:641`. HBAOCS из `a04aeb41`
  отдельно воспроизводит те же X3000/X3013. Сторонний файл не исправлялся;
  совместимость AO с FXC остаётся открытым ограничением, не регрессией этого diff.
- Дополнительно проверены 12 entry points ParticleRaster/ParticleRender/Skinning/
  InstanceBatching: **36/36** SM6.0/6.8/5.1 успешно. SkyAtmosphere (5 entry points)
  и Canvas (3 entry points × default/CANVAS_MEDIUM) прошли три модели.
  GrassExpand/ExpandCS и GrassInteraction/CS прошли SM6.0.
- FXC сохраняет предупреждения в общих PBR/CSM/LocalShadows include-файлах;
  исправленный Simulate также предупреждает об индексировании ConfineVolume
  и потенциально неинициализированных значениях при ранних выходах. Ошибок в нём нет.
- `SecondGroupTests.cpp` отдельно скомпилирован MSVC 14.52, используя compile database
  с актуальными путями checkout/toolset. Это не полная сборка и не запуск теста.
- `git diff --check` по файлам задачи прошёл.

Расширены существующие GPU-регрессии SecondGroupTests:

1. Exact live count через alpha motion target: 1, 255, 256, 257, 250000, 1000000,
   первое заполнение, отсутствие свободных слотов и повторное заполнение после смерти.
2. Последовательные когорты по 257 в пуле 1024: выжившие + умершие + новые;
   ожидаемые live counts 257, 514, 771, 771, 771.
3. Cloud Coverage 0/1 при Enabled false/true: точные RGB и alpha включения
   непосредственно из cloud texture. Read helper дополнен чтением alpha HDR.

Эти GPU-регрессии **скомпилированы, но не исполнены**. Существующие тесты
ParticleSimulationControlsChecks покрывают повёрнутый box, внутреннюю точку и
параллельный проход мимо slab; нового полного перебора осей не добавлено.

Локальные артефакты (не входят в коммит):

- `Build/ShaderEffectsAudit/`: финальные bin/asm/log, `compile-results.json`,
  baseline HBAO/MotionBlur/SSSR DXIL, `AO-before-fxc.log`, `SecondGroupTests.compile.log`.
- `Build/ShaderEffectsParticleAudit/`: baseline FXC failure и сравнение частиц,
  включая `Simulate-before-5_1.log`, `*-normal-*`.
- `Build/shader-audit-atmosphere/`: baseline/candidate IBL и Clouds, compile-results.
- `Build/grass-scalar-audit/`: baseline/candidate Grass и Wind (SDK 10.0.22621.0).

## Что оставлено для аппаратной проверки

Не вводились wave/group compaction для Simulate/Cull/GrassExpand/InstanceBatching,
shared-memory staging источников GrassInteraction и новые PSO permutations.
Эти изменения требуют дополнительных барьеров или SM5.1 fallback и могут увеличить
register pressure. Полезные ранние отсеки, неполные группы и существующие барьеры
сохранены; новых предположений о wave32/64 нет.

Нужны прогон SecondGroupTests с debug layer, AOTests/MotionBlurTests и сравнение
изображений на фиксированных входах. Для принятия как ускорения — timestamps
Spawn отдельно от Simulate, IBL фильтрации по roughness/sample count, Clouds для
покрытий 0/1/среднего, HBAO, Gather на статичном и движущемся кадре, Grass GBuffer
и SSSR Prepare при разной доле reflective pixels. Проверить регистры/occupancy IBL.

В этом чате полная сборка, запуск приложения, GPU benchmark/capture и проверка
изображения не выполнялись по разделению работы с чатом диагностики bottleneck.
Численного обещания ускорения кадра нет.
