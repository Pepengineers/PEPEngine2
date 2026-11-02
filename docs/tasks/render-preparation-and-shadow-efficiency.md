# Исправить стоимость local-shadow culling и границу подготовки кадра

## Цель

Уменьшить реальную стоимость Bistro на CPU/GPU при прежней сцене и качестве изображения.
Подготовка CPU-данных заканчивается перед записью render graph; GPU-проходы получают готовые
snapshot/upload данные и сохраняют зависимости culling → ExecuteIndirect.

Начни с [измерений и ограничений захватов](../research/2026-11-02-bistro-system-order-captures.md).
Работай по этапам ниже, отдельными проверяемыми коммитами. Сначала устрани дорогие повторные
GPU-чтения и проверь результат, затем меняй организацию CPU-подготовки.

## Измеренная проблема

На RTX 3060 Laptop, 1280×800, Release, Render.NumFrames=3, неподвижная камера Bistro:

- GR engine active 98,72%, SM throughput 21,34%; этот GPU-интервал почти непрерывно занят.
- Local-shadow sequence занимает 15,5–26,8 мс: 167–168 dirty views, на каждую copy + clear + cull + indirect.
- Dispatch cost повторяет трёхкадровый цикл около 9,4 / 7,3 / 18,5 мс;
  shadow draw около 4,6 мс, copies около 2,6 мс. Причина трёхкадрового ритма не доказана.
- Каждый face culling сканирует общий буфер ёмкостью 24 777 templates. Отсев static/dynamic выполняется в shader.
- Templates и часто читаемые instance/material/light/transform данные находятся в UPLOAD heaps.
- CPU-сэмплы главного потока: local-shadow Execute присутствует в 37,2%, CollectReflectionScene — в 9,2%.
  Доли inclusive, включают драйвер и не являются процентами wall time кадра.

Raw captures и supporting JSON: `D:\Project\PEPEngine2\Build\Captures\2026-11-02-system-order`.
Основные файлы — `bistro-clean-12frames.ngfx-gputrace`, `bistro-warm-timing.wpix`, `bistro.rdc`.
Точные SHA-256, API IDs, ranges и caveats находятся в связанном отчёте.

## 1. Восстановить сравнимую базу

Создай изолированную ветку `codex/...` и отдельный Build. Сохрани чужие правки и staging.
Прочитай актуальные `AGENTS.md`, `CONTEXT.md` и ADR 0001/0002/0003.
Существующий захваченный EXE старше merge `33ca359`: для A/B собери baseline и candidate заново
одним toolchain и одинаковыми флагами, сохрани commit IDs, EXE/PDB hashes и runtime INI.

Используй MCP/CLI для Nsight, PIX и RenderDoc. Управление экраном не требуется.
Сначала проверь live schemas и доступность данных: Rust analyzer не читает WRPV v11;
native Nsight читает. Длинные trace переполняли буферы: начни с короткого прогретого окна
12 кадров, trigger frame 180, Top-Level Triage, PM bandwidth 1000. Проверяй warnings после записи.

Для нужной атрибуции добавь минимальные CPU/GPU scopes в оптимизированную profiling-сборку:
game update, frame-slot wait/upload, scene snapshot, shadow visibility, light grid, local-shadow
clear/cull/draw, graph recording и submit. Debug layer используй отдельно для correctness.
Текущие `GDX12_SCOPED_EVENT` выключены в Release; включение `_DEBUG` меняет слишком многое.

Готово, когда есть воспроизводимый baseline полного кадра и shadow stages, с прогревом,
несколькими независимыми повторами и разбросом. При отсутствии stage data зафиксируй ограничение,
а не выдавай inclusive samples или action envelopes за точное время функции.

## 2. Проверить и исправить GPU-размещение часто читаемых данных

Первый A/B — прежний алгоритм и число views, но GPU читает `DEFAULT`-копии данных local-shadow culling.
Начальные места:

- `Engine/Engine.RendererDX12/src/GDX12DeviceResources.cpp` — IndirectCommandsCache.
- `Engine/Engine.RendererDX12/src/GDX12FrameConstants.cpp` — InstanceCache, MaterialCache, LocalLightCache.
- `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/GDX12UploadBuffer.h` — фактические heap types и CopyData.
- `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12LocalShadowPass.h`.
- `Engine/Engine.RendererDX12/Shaders/LocalShadowPass.hlsl`.

Оставь CPU staging для обновлений; скопируй нужные данные в GPU memory один раз перед потребителями.
Статические templates копируй при изменении, frame-local данные — после завершения подготовки
и ожидания fence используемого слота. Начни с узкого shadow-потребителя; расширяй общий путь,
когда это действительно убирает дублирование. Простая замена enum на Default сломает CopyData:
сейчас он пишет только mapped Upload.

