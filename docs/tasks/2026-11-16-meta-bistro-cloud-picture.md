# Bistro: облака-картина, разрешение и GPU вычисления

Репозиторий `D:\Project\PEPEngine2`, обычный App.MetaRenderGraph.
Основа подготовки `a071d42b`; начать после реализации и пользовательской приёмки
[world-space UI](2026-11-16-meta-bistro-world-ui.md). Перед работой сверить live
HEAD/status и сохранить чужие правки. Это последний выбранный этап текущей
[очереди](../plans/2026-11-15-bistro-incremental-migration.md), не разрешение
автоматически начинать сравнение App/Meta или отложенные техники.

## Результат и настройки

Одна картина на стене Bistro показывает движущиеся процедурные облака.
Пользователь независимо задаёт width/height текстуры и GPU, на котором считаются
облака. World-space UI canvas, screen UI, глобальная атмосфера/солнце, вода и
остальные техники сохраняются. Картина — дополнительная поверхность, не замена
ATRIUM/TERRACE и не полноэкранный слой. Выбрать доступную для осмотра стену
без z-fighting, записать transform и положение камеры в отчёт.

Источник алгоритма по уточнению пользователя — **MGPU-Noise из MGPU DX12**,
не GDX12CloudComponent/CloudNoise.hlsli из PEPEngine2. Сохранить параметры
CloudGeneratorData донора. Для управления картиной добавить CVar/INI:
`Meta.CloudPicture.Enabled`, `.Width`, `.Height`, `.Adapter`.
Предлагаемые стартовые значения: true, 1024, 512, -1 (актуальный primary).
Adapter >=0 — DXGI hardware adapter index по существующей enumeration проекта;
если его LUID совпадает с primary, используется local путь. Логировать requested
и effective размеры/index/name/LUID. Width/height должны быть ненулевыми и
допустимыми для устройства/формата/footprint; проверять overflow размера transfer.
Некорректный/недоступный GPU отклоняется с понятной причиной, без скрытого fallback.
При смене настроек во время работы сохранить последний валидный результат до
готовности нового. Размеры картины в мире, окно, SR и UI quality не меняют её grid.

## 1. Перенос 2D producer из MGPU-Noise

Обязательные исходники в `D:\Project\Multi-GPU-Render-DX12`:
- `MGPU-Noise/Shaders/CloudsNoiseCS.hlsl` — compute entry CS, RWTexture2D,
  numthreads(32,32,1), hash/simplex noise, fbm и ridged noise;
- `Common/CloudGenerator.h/.cpp` — CloudGeneratorData, ChangeTextureSize,
  PrimeCompute/SecondCompute/PrimeCopy, параметры и resource bindings;
- `MGPU-Noise/HybridNoiseApp.cpp` — InitRenderPaths, ComputeClouds, RenderScene
  и выбор SRV в PopulateForwardPathCommands;
- `MGPU-Noise/MGPU-Noise.vcxproj` — связь с Common и project-local shader.

CloudGenerator компилирует `Shaders/CloudsNoiseCS.hlsl`. Project-local файл и
`Shaders/Common/CloudsNoiseCS.hlsl` в корне репозитория различаются; источником
задания является версия MGPU-Noise. NoiseDraw.hlsl — другой pixel-shader путь,
не подмена нужного compute texture producer. Зафиксировать donor revision/hash
использованного shader в отчёте; сохранить исходные attribution comments.

Перенести арифметику, RotateMatrix, последовательность 7/8/8/7/7 noise loops,
смешивание sky/cloud цвета и CloudGeneratorData в узкий Meta owner. Сохранить
фактические constructor defaults: CloudScale=1.1, CloudSpeed=0.006,
CloudDark=0.5, CloudLight=0.3, CloudCover=0.2, CloudAlpha=8, SkyTint=0.5,
SkyColor1=(0.2,0.4,0.6,1), SkyColor2=(0.4,0.7,1,1),
CloudColor=(1.1,1.1,0.9,1). В header CloudSpeed=0.03, но constructor его
переопределяет; не принимать header value за runtime default. Управление
разрешением/GPU не должно незаметно менять число octaves или качество алгоритма.

Результат — реальная Width×Height texture, без cubemap и пересчёта шести граней.
В donor ChangeTextureSize group counts округляются вниз, shader не проверяет
границы, RenderTargetSize задаётся как (height,width) только в constructor.
Для произвольного прямоугольного grid исправить integration: обновлять
RenderTargetSize=(width,height) при resize, dispatch ceil(width/32)×ceil(height/32)×1
и bounds check перед записью. Сохранить формулы UV/aspect shader и фиксированное
изображение в UV картины: viewer камера не меняет projection шума.
Не заменять алгоритм на CloudDirectionDensity из существующего environment.

