# GPU DEFAULT resource placement

Дата: 2026-11-03. Задание: [gpu-default-resource-placement](../tasks/gpu-default-resource-placement.md).
Baseline — `741a5c0`; production-код совпадает с указанным в задании `1283f8f`.
Код и регрессии: локальный commit `36f35c8` (без push).
Сцена, 346 Bistro props, particle budget 250000, качество, физика, animation/motion,
ExecuteIndirect и последовательность local-shadow faces сохранены. Runtime HLSL не менялся.

## Что изменилось

- Deferred lighting, reflection culling/capture/sun shadow и grass Draw используют
  существующие DEFAULT `ShadowTemplates/Instances/Materials/Lights/Transforms`.
  Старые имена сохранены без aliases; новые дубли scene arrays не создаются.
- Grass roots: DEFAULT на owner/frame slot, frame-local UPLOAD staging.
  Roots fingerprint вычисляется в существующем SettingsHash проходе по массиву и входит
  в общую settings signature; дополнительного обхода/hashing массива нет. Material/settings-only
  edits сохраняют прежний history contract, но не повторяют roots copy. Неизменный slot копии не требует.
- Skin vertices/ranges/influences: DEFAULT с завершённым initial upload перед возвратом
  CreateMesh. Palettes: DEFAULT + UPLOAD на slot; копирование внутри Dispatch после CPU pose update.
- Firefly VB: ленивые DEFAULT 120 × 44 bytes; initial staging живёт до upload fence.
- Probe metadata: DEFAULT + UPLOAD на slot. WriteProbeData копирует после окончательной
  записи в Execute, включая publication/no-work/disabled/empty ветви. Уже существующий
  TrackChanges/revision пропускает копию неизменной версии, включая нейтральные кадры.
- Fluid seed staging освобождается при следующем безопасном использовании его frame slot;
  до этого остаётся частью State. Удаление/замена State сохраняет существующий Flush.
- Общий buffer helper проверяет UINT byte-size limit до allocation и copy range через
  вычитание, исключая переполнение `elementIndex + count`.
- Direct GPU fixtures готовят scene/probe snapshots. Dormant secondary-device loop также
  готовит snapshot; сам secondary mode по-прежнему hard-disabled и не проверялся на двух GPU.

## Инвентаризация allocation → writer → binding

Поиск охватывал все Engine и production Apps, явные heap types, default constructor,
Buffer aliases/factories, root SRV/CBV, descriptor SRV, IA и indirect paths.
Начальные поисковые списки сохранены в `upload-sites.txt` и `all-buffer-sites.txt` артефактов.

Обозначения: F — frame slots (в прогонах 3); T/I/M/L/X — достигнутые capacities
templates/instances/materials/lights/transforms; V/B/J — skin vertices/bones/influences;
N — roots или fluid particles. Resize grow-only. Размеры — logical payload, без allocation padding.
Для GDX12UploadBuffer теперь применяется предел `count <= UINT_MAX / stride`;
CBV stride округляется до 256. У scene caches дополнительного малого фиксированного cap нет.
DEFAULT scene targets заменяют нулевую capacity одним валидным placeholder element;
нечитаемый хвост не означает живой объект, а пустой UPLOAD source может не иметь allocation.

Source map: [DeviceResources](../../Engine/Engine.RendererDX12/src/GDX12DeviceResources.cpp),
[FrameConstants](../../Engine/Engine.RendererDX12/src/GDX12FrameConstants.cpp),
[GrassRenderer](../../Engine/Engine.RendererDX12/src/GDX12GrassRenderer.cpp),
[Skinning](../../Engine/Engine.RendererDX12/src/GDX12Skinning.cpp),
[ParticlesPass](../../Engine/Engine.RendererDX12/src/GDX12ParticlesPass.cpp),
[ReflectionProbePass](../../Engine/Engine.RendererDX12/src/GDX12ReflectionProbePass.cpp),
[FluidPass](../../Engine/Engine.RendererDX12/src/GDX12FluidPass.cpp),
[SSSRPass](../../Engine/Engine.RendererDX12/src/GDX12SSSRPass.cpp),
[AmbientOcclusionPass](../../Engine/Engine.RendererDX12/src/GDX12AmbientOcclusionPass.cpp),
[GeometryBuffer](../../Engine/Engine.RendererDX12/src/GDX12GeometryBuffer.cpp),
[EnvironmentIBLPass](../../Engine/Engine.RendererDX12/src/GDX12EnvironmentIBLPass.cpp),
[DeferredLightingPass](../../Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12DeferredLightingPass.h),
[CPU texture writer / render order](../../Apps/App.Base/src/RenderModule.cpp).

