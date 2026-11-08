# P0: общий контракт submissions и снимков

2026-11-08. База: `7d2ee938b4b16488afd1f52831346d065f80ca78`.
Worktree: `D:\Project\PEPEngine2\.tmp\mgpu-feature-2026-11-08\integrator`, branch `codex/mgpu-integrator`.
Область P0: маленький общий header и исполняемая CPU-проверка; scheduler, queues и transport следующих пакетов здесь не реализованы.

## Зафиксированный общий API

`Engine/Engine.RendererDX12/Include/Engine.RendererDX12/GDX12Submission.h`, namespace `GDX12Submission`, C++17, без Windows/D3D includes.

| Тип | Значение / владелец выдачи |
| --- | --- |
| `AdapterId` | Ненулевая identity **экземпляра устройства**, не физический LUID и не primary/secondary role. P1; reset/recreate выдаёт новую identity. |
| `QueueKind` | `Direct`, `Compute`, `Copy`. P1 переводит в native list type. |
| `Placement { AdapterId Adapter; QueueKind Queue; }` | Допустимое размещение. Без Adapter значение invalid; это не неявный выбор primary. Старый API графа сохраняет собственный прежний default. |
| `QueueId { Placement Location; uint64_t Instance; }` | Реальная очередь, включая incarnation. P1. |
| `TimelineId { QueueId Producer; uint64_t Instance; }` | Единственная producer queue и конкретный fence timeline. При замене fence новая identity. P1. |
| `CompletionPoint` | Уже успешно поставленный **queue Signal**, с исходными `TimelineId` и value. P1. |
| `FenceProgress { TimelineId Timeline; uint64_t CompletedValue; }` | Наблюдение native fence. Imported view сохраняет исходную timeline identity. P1. |
| `ResourceId`, `AllocationId` | Различают device-local resource view и физическую allocation/hazard alias соответственно. Создатель ресурсов/runtime; cross-adapter registration — P3b. |
| `TransferId`, `Epoch`, `ProducerInvocationId` | Поток публикаций, совместимость и отдельный запуск producer. Runtime/transport и техника. |
| `FrameSubmissionId` | Отдельный счётчик запуска render frame; не invocation и не swapchain index. Runtime P4. |
| `ResourceIdentity { ResourceId Id; AdapterId Adapter; Epoch ResourceEpoch; TransferId Transfer; AllocationId Allocation; }` | Зарегистрированная view. `Transfer=Invalid` для непередаваемого ресурса. |
| `TransferVersion { TransferId Transfer; Epoch VersionEpoch; ProducerInvocationId Invocation; }` | Точное поколение публикации. Пропуск необязательной публикации может оставить gap в invocation sequence. Simulation step/time/input revisions находятся в payload техники, не выводятся из этих ID. |
| `SnapshotReadMode` | `ExactVersion`, `LatestCompleted`. P3a реализует выбор и резервирование. |

Все ID — разные `enum class : uint64_t`, с `Invalid=0`; неявных конверсий из integers и между ID нет (`AdapterId{1}` допустим). Полные object/frame identities не переиспользуются за жизнь runtime. `Epoch` уникален внутри `TransferId`, invocation — внутри `(TransferId, Epoch)`; полная `TransferVersion` никогда не переиспользуется. Выдающие владельцы проверяют overflow до выдачи; на recreated stream требуется новая TransferId либо новый epoch. Не выводить identities из указателей, номера кадра или локального submission counter другой очереди.

Сигнатуры общего value API:

```cpp
constexpr bool Placement::IsValid() const;
constexpr bool QueueId::IsValid() const;
constexpr bool TimelineId::IsValid() const;
constexpr CompletionPoint::CompletionPoint(); // только invalid sentinel
constexpr TimelineId CompletionPoint::Timeline() const;
constexpr uint64_t CompletionPoint::Value() const;
constexpr bool CompletionPoint::IsValid() const;
constexpr bool CompletionPoint::IsReachedBy(FenceProgress) const;
constexpr bool TransferVersion::IsValid() const;
```

