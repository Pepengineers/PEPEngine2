# CVar и стартовая конфигурация

`BenchmarkEngine` применяет конфигурацию в своём конструкторе, до создания членов производного приложения (включая Jobs), окна и модулей. Все статические CVar к этому моменту зарегистрированы. Настройки, объявленные позднее, не участвуют в загрузке INI.

Bistro применяет INI ещё раньше, до разбора аргументов запуска: `-Loading.TransientBudgetMiB=...` переопределяет значение из INI. Повторная инициализация конфигурации в конструкторе ядра не перечитывает файл.

Приложение ищет `Config/Engine.ini` относительно своего exe, независимо от текущей рабочей папки. При первой сборке приложения MSBuild копирует туда [шаблон](../Config/Engine.ini); уже существующий файл не перезаписывается. Если файла нет, остаются значения из C++.

```ini
[ConsoleVariables]
Render.NumFrames = 3
Jobs.ThreadCount = 0
Jobs.BatchSize = 64
```

Любая ошибка синтаксиса, неизвестная секция/CVar, недопустимое значение или ошибка чтения прекращает загрузку. Путь, номер строки и причина дописываются в `Logs/Engine.log` рядом с exe, после чего выбрасывается `ConsoleConfigError`. Все три приложения завершаются с ненулевым кодом без создания окна или диалога. Если лог невозможно записать, запуск всё равно прерывается, а сообщение также выводится в stderr/отладчик.