| Ресурс / владелец | Baseline → итоговый heap, bytes / рост | Writer и частота | GPU consumers / чтение | Update, lifetime и решение |
| --- | --- | --- | --- | --- |
| IndirectCommandsCache, DeviceResources / ECS indirect build | UPLOAD, 24T/device | Изменение/удаление draw records, revision | Раньше reflection compute; теперь только CPU mirror/source | Frame-local template staging исключает GPU-чтение глобального изменяемого cache |
| ShadowTemplatesUpload → ShadowTemplates, DeviceResources | UPLOAD → DEFAULT, каждый F×24T | CPU mirror при новой revision slot | Main/CSM/local/probe culling, несколько views | Copy dirty slot, slot fence; baseline DEFAULT сохранён, reflection перепривязан |
| InstanceCache → ShadowInstances | UPLOAD → DEFAULT, каждый F×48I | ECS jobs, каждый подготовленный кадр | Main/shadow/probe compute и graphics | Общий существующий copy перед graph; per-slot fence/growth |
| MaterialCache → ShadowMaterials | UPLOAD → DEFAULT, каждый F×80M | Последний UpdateMaterialCB после lazy materials, dirty frames | Main/shadow/WBOIT/probe/grass graphics и culling | Общий snapshot после финальных CPU edits; baseline main paths сохранены |
| TransformCache → ShadowTransforms | UPLOAD → DEFAULT, каждый F×128X | ECS transform jobs, dirty frames | Main/shadow/WBOIT/probe graphics | Тот же общий snapshot; growth сохраняет CPU source, target меняется после slot fence |
| LocalLightCache → ShadowLights | UPLOAD → DEFAULT, каждый F×464max(1,L) | Light jobs и shadow Prepare, каждый кадр | Local shadows/WBOIT уже DEFAULT; теперь deferred/probe/grass тоже | Copy после Prepare, независимо от включения local shadows; LocalLightCount исключает чтение хвоста |
| ReflectionProbes metadata, FrameConstants / ReflectionProbePass | UPLOAD → DEFAULT, F×1664 + столько же staging; 32×52 fixed | Execute после publication; также neutral writes | Deferred/WBOIT/SSSR/fluid, по pixels/rays | Copy после последней записи, revision per slot; не ранний scene upload |
| Skin immutable Vertices/Ranges/Influences, Skinning | UPLOAD → DEFAULT, S=64V+8max(1,J) | CreateMesh один раз после прежней валидации | Compute root SRV каждый skin dispatch | Три initial copies, один load-only queue wait, затем staging освобождён |
| Skin palettes | UPLOAD → DEFAULT, F×P, P=128(B+1); staging F×P | Animation update перед выбранным dispatch | Compute root SRV по influences/vertices | Copy P bytes внутри Dispatch; frame-slot fence; current/previous сохранены |
| Grass Roots, GrassRenderer | UPLOAD → DEFAULT, F×12max(1,N) + staging; N≤262144 | BeginFrame, existing settings hash | Expansion compute SRV при каждом Draw/view | Copy dirty slot перед ранним dispatch; рост после slot fence; retired owner удерживает обе памяти |
| FireflyVertices, ParticlesPass | UPLOAD → DEFAULT, 5280 fixed | Первый lazy mesh enable | IA VB, mesh color и motion draws, много instances | One-time copy + fence; нет новой постоянно живущей arena для выключенного mesh path |
| MainCB / ShadowCB / SkyCB | UPLOAD, F×256 / F×512 / F×2×256 | CPU каждый нужный кадр | Root CBV разных graphics/compute passes | Малые динамические CBV оставлены; slot fence |
| CameraCB | UPLOAD, F×512×camera capacity | Camera jobs, current/previous matrices | Graphics/compute root CBV | Payload 352, stride512; slot fence/growth |
| LightCB legacy | UPLOAD object, count0, allocation0 | Production writers отсутствуют | Bindings отсутствуют | Не активный GPU-вход |
| Probe face Cameras / Shadow CB | UPLOAD, F×(6×512+512) | Capture Execute | Capture/cull/sun-shadow root CBV | Малые динамические CBV, per-slot fence |
| Deferred grid Upload → Masks | UPLOAD staging + DEFAULT, F×4×max(1,cells×ceil(L/32)) | CPU grid Prepare | Только Masks — compute SRV | Copy перед lighting; bounds проверяет grid builder; source не shader-bound |
| Deferred grid Constants | UPLOAD, F×256 (payload32) | Prepare | Root CBV | Малый динамический CBV, slot fence |
| Grass draw / wind Parameters | UPLOAD, 512 / 256 на вызов в slot (payload368 / 80) | Draw / wind update | Root CBV | Переиспользуемые per-slot vectors; число вызовов зависит от owners/views |
| Particle emitter Constants | UPLOAD, F×512/pool (payload288) | CPU emitter preparation | Simulation compute root CBV | Малый динамический CBV; pool replacement Flush |
| Fluid Constants | UPLOAD, F×256/state | CPU fluid settings | Simulation/surface/resolve root CBV | Малый CBV; slot fence |
| Fluid SeedUpload → Particles | UPLOAD copy-only, 36N; N≤1048576 | Validated CPU seeds при создании/reset | Только DEFAULT particles shader SRV/UAV | Seed copy один раз; staging живёт до безопасного возврата seed slot либо teardown State; disabled State может удерживаться до этого |
| AO own / XeGTAO Constants | UPLOAD, F×align256(sizeof T), own payload64 | AO Execute | Compute root CBV | Малые динамические CBV; slot fence |
| SSSR Constants | UPLOAD, F×256 (payload16) | SSSR Execute | Compute root CBV | Малый динамический CBV; slot fence |
| Geometry VB/IB staging, GeometryBuffer | UPLOAD, 56V / 4indices при load/growth | CPU geometry load | Copy source; реальные IA buffers DEFAULT | Local staging до Execute+Flush; рост сохраняет старые GPU-skinned slices |
| Texture staging, RenderModule | UPLOAD, GetCopyableFootprints bytes, loading budget | CPU subresources | CopyTextureRegion source; texture DEFAULT | Immediate Flush либо batch handles до WaitForUploads/Flush; размер не ограничен BAR window |
| Environment box staging, EnvironmentIBLPass | UPLOAD, footprints bytes | CPU cubemap rows при load | Copy source; cube texture DEFAULT | Execute+Flush до освобождения |

