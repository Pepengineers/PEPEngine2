# Задание: MGPU и DIRECT / COMPUTE / COPY в PEPEngine2

Дата: 2026-11-08. Согласованные в интервью решения и проект технического контракта; это задание на последующую разработку, не реализация MGPU. [Исследование исходников и статей](../research/2026-11-08-mgpu-source-audit.md) содержит версии и доказательства. [Пакеты для чатов Sol/high](../tasks/mgpu-integration-work-packets.md) задают порядок исполнения и владельцев файлов.

## Результат, который требуется получить

Общая инфраструктура двух независимых D3D12-адаптеров, каждый с DIRECT, COMPUTE и COPY, затем подключение **воды, облаков/окружения, теней, AO, травы и UI**. Состав pipeline остаётся у приложения, исполнение и GPU lifetime — у runtime согласно [ADR-0010](../adr/0010-renderer-and-application-feature-ownership.md).

Одна логическая модель зависимостей с placement `(adapter, queue)`, независимыми producer-подграфами и несколькими физическими потоками submissions. Два физических устройства не требуют двух изолированных графов с ручными fences в приложении. Допустимы внутренние per-device планы и отдельные экземпляры запуска подграфа, но у них один контракт зависимостей и владения ресурсами.

Два режима потребления: точная требуемая версия и последняя завершённая допустимая версия. Их применимость задаёт техника. Ручные настройки и профили платформы определяют placement, свежесть и частоту обновлений. Автоподбор частоты, разрешения и качества — последующая задача после измерений; автоматического выравнивания всех техник по медленному GPU нет.

55 публикаций облаков в секунду при 60 показанных кадрах могут быть приемлемы. Это не универсальная норма: оцениваются возраст, неравномерность поступления, движение камеры и скорость изменения самой техники. Частота публикаций не равна скорости течения симуляционного времени.

## Согласование и дерево решений

| Узел | Состояние |
| --- | --- |
| Цель: инфраструктура → техники → измерения | Принято пользователем, Q1. |
| Допускается старое завершённое состояние; строгий режим тоже нужен | Принято, Q2. |
| Источник конференционной работы | Q3: AMorunov, возможно merge; найден merged `MoreRenderFeatures`. Точная launch-конфигурация неизвестна, это ограничение доказательств, не блокировка архитектуры. |
| Один логический граф с независимыми подграфами | Принято, Q4. |
| Универсальная частота / единое поведение медленного GPU | Пользователь отверг универсальное решение, Q5: зависит от платформы и техники. |
| Ручные настройки и профили, автоматическая адаптация позже | Принято, Q6. |
| Первая группа техник | Q7: вода, облака, тени, AO, трава; последующее уточнение добавляет UI, half-resolution AO и выбор источников теней. Все шесть входят в результат; частицы не подменяют этот список. |
| Действие при превышении заданного возраста | Принято, Q8: повторять последний содержательно допустимый снимок и показывать превышение возраста. Без автоматического переключения GPU, снижения качества или включения строгого ожидания. |
| World-space UI и LOD | Принято, Q9 и уточнение: только отображение/анимация, без разработки ввода; перенос всего UI или отдельного canvas; ручной CVar качества/LOD для сравнения производительности. |

Числа target Hz и возрастов не выдумываются для всех устройств: пакет интеграции определяет явные ручные параметры, пакет измерений выпускает проверенные профили конкретных пар адаптеров. Новая техника не включает MGPU автоматически.

В `LatestCompleted` заданный возраст — диагностический порог, не жёсткий TTL. Его превышение не отменяет выбранный режим. Несовместимость epoch/формата/камеры — отдельная проверка валидности: такой результат применять нельзя даже при малом возрасте; используется определённый локальный baseline/нейтральный результат. Требование обязательно свежего состояния выражается через `ExactVersion`.

## Архитектурный контракт

### Placement и зависимости

Расширить существующий `GDX12RenderGraph`, сохранив классы passes, declaration API, RAW/WAR/WAW проверки, culling/forwarding и single-GPU fallback. Заменить смысл одиночного `AsyncCompute` на выбор поддерживаемого adapter/queue; старый API может делегировать новому на время миграции. Не создавать второй общий render engine.

