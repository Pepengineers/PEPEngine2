# D3D12 Memory Allocator: Unreal, AMD и PEPEngine2

Дата проверки: 2026-11-01. Статус: исследование, внедрение не выполнялось.

## Рекомендация

**Trial — добавить AMD D3D12 Memory Allocator на ограниченный участок и измерить результат.** Для PEPEngine2 полезен общий механизм размещения GPU-текстур и буферов. Писать собственный аналог Unreal сейчас не оправдано: D3D12MA предоставляет нужный нижний уровень, а перенос системы Unreal потребовал бы интеграции с другими частями RHI и RDG.

Первый участок — принадлежащие движку текстуры и буферы через существующие GDX12-классы и загрузчик текстур. Существующие ожидания GPU сохраняются. Включать автоматический aliasing, runtime-дефрагментацию и новый residency manager в эту работу не предлагается.

## Что подтверждено у Epic

В текущей публичной документации Unreal 5.8 видны отдельные механизмы D3D12RHI: пулы read-only textures, RT/UAV textures, upload small/big blocks, дефрагментация buffer/texture pools с ограничением копируемых данных за кадр и отдельное управление residency. Наличие настройки не означает, что каждый пул активен: например, RT/UAV texture pool в таблице имеет нулевой размер по умолчанию. [Epic: Console Variables Reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-console-variables-reference).

Над этим уровнем RDG рассчитывает время жизни временных ресурсов, переиспользование одной области памяти разными ресурсами, переходы состояний и async-compute fences. Публичный `FRHITransientHeapAllocator` описан как first-fit allocator; его API учитывает fences и aliasing overlaps. Это механизм временных ресурсов, а не доказательство алгоритма всех D3D12RHI pools. [Epic: RDG](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine), [Epic: FRHITransientHeapAllocator](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RHICore/FRHITransientHeapAllocator).

Таким образом, «аналог AMD у Epic» — несколько встроенных механизмов размещения и управления ресурсами. Текущую private-реализацию классов `FD3D12PoolAllocator` / `FD3D12MultiBuddyAllocator` не исследовали; утверждать их актуальный default-алгоритм или превосходство над AMD по скорости оснований нет.

## Сравнение

| Задача | Unreal | AMD D3D12MA |
|---|---|---|
| Выделение памяти под ресурсы | D3D12RHI pools и специализированные allocators | Большие heaps, размещение ресурсов внутри них; TLSF, custom pools |
| Временные ресурсы кадра | RDG определяет время жизни и aliasing | API для aliasing есть; зависимости, barriers и время жизни задаёт движок |
| Дефрагментация | Предусмотрена интеграция с D3D12RHI pools | План перемещений; пересоздание, копирование, синхронизация и обновление ссылок выполняет движок |
| Residency | Отдельный механизм RHI | Бюджет и политика выделения; без собственного `Evict` / `MakeResident` |
| Применение в PEPEngine2 | Потребовалась бы адаптация частей движка | Самостоятельная C++-библиотека, один `.h` и один `.cpp`, MIT, работает на DX12 GPU разных производителей |

Источники AMD: [назначение и совместимость](https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/faq.html), [размещение и TLSF](https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/optimal_allocation.html), [aliasing](https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/resource_aliasing.html), [дефрагментация](https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/defragmentation.html), [ограничения](https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/general_considerations.html), [подключение](https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/quick_start.html). Факты о Unreal подтверждены источниками Epic выше; сравнительной оценки производительности эти документы не дают.

Проверенный последний релиз — **v3.2.1 от 2026-10-26**. Для эксперимента разумно закрепить этот тег. [AMD: release v3.2.1](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator/releases/tag/v3.2.1).

## Что уже есть в PEPEngine2

При проверке не найдены D3D12MA в подключениях проекта или выделенная система общего размещения обычных GPU-ресурсов. В `Engine.RendererDX12` и `App.Base` найдено 20 мест с `CreateCommittedResource`; это число мест в исходниках, а не runtime-аллокаций. Тесты и внутренние выделения сторонних SDK в него не входят.

- `Engine/Engine.RendererDX12/src/GDX12Texture.cpp:126` — обычная текстура создаёт committed resource. `Apps/App.Base/src/RenderModule.cpp:265,287,318` отдельно создаёт текстуры сцены и upload-буфер на каждую загрузку. Замены только внутри `GDX12Texture` недостаточно: загрузчик передаёт уже созданный `ExternalResource`.
- `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/GDX12UploadBuffer.h:81` — upload/default buffers создаются по отдельности; `GDX12AppendBuffer.h:66` создаёт отдельный ресурс для `sizeof(UINT)` counter.
- `Engine/Engine.RendererDX12/src/GDX12GeometryBuffer.cpp:109` — меши уже объединяются в общие vertex/index buffers. D3D12MA не заменяет эту арену. `Grow:17` и `Upload:35` создают ресурсы и вызывают `Flush()`; библиотека размещения сама эти ожидания не устраняет.
- `Apps/App.Base/src/RenderModule.cpp:205,216,357` — пакетная загрузка текстур уже существует, ресурсы удерживаются до завершения GPU. Это реализованная основа, а не отсутствующая возможность.
- `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/GDX12Resource.h:35` и `GDX12Texture.h:63` — текущие владельцы хранят `ComPtr<ID3D12Resource>`, но не metadata allocation.
- `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/GDX12SharedTexture.h` — специальный cross-adapter путь уже использует shared heap и placed resources; его не следует смешивать с первым этапом.
- `Engine/Engine.RendererDX12/src/GDX12FidelityFXPasses.cpp:127` — используется готовый DX12 backend FidelityFX. Его внутренние выделения не перейдут под D3D12MA от замены GDX12 helpers.
- `Apps/App/tests/TechniqueGpuBenchmark.cpp:169` — уже есть DXGI-замер usage/budget. Для сравнения нужно использовать его и добавить статистику heaps/allocations D3D12MA, а не вводить отдельную измерительную систему.

