# CVar и стартовая конфигурация

Параметры AMD SSSR/Reflections Denoiser: [defaults, ranges, presets и runtime-поведение](sssr.md#runtime-controls).
Runtime-параметры `Render.MotionBlur.*`, их единицы, ranges и независимость ручных
overrides от Off/Low/Mid/High описаны в [motion-blur.md](motion-blur.md#runtime-cvars).

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

### Общие CVar

Из Developer PowerShell Visual Studio, в корне репозитория:

```powershell
New-Item -ItemType Directory -Force Build/CVarTests/Manual | Out-Null
cl /nologo /std:c++17 /EHsc /W4 /Z7 /IEngine/Engine.Core/Include /IExternal/External.Libraries/inih /DINI_STOP_ON_FIRST_ERROR=1 /DINI_ALLOW_MULTILINE=0 /DINI_CALL_HANDLER_ON_NEW_SECTION=1 /DINI_MAX_LINE=4096 Engine/Engine.Core/tests/ConsoleVariableTests.cpp Engine/Engine.Core/src/ConsoleModule.cpp Engine/Engine.Core/src/Logger.cpp Engine/Engine.Core/src/RuntimePaths.cpp Engine/Engine.Core/src/BenchmarkEngine.cpp Engine/Engine.Core/src/GameTimer.cpp External/External.Libraries/inih/ini.c /FoBuild/CVarTests/Manual/ /FeBuild/CVarTests/Manual/ConsoleVariableTests.exe /link kernel32.lib
Build/CVarTests/Manual/ConsoleVariableTests.exe
```

Для проверки Release добавьте `/DNDEBUG /O2`. Тест намеренно вызывает ошибку запуска и проверяет её запись в лог. Он использует свой `Config/Engine.ini` рядом с тестовым exe и отказывается перезаписывать существующий файл.

## Ambient occlusion

`Render.AO.*` settings are runtime writable. Algorithms, preset inheritance, units,
defaults and ranges are listed in [ambient-occlusion.md](ambient-occlusion.md).
## Локальные отражения

`Render.Reflection.Enabled` и `Render.Reflection.Quality` — глобальные runtime Default CVar. Остальные параметры принадлежат `ReflectionProbeComponent` и изменяются из C++; единицы, диапазоны, пресеты и наследование описаны в [reflection-probes.md](reflection-probes.md). Регистрация находится в `GDX12ReflectionProbes.cpp`. `ReflectionProbeTests.exe --cpu` содержит проверки глобальных CVar и сохранения авторских параметров при смене качества. После переноса параметров в компонент проверки не запускались по новому указанию.

## GPU particles

`Render.Particles.Enabled=true` и `Render.Particles.Quality=2` — единственные
глобальные controls техники, оба runtime `Default`. Quality принимает 0 Off,
1 Low, 2 Mid, 3 High. Параметры отдельных эмиттеров задаются через
`ParticleEmitterComponent::SetSettings`, без CVar-дублей. API, единицы, значения,
наследование и ограничения описаны в [gpu-particles.md](rendering/gpu-particles.md).
Регистрация — `GDX12Particles.cpp`; проверки реализации пока не запускались.
## Depth of field

`Render.DOF.*` uses runtime-writable Default variables for enable, Off/Low/Mid/High,
focus distance in cm, f-number, vertical sensor size, CoC limit, SDK ring budget and
ring merging. Manual overrides survive quality changes; zero restores preset
inheritance. Defaults, units, ranges, HDR/XeSS contracts and checks are documented
in [depth-of-field.md](rendering/depth-of-field.md).

## SkyBox / SkyAtmosphere

`Render.SkyBox.Enabled/Quality` (defaults true/3) and
`Render.SkyAtmosphere.Enabled/Quality` (true/2) are the only global controls for
these component techniques. Quality is 0 Off, 1 Low, 2 Mid, 3 High. All four use
runtime `Default`; startup `Engine.ini` and ConsoleModule writes are supported.
Source selection belongs to World; every asset/optical/LUT parameter belongs to a
component. [API, defaults, units, ranges and integration](sky.md).
Implementation delivery is **unverified**; the author ran no builds/tests/GPU checks.
## Grass и wind

`Render.Grass.Enabled`, `Render.Grass.Quality`, `Render.Wind.Enabled` и
`Render.Wind.Quality` — единственные глобальные controls этих техник, runtime
`Default`. Enabled по умолчанию `true`; Quality `2`, допустимо `0..3`
(Off/Low/Mid/High). Все остальные настройки принадлежат `GrassRenderComponent`
или `WindFieldComponent`. [API, units, ranges, budgets и seams](grass-wind.md).
Реализация автора **НЕПРОВЕРЕНА**: сборки, тесты и GPU-проверки не запускались
по прямому указанию пользователя.
## Fluid / water

Render.Fluid.Enabled (true) and Render.Fluid.Quality (2; 0 Off, 1 Low, 2 Mid, 3 High) are the only global controls, both runtime Default. Solver, source, reset and optics belong to FluidRenderComponent. See [fluid.md](rendering/fluid.md) for its API, ranges, presets and integration contract. Implementation is unverified; no builds or checks were run by the author.
