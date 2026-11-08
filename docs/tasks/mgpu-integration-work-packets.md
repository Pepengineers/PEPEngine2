# MGPU: пакеты для параллельных чатов Sol/high

Исполнять после отдельного запроса на разработку. Спецификация: [план](../plans/2026-11-08-mgpu-integration-plan.md). При выборе алгоритма или заимствовании кода читать соответствующую строку [аудита](../research/2026-11-08-mgpu-source-audit.md), а не принимать весь reference project за корректную реализацию.

## Порядок и владение

Каждый пакет — отдельный чат **Sol / high**, изолированный worktree и `codex/mgpu-<packet>` branch от указанного интегратором commit. Подключение всех шести техник входит в результат. Пакеты не требуют одновременного исполнения одним агентом всего задания.

```text
P0: согласовать минимальные общие типы и исходный baseline
 ├─ P1: queues/capabilities
 ├─ P2a: graph plan/compiler
 └─ P3a: snapshot protocol/model tests
P1 + P2a → P2b: graph executor/lifetime
P1 + P3a → P3b: DX12 snapshot transport
P2b + P3b → P4: runtime + приложение + synthetic two-GPU chain
P4 → W: вода ───────────────────────┐
P4 → C: облака/окружение ────────────┤
P4 → S1 → S2: выбранные тени ────────┤
A0: half-AO → P4 + A0 → A1: MGPU AO ├─ P6: объединение всех шести + validation/profiles
P4 → G: трава ──────────────────────┤
U0: offscreen UI → U1: canvas/LOD    │
P4 + U1 → U2: UI placement ──────────┘
```

`A0` и `U0/U1` могут начаться после P0 на single-GPU baseline параллельно инфраструктуре: они не зависят от незавершённого MGPU API. Максимум 3 рабочих чата плюс интегратор — достаточная начальная организация; не требуется запускать все сразу. GPU-прогоны и benchmarks имеют одного владельца оборудования в каждый момент времени.

Один интегратор владеет `RenderModule.*`, `IRenderPipeline.h`, `BistroRenderPipeline.*`, `DefaultRenderPipeline.*`, проектными `.vcxproj/.filters` и общими runtime/settings tests. В P2a/P2b один последовательный владелец `GDX12RenderGraph.*` и `RenderGraphTests.cpp`. Technique-чаты меняют свои passes/ресурсы/шейдеры/проверки и передают интегратору точные wiring changes; общий pipeline не редактируется пятью чатами одновременно. P6 tests authoring не означает параллельное изменение production-кода владельцев.

После каждого пакета: локальный commit только своих файлов, фактические команды/результаты проверок, перечень неподтверждённого и интерфейсные изменения. Интегратор принимает commit на закреплённую базу, выполняет затронутые проверки и выдаёт следующую базу зависимым чатам. Нельзя продолжать по старой копии общего API после его изменения. Push и создание PR требуют отдельного запроса.

## P0 — минимальный общий контракт

**Владелец:** интегратор. **Вход:** план и текущая чисто зафиксированная интеграционная база; посторонние physics-правки в исходном worktree сохраняются.

Закрепить имена и расположение маленьких общих типов: adapter/queue identity, completion point, placement, resource/transfer identity, epoch. Отделить frame submission от producer invocation. Определить API границы runtime/graph/transport на одном buffer-copy fixture; не строить общую систему GPU jobs «на будущее». Типы snapshot payload принадлежат технике, протокол слота — transport.

**Готово:** все три инфраструктурных чата используют один проверяемый header/API commit, существующие DIRECT/async tests имеют записанный baseline; дефекты исходной базы отделены от новых. P0 не включает enable legacy `if(false)` как «готовый MGPU».

## P1 — устройства, очереди, fences

**Файлы:** `GDX12Device.*`, `GDX12CommandQueue.*`, необходимые участки `GDX12CommandList.*`; queue-specific tests отдельно от общего `RenderGraphTests.cpp`.

Добавить COPY, точные producer completion points, импорт shared fence по producer timeline, capability/failure handling и корректное владение Win32 handles. Для local queues не создавать cross-adapter sharing без необходимости. Исправить naming/типизацию команд; переиспользовать существующий `WaitForQueue`. Не связывать remote wait с локальным счётчиком submissions.

**Готово:** D→C→Copy→D на одном GPU и cross-adapter token fixture на двух; CPU не блокируется на queue Wait; проверены HRESULT, ошибочный импорт, shutdown и allocator reuse. Работает прежний single-GPU fallback. Нет нового бесконечного polling worker на каждую очередь.

