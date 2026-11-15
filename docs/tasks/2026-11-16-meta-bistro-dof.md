# Bistro: Depth of Field в ordinary Meta

Следующий этап после атмосферы по запросу пользователя 2026-11-16.
Репозиторий `D:\Project\PEPEngine2`, обычный `App.MetaRenderGraph`.
При подготовке HEAD `e313d2f9`; реализация атмосферы и её отчёт находятся в working
tree, ещё без коммита. Перед работой сверить status/HEAD и сохранить все чужие
staged/unstaged изменения. Не откатывать атмосферу к HEAD ради чистой основы.
Источник очереди: [план переноса](../plans/2026-11-15-bistro-incremental-migration.md).

## Результат и границы

Перенести существующий FidelityFX DoF из App, сохранив его оптическую модель и
quality presets. Порядок ordinary кадра:
готовый HDR сцены с атмосферой, водой, стеклом и частицами → DoF → Motion Blur
и complete motion → reconstruction/display → screen UI → один PRESENT.
DoF получает актуальную глубину сцены и работает на render resolution выбранного
апскейлера. Screen UI остаётся резким и output-sized.

Атмосфера, текущая дневная дуга и согласованный размер солнечного диска сохраняются
по фактическому состоянию на начало работы. Вода остаётся в текущем режиме opaque
particles с temporal fix `83755c8c`. Не возвращать старую water optics композицию.
Сохранить качество/число объектов остальных техник, paused world, XeSS/default
настройки обычного старта. World-space canvas и облака-картина — следующие отдельные
этапы; probes, FG и физика остаются вне этого задания.

## 1. Подключение существующего прохода

Исходники: `GDX12DepthOfField.h/.cpp`, `GDX12DepthOfFieldPass.h/.cpp`,
`Apps/App/src/Rendering/BistroRenderPipeline.cpp` и `ScenePostUI.h/.cpp`.
В диагностическом ScenePostUI::Append DoF уже записывается; ordinary AppendOpaque
и Passes(true) его обходят. Адаптировать ordinary путь минимально, не включать
полный diagnostic pipeline через `_opaqueOnly=false`.

HDR вход/выход — RGBA16F, depth — совпадающая по размерам reverse-Z сцена.
Использовать depth, опубликованную текущей композицией/водой, а не устаревший
opaque prepass по имени. Прозрачные материалы по-прежнему не дают отдельную
полноценную глубину для DoF: сохранить App поведение и отметить это ограничение.
Математику CoC брать из CalculateDepthOfFieldCoc: текущие FOV/projection,
FocusDistanceCm, FNumber, SensorHeightMm и фактическая высота render grid.
Отдельный autofocus/новую модель линзы не вводить.

DoF меняет HDR, но не depth/motion. Передать его результат и в _blur.SetInputs,
и в декларации ресурсов AppendReconstruction; сохранить добавление feature motion
частиц один раз. Инициализация/resize DoF должны быть независимы от уже созданного
blur: общий `_initialized` не должен пропустить создание DoF при позднем enable.

## 2. Bypass, ресурсы и SDK

При Render.DOF.Enabled=false, Quality=Off или неподдерживаемом маленьком grid
вернуть исходный HDR по тому же правилу активности, что существующий pass.
Обычный bypass не создаёт DoF packet, dispatch или CopyResource. Сейчас
GDX12DepthOfFieldPass::RecordCommands копирует HDR при disabled: ordinary должен
обойти этот путь. Сохранить прежние оптимизации Native/NIS и отключённого blur.

PrepareRecording выполняется владельцем до записи команд; SDK dispatch — через
Meta Executor на primary DIRECT. Объявить HDR/depth reads, HDR output write,
SDK recording boundary и зависимости потребителей. Сохранить восстановление
состояния после SDK (InvalidateCachedState), реальные states/barriers и lifetime
внутренних SDK ресурсов. Не переносить allocation/Flush в recording callback.

Контекст и targets сохраняются между кадрами. Resize/SR provider change сначала
дренирует владельца, затем связывает новые inputs и пересоздаёт зависящие от grid
ресурсы. Rings/CocLimit/RingMerge меняют SDK context через безопасное retirement;
FocusDistance/FNumber/FOV не должны пересоздавать его каждый кадр. Проверить
существующий FFX_MAX_QUEUED_FRAMES guard относительно Render.NumFrames: сохранить
защиту SDK, но не добавлять безусловный steady-state Flush/CPU readback.

Сохранить FP32 HDR fix и форматы промежуточных ресурсов из
`External/FidelityFX/dof-hdr-precision.patch`; не возвращать FP16 accumulation
ради производительности. Это ранее исправленный численный контракт.
Enable/disable и изменение CoC должны уведомлять temporal SR существующим
history-reset механизмом. При неизменных параметрах история не сбрасывается
каждый кадр; движение солнца само по себе не меняет параметры фокуса.

## 3. Настройки и приёмка

В Meta Startup.ini явно включить Render.DOF.Enabled=true, Quality=2 и исходные
параметры App: FocusDistanceCm=500, FNumber=2.8, SensorHeightMm=24,
CocLimit=0, Rings=0, RingMerge=true; сохранить существующее правило FixedCocLimit.
Использовать имеющиеся CVars, задокументировать управление фокусом и качеством.
Фокус подбирать для осмотра через эти настройки, не менять математику ради
сильнее заметного размытия. Эффективные настройки записать в отчёт.

По AGENTS.md/Verification Contract: Release build и один короткий обычный запуск
без diagnostic arguments. Увидеть объект около плоскости фокуса и размытый
передний/дальний план Bistro, резкий screen UI, GPU DoF stage и представленный
кадр без явных ошибок. Зафиксировать настройки, место осмотра, SR provider и
render/output grid. Успешный smoke не повторять; матрицы режимов, новые тесты,
проверки точности, профилирование и длительные прогоны не входят в задачу.
Непроверенные runtime переключения/resize отметить как source-reviewed.

Обновить README, план и отчёт: порядок, bypass, depth limitations, SDK lifetime,
фактические проверки. Локальный коммит только своих изменений, без push;
сообщить hash/executable и остановиться на пользовательскую оценку FPS/CPU/GPU.
Следующее задание после приёмки — world-space UI canvas по образцу MGPU-IMGUI
и существующих BistroUIPosters; автоматически его не начинать.