Уже корректные DEFAULT, без изменения placement: camera visibility commands/counters,
CSM/local-shadow/probe commands/counters, geometry arenas, grass expanded VB/IB/args,
wind fields, particle simulation/alive/dead/draw/args/sort, fluid particles/density/keys/offsets,
append buffers и cross-adapter texture. Factories Buffer<T> в grass/fluid/particles явно DEFAULT.

Отдельно от production: synthetic texture/geometry staging и READBACK fixtures;
legacy lighting oracle/benchmark `bindPrevious` намеренно читает исходный UPLOAD lights buffer.
Production execution fixtures используют новые snapshots, старый oracle не является проверкой их актуальности.
Внутренние allocations ImGui DX12 backend, FidelityFX, XeSS, Streamline/NGX и frame-generation SDK
исключены из патча; wrapper CBV перечислены выше. Собственных дополнительных UPLOAD allocations
в Engine.Core/Engine.UI не обнаружено. Малых structured UPLOAD исключений в production не оставлено.

## Порядок и lifetime

OnUpdate ждёт fence текущего slot. OnRender по-прежнему выполняет skeletal dispatch и
CollectGrassScene до PrepareLocalShadowData/UploadLocalShadowData. Palettes и roots копируются
своими ранними командами; готовность не зависит от local shadows. Scene mirrors копируются
после material/light/instance/transform preparation, перед всеми geometry/lighting consumers.
Probe metadata публикуются позже, внутри ReflectionProbePass::Execute.

