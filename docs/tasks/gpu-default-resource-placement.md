# Устранить неоправданные прямые GPU-чтения из UPLOAD

## Цель и границы

На дискретной GPU рабочие статические и многократно читаемые массивы находятся в DEFAULT.
UPLOAD используется для передачи CPU → GPU и обоснованных небольших динамических входов.
Проверить все собственные пути движка, включая редко включаемые эффекты и прямые вызовы в тестах;
одного успешного запуска Bistro недостаточно для закрытия задачи.

Исходная база задания: `1283f8f`. Сначала проверь актуальный код: часть работы могла быть выполнена позже.
Прочитай корневые `AGENTS.md`, `CONTEXT.md`, ADR [0001](../adr/0001-sequential-systems-parallel-batches.md),
[0002](../adr/0002-application-owned-update-order.md), [0003](../adr/0003-indirect-rendering-parallel-object-preparation.md).
Сохрани чужие staged/unstaged изменения; выполняй задачу отдельными проверяемыми локальными коммитами.

Сцена, число объектов/частиц/светов, качество, ExecuteIndirect, физика, animation/motion и алгоритмы
рендера сохраняются. Пакетирование теневых faces остаётся отдельной задачей и сюда не входит.
Работа должна быть полезна при выключенном ReBAR; включение ReBAR и внедрение GPU_UPLOAD не требуются.

## Корректная модель памяти

- Обычный D3D12_HEAP_TYPE_UPLOAD на дискретной GPU — системная RAM; его размер не ограничен 256 МБ.
  Размер старого окна BAR относится к CPU-доступной области VRAM. Это разные механизмы.
- Постоянные VB/IB и статические structured arrays — DEFAULT, с первоначальным staging upload.
- Изменяемые массивы для многократного shader-чтения — актуальный DEFAULT snapshot:
  CPU → frame-local UPLOAD → GPU copy → DEFAULT → consumer.
- UPLOAD staging допустим и во время загрузки, и каждый кадр. Его наличие само по себе не дефект.
- Небольшие динамические CBV/root constants оставляй в текущем подходящем размещении.
  Малый structured buffer можно оставить напрямую в UPLOAD только как явное обоснованное исключение:
  известны максимальные bytes/count, частота чтения, lifetime и результат подходящего замера.