Проход объявляет входную версию, выходы, GPU accesses и допустимые очереди. Compile строит submissions, переходы и waits. Фактический completion token содержит **идентичность device/queue/fence и UINT64 value**, а не только номер кадра. Числа разных fences никогда не сравниваются как одна временная шкала. Каждый сигнализируемый timeline имеет одного владельца-очередь; imported fence на другом устройстве — view того же timeline.

COPY — полноценная цель планирования. Допустимые команды и states проверяются для выбранного типа list. Device-local экземпляры одного передаваемого результата имеют общий логический transfer identity, но разные ресурсы, descriptors и state tracking. Поле `SharedResource` существующего графа остаётся логической зависимостью pipeline; оно не становится именем cross-adapter heap.

Обычные queue Wait/Signal используются на границах submissions. DIRECT может выполнить независимый prefix, затем поставить Wait и выполнить потребителя. CPU не обязан ждать. Нельзя вставить такой Wait посередине уже submitted command list. [Контракт D3D12](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-wait).

### Строгая цепочка: AO как опорная проверка

```text
GPU0 DIRECT : depth/normal → release → signal D0
              независимые shadows / GBuffer → wait C0out → lighting(AO)
GPU0 COPY   : wait D0 → export inputs → signal C0in
GPU1 COPY   : wait C0in → import inputs → signal C1in
GPU1 COMPUTE: wait C1in → AO → release → signal K1
GPU1 COPY   : wait K1 → export AO → signal C1out
GPU0 COPY   : wait C1out → import AO → signal C0out
```

Это зависимости, а не обещание одновременного физического исполнения всех очередей. Input/output transport также имеет обратные release-зависимости перед повторным использованием слотов, не показанные в короткой схеме. Перед экспортом depth/normals определить поддерживаемое линейное представление: не пытаться напрямую шарить произвольный depth-stencil layout.

HAO, PDF p7, уже показывает сокращённый вариант с DIRECT второго GPU; расширение до его COMPUTE/COPY проверяет требуемый общий механизм. На одной COPY queue порядок входящего и исходящего обмена должен сохранять топологию: заранее поставленный wait на далёкое событие не должен блокировать нужные для него же копии.

### Свежесть результата и независимый запуск

| Режим | Контракт |
| --- | --- |
| `ExactVersion` | Потребитель требует конкретный generation/source step. GPU ждёт готовности и импорта перед первым чтением; CPU wait не является обычным способом связи. Для same-frame AO нужны текущие согласованные depth, normals и camera. |
| `LatestCompleted` | В начале подготовки кадра выбирается согласованный **уже импортированный локальный** снимок. Более новый producer/transfer может идти параллельно. Кадр не ставит wait на незавершённый новый снимок только ради его свежести. Выбор и metadata неизменны для всех passes этого кадра. |

Готовность shared export и готовность локального import — разные события. Если новый import ещё в полёте, потребитель продолжает использовать предыдущую локальную версию. Нельзя читать «текущий shared buffer» без резервирования и completion.

Первый кадр имеет определённый локальный baseline/нейтральный результат техники; uninitialized shared memory не показывается. После reset/resize, camera cut или смены алгоритма старые результаты принимаются только при совместимости epoch и входных revisions. Техника определяет, какие изменения делают предыдущую версию недопустимой.

Независимый producer-подграф запускается по своему расписанию и входному snapshot, а не обязательным проходом каждого Present. В первом варианте достаточно bounded очереди: один последовательный stateful job на технику, отсутствие free slot означает пропуск новой публикации/запуска по правилам техники. Не создавать perpetual CPU polling threads для каждой GPU queue.

Пропуск относится к необязательной публикации Latest. Для Exact востребованное поколение и необходимые slots резервируются **до** выдачи completion token потребителю. При нехватке места применяется backpressure/отложенный submit; требуемое поколение нельзя отбросить и затем поставить wait на невыполнимый сигнал. Отмена/ошибка разрешается явно до зависимого submit. Освобождающий slot reader не должен зависеть от будущей работы, требующей этот slot.

