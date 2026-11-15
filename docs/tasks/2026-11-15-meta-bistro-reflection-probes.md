# Следующий этап: локальные reflection probes Bistro

**Запланировано после реконструкции/апскейлинга.** Сейчас это задание не выполнять.
Актуальная очередь находится в
`docs/plans/2026-11-15-bistro-incremental-migration.md#текущий-порядок-по-решению-пользователя`.
При запуске задания заново сверить исходную точку: описанная ниже основа
предшествует переносу травы, частиц, анимации, Motion Blur, UI и реконструкции.

## Результат и граница этапа

Репозиторий: `D:\Project\PEPEngine2`. Перенести из App локальные cubemap probes
в обычный запуск App.MetaRenderGraph с interior/exterior. Отражения должны
использоваться в opaque PBR и mesh transparency; вне probe volume и до готовности
capture сохраняется static IBL. После реализации остановиться на приёмке
пользователем картинки, FPS и нагрузки CPU/GPU.

Основа проверена на `75db74ad`: opaque + sun/local shadows + AO + WBOIT.
Сначала прочитать AGENTS.md, отчёт
`docs/reports/2026-11-15-meta-bistro-transparency-review.md` и актуальный код.
Сохранить посторонние изменения, особенно SceneManagerModule.cpp,
BistroAdditionsTests.cpp и Temp/; их наличие перепроверить перед работой.

Это один этап отражений cubemap. SSR, вода, частицы, grass, clouds, анимация,
физика, refraction, post effects, reconstruction и FG остаются выключены.
Native resolution, сцена, материалы, камера, paused simulation, настройки AO
и теней сохраняются. `_opaqueOnly` сейчас обозначает поэтапный Bistro-путь:
переключать его целиком на полный диагностический pipeline нельзя.

## 1. Размещение и настройки

Переиспользовать размещение/параметры App из `Apps/App/src/BistroReflections.h`.
Обычный MetaApplication загружает OBJ и сейчас не создаёт эти probe entities;
одного включения Render.Reflection.Enabled недостаточно. Подключить минимально
нужную часть создания probes, без остальных Bistro additions и debug spheres/boxes.
Не добавлять демонстрационные меши и не менять roughness/opacity для эффекта.

Текущий App по умолчанию использует четыре probes и Implementation=2
(frozen manual capture). Сохранить этот режим для текущей статической сцены,
а также актуальные App placement/seed, resolution, filtering и quality.
Сверить их в коде и явно записать выбранные значения в отчёт/Meta Startup.ini,
включая `App.ReflectionProbes.ShowBounds=false` и `ShowSpheres=false`.
Не заменять frozen режим непрерывным full-scene capture. Движение камеры меняет
направление выборки отражения, но не требует повторного захвата статической сцены.

Использовать существующие Render.Reflection.* и App.ReflectionProbes.* CVars;
сохранить их валидацию и документировать поддержанные в обычном Meta пути режимы.
Не обещать поддержку динамических объектов, которые ещё не перенесены.
Отключение probes должно возвращать прежний static IBL без capture/filter работы
и без чтения неинициализированных дескрипторов.

## 2. Адаптация capture и publication

Исходники для повторного использования:

- `Apps/App.MetaRenderGraph/src/Techniques/SceneReflections.cpp/.h`;
- `Engine/Engine.RendererDX12/src/GDX12ReflectionProbePass.cpp` и соответствующий header;
- `Engine/Engine.RendererDX12/Shaders/ReflectionProbes.hlsli`;
- `Apps/App.Base/src/RenderModuleSnapshot.cpp` — семантика исходных probe/scene данных.

Сохранить существующие cubemap capture, prefilter, box projection, volume blending,
roughness mip selection и publication. Для frozen режима захватывать статическую
геометрию с теми же opaque/masked правилами, что App. Получение стеклом отражений
не требует добавления прозрачных мешей в capture сверх поведения App.

Устранить конкретные зависимости старого пути:

- `SceneReflections::Append` прямо читает `geometry._shadows`, отсутствующий в
  обычном Bistro. Подать необходимые shadow/lighting inputs явно из текущих
  владельцев. Не включать весь SceneShadows/SceneWaterParticles ради probes.
