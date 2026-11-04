---
title: Application-Owned Render Pipelines - Plan
type: refactor
date: 2026-11-05
artifact_contract: ce-unified-plan/v1
product_contract_source: adr-and-session-decisions
origin: docs/adr/0010-renderer-and-application-feature-ownership.md
execution: code
---

# Application-Owned Render Pipelines - Plan

## Goal Capsule

- **Objective:** приложение выбирает passes и их связи; движок исполняет граф. Bistro сохраняет текущий результат и самостоятельно развивает воду, траву и порядок техник.
- **Means:** извлечь композицию из RenderModule в объект пайплайна приложения; использовать существующие GDX12RenderGraph и общие классы проходов.
- **Authority:** инструкции пользователя и AGENTS.md, затем [ADR-0010](../adr/0010-renderer-and-application-feature-ownership.md) и этот план.
- **Revision:** решение пользователя от 2026-11-05 заменяет прежний план с GDX12RenderExtension, injection points и BuildTransparency. Путь документа сохранён для существующего чата исполнения. Старые требования к этим интерфейсам больше не действуют.
- **Execution:** последовательная реализация U1–U5, подходящие проверки, локальные коммиты только своей работы. Push не запрошен. Сверить уже сделанные изменения в worktree и переиспользовать полезный перенос; не продолжать устаревшую архитектуру.
- **Stop conditions:** не затирать чужую работу и не разрешать чужие конфликты ради задачи. Изолированный результат явно отличать от интеграции в основной checkout.

## Product Contract

### Requirements

- R1. Apps/App владеет всеми специализированными компонентами, алгоритмами, GPU-ресурсами, passes и шейдерами воды, травы и обслуживающего её ветра.
- R2. Engine и App.Base не включают Bistro headers, не знают конкретные water/grass types; приложение с общим пайплайном не создаёт их ресурсов.
- R3. Частицы, атмосфера, AO, SSR, skinning, освещение, тени, постобработка и граф остаются общими возможностями.
- R4. Bistro создаёт экземпляры общих и собственных passes, выбирает их входы, выходы и зависимости. Другому приложению доступна своя композиция без правок RenderModule. Per-stage injection points и callback registry не вводятся.
- R5. Общий runtime владеет устройствами, очередями, swapchain и синхронизацией кадра; компилирует и исполняет существующий граф. Пайплайн описывает зависимости ресурсов; не расставляет собственные межочередные fence values.
- R6. Изменение положения AO или добавление нового app pass выполняется в коде пайплайна приложения. Потребители получают корректные depth/normals/color версии; одного изменения порядка AddPass недостаточно.
- R7. Сохранить траву в depth/normals/GBuffer, каскадных и локальных тенях, динамических probe captures и их тенях; сохранить bounds, revisions и static/dynamic filtering.
- R8. Сохранить water surface/resolve, прозрачность по обе стороны воды, однократную композицию частиц и aerial perspective; сохранить fallback depth/motion/HDR при off/empty.
- R9. Физика независима от отрисовки; условия симуляции собственных техник определяет Bistro. Перенос сохраняет нынешние visibility/pause/time-scale правила. Rebuild не делает лишний simulation tick.
- R10. Сохранить runtime toggles/quality, CVar names, строгую конфигурацию, resize, temporal history, frame-slot lifetime, публичные queries RenderModule и существующие режимы исполнения.
- R11. Общие geometry passes получают primitives/batches вместо конкретных компонентов и отдельных grass draw hooks. Static mesh, skeletal mesh и трава используют этот контракт; сохраняются indirect draws, instancing, per-view culling и GPU preparation dependencies.

### Scope Boundaries

В объёме: отделение сборки пайплайна от исполнения; единое render-представление static mesh, skeletal mesh и травы; перенос воды/травы, регистрация ECS, shader/build/config paths, затронутые consumers и проверки. Единые primitives/batches дополнительно согласованы пользователем при обновлении плана.

Не входят: нодовый редактор, сериализация графа, hot reload/DLL loader, расширение multi-GPU, новый async scheduler, второй render graph, новая ECS, массовая переработка всех pass APIs, новые визуальные алгоритмы или оптимизация FPS. Существующий secondary-device путь и async compute сохраняются; перенос не обещает универсальный граф нескольких GPU. Политика общей скелетной анимации, перенос светильников/меша светлячка и новые сериализаторы воды/травы отложены.