Время симуляции берётся из application simulation clock. Сохраняются существующие pause, time scale, stable timestep и bounded catch-up. Если запуск пропущен, elapsed time нельзя ни молча потерять, ни передать solver одним неограниченным dt. Показывать simulation debt отдельно от задержки доставки. Повторный render snapshot не означает повторный шаг симуляции.

Пропуск output-публикации не разрешает терять одноразовые входные события. Reset/импульсы и другие события техники применяются ровно один раз; coalescing непрерывных входов допустим только по её контракту. UI visual animation может иметь свой существующий application clock, не обязанный совпадать с физическим временем воды.

У текущей воды `GDX12FluidPass.cpp:339–340` обрезает elapsed до `safeStep*MaxSubsteps`. W обязан учитывать время пропущенных invocations и остаток снаружи solver, выдавая ограниченные шаги и сохраняя его safe step/математику. При исчерпании bounded catch-up отброшенное время явно учитывается в `droppedSimulationTime`; timestamp снимка отражает реально продвинутое состояние. Тест 60/30 Hz должен давать одинаковую симуляционную последовательность при достаточном бюджете и явный debt/drop при перегрузке. Передачи накопленного dt в нынешний clamp недостаточно.

### Протокол владения снимками

Снимок включает payload и согласованные metadata: generation/epoch, source inputs revision, simulation time, producer sequence; камера/матрицы/счётчики/indirect arguments включаются там, где они определяют смысл payload. Не собирать из разных поколений positions/count, shadow map/light matrices или cubemap/IBL coefficients.

Начальный bounded pool — три transfer slots и три consumer-local slots, с общей проверкой доступности; число можно изменить после оценки памяти. **Три слота не являются доказательством безопасности**: при занятости всех слотов работает backpressure, а не modulo overwrite. Два пула не обязаны продвигаться синхронно и не являются swapchain frame slots.

1. Producer резервирует свободный shared slot. COMPUTE/DIRECT завершает расчёт; COPY экспортирует согласованные данные и queue-сигналит `ready` после экспорта. Локальный solver buffer тоже защищён до окончания чтения COPY: следующий шаг не перезаписывает его преждевременно.
2. Latest выбирает допустимую готовую версию и резервирует её до consumer copy. Exact резервирует требуемую версию и связывает копию с реальным producer-ready token, включая ещё незавершённую работу. Перезапись зарезервированного для чтения slot запрещена. Отбросить Ready slot можно только если он никем не захвачен, не востребован Exact-потребителем и его writer завершён.
3. Consumer COPY импортирует в свободный локальный slot, сигналит import completion и release shared slot после окончания чтения. Producer получает право повторного использования shared slot только по этому release, а не по факту CPU submission.
4. В `LatestCompleted` runtime продвигает displayed version только после завершения local import. В `ExactVersion` добавляет зависимость нужного потребителя от import completion.
5. Локальный slot живёт до последнего GPU reader и до использования как previous displayed state. Освобождение учитывает все очереди читателей или доказанный транзитивный join. Несколько читателей не могут сигналить общий timeline несогласованными values.

Протокол должен сохраняться при producer быстрее/медленнее consumer, skipped frames, paused rendering и нескольких кадрах в полёте. CPU `ID3D12Fence::Signal` не публикует завершение GPU-команд. `GetCompletedValue()==UINT64_MAX` при device removal не трактуется как успешное завершение всех работ.

### Transfer и resource states

Первый транспорт: локальный producer resource → shared staging → локальный consumer resource. Буферы и texture footprints имеют явный размер, alignment, format, subresource range и checked арифметику. Для неподдерживаемых texture layouts использовать линейный shared buffer с `GetCopyableFootprints` и `CopyTextureRegion`; ограничения совместимости проверять на обоих устройствах. Cube faces/mips передаются как набор subresources, не как заведомо допустимая shared cubemap. API не должен обещать поддержку всех форматов без тестов.

