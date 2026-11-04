# C++ exceptions, SIMD и загрузка CPU: PEPEngine2 и MGPU

Дата проверки: 2026-11-02. Исследование официальных источников и текущих рабочих деревьев PEPEngine2 и `D:\Project\Multi-GPU-Render-DX12`. Настройки компилятора не менялись. По просьбе пользователя восстановлено движение Bistro; проверки описаны ниже. Это сравнение исходников, а не профиль двух запущенных приложений.

## Вывод

Политику отключённых C++ exceptions можно применять к подготовленному коду, но простое снятие `/EHsc` при оставшихся `throw`/`catch` не сохраняет поведение. В x64 MSVC SSE2 уже является базовым набором; переход на AVX2 повышает требования к CPU и требует измерения полезного выигрыша. SIMD и распределение работы между потоками — отдельные механизмы.

## Что подтверждено у Epic

UnrealBuildTool задаёт настройки по модулям через `.build.cs`. Свойство `bEnableExceptions` включает обработку исключений отдельного модуля. На уровне target есть `bForceEnableExceptions`, включающее её для всех модулей. Поэтому фраза «у Epic exceptions отключены вообще везде» некорректна как описание возможностей сборки. [Epic: Module Properties](https://dev.epicgames.com/documentation/en-us/unreal-engine/module-properties-in-unreal-engine), [Epic: Target Reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-build-tool-target-reference).

Открытые текущие страницы свойств не указывают значение `bEnableExceptions` по умолчанию. Конкретный default и все исключения из него нужно сверять с исходниками используемой версии UBT; в этой проверке приватный репозиторий Unreal не читался. Политику по умолчанию нельзя выдавать за доказанное отсутствие исключений во всех сторонних модулях.

## MSVC: что означает отключение exceptions

`/EHsc` обеспечивает стандартное C++ stack unwinding. Без `/EHsc`, `/EHs` и `/EHa` MSVC сохраняет частичную поддержку C++ exceptions и SEH, но автоматические C++ объекты вне `try` могут не уничтожаться при раскрутке стека. Microsoft прямо указывает риск утечек и undefined behavior. Это не безопасная замена существующего протокола обработки ошибок. `/EHsc-` также не означает «отключить всё»: синтаксически это `/EHs /EHc-`. [Microsoft: /EH](https://learn.microsoft.com/en-us/cpp/build/reference/eh-exception-handling-model?view=msvc-170).

`_HAS_EXCEPTIONS=0` — отдельная настройка реализации Microsoft STL, а не переключатель языка. В `yvals.h` она меняет внутренние `_TRY_BEGIN`, `_CATCH` и `_THROW`; путь `_RAISE` становится фатальным. Она не переписывает пользовательские `throw`/`catch` и код уже собранных библиотек. [Microsoft STL: yvals.h](https://github.com/microsoft/STL/blob/main/stl/inc/yvals.h). Сам Microsoft STL называет этот режим undocumented/quasi-supported в changelog исправлений. [Microsoft STL: changelog](https://github.com/microsoft/STL/wiki/Changelog/bf5d0b17600b2058e60c87fe85394ba742783928).

Практический вывод: перед отключением нужно заменить или изолировать throwing API, сохранение ошибок jobs/futures и восстановление после ошибок; исключение не должно проходить через кадры кода, собранного без корректного unwinding. Это инженерное следствие приведённых правил MSVC, а не измеренный выигрыш производительности.

## MSVC x64: SSE2, AVX и AVX2

При отсутствии `/arch` MSVC x64 использует SSE2, включая scalar floating-point и векторные вычисления. Поэтому отсутствие явно записанного `EnableEnhancedInstructionSet` не доказывает отсутствие SSE. `/arch:AVX` разрешает более новый ISA, `/arch:AVX2` дополнительно разрешает 256-битные целочисленные операции и FMA; флаг может разрешать также связанные невекторные инструкции, например BMI. [Microsoft: /arch (x64)](https://learn.microsoft.com/en-us/cpp/build/reference/arch-x64?view=msvc-170).

AVX2 нельзя считать автоматическим runtime-dispatch: скомпилированный под него общий код может выполнять инструкции до любого ручного CPU check. Для распространения нужно либо принять AVX2 как минимальное требование, либо держать отдельные ISA-варианты с корректным выбором до их исполнения. Microsoft рекомендует проверять поддержку расширений и указывает, что AVX2 binaries требуют соответствующего CPU. [Microsoft: AVX2 support](https://devblogs.microsoft.com/cppblog/avx2-support-in-visual-studio-c-compiler/).

Автовекторизатор анализирует подходящие циклы; он включён по умолчанию и работает в оптимизированных сборках `/O2`/`/Ox`. `/Qvec-report:2` показывает причины, по которым конкретный цикл не векторизован. `/Qpar` — отдельное включение автоматической многопоточности и может добавлять thread-pool/synchronization overhead. Сам `/arch` не загружает все CPU cores и не превращает последовательный render loop в job graph. [Microsoft: Auto-Parallelization and Auto-Vectorization](https://learn.microsoft.com/en-us/cpp/parallel/auto-parallelization-and-auto-vectorization?view=msvc-170), [Microsoft: GDK compiler technology](https://learn.microsoft.com/en-us/gaming/gdk/docs/tools/tools-console/visualstudio/compiler-technology?view=gdk-2604).

## Что фактически включено в проектах

В MGPU свойства `EnableEnhancedInstructionSet=StreamingSIMDExtensions2` явно указаны в `Graphics/Graphics.vcxproj` и `SingleGPU/SingleGPU.vcxproj`, в том числе для x64. Флагов AVX/AVX2 в проверенных собственных `.vcxproj`/`.props` нет. У Graphics и SingleGPU также включён `FloatingPointModel=Fast`; это отдельная политика арифметики, а не включение многопоточности.

PEPEngine2 не задаёт `/arch`, то есть на x64 уже использует базовый SSE2. В сохранённых командах Release-компиляции `Engine.Core` подтверждены `/O2 /Oi /GL /EHsc /fp:precise`. Поэтому копирование SSE2 из MGPU не даст дополнительного набора инструкций. `MultiProcessorCompilation` относится к параллельной **сборке**, а не к выполнению приложения. Win32 и выполнение AVX2-вариантов не проверялись.

## Почему CPU может быть загружен по-разному

| Участок | PEPEngine2 | MGPU |
|---|---|---|
| Обслуживание command lists | `ClearCompletedLists()` проверяет готовые списки по запросу, отдельного polling-потока нет | Каждый `GCommandQueue` создаёт поток `ProccessInFlightCommandLists`; при пустой очереди он продолжает цикл с `yield()` |
| Количество таких потоков | Нет фоновых executor-потоков этого типа | `GDevice::InitialCommandQueue` создаёт graphics/compute/copy: три потока на устройство |
| Ожидание GPU | `CPUWaitForFenceValue` блокируется на событии; кадр ждёт освобождения своего frame slot | Fence wait тоже блокирующий, но при пустых очередях executor-потоки всё равно опрашивают их |
| Отрисовка объектов | GPU culling строит списки, `ExecuteIndirect` выполняет отрисовку камеры и теней | Например, SFR обходит renderers; `ModelRenderer::Draw` обходит meshes, `GMesh::Draw` записывает отдельный draw |
| CPU jobs | enkiTS: пакеты внутри системы параллельны, системы связаны последовательными зависимостями | В исследованных путях нет эквивалента нашего ECS job graph; есть служебные потоки очередей и логирования |
| Бездействие workers | enkiTS после короткого spin блокируется на semaphore | Executor queues используют бесконечный polling + yield, без ожидания condition variable |
| Потеря фокуса | `App::MsgProc` ставит паузу, `App::Run` делает `Sleep(100)` | В общем `D3DApp::Run` проверка `isAppPaused` и `Sleep(100)` закомментированы |

Дополнительно `MGPU-SFR/SFRApp.cpp::LogWriting` постоянно опрашивает очередь, делает `flush()` и `yield()`, даже без сообщений. Это ещё один источник нагрузки без обработки сцены. Наличие `condition_variable` в классе очереди MGPU не означает блокирующего ожидания: в исследованном цикле есть `notify_one()`, но нет `wait()`.

В PEPEngine2 `RenderModule::OnUpdate` ждёт GPU fence перед повторным использованием frame slot. Затем приложение запускает системы, завершает CPU jobs и последовательно записывает render passes. Это не архитектура с независимыми game/render/RHI-потоками: job system распараллеливает подготовку данных, а не весь кадр. GPU-culling и indirect rendering также сокращают CPU draw overhead. Такой дизайн допускает короткий всплеск работы CPU и длительное ожидание GPU.

Оба проверенных пути создают swapchain/window с VSync выключенным по умолчанию. Объяснение «у нас просто включён VSync» из исходных настроек не следует. Фактические настройки работающего приложения, FG/Reflex pacing и доли ожидания требуют отдельного измерения.

Источники в рабочих деревьях:

- PEPEngine2: [очередь](../../Engine/Engine.RendererDX12/src/GDX12CommandQueue.cpp), [frame-slot wait и запись проходов](../../Apps/App.Base/src/RenderModule.cpp), [цикл и пауза](../../Apps/App.Base/src/App.cpp), [порядок jobs Bistro](../../Apps/App/src/App.cpp), [dirty uploads](../../Apps/App.Base/Include/App.Base/Systems/GPUDataUpdateSystem.h), [enkiTS wait](../../External/External.Libraries/enkiTS/TaskScheduler.cpp).
- Архитектурные решения: [последовательные системы и параллельные пакеты](../adr/0001-sequential-systems-parallel-batches.md), [indirect rendering](../adr/0003-indirect-rendering-parallel-object-preparation.md).
- MGPU, корень `D:\Project\Multi-GPU-Render-DX12`: `Graphics/GCommandQueue.cpp:80,297`, `Graphics/GDevice.cpp:103`, `Common/d3dApp.cpp:207`, `Common/ModelRenderer.cpp:12`, `Common/GMesh.cpp:48`, `MGPU-SFR/SFRApp.cpp:802,1274`.

Это подтверждает наличие лишней polling-работы в MGPU, но не доказывает, что она объясняет весь наблюдавшийся процент CPU. Три или шесть таких потоков не означают автоматически загрузку всех логических процессоров на 100%. Высокая загрузка сама по себе не показывает полезный параллелизм, низкая не доказывает отсутствие CPU bottleneck на одном потоке.

## Восстановление нагрузки Bistro

В текущем `FlyingPropSystem::TickRange` обнаружен безусловный `return` перед обходом компонентов. Jobs планировались, но движение предметов не выполнялось. После заполнения frame slots большая часть transform/instance uploads пропускалась по dirty flags. По явному указанию пользователя этот `return` удалён.

Всего двигаются 346 исходных предметов; это небольшая CPU-нагрузка даже после исправления. При `Jobs.BatchSize=64` нельзя ожидать длительной занятости всего пула только на движении этих объектов. Увеличение числа потоков не увеличивает объём полезной работы. Результаты старого [замера 31 октября](../reports/2026-10-31-job-system-implementation.md) относятся к прежней версии и здесь не выдаются за новые измерения.

В [BistroPropsTests](../../Apps/App/tests/BistroPropsTests.cpp) добавлена CPU-проверка реального `FlyingPropSystem::Schedule` на одном и четырёх потоках: время и положение каждого предмета изменяются, выставляется `DirtyFlag`. Она не требует создания DX12 renderer.

Свежий Release x64 CPU-замер после восстановления движения, настоящий Bistro, `batch_size=64`, 346 предметов, 16 прогревочных + 256 измеряемых итераций:

| Потоки | Движение, мкс | Данные объектов, мкс |
|---:|---:|---:|
| 1 | 9.33 | 47.93 |
| 4 | 10.55 | 25.00 |
| 16 | 19.11 | 34.33 |

Измерены wall-clock интервалы Schedule/Complete реальных FlyingPropSystem и GPUDataUpdateSystem; это не суммарное CPU time всех workers, FPS или время полного кадра. Render/Present и lighting jobs в интервалы не входят. Четыре потока ускоряют подготовку данных примерно в 1.9 раза; шестнадцать хуже четырёх на этом объёме. Движение слишком дёшево, чтобы окупить распараллеливание. Сборка во время замера не выполнялась. Вывод ограничен этим запуском и сценой.

Команда: `Build/bin/x64/Release/ObjectDataJobsTests/ObjectDataJobsTests.exe "D:\Project\PEPEngine2\Assets\Scenes\Amazon Lumberyard Bistro"`. Лог: `Build/cpu-comparison-bistro-benchmark.log` (exit 0).

Промежуточная пересборка внутри песочницы завершилась MSVC C1902; та же Release-сборка вне песочницы прошла. Общий ObjectDataJobsTests без аргументов дошёл до проверки материала и завершился с `An actual material edit must reach the GPU cache used by reflection capture`, до проверки displacement. Это отдельный обнаруженный сбой интеграционного теста текущего рабочего дерева, он не исправлялся и не считается успешной проверкой GPU uploads движения. Именно поэтому добавлена независимая CPU-проверка системы.

Итог проверки изменения:

- Release x64: App, ObjectDataJobsTests и BistroPropsTests собраны успешно. В текущем дереве остаются предупреждения C4267 в renderer headers и LNK4099 от сторонних библиотек; сборка не объявляется свободной от предупреждений.
- `BistroPropsTests.exe Build/tests/cpu-comparison` прошёл, включая новую проверку реального движения и DirtyFlag на 1/4 потоках. Лог: `Build/cpu-comparison-prop-tests.log`.
- CPU benchmark настоящего Bistro прошёл на 1/4/16 потоках. Общий GPU integration test завершился описанным выше сбоем материала.
- `git diff --check` для изменённых исходников прошёл. Debug, MGPU runtime и длительный визуальный прогон не запускались.

Полное отключение exceptions потребовало бы изменений в `Engine.Core` (jobs, IO/cache, console, logger), `Engine.RendererDX12` (включая `ThrowIfFailed`), `App.Base` (загрузка/отмена/cleanup), точках входа App/Editor/Benchmark и тестах, плюс проверки сторонних библиотек и публичных заголовков. Замена всех `throw` на `abort` уничтожила бы восстановление после ошибок, отмену загрузки и передачу ошибок jobs; такая миграция здесь не выполнялась.

## Что здесь не проверялось

Не измерялись сравнительные FPS и CPU active time двух приложений, влияние EH/SIMD на размер бинарника и выполнение, CPU/OS compatibility и реальные инструкции в дизассемблировании. Полная атрибуция нагрузки требует одинаковых сцены, разрешения, настроек и фокуса окна, затем CPU/GPU timeline с разделением useful work, polling и waits. Настройки MGPU и обработка ошибок PEPEngine2 оставлены без изменений.
