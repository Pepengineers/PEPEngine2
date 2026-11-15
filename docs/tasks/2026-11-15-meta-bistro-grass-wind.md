# Следующий этап: трава и ветер Bistro

**Реализовано и проверено** с учётом последующих оптимизаций до `879afae6`.
Уточнение пользователя о намеренном отсечении без учёта изгиба и границы проверки:
`docs/reports/2026-11-15-meta-bistro-grass-wind-review.md`.
Следующее задание — `docs/tasks/2026-11-15-meta-bistro-particles.md`.

## Результат

В `D:\Project\PEPEngine2` перенести из App траву и её ветер в обычный
App.MetaRenderGraph. Пользователь должен увидеть существующее размещение травы
Bistro, движение от ветра, корректные depth/GBuffer/motion vectors и тени.
После сдачи остановиться для пользовательской проверки картинки, FPS и CPU/GPU.

Прочитать AGENTS.md, актуальный
`docs/plans/2026-11-15-bistro-incremental-migration.md` и
`docs/reports/2026-11-15-meta-bistro-ssr-review.md`. Проверенная основа — `5c29050c`:
opaque, sun/local shadows, AO, SSR без probes, mesh WBOIT, static SkyBox/IBL.
Сохранить посторонние staged/unstaged изменения; при подготовке задания они есть
в SceneManagerModule.cpp, BistroAdditionsTests.cpp и Temp/.

## 1. Создание сцены и время

Переиспользовать `Apps/App/src/BistroGrass.h` и существующие GrassRenderComponent,
WindFieldComponent, BistroGrassGeometry, GDX12GrassRenderer/GDX12GrassWind.
Обычный MetaApplication загружает interior/exterior, но пока не создаёт grass/wind
entities. Подключить только соответствующую часть Bistro additions. Сохранить
размещение, seed, материал, количество patches, плотность, бюджет blades, LOD и
форму из App; не добавлять демонстрационную траву или остальные additions.

Использовать существующие `App.Grass.*`, `Render.Grass.*`, `Render.Wind.*` CVars.
Зафиксировать фактические startup значения и место осмотра в отчёте. Начальный
режим — существующий Analytic wind и текущая геометрия App; сохранить доступные
Ribbon/CrossedRibbons и Analytic/ProjectedGrid реализации без переписывания
алгоритмов. В рамках приёмки достаточно одного обычного режима, без матрицы.

Критическая зависимость: `BistroGrassGeometry::CollectScene` обнуляет общий dt и
wind DeltaSeconds при `world.IsPaused()`. Простое создание SceneGrass оставит
ветер замороженным. Дать grass/wind обновляться в обычном Meta-пути, сохранив
остальную симуляцию paused. Сделать это через минимальный явный путь времени,
сохранив time scale, ограничение шага/substeps и reset семантику ветра. Не менять
глобальную семантику паузы App и не запускать физику/анимацию ради dt.

Персонажи и их контактная деформация будут отдельным этапом; частицы — следующим.
Существующий interaction field должен иметь валидное нейтральное состояние при
отсутствии источников. Не создавать искусственные interaction sources.

## 2. Native Meta-проходы

Адаптировать существующий `Apps/App.MetaRenderGraph/src/Techniques/SceneGrass.cpp`,
подключение в SceneGeometry и PrepareFrame. `_opaqueOnly` сейчас означает обычный
поэтапный путь: целиком его не переключать. `PrepareGrass` не должен приводить к
включению `c.PrepareScene()`/CollectReflectionScene или остальных техник.

Существующий SceneGrass::Append при `viewsOnly=false` добавляет шесть
диагностических probe GBuffer, field/version readback и probe readback. В обычном
пути эти allocations, clears, draws и copies не нужны. Отделить диагностические
операции явным признаком, сохранив обычные wind/interaction/expansion/draw работы.
Параметр viewsOnly сам по себе не замена: он также пропускает подготовительные
работы травы. Не включать отражательные probes и не выполнять legacy Execute.

