# Bistro: картина MGPU-Noise — 2026-11-16

Основа `7ac2b4af`, ветка `codex/bistro-rendering`. Посторонние изменения
`BistroAdditionsTests.cpp`, `Temp/` и task animation/UI не включены.

## Источник и изображение

Донор: `D:\Project\Multi-GPU-Render-DX12\MGPU-Noise\Shaders\CloudsNoiseCS.hlsl`.
SHA256: `9196D78A818BCFF0B335983FEAECCAD199E7250F1A3A7E673F368604C8C4F151`.
Git revision донора недоступна из текущего filesystem context; конкретный shader
зафиксирован hash. Изучены Common/CloudGenerator.h/.cpp, GCrossAdapterResource,
HybridNoiseApp::InitRenderPaths/ComputeClouds/RenderScene/PopulateForwardPathCommands
и project-local shader linkage. Корневой Shaders/Common и NoiseDraw не использованы.
Attribution comments с двумя Shadertoy ссылками сохранены.

Новый CloudPictureNoise.hlsl сохраняет hash/simplex, RotateMatrix, fbm,
ridged/color noise и 7/8/8/7/7 loops. Добавлены bounds check и финальное
преобразование RGB; UV/aspect формулы донора неизменны. Dispatch —
ceil(width/32)×ceil(height/32)×1; RenderTargetSize=(width,height) каждой generation.
Viewer camera не меняет шум в UV поверхности. Параметры 96-byte CloudGeneratorData:
CloudCover=0.2, CloudAlpha=8, SkyTint=0.5, CloudScale=1.1,
CloudSpeed=0.006 (constructor донора, не header 0.03), CloudDark=0.5,
CloudLight=0.3, SkyColor1=(0.2,0.4,0.6,1), SkyColor2=(0.4,0.7,1,1),
CloudColor=(1.1,1.1,0.9,1). TotalTime берётся один раз на tick, не суммируется
отдельно для двух карт и не сбрасывается при смене GPU.

Формат — R8G8B8A8_UNORM, typed UAV и UNORM SRV. Итоговый RGB донора трактуется
как display/sRGB и явно декодируется в scene-linear перед записью в UAV;
alpha сохранена. SRV не декодирует RGB повторно. Это явная адаптация цветового
контракта вместо недопустимого sRGB UAV донора, без предположения о RGBA16F HDR.
Opaque UseBakedLighting material показывает картинку без повторного PBR lighting;
обычный display transform/DoF/MB/SR остаётся единственным scene post/display путём.

## Поверхность и настройки

Отдельная BistroCloudPicture над входом, центр (-194.86,230,-33.58) см,
rotation=(0,-63,0), размер 100×50 см. Использован тот же двусторонний quad,
что у BistroUIPosters, через общий CreateSurfaceMesh; новый UI renderer не создан.
Поверхность создаётся существующей scene-factory цепочкой при загрузке Exterior.
Рисунок стабилен в UV и участвует в обычном depth/culling/GBuffer как геометрия.
ATRIUM/TERRACE и screen UI сохранены.

Startup.ini: Enabled=true, Width=1024, Height=512, Adapter=-1 в Meta.CloudPicture.*.
Width/Height независимы от окна, SR, world size и Render.UI.Quality.
Adapter=-1 — актуальный primary; >=0 — индекс DXGI EnumAdapters1.
Совпадение LUID с primary выбирает local путь. Software/недоступный adapter,
неподдерживаемый format и недопустимые grid/footprint отклоняются явно.
Runtime отклонённая настройка логируется один раз на изменение, прежняя валидная
generation сохраняется; скрытого выбора другой карты нет.
Disabled скрывает только картину через её draw command InstanceCount=0,
обнуляет material binding и не добавляет producer/import/material-read packets.

## Graph и transport

CloudPictureTexture — узкий owner persistent targets/PSO/root/descriptors.
Local: DEFAULT UAV texture → GBuffer SRV, без staging/cross-adapter copy.
Remote: secondary device-local UAV → COPY в shared row-major buffer → primary
COPY в primary-local sampleable texture → GBuffer. CPU image readback отсутствует.
Использованы существующие QueueSubmission, CompletionToken, Executor external
waits и bounded per-frame slots. Deterministic 256-word CrossAdapterTransfer
fixture не запускается и не расширяется в общий scheduler; water/particles не менялись.