Все новые copies используют существующую DIRECT queue. Нет нового upload framework,
allocator, GPU_UPLOAD или изменения default heap конструктора. DEFAULT проходит COPY_DEST →
shader-read; Firefly VB — COPY_DEST → GENERIC_READ, включающий vertex input.
Mesh detach/world teardown сохраняют существующий Flush; grass retirement покрывает все
предыдущие submissions до повторного использования retirement slot. GPU-проверки включают
удаление/recreation skin mesh, удаление/reuse grass owner и несколько queued slots.

## Цена памяти и copies

| Семейство | Постоянный DEFAULT / allocations | UPLOAD / in-flight | Дополнительные copies |
| --- | --- | --- | --- |
| Scene rebindings | 24T+48I+80M+464L+128X на slot, уже существовало | Существующие CPU caches + template staging | 0 новых; прежние четыре массива каждый кадр, templates по revision |
| Skin | S+F×P, 3+F allocations/attachment | Постоянные F×P, F allocations; transient S, 3 allocations при load | S один раз, P на фактический dispatch; initial один queue wait |
| Grass | F×12capacity, один DEFAULT buffer на owner/инициализированный slot | Столько же существующего staging; retired owner/growth учитываются до fence | Dirty slot:12max(1,N); неизменный slot:0 |
| Firefly | 5280, одна allocation при lazy enable | Transient5280, одна allocation до fence | 5280 один раз; steady0; initial один queue wait |
| Metadata | F×1664, F allocations; CPU mirror ещё F×1664 | Существующий UPLOAD F×1664 | 1664 при новой версии slot, неизменный0 |

Skin benchmark: S=6815744, P=16512, F=3; DEFAULT payload6865280, staging49536,
transient6815744 bytes. CreateMesh CPU (один cold sample, не статистика):4.038 →5.9251 ms,
включая новый static upload/wait. Grass benchmark:8192 roots, dirty copy98304 bytes;
DEFAULT/staging по294912 logical bytes на три slots, allocation padding отдельно.

В Bistro собственный D3D12MA allocation total:538 →541 allocations,
2986586112 →2986782720 bytes; blocks138 →141, block bytes3258449920 →3258646528.
Net +196608 allocation bytes согласуется с тремя metadata buffers и переносом Firefly.
Это **allocations/blocks**, не resident VRAM. GPU-produced arenas и SDK allocations не смешиваются
с этими logical формулами. Process totals для каждого benchmark phase записаны как PLACEMENT_MEMORY;
они включают общие resources fixture, а не только одно семейство.

## Корректность и доказательство bindings

Свежий baseline: все семь требуемых Release x64 suites прошли до production edits.
До исправления новые проверки дали ожидаемый red:

- Skin: `Skin shader inputs must use DEFAULT memory`.
- Grass: snapshot-poisoning depth readback потерял coverage на старом MaterialCache binding.
- Deferred: `Deferred lighting must read the published DEFAULT lights snapshot`.
- Probes: `Probe metadata consumers must use DEFAULT memory`.

Skin checks читают DEFAULT через READBACK, проверяют heap, static source bytes,
queued palette slots, current/previous pose, normals, zero influences и recreation.
Grass checks покрывают roots edits/growth, deletion/reuse, disable/enable, procedural empty roots,
Depth/Shadow/Full/Equal GBuffer/PrepassNormals/Capture, material/light edits и motion во всех slots.
CPU staging poisoning выполняется после завершённой snapshot copy, чтобы не подменить её source.
ObjectDataJobsTests сохраняет прежний Roughness0.23 assert и дополнительно читает DEFAULT material
snapshot во всех slots. Probes проверяются реальным READBACK вместо Map CPU staging.
Есть size-overflow/range regression для общего buffer helper.