Проверь все writers/readers, создание/обновление/удаление meshes и materials, рост буферов,
descriptor lifetime, dirty propagation во все frame slots и освобождение после GPU fence.
Переиспользуй существующие upload/copy helpers. GPU_UPLOAD допустим лишь как измеренная
опциональная оптимизация с capability check и рабочим fallback; DEFAULT + staging — базовый вариант.

Готово, когда A/B с прежними shaders/качеством подтверждает изменение shadow GPU time и PCIe cost,
а обновления/удаления и смена slots остаются корректны. Если выигрыша сверх шума нет, откати
неоправданное усложнение и зафиксируй результат. Отдельно проверь, сохраняется ли трёхкадровый ритм.

## 3. Сократить повторный culling по результатам этапа 2

Если full-list culling остаётся существенным, подготовь отдельные static/dynamic candidate streams
или эквивалентное компактное представление. Dynamic face не должна каждый раз сканировать все
статические templates только ради раннего выхода в shader. Сохрани перестановки static/dynamic,
выход caster из frustum, удаление объекта, masked materials и invalidation shadow cache.

Следующая возможность — пакетная обработка dirty faces: clear counters вместе, cull с независимыми
выходными диапазонами, затем barriers и draws. Выбирай её только после замера оставшейся стоимости.
Текущий общий Commands/Counter переиспользуется между faces: перестановка dispatches без изменения
выходного хранения некорректна. Измерь дополнительную VRAM; используй ограниченную ёмкость/рост
и обработку переполнения, сохраняя полный набор видимых casters.

Готово, когда уменьшены измеренные scanning/dispatch cost, число/объём работ объяснены,
а итоговые visibility и shadow results эквивалентны baseline. Если этап 2 уже убрал значимую
стоимость, оставь пакетирование отдельной последующей задачей с цифрами, без добавления в этот патч.

## 4. Закончить CPU-подготовку до записи рендера

Нынешние места: `Apps/App.Base/src/RenderModule.cpp:865,911,1065`,
`GDX12DeferredLightingPass.h:64`, `GDX12LightGrid.cpp`, `ShadowVisibilitySystem.h`.
После финальных camera/light/transform updates подготовь immutable frame snapshot, caster signatures,
shadow visibility/dirty-view decisions и CPU light grid. Выполняй независимые jobs параллельно;
завершай их перед upload/потребителем. Render graph Execute записывает GPU-работу по готовым данным.
Раздели CPU collection и GPU dispatch внутри существующих смешанных методов только по необходимости.

Сохрани CPU simulation до upload, ожидание fence до записи в занятый слот, размеры после resize,
pause, multi-world и существующие secondary-device контракты. `System::Schedule` вызывает Prepare
синхронно до завершения dependency: зависимую работу нельзя просто перенести в Prepare.
GPU culling, barriers и ExecuteIndirect остаются GPU nodes с корректными зависимостями.
Сам перенос CPU-функции между методами не считается оптимизацией: покажи уменьшение работы,
ожиданий или recording critical path. Дорогой повторный hashing заменяй dirty/revision данными
только с полной invalidation при изменениях сцены.

Отдельным небольшим исправлением восстанови `BeginFrame` в Bistro Update:
`Apps/App/src/App.cpp:71` обходит базовый Update, где вызов находится в `Apps/App.Base/src/App.cpp:497`.
Проверь lifecycle на реальном application path; тест, вручную вызывающий BeginFrame, этот дефект не ловит.
Считай это исправлением correctness, а не объяснением измеренных 15–27 мс теней.

Готово, когда порядок фаз очевиден из кода и профиля, CPU light-grid/scene collection отсутствуют
в Execute, а jobs dependency и GPU lifetime проходят targeted tests.

## Приёмка и результат

1. Сохрани сцену, camera, число частиц/светов, shadow resolution/quality, upscaler и видимые casters.
   Одинаковые входы для correctness; динамическая нагрузка отдельно для performance.
2. Выполни подходящие существующие lighting/shadow, render graph, object-data/jobs и Bistro tests;
   добавь короткие регрессии для изменённых lifetime/invalidation/application-order контрактов.
   Проверь GPU debug-layer errors отдельно от timing runs и визуальные кадры при движении объектов.
3. Для baseline/candidate представь median/p95 full-frame, CPU preparation/recording/wait,
   GPU shadow clear/cull/draw/copy, число views/commands/barriers, PCIe и VRAM. Укажи повторяемость,
   частоты/условия GPU, capture overhead и качество данных. Первые 12 кадров — ориентир, не p95 benchmark.
4. Оставь только изменения с доказанным эффектом либо необходимые исправления correctness;
   объясни отрицательные результаты. Снижение качества и отключение эффектов не являются починкой.
5. Проверь diff, создай локальные коммиты только своих правок. Передай hashes, команды проверок,
   пути к захватам и таблицу до/после. Push выполняй только по запросу пользователя.

Этот документ — задание; оптимизации, A/B DEFAULT и FG runtime regression ещё не выполнены.