Подать реальные depth/GBuffer targets обычного кадра. Трава участвует в depth
prepass и записывает все нужные GBuffer attachments, включая velocity, до AO,
lighting и SSR. Сохранить alpha-test, normals, roughness, material shading,
reverse-Z и read-only depth GBuffer. Использовать существующие GrassExpand.hlsl
и GrassRender.hlsl: previous deformation/world/camera должны соответствовать
предыдущему представленному кадру, а не последней записи shadow view или старому
содержимому того же frame slot. Depth и GBuffer используют одно состояние ветра
и одинаковую геометрию в пределах кадра.

Начальный путь — один primary GPU, `Render.Grass.AsyncCompute=false`, native
Meta recording с явными resource accesses и зависимостями. Отдельный перенос
на COMPUTE/другой GPU в этот этап не включать. Roots и неизменная геометрия
остаются в DEFAULT после первоначальной загрузки; повторная загрузка нужна
только при изменении соответствующих данных. Ветер, expansion и indirect
arguments вычисляются на GPU, без CPU readback видимости и генерации blades.

## 3. Интеграция с тенями и кэшами

Текущие SceneSunShadows и SceneLocalShadows обычного пути рисуют mesh indirect
commands. Старый SceneGeometry берёт grass shadow targets из `_shadows`,
которого в обычном пути нет. Поэтому включения SceneGrass недостаточно для теней.
Подключить grass shadow draws к текущим владельцам sun/local shadows, используя
существующий GeometryTarget/grass recording, без запуска старого SceneShadows.

Сохранить `App.Grass.CastShadows` и правила App. Grass bounds, включая высоту и
максимальное отклонение, должны участвовать в отборе shadow receivers/casters
и bounds солнца. Трава должна получать тени даже там, где за ней нет mesh receiver.
Grass outside camera frustum может отбрасывать тень в видимую область: shadow
view не должен ограничиваться camera-visible blade list.

`GDX12GrassRenderer::PublishPrimitives` уже публикует bounds, CastsShadows,
Geometry/Material/Transform/DeformationRevision. Переиспользовать эти данные или
минимальный прямой seam; сама публикация в registry ещё не означает, что текущий
local cache их читает. При изменении деформации обновлять только затронутые
local faces. Их static caster lists сохранять; при восстановлении карты можно
повторно нарисовать сохранённый static list вместе с актуальной травой.
Учитывать старые и новые bounds при перемещении, удалении и отключении patch.
Повторное рисование поверх старой глубины без её восстановления оставляет
«призрачные» тени. Публиковать готовность face после всех необходимых draw.

Изменения травы не должны инвалидировать всю статическую сцену, пересобирать её
BVH/командные списки или заставлять SunShadows каждый кадр сканировать все меши.
Переиспользовать static bounds и отдельно учесть малый набор grass bounds.
Обход небольшого списка patches допустим; обход всей Bistro при движении камеры
не нужен. Существующий mesh change journal и его потребителей сохранить.

## 4. Проверка и сдача

Сохранить SSR, AO, прозрачность, исходные meshes/materials, настройки теней,
native resolution и свободную камеру. Остальные этапы остаются выключены.
Не менять качество соседних техник ради FPS. Не добавлять расширенных тестов.

По AGENTS.md и Verification Contract достаточно Release-сборки и одного короткого
обычного запуска без `--smoke`: видна трава и движение ветра, depth/освещение/тени
не имеют явного сбоя, кадры продолжают выводиться и в текущем логе нет явных ошибок.
Показать место для осмотра и записать добавленные ordinary operations. Уже
успешный smoke повторять не требуется; профилирование, captures, длительные
прогоны и матрицы требуют отдельного запроса.

Обновить README и краткий отчёт: выбранные параметры, источник времени,
изменения shadow cache, выполненные проверки и оставшиеся ограничения.
Отделить source review от реально наблюдавшегося результата; не заявлять
performance-эффект без измерений. Локальный коммит только своих правок, без push;
сообщить хеш и путь exe. После приёмки этого этапа следующий — частицы.
Частицы, анимацию и последующие этапы сейчас не начинать.