Равенство определено для placement, queue, timeline, transfer version. У completion point нет числового ordering/conversion. `Value()` нужен native queue API, не для сравнения независимых timelines. `IsReachedBy` сначала проверяет полную provenance; `UINT64_MAX` как device-removal sentinel никогда не означает completion.

Единственный friend-конструктор `CompletionPoint(TimelineId, uint64_t)` доступен `::GDX12CommandQueue`. Вызывать его разрешено **после успешного `ID3D12CommandQueue::Signal`** на собственной очереди. Это issued dependency, она ещё может быть GPU-in-flight. CPU `ID3D12Fence::Signal`, frame counter, запланированный submit и чужой локальный value не выдают такой token. Queue timelines не допускают CPU-signal продвижения; исходная очередь — единственный signal owner. P1 отвечает за это native ограничение; value type не является security boundary против backend-кода.

## P1: очередь и наблюдение completion

P1 реализует следующие дополнения, сохраняя прежние методы и callers:

```cpp
GDX12Submission::AdapterId GDX12Device::GetSubmissionAdapterId() const;
GDX12Submission::QueueId GDX12CommandQueue::GetIdentity() const;
GDX12Submission::TimelineId GDX12CommandQueue::GetTimelineIdentity() const;
GDX12Submission::CompletionPoint GDX12CommandQueue::GetLastCompletionPoint() const;
GDX12Submission::CompletionPoint GDX12CommandQueue::SignalCompletion();
GDX12Submission::FenceProgress GDX12CommandQueue::GetCompletionProgress() const;
void GDX12CommandQueue::WaitFor(const GDX12Submission::CompletionPoint& producer);
```

`ExecuteCommandList(s)` может сохранить существующий `void` API: после успешного queue Signal обновляется последний point, который забирает executor. `SignalCompletion()` нужен для явной границы без нового command list; не добавлять второй signal после каждого уже сигналящего Execute. При ошибке Signal новый token не выдаётся. `WaitFor` проверяет token и находит local/imported fence по **producer timeline**, проверяет HRESULT; CPU не блокируется. Неизвестный/неимпортированный timeline отклоняется. Импорт меняет native view fence, но не TimelineId. Нельзя сравнивать remote point с локальным `FenceValue` или брать максимум points разных queues. Retirement хранит набор points по timelines, а не одно максимальное число.

COPY/capabilities/handles и точный способ registry imports принадлежат P1. Существующий `WaitForQueue` переиспользуется там, где он корректен. Новые методы выше здесь документированы, **ещё не объявлены/реализованы в queue/device headers**; P0 не оставляет недоопределённых методов production API.

## P2a: dependency до Signal и physical hazards

Компилятор оперирует **planned batch references**, не `CompletionPoint`. Внутри `GDX12RenderGraph` P2a вводит plan-local `PlannedBatchId` и edge `BatchDependency { PlannedBatchId Producer; PlannedBatchId Consumer; }`. Это локальные типы compiler, не новый runtime token. Cross-invocation input задаётся `TransferVersion` и сохранённым Exact reservation, а не выдуманным fence value. Plan-local ID действителен только в своём compiled plan/invocation.

P2b после принятого producer submit связывает planned producer batch с `GetLastCompletionPoint()`; только затем разрешает native Wait зависимого consumer. До этого consumer submit отложен. Это не CPU wait на GPU completion. Никакой конструктор completion point из planned batch/frame/invocation в graph API не добавляется. Проверяется расширенный DAG с FIFO-порядком очередей и slot-release credits; unsatisfiable/cyclic dependency отвергается до зависимого submit.

Три независимые identity:

- `ResourceId` различает source-local, shared producer view, shared imported view и consumer-local resources. У каждого свои descriptors и state tracking.
- `TransferId` связывает все версии/слоты потока. Сам по себе он **не доказывает** физический alias, готовность или корректность памяти.
- `AllocationId` одинаков у views общей backing allocation на разных devices. Разные allocations ring slots получают разные AllocationId. Если несколько slots/placed regions находятся в одном heap и безопасное разделение ещё не доказано, они **консервативно получают один AllocationId**, а accesses сериализуются целиком. Мелкий subresource/byte-range alias tracker в P0 не требуется.

