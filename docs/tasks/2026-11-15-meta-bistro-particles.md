# Следующий этап: частицы Bistro

## Цель и основа

В `D:\Project\PEPEngine2` перенести существующие частицы App в обычный
App.MetaRenderGraph, на одном primary GPU. После сдачи остановиться на
пользовательскую приёмку картинки, FPS и нагрузки CPU/GPU.

Сначала прочитать AGENTS.md,
`docs/plans/2026-11-15-bistro-incremental-migration.md` и
`docs/reports/2026-11-15-meta-bistro-grass-wind-review.md`. Проверенная основа —
`879afae6`: opaque, sun/local shadows, AO, SSR без probes, WBOIT, grass/wind.
Отсечение травы без учёта изгиба — явное пожелание пользователя; не менять его
в ходе переноса частиц. Сохранить чужие правки: при подготовке задания изменены
SceneManagerModule.cpp, BistroAdditionsTests.cpp, имеется Temp/.

Вода/SPH, анимация/skinning, физика, облака/атмосфера, probes, Motion Blur,
UI, реконструкция, FG и межкарточный перенос остаются за рамками. Текущая
сцена, материалы, камера, native resolution, настройки соседних техник и
grass/shadow caches сохраняются. `_opaqueOnly` целиком не переключать.

## 1. Источники частиц и время

Переиспользовать `BistroParticles::Create/Update` из `Apps/App/src/BistroScene.h`,
ParticleEmitterComponent, существующий atlas и mesh-геометрию частиц.
Подключить только particle additions, а не все BistroScene additions.
Сохранить исходные эффекты и их параметры: Entrance/Blue fountains, TerraceEmbers,
PlanterFireflies, StreetFirebursts, InteriorDust, TableSteam, SparkRain. Названия
fountain/rain не означают необходимость включать fluid surface или SPH.

Сохранить placement, seeds, lifetime, emission/burst, shapes, volumes, blend modes,
mesh Firefly/WaterDrop, atlas animation, soft particles и authored collision box.
Использовать существующие App.Particles.* и Render.Particles.* controls; записать
фактические бюджеты и quality в Meta Startup.ini/отчёте. Не уменьшать число частиц
или качество соседних техник для получения удобного FPS. Текущие code defaults
App.Particles.Count=-1 и CountScale=2 дают общий запрашиваемый бюджет 500000;
перед реализацией сверить App overrides и документировать итоговый выбор.

Paused world требует двух явных адаптаций: BistroParticles::Update сейчас
не вызывает Emit при world.IsPaused(), а RenderModule::CollectParticleEmitters
публикует emitter.Paused с учётом паузы мира. Дать только Meta particles обновлять
emission и GPU simulation при paused остальной сцене. Сохранить отдельную паузу
emitter, time scale, substeps, ResetRevision и BurstTotal; default App semantics
не менять. Не обходить паузу вслепую после смешивания её с emitter.IsPaused().
Сначала обновить controls/bursts и собрать emitter snapshot, затем BeginFrame
симуляции. Burst потребляется один раз, не повторно на каждый слот/view/group.

## 2. GPU simulation и зависимости

Переиспользовать `Engine/Engine.RendererDX12/src/GDX12ParticleSimulation.cpp`,
GDX12ParticlesPass и particle-specific recording из
`Apps/App.MetaRenderGraph/src/Techniques/SceneWaterParticles.cpp`.
Сделать минимальное подключение отдельного particle пути; полный SceneWaterParticles
создаёт воду, far/near composition и обращается к отсутствующему geometry._shadows.
Он не должен становиться владельцем обычного кадра ради частиц.

Simulation pools остаются persistent GPU resources; обновления CPU ограничены
emitter settings/transforms и действительно изменившимися данными. Разделение
групп по DepthCollision сохранить. Colliding group читает актуальную opaque depth
после mesh и grass prepass: ни glass depth, ни fluid merged depth здесь нет.
Сохранить reverse-Z collision, camera data и существующие collision boxes.
Отражённые в SSR частицы в этом этапе не требуются: SSR сохраняет opaque inputs.

Fireflies получают существующий grass interaction field текущего кадра.
Объявить producer → consumer зависимость и lifetime; не добавлять персонажей
или искусственные interaction sources. При отсутствии источников поле нейтрально.

Первый перенос — local-only на primary GPU, Render.Particles.AsyncCompute=false,
secondary adapter/offload выключены. Использовать Meta native operations/accesses,
правильные states UPLOAD/DEFAULT и fences между simulation/culling/draw. Существующие
render helpers допустимы через корректный borrowed command list/state tracker;
они не должны запускать отдельные submit/Flush или legacy render graph Execute.
Не копировать диагностическую схему временной подмены глобального simulation owner
в worker callbacks без её owner-thread/lifetime гарантий.

## 3. Видимость, смешивание и motion

Без воды остаётся одна mesh transparency All и один WBOIT resolve:

1. Particle simulation и GPU preparation/culling/sorting готовы к draw.
2. Существующая mesh WBOIT очищает accumulation/revealage один раз и рисует стекло.
3. WeightedAlpha particles дописывают те же targets через DrawWeightedAndMotion.
4. Один WBOIT resolve смешивает общий результат с HDR после SSR.
5. Существующий Composite добавляет SortedAlpha/additive к HDR, затем display/PRESENT.

Переиспользовать GDX12ParticlesPass::PrepareRenderGroup, DrawWeightedAndMotion,
Composite. Не очищать WBOIT на каждый emitter/group и не заменять все blends одним.
SortedAlpha сортировать глобально по particle draw input, а не независимо внутри
групп A/B; finalization/sort выполнить после сбора всех нужных групп. Empty groups
и отсутствие emitters не должны оставлять старые counts/arguments/colour/motion.

Сохранить depth test без записи opaque depth, soft distance, GPU indirect draw,
size culling и mesh particle path. Не вводить CPU sorting или particle readback.
Одноразовая загрузка Firefly geometry допустима; повторные неизменные uploads
не нужны. MotionOutput и предыдущие позиции поддерживать по существующему контракту
для будущих Motion Blur/reconstruction, не включая эти техники сейчас. Reset/cut,
resize и выключение должны исключать старый motion и висячие resource pointers.

Render.Particles.Enabled, DrawEnabled и SimulationEnabled сохраняют раздельную
семантику: выключенная отрисовка не замораживает симуляцию, выключенная симуляция
не скрывает уже существующие частицы. Проверить по коду существующий контракт
reset/pause и zero-count; не расширять для этого runtime-матрицу.

## 4. Приёмка

Release-сборка и один короткий обычный запуск без `--smoke` по AGENTS.md и
Verification Contract. Видны существующие эффекты (например, entrance fountain
и fireflies у травы), частицы движутся при paused world, opaque geometry их
перекрывает, grass и glass продолжают работать, в текущем логе нет явных ошибок.
Уже успешный smoke повторять не требуется. GPU capture, профилирование, длительные
прогоны, новые тесты и матрицы сюда не входят.

Обновить README и краткий отчёт: count/settings, источник времени, composition
order, depth/interaction inputs, добавленные operations, результаты проверок и
непроверенные режимы. Не заявлять speedup без замеров. Создать локальный коммит
только своих изменений, без push; сообщить hash и executable.
После приёмки частиц следующий этап — анимация и GPU skinning. Сейчас его не начинать.