Добавление C++ pass требует пересборки приложения. Критерий R6 — отсутствие необходимых изменений в исходниках общего движка при достаточном существующем API; это не обещание отсутствия relink или runtime-редактирования.

### Acceptance Examples

- AE1. Два приложения передают разные пайплайны одному RenderModule. Общий вариант работает без воды/травы и сохраняет общие частицы/постобработку.
- AE2. Bistro меняет допустимую позицию AO и добавляет тестовый pass между выбранными производителем и потребителем только своей композицией; граф исполняет корректные зависимости.
- AE3. Вода on/off/empty сохраняет прозрачность, частицы, depth/motion и history; нет повторного particle composite.
- AE4. Трава вне main-camera frustum остаётся в нужном shadow/probe view; static-only captures по-прежнему её исключают.
- AE5. Rebuild и resize не удваивают проходы, симуляцию и подписки; GPU не использует уничтоженные pass/state objects.
- AE6. Static mesh, skeletal mesh и трава проходят через один batch-consumer путь; тестовый новый источник геометрии подключается без правок opaque/shadow/probe passes. Различные материалы, vertex layouts и direct/indirect команды различаются данными/контрактами, а не проверками типа игрового компонента.

## Planning Contract

### Current Owning Sources

- Apps/App.Base/Include/App.Base/Modules/RenderModule.h и src/RenderModule.cpp: ConfigureRenderPipeline, SetupRenderPasses, InitializeRenderPasses, RebuildRenderGraph, конкретные pass pointers, preparation, AO ordering, app-specific extraction.
- Engine/Engine.RendererDX12/Include/Engine.RendererDX12/GDX12RenderGraph.h и src/GDX12RenderGraph.cpp: AddPass, links, DependsOn, exports, Compile/Execute, barriers и текущая async execution.
- Engine/Engine.Core/Include/Engine.Core/ModuleLocator.h: поиск по точному типу. Просто зарегистрировать BistroRenderModule вместо RenderModule недостаточно для существующих GetModule<RenderModule>.
- Apps/App/src/App.cpp и Apps/App.Base/src/App.cpp: создание RenderModule. Тестовые callers перечислены в U4.
- Исследования .tmp/render-extension-planning/repo-research.md и spec-flow.md полезны для поведения воды/травы; их рекомендации injection points устарели. Актуальный код выбранного checkout имеет приоритет.

### KTD1. Общий runtime и объект пайплайна

Оставить RenderModule общим модулем и сохранить его регистрацию под существующим типом. Он получает unique_ptr<IRenderPipeline> до Initialize. Не копировать RenderModule и не строить иерархию его наследников для каждого приложения.

Новый App.Base/Rendering/IRenderPipeline.h описывает только границу lifecycle. BistroRenderPipeline располагается в Apps/App/src/Rendering/. DefaultRenderPipeline в App.Base предоставляет существующим общим consumers рабочий вариант без воды/травы. Вариант по умолчанию допустим для прежних двухаргументных вызовов RenderModule; Bistro передаёт свой пайплайн явно.

Предлагаемый минимальный контракт (точные сигнатуры согласовать с существующими renderer-типами):

| Виртуальный метод | Обязанность |
|---|---|
| Initialize(context) | Создать/настроить pass instances и принадлежащие пайплайну ресурсы через существующие Setup/Initialize |
| PrepareFrame(context) | После ожидания frame slot подготовить технику и metadata; сообщить необходимость rebuild при изменении настроек |
| BuildGraph(graph, inputs) | Добавить выбранные passes, связать ресурсы/зависимости, назначить выходы; не выполнять GPU работу |
| Resize(context) | Обновить размеры ресурсов через прежний pass lifecycle после GPU synchronization |
| Виртуальный деструктор | Освободить собственное состояние после прекращения исполнения и уничтожения графа |

BuildGraph — одна точка передачи всей композиции, а не каталог этапов. Никаких AfterOpaque/AfterLighting/BuildTransparency, приоритетов extensions или tail-provider protocol.

Context содержит нужные device resources, frame/common data и параметры размера; inputs — внешние scene/backbuffer links. Использовать IRenderPassLink и нынешние pass APIs. Не вводить вторую модель ресурсов. Pass instances принадлежат пайплайну и живут дольше графа, который их ссылает.