P3b регистрирует allocation alias до передачи resources графу. P2a учитывает и local resource hazards, и общую allocation identity; сравнения native pointers разных devices недостаточно. Для cross-adapter access отсутствующий AllocationId — ошибка регистрации, а не отсутствие hazard. Нельзя присвоить разным aliases разные ID ради устранения compiler dependency. При переиспользовании памяти прежние пользователи должны retire; смена epoch сама по себе не освобождает allocation. Разные streams также не могут скрывать общий backing allocation разными keys.

`GDX12RenderGraph::SharedResource` остаётся логической pipeline-зависимостью Scene/Environment/etc. Она не конвертируется в TransferId/AllocationId. Existing unmanaged buffers не становятся безопасными от placement tag.

## P3a: reservation и буферная опорная цепочка

Модель bounded shared/local slots принадлежит P3a; типы payload — технике. P0 не задаёт generic GPU-job registry, texture format model или layout payload.

Опорный fixture: producer-local buffer → shared staging buffer → consumer-local buffer → reader. Payload содержит sequence/epoch/checksum; metadata согласованы с `TransferVersion`. До повторной записи source требуется completion export COPY. Shared slot защищён до завершения import COPY; consumer-local slot — до завершения **каждого** reader. Export-ready, import-ready и reader release — разные events/CompletionPoints, возможно разных timelines.

Для Exact сначала atomically резервируются требуемое поколение и необходимые credits shared/local slots. Только после успешной reservation graph получает разрешимую **symbolic** зависимость версии. Exhaustion возвращает backpressure/deferred submit, не успешный token, не dropping требуемой версии. Native CompletionPoint появится после реального queue Signal. Ошибка/отмена обрабатывается до consumer submit; reader, освобождающий credit, не может зависеть от будущей работы, требующей этот credit.

Latest выбирает совместимую **уже импортированную локально завершённую** версию и удерживает её для всех readers кадра. Новый незавершённый export/import не добавляет wait ради свежести. Возраст диагностируется отдельно; техника решает содержательную совместимость epoch/revisions и fallback. Reset останавливает старые submissions и retires старые references перед reuse. Frame submission и producer invocation могут продвигаться независимо; повтор snapshot не повторяет simulation step.

## Владение следующей волной

| Пакет | Разрешённые файлы / граница |
| --- | --- |
| P1 | `GDX12Device.h/.cpp`, `GDX12CommandQueue.h/.cpp`, необходимые участки `GDX12CommandList.h/.cpp`; новый `Apps/App/tests/MGPUQueueTests.cpp`. Queue-specific fixture отдельно от `RenderGraphTests.cpp`. |
| P2a | `GDX12RenderGraph.h/.cpp`, минимальный `RenderPasses/GDX12RenderPass.h`, `Apps/App/tests/RenderGraphTests.cpp`. Только compile/plan и CPU tests; execution/retirement — P2b после P1. |
| P3a | Новые `GDX12SnapshotProtocol.h/.cpp` в renderer Include/src и `Apps/App/tests/MGPUSnapshotProtocolTests.cpp`. Только модель slots/versions/reservations; native transport — отдельные `GDX12SnapshotTransport.*` P3b после P1. |
| Интегратор | `GDX12Submission.h`, все `.vcxproj/.filters/.sln`, `RenderModule.*`, `IRenderPipeline.h`, `DefaultRenderPipeline.*`, `BistroRenderPipeline.*`, общие runtime/settings tests. Следующие пакеты передают точные project additions; не правят их параллельно. |

P2a CPU compiler tests используют planned batch IDs/versions, не нуждаются в фабрикации CompletionPoint. P3a model tests могут иметь отдельный standalone test executable без native renderer linkage. Текущий `MGPUSubmissionContractTests.cpp` содержит **fake глобальный `GDX12CommandQueue`** только для проверки закрытого issuer API. Его нельзя добавлять в `RenderGraphTests`, приложение, renderer library, или любой target с настоящим `GDX12CommandQueue` — это ODR violation. Не include настоящий queue header туда же. Для P3a такой model issuer допустим только в отдельном CPU target; для native/graph fixtures valid points выдаёт настоящая P1 queue, invalid-input tests используют default sentinel. Не добавлять production test backdoor.

