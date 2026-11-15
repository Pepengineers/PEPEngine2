# Bistro: world-space UI canvas

Репозиторий `D:\Project\PEPEngine2`, обычный App.MetaRenderGraph.
Основа подготовки `d48aae2a`: атмосфера и DoF реализованы; исходное задание
world UI уже добавлено в `acdf4086`, здесь оно уточнено по текущим исходникам.
Это следующий этап
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

Существующий Bistro canvas — адаптация, а не полная копия sample: надпись
ATRIUM/TERRACE и RESERVE POWER, анимированный градиент, полоса заполнения,
движущиеся звёзды и separable blur из `Engine/Engine.RendererDX12/Shaders/CanvasEffects.hlsl`.
Его blur сейчас box с равными весами, а у sample — Gaussian blur minimap; не называть
их одинаковыми. Не требуется переносить весь HUD, 28 gradient textures, minimap
и статические иконки sample ради визуального паритета. Достаточен существующий
полноценный Bistro canvas с работающей анимацией и эффектами.

Сохранить CanvasId Atrium=1, Terrace=2, logical sizes 384×216 и 480×270,
исходные размещение/материалы и `Render.UI.Quality`. Документировать logical,
raster и effects размеры отдельно: GetCanvasProfile меняет raster/effects grid
по quality; они не равны разрешению окна/SR. Новые дублирующие quality controls
не нужны. Независимые width/height облаков добавляются в следующем этапе.

Текущие профили `GetCanvasProfile` (effects размеры округляются вверх):

| Canvas | Logical | Low raster / effects | Medium raster / effects |
| --- | --- | --- | --- |
| ATRIUM | 384×216 | 384×216 / 96×54 | 512×288 / 171×96 |
| TERRACE | 480×270 | 480×270 / 120×68 | 640×360 / 214×120 |

Low/Medium: 20/40 звёзд, blur radius 2/5; Medium добавляет существующий shader
detail. Targets используют Engine.UI OffscreenFormat (RGBA8 UNORM), а не формат
HDR сцены. В Meta Startup.ini явно сохранить исходный `Render.UI.Quality=0`
(Low) и local DIRECT (`Render.UI.AsyncCompute=false`). Quality=1 оставляет
существующий Medium; новые presets и автоматический distance LOD не вводить.

Исходные поверхности `BistroUIPosterAtrium` и `BistroUIPosterTerrace`:
центры (-214.84,177,-72.78) и (-174.89,177,5.63) см, yaw=-63°, размер
68×38.25 см; исходный quad двусторонний. Сохранить эти места и привязку
`GPUTexture::PrimaryDeviceTexture` к Diffuse opaque material с UseBakedLighting.
Не заменять world geometry экранным overlay и не исключать canvas из DoF/окклюзии.

## 2. Ordinary integration и порядок

ScenePostUI сейчас пропускает создание _posters в ordinary конструкторе;
MetaRenderPipeline пропускает и PrepareMaterials в PrepareMaterialBindings,
и AppendPosters в prelude. Подключить недостающий ordinary
lifecycle минимально, сохранив DoF/MB/SR/screen UI; `_opaqueOnly=false` не включать.
Не убирать constructor early return целиком: diagnostic collision view и другие
diagnostic-only owners не должны автоматически включиться вместе с posters.
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

Начать с opaque GBuffer material sampling; отдельно проверить фактический shader
depth/prepass/shadow, прежде чем объявлять им texture read. Объекты canvas должны
попасть в существующие snapshot/culling/depth/GBuffer как обычная геометрия.
Нельзя получить зависимость producer → consumer только порядком регистрации:
prelude сейчас вызывается после добавления scene operations, поэтому направление
ребра нужно задать явно/структурой графа до compilation, без цикла read → write.
Общий bindless descriptor table сам по себе не означает, что каждый packet
читает каждую canvas texture. Сохранить диагностический AppendPosters при
сужении material read domain; probes в ordinary ради posters не включать.

Первый перенос использует local DIRECT recording существующих effects/raster
с Meta Executor; сохранить текущие очереди остальных техник. Owner objects,
frame targets и descriptors живут до completion всех GPU readers. Размер/quality
меняется через безопасную замену ресурсов; resize окна/SR сам по себе не меняет
logical canvas size. Сохранять кеши static mesh/textures и font atlas; обновлять
только draw snapshot/изменённые bindings. Не пересобирать всю статическую сцену
и не создавать PSO/targets каждый кадр. CPU readback и private submit/Flush
в recording не нужны.

Учесть существующую реализацию `CanvasEffects::Record`: она создаёт Recording,
descriptor heap и два scratch GPU texture при каждом вызове. Для ordinary
canvas подготовить переиспользуемые GPU scratch/descriptors на владельце/слоте
до recording и удерживать до completion; не переносить это создание в новый
Meta callback. Изменения ограничить этим lifecycle, сохранив публичные contracts
и существующих App callers. Retained EffectOwner/RasterOwner нельзя освобождать
по завершению только producer: текущая texture ещё читается scene consumers.
Сохранить UAV → SRV, render-target → material SRV barriers и восстановление
descriptor heaps/PSO после Engine.UI recording через существующие средства.
Resize/quality/world teardown освобождают ресурсы после нужных fences; постоянный
steady-state Flush и новая полная пересборка graph из-за одной анимации не нужны.

## Приёмка и передача

По AGENTS.md/Verification Contract: Release build и один короткий обычный запуск.
Увидеть оба анимированных canvas на исходных местах, перспективу/окклюзию и резкий
screen UI; GPU выполняет effects/raster, кадр представлен, явных ошибок нет.
Записать места осмотра обоих posters, quality и logical/raster/effects размеры,
SR provider и render/output grid. По operation list и представленному кадру
зафиксировать producer → material consumer → post → screen UI → один PRESENT;
это обычная проверка результата, не запрос GPU capture/profiling. Успешный smoke
не повторять; не запускать sample benchmark, матрицы, новые тесты, profiling
или долгие прогоны. Непроверенные runtime quality/resize/reload отметить отдельно.

Обновить README, migration plan и `docs/reports/2026-11-16-meta-bistro-world-ui.md`
с producer/consumer dependencies,
ownership, отличиями от MGPU-IMGUI и фактическими проверками. Локальный коммит
только своих изменений, без push; сообщить hash/executable. Остановиться на
пользовательскую приёмку FPS/CPU/GPU. После неё — облака-картина, отдельно.

Сохранить фактические на начало работы атмосферу/дневную дугу/размер солнца,
DoF и его оптические настройки, Motion Blur, XeSS/default, opaque particle-water
с temporal fix, SSR, траву, частицы, анимацию персонажей и их число.
Не уменьшать качество остальных техник ради цены UI. Настройка облаков,
UI offload/выбор secondary GPU, probes, FG, новая физика и world-space input
не входят в приёмку этого этапа. Приложения при подготовке задания не запускать;
реализацию начинать только по отдельному запросу пользователя.