Pipeline outputs/bindings возвращают только реально необходимые runtime links/capabilities: итоговый вывод и доступ к существующему reconstruction/FG status, где это требуется текущими consumers. Сохранить GetReconstructionStatus, GetFrameGenerationStatus, suspension, memory/graph statistics, timers и WriteRenderGraph. Не заставлять runtime разыскивать passes по именам или downcast к Bistro. Если прежний query неприменим к выбранному пайплайну, использовать существующую ошибку/disabled семантику, а не ложный успешный статус.

Перенести выбор цепочки и пасс-специфическую подготовку (_particlesPass->BeginFrame, lighting/local-shadow/reflection Prepare, AO settings/rebinding, reconstruction settings/history) из жёстких веток runtime в владельца пайплайна или общий helper техники. Сохранить их зависимости относительно общих scene uploads и shadow/probe metadata. Общий scene extraction, материал/texture uploads и сервисы могут остаться в RenderModule; concrete water/grass extraction — нет.

Общие helper-операции допустимы, когда действительно устраняют дублирование Default/Bistro; они возвращают passes/links и не навязывают полный порядок. Не копировать весь RenderModule в два builder-класса.

### KTD2. Композиция и исполнение

RenderModule создаёт граф с устройством и внешними imports, вызывает BuildGraph, затем Compile. Он же вызывает Execute, отправляет итоговый command list и завершает кадр fence. Runtime обслуживает запрос перестройки без уничтожения ещё используемых GPU данных.

Bistro хранит выбранные AO inputs и порядок в собственном builder. Удалить специальные вставки AO из общего RebuildRenderGraph. Для early AO взять существующие prepass depth/normals; для late AO — нужные opaque outputs, сохранить прежние потребители AO. Несуществующий ресурс нельзя заменить одной перестановкой calls.

Каждый pass объявляет Read/Write/ReadWrite и физические buffer accesses. Использовать DependsOn только для порядка, не выраженного ресурсами. Экспортировать нужный выход, сохранять fallbacks и side effects, чтобы graph culling не удалял нужную работу. Queue eligibility остаётся описанием pass для текущего executor; не добавлять ручную queue/fence choreography в Bistro.

Существующие secondary passes и их links выбираются пайплайном, а устройства/ShareFences/исполнение legacy secondary path остаются runtime. Не переносить их в новый multi-GPU graph и не ломать старый режим. Проверить точную baseline: отдельный worktree первоначально основан на 9411599, актуальный основной checkout уже содержит более поздние изменения графа и частиц.

### KTD3. Владение и карта переноса

Сохранить имена переносимых GDX12-классов, если это уменьшает diff. Новое расположение определяет владельца.

| Источник | Назначение / действие |
|---|---|
| Engine.RendererDX12 GDX12GrassWind.h + src/GDX12GrassWind.cpp | Apps/App/src/Rendering/Grass/, вместе с Render.Grass/Render.Wind registrations |
| Engine.RendererDX12 GDX12GrassRenderer.h + src/GDX12GrassRenderer.cpp | Apps/App/src/Rendering/Grass/ |
| Engine.RendererDX12 GDX12Fluid.h + src/GDX12Fluid.cpp | Apps/App/src/Rendering/Water/, вместе с Render.Fluid registrations |
| Engine.RendererDX12 RenderPasses/GDX12FluidPass.h + src/GDX12FluidPass.cpp | Apps/App/src/Rendering/Water/ |
| App.Base ECS/Components/GrassRenderComponent.h, WindFieldComponent.h, FluidRenderComponent.h | Apps/App/src/Components/ |
| Engine.RendererDX12/Shaders/GrassCommon.hlsli, GrassExpand.hlsl, GrassRender.hlsl, WindField.hlsl | Apps/App/Shaders/ |
| Engine.RendererDX12/Shaders/FluidParameters.hlsli, FluidSolver.hlsl, FluidSurface.hlsl | Apps/App/Shaders/ |
| RenderModule::CollectGrassScene, fluid extraction из CollectReflectionScene, grass/wind subscriptions | Bistro-owned preparation и world lifecycle |
| GDX12DeviceResources::Grass и Fluids | Состояние Bistro; общие passes получают только нужный нейтральный интерфейс |
| SharedResource::FluidSurface | Заменить реальными links/dependencies в app passes и удалить из общего enum |

Удалить специализированные компоненты из WorldECS tuple. Использовать ECSStorage::RegisterComponent<T>() и existing additional pools во всех Bistro/test world creation paths до GetPool/ForEach. Bistro управляет своими listeners и очисткой при удалении мира/повторном использовании entity. Не менять всю ECS.