Для cross-adapter buffers использовать корректный placed-resource/shared-heap путь; не переносить ограничения legacy single-texture helper. Обычные UAV/render targets остаются в локальной VRAM. Imported resources, fences и handles освобождаются владельцем после окончания всех пользователей. [Ограничения shared heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps).

Оставить используемую проектом legacy state model: правильные release/COMMON/acquire переходы для COPY, UAV barriers между зависимыми dispatches, совместимые read-state unions. Wait обеспечивает порядок, но не заменяет переходы. Перенос на Enhanced Barriers или preview fence barriers не нужен для выполнения задания.

### Retirement и deadlocks

Текущий final DIRECT join всех COMPUTE безопасен только внутри frame-bound работы. Для независимых producers нужны собственные allocator/upload/descriptor/resource slots и retirement tokens; их нельзя освобождать по Present fence или убирать финальный join без замены lifetime-контракта.

Transient pooling остаётся per-device. Для пересекающихся очередей/кадров сначала запретить alias/reuse без доказанного happens-before; оптимизацию памяти делать позже по измерениям. Состояния imported aliases не выводить из сравнения native pointers разных устройств.

Проверять расширенный wait graph с FIFO-порядком очередей и межкадровыми slot credits: ацикличности исходного pass DAG недостаточно для исключения deadlock после разбиения на submissions. Длинный фоновый dispatch не отправлять перед срочным compute-потребителем без оценки задержки. Не ставить в shared COPY queue ожидание ещё не готового фонового producer в `LatestCompleted`, блокируя независимые uploads.

Resize/reset/toggle: остановить новые submissions старого epoch, дождаться/retire только относящиеся к нему работы, затем пересоздать и отклонять поздние результаты старого epoch. Startup capability failure возвращает рабочий single-GPU путь с причиной. При device removal — диагностируемая остановка MGPU-пути без вечного ожидания; полное восстановление потерянного D3D12 device не является скрытым требованием этого этапа.

## Техники: текущее состояние и план

В текущем checkout активного MGPU placement нет ни у одной техники. Существующие четыре async CVars default-off. «Есть compute shader» и «есть async queue» здесь различаются.

| Техника | Async сейчас | Первый MGPU-контракт и оставшаяся работа |
| --- | --- | --- |
| **Вода** | Solver уже COMPUTE opt-in | Secondary COMPUTE solver + COPY immutable state; surface/filter/resolve primary. Exact и Latest; сохранить dt/reset/pause, состояние контейнера и previous **displayed** positions. Не читать solver ping-pong, который уже перезаписывает следующий шаг. |
| **Облака / окружение** | Async declaration отсутствует | Выделить cloud generation и compute LUT/filter stages из монолитного Environment; сначала local async parity, затем secondary producer. Публиковать согласованный комплект облака/неба/зависимых IBL данных, обновляя лишь реально dirty части. Latest подходит для медленных изменений; epoch invalidate для смены параметров. |
| **Тени** | Raster DIRECT; culling compute внутри прохода | Secondary DIRECT shadow capture, COPY результата и matrices/light/cascade revision. Переносить солнце (каскады) и локальные источники: весь набор, все локальные либо конкретный выбранный источник по стабильному ID; неперенесённые остаются primary. Сохранить существующие shadow algorithms. Реплицировать scene/geometry/material alpha-test/transforms. Exact первым; Latest только при explicit validity. Разделение отдельных каскадов между GPU — дальнейшее расширение, не замена выбора источников. |
| **AO** | Все текущие AO implementations поддерживают COMPUTE opt-in | Добавить независимый режим **половины линейного разрешения**, затем local/secondary исполнение. Same-frame Exact: согласованные depth/normals→AO→depth/normal-aware upsample→primary lighting. Поддержать все существующие AO modes, early/late bindings и history/reset. Stale AO с другой камерой без reprojection не разрешать. |
| **Трава** | Interaction/wind preparation уже COMPUTE opt-in; expansion DIRECT | Передавать компактное поле wind/interaction и matching metadata. Expansion/cull/draw оставить primary, сохранить связь с fireflies. Exact/Latest по профилю и допустимому возрасту реакции. Не пересылать expanded vertices каждого view без измеренного основания. |
| Частицы | Simulation COMPUTE opt-in | Следующая группа: текущий primary depth и grass field должны иметь согласованные revisions; positions/count/indirect и previous displayed state передаются вместе. |
| Reflection probes | Нет async | Следующая группа: secondary DIRECT capture + COMPUTE filtering + COPY публикация **полного** probe; репликация scene inputs и атомарный набор faces/mips. Переиспользовать инфраструктуру теней. |
| Skinning / culling | Нет async; skinning до графа | Сначала graph nodes и явные buffer accesses; MGPU только после оценки объёма geometry transfer и ранних потребителей. |
| Deferred lighting | Нет async | Local COMPUTE-кандидат после аудита GBuffer/AO/shadow dependencies; offload всего GBuffer bandwidth-heavy, в первую группу не входит. |
| SSR / DoF / motion blur | Нет async declaration | Local async и temporal MGPU — отдельные задачи. Нужны camera/history/source frame и критерии задержки; два кадра из SSR-статьи не являются автоматически приемлемыми для Bistro. |
| Water surface / transparency / GBuffer | Mixed/raster DIRECT | Сохраняются primary; stage split возможен после измерений. Нельзя объявить весь raster pass async compute. |
| **UI** | Primary DIRECT вне графа; GPU effects и world canvas предстоит добавить | Перенос всей UI-композиции либо конкретного world-space canvas/анимированного плаката. Secondary выдаёт законченную texture с alpha/цветовым контрактом; primary выполняет экранную композицию или отображает её на world geometry. CPU construction сохраняется, generation и animation time определяются явно; режимы статьи проверяются отдельно от нового world-space расширения. |
| Reconstruction / FG / present | Primary / SDK-owned | Сохраняются primary. SFR/AFR и второй swapchain не нужны для UI offload. |