Grass capture fixture использует настоящую tangent-space −Z normal map: существующий ribbon winding
противоположен записанной normal и обычный camera-side light давал только emissive.
Production shading не изменён. Lit/dark readbacks нового материала18.2598 /17.25 во всех slots;
assert на отклик света сохранён. Исправление winding/normal — отдельная задача.

Точные private roots/Firefly heaps доказаны PIX captures и экспортом C++:

| Binding | Baseline | Candidate |
| --- | --- | --- |
| Grass compute root1/t0, 8192×12 | Descriptor heap39[61] →resource70 →heap69 UPLOAD | Descriptor heap42[61] →resource13 →heap5 DEFAULT |
| Grass staging/copy | Прямое чтение roots UPLOAD | UPLOAD resource34 →copy98304 bytes →DEFAULT resource13 →NON_PIXEL_SRV перед dispatch |
| Firefly IA VB,5280 bytes,stride44 | Resource97 UPLOAD | Resource97 DEFAULT |
| Skin compute root1/2/3, финальный capture | Baseline heap checks дают red | DEFAULT heap11: vertices13/ranges12/influences14 |
| Skin compute root4 palette, финальный capture | UPLOAD в исходном Dispatch | DEFAULT resource6; UPLOAD resource5 →copy16512 →NON_PIXEL_SRV перед Dispatch1024 |

`binding-evidence.json` содержит source/export строки и SHA-256 capture файлов. Firefly initial upload
завершён до capture; utility copies экспортера — восстановление replay, не исходные upload events.
PIX bridge здесь отключён; binding chain извлечена из полного C++ export, а не выведена из имени.

## Сопоставимые timing runs

Оба варианта заново собраны VS MSBuild18.8.2 /MSVC14.51.36231, Release x64,
/O2 /GL /MD /Zi, PROFILE_BUILD, PreferredToolArchitecture=x64, /m:1 /nr:false.
Baseline production files берутся из Git741a5c0 через include/source overlay; рабочая копия
не переключалась. Harness одинаковый; baseline adapters убирают новый metadata upload API
и DEFAULT-only assert в старом metadata reader. Manifest сохраняет source hashes.
Идентичные INI скопированы из исходных baseline directories. Timed EXE/PDB сохранены
до добавления test-only PIX capture instrumentation; capture overhead в timings не включён.

RTX3060 Laptop, driver617.42. API:UMA=0, GPUUploadHeapSupported=1;
nvidia-smi BAR1 total8192MiB. BIOS ReBAR state отдельно не читался: это **не ReBAR-off validation**.
BIOS/system settings не менялись. Основной код использует обычные DEFAULT/UPLOAD и не проверяет ReBAR.

Для каждого fixture workload:5 repeats, минимум128 warm submissions и2 seconds warmup
перед каждым repeat, затем768 samples. Числа ниже — median пяти repeat medians, ms.
GPU timer охватывает CPU-prepared upload/barriers + consumers в callback; CPU record включает
preparation, CPU submit и fence wait логируются отдельно. Каждая submission завершается Flush:
это изолированная стоимость workload, не throughput полного queued renderer.
Эта основная таблица записана до последнего уточнения roots-only invalidation;
skin/reflection/particle production paths после неё не менялись. Окончательный grass повтор приведён ниже.

| Workload | GPU baseline →candidate | CPU record baseline →candidate | Fence wait baseline →candidate |
| --- | ---: | ---: | ---: |
| Skin65536 vertices,128 bones,5 influences/vertex | 1.314820 →0.172032 | 0.0094 →0.0115 | 1.3826 →0.2325 |
| Grass8192 roots, unchanged, one depth draw | 0.120832 →0.118784 | 0.1514 →0.1525 | 0.1973 →0.2018 |
| Grass8192 roots, edited, copy+one depth draw | 0.125952 →0.132096 | 0.1512 →0.1578 | 0.2005 →0.2075 |
| Mesh particles1024, weighted | 0.073728 →0.069632 | 0.0506 →0.0559 | 0.1436 →0.1444 |
| Mesh particles1024, additive | 0.074752 →0.074752 | 0.0518 →0.0599 | 0.1449 →0.1491 |
| Reflection quality1, scene copy+capture+GGX+metadata | 0.035840 →0.032768 | 0.0424 →0.0425 | 0.1186 →0.1101 |
| Reflection quality2 | 0.049152 →0.046080 | 0.0610 →0.0547 | 0.1187 →0.1104 |
| Reflection quality3 | 0.559104 →0.552960 | 0.1504 →0.1467 | 0.6216 →0.6163 |

