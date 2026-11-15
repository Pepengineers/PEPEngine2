# Задание агенту: прозрачные меши Bistro в MetaRenderGraph

## Цель

Перенести обычные прозрачные материалы исходных интерьера и экстерьера Bistro из App в обычный запуск App.MetaRenderGraph. Добавить только отрисовку прозрачных мешей и композицию с существующим HDR. После короткой проверки и локального коммита остановиться: пользователь проверяет изображение, FPS и CPU/GPU.

## Исходная база

Репозиторий `D:/Project/PEPEngine2`, ветка на момент передачи `codex/bistro-rendering`. Последний кодовый коммит `03361486` включает AO. Предыдущие важные изменения: `820d2b13` — кэш локальных теней, `23c07850` — инкрементальные входы сцены и загрузки диапазонов.

Прочитать `AGENTS.md`, `Apps/App.MetaRenderGraph/README.md`, отчёты `docs/reports/2026-11-15-meta-bistro-ao-review.md`, `2026-11-15-meta-local-shadow-cache.md` и `2026-11-15-meta-shadow-incremental-inputs.md` из той же папки. Перепроверить актуальный status/branch/HEAD.

На момент передачи есть посторонние изменения `Apps/App.Base/src/SceneManagerModule.cpp`, `Apps/App/tests/BistroAdditionsTests.cpp` и неотслеживаемый `Temp/`. Сохранить их; не включать в свой коммит. Текущие типы и размещение источников света не заменять историческими вариантами.

## Способ переноса

1. Использовать существующий weighted blended OIT: `GDX12WBOITTransparencyPass`, `GDX12WBOITCompositionPass`, `TransparentPass.hlsl` и `CompositionPass.hlsl`. Сохранить alpha = texture alpha × material opacity, вес, blending, PBR, depth test и отсутствие записи прозрачности в opaque depth. Alpha-tested вырезы остаются в существующем opaque-пути.
2. Поскольку воды ещё нет, использовать один проход `DepthPartition::All` и одну композицию. Не подключать разделение far/near вокруг воды, particles sort и water resolve. Не добавлять сортировку прозрачных мешей для WBOIT или альтернативную OIT-технологию.
3. Переиспользовать существующий GPU-culling и поток прозрачных indirect-команд/счётчик из SceneGeometry (`Buffers[6]` и `[7]`). Использовать правильный режим unbatched, как в существующем Meta-коде: command InstanceID не является индексом opaque batch remap. Не строить покадровые CPU-списки всех прозрачных объектов и не читать draw count обратно с GPU.
4. Цепочка: текущие Opaque + тени + AO → deferred HDR → прозрачность с read-only opaque depth → WBOIT resolve → существующий HDR display transform/PRESENT. Accumulation очищается в 0, revealage в 1; при нуле видимых прозрачных draws результат должен оставаться исходным HDR без данных прошлого кадра. Использовать существующий механизм пропуска при необходимости, не вводить CPU readback.
5. Освещение прозрачности использует существующие солнце, локальные источники, их карты теней и статический IBL. Локальные данные света должны быть теми же опубликованными данными, что использует deferred lighting: стабильные ShadowFirstSlice и per-face mask из кэша. Нельзя брать исходный список LocalLights с другими номерами срезов.
6. Сохранить forward-освещение прозрачности из App. Не использовать для неё без проверки light grid, ограниченный opaque depth: прозрачная поверхность может находиться перед этим depth. Не применять экранный AO непрозрачной поверхности к стеклу по тем же экранным координатам.
7. В TransparentPass.hlsl сейчас безусловно включены LOCAL_REFLECTION_PROBES. Для этапа без probes обеспечить корректный вариант shader/bindings со статическим IBL; не включать захват probes ради прозрачности и не оставлять чтения непривязанных SRV. Поведение обычного App по умолчанию сохранить. Для текущего статического SkyBox не добавлять atmospheric rendering или повторную aerial-композицию.
8. Не снимать `_opaqueOnly` целиком и не создавать весь SceneWaterParticles ради мешей. Выделить минимальный владелец прозрачности либо переиспользовать необходимую часть существующего кода, сохраняя Meta graph/executor и объявленные accesses/barriers. Не возвращать legacy GDX12RenderGraph::Execute и покадровый Flush.