Публикация несёт format/grid/generation/sequence и уже issued completion token.
StrictPublication использует snapshot текущего tick: GPU wait стоит у primary
import, затем material-read dependency ведёт в GBuffer. LatestCompleted и
скрытая задержка на кадр не вводились. Secondary план — DIRECT producer и COPY
export; primary import — COPY. SceneGeometry prelude reorder ставит producers
перед material consumers с сохранением native IDs. Shader/record callbacks
не выделяют ресурсы, не submit/Flush и не CPU-wait.

1024×512 RGBA8: 4096 bytes/row, 512 rows, footprint 2,097,152 bytes на publication;
три слота. Shared heaps/handles, локальные textures и descriptors persistent.
Перед reuse учитываются последние успешно submitted primary tokens по каждой
очереди, включая частичный провал кадра; secondary slot также дренируется перед
повторным использованием. Новую публикацию CPU не ожидает, только старых readers
при retirement. Внешние graph states возвращаются в COMMON, shared buffers
учитывают copy-queue decay. При ошибке drain generation удерживается по существующей
quarantine policy Meta, без освобождения ресурсов под неизвестной GPU работой.

Runtime grid/adapter change сначала конструирует и валидирует replacement,
затем дренирует и уничтожает старый primary executor, пока старый secondary
device ещё жив, и дренирует/заменяет producer generation вместе с submission
domain. Текущий валидный результат остаётся до GPU-ready
replacement, descriptor/current-slot binding публикуется до material uploads.
Ошибки самого drain не скрываются как ошибка пользовательских настроек.
Steady-state Flush, CPU readback и per-frame allocation отсутствуют.
Meta.Clouds.Enabled=false, SetClouds не вызывается для картины; environment
revision/IBL/probes и temporal SR history не меняются из-за её анимации.
SlotBindings запоминает стабильный target каждого primary frame slot:
material dirty/revision обновляется при новом binding/generation/disable,
после warmup не меняется каждый кадр только из-за анимации картинки.

## Проверка

Release build успешен после исправления C++17 comparison syntax; сохранены
существующие C4244/LNK4099 warnings. Build log: Build/meta-bistro-cloud-picture-build.log.
Один обычный запуск без аргументов выполнен со secondary, временно задав
Adapter=1 только в Startup.ini рядом с executable. Source и deployment Startup.ini
после smoke возвращены к Adapter=-1; следующий обычный запуск использует primary.

Фактически: primary NVIDIA GeForce RTX 3060 Laptop GPU; producer AMD Radeon(TM)
Graphics, effective DXGI index=1, LUID=0:57028; requested/effective grid=1024×512,
remote=1, strict=1, rowPitch=4096, transferBytes=2097152, slots=3.
Журнал зафиксировал generation=1 sequence=1 issued secondary token=2.
Картина с движущимся рисунком облаков видна над входом, оба анимированных canvas
и обычный Bistro сохранены, явного z-fighting не видно. Камера исходная
(-300,200,20) см, rotation=(0,117,0). Screen UI резкий; effective XeSS SDK default,
render 985×616 → output 1280×800. Primary operation lists содержат strict import,
GBuffer и обычный post/display/screen/PRESENT; producer/issued publication
подтверждены secondary log и видимым GPU результатом. Исходные operation IDs
не являются порядком исполнения после prelude reorder.
Текущая сессия с 2026-11-16T00:31 по UTC (03:31 по Москве) без ERROR/FATAL;
логи сохранены в Build/Verification/meta-bistro-cloud-picture/.

Успешный smoke не повторялся. Local path, runtime resize/adapter/enable и
quarantine reviewed по исходникам; runtime NOT RUN. После smoke source review
исправил удержание generation при неуспешном drain, порядок уничтожения старого
executor до его secondary device и steady material revisions через SlotBindings.
Все три правки проверены по коду и финальной Release-сборкой (exit=0), без нового
запуска приложения. Финальный log: Build/meta-bistro-cloud-picture-final-build.log.
Code review завершён без оставшихся actionable findings; receipt:
Build/Verification/ce-code-review/meta-bistro-cloud-picture-20261116/.
Проверка упрощений удалила два лишних комментария; git diff --check успешен.
Новые тесты, режимные матрицы, profiling, сравнения точности
и длительные прогоны не выполнялись по заданию/AGENTS. Выигрыш FPS не заявлен.
Probes, FG, физика и итоговое App/Meta сравнение не начинались.