### KTD4. Единые render primitives и batches

Решение пользователя: включить общий путь static mesh, skeletal mesh и травы сейчас, вместо IRenderGeometryContributor только для травы. Принцип близок к Unreal PrimitiveSceneProxy → MeshBatch → pass-specific MeshDrawCommand; это ориентир, а не требование перенести весь Unreal renderer.

**Граница данных.** В Engine.RendererDX12 определить RenderPrimitive/RenderBatch и общий сбор/исполнение batches. Существующие ECS-компоненты остаются типизированными данными; общий слой extraction в App.Base знает static/skeletal компоненты, Bistro extraction знает grass. Они создают/обновляют renderer-owned records с устойчивыми handles. Geometry passes не читают World/ECS и не делают switch/dynamic_cast по Static/Skeletal/Grass. Не строить иерархию ECS-компонентов только ради полиморфизма.

**Минимальные данные primitive:** stable ID + generation, current/previous transform или существующие индексы, world bounds, visibility и shadow/reflection/static-dynamic признаки, revision геометрии/материала/преобразования для прежних кешей, ссылки на batches. Snapshot/записи живут столько, сколько использующий их кадр; удаление/повторное использование entity не переиспользует ещё занятые GPU данные.

**Минимальные данные batch:** geometry/vertex-source binding, material/shader binding, submesh/topology, диапазон instances, direct indexed/non-indexed либо indirect arguments/count-buffer descriptor, допустимые stages и ссылки на используемые GPU ресурсы. Использовать имеющиеся GDX12InstanceData, GDX12IndirectDrawArgs, geometry buffers и GPU visibility output; не конвертировать массовый ExecuteIndirect в цикл CPU Draw по объектам. Batch может описывать уже сгруппированный indirect stream.

**Контракт источника.** Маленький виртуальный интерфейс render-side источника собирает batches для запрошенного view; bounds/revisions публикуются отдельно до расчёта видимости. Источник не получает право менять pipeline и не скрывает dispatch/draw внутри произвольного Draw callback. Реализации могут кешировать неизменные данные; полноценная система retained-command caching не требуется. App.Base предоставляет static/skinned adapters поверх существующих загрузки/скиннинга, Bistro — grass adapter.

**Контракт pass.** Pass выбирает batches по visibility/stage/material признакам и формирует/исполняет draw descriptions общим кодом. Pass задаёт targets/formats, view/constants, depth/blend требования. Источник геометрии/материала предоставляет совместимые shader variants и bindings через узкий объектный контракт; specialised grass shaders остаются в Bistro. Проверять совместимость shader/root signature/targets вместо предположения, что у всей геометрии один layout. Не вводить material graph или полный аналог Unreal VertexFactory framework: поддержать реально используемые mesh/skinned/grass bindings. Запрещён grass-only bypass после общего mesh draw.

**Подготовка геометрии.** Существующий Grass::Draw одновременно делает expansion compute и draw, поэтому замена имени Draw не решает задачу. Разделить view-dependent expansion и готовый draw batch. Pipeline сначала публикует primitive bounds/revisions, затем определяет реально обновляемые camera/shadow/probe views, затем добавляет app-owned preparation passes перед их потребителями. Skinned batches аналогично ссылаются на результат существующего skinning. Каждый используемый view получает свой сохраняемый output/range в frame-slot ресурсах; не переиспользовать один scratch, пока предыдущий view его читает. Можно группировать подготовку нескольких views в одном pass с полным набором объявленных accesses. Не выполнять compute скрыто при сборе batches и не возвращаться к специальному Grass.Draw внутри common passes.

Точные активные shadow/probe views и cache decisions предоставляет существующая подготовка соответствующих passes через нейтральные view descriptions. Сохранить её семантику и создавать нужные outputs только для действительно исполняемых views. При изменении topology/resource bindings перестроить graph в штатной точке; изменения frame data не должны автоматически пересоздавать все passes. Исполнитель явно проверяет стоимость/память per-view outputs относительно baseline, не обещая ускорения.

**Все потребители.** Перевести opaque depth/normals/GBuffer/forward, cascaded/local shadows, probe shadow/face и общую mesh transparency на общий batch-consumer путь. Сохранить существующие GPU culling/indirect streams, opaque/masked/translucent filtering, material overrides и previous geometry для motion. Общие caches получают primitive revisions/caster bounds без отдельного grass hash. Main-camera visibility не заменяет отбор для света/probe; static-only captures исключают динамические primitives по признаку, а не типу.