## Обязательная интеграция с кэшем теней

Текущий `SceneLocalShadows` исключает RenderLayer=2 из BVH и проверок членства. Теперь прозрачные меши могут получать свет и тени: учесть их bounds при выборе необходимых граней. При этом не делать blended-материалы отбрасывающими непрозрачную тень — сохранить правила caster из App.

Обновить обе стороны: начальное построение и инкрементальную обработку изменений RenderLayer/видимости/материала. Сохранить обратные ссылки, обработку старых/новых bounds, стабильные срезы и правильную инвалидацию. Не возвращать полный обход статической сцены при движении камеры. Проверить, что солнечный caster-culling продолжает исключать прозрачные материалы согласно App.

## Границы этапа

Сохранить исходные материалы/текстуры и их классификацию; не менять opacity ради заметности результата и не добавлять искусственные стеклянные объекты. Использовать реальные прозрачные поверхности сцены и указать пользователю, где их смотреть.

Не добавлять воду, частицы, траву, облака, анимацию, физику, SSR, probes, refraction, постэффекты, реконструкцию или frame generation. Не реализовывать новые стеклянные эффекты под видом обычной прозрачности. Opaque, AO, параметры теней, native resolution и основной GPU остаются прежними. Исключение — необходимое расширение выбора shadow receivers, описанное выше.

## Навигация

- `Apps/App.Base/src/Rendering/DefaultRenderPipeline.cpp`: обычная WBOIT-цепочка без водного разделителя.
- `Apps/App/src/Rendering/BistroRenderPipeline.cpp`: production-входы прозрачности и композиции; far/near нужен этому полному пути из-за воды, не текущему этапу.
- `Apps/App.MetaRenderGraph/src/Techniques/SceneWaterParticles.cpp`: уже существующая Meta-запись clear/draw/resolve и unbatched bindings; брать нужные части, не включать весь владелец.
- `Apps/App.MetaRenderGraph/src/Techniques/SceneGeometry.cpp/.h`, `Engine/Engine.RendererDX12/Shaders/Culling.hlsl`: готовые прозрачные команды и счётчик.
- `Apps/App.MetaRenderGraph/src/Techniques/SceneLighting.cpp/.h`: deferred HDR, опубликованные локальные источники/карты теней и HDR continuation.
- `Apps/App.MetaRenderGraph/src/MetaRenderPipeline.cpp`, `Techniques/ScenePostUI.cpp` относительно той же папки: связывание нового HDR и окончательный вывод.
- `Apps/App.MetaRenderGraph/src/Techniques/SceneLocalShadows.cpp/.h`: receiver/caster-фильтры и инкрементальное обновление.
- `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12WBOITTransparencyPass.h` и `GDX12WBOITCompositionPass.h` в той же папке: PSO, blending, depth, ресурсы и композиция.
- `Engine/Engine.RendererDX12/Shaders/TransparentPass.hlsl`, `CompositionPass.hlsl`, `PBR.hlsli`, `LocalShadows.hlsli`: существующие shading и alpha-семантика.

## Проверка и передача

Release x64 и один короткий обычный smoke: сцена рисуется, настоящие прозрачные поверхности видимы и корректно перекрываются opaque depth, явных ошибок нет. Осмотреть результат при обычном движении камеры. Сверить список исполняемых проходов: добавилась только прозрачность/композиция, остальные исключённые техники отсутствуют.

Без `--smoke`, новых тестов ради покрытия, матриц режимов, длительных прогонов и профилирования без отдельного запроса. Обновить README и краткий отчёт: что подключено, где смотреть прозрачность, как решён выбор shadow receivers, какие проверки выполнены и что осталось непроверенным.

Проверить diff, создать локальный коммит только своих правок, без push. Передать хеш и `D:/Project/PEPEngine2/Build/bin/x64/Release/App.MetaRenderGraph/App.MetaRenderGraph.exe`. Не заявлять измеренную стоимость техники без замера. Остановиться до приёмки пользователя; следующую технику выбрать отдельно. Итоговое сравнение архитектур остаётся после завершения переноса.
