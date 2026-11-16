# Bistro: cubemap probes как unlit-сферы внутри кафе

Контракт пользователя 2026-11-16 заменяет прежний перенос probes в PBR/SSR.
Репозиторий `D:\Project\PEPEngine2`, ordinary Meta, основа подготовки `5e836132`.
Сверить live HEAD/status; сохранить чужие изменения, в том числе текущие правки
воды. Сначала этот этап, затем отдельная приёмка и симуляция интерьера.

## Размещение и результат

Сферы показывают собственные cubemap **только над столами внутри Bistro**.
Это unlit-визуализация без BRDF, освещения/AO, теней от сфер и влияния probes на
материалы сцены. SSR/global IBL остаются в текущем режиме. Display/post общие,
без повторного освещения или tone mapping результата.

Переиспользовать BistroReflections.h: Tables содержит 11 terrace и 10 interior
anchors, текущий shuffle смешивает обе группы. Для Meta выделить interior subset
и выбирать уникальные столы случайно с фиксируемым CVar seed; записать seed/anchors
в отчёт. Count=4 по умолчанию, Meta диапазон 0..10. Spawn/reset/count/seed меняют
выбор, движение камеры — нет. Сфера целиком помещается над столешницей, не в стене
или потолке. Generated sphere mesh уже есть; сохранить App default отдельно.

Capture origin и preview связаны одним anchor. На будущем Epic столы могут летать:
сферы остаются на **исходных anchors комнаты**, не получают physics bodies и не
следуют за мебелью. Это сохраняет frozen/manual capture; смена anchors — явный reset.

## Cubemap и обновление

Переиспользовать SceneReflections, GDX12ReflectionProbePass,
ReflectionProbeComponent и Render.Reflection.* / App.ReflectionProbes.* CVars.
Старт: ShowSpheres=true, ShowBounds=false, Count=4, Resolution=128,
PrefilterSamples=64, Implementation=2. Добавить seed/manual recapture control,
если существующее API не доступно из Meta настроек. Сохранить режимы:

| Implementation | Контракт |
| --- | --- |
| 0 dynamic | Актуальная разрешённая capture-геометрия, обновление по бюджету faces. |
| 1 baked + dynamic overlay | Static color/depth cache + актуальная динамика с depth-окклюзией. |
| 2 frozen/manual | Однократный static capture; повтор по явному recapture/смене topology или ресурсов. |

Frozen не инвалидируется камерой, динамическими props или солнечным циклом.
Режим 1 сохраняет реальные зависимости статического освещения: при непрерывно
меняющемся солнце описать, что требует rebake и сколько работы это создаёт.
Не отключать дневной цикл скрыто и не обещать неизменный light cache при его анимации.
Capture filters для прозрачности/частиц сохранить как в App, не заявлять их
отражение без соответствующей реализации.

Preview-сферы исключены из всех probe captures, shadow lists и physics.
Будущие dynamic props имеют ReflectionDynamic даже во сне; в static bake нет
их неподвижных дублей. Классификация static/dynamic задаётся ownership, а не
фактом движения в текущем кадре. Смена quality/membership инвалидирует нужный cache.

SceneReflections сейчас читает geometry._shadows, отсутствующий ordinary:
передать lighting/shadow inputs явно от текущих owners. Отсутствие теней от
preview сфер не отключает тени остальных объектов в capture. Не принимать
camera-only CSM/face masks за покрытие всех cubemap faces. `_opaqueOnly=false`
целиком не включать. Static inputs собрать на старте/topology change, далее
dirty revisions; не подключать полный c.PrepareScene с обходом сцены каждый кадр.

Явные Meta dependencies: capture → нужная фильтрация → publication → preview.
Публиковать законченные faces/mips, до готовности — neutral/IBL fallback или
предыдущая готовая публикация. Сохранить Submitted/fences, resource/descriptor
retirement. Без обычных evidence/readback buffers, private submit и steady Flush.
Dirty journal используется всеми consumers, не отбирается вторым destructive consume.

## Отдельный HDR preview pass

Порядок: opaque lighting + SSR и текущая opaque вода → **unlit spheres** →
mesh transparency/particles → DoF/MB/SR → display/screen UI. Это отдельный draw
после освещения, не special material в GBuffer/deferred. SSR не включает сферы.
Шейдер выбирает опубликованный cube index, sample по reflect(view, normal),
по умолчанию mip 0. Без BRDF/Fresnel и второго IBL; стабильный entity→cube index
обязателен при изменении count/reset. Сферы не участвуют в AO/capture/shadows.

Depth test/write как у opaque: стены скрывают сферы, прозрачность правильно
располагается перед/за ними. Передать обновлённую depth raster/post consumers,
сохранив исходную opaque depth для particle collision. Motion в пикселях сферы
соответствует камере/transform, не фоновому объекту; учесть появление/reset.
Не переделывать opaque GBuffer ради late preview. Не применять aerial к сфере
повторно; dynamic cubemap publication не сбрасывает всю temporal history каждый кадр.

## Приёмка и передача

AGENTS.md/Verification Contract: Release build и один короткий обычный smoke.
Увидеть несколько сфер над внутренними столами с отражением комнаты и корректной
окклюзией; снаружи сфер нет, кадр представлен без явных ошибок. Отчёт: seed/anchors,
mode/resolution, capture content, preview order, depth/motion, publication/fences.
Без матриц трёх режимов, новых тестов, profiling и длинных прогонов;
непроверенные режимы/recapture отметить как source-reviewed.

Обновить README/план/отчёт, локальный коммит своих правок без push, hash/executable
пользователю. Остановиться на приёмку FPS/CPU/GPU. Следующий этап —
[симуляция интерьера](2026-11-16-meta-bistro-interior-physics.md), отдельно.