## Конфигурация и проверки

Новые реально подключённые MGPU technique CVars должны иметь `false` default. Только correctness fixtures явно включают нужный режим и восстанавливают defaults при teardown, включая exceptional exit. P0 не подключает ни одной техники и не добавляет фиктивных switches. Существующие single-GPU/async paths, defaults, качество и simulation frequency не изменены.

Проверены existing defaults: `Render.Fluid.AsyncCompute`, `Render.Grass.AsyncCompute`, `Render.Particles.AsyncCompute`, `Render.AO.AsyncCompute` — все `false`. Новые MGPU switches отсутствуют. Единственный validation lease остаётся у ведущего; P0 его не изменяет.

### Фактические результаты

Все команды исполнялись из указанного integrator worktree, последовательно, с повторным чтением `validation-lease.json` и проверкой `owner=P0`, `state=granted`, точного worktree перед каждым build/test. Performance measurements не выполнялись.

| Проверка | Результат и log относительно worktree |
| --- | --- |
| Standalone C++17 contract, Debug/x64, warnings-as-errors | PASS; `Build/P0/contract-build-x64.log`, `Build/P0/contract-test.log`. Compile-time assertions: timeline provenance, одинаковые values разных очередей/adapters, replaced queues/fences, device removal, invalid tokens, private issuance, разные ID/epochs/allocations. |
| Compile database generation | PASS; `Build/P0/compile-commands.log`: 34 projects, 124 translation units. Только ignored worktree `compile_commands.json`; общий Serena project не переактивирован. |
| Fresh worktree `RenderGraphTests` Debug/x64 build | PASS с локальным `/Z7` override; `Build/P0/baseline-build-z7-final.json` и соответствующий `.stdout.log`. Renderer, Core, App.Base, UI и External.Libraries собраны локально; SDK/Jolt archives переиспользованы как dependency copies. |
| Штатный config fixture | FAIL до GPU startup: unknown `App.Water.HeightScale`; `Build/P0/baseline-debug.json` / `.stderr.log`. Исходный `Config/Engine.ini` не менялся. |
| RenderGraph, debug layer, fixture-only empty INI | PASS; `Build/P0/baseline-debug-minimal-ini.json` / `.stdout.log`. |
| RenderGraph, debug layer + GPU-based validation, тот же INI | PASS; `Build/P0/baseline-gbv.json` / `.stdout.log`, stderr пуст. |

Оба успешных GPU runs сообщили: scheduling/cycles, linked inputs/rollback, pooled/dedicated resources, queued frames/resize/fallback, multiple async producers, UAV texture aliases/prefix/retirement, RAW/WAR/WAW, side-effect/final joins, **400 changing frames concurrent texture readers**, `Render graph tests passed; clean D3D12 debug layer.` COMPUTE capability обязательна в fixture, skip отсутствует. Это single-device DIRECT/COMPUTE correctness baseline; COPY и physical two-GPU acceptance принадлежат следующим пакетам и здесь не проверялись. Ни full Bistro/IBL suite, ни длительный soak не запускались.

Успешный GPU executable SHA256: `1d4d500c3a57862812aa2d1a0f9a0426c0c219818d59029e096dae8a07b255dc`. Runner проверял executable/INI до и после каждого запуска; inputs не изменились. Queue/graph/test source fingerprints также сохранены в build state. P0 не изменяет эти исходники.

### Воспроизводимый setup следующей волны