Файлы-владельцы, исходные anchors и конкретные проверки приведены в [пакетах работ](../tasks/mgpu-integration-work-packets.md); состояние APIs перед началом каждого пакета перепроверяется на интеграционной базе.

### Обязательные режимы AO

Full/Half выбирается отдельно от алгоритма, его quality и placement. Half означает `ceil(W/2) × ceil(H/2)`, то есть примерно четверть пикселей относительно текущего разрешения рендера сцены; это не глобальное уменьшение всей сцены. Поддержать SSAO, HBAO, XeGTAO и CACAO. Сейчас все четыре используют полную input grid; intrinsic mips/deinterleaving и имеющийся, но не включённый native CACAO downsample не выполняют это требование автоматически.

Downsample depth выбирает геометрически корректную глубину с учётом принятого reverse-Z, а normal берётся из согласованной поверхности. Константы реконструкции, bounds, jitter, history и размер dispatch соответствуют реальной grid. В Half MGPU-path reduced inputs готовятся на primary, secondary возвращает low-resolution AO, primary восстанавливает AO на lighting grid с depth/normal-aware upsample. Проверить края объектов, тонкую геометрию, нечётные/минимальные размеры и dynamic resolution. Простое изменение размера output texture неверно. CACAO нельзя уменьшить дважды случайным сочетанием native flag и общего Half режима.

### Обязательные режимы теней

Выбор размещения: все тени; только солнце; все локальные источники; один конкретный источник. Солнце использует существующие cascaded shadows, локальные — существующие spot/point режимы. Выбранный point light переносится со всеми шестью faces; оставшиеся источники продолжают рассчитываться primary. Перенос отдельного источника не должен заставлять копировать или заново рисовать весь atlas.

Источник идентифицируется устойчивым light key (`Sun` и локальный EntityId с учётом lifetime), не индексом массива/текущим slice. Публикуются matching matrices, cascade splits, light/scene revisions и mapping импортированных регионов. Удаление/создание света и пересортировка `ShadowFirstSlice` не должны подставить чужую карту. Обновление cache/static/dynamic geometry остаётся корректным. Согласованность требуется во всех потребителях теней: lighting, transparency и probes.

В статьях исследовались обычные directional shadow maps и CSM; выбор конкретного локального источника — новая функция PEPEngine2. Предложение статьи рассчитывать near cascades primary, far cascades secondary остаётся отдельным расширением, а не обязательной частью первого переноса.