## P2a / P2b — граф: сначала план, затем исполнение

**Файлы:** `GDX12RenderGraph.*`, минимальное расширение `GDX12RenderPass.h`, `RenderGraphTests.cpp`. P2b начинается после P1 и P2a; файлы не делятся между конкурирующими чатами.

P2a: сохранить существующую компиляцию и добавить placement, transfer accesses/aliases, версии и queue batch plan. CPU tests проверяют зависимость от правильного producer token и циклы с FIFO-order. P2b: реализовать submissions, barriers, partial joins и queue-aware retirement; independent producer invocations не висят на финальном Present join.

**Готово P2a:** deterministic plan для HAO-схемы и для background producer, некорректный план отвергается до GPU submission. **Готово P2b:** existing graph tests плюс новые Copy/multi-device/UAV/texture/prefix/retirement проверки; DIRECT fallback эквивалентен исходному. Unmanaged hidden buffers не становятся молча безопасными из-за placement tag.

## P3a / P3b — публикация и перенос снимков

**Файлы:** узкие новые transport files рядом с renderer; старые `GDX12SharedTexture`/copy-pass helpers менять только в рамках миграции их подтверждённых callers. Не редактировать `RenderModule.cpp`.

P3a: тестируемая модель bounded shared/local slots, ready/reserve/release/last-reader, Exact/Latest выбор, epoch, multi-reader retirement и отказ при исчерпании slots. P3b: реальные shared heaps/resources/fences, buffer/texture footprints, source-buffer protection и per-device views. Ready, import-ready и release — разные зависимости.

Exact-поколение нельзя пропустить при slot exhaustion: резервировать его до выдачи token, отложить submit при backpressure и не создавать невыполнимый wait. Latest выбирает completed-local version; Exact может ждать запланированный producer. Проверить demanded-generation preservation и отсутствие цикла через slot release.

**Готово:** sequence/epoch/checksum fixture не смешивает версии при независимом замедлении producer, exporter, importer и reader; memory/handle counts ограничены. Проверены odd dimensions, texture subresources, buffer payload, несовместимые форматы и размеры. Latest не ждёт новую незавершённую работу, Exact получает нужную версию. Native flags/footprints проверены по D3D12, не скопированы из reference без аудита.

## P4 — runtime и интеграция приложения

**Файлы:** принадлежащие интегратору `RenderModule.*`, `IRenderPipeline.h`, primary/default/Bistro pipeline wiring, frame resource integration и controls.

Подключить общую модель вместо secondary pass-vector rendezvous и снять взаимоисключение MGPU/async после покрытия тестами. Independent producer имеет свои inputs, constants/allocators/descriptors и simulation time. Добавить ручной выбор адаптеров/placement по технике, requested/active режим и причину fallback, profiling metadata возраста/пропусков/transfer.

Сохранить ADR-0010: runtime не содержит Bistro water/grass policy. Проверить текущие secondary scene uploads и предоставить S1 корректную точку репликации, не новую копию общего RenderModule. Удалять legacy APIs только после миграции всех callers.

**Готово:** synthetic строгая HAO-shaped chain + независимая публикация работают с несколькими frames in flight на двух адаптерах; pause/reset/resize/toggle/unsupported startup корректны. Primary rendering продолжает работать без secondary. Режимы включаются явно, проект не включает их по умолчанию.

## W — вода

**Файлы:** `Apps/App/src/Rendering/Water/*`, water checks в выделенном test-файле/существующем fixture; интегратор меняет вызовы из общих suite/pipeline.

Отделить solver ownership от surface owner, публиковать неизменяемые particle-state snapshots и необходимые container/input revisions. Сохранить Direct baseline и local async. Secondary solver, source→shared→primary copy, Exact/Latest. Обновить previous displayed positions для повторов и пропусков снимков; shader math не менять ради placement.

W владеет accumulation/debt между producer invocations: текущий clamp сам не сохраняет пропущенное время. Ограниченные шаги, остаток и dropped time тестируются для 60→30 Hz и перегрузки; при достаточном бюджете timeline совпадает с baseline.

**Готово:** parity того же simulation step, pause/hidden simulation/reset/moving container, bounded dt/catch-up и корректные motion vectors; transfer bytes и age измерены отдельно. Работает вместе с AO/grass async, в том числе когда solver медленнее рендера.

