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

Переиспользовать существующие параметры алгоритма Render.Clouds.Enabled/Quality
и GDX12CloudParameters. Для отсутствующих controls добавить CVar/INI:
`Meta.CloudPicture.Enabled`, `.Width`, `.Height`, `.Adapter`.
Предлагаемые стартовые значения: true, 1024, 512, -1 (актуальный primary).
Adapter >=0 — DXGI hardware adapter index по существующей enumeration проекта;
если его LUID совпадает с primary, используется local путь. Логировать requested
и effective размеры/index/name/LUID. Width/height должны быть ненулевыми и
допустимыми для устройства/формата/footprint; проверять overflow размера transfer.
Некорректный/недоступный GPU отклоняется с понятной причиной, без скрытого fallback.
При смене настроек во время работы сохранить последний валидный результат до
готовности нового. Размеры картины в мире, окно, SR и UI quality не меняют её grid.

## 1. Настоящий 2D producer

Исходники: GDX12CloudComponent.h/.cpp, Clouds.hlsl, CloudNoise.hlsli,
SceneEnvironment и завершённый world-space material path.
Текущий GDX12CloudComponent создаёт cubemap 256×256×6 и dispatch(32,32,6),
Clouds.hlsl использует один OutputSize и RWTexture2DArray. Поэтому недостаточно
подключить его SRV как Texture2D или растянуть прежнюю грань на большой quad.

Добавить узкий 2D output/recording вариант: реальный Width×Height target,
dispatch ceil(width/8)×ceil(height/8)×1 с bounds checks. Переиспользовать
CloudDirectionDensity, noise, phase/seed, coverage/softness и цвета. Задать
фиксированное направление/проекцию через UV картины с учётом aspect ratio:
движение viewer камеры не перегенерирует облака в другой проекции. Сохранить
старый cubemap API и поведение environment consumers.

Показывать texture в обычном материале/поверхности, используя готовые способы
binding из world-space этапа; не создавать новый UI renderer для облаков.
Согласовать linear HDR формат producer с материалом: не подавать RGBA16F linear
как sRGB offscreen UI без конверсии. Сохранить читаемое изображение без двойного
освещения/tone mapping; выбранный material mode и цветовой контракт описать.
Картина проходит scene depth/DoF/MB/SR как геометрия, screen UI — отдельно.

Meta.SceneEnvironment::SetClouds сейчас заменяет global sky/IBL; для картины
этот вызов не использовать. `Meta.Clouds.Enabled` остаётся false. Анимация
картины не увеличивает environment revision, не перефильтровывает IBL/probes
и не сбрасывает temporal SR каждый кадр. Время/parameters snapshot захватывается
один раз на frame tick и работает при paused физическом мире; смена GPU не
обнуляет phase. Disabled — без producer dispatch/transfer и чтения stale SRV;
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

Сверить существующие cross-adapter texture/copy helpers проекта и MGPU-IMGUI.
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