Текущий state tracking относится ко всему ресурсу. Минимальная безопасная интеграция сериализует primary writes/import/reads общего atlas через graph либо использует отдельные imported textures с явным mapping. Частичный `CopyTextureRegion` не разрешает автоматически конкурентную запись/чтение разных slices; новая полная система subresource-state tracking не является скрытым обязательством S2.

### Обязательные режимы UI и LOD

Существующий экранный ImGui привязан к primary backend и рисуется после графа; world-space canvas в проекте пока отсутствует. Выделить offscreen render surface и device-owned ресурсы, затем добавить небольшую canvas-модель для отображения/анимации. Минимальная демонстрация — экранный UI и два плаката в сцене, чтобы можно было отправить весь UI либо один выбранный CanvasId на secondary. Остальные поверхности остаются primary. World canvas использует обычное material sampling/occlusion основной сцены; переносить сцену целиком ради плаката не нужно.

CPU продолжает формировать UI state, draw data и animation time; GPU эффекты, raster и offscreen composition размещаются на выбранном адаптере. Это не перенос CPU layout на видеокарту. Для GPU-эффектов поддержать secondary COMPUTE→DIRECT зависимость, затем COPY и primary consumer. Простой UI может обходиться DIRECT. Draw data, textures/fonts и per-device descriptors живут до завершения использования; HWND/PRESENT не являются свойствами offscreen canvas.

**Ввод для world canvas не проектировать и не реализовывать.** Существующий ввод экранного интерфейса сохраняется, новые hit testing, focus и 3D interaction не входят в пакет. Для существующего интерактивного экранного слоя начальный режим — Exact; Latest предназначен для визуальных/анимированных поверхностей, пока не задан иной контракт. Статья UI исследует перенос экранной композиции целиком; выбор canvas и world poster — наше расширение.