## C — облака и окружение

**Файлы:** `GDX12CloudComponent.*`, `GDX12EnvironmentIBLPass.*`, environment shaders и focused environment tests.

Разделить cloud generation и compute LUT/filter stages с явными inputs/dirty revisions; сначала проверить local COMPUTE, затем secondary placement и цельную публикацию cloud/environment outputs. Не выполнять static IBL работу каждый кадр ради демонстрации GPU occupancy. Размер/качество не меняется автоматически как в калибровке reference.

**Готово:** Direct/local async/secondary при одинаковых параметрах, изменение времени/солнца/настроек, cached vs regenerated resources, согласованные faces/mips/IBL, seed/reset/resize; baseline IBL validation отдельно от новых дефектов. 55→60 Hz — отдельный acceptance-сценарий повторения снимков, не глобальная целевая частота.

## S1 / S2 — выбор теней и перенос

**Файлы:** `GDX12CascadedShadowPass.h`, `GDX12LocalShadowPass.h`, `GDX12LocalLights.*`, связанные light snapshot structures и shadow-specific tests. `LightUpdateSystem.h`/общие constants меняются через согласованный commit S1; runtime wiring — интегратор.

S1 сохраняет single-GPU rendering и вводит устойчивую идентичность `Sun`/local EntityId, selection `{all, sun, all-local, specific-light}` и per-view output/revision mapping. Existing frame-local `ShadowFirstSlice`/`lightIndex*6+face` не являются идентичностью публикации. Проверить scene/material/geometry replicas, включая alpha-tested и динамические объекты.

S2 выполняет выбранные passes на secondary DIRECT и возвращает карты/регионы с их матрицами и mapping, оставляя неперенесённые тени primary. Поддержать солнце со всеми его каскадами, spot и все шесть faces point light. Публикация одного света атомарна по требуемому набору views; нельзя перезаписать primary atlas чужими/stale slices. Все потребители — lighting, transparency, reflection capture — видят согласованные bindings.

При whole-resource state tracking сериализовать import и primary atlas accesses либо выбрать отдельные imported textures. Разные slices не считать автоматически независимо синхронизируемыми ресурсами.

**Готово:** all/sun/all-local/single-point/single-spot, смешанное размещение, изменение порядка и удаление/создание света, static/dynamic caches, cascade coverage/camera cuts, placement toggle и selected-only transfers. Strict первым; Latest допускается лишь для явно валидного снимка с matching light/matrices. Разделение отдельных CSM cascades по GPU не подменяет обязательный выбор источников.

## A0 / A1 — half-resolution AO и MGPU

**Файлы:** `GDX12AmbientOcclusion.*`, `GDX12AmbientOcclusionPass.*`, `AmbientOcclusion.hlsl`, `AOTests.cpp`; settings/pipeline binding changes интегрирует владелец общих файлов.

A0: независимый Full/Half mode, Half = `ceil(renderWidth/2) × ceil(renderHeight/2)` относительно текущей scene render grid. Согласованное уменьшение depth/normals, обновлённые reconstruction constants, dispatch bounds и depth/normal-aware upscale на lighting grid. Поддержать SSAO/HBAO/XeGTAO/CACAO; native CACAO half flag не включать поверх уже half-sized graph input без доказанного контракта. Scene render scale, algorithm quality и AO resolution — разные настройки.

A1: ExactVersion input round trip и AO result transfer. В Half mode downsample inputs на primary, вернуть half AO, upscale primary; не пересылать full-sized inputs без необходимости. Сохранить early/late AO bindings, history/invalidation и local async.

**Готово:** 4 algorithms × Full/Half × primary DIRECT/COMPUTE/secondary COMPUTE, early/late binding checks, odd/tiny dimensions, depth discontinuities/halos, jitter/cuts, disabled neutral result, queued resize. Сравнение MGPU выигрыша делается Full↔Full и Half↔Half; Full↔Half — отдельное сравнение качества/стоимости. Ни статья, ни reference half-size accessor не заменяют проверку pipeline.

## G — трава

**Файлы:** `Apps/App/src/Rendering/Grass/*`, `GrassInteractionChecks.h` и focused checks; общий Bistro wiring — интегратор.

Перенос wind/interaction field с matching extent/origin/time/revisions, primary expansion и per-view culling. Сохранить Analytic/ProjectedGrid, actor interaction и firefly input contract. Не переносить generated vertex arena каждого view без отдельного обоснования.