Skin steady GPU cost уменьшился86.9%; CPU preparation вырос на0.0021ms, static creation дороже.
Для grass и particles универсального ускорения не установлено: additive GPU time не изменилось.
В первом edited-grass прогоне copy добавила0.006144ms (+4.88%). Повтор после всей серии:
unchanged0.134144 →0.125952, edited0.137216 →0.137216ms.
После roots-only fingerprint окончательный повтор: unchanged GPU0.124928 →0.116736,
CPU record0.1482 →0.1481; edited GPU0.124928 →0.133120, CPU record0.1483 →0.1551ms.
Для frequently edited roots с одним consumer результат **отрицательный**: до0.008192ms
добавленной copy/transition стоимости. Это цена DEFAULT snapshot в данном узком workload,
не подтверждённая regression полного renderer; ускорение edited path не заявляется.
Material/settings-only copies устранены без нового array scan. Времена single-draw workload
нельзя переносить на main+shadow+reflection суммарно без отдельного замера этих consumers.

Дополнительный LightingGpuTests --benchmark:1280×720, clustered4-thread CPU/GPU ms:
135 dense lights0.004500/3.655740 →0.004950/3.549950;
512 dense0.00371875/13.092500 →0.00403125/13.213600;
135 depth layers0.004725/0.483392 →0.00485625/0.475840;
512 layers0.00499375/1.323390 →0.004450/1.301760.
Небольшие разнонаправленные результаты; улучшение для всех lighting workloads не заявляется.
Scene snapshot copy уже существовала в baseline, новых scene allocations/copies этот binding не добавил.

## Nsight counters

По отдельному trace baseline/candidate на каждое семейство: trigger submit1000, limit128 submissions,
Ampere GA10x /Top-Level Triage, PM bandwidth1000, PC sampling и multi-pass off.
128 подходящих benchmark markers в каждом trace. Ниже — median трёх внутренних marker windows
(четверть/середина/три четверти), не средний FPS и не compiled shader register count.

| Семейство | PCIe RX GB/s baseline →candidate | CS occupancy% baseline →candidate | SM throughput% baseline →candidate |
| --- | ---: | ---: | ---: |
| Skin | 7.0515 →0.0225 | 49.718 →35.708 | 0.732 →2.578 |
| Grass unchanged | 0.5446 →0.0097 | 9.938 →10.673 | 1.501 →1.566 |
| Mesh particles weighted | 0.1126 →0.0706 | 0.237 →0.289 | 8.297 →7.045 |
| Reflection quality1 | 0.1628 →0.2146 | 1.258 →1.281 | 0.207 →0.217 |

GPC frequency этих windows около900MHz во всех вариантах; точные значения, VTG/PS occupancy,
VRAM throughput, RX bytes/TX и marker durations находятся в metric-evidence/metric-summary.json.
PCIe counters включают profiling/system traffic, не являются точным attribution одного buffer.
Reflection RX вырос на очень коротком workload с меняющейся metadata; обещания снижения всего traffic нет.
Occupancy skin снизилась одновременно с большим сокращением времени — её рост не критерий приёмки.
У trace есть предупреждения о missing per-shader metrics; такие rows не использованы.
Overflow warnings не обнаружены; warnings сохранены. Timing runs выше не выполнялись под capture.

## Bistro и ограничения измерения

