# Следующий этап: FidelityFX SSSR без reflection probes

**Отложено по решению пользователя.** Сейчас это задание не выполнять.
Актуальная очередь находится в
`docs/plans/2026-11-15-bistro-incremental-migration.md#текущий-порядок-по-решению-пользователя`.
Имеющиеся рабочие изменения SSR не откатывать из-за изменения очереди.

## Цель

В `D:\Project\PEPEngine2` перенести существующий FidelityFX SSSR + denoiser
из App в обычный Bistro-путь App.MetaRenderGraph. Reflection probes пользователь
решил пока пропустить. Для промахов экранной трассировки использовать существующий
static IBL; SceneReflections и probe entities не включать.

До изменений прочитать AGENTS.md и
`docs/reports/2026-11-15-meta-bistro-transparency-review.md`, проверить текущий HEAD
и посторонние правки. Основа: opaque/depth prepass, sun/local shadows с incremental
cache, AO, mesh WBOIT. Сохранить interior/exterior, материалы, настройки теней/AO,
native resolution, paused simulation и свободную камеру. Не включать воду,
частицы, grass, clouds, физику, анимацию, refraction, другие post effects,
reconstruction и FG. `_opaqueOnly` целиком не переключать.

## Реализация

1. Переиспользовать `GDX12SSSRPass` и существующую последовательность в
   `Apps/App.MetaRenderGraph/src/Techniques/SceneLighting.cpp`: prepare → SDK SSSR
   с denoiser → specular resolve. Реализация SDK и математика находятся в
   `Engine/Engine.RendererDX12/src/GDX12SSSRPass.cpp` и
   `Engine/Engine.RendererDX12/Shaders/SSSR.hlsl`. Новый SSR алгоритм не нужен.

2. Подключить эту последовательность **до прозрачности**:
   opaque lighting → SSSR/resolve → существующий SceneTransparency → WBOIT resolve
   → display/PRESENT. Вход SSR — opaque HDR, opaque depth, GBuffer normals/roughness,
   F0/baked flag, motion vectors, prefiltered environment и BRDF LUT. Glass
   продолжает получать static IBL через forward PBR. Отражения на стекле и захват
   прозрачных поверхностей в SSR в этот этап не входят: у них нет соответствующих
   opaque GBuffer/depth данных. Это ограничение явно указать в отчёте.

3. Отвязать SSR от SceneReflections. Сейчас SceneLighting безусловно читает
   `_reflections->History/Radiance/Published`, а SSSR.hlsl включает
   ReflectionProbes.hlsli и вызывает SampleReflectionRadiance. Добавить минимальный
   shader-вариант со static IBL fallback без чтения probe SRV t9/t10; убрать эти
   accesses/bindings из обычного пути. Default App-вариант с probes сохранить.
   Не создавать фиктивный capture pipeline или загружать metadata ради нулевых probes.

4. Сохранить composition: SDK уже смешивает отражение с environment fallback;
   resolve заменяет исходный specular IBL через разность radiance − fallback,
   применяя split-sum BRDF один раз. Fallback должен совпадать с IBL исходного
   deferred lighting по roughness/mip и интенсивности. Не добавлять specular
   второй раз, не использовать denoiser alpha как произвольную маску смешивания.
   Baked/nonreflective/sky pixels сохраняют исходную семантику.

5. Подать реальные motion vectors при движении камеры, даже при paused world.
   Сохранить reverse-Z, normal unpack, squared roughness, преобразование motion
   NDC → UV и jitter delta существующей реализации. Проверить, что обычный
   GBuffer заполняет используемый motion target и предыдущие camera transforms.

6. History принадлежит последовательности представляемых кадров. Ротация frame
   slots и смена адресов GBuffer не должны каждый кадр пересоздавать SDK context.
   Вместо probe History использовать существующую подходящую history revision
   обычного пути. Сохранить reset при camera cut, resize, изменении нужных настроек,
   environment и выключении/повторном включении SSR. Обеспечить fences/lifetime
   при замене контекста, сериализацию доступа к persistent SDK history и
   существующий OwnerRecording для первого dispatch. Не добавлять покадровый Flush;
   проверить queued-frame contract SDK для текущих трёх кадров в полёте.

7. Объявить Meta accesses и зависимости prepare/SDK/resolve, согласовать входные
   и выходные states с SDK. В обычном Bistro camera constants находятся в UPLOAD:
   для них сохранить GENERIC_READ, не копировать слепо состояние из диагностического
   пути. Записывать native команды через Executor, без legacy Execute.
   Не подключать полный PrepareScene/CollectReflectionScene, scene uploads или
   readback из диагностического хвоста SceneLighting. Shadow cache и sparse
   обновления статических данных сохраняются.

8. Использовать существующие Render.SSR.* CVars. В Meta Startup.ini включить SSR
   с текущим App preset Quality=2 и остальными существующими defaults/overrides;
   зафиксировать фактические настройки в отчёте. При Enabled=false передавать
   прежний opaque HDR напрямую в прозрачность, без SSR dispatch и лишней
   полноэкранной copy-through. Сохранить корректный history reset для re-enable.

## Приёмка и остановка

Следовать AGENTS.md и Verification Contract из
`docs/plans/2026-11-13-meta-render-graph/00-meta-plan.md#verification-contract`.
Достаточны Release-сборка и один короткий обычный запуск без `--smoke`:
есть GPU-работа SSSR/denoiser, видимое отражение на существующей отражающей opaque
поверхности, движение камеры не даёт явного сбоя, WBOIT продолжает работать,
явных ошибок в текущем логе нет. Уже успешный smoke повторять не требуется.
Тестовые матрицы, искусственные сцены, новые тесты, capture и профилирование
в это задание не входят.

Обновить README и создать краткий отчёт: что подключено, настройки, место для
осмотра, операции кадра, какие проверки выполнены и что осталось непроверенным.
Указать отсутствие probes и ограничение отражений данными экрана; не обещать
отражения объектов за камерой. Локальный коммит только своих изменений, без push.
После сдачи остановиться: пользователь проверяет картинку/FPS/нагрузку.
Следующую технику и отложенные probes автоматически не начинать.