Показывать texture в обычном материале/поверхности, используя готовые способы
binding из world-space этапа; не создавать новый UI renderer для облаков.
Сверить format/view contract донора: CloudGenerator выбирает формат через
GetSRGBFormat(R8G8B8A8_UNORM). Для Meta явно согласовать typed UAV format,
SRV decoding и scene-linear material; sRGB UAV напрямую не использовать.
Не считать результат автоматически RGBA16F linear как у прежнего cubemap.
Сохранить цвета исходной картинки без двойной gamma/tone mapping; выбранный
формат, нужную конверсию и material mode описать в отчёте.
Картина проходит scene depth/DoF/MB/SR как геометрия, screen UI — отдельно.

Meta.SceneEnvironment::SetClouds сейчас заменяет global sky/IBL; для картины
этот вызов не использовать. `Meta.Clouds.Enabled` остаётся false. Анимация
картины не увеличивает environment revision, не перефильтровывает IBL/probes
и не сбрасывает temporal SR каждый кадр. Время/parameters snapshot захватывается
один раз на frame tick и работает при paused физическом мире; смена GPU не
обнуляет TotalTime. Не инкрементировать его отдельно для обоих GPU, как при
вызове двух donor Compute методов. Disabled — без producer dispatch/transfer и чтения stale SRV;
безопасно скрыть только картину.

## 2. Local и cross-adapter пути

Primary: persistent DEFAULT texture → material SRV, без staging/cross-adapter
copy. Producer/consumer ordering и resource states принадлежат Meta graph.
Ресурсы и PSO создаются до recording, inputs immutable; private Flush/submit
из callback и CPU readback картинки не допускаются.

Другой GPU: отдельный device-local producer → GPU transfer → primary-local
sampleable texture. Использовать существующие QueueSubmission/CompletionToken,
executor external waits и схемы bounded publication/retirement из
Graph/CrossAdapterTransfer.*, Graph/SimulationDomain.h, Techniques/WaterTransfer.*.
CrossAdapterTransfer сейчас является fixed buffer fixture (256 words), а не
готовым texture transport: адаптировать нужные механизмы, не подключать картинку
к его deterministic payload и не запускать fixture в обычном приложении.
Не переписывать water/particle offload и не строить новый общий scheduler.

Сверить CloudGenerator/GCrossAdapterResource и ComputeClouds/RenderScene из
MGPU-Noise с существующими Meta transport primitives. Алгоритм и texture producer
переносятся из донора; его benchmark/calibration loop и queue ownership заменяются Meta.
Для portable transfer допустим shared row-major buffer с GetCopyableFootprints,
CopyTextureRegion и правильными row pitch/offsets; shader читает локальную
текстуру, не shared staging. Формат, размеры, generation и sequence входят
в publication contract; shared fences связывают producer → copy/import → reader.
Каждый успешно submitted reader учитывается при освобождении слота, включая
частично неуспешный кадр. Не перезаписывать shared/display slot, пока есть GPU readers.

Первый вариант использует результат того же snapshot кадра (StrictPublication),
чтобы GPU selection не менял фазу/задержку анимации. Ожидание GPU ставить перед
первым consumer, не блокировать CPU всё время вычисления. LatestCompleted с
задержкой N→N+1 не вводить скрыто; это отдельное последующее решение пользователя.
Resize/смена adapter создаёт новую generation с retirement старой; дескрипторы
заменяются только для безопасного frame slot. Размер/adapter не меняются каждый
кадр, shared heaps/handles/contexts и слоты имеют ограниченный lifetime/число.

## Приёмка и передача

Release build и один короткий обычный smoke по AGENTS.md/Verification Contract.
Если доступен второй аппаратный GPU, этот запуск выполнить с выбранным secondary:
увидеть движущиеся облака на стене и готовый primary кадр, явных ошибок нет;
по operations/log подтвердить producer adapter, publication и primary material read.
На одном GPU проверить local путь и явно отметить cross-adapter runtime NOT RUN;
не выдавать source review за аппаратную приёмку. Матрицы режимов, profiling,
новые тесты и долгие прогоны не входят в задание. Успешный smoke не повторять.

В README/отчёте дать CVar/INI пример, реальные разрешение/format/adapter,
положение картины, алгоритм, transfer bytes/footprint, freshness policy,
ownership/resize ограничения и выполненные проверки. Не обещать speedup от
второй карты без замеров. Обновить migration plan, сделать локальный коммит
своих правок без push, сообщить executable/hash. Остановиться на пользовательскую
оценку FPS/CPU/GPU; probes, физика, FG и итоговое сравнение остаются отдельными.