Здесь не создаётся новый ISM gameplay component: существующий instancing представляется batch instance range/indirect stream. Special fluid/particle/fullscreen passes остаются специализированными узлами графа. Их искусственный перевод в primitive hierarchy не нужен.

### KTD5. Вода как явная цепочка Bistro

Bistro создаёт и связывает water passes обычным кодом, без override механизма общего пайплайна:

1. Opaque depth/motion → fluid surface; её выбранные scene depth/motion имеют fallback на opaque.
2. Lighting/SSR HDR → behind-water WBOIT → behind composition → fluid resolve.
3. Water resolve → front-water WBOIT → common particles → final composition.
4. Итоговый HDR → DOF → motion blur → reconstruction/output/FG; downstream получает выбранные scene depth/motion, SSR сохраняет opaque inputs.

Common WBOIT остаётся в движке. Если текущий интерфейс привязан к воде, заменить WaterPartition и entrypoint naming минимальным нейтральным DepthPartition All/Far/Near с optional separator depth. Оптика/выбор water depth принадлежат Bistro. DefaultRenderPipeline использует All без fluid passes.

Particles пишут финальные accumulation/revealage; финальная composition вызывает particle HDR blend для sorted/additive ровно один раз. При воде aerial perspective применяется в behind composition; final пропускает повторное применение. При отсутствии воды сохраняется один необходимый путь применения.

Off/empty ветки сохраняют aliases для surface и resolve и не оставляют dangling links. Удаление SharedResource::FluidSurface требует явных зависимостей на все внутренние потребляемые ресурсы либо producing pass, с отдельным учётом физических accesses. Не заменять это незадекларированными скрытыми reads.

### KTD6. Lifecycle и независимые обновления

Порядок: создать app pipeline → Initialize → каждый кадр wait frame slot → существующее обновление/snapshot приложения и общая подготовка в прежнем порядке → pipeline preparation/metadata → BuildGraph при необходимости → Execute → retire frame.

Не переносить игровую физику в PrepareFrame. Simulation delta и условия обновления воды/травы задаёт Bistro; BuildGraph не симулирует. Сохранить текущую семантику частиц и анимации выбранной baseline.

Resize/rebuild: дождаться требуемого GPU завершения, убрать старый graph до уничтожения используемых pass/link objects, обновить размеры/links, построить новый graph и сбросить history там, где это требовалось раньше. Не пересоздавать state без нужды и не дублировать listeners. Shutdown: остановить submissions, дождаться GPU, уничтожить graph, passes/state, затем устройства; world listeners снять до уничтожения мира.

### KTD7. Конфигурация и шейдеры

Удалить из общего Config/Engine.ini шесть ключей Render.Grass/Render.Wind/Render.Fluid Enabled/Quality. Те же true/Mid defaults уже предоставляют перемещаемые C++ registrations Bistro. Сохранить имена, metadata и startup ReadOnly locking.

ConsoleModule строго отвергает неизвестные CVars, загружая INI до AddModules. GlobalSettings.props копирует INI только при отсутствии output config. Поэтому старый non-Bistro output может содержать теперь неизвестные шесть ключей: документировать одноразовое удаление только этих ключей с backup и сохранением остальных настроек. Существующие Bistro configs продолжают работать без потери значений. Не добавлять dummy CVars, ignore-unknown или перезапись INI при каждой сборке.

Common shader headers PBR/CBufferStructures/ReflectionProbes/ShadowFrustum остаются в Engine. Исправить generic source-relative include lookup при необходимости, сохранить engine include root; shader compiler не знает путь Bistro. App project владеет app shader paths и packaging.

### Integration Risks

Проверить git state и текущие API перед работой. Прежняя задача остановлена с частичной реализацией в D:/Project/PEPEngine2/.worktrees/bistro-render-extensions на codex/bistro-render-extensions. Не удалять её вслепую: сохранить полезный перенос, заменить obsolete hook framework, проверить изменения её дочерних агентов.

На момент обновления документов основной HEAD — b8ef53d, незавершённых index conflicts больше нет; незакоммиченные параллельные правки всё ещё возможны. Это наблюдение не разрешает перезаписывать основной checkout. Исполнитель фиксирует выбранную baseline и сохраняет актуальные изменения async/particles/physics при интеграции. Проверки старой базы не выдавать за проверки текущего checkout.