INI разбирает [inih r62](https://github.com/benhoyt/inih/tree/r62), подключённый как submodule. После клонирования проекта выполните `git submodule update --init --recursive`. Поддерживаются UTF-8 с BOM или без него, секция `[ConsoleVariables]`, комментарии `;` и `#` в начале строки и `;` после значения. Многострочные значения отключены; максимальная длина строки — 4093 байта без перевода строки. Повторная запись имени переопределяет предыдущую, регистр имени CVar не учитывается.

## Флаги и запись

| Флаг | Во время стартовой настройки | После её завершения |
| --- | --- | --- |
| `Default` | Чтение и запись | Чтение и запись |
| `ReadOnly` | Чтение и запись | Только чтение |

`ReadOnly` не означает константу из C++: INI может переопределить начальное значение. Фаза записи закрывается после успешной загрузки конфигурации, до создания ресурсов приложения. Повторный `Initialize()` не открывает её снова; повторная загрузка INI после запуска запрещена.

```cpp
static AutoConsoleVariable<float> Exposure(
    L"Render.Exposure", 1.0f, L"Positive exposure multiplier",
    EConsoleVariableFlags::Default,
    [](float value) { return value > 0; });

const bool changed = Exposure.SetValue(1.25f);
const bool parsed = console.FindConsoleVariable(L"Render.Exposure").SetFromString("1.5");
```

`SetValue()` и `SetFromString()` возвращают `false` при отказе и сохраняют старое значение. Поддерживаются арифметические типы и enum (числовое значение underlying type); строки пока не поддерживаются. Целые числа — десятичные, bool — `true`, `false`, `1`, `0`. Переполнение, лишние символы, NaN и бесконечность отклоняются. Допустимые значения enum ограничивает переданный валидатор.

`Render.NumFrames` имеет флаг `ReadOnly` и принимает положительные значения, представимые как `int`, поскольку часть потребителей использует `GetInt()`. После создания GPU-ресурсов менять число кадров нельзя.

`Jobs.ThreadCount` имеет флаг `ReadOnly`, тип `int` и значение по умолчанию `0`. При значении `<= 0` общий пул приложения использует число логических потоков оборудования (минимум один); при положительном значении — ровно указанное число, включая вызывающий поток. Например, `4` означает три worker-потока и вызывающий поток. Настройка применяется до загрузки сцены к общему пулу ECS/подготовки объектов и загрузчика. Для изменения количества потоков требуется перезапуск приложения.

`Jobs.BatchSize` имеет флаг `ReadOnly`, тип `int`, значение по умолчанию `64` и принимает только положительные числа. Определяет минимальное число элементов диапазона enkiTS для вызовов Schedule без явного размера пакета; последний остаток может быть меньше, а планировщик может объединять больше элементов в диапазон. Явный аргумент размера пакета перед dependency переопределяет настройку для конкретного вызова. Цепочка систем приложения и начальное заполнение indirect-шаблонов используют общий дефолт; импорт и декодирование текстур явно используют один элемент. Для подбора значения достаточно изменить INI рядом с exe и перезапустить приложение, пересборка не требуется.

Размер пакета измеряется в элементах и не задаёт выравнивание кэш-линии. Для устранения false sharing нужно также учитывать адреса массивов, размеры/шаги элементов и фактические записи; enkiTS не гарантирует границы диапазонов на кэш-линиях. Автоматическое выравнивание или padding данных не выполняется.

`Loading.TransientBudgetMiB` использует `Default` и принимает только положительные целые значения; неверный бюджет в INI отклоняется до начала загрузки сцены.

Дубликат зарегистрированного имени и пустое имя отклоняются исключением и в Debug, и в Release. `TryFindConsoleVariable()` возвращает `false`/`nullptr` для отсутствующей переменной; `FindConsoleVariable()` выбрасывает исключение для обязательной настройки.

## Потоки и время жизни

Прикладные CVar объявляются как `static AutoConsoleVariable` в `.cpp`, чтобы регистрироваться до чтения INI. Общие настройки не определяются как `static` в заголовках: это создаёт отдельный экземпляр на каждую единицу компиляции. Для бюджета загрузки используются функции из `LoadingSettings.h`, а сам объект скрыт в `LoadingSettings.cpp`. Локальные CVar в тестах намеренно проверяют регистрацию и уничтожение объектов.

Чтение и запись значения через CVar сериализованы; `GetValue()` возвращает копию. Регистрацию, уничтожение и загрузку INI выполняйте без параллельных читателей: найденные указатели не продлевают жизнь объекта. Для `AutoConsoleVariableRef` внешнее значение должно жить дольше CVar; прямой доступ к нему обходит флаги, проверку и блокировку. Для новых настроек предпочтителен владеющий `AutoConsoleVariable`.

## Проверка

### Ambient occlusion

AO работает после G-buffer и применяется к непрямому освещению; direct lighting,
emissive и baked color сохраняются. Прозрачный forward-путь использует нейтральный AO.

| CVar | Значение по умолчанию | Допустимые значения |
| --- | --- | --- |
| `Render.AO.Enabled` | `true` | bool |
| `Render.AO.Quality` | `2` | 0 Off, 1 Low, 2 Mid, 3 High |
| `Render.AO.Implementation` | `2` | 0 SSAO, 1 HBAO, 2 XeGTAO |
| `Render.AO.Radius` | `100` | (0, 100000], мировые единицы; в Bistro сантиметры |
| `Render.AO.Intensity` | `1` | [0, 8], 0 даёт нейтральную видимость |
| `Render.AO.Bias` | `0.02` | [0, 1], SSAO depth bias / HBAO horizon bias |
| `Render.AO.Samples` | `-1` | -1 из пресета, иначе 4..64 |
| `Render.AO.DenoisePasses` | `-1` | -1 из пресета, иначе 0..4 |

Все имеют флаг Default и читаются на кадре. Low/Mid/High задают бюджеты 8/16/32;
алгоритмы распределяют их между направлениями/шагами по-разному. Denoise по умолчанию
1/1/2 прохода (для классических AO один проход состоит из двух осей). Ручные значения
не перезаписываются сменой качества. Off выводит ровно 1, исключая оставшийся AO
прошлого кадра. Точность XeGTAO включает исходную аппроксимацию: наклонная относительно
луча незатенённая плоскость может слегка затемняться у углов экрана.

GPU-проверка: `Apps/App/AOGpuSmoke.vcxproj` (Release/x64). Проверяет все алгоритмы,
нечётный размер 65x49, три кадровых слота, плоскость, ступень глубины, denoise 0/1/4,
Off, диапазон результатов и DX12 debug layer.

### SSR, motion blur и DoF

Граф сохраняет linear HDR до финального Reinhard + sRGB: deferred → SSSR и
specular resolve → OIT composition → XeSS → depth/motion alignment → motion blur
→ DoF → tone mapping. SSR заменяет прежнее environment specular с одним применением
DFG и specular AO; direct/diffuse/emissive/baked сохраняются. До готовности SSR history
и в Off остаётся исходный environment fallback. Смена камеры и resize сбрасывают
temporal history. Alpha результата SSSR не используется как confidence.

| CVar | По умолчанию | Допустимые значения |
| --- | --- | --- |
| `Render.SSR.Enabled` | `true` | bool |
| `Render.SSR.Quality` | `2` | 0 Off, 1 Low, 2 Mid, 3 High |
| `Render.SSR.Thickness` | `0.1` | (0, 1000000], view-space distance в мировых единицах |
| `Render.SSR.RoughnessThreshold` | `0.8` | [0, 1], perceptual roughness |
| `Render.SSR.MaxTraversalSteps` | `-1` | -1 пресет 32/64/96; иначе 8..1024 |
| `Render.SSR.TemporalStability` | `0.9` | [0, 1), вес истории |
| `Render.SSR.SamplesPerQuad` | `-1` | -1 пресет 1/2/4; иначе 1, 2 или 4 |
| `Render.MotionBlur.Enabled` | `false` | bool |
| `Render.MotionBlur.Quality` | `2` | 0 Off, 1 Low, 2 Mid, 3 High |
| `Render.MotionBlur.ShutterAngle` | `180` | [0, 360], 0 сохраняет изображение |
| `Render.MotionBlur.MaxRadius` | `32` | [1, 256], output pixels |
| `Render.MotionBlur.Samples` | `-1` | -1 пресет 8/16/32; иначе 4..64 |
| `Render.DOF.Enabled` | `false` | bool |
| `Render.DOF.Quality` | `2` | 0 Off, 1 Low, 2 Mid, 3 High |
| `Render.DOF.FocusDistance` | `1000` | (0, 1000000], сантиметры; должен превышать focal length |
| `Render.DOF.Aperture` | `2.8` | [0.5, 64], lens f-number |
| `Render.DOF.MaxCoC` | `-1` | -1 пресет 8/16/32; иначе [1, 256], максимальный radius в output pixels |

Все имеют флаг Default; ручные overrides не стираются сменой quality. У SSSR и
DoF по одной реализации, поэтому отдельного фиктивного Implementation CVar нет.
Motion blur/DoF Off точно копируют HDR. Motion blur также пропускает кадр после
cut/resize/включения, чтобы не размазывать несогласованное движение.

Порядок после XeSS выбран для стабильного HDR-входа по
[рекомендации Intel](https://github.com/intel/xess/blob/main/doc/xess_sr_developer_guide_english.md).
[AMD рекомендует DoF до upscale](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/depth-of-field/),
когда depth есть только на render resolution; выбранный здесь путь дороже и
готовит output-grid depth/motion отдельно. Depth и motion берутся одной ближайшей
выборкой с учётом отрицательного экранного сдвига RH projection jitter, без
bilinear смешивания глубин и противоположных скоростей. Это сохраняет границы,
но не восстанавливает дополнительную геометрию между исходными samples.
OIT использует opaque depth/motion: фокус/движение прозрачного слоя отдельно
от фона пока не описываются этой однослойной парой буферов.

GPU-проверка графических контрактов: `Apps/App/PostProcessGpuSmoke.vcxproj`;
алгоритмов — `FidelityFXGpuSmoke.vcxproj` и `MotionBlurGpuSmoke.vcxproj`.

### Общие CVar

Из Developer PowerShell Visual Studio, в корне репозитория:

```powershell
New-Item -ItemType Directory -Force Build/CVarTests/Manual | Out-Null
cl /nologo /std:c++17 /EHsc /W4 /Z7 /IEngine/Engine.Core/Include /IExternal/External.Libraries/inih /DINI_STOP_ON_FIRST_ERROR=1 /DINI_ALLOW_MULTILINE=0 /DINI_CALL_HANDLER_ON_NEW_SECTION=1 /DINI_MAX_LINE=4096 Engine/Engine.Core/tests/ConsoleVariableTests.cpp Engine/Engine.Core/src/ConsoleModule.cpp Engine/Engine.Core/src/Logger.cpp Engine/Engine.Core/src/RuntimePaths.cpp Engine/Engine.Core/src/BenchmarkEngine.cpp Engine/Engine.Core/src/GameTimer.cpp External/External.Libraries/inih/ini.c /FoBuild/CVarTests/Manual/ /FeBuild/CVarTests/Manual/ConsoleVariableTests.exe /link kernel32.lib
Build/CVarTests/Manual/ConsoleVariableTests.exe
```

Для проверки Release добавьте `/DNDEBUG /O2`. Тест намеренно вызывает ошибку запуска и проверяет её запись в лог. Он использует свой `Config/Engine.ini` рядом с тестовым exe и отказывается перезаписывать существующий файл.

### Захват локальных reflection probes

`ReflectionProbeComponent` задаёт включение, world-aligned influence extents,
near/far clip, режим и явную `Invalidate()`. Позиция берётся из TransformComponent,
extents масштабируются абсолютным scale. `NearPlane > 0`, `NearPlane < FarPlane <= 1000000`.
По умолчанию компоненты не создаются; при отсутствии проб cubemap-ресурсы не выделяются.

| CVar | По умолчанию | Допустимые значения |
| --- | --- | --- |
| `Render.Reflection.Enabled` | `true` | bool |
| `Render.Reflection.Quality` | `2` | 0 Off, 1 Low, 2 Mid, 3 High |
| `Render.Reflection.Implementation` | `0` | 0 Dynamic, 1 BakedStatic + Dynamic |
| `Render.Reflection.Resolution` | `-1` | -1 пресет 64/128/256; иначе степень двойки 16..1024 |
| `Render.Reflection.FacesPerFrame` | `-1` | -1 пресет 1/2/6; иначе 1..6 на пробу |

Component Mode `Inherit` использует CVar; конкретные Dynamic/BakedStatic выбираются
на отдельной пробе. Dynamic снимает всю opaque/masked геометрию. BakedStatic хранит
HDR color и соответствующий reverse-Z depth, копирует оба и дорисовывает только
объекты с `StaticMeshRenderComponent::ReflectionDynamic`; менять этот флаг следует
через `SetReflectionDynamic`. Существующие летающие предметы уже помечены dynamic.

Проба использует собственную directional shadow map, покрывающую capture volume;
main-camera CSM для неё не используется. Глобальный IBL и local shadows сохраняются,
обратной связи от SSR/локальных проб в capture нет. После шести faces выполняется
GGX prefilter из явно выбранного mip 0 с 256 samples; все roughness mips публикуются
вместе с capture position, influence и revision. До этого сохраняется предыдущее
полностью готовое окружение. Ресурсы имеют постоянные SRV slots.

Изменения проб/static geometry/materials/источника неба перезапускают поколение.
Изменения света и dynamic caster shadows сбрасывают затронутый shading cache;
budgeted faces снимаются в последовательных кадрах, поэтому быстрое движение
может давать временное расхождение между направлениями. FacesPerFrame=6 снимает
все направления в одном кадре. Ради корректности широкая scene invalidation пока
может переснимать больше статики, чем нужно; spatial dependency tracking не добавлен.

GPU fixture: `Apps/App/ProbeGpuSmoke.vcxproj` проверяет color/depth reuse,
dynamic front/behind, публикацию metadata/mips, restart, собственные sun shadows,
смену мира, Off и frame slots. Пробы предоставляет `GDX12ReflectionProbePass::GetPublished()`.

Deferred и прозрачный forward PBR используют один specular environment sampler.
Box projection применяется только внутри world-aligned influence box; вес линейно
падает от 1 в центре до 0 на ближайшей границе. Перекрывающиеся веса нормализуются;
недостающий суммарный вес заполняется глобальным environment. Diffuse IBL остаётся
глобальным, а capture использует только глобальное окружение без рекурсивных проб.
При выключенном SSR локальные specular отражения продолжают работать.

Перед SSSR готовится отдельный material buffer: RGB — prefiltered probe/global
fallback, A — perceptual roughness. Отрицательный RGB сохраняет исходную global
environment выборку SDK. Baked surfaces и sky получают A=1, исключая ненужную
stochastic трассировку; общий normal/roughness G-buffer не изменяется. Небольшой
[SDK patch](../MSBuild/Patches/FidelityFX-probe-fallback.md) подставляет этот fallback
до смешивания ray hits и denoising; alpha SSSR не трактуется как confidence.
Это prefiltered probe approximation, а не отдельная stochastic выборка куба для
каждого miss-направления. Final specular resolve заменяет тот же environment,
который использовался в PBR, применяя DFG/AO один раз.

`Apps/App/ProbeLightingGpuSmoke.vcxproj` проверяет box projection, overlap/global
blend, Off, roughness mips, baked/sky tracing mask, PBR/SSR fallback и frame slots.