1. VS 18 Professional: `C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe`, toolset **v145**, `/p:PreferredToolArchitecture=x64 /m:1 /nr:false`. Попытка v143 дала MSB8020; стандартная PDB-сборка дала LNK1101/C1902. Это ошибки окружения, не результат MGPU API. Не менять tracked project flags ради PDB workaround.
2. Worktree submodules изначально пусты. Создание junction было отклонено OS (`Access denied`); повторов с повышением прав и junctions нет. Выполнены обычные **локальные копии** из существующего `D:\Project\PEPEngine2`: `inih/*`, `rapidyaml/src`, `rapidyaml/ext/c4core/src`, `jolt/JoltPhysics/Jolt`. Последующие builds читают эти копии. Общие source/build output не изменялись.
3. В собственный `Build/Dependencies/FidelityFX-SDK-c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/` скопированы `include` и `bin/ffx_sdk`; timestamps **локальных** lib copies выставлены после checkout inputs, чтобы существующий target не пересобирал готовый SDK. В собственный `Build/Dependencies/AgilitySDK-1.619.6/` скопирован установленный SDK. В собственный `Build/JoltOptimized/Debug/` скопирован `Jolt.lib`. Ни SDK, ни Jolt не пересобирались; не направлять output на общий cache.
4. В `Build/P0/Validation.targets` находится только validation override ниже. Он переопределяет и per-file `/Zi`, отключает PDB linker и проверяет наличие локальной Jolt dependency вместо её пересборки. Оптимизация, CRT, defines, shaders, качество и алгоритмы не меняются. Это ignored build artifact, не production change:

```xml
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemDefinitionGroup>
    <ClCompile><DebugInformationFormat>OldStyle</DebugInformationFormat></ClCompile>
    <Link><GenerateDebugInformation>false</GenerateDebugInformation></Link>
  </ItemDefinitionGroup>
  <ItemGroup><ClCompile Update="@(ClCompile)"><DebugInformationFormat>OldStyle</DebugInformationFormat></ClCompile></ItemGroup>
  <Target Name="PrepareOptimizedDebugJolt" BeforeTargets="PrepareForBuild">
    <Error Condition="!Exists('$(PEPExternalJoltLibDir)Jolt.lib')" Text="P0 validation requires a local copy of the existing optimized Debug Jolt archive." />
  </Target>
</Project>
```

Точная successful build invocation, запущенная runner с timeout 900 и fingerprints `GDX12CommandQueue.cpp`, `GDX12RenderGraph.cpp`, `RenderGraphTests.cpp`:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe' Apps/App/RenderGraphTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:PreferredToolArchitecture=x64 /p:ForceImportBeforeCppTargets=D:\Project\PEPEngine2\.tmp\mgpu-feature-2026-11-08\integrator\Build\P0\Validation.targets /m:1 /nr:false /nologo /v:minimal
```

Standalone CPU build использует ту же MSBuild invocation, но project `Apps/App/MGPUSubmissionContractTests.vcxproj` и **без** `ForceImportBeforeCppTargets`; запуск: `Build/bin/x64/Debug/MGPUSubmissionContractTests/MGPUSubmissionContractTests.exe`.

5. Только в output `Build/bin/x64/Debug/RenderGraphTests/Config/Engine.ini` создано содержимое `[ConsoleVariables]` без настроек. Предыдущая копия сохранена как `Build/P0/Engine.original.ini`. Это fixture startup setup, не настройка нагрузки/качества. Точный successful GPU запуск из integrator worktree после проверки lease:

```powershell
python C:/Users/Asus/.codex/skills/peengine-perf/scripts/run_job.py --cwd Build/bin/x64/Debug/RenderGraphTests --state Build/P0/baseline-gbv.json --timeout 300 --heartbeat 25 --fingerprint Build/bin/x64/Debug/RenderGraphTests/RenderGraphTests.exe --fingerprint Build/bin/x64/Debug/RenderGraphTests/Config/Engine.ini -- D:\Project\PEPEngine2\.tmp\mgpu-feature-2026-11-08\integrator\Build\bin\x64\Debug\RenderGraphTests\RenderGraphTests.exe --gpu-validation
```

Debug-layer run — та же команда без `--gpu-validation`, state `Build/P0/baseline-debug-minimal-ini.json`. Следующие worktrees заменяют **все** integrator paths своими и не запускают hardware workloads без отдельного granted lease.

Все запущенные builds, generator и CPU/GPU test workloads завершились. Проверка процессов `MSBuild`, `cl`, `link`, `MGPUSubmissionContractTests`, `RenderGraphTests` после проверок не нашла активных процессов. Тестовый слот можно передать; lease не изменён. Остаточные LFS-различия исходного worktree не входят в P0.