## Implementation Units

### U1. Отделить сборку пайплайна от runtime

**Requirements:** R2–R6, R10. **Dependencies:** нет.

**Files:** RenderModule.h/.cpp; новые App.Base/Rendering/IRenderPipeline.h, DefaultRenderPipeline.h/.cpp; Apps/App/src/Rendering/BistroRenderPipeline.h/.cpp; затронутые project entries; RenderGraphTests и новые сфокусированные RenderPipelineTests при необходимости.

**Work:** KTD1/KTD2/KTD6. Перенести владение passes и composition в pipeline, оставить executor общим. Перенести hardcoded AO ordering и pass preparation с сохранением зависимостей. Сохранить ModuleLocator и public query consumers. Начальная механическая extraction может ещё использовать старое размещение water/grass; U2/U3 завершают границу владения.

**Checks:** два разных builder используют один runtime; тестовый app pass добавляется только в app code и связывается producer → pass → consumer; ранний/поздний AO имеет верные inputs и consumers. Rebuild не удваивает passes, destruction не оставляет graph references. Проверить existing graph order/fallback/async tests, не писать тесты, зеркалящие каждый getter.

### U2. Ввести primitives/batches и перенести траву/ветер

**Requirements:** R1–R3, R7, R9–R11. **Dependencies:** U1.

**Files:** новые renderer-side primitive/batch/source/draw-description definitions и общий consumer; RenderModule.cpp и RenderModuleSkinning.cpp; static/skeletal extraction adapters; GPU culling/visibility consumers по фактическим зависимостям; grass/wind строки Move Map; opaque/cascaded/local/probe/WBOIT geometry passes; GDX12DeviceResources; World.h; BistroGrass/BistroBounds и app world initialization; SecondGroupTests, GrassPlacementChecks, BistroGrassChecks, BistroAdditionsTests и сфокусированные primitive/batch tests.

**Work:** KTD3/KTD4. Сначала провести static/skinned geometry через общий representation, сохранив существующие indirect streams; затем подключить траву тем же контрактом и удалить concrete grass ownership/subscriptions/draw calls. Разделить grass expansion/draw и объявить per-view preparation dependencies. Перевести shadow/probe metadata на primitive records. Проверять каждую миграцию отдельно; не оставлять два независимых mesh/grass consumer пути.

**Checks:** static mesh и skinned character с material overrides, motion и shadows сохраняют результат; batches поддерживают current direct/indirect/instanced paths и не увеличивают число draws до количества instances. Новый тестовый geometry source принимается общими passes без правок их кода. Проверить pool registration/world destroy/entity reuse, placement/count/quality, shadow/probe views/static-only filter, revisions и порядок GPU preparation → draw. GPU smoke: все три источника в depth/normals/GBuffer, каскадных/локальных тенях и динамических отражениях, включая geometry вне main-camera frustum; сохранить masked/translucent materials. Отдельно проверить несколько views одного кадра на перезапись expansion buffers.

### U3. Перенести воду и связать прозрачность в Bistro

**Requirements:** R1–R3, R5, R8–R10. **Dependencies:** U1.

**Files:** water строки Move Map; BistroRenderPipeline; RenderModule/GDX12DeviceResources; common WBOITTransparency/Composition, TransparentPass.hlsl при обобщении split; GDX12RenderGraph.h; BistroWater/BistroBounds; SecondGroupTests, BistroWaterChecks, BistroRenderGraphTests.

**Work:** KTD3/KTD5. Снять fluid extraction с общего CollectReflectionScene, перенести state и delta в Bistro. Связать весь сегмент явно, включая particles и постобработку; удалить obsolete BuildTransparency provider/tail из старой реализации.

**Checks:** water on/off/empty, reset/clear/quality/resize, корректные depth/motion/HDR aliases и unchanged opaque inputs SSR; exactly-once particles/aerial perspective. Проверить transparency позади и перед водой и разные particle blend modes на GPU.

### U4. Обновить consumers, сборку и конфигурацию

**Requirements:** R1–R4, R10. **Dependencies:** U2, U3.

**Files:** App/App.Base/Engine.RendererDX12 vcxproj + filters; Apps/App/src/App.cpp; Apps/App.Base/src/App.cpp; Config/Engine.ini; shader compiler при необходимости; BistroStartupTests и все standalone world creation paths. Проверить RenderModule callers в BistroDofSmoke, BistroRenderGraphTests, LightingGpuSmoke, ObjectDataJobsTests, LoadingTests, SSRBistroTests: Bistro-сценарии получают Bistro pipeline, общие consumers — default.

