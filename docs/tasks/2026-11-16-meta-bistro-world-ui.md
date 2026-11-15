# Bistro: world-space UI canvas

Репозиторий `D:\Project\PEPEngine2`, обычный App.MetaRenderGraph.
Основа подготовки `a071d42b`: атмосфера и DoF реализованы. Это следующий этап
[согласованной очереди](../plans/2026-11-15-bistro-incremental-migration.md).
Сверить актуальный HEAD/status, сохранить чужие staged/unstaged изменения.

## Результат

Два существующих Bistro canvas (ATRIUM и TERRACE) отображаются на поверхностях
в мире: перспектива, окклюзия стенами, анимированное UI-содержимое и эффекты.
Они участвуют в обычном HDR/post пути, включая DoF и reconstruction. Screen UI
остаётся отдельным резким слоем после display. Мир/Jolt остаётся paused,
анимация canvas имеет явное продвижение времени, как остальные ordinary эффекты.

Этот этап переносит world-space UI на основной GPU. Вычисление облаков на
выбранном GPU относится к следующему [заданию](2026-11-16-meta-bistro-cloud-picture.md).
Наличие MGPU в проекте-образце не означает, что UI offload уже реализован здесь;
не заявлять это в отчёте. Новое взаимодействие мышью/raycast по canvas не требуется.

## 1. Переиспользование и содержимое

Основные исходники:
- `Apps/App/src/BistroUICanvases.h`;
- `Apps/App/src/Rendering/UI/BistroUIPosters.h/.cpp`;
- `Engine/Engine.UI/Include/Engine.UI/WorldCanvas.h`, `src/WorldCanvas.cpp`;
- Engine.UI OffscreenUI, CanvasEffects и ScenePostUI::AppendPosters.

Референс пользователя — `D:\Project\Multi-GPU-Render-DX12\MGPU-IMGUI`:
прочитать README.md, UILayer.*, UIQuality.h и offscreen-путь HybridUIApp.cpp.
Оттуда брать принцип UI → texture → consumer и характер содержимого/эффектов;
размещение в мире и ownership уже есть в BistroUIPosters. Сохранить полноценные
анимированные canvas, не заменять их статическим изображением или пустым quad.
В отчёте кратко сопоставить переиспользованные эффекты с референсом; Low/Medium
в том sample — workload presets, а не алгоритм distance LOD. Benchmark loop,
ExitProcess и отдельную систему UI из sample в PEPEngine не переносить.

Сохранить CanvasId Atrium=1, Terrace=2, logical sizes 384×216 и 480×270,
исходные размещение/материалы и `Render.UI.Quality`. Документировать logical,
raster и effects размеры отдельно: GetCanvasProfile меняет raster/effects grid
по quality; они не равны разрешению окна/SR. Новые дублирующие quality controls
не нужны. Независимые width/height облаков добавляются в следующем этапе.

## 2. Ordinary integration и порядок

ScenePostUI сейчас пропускает создание _posters в ordinary конструкторе;
MetaRenderPipeline пропускает AppendPosters. Подключить недостающий ordinary
lifecycle минимально, сохранив DoF/MB/SR/screen UI; `_opaqueOnly=false` не включать.
WorldCanvas BuildFrame работает на ImGui owner thread и возвращает immutable
CanvasFrame/DrawData. Время продвигается один раз на кадр, не на слот/view/recording.

Создание сущностей/мешей — при загрузке сцены. PrepareFrame выполняется после
ожидания frame slot и до загрузки материалов/снимка потребителей. Проверить
порядок MetaApplication/MetaRenderPipeline и shared RenderModule, чтобы новый
SRV оказался в материале этого кадра. Сохранить подписки world create/destroy
и восстановление binding при пересоздании мира.

Порядок: effects → offscreen raster → чтение текстуры материалом геометрии
→ lighting/SSR/composition → DoF/MB/SR → screen UI. Объявить реальные Meta
read/write accesses; producer должен предшествовать material consumer независимо
от порядка добавления операций в коде. Нынешний diagnostic AppendPosters добавляет
reads ко всем предыдущим DIRECT operations: сузить до тех проходов, shader которых
действительно читает canvas texture. Не добавлять зависимости AO/постэффектам
и depth-only shadows только потому, что они на DIRECT.

Первый перенос использует local DIRECT recording существующих effects/raster
с Meta Executor; сохранить текущие очереди остальных техник. Owner objects,
frame targets и descriptors живут до completion всех GPU readers. Размер/quality
меняется через безопасную замену ресурсов; resize окна/SR сам по себе не меняет
logical canvas size. Сохранять кеши static mesh/textures и font atlas; обновлять
только draw snapshot/изменённые bindings. Не пересобирать всю статическую сцену
и не создавать PSO/targets каждый кадр. CPU readback и private submit/Flush
в recording не нужны.

## Приёмка и передача

По AGENTS.md/Verification Contract: Release build и один короткий обычный запуск.
Увидеть оба анимированных canvas на исходных местах, перспективу/окклюзию и резкий
screen UI; GPU выполняет effects/raster, кадр представлен, явных ошибок нет.
Записать места осмотра, quality и logical/raster/effects размеры. Успешный smoke
не повторять; не запускать sample benchmark, матрицы, новые тесты, profiling
или долгие прогоны. Непроверенные runtime quality/resize/reload отметить отдельно.

Обновить README, migration plan и отчёт с producer/consumer dependencies,
ownership, отличиями от MGPU-IMGUI и фактическими проверками. Локальный коммит
только своих изменений, без push; сообщить hash/executable. Остановиться на
пользовательскую приёмку FPS/CPU/GPU. После неё — облака-картина, отдельно.