Проект собирает DX12-модуль через MSBuild, x64, v145, C++17; подключены DirectX-Headers. Сборку с D3D12MA и поддержку дополнительных аппаратных возможностей в этой проверке не тестировали.

Предыдущие решения: [ADR-0005](../adr/0005-modern-rendering-techniques-single-gpu.md) сохраняет текущий этап на одном GPU; [ADR-0008](../adr/0008-bounded-parallel-scene-loading.md) требует удержания upload-ресурсов до fence и определяет бюджет загрузки как оценку временных данных, а не предел VRAM. Ранее принятого решения о D3D12MA в ADR/research не найдено.

## Ожидаемая польза и пределы

Общие heaps позволяют реже выделять новую память при создании ресурсов. Возможна экономия накладных расходов и потерь на выравнивание; статистика покажет полезные allocation bytes, выделенные blocks и доступный бюджет. Пулы при этом могут удерживать свободную ёмкость, поэтому общий usage не обязан уменьшиться. Прирост FPS заранее не следует из этих механизмов. [AMD: allocation performance и budget](https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/optimal_allocation.html), [AMD: statistics](https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/statistics.html).

Один маленький буфер всё ещё может требовать крупного выравнивания. Простая замена committed на placed не превращает все counters в плотно упакованные 4-байтовые allocations: нужны поддерживаемый tight alignment либо диапазоны в общем buffer. Современный tight alignment зависит от SDK/runtime/драйвера/устройства; универсальное предположение «каждый buffer всегда тратит минимум 64 КиБ» неверно. [AMD: resource alignment и sub-allocation](https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/optimal_allocation.html), [Microsoft: tight alignment](https://devblogs.microsoft.com/directx/agility-sdk-1-716-0-preview-tight-alignment/).

Для заметного сокращения памяти одновременно существующих промежуточных render targets требуется анализ времени жизни и aliasing. D3D12MA предоставляет механизм размещения, но не строит render graph и не вставляет необходимые barriers. Temporal history и постоянные ресурсы нельзя считать временными только по имени прохода. [AMD: resource aliasing](https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/resource_aliasing.html).

Ускорение загрузки Bistro также ограничено: существующий отчёт относит 4,69–5,75 с к публикации ресурсов и GPU-upload, включая копирование и ожидания, а не только allocations; импорт OBJ и подготовка текстур занимают отдельные значительные этапы. Эти старые измерения не переносятся автоматически на текущую сборку. [Отчёт загрузки Bistro](../reports/2026-11-01-bistro-loading.md).

## Минимальный следующий эксперимент

1. Закрепить D3D12MA v3.2.1 в существующей системе External/MSBuild. Создавать allocator на `GDX12Device`, уничтожать после ресурсов и завершения GPU, перед device.
2. Подключить сначала принадлежащие движку текстуры и основные buffer helpers, включая создание ресурсов в `RenderModule`. Сохранить descriptor management и barriers. Учесть оставшиеся прямые вызовы в sky/probes/shadows/AO, чтобы статистика честно показывала охват.
3. Связать `Allocation` с существующим владением ресурсами, включая `ExternalResource`, копии обёрток, reset/resize и пакетные upload-списки. Удерживать allocation до завершения всех использующих её GPU-команд. Один `ComPtr<ID3D12Resource>` не заменяет владение субаллокацией. Память placed resources требуется инициализировать перед чтением; нельзя рассчитывать на прежние нули. [AMD: ownership и initialization](https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/quick_start.html).
4. Повторить существующий Release loading benchmark, DX12 smoke/readback с debug layer, resize, повторное создание/удаление сцены и несколько кадров в полёте. Сравнить время создания ресурсов, время до полной Bistro, peak/current DXGI usage, bytes полезных allocations против heap blocks и время кадра. Текущие тестовые committed readback-ресурсы могут оставаться независимыми от миграции.

Переход от Trial к широкому внедрению оправдан, если размещение действительно уменьшает стоимость создания ресурсов или расход памяти без регрессий в валидации, resize и времени кадра. При доминировании крупных постоянных ресурсов экономия может оказаться небольшой. Aliasing и дефрагментация оцениваются отдельно после этих измерений.

Новые сборки, GPU-прогоны и сравнение с исполняемым Unreal не выполнялись. Изменена только эта исследовательская заметка; существующие правки рабочего дерева не затрагивались.
