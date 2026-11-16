# Bistro world-space UI в ordinary Meta — 2026-11-16

Основа `42d01391`, ветка `codex/bistro-rendering`. Посторонние изменения
`BistroAdditionsTests.cpp`, `Temp/` и task animation/UI сохранены отдельно.

## Реализация

Ordinary ScenePostUI создаёт существующий BistroUIPosters до загрузки мира,
сохраняя diagnostic-only collision owner отдельно. В PrepareMaterialBindings
после Executor::Begin/ожидания slot вызывается PrepareMaterials, до UpdateMaterialCB
и snapshot/freeze. Существующие world subscriptions/scene factory создают два
исходных quad/material; material Diffuse получает текущий slot target SRV.
Время берётся один раз из TotalTime при подготовке кадра, включая paused world;
recording не перестраивает ImGui layout. Screen UI сохраняет отдельный context.

Порядок: canvas effects → offscreen raster → material GBuffer → lighting/SSR/
composition → DoF → Motion Blur и complete motion → reconstruction/display →
screen UI → один PRESENT. SceneGeometry уже переносит prelude producers вперёд
до Compile, сохраняя/remapping native callback IDs. AppendPosters объявляет
texture reads только для `scene GBuffer ExecuteIndirect` и diagnostic
`probe HDR capture mesh/sky`. Opaque poster material не требует alpha texture
в DepthPS; depth-only shadows, AO, grass, glass и post не получают ложные reads.
Probes в ordinary не включены; диагностический color consumer сохранён.

Оба canvas проходят ordinary geometry depth/culling, имеют перспективу и
окклюзию, участвуют в DoF/MB/SR. Ни screen overlay, ни отдельного исключения из
postprocessing нет. Opaque water/temporal fix, атмосфера/дневной цикл/диск,
DoF-настройки, XeSS/default, остальные техники и число объектов сохранены.

## Содержимое и настройки

ATRIUM: центр (-214.84,177,-72.78) см; TERRACE: (-174.89,177,5.63) см.
Оба двусторонние 68×38.25 см, yaw=-63°, существующий opaque UseBakedLighting material.
CanvasId=1/2; TERRACE сохраняет исходный временной offset +3 s.
Startup: Render.UI.Quality=0 (Low), Render.UI.AsyncCompute=false, primary DIRECT.
Quality=1 — Medium; никакого UI offload или автоматического distance LOD.

| Canvas | Logical | Low raster / effects | Medium raster / effects |
| --- | --- | --- | --- |
| ATRIUM | 384×216 | 384×216 / 96×54 | 512×288 / 171×96 |
| TERRACE | 480×270 | 480×270 / 120×68 | 640×360 / 214×120 |

Low/Medium: 20/40 stars, blur radius 2/5; Medium shader detail сохранён.
Targets — RGBA8 UNORM Engine.UI OffscreenFormat, canvas grids независимы от SR/окна.
MGPU-IMGUI сверён по README, HybridUIApp.cpp, UILayer.* и UIQuality.h:
переиспользован принцип offscreen UI → texture → consumer и характер gradient/
bar/stars/blur. Bistro canvas — существующая адаптация, без sample HUD/icons/
28 gradients/minimap. Bistro blur — separable box; sample minimap — Gaussian.
Benchmark loop, ExitProcess, cross-adapter transfer sample не переносились.

## Ресурсы и lifetime

Effect/target persistent по canvas/frame slot; immutable CanvasFrame/DrawData и
EffectOwner/RasterOwner удерживаются до повторного использования завершённого
slot. CanvasEffects готовит scratch textures/descriptors на владельце до записи
и переиспользует их; scratch states восстанавливаются для следующего кадра.
Старый Record contract сохраняется для App callers. SDK-like Engine.UI boundary
сохраняет реальные UAV/SRV/RT transitions и восстановление heap/PSO состояния.
Каждая поверхность имеет свой OffscreenRenderer с frame-count ring и включённым
immutable atlas cache. Новый CPU snapshot с теми же размерами/пикселями находит
прежнюю GPU atlas texture; меняющийся атлас заменяется через существующий upload.
Отдельные owners не вытесняют атласы ATRIUM/TERRACE друг другом.
Quality/grid replacement заменяет ресурсы только завершённого slot; окно/SR
не меняют logical size. Font atlas, mesh и PSO переиспользуются; per-frame animation
не пересоздаёт targets и не вызывает steady-state Flush/readback.

## Проверка

Финальный Release build успешен (MSBuild exit=0), включая atlas-cache исправление.
Сохранились существующие C4244/LNK4099 warnings. Build logs:
`Build/meta-bistro-world-ui-build.log` и `Build/meta-bistro-world-ui-final-build.log`.
Обычный запуск без аргументов выполнен один раз:
ATRIUM и TERRACE видны перед входными дверьми Bistro с исходной камеры
(-300,200,20) см, rotation (0,117,0). Обе надписи/полосы/звёзды видны,
заполнение и рисунок изменяются; surfaces перекрывают расположенную за ними
геометрию входа, depth/material integration дополнительно проверена по коду.
Само скрытие poster стеной отдельным перемещением камеры не проверялось.
DoF действует на близкие posters; Settings/Reconstruction остаются резкими.
XeSS SDK default, effective XeSS, render 985×616 → output 1280×800.
UI Low: ATRIUM raster/effects 384×216/96×54, TERRACE 480×270/120×68.
First/reused-slot operation lists содержат два effects/raster DIRECT boundary,
GBuffer, DoF, MB, reconstruction/display, screen composition и один PRESENT.
В списке исходных IDs producers записаны в конце, но prelude reorder ставит их
впереди material reads при Compile; это не порядок записи operation-list файла.
Текущая сессия Engine.log (после строки 15687 предыдущих запусков) без ERROR/FATAL.
Session log и operation lists сохранены в `Build/Verification/meta-bistro-world-ui/`.
GPU capture/readback/profiling не выполнялись. Управление окном остановлено
физической Escape пользователя; дальнейших UI-действий не было.

Source review выявил отсутствие font atlas reuse в исходном poster path:
два ImGui contexts вытесняли атласы, а CPU snapshots имели новую identity каждый
кадр. Исправление per-surface renderer/cache проверено по коду после успешного
smoke; этот smoke по контракту не повторялся. Новый запуск финального atlas-cache
пути не выполнялся. Проверка упрощений не потребовала дополнительных изменений.
Source-review receipt: `Build/Verification/ce-code-review/meta-bistro-world-ui-20261116`.
`git diff --check` успешен. Измерений FPS/CPU/GPU и заявлений о выигрыше нет.
Новые тесты не создаются и расширенные WorldCanvasTests не запускаются по
заданию/AGENTS: замена — source review, Release build, один обычный smoke.
Runtime quality/resize/world reload проверяются только по исходникам.
Облака-картина, secondary UI GPU, probes, FG, физика и world-space input не включены.
