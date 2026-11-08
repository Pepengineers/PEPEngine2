# MGPU: исходники, история AMorunov и статьи

Дата: 2026-11-08. Read-only исследование для [задания](../plans/2026-11-08-mgpu-integration-plan.md). Это анализ гарантий исходного кода, а не результат запуска двух GPU.

## Проверенные версии

- PEPEngine2: `codex/bistro-rendering`, `174a6aa593955d276f9fc0594d9c7504a886f32e`. Незакоммиченные изменения физики и `.tmp/` не относятся к исследованию.
- `D:/Project/Multi-GPU-Render-DX12`: `master`, `d17e45bcfef0a16af0f7ee74e936c162845bd221`, рабочее дерево чистое на момент проверки.
- Уточнённый UI-референс пользователя: [Zakgard/Multi-GPU-UI-Framework](https://github.com/Zakgard/Multi-GPU-UI-Framework), `master`, `a0a80c03438c5315afccfc5cdf2e91cfa488dc9f`. Ветка и полное дерево проверены через GitHub API. Ранее переданный другой fork не используется как источник выводов о UI-Framework.
- Статьи: восемь PDF в `D:/Preentation/Articles`. Ниже используются номера страниц PDF с 1; в `2026-ИПС-1.pdf` одна страница PDF содержит две печатные страницы.
- Точные настройки конференционного запуска неизвестны. Исходная MGPU-линия AMorunov найдена в достижимой истории и уже входит в текущую ветку.

## История AMorunov

`MoreRenderFeatures` вошла через merge `3cc7f65bc758e5890ed771b2b225e51aa1c8dec9`; его принадлежность HEAD проверена через `git merge-base --is-ancestor`. Удаление ветки также отмечено в [истории интеграции](../rendering/second-group-integration.md). Авторы перечисленных ниже MGPU-коммитов — AMorunov.

| Версия | Факт из исходников |
| --- | --- |
| `b7c783da7d2dceb36f8e3e22a4e2e25794161d4d` | `Apps/App.Base/src/RenderModule.cpp:656–680`: `GDX12SyncPass`, submit локального DIRECT list и ожидание чужого fence на **локальном** числовом fence value. |
| `e22621f487ead0c8548942b07694220a78286b6b` | Тот же файл, `777–785`: secondary рисует сцену и копирует итог в shared texture; primary копирует и показывает. В этой конфигурации нет явных `GDX12SyncPass`. |
| `30d520bfb59e3dda5c63c3ad6de4da4eadb25a31` | Исправления cross-GPU transfer; настроенный pipeline снова primary-only. |
| `3cc7f65` | `RenderModule.cpp:64`: secondary initialization уже под `if (false)`; `825–831`: настроен primary-only pipeline. `688–718`: общий update loop и таймер, frame rings обоих GPU продвигаются вместе. |
| `origin/main`, `672a4ef283f9104306bb1dbb7446347ee8aaf2ce` | `RenderModule.cpp:806–832`: старые симметричные waits сохранились, но `900–906` настраивает только primary passes. |

Для воспроизведения ссылок на прошлые версии: `git show <commit>:Apps/App.Base/src/RenderModule.cpp`.

Подтверждено сопряжение темпа через ожидания и общий frame loop. **Отдельный регулятор частоты расчётов по времени медленного GPU в этой истории не найден.** `Frequency` в timestamp-коммитах `9b42b58` и `03a3efb` служит переводу GPU ticks в миллисекунды; это не частота обновления техники. Новые COPY submissions, пропуски работ и независимые частоты разрушат предположение о совпадении чисел локального и удалённого fences.

## Текущий PEPEngine2

| Область | Проверенный контракт / ограничение |
| --- | --- |
| Runtime | [RenderModule.cpp](../../Apps/App.Base/src/RenderModule.cpp):107 отключает secondary; :952 исключает COMPUTE при `_dualGPUMode`; :969–977 содержит старый последовательный secondary loop. |
| Graph API | [GDX12RenderGraph.h](../../Engine/Engine.RendererDX12/Include/Engine.RendererDX12/GDX12RenderGraph.h):66 — `Execute(directList, computeQueue)`; :88 — `bool AsyncCompute`; :103 — один `_device`. Нет placement по адаптеру, COPY, поколений данных или независимого producer tick. |
| Dependency model | `SharedResource` в том же header:26 — логические токены Scene/Environment/Particles и т. п., **не cross-adapter allocation**. Physical hazards в executor сопоставляются по native pointer; разные device views общей памяти так не объединяются. |
| Compiler / lifetime | [GDX12RenderGraph.cpp](../../Engine/Engine.RendererDX12/src/GDX12RenderGraph.cpp):207 проверяет зависимости; :319 консервативно продлевает async lifetimes; :419 поддерживает partial joins; :578 закрывает весь COMPUTE перед retirement кадра. |
| Queue | [GDX12CommandQueue.cpp](../../Engine/Engine.RendererDX12/src/GDX12CommandQueue.cpp):23 создаёт shared cross-adapter fence только для DIRECT; :135 уже имеет queue-side `WaitForQueue`. Device предоставляет DIRECT и lazy COMPUTE, COPY в Engine/Apps не найден. |
| Legacy transport | [GDX12SharedTexture.h](../../Engine/Engine.RendererDX12/Include/Engine.RendererDX12/GDX12SharedTexture.h):87–177 — одна 2D ROW_MAJOR texture, один mip/slice; нет кольца, ready/release handshake, buffer transport. Allocation info запрашивается с UNKNOWN layout, placed resource — ROW_MAJOR; переносить это без проверки нельзя. |
| Legacy import | `RenderModule.cpp:1256–1277`: один чужой DIRECT fence на очередь; shared handles здесь не закрываются. Будущая реализация должна владеть каждым handle и импортом явно. |
| Resource state | [GDX12Resource.h](../../Engine/Engine.RendererDX12/Include/Engine.RendererDX12/GDX12Resource.h):31–46: одно tracked state и concurrent-read union. Нужны device-local состояния, общий transfer identity и завершение каждого пользователя. |

Граница приложения и движка уже задана [ADR-0010](../adr/0010-renderer-and-application-feature-ownership.md): приложение собирает pipeline и выбирает правила техники; runtime владеет устройствами, очередями, lifetime и исполнением зависимостей.

Дополнительная сверка после уточнений пользователя:

- `GDX12AmbientOcclusionPass.cpp:75–85` создаёт рабочие textures по полной input grid; `AmbientOcclusion.hlsl:53–65,163` и dispatches предполагают согласованные размеры. `CommonRenderPasses.cpp:15` относится к scene render scale, не независимому half AO. Native CACAO `useDownsampledSsao` есть в vendor header, но zero-initialized screen-size struct в pass:247 его не включает. Настоящий Full/Half pipeline — новая задача.
- `LightUpdateSystem.h:37,51,56` сохраняет локальный EntityId, сортирует lights и пересчитывает `ShadowFirstSlice`; slices нельзя использовать как постоянный ключ. Sun identity сворачивается в singleton constants (:28–34). `GDX12CascadedShadowPass.h:35,119` содержит sun atlas и четыре каскада; `GDX12LocalLights.h:8,24` — local resolution и 6/1 faces point/spot. Consumers теней включают transparency и probes.
- `Engine/Engine.UI/src/UI_test.cpp:146–182` создаёт один primary ImGui backend, :267 безусловно переводит target в PRESENT. `RenderModule.cpp:953` вызывает UI после графа. World canvas отсутствует; `DrawWorldBox` — projected screen overlay. Материал уже поддерживает diffuse/emissive textures (`GDX12Material.h:30–45`), что даёт primary consumer для плаката.

## Multi-GPU-Render-DX12: разные модели, а не один «режим без ожидания»

Все пути ниже относительны `D:/Project/Multi-GPU-Render-DX12` и относятся к указанному выше commit.

`Graphics/GDevice.cpp:85–108` создаёт DIRECT/COMPUTE/COPY. `Graphics/GCommandQueue.cpp:116–149,216–243` различает queue Signal, polling, CPU wait и GPU queue Wait. `Graphics/GCrossAdapterResource.cpp:16–59` создаёт две views общей памяти, но не задаёт владение версиями; один footprint не является универсальным транспортом mip/array/BC textures.

| Пример | Что делает код; что нельзя считать гарантией |
| --- | --- |
| ShadowMap | `MGPU-ShadowMap/HybridShadowApp.cpp:1403–1454`: проверяет старые fences, затем отправляет secondary, primary COPY и primary DIRECT без связующих waits. Один shared shadow map; старый fence не подтверждает готовность новой записи. |
| SSAO/HBAO/XeGTAO | `MGPU-SSAO/HybridSSAOApp.cpp:205–299`: независимые копии depth/normals и AO; нет токена согласованной версии входов/выхода. Опциональный CPU wait не является основным режимом (`HybridSSAOApp.h:159`). |
| Grass | `MGPU-Grass/HybridGrassApp.cpp:792–891`; `CrossAdapterGrassEmitter.cpp:826–845,1078–1087`: после готовности предыдущего batch сразу отправляется следующая запись в те же shared vertices/count. One-time ready flag не защищает последующее чтение. Same-GPU путь имеет Compute↔Direct waits. |
| SSR / probes | `MGPU-SSR/ReflectionRenderer.cpp:543–597,2084–2122,2467–2516`: skip-if-busy, постепенный расчёт probe faces; fence защищает frame-resource reuse, но не гарантирует неизменность передаваемой версии. |
| Particles | `MGPU-Particles/HybridParticleApp.cpp:250–328`: `ID3D12Fence::Signal` вызывается **с CPU** в :291/:328. Он не подтверждает завершение ранее отправленных GPU-команд. |
| Fluid | `MGPU-FluidSimulation/HybridParticleApp.cpp:299–322`: накапливает dt при busy; после submit сразу записывает primary copy без зависимости готовности. Комментарий о завершении secondary не создаёт ordering. |
| DOF | `MGPU-DOF/DOFApp.cpp:200–204,852–923`: N→N+1; две очереди сигналят одинаковое value одного shared fence (:884/:894). Такой value не различает input-ready и DOF-ready. |
| Noise / clouds | `MGPU-Noise/HybridNoiseApp.cpp:276–369`: присутствуют и polling, и вариант с queue Signal/Wait; :380–435 калибрует размер текстуры по FPS, то есть меняет качество. |
| SimpleSample / SFR | `MGPU-SimpleSample/App.cpp:84–129`; `MGPU-SFR/SFRApp.cpp:1325–1444`: явные межочередные и межадаптерные waits. Утверждение «во всём проекте нет ожиданий» неверно. |
| Animations | `MGPU-Animations/SkinnedMeshApp.cpp:2316–2383,471`: slot-shaped обмен и shared fence; `mLastFinishedWriteIndex` обновляется сразу после submit, а не после наблюдения completion. Требуется отдельная проверка retirement. |
| CubeMap / Reflection / UI / Atmosphere / MotionBlur | Также присутствуют локальные frame waits и копирование доступных ресурсов: `MGPU-CubeMap/HybridCubeMapApp.cpp:995–1018,1417–1456`; `MGPU-Reflection/ReflectionRenderer.cpp:998–1077`; `MGPU-IMGUI/HybridUIApp.cpp:36–56,252–284`; `MGPU-Atmosphere/HybridAtmosphereApp.cpp:245–325`; `MGPU-MBlur/HybridMBlurApp.cpp:203–282`. Наличие CPU frame wait само по себе не доказывает межадаптерную согласованность. |

В исходном перечне также есть `SingleGPU`, `GPU-CopyTest`, `Graphics`, `Common`, `Utils`, `Allocator`. Compute shader внутри DIRECT list не означает async COMPUTE queue.

Найденные нарушения — отсутствие необходимых ordering guarantees в проверенных путях, CPU publication вместо GPU completion и неоднозначная принадлежность fence value. Видимые артефакты, частота гонок и device removal **не воспроизводились**. Полезные идеи примеров: skip-if-busy, независимая частота producer, локальная копия последнего результата, incremental probe updates. Их следует реализовать с новым безопасным протоколом публикации.

## Что добавляют статьи

| PDF в `D:/Preentation/Articles` | Страницы PDF и вывод для задания |
| --- | --- |
| `2025-Graphicon HAO.pdf` | 7–8: явная **same-frame** схема GPU A DIRECT→A COPY→GPU B DIRECT→A COPY→A DIRECT. Wait стоит перед потребителем, независимый рендер продолжается. На p4 отдельно допускается небольшая задержка AO, но не описаны возраст/reprojection/ownership. Схема p7 визуально проверена; она отличается от текущего `MGPU-SSAO` кода. |
| `2025-Graphicon Clouds.pdf` | 5–7: при старте подбирается разрешение текстуры, выравнивающее время GPU; оно фиксируется на запуск. P8 признаёт компромисс детализации. Это настройка **качества**, не протокол безопасности общей памяти. |
| `2025-КМУ_Cloude-Noise.pdf` | 1 и последующие разделы: предварительная оценка производительности и масштабирование текстуры для уменьшения артефактов. Не подтверждает zero-wait snapshot transport. |
| `2025-Graphicon Shadows.pdf` | 3–4: карты передаются **по готовности**, основной GPU параллельно выполняет независимые проходы. P8: ближние каскады на primary, дальние на secondary — дальнейшая работа. P7 содержит несогласованность: 4.55→4.88 ms подписано как положительный выигрыш; эти проценты не использовать. |
| `2025-КМУ_Hybrid-CSM.pdf` | 3–4: RHI/Mid/High layers, SparseMap и кольцо из трёх `RenderHandle<Texture>`. Эти типы не найдены ни в текущем PEPEngine2, ни в достижимой истории Apps/Engine; это отдельный архитектурный референс, не найденная реализация AMorunov. Количество слотов само по себе не заменяет completion/retirement. |
| `2025-Graphicon SSR.pdf` | 6–8: пересылка GBuffer/lighting, использование результатов позже; явно описана задержка **два кадра**. Предлагаемая адаптация RayStepLength/MaxDistance меняет качество. Камера, source frame и temporal history должны сопровождать результат. |
| `2026-Вестник-UI.pdf` | 6–10: UI render-to-texture и поздняя композиция; потенциальная польза на определённых парах GPU. Это не основание переносить интерактивный UI Bistro с дополнительной задержкой в первую итерацию. |
| `2026-ИПС-1.pdf` | 3 (печатные 165–166): независимость, компактный output и достаточная вычислительная стоимость как критерии offload; clouds scaling. 5 (169–170): неблокирующая до потребителя синхронизация, адаптивная частота, caching и экспериментальная методика. Общие заявления статьи не подтверждают наличие соответствующего регулятора в выбранном commit. Страница 5 визуально проверена. |

Численные результаты статей относятся к другим сценам, аппаратным парам и иногда другому качеству. Не переносим проценты в критерий успеха PEPEngine2. Особенно разделяем fixed-quality и quality-adapted сравнения.

## UI-Framework: уточнённый референс пользователя

`Zakgard/Multi-GPU-UI-Framework` на указанном SHA содержит исследуемые эффекты и перенос UI целиком. CPU строит ImGui/draw data и двигает animation time, выбранный GPU вычисляет textures эффектов и растеризует UI, primary копирует итоговую RGBA8 texture и показывает экранный quad. Отдельного CanvasId/world-space consumer нет. [Rendering](https://github.com/Zakgard/Multi-GPU-UI-Framework/blob/a0a80c03438c5315afccfc5cdf2e91cfa488dc9f/MGPU-IMGUI/HybridUIApp.cpp#L248-L317), [device switch](https://github.com/Zakgard/Multi-GPU-UI-Framework/blob/a0a80c03438c5315afccfc5cdf2e91cfa488dc9f/MGPU-IMGUI/UILayer.cpp#L801-L816).

В текущем коде нет runtime LOD enum/profile switch. Параметры заданы константами: gradients 128² (`UILayer.cpp:549–550`), HP texture 2048² (:614–615), blur один проход radius 5 (:426/:432), gradient update loop с 30 вызовами `Update` (:740–743). HP shader считает `70 + floor(rand*150)` звёзд, а UI helper имеет отдельное default 25: это разные нагрузки. Итоговая texture соответствует размеру окна. [Создание ресурсов/параметры](https://github.com/Zakgard/Multi-GPU-UI-Framework/blob/a0a80c03438c5315afccfc5cdf2e91cfa488dc9f/MGPU-IMGUI/UILayer.cpp#L505-L660), [HP shader](https://github.com/Zakgard/Multi-GPU-UI-Framework/blob/a0a80c03438c5315afccfc5cdf2e91cfa488dc9f/MGPU-IMGUI/Shaders/HPBarComputeShader.hlsl#L81-L95).

Статья UI p7–8 задаёт экспериментальные Low/Medium: 20/40 звёзд, UI workload 1920×1080/2560×1440 при выводе 1920×1080; Low также упрощает детализацию и анимацию. Точные числа всех упрощений не раскрыты. Новый CVar должен превратить это в явную фиксированную таблицу и менять реальные GPU параметры; готовый runtime API из опубликованной ревизии заимствовать нельзя. Существование другого исторического варианта этим не опровергается.

`RenderEffects` отправляет COMPUTE (:765), но следующий DIRECT не получает соответствующего ready token. Secondary export→primary COPY→primary DIRECT также не связаны нужными readiness waits. CPU recycling проверяет `PrimeCopyFenceValue` на Graphics timeline, а secondary ждёт `PrimeRenderFenceValue`. Это ошибки принадлежности токенов в проверенных путях, не воспроизведённые runtime failures. [RenderEffects](https://github.com/Zakgard/Multi-GPU-UI-Framework/blob/a0a80c03438c5315afccfc5cdf2e91cfa488dc9f/MGPU-IMGUI/UILayer.cpp#L731-L765), [recycling](https://github.com/Zakgard/Multi-GPU-UI-Framework/blob/a0a80c03438c5315afccfc5cdf2e91cfa488dc9f/MGPU-IMGUI/HybridUIApp.cpp#L36-L56).

Alpha blend задан как `Src=ONE, Dest=SRC_ALPHA`; переносить его без явного color/alpha контракта нельзя. Смена устройства использует глобальный Flush и пересоздание backend — не готовый per-canvas lifecycle. [Blend](https://github.com/Zakgard/Multi-GPU-UI-Framework/blob/a0a80c03438c5315afccfc5cdf2e91cfa488dc9f/Common/RenderModeFactory.cpp#L151-L168). Эффекты и разделение CPU/GPU работ полезны как референс; синхронизация и публикация внедряются через общий новый transport. Сборки/запуски этого репозитория не выполнялись.

## Проверка D3D12-контрактов

- Queue `Wait` возвращает управление CPU сразу; GPU останавливает **последующие работы этой очереди** до достижения значения. Внутрь уже отправленного command list такой обычный queue wait вставить нельзя: нужны границы submissions. [Microsoft: ID3D12CommandQueue::Wait](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-wait).
- Межочередная зависимость задаётся fence конкретного producer. COPY↔DIRECT/COMPUTE требует корректного COMMON handoff для используемой legacy resource-state модели; fence не заменяет resource barriers. [Microsoft: Multi-engine synchronization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization).
- Cross-adapter shared heaps имеют ограничения layout/flags/памяти; нужны cross-adapter fences и barriers. Нельзя считать shared resource обычной локальной texture или обещать быстрый P2P между любыми GPU. [Microsoft: Shared heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps), [Fence flags](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_fence_flags).

Сборки, GPU validation, сравнение качества и производительности в этом исследовании не запускались. Все рекомендации пригодности техник — гипотезы по зависимостям, которые должны пройти проверки из задания.