**Готово:** оба wind modes, grass/fireflies off/on, изменения actors/field origin, Exact/Latest, age threshold, same-frame dependency с particles, primary fallback и совместная вода/AO. Измеренный ранее проигрыш local async не трактовать как запрет MGPU или обещание выигрыша.

## U0 / U1 / U2 — UI, world canvas и качество

**Файлы:** `Engine/Engine.UI` UI renderer/controls, узкая canvas render-surface модель и GPU effect shaders; пример плаката и material binding — Apps/App. `RenderModule`/pipeline/project files — интегратор. Ввод для world canvas **не входит**: пользователь запросил только отображение и анимацию.

U0: отделить ImGui CPU draw-data construction от device-owned offscreen renderer; убрать обязательный PRESENT transition у offscreen target. Сохранять draw data/texture identity/font lifetime до GPU completion. Прозрачный render target имеет явный alpha/color-space контракт, корректный blend и scale. Не отдавать secondary primary GPU descriptor handles и не инициализировать второй backend поверх живого global backend.

U1: небольшая поверхность с CanvasId, размерами, animation time, quality revision и material output. Добавить screen UI и минимум два world-space плаката, чтобы проверить выбор конкретного canvas; геометрия/камера/occlusion плаката остаются primary. Эффекты взять из [Multi-GPU-UI-Framework `a0a80c0`](https://github.com/Zakgard/Multi-GPU-UI-Framework/tree/a0a80c03438c5315afccfc5cdf2e91cfa488dc9f/MGPU-IMGUI), Low/Medium — как ориентиры статьи. Runtime LOD в этой ревизии отсутствует: `Render.UI.Quality=0|1` оформляет новый воспроизводимый выбор Low/Medium. Точные resolution, GPU star counts, shader variants и blur settings документируются; профиль должен доходить до shader constants/dispatch, не только UI helper. Нет автоматического снижения качества при смене GPU или уменьшения частоты всей сцены.

U2: whole-UI/selected-CanvasId placement; secondary COMPUTE effects → DIRECT UI raster → COPY export/import → primary screen composition или material sampling. Остальные canvases остаются primary. Простые поверхности могут использовать только DIRECT; наличие всех очередей не требует искусственного dispatch. Весь UI означает набор UI surfaces, а не перенос swapchain/scene.

**Готово:** screen whole-UI, один из двух canvases, animated poster, alpha/text/font/textures, LOD switch и resize в полёте, Exact/Latest для визуальных поверхностей, fallback и slot lifetime. Existing screen UI input не переносится на GPU; новых world hit-testing/focus/interaction задач нет. Сравнить каждый LOD на одинаковых settings/time/device baseline; отдельно показать цену RGBA transfer и выигрыши effect compute. Scene backdrop blur, если появится, требует отдельного входного scene-color transfer и не считается бесплатным свойством UI offload.

## P6 — объединение, проверки и профили

**Владелец:** интегратор с отдельным read-only/тестовым чатом. **Вход:** commits всех W/C/S2/A1/G/U2.

Собрать весь режимный перечень и матрицу из плана, добавить combined six-technique fixture и serial hardware runs. Проверить реальные requests/active placements, shared Copy contention, lifetime, memory, CPU overhead, age/debt и длительный soak. Выпустить ручные профили конкретных платформ с указанием качества и неподдерживаемых вариантов. Проценты из статей не являются acceptance threshold.

**Готово:** каждая заявленная строка режима имеет PASS или явно блокирующий дефект; отсутствие второй физической GPU означает, что MGPU hardware acceptance не завершён. Успешный single-GPU compile не закрывает этот пункт. Локальный commit, итоговый отчёт и документация controls обязательны; push отдельно.

## Короткий стартовый запрос для будущего чата

> Выполни пакет `<ID>` из `docs/tasks/mgpu-integration-work-packets.md` на интеграционной базе `<commit>` в отдельном worktree, модель Sol, reasoning high. Прочитай общий контракт плана и только относящиеся к пакету источники аудита. Владелец общих файлов — `<чат-интегратор>`; согласуй необходимые изменения интерфейса до редактирования его файлов. Закончи код, релевантные проверки и локальный commit; сообщи доказанные результаты, пределы проверки и точный wiring, нужный следующему пакету. GPU-прогоны согласуй по единственному слоту оборудования; не меняй качество или частоту симуляции ради результата.

Этот шаблон не создаёт чаты автоматически и не выдаёт разрешения на публикацию.