- Capture видит другую область, чем экранная камера. Тени для capture должны
  покрывать получателей в probe faces, включая объекты за экраном. Нельзя
  использовать camera-selected face mask как доказательство готовности всех
  локальных теней для capture или подменять probe sun projection экранными CSM.
  Согласовать capture light metadata с реально готовыми slice/maps; сохранить
  screen shadow cache и обновление его входов по изменениям.
- Static SkyBox должен работать без включения физической атмосферы. Согласовать
  реальные bindings capture shader с доступными environment inputs.
- Существующий Append безусловно добавляет `published probe metadata/mip readback`.
  В обычном запуске не планировать evidence/readback и не создавать его буферы.
  Сохранить отдельный явно включаемый диагностический путь, если он нужен старым
  проверкам. Metadata upload выполнять при изменении содержимого для конкретного
  кадрового слота, а не просто каждый кадр.

Записывать команды через Meta accesses/Executor; зависимости capture → prefilter
→ publication → sampling должны быть явными. Публиковать только законченный
набор faces/mips; до этого использовать предыдущую готовую публикацию или IBL.
Проверить Submitted/fence callback, lifetime descriptors/resources и замену
allocation с учётом кадров в полёте. Штатная запись кадра не должна добавлять Flush.
Разовые upload/copy для инициализации и публикации допустимы, неизменные
геометрические данные остаются в существующих DEFAULT-буферах.

## 3. CPU и кэш статической сцены

Не подключать `c.PrepareScene()` целиком: он вызывает CollectReflectionScene,
который обходит статические meshes, пересчитывает hashes и shadow lists.
Собрать необходимые capture inputs при инициализации и обновлять по реальным
изменениям геометрии, материала, transform, light, environment или probe settings
в соответствии с семантикой выбранного режима. Frozen/manual semantics App
сохранить; не превращать движение камеры в static-scene invalidation.

Переиспользовать существующие revisions/журнал изменений. У журнала уже есть
потребитель SceneLocalShadows: второй ConsumeChangedElements не должен отбирать
у него события. При необходимости раздать один набор изменений владельцам
на существующей границе подготовки кадра; не вводить новую ECS/framework ради
этого этапа. Полный сбор допустим при старте/реальной смене topology, не при
обычном движении камеры. Выборка малого списка probes не равна обходу мешей.

## 4. Подключение к PBR

В `SceneLighting` подключить Published/Radiance и probe metadata в deferred
lighting с соответствующими Meta accesses. Текущий DEFERRED_OPAQUE_ONLY отключает
LOCAL_REFLECTION_PROBES: отделить эту возможность от остальных запрещённых техник.

В `SceneTransparency` подключить radiance/metadata (shader slots t9/t10).
Текущий TRANSPARENT_STATIC_IBL_ONLY одновременно выключает probes и atmosphere:
просто убрать этот флаг недостаточно, иначе появятся несвязанные sky reads.
Разделить необходимые shader capabilities минимальным изменением, сохранив
default App behavior и static sky. Оба потребителя должны видеть согласованные
metadata и cubemap publication. BRDF применяется один раз в существующем PBR;
AO по-прежнему не умножает отражения/стекло.

Ожидаемый кадр: необходимые capture/prefilter/publication работы → opaque
lighting с probes → mesh WBOIT с probes → resolve → display/PRESENT.
После готовности frozen probes обычное движение камеры требует sampling,
а не повторного capture/prefilter/readback.

## 5. Проверка и сдача

Применить Verification Contract из
`docs/plans/2026-11-13-meta-render-graph/00-meta-plan.md#verification-contract`
и актуальный AGENTS.md. Разрешены сборка Release и один короткий обычный запуск
без аргумента `--smoke`: дождаться готовой публикации, увидеть локальное отражение
на существующем материале/стекле и проверить отсутствие явных ошибок.
Уже успешный обычный smoke повторять не требуется. Профилирование, матрицы,
искусственные сцены, длительные прогоны и расширение тестов сюда не входят.

В отчёте указать фактические count/settings и место в Bistro для осмотра,
успешные capture/publication и обычную работу после них, изменения операций
кадра, способ обновления CPU inputs и сохранения shadow cache. Различать
прочитанный код, реально наблюдавшийся результат и непроверенные режимы.
Не выдавать startup capture cost за steady-state FPS и не заявлять ускорение
без замеров. Обновить README, подготовить локальный коммит только своих правок,
сообщить хеш, exe и результаты проверок; push не выполнять.

После сдачи остановиться. SSR остаётся отложенным до отдельного решения
пользователя; в этом задании его не включать.