Основания: [Microsoft — heap types](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_heap_type),
[NVIDIA — GPU-чтения системной памяти и предварительное копирование](https://developer.nvidia.com/blog/optimizing-dx12-resource-uploads-to-the-gpu-using-gpu-upload-heaps/),
[AMD — BAR и GPU_UPLOAD](https://gpuopen.com/learn/using-d3d12-heap-type-gpu-upload/).

## 1. Инвентаризация до изменений

Проследи выделение → CPU writer → copy/update → реальный GPU binding для всех собственных
UPLOAD-ресурсов в `Engine/` и production-коде `Apps/`. Учитывай явные D3D12 heap types,
EBufferType::Upload, аргументы конструктора по умолчанию, алиасы Buffer<T>, helper factories,
root SRV/CBV, descriptor SRV, IA vertex/index bindings и косвенные вызовы.
Название GDX12UploadBuffer не определяет heap: этот класс также обслуживает DEFAULT.

Составь таблицу ресурсов: владелец/пути, фактический heap, размер и предел роста, частота записи,
GPU-потребители и частота чтения, способ обновления, frame/fence lifetime, решение и обоснование.
Раздели staging, прямые GPU-входы и уже корректные DEFAULT resources. Тестовые негативные фикстуры
и внутренние выделения сторонних SDK отмечай отдельно; код SDK в рамках этой задачи не переписывай.
Результат поиска по одному слову Upload или замена типов без проверки bindings инвентаризацией не считается.

Известные места на базе задания — стартовая карта, а не исчерпывающий список:

| Ресурс / путь | Подтверждённое использование | Требуемая работа |
| --- | --- | --- |
| LocalLightCache, GDX12DeferredLightingPass.h | Прямой compute SRV | Переиспользовать существующий DEFAULT ShadowLights с актуальным содержимым |
| Scene caches, GDX12ReflectionProbePass.cpp | Instance/material/templates в culling; material/transform/instance/light в capture и probe sun shadow | Перевести всех совместимых потребителей на общий DEFAULT scene snapshot |
| MaterialCache/LocalLightCache, GDX12GrassRenderer.cpp | Graphics SRV при Draw | Переиспользовать общий snapshot; проверить main/shadow/reflection вызовы |
| Grass roots, GDX12GrassRenderer.cpp | Per-owner/per-slot UPLOAD Roots, compute SRV | DEFAULT roots с корректным staging, изменением содержимого и ростом |
| Skinning Vertices/Ranges/Influences, GDX12Skinning.cpp | Статические UPLOAD массивы читаются compute при каждом Dispatch | Однократный upload в DEFAULT; сохранить проверки входных мешей и готовность до первого dispatch |
| Skinning Palettes, GDX12Skinning.cpp | Динамический structured root SRV, данные current/previous bones | DEFAULT per-slot palette после CPU animation update; прямой UPLOAD допустим только по правилу малого исключения выше |
| FireflyVertices, GDX12ParticlesPass.cpp | Статический mesh-particle VB непосредственно в UPLOAD | Однократный upload в DEFAULT до первого raster draw, включая ленивое включение mesh режима |
| ReflectionProbes metadata | UPLOAD structured SRV в deferred/WBOIT/SSSR/fluid | Оценить как малое исключение: сейчас максимум 32 × 52 = 1664 байта на frame slot; при переносе сохранить момент публикации |
| Main/Camera/Shadow/Sky и pass constants | Динамические CBV; в skinning параметры dispatch — root constants | Классифицировать; перенос только из-за названия Upload не нужен |
| Geometry/texture staging, light-grid Upload → Masks, fluid SeedUpload | Источники GPU copy в DEFAULT | Сохранить staging; проверить освобождение после fence и отсутствие shader/IA binding источника |

Основные файлы: `Engine/Engine.RendererDX12/src/GDX12DeviceResources.cpp`,
`GDX12FrameConstants.cpp`, `GDX12ReflectionProbePass.cpp`, `GDX12GrassRenderer.cpp`,
`GDX12Skinning.cpp`, `GDX12ParticlesPass.cpp`, `GDX12FluidPass.cpp`, `GDX12SSSRPass.cpp`,
`Include/Engine.RendererDX12/GDX12UploadBuffer.h`, `GDX12FrameConstants.h`,
`RenderPasses/GDX12DeferredLightingPass.h`, `GDX12WBOITTransparencyPass.h`,
`Apps/App.Base/src/RenderModule.cpp`. Уточни весь набор callers по коду.

Готово, когда каждое собственное прямое GPU-чтение из UPLOAD имеет строку с решением,
а уже перенесённые main culling/CSM/prepass/G-buffer/WBOIT из `1283f8f` учтены как baseline.

## 2. Общий scene snapshot и моменты обновления

Переиспользуй существующие DEFAULT ShadowTemplates/Instances/Materials/Lights/Transforms;
UPLOAD CPU caches сохраняют роль источника изменений. Дополнительные дубли тех же массивов
для каждого прохода не нужны. Общие методы/поля можно переименовать по реальной ответственности,
если все callers и тесты обновляются вместе; совместимые aliases только ради этого патча не добавляй.

Проверь порядок каждого потребителя, а не только порядок main render graph:

- В RenderModule::OnRender DispatchSkeletalMeshes и CollectGrassScene выполняются ДО
  PrepareLocalShadowData/UploadLocalShadowData. Копии skin palettes/grass roots должны быть готовы
  перед их ранним dispatch, а не ждать общего scene upload. Сохранить готовность GPU-skinned geometry
  перед потребителями и current/previous motion данные.
- Материалы/света/instances/transforms для main/shadow/reflection Draw используют snapshot после
  финальных соответствующих CPU updates. Работа не зависит от включения локальных теней.
- ReflectionProbePass::WriteProbeData записывает метаданные ВНУТРИ Execute, после решений о готовности
  и публикации cubemap; disabled/empty path тоже записывает нейтральные данные.
  Ранняя общая копия metadata может устареть ещё в этом кадре. Если metadata переносятся, запиши
  copy после последней нужной записи и до первого потребителя этой версии, сохранив атомарную
  публикацию color/depth/boxes/mips и нейтральные записи для unready/disabled slots.
- Обнови direct Execute/Draw callers в GPU fixtures: тесты должны готовить тот же snapshot,
  который теперь читает production. Проверка старого CPU upload не доказывает актуальность DEFAULT.

Встраивай копии в существующий порядок команд и жизненный цикл ресурсов. Новый глобальный
upload framework, новые очереди или смена allocator не являются целью задачи.
Не меняй массово default heap конструктора GDX12UploadBuffer: CopyData и CBV callers имеют другие контракты.

## 3. Lifetime и память

Для каждого нового/переиспользованного DEFAULT ресурса обеспечить:

- CPU не переписывает staging, пока его читает GPU; frame slot и его fence соответствуют источнику.
  Статический staging освобождается после завершения upload, а не сразу после записи copy.
- Resource growth и descriptor replacement сохраняют данные и время жизни старых ресурсов до
  завершения всех использующих их команд. Учитывать retired grass owners, mesh detach и world unload.
- В bounds/dirty/revision contracts входят material/transform/light edits, изменение массивов,
  удаление объекта, смена владельца и повторная инициализация. Не добавлять дорогое hashing
  каждого неизменного массива вместо уже существующего dirty/revision контракта.
- Правильные COPY_DEST → NON_PIXEL/PIXEL SRV или VERTEX/INDEX состояния до фактического binding,
  включая первый вызов, повторный upload и смешанные consumers. GPU-produced arenas остаются DEFAULT.
- Пустые массивы, нулевые influences/lights/roots, пределы размеров и рост не создают invalid descriptors,
  uninitialized reads, overflow/truncation или старые видимые draw commands.

Отдельно посчитать постоянный DEFAULT объём, staging/in-flight объём, число allocations и copy bytes/frame.
Не называть сумму D3D12MA allocations resident VRAM. Для отключённых эффектов сохранять текущую
ленивую инициализацию; новые постоянно живущие большие arenas без активных потребителей не нужны.

## 4. Проверки корректности

Используй существующие GPU readback fixtures и добавь короткие регрессии для нового размещения:

- Реальные GPU buffers/bindings имеют ожидаемый heap; проверки результата читают DEFAULT через
  READBACK. CPU staging остаётся отдельно проверяемым источником, а не суррогатом GPU результата.
- Непустые initial uploads и первый draw/dispatch; edits/growth/deletion во всех frame slots,
  несколько кадров в очереди, пустые/отключённые paths, повторный enable, world unload/reload.
- Для static mesh/skin inputs: эквивалентные вершины, influences, current/previous pose, normals,
  skinned bounds и motion; malformed skin inputs сохраняют прежнюю валидацию.
- Для grass: изменение roots/materials/light, рост roots, main/depth/shadow/reflection stages,
  удаление и повторное использование owner, отсутствие geometry/motion из старого slot.
- Для mesh particles: первый lazy mesh draw, смена geometry/blend, depth/motion и HDR accumulation.
- Для scene/probe consumers: реальные material/transform/light edits достигают используемого
  GPU snapshot; publication/disable нейтрализуются во всех slots без лишнего кадра задержки.

Базовые цели: LightingGpuTests, ReflectionProbeTests --debug, SecondGroupTests,
ObjectDataJobsTests, BistroStartupTests, BistroRenderGraphTests. При изменении metadata probes
добавь SSRTests и затронутые fluid/reflection readbacks. Выбирай дополнительные проверки по callers.
Установи фактический baseline проверок до работы и отдели существующие сбои от новых регрессий;
не ослабляй assert ради green, особенно проверки material edits и GPU snapshot в ObjectDataJobsTests.

DX12 debug layer/GBV проверять отдельно от timing runs. Изменённые lifetime/update контракты должны
иметь регрессию, падающую при возврате старого/неактуального GPU binding, когда это практично.

## 5. Производительность и приёмка

Собери свежие baseline/candidate Release x64 одним toolchain и флагами; сохрани commit/diff,
EXE/PDB и runtime HLSL hashes, INI, GPU/driver, фактические ReBAR/capabilities и clocks.
Основной путь не зависит от ReBAR. Фактический статус зафиксируй без изменения BIOS;
при включённом или неподтверждённом ReBAR не называй прогон проверкой ReBAR-off.

Замерь не только Bistro: если в нём нет активных skinned meshes/probes/grass, он не проверяет
производительность этих путей. Используй существующие непустые fixtures/benchmark paths
для skinning, grass, reflection captures и mesh particles, не меняя пользовательскую Bistro-сцену.
Сравни одинаковые данные/число потребителей, копируемые bytes и синхронизацию.

Для каждого перенесённого семейства покажи повторные до/после: время GPU-потребителя вместе
с upload/barrier cost, CPU preparation/recording/wait, PCIe/system-memory traffic где доступно,
occupancy/throughput и цену памяти. Для заявленного median/p95 полного кадра нужен достаточно
длинный сопоставимый прогретый прогон, а не p95 из 12 кадров. Фазы динамической симуляции,
фоновые нагрузки и capture overhead учитывай явно. Рост occupancy не является критерием успеха.

Последнее сравнение [scene residency + lighting](../research/2026-11-03-scene-residency-lighting-results.md)
ускорило отдельные проходы, но не подтвердило FPS: полные кадры и работа частиц различались.
Эти цифры — контекст, не baseline новых сборок и не обещание ускорения всех эффектов.

Задача готова, когда:

1. Все собственные прямые UPLOAD consumers перечислены; статические и многократно читаемые
   рабочие массивы перенесены либо для малого динамического исключения приведены bytes/count,
   lifetime, результаты сравнения и причина оставить прямое чтение. Сторонние SDK выделены отдельно.
2. Во всех изменённых путях тестами доказаны актуальность данных и корректный lifetime/states;
   scene/качество/ExecuteIndirect и последовательность local-shadow faces сохранены.
3. Для placement-исправлений без выигрыша на текущей GPU прямо записан отрицательный результат;
   отсутствие непустого тестового workload не выдаётся за отсутствие эффекта или успешную проверку.
   Подтверждённые регрессии устраняются; ненужные дополнительные copies/allocations удаляются.
4. В отчёте есть полная итоговая таблица, команды/результаты проверок, открытые аппаратные
   ограничения, память/copy cost и пути к captures. Большие артефакты остаются вне Git.
5. Проверен diff, созданы локальные коммиты только этой задачи; переданы hashes. Push — только по запросу.