**Work:** KTD7; удалить engine/app-base ссылки на moved types и shaders, обновить shader paths и ECS registration. App.vcxproj-importing тестовые проекты получают перенесённые app sources; отдельно проверить типы без startup регистрации. Сохранить null-window тестовые сценарии и публичный RenderModule API.

**Checks:** Debug/Release x64 затронутых проектов, App.Editor/base-only consumer без Bistro link dependency, app shaders со всеми используемыми entrypoints. Fresh shared INI startup и stale-config migration с сохранением пользовательских общих настроек; Bistro принимает прежние настройки воды/травы.

### U5. Приёмка и документация

**Requirements:** R1–R11. **Dependencies:** U1–U4.

**Work:** добавить docs/rendering/application-render-pipelines.md с короткими примерами передачи pipeline, создания/связывания своего pass, публикации primitive/batches и правилами lifecycle. Обновить старые ссылки/документы расширений, если они появились в остановленной реализации. Зафиксировать реальные сборки, CPU/GPU проверки, baseline и ограничения. Проверить diff; локальные коммиты только задачи; push не делать.

**Acceptance:** выполнить Verification Contract, показать граф Bistro и базового приложения; не считать rg-аудит заменой link/GPU проверок. Не менять ADR на «реализовано», если проверяемый результат ещё не завершён.

## Verification Contract

- **V1 Ownership:** source/project audit Engine/App.Base не содержит конкретных water/grass types, shader paths или скрытой сборки Bistro; общий consumer собирается и запускается без app-specific registrations.
- **V2 Composition:** два разных pipeline builders, изменение AO inputs/ordering и добавление app pass без изменения engine sources; корректные exports, side effects и fallback. Graph/RenderPipeline CPU tests.
- **V3 Visual routing:** BistroRenderGraphTests/SecondGroupTests/BistroAdditionsTests и GPU smoke для water/grass/transparent/particles/shadows/probes; проверить изображение и depth/motion, а не только успешный запуск.
- **V4 Lifetime:** повторные toggle/quality/resize/rebuild/world replacement; D3D12 validation не показывает новых state/use-after-free ошибок. Сравнить с baseline и отдельно назвать pre-existing failures.
- **V5 Compatibility:** Reconstruction/FG и history reset, AO early/late, graph statistics/timers, existing async controls и secondary-device route сохраняются. Если второго GPU нет, явно отметить аппаратную проверку как невыполненную.
- **V6 Build/config:** Debug/Release x64, relocated shader compilation, fresh/stale INI cases и existing startup tests. Сборка app-only изменения не должна требовать редактирования engine sources.
- **V7 Delivery:** task-scoped diff, docs, commit hashes, checkout/base и точные результаты. Отдельно указать отсутствие интеграции в main checkout, если работа остаётся в worktree.
- **V8 Primitives:** единый путь static/skinned/grass, отсутствие concrete-type dispatch в общих geometry passes, сохранение GPU culling/indirect/instancing, per-view preparation/resource lifetime и cache invalidation. Минимальный внешний geometry source проверяет расширение контракта без смены common passes.

## Definition of Done

Bistro сам собирает граф из общих и собственных passes; общий runtime исполняет его без знания воды/травы и без injection-point framework. Default pipeline пригоден другому приложению. Static mesh, skeletal mesh и трава используют общие primitives/batches и общий путь geometry passes без grass-only bypass. Вода/трава целиком принадлежат Bistro, существующие outputs, визуальное поведение, настройки и lifetime сохранены. Требуемые проверки выполнены либо ограничения явно задокументированы; непроверенное не выдаётся за подтверждённое. Нодовый редактор и расширение multi-GPU не реализуются.

## Design References

- [Unreal Mesh Drawing Pipeline](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-drawing-pipeline-in-unreal-engine): разделяет primitive proxy, mesh batch и команды конкретного pass. Использовать принцип, не копировать весь framework.
- [Unity BatchRendererGroup](https://docs.unity3d.com/6000.0/Documentation/Manual/batch-renderer-group-how.html): batches/draw commands и фильтры участия, в том числе shadows/motion. Это не доказательство одинакового устройства Unity и Unreal.