Три процесса на вариант,601 frames/process,600 sampled frames; неизменные346 props и250000 budget.
Среднее полного кадра по процессам baseline:20.6035 /20.8229 /21.7128ms;
candidate:20.8035 /21.5263 /22.4174ms. Это wall-clock means, **не frame median/p95**.
Серия последовательно нагревала ноутбук: telemetry показывает до87°C и снижение graphics clock
примерно с2GHz до1.34GHz. Simulation фазы и фоновые процессы не фиксировались детерминированно.
Эти runs не доказывают ни FPS gain, ни причинную regression placement. Результаты не скрыты.
Bistro не заменяет непустые skin/grass/probe fixtures, приведённые отдельно.

Не проверены другая GPU, фактический ReBAR-off, x86/Debug configuration, prolonged soak
и детерминированный full-frame A/B с зафиксированными simulation/clocks.
Программные PIX captures проверяют heap/binding, не resident VRAM. Прямой UPLOAD не объявляется
256MiB ограниченным: старое BAR window и системная UPLOAD RAM — разные механизмы.

## Команды, проверки и артефакты

Обычная сборка: `MSBuild Apps/App/<target>.vcxproj /t:Build /p:Configuration=Release
/p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /nr:false`.
Для timing добавить `/p:PEPEnableProfiling=true
/p:ForceImportBeforeCppTargets=<artifact-root>/<baseline|candidate>.props`.

Запуски: LightingGpuTests; ReflectionProbeTests --debug; SecondGroupTests;
ObjectDataJobsTests; BistroStartupTests; BistroRenderGraphTests; SSRTests.
GBV отдельно: SecondGroupTests --gpu-validation и ReflectionProbeTests --capture-gpu-validation.
Timings отдельно: SecondGroupTests --benchmark (также --benchmark-skin/grass/particles),
ReflectionProbeTests --benchmark, LightingGpuTests --benchmark.
Bistro: `App.exe -Loading.BenchmarkOutput=<json> -Loading.BenchmarkFrames=601`.
Неверный --benchmark-skn проверен: exit1, а не пустой успешный benchmark.

Начальный sandbox build дал MSVC C1902/PDB failure, host build прошёл; source baseline failures
не обнаружены. Обычные build logs содержат existing conversion/vendor PDB warnings;
для измерения использованы отдельные свежие output directories и одинаковые flags.
Первый ошибочный Bistro launch с флагом Benchmark вместо BenchmarkOutput остановлен;
он не вошёл в таблицы. Первые ошибки overlay duplicate source были исправлены до successful builds.

Artifacts, исключённые Git: `Build/Captures/2026-11-03-default-placement/`.
Там timed-binaries EXE/PDB/INI, baseline source overlay/manifest/props, hashes,
timing logs/summary, telemetry,6 Bistro JSON,
8 ngfx-gputrace, PIX captures и C++ exports (включая final grass и skin),
metric evidence и binding-evidence.json, review receipt.
Build/regression logs: `Build/default-*.log`; финальные — `default-acceptance-*`.
Code review: focused correctness + независимый local adversarial, завершён без findings;
внешняя модель не использована, пользовательский WIP не передавался.

Финальный результат: Release x64 App и все7 основных test targets собраны;
LightingGpuTests, ReflectionProbeTests --debug, SecondGroupTests, ObjectDataJobsTests,
BistroStartupTests, BistroRenderGraphTests, SSRTests — exit0. Дополнительный
MemoryAllocatorTests — exit0 (suballocation/copies/upload initialization/resize/ownership/teardown).
Оба отдельных GBV набора — exit0. После последней roots-only правки повторены App build,
SecondGroupTests --gpu-validation и ReflectionProbeTests --debug — успешно; финальные grass
timing и PIX capture тоже exit0. `git diff --check` прошёл.

Основания модели памяти: [Microsoft heap types](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_heap_type),
[NVIDIA uploads](https://developer.nvidia.com/blog/optimizing-dx12-resource-uploads-to-the-gpu-using-gpu-upload-heaps/),
[AMD BAR/GPU_UPLOAD](https://gpuopen.com/learn/using-d3d12-heap-type-gpu-upload/).