Добавить `Render.UI.Quality` как ручной выбор минимум двух фиксированных профилей **Low/Medium** (значения 0/1). Основной UI-референс — предоставленный пользователем [Zakgard/Multi-GPU-UI-Framework, `a0a80c0`](https://github.com/Zakgard/Multi-GPU-UI-Framework/tree/a0a80c03438c5315afccfc5cdf2e91cfa488dc9f/MGPU-IMGUI): gradient effects, blur, динамическая шкала/звёзды и перенос всего UI. Локальный MGPU остаётся дополнительным источником. Выбор canvas — новая функция.

В статье Low/Medium имеют 20/40 звёзд и UI workload 1920×1080 / 2560×1440 при выводе 1920×1080. В указанном UI-Framework texture sizes, star counts и blur workload пока заданы константами, готового runtime LOD enum нет. Некоторые параметры упрощения статья не раскрывает. Поэтому новый CVar оформляет воспроизводимую таблицу качества по этим материалам; он не объявляется существующим API референса. Изменение числа декоративных ImGui-звёзд не заменяет изменение фактической GPU-нагрузки HP shader: профиль должен доходить до нужных shader constants и dispatch размеров.

Пакет U1 фиксирует точную таблицу разрешений canvas/effect textures, числа элементов, blur passes/radius и shader variants. Для другого размера canvas определить масштаб и округление явно. Каждый профиль выполняет полезную визуальную работу; не добавлять холостые повторные dispatches ради демонстрации ускорения. Профиль и seed/animation sequence одинаковы в single-GPU и MGPU. Смена качества меняет epoch и безопасно пересоздаёт затронутые ресурсы; смена адаптера не меняет качество. В отчёте указывать фактические размеры и bytes/update.

В UI-Framework не прослеживаются необходимые Compute→Direct и export→Copy→draw dependencies; его CPU recycling также использует некоторые values не той очереди. Заимствуются эффекты и структура работ, а ordering, slot ownership и backend lifetime строятся на общем новом контракте. [Anchors и различия версий](../research/2026-11-08-mgpu-source-audit.md#ui-framework-уточнённый-референс-пользователя).

Alpha convention и color space определяются явно и проверяются на прозрачном/полупрозрачном UI; blend из reference не копировать без проверки. Blur собственной UI-картинки не равен backdrop blur 3D сцены: второй требует отдельного primary→secondary scene-color input и отдельного измерения. Автоматическая подстройка LOD по FPS исключена из этого задания.

## Приёмка

### Корректность и управление

- CPU/model tests: токены разных queues с одинаковыми числами; RAW/WAR/WAW и transfer alias; FIFO-induced cycle; exact/late selection; slot exhaustion; slow/fast producer; dropped publications; reset epoch; независимые frame/producer counters.
- Pattern transfer tests на buffers/textures: sequence/epoch/checksum внутри payload, metadata и readback должны совпадать. Искусственно задерживать export, import и последнего reader независимо; ни один live slot не перезаписывается.
- Проверить 1 GPU DIRECT fallback, 1 GPU D+C+Copy, 2 GPU с D+C+Copy на каждом; toggles, absent/unsupported secondary, allocation/import failure, resize, minimization, pause/reset и несколько frames in flight. WARP/model tests не заменяют две физические видеокарты.
- Extend `RenderGraphTests`, `RenderPipelineTests`, `BistroRenderGraphTests`, water/grass/AO checks. Добавить реальные secondary DIRECT и COMPUTE producers; тест только двух очередей одного GPU не подтверждает MGPU.
- Для Exact — сравнение того же шага/входов с single-GPU baseline; допустимые численные погрешности между разными GPU задаются для алгоритма. Для Latest — сравнение с baseline **того же выбранного снимка**, отдельно визуальная задержка движущейся сцены.
- Motion vectors/history: previous displayed state соответствует предыдущему показанному результату, включая повтор одного snapshot и скачок через несколько production steps. Camera cuts, движущиеся light/objects и viewport changes проверяются отдельно.
- DX12 debug layer + GPU-based validation; существующую нестабильность IBL suite из [follow-up](../research/2026-11-07-ibl-gbv-followup.md) отделять контрольным baseline, не скрывать fail как «известный» без воспроизведения.
- Bounded 10-минутный soak с искусственным producer slowdown: стабильные slot/handle/VRAM counts, нет deadlock или нарастающей очереди. Отдельно провести интерактивный просмотр переходов и устаревания шести техник.

### Производительность и профили платформ

Фиксировать commits, adapters/LUID, драйверы, настройки, camera/input/seed, одинаковые resolution/quality и simulation timeline. Сравнивать primary DIRECT, primary async и secondary execution; для фонового режима отдельно фиксировать cadence/age, для Exact — critical-path wait. Проверять техники по одной, затем попарные конфликтующие сочетания и все шесть вместе.

Метрики: pipelined frame median/p95/p99 и present throughput, CPU submit, GPU intervals по queues, transfer bytes/time, wait time, producer Hz, simulation debt, age median/p95/max, repeated/dropped snapshots и память. GPU timestamp clocks разных адаптеров нельзя вычитать напрямую: использовать clock calibration/профайлер, сохранять queue frequency. COPY timestamps включать только при поддержке capability.

Работоспособность, overlap и выигрыш полного кадра — три разных результата. Сумма queue timings и CPU-drain harness не заменяют pipelined throughput. Профилировать hardware serially, без конкурирующих GPU benchmark чатов. Пользователь не просил запускать замеры в задаче подготовки этого документа.

Успех инфраструктуры: вся матрица корректности выполнена, fallback работает, данные и память согласованы. Успех каждой из шести техник: заявленные placement-возможности работают с сохранением её семантики; отчёт честно фиксирует выигрыш, отсутствие выигрыша или regression и допустимый возраст. Default-on и обещание ускорения всех техник не являются условием этого этапа. Для профиля, рекомендуемого к использованию, преимущество подтверждается на его конкретной паре GPU при оговорённом качестве/задержке.

Полное завершение разработки требует всех шести technique adapters, интеграционной матрицы, документации controls и отчёта. Выпуск одной воды не закрывает всё задание. Реализация не начинается автоматически в чате, который только подготовил это задание.
