# Bistro: динамическое небо и атмосфера

Следующий этап ordinary `App.MetaRenderGraph` по решению пользователя 2026-11-16.
Репозиторий `D:\Project\PEPEngine2`, основа задания `83755c8c`.
Сверить текущие HEAD/status и сохранить чужие изменения. Реализуется только этот
этап; затем пользователь проверяет нагрузку/FPS. Порядок дальнейших этапов и
контракт облаков-картины: [план переноса](../plans/2026-11-15-bistro-incremental-migration.md).

## 1. Выбор неба и источник параметров

Переиспользовать `SkyAtmosphereComponent`, `World::SetSkySource`,
`RenderModule::SubmitSky`, `GDX12Sky.*`,
`GDX12EnvironmentIBLPass.*`, `Meta::SceneEnvironment` и SkyAtmosphere.hlsl.
В Bistro сейчас SceneManagerModule создаёт Sun со SkyBoxComponent и выбирает
SkyBox; включение Render.SkyAtmosphere.Enabled само по себе не выбирает компонент.
Добавить явный выбор atmosphere для ordinary Meta через настройки/инициализацию
сцены, сохранив существующий App default и возможность вернуться к SkyBox.

Использовать существующие CVar/INI quality и параметры компонента. Небо обновляется
по актуальной камере, солнцу и ревизиям настроек; отдельный автоматический цикл
день/ночь в задачу не входит. Солнце в атмосфере, direct lighting и sun shadows
получает согласованное направление/интенсивность из существующего источника;
не создавать второй directional light. Сохранить единицы cm/km и lux/luminance scale.
Физический мир остаётся paused, настройки неба при этом применяются.

Готово: ordinary использует выбранную атмосферу, а переключение/выключение имеют
явный корректный результат без скрытого запуска остальных diagnostic техник.

## 2. LUT, IBL и потребители

Адаптировать существующий SceneEnvironment::Freeze/Build/Append, не писать второй
solver атмосферы. Статические LUT/IBL обновлять по их зависимостям, view-dependent
SkyView/aerial — при нужном изменении камеры/состояния. Движение камеры не должно
пересобирать статическую сцену, загружать статические буферы или без причины
пересчитывать все IBL mip/filter stages. Камерные ресурсы и history следуют SR grid.
Сохранить persistent outputs, resource states, owner recording и fence retirement.
Первый перенос использует primary GPU и существующие Meta очереди/Executor.

Проверить реальные потребители: SceneLighting/deferred, SSSR fallback,
SceneTransparency, SceneWater и композиция частиц. Ordinary варианты
DEFERRED_OPAQUE_ONLY, SSSR_STATIC_IBL_ONLY, TRANSPARENT_STATIC_IBL_ONLY и
FLUID_STATIC_IBL имеют разные ограничения: разделить atmosphere/probes capabilities
там, где это необходимо, сохранив probes выключенными. Не отключать `_opaqueOnly`
целиком. Текущая вода после `9da1611c` рисуется opaque particles; сохранить её
фактический выбранный режим и temporal fix `83755c8c`, не возвращать старую
композицию по устаревшему отчёту.

Видимое небо, IBL и specular fallback SSR должны использовать одно состояние
environment. Aerial perspective применяется в правильном месте композиции ровно
один раз: сверить ordinary с App `BistroRenderPipeline` и WBOITComposition,
включая стекло/частицы и актуальный water mode. Не туманить screen UI.
Явно объявить reads опубликованных LUT/IBL для каждого потребителя; нейтральные
bindings/bypass нужны при отключении. Новая environment publication уведомляет
temporal consumers существующими revisions, без постоянного reset истории.

Готово: атмосфера видна в фоне и освещении/композиции; ресурсы готовы до чтения,
тени/AO/SSR/вода/трава/частицы/анимация/MB/SR/UI сохраняются.

## 3. Граница с последующими этапами

DoF, world-space canvas и облака-картина реализуются отдельными этапами после
приёмки этого. `Meta.Clouds.Enabled` остаётся выключенным: текущий SetClouds
подменяет источник environment, а пользователь запросил облака на стене.
Reflection probes, FG и физическую симуляцию сейчас не добавлять.

## Проверка и передача

Следовать AGENTS.md и Verification Contract: Release build и один короткий обычный
запуск без диагностических аргументов; увидеть небо, освещённый Bistro и работающий
кадр без явных ошибок. Уже успешный запуск повторять не нужно. Зафиксировать
effective source/quality, SR grid, место наблюдения и выполненные GPU stages.
Без матриц режимов, новых тестов, профилирования и длительных прогонов.
Непроверенные runtime переключения отметить отдельно от source review.

Обновить README, план и отчёт с источниками параметров, invalidation/dependencies,
местом aerial composition и ограничениями. Создать локальный коммит своих правок,
сообщить hash/executable и остановиться на пользовательскую приёмку. Не запускать
DoF или сравнение App/Meta автоматически.
