# Реализация техник рендера

Требования: [согласованный документ](../research/2026-10-31-technique-migration.md).
Пользователь подтвердил реализацию 2026-11-01: сначала база и CVar, затем компоненты,
без новых объектов в сцене. Коммиты нужны для каждой проверенной итерации.
Разрешено включать затронутые файлы целиком вместе с прежними правками; остальные
незакоммиченные изменения не включаются автоматически.

## Проверенные итерации

### Настройки техник

- Коммит `ab061f9`, `feat(render): add validated technique settings`.
- Общие типы качества/выбора AO, параметры AO и motion blur; режимы Off, presets
  и ручные переопределения, которые не стираются изменением качества.
- `TechniqueTests` Release/x64 прошёл: все три AO, Off, монотонность бюджета,
  независимость override, отказ на неверных enum/радиусе/NaN/числе samples.
- Motion blur settings пока не означают готовый motion blur pass.

### Три AO и интеграция

- Коммит `e266912`, `feat(render): add switchable SSAO HBAO and XeGTAO`.
- Реализованы отдельные SSAO, HBAO и Intel XeGTAO; единый выход visibility.
- Исправлены prefilter dispatch и reduction conditions старого XeGTAO, обработка
  denoise/packed scale, RH/world-normal/reverse-Z mapping и нечётных размеров.
- AO подключён после G-buffer к ambient lighting; direct/emissive/baked сохраняются.
  Прозрачный forward-путь пока использует neutral AO.
- `AOGpuSmoke` Release/x64 прошёл на RTX 3060 Laptop с DX12 debug layer:
  65x49, три frame slots, плоскость, ступень глубины, denoise 0/1/4, Off,
  конечные значения [0,1], отсутствие DX12 errors.
- Обнаруженное затемнение XeGTAO у угла незатенённой плоскости подтверждено
  независимым расчётом donor equations: 0.917647 при настройках smoke.
  Центр равен 1. Это исходная угловая аппроксимация, а не исправленная постфактум
  ошибка матрицы; тест отдельно проверяет нейтральный центр и известный baseline.
- `LightingGpuSmoke` прошёл: IBL, shadow depth/кэш, ExecuteIndirect и deferred
  lighting readback. Для него заполнены projection/common data нового AO.
- Release-сборка `Apps/App/App.vcxproj` прошла; `verify_scene.py`: 4 tests OK.
- Существующая Bistro загружена и отрисовала один настоящий кадр через уже
  имеющийся loading benchmark, exit 0. Report: `Build/ao-bistro-frame.json`.
  Время 42.37 s до готового кадра — единичный прогон, не performance baseline.
  Новые объекты для этого не создавались. Скриншот/длительное движение не проверялись.
- Логи: `Build/ao-gpu.log`, `Build/lighting-ao-gpu.log`, `Build/app-ao-build.log`.

### Адаптеры FidelityFX и воспроизводимая зависимость

- Коммит `bb5a28a`, `feat(render): add FidelityFX pass adapters`.
- Реально загружен `v1.1.4`, commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`.
  Рабочий sparse checkout: `Build/FidelityFXSource`, не часть продуктового кода.
- Собраны Release SSSR, DoF, Reflections Denoiser, DX12 backend и shader permutations.
  CMake build: `Build/FidelityFXBuild`, log `Build/fidelityfx-build.log`.
- `MSBuild/BuildFidelityFX.ps1` проверяет точный commit SDK, восстанавливает
  необходимые исходники/инструменты и проверяет выходные библиотеки. Обычная
  сборка renderer вызывает bootstrap; демонстрационный Cauldron не используется.
- Добавлены реальные SSSR/DoF passes, CVar, PrepareFrame для безопасного rebuild
  контекстов и сброс cache native state после внешнего SDK dispatch.
- Авторитетный `FidelityFXGpuSmoke` Release/x64 прошёл: нечерный SSR environment
  fallback после инициализации history, reset/resize/quality/Off, sharp focus и
  реальный blur DOF, точный HDR bypass, обычный AO после SDK, отсутствие DX12 errors.
- У закреплённого SDK первый SSSR dispatch выдаёт чёрный radiance даже с history
  weight=0; второй уже ненулевой. HasValidHistory явно сообщает готовность со
  второго dispatch и сбрасывается при reset/Off/resize. Resolve должен сохранить
  environment/probe fallback до готовности; угадывать confidence по alpha нельзя.
- SSSR screen-space ray-hit fixture пока не проверен. Граф App на этой итерации
  ещё не вызывает эти passes; их подключение и общий specular resolve — следующий шаг.
- Логи: `Build/fidelityfx-integration-build.log`, `Build/fidelityfx-gpu.log`.

### Motion blur

- Коммит `368664d`, `feat(render): add feature-aware motion blur`.
- Реализованы TileMax → NeighborMax → двухнаправленный gather по Guertin,
  McGuire, Nowrouzezahrai, HPG 2014; равное распределение samples без TileVariance.
  Неоднозначность направления в Appendix разрешена по нормализованной конструкции §4.1.
- Полувектор экспозиции ограничивается по длине; depth comparisons используют
  обратную положительную глубину; небо получает rotation-only camera motion.
- Off, нулевой shutter, cut, первый кадр включения и resize точно копируют HDR.
- Авторитетные Release-сборка и `MotionBlurGpuSmoke` прошли с DX12 debug layer:
  нечётные 65x49, HDR > 1, два тонких перпендикулярных moving features,
  симметрия знака скорости, constant color, вращение/перенос камеры для неба,
  все quality и frame slots, отсутствие DX12 errors.
- Первый прогон обнаружил ошибку fixture: sky-проверка попадала на предусмотренный
  cold-frame bypass после восстановления shutter. Тест теперь проверяет bypass
  отдельно и blur на следующем кадре; production shader из-за этого не менялся.
- Логи: `Build/motion-blur-build.log`, `Build/motion-blur-gpu.log`.
  Pass ещё не подключён к графу App; проверка Bistro и стоимость остаются впереди.

### Упрощение готовых проходов

- Коммит `faad732`, `refactor(render): simplify effect pass state`.
- Выполнены отдельные read-only reviews reuse, quality, efficiency, tests и comments.
- Motion blur использует существующий CopyResource helper и один флаг WasEnabled
  вместо дублирующего History. Одинаковый Halton jitter вычисляется один раз
  на группу из четырёх taps, с сохранением формулы и порядка выборок.
- Единицы sensor width выражены именем переменной; удалены три комментария,
  повторявших код или историю переноса. Алгоритмические/API/vendor constraints сохранены.
- Proof tests оставлены без изменений. Повторные fills нейтрального AO/SSR в Off
  пока сохранены: дополнительный persistent-state cache отложен до измерения стоимости.
- Проверка: Release builds и GPU smoke трёх проходов; логи `Build/simplify-*-{build,gpu}.log`.

### Temporal SSR и попадание лучей

- Коммит `ed261a6`, `fix(render): align SSR history with jitter`.
- SSSR получает собственный RG16F motion с jitter delta; общий unjittered motion
  сохраняется для XeSS и motion blur. Denoiser использует предыдущую jittered VP.
- Знак проверен проекцией точки: у RH row-vector projection экранный сдвиг равен
  минус добавке jitter в projection, поэтому current-to-previous NDC поправка
  равна `(2*(current.x-previous.x)/width, -2*(current.y-previous.y)/height)`.
- Release `FidelityFXGpuSmoke` прошёл с проверкой production motion kernel,
  сохранения исходного motion и настоящего попадания screen-space rays.
  Изменение красной вертикальной стенки на зелёную при неизменном environment
  изменило radiance в отражающей области: red delta 367.869, green delta 322.359.
- Старые focus/blur/Off/history/resize/state-restoration проверки также прошли.
  Логи: `Build/hdr-FidelityFXGpuSmoke-{build,gpu}.log`.

### HDR-граф базовых эффектов

- Коммит `e5dc247`, `feat(render): integrate HDR post effects`.
- App исполняет deferred → SSSR/specular replacement → linear HDR OIT → XeSS →
  aligned depth/motion → motion blur → DoF → final Reinhard/sRGB → backbuffer.
- Specular resolve заменяет старое environment specular, применяя общий DFG/AO
  helper один раз; direct/diffuse/emissive/baked и cold/Off fallback сохраняются.
- Все PrepareFrame вызываются до получения command lists; camera/resize меняют
  HistoryRevision. CPU matrices заполнены с явным разделением jittered и unjittered.
- XeSS принимает RGBA16F HDR, сбрасывает историю и инвалидирует native state cache.
  CVar SSR/DOF/motion blur опубликованы в Config/Engine.ini и console documentation.
- `PostProcessGpuSmoke` Release прошёл: HDR OIT > 1, BRDF once, точный Off/cold,
  baked bypass, нечётные размеры, discontinuity-safe point depth/motion alignment,
  независимый RH jitter oracle, настоящий XeSS HDR/history/reset/resize и tone mapping.
- `BistroEffectsSmoke` прошёл: десять настоящих кадров существующей Bistro,
  переключение эффектов и Low/High, перенос/вращение камеры, cut, DX12 debug layer.
  Новые объекты не создаются, существующая камера меняется только в тестовом процессе.
- `LightingGpuSmoke` прошёл; Release App собран; `verify_scene.py`: 4 tests OK.
  Логи: `Build/hdr-{PostProcessGpuSmoke,BistroEffectsSmoke,LightingGpuSmoke}-{build,gpu}.log`,
  `Build/hdr-App-build.log`.
- Ограничения: point aux не восстанавливает субпиксельную геометрию; OIT использует
  opaque depth/motion и не описывает отдельный фокус/скорость стекла. Визуальный
  capture и сравнение производительности двух порядков с XeSS ещё не выполнены.

### Захват cubemap-проб

- Коммит `d4abaaf`, `feat(render): add cached reflection capture`.
- Добавлены ReflectionProbeComponent, submission в обеих update chains и настоящий
  capture producer. В сцене пробы не создаются; GPU-ресурсы выделяются лениво.
- Dynamic снимает opaque/masked геометрию; BakedStatic копирует статический HDR
  color и matching reverse-Z depth, затем дорисовывает только dynamic instances.
  Существующие FlyingProps помечены dynamic, без новых entities.
- Собственная directional shadow map охватывает capture volume независимо от
  main-camera CSM. Source-only IBL исключает рекурсивное отражение проб/SSR.
- GGX prefilter явно читает mip 0; публикация всех faces/mips и capture metadata
  происходит вместе. При частичном обновлении сохраняется прежняя публикация.
- World имеет монотонную runtime identity: повторное создание мира по тому же
  адресу и повторный entity ID не переиспользуют его старый capture.
- ProbeGpuSmoke Release прошёл: static color/depth reuse, dynamic front/behind,
  roughness mips, partial publication/restart, metadata после перемещения,
  scene/environment invalidation, world reuse, Off, frame slots и DX12 debug layer.
  Own-shadow receiver red: 8.51562 без тени, 4.89453 с тенью.
- Первый GPU-прогон выявил DSV/SRV alias при shadow draw; собственная depth map
  больше не привязывается как SRV на этом проходе. Повторная проверка прошла.
  Один повторный build остановился из-за временного LNK1104 общей Engine.UI.lib;
  последующая последовательная сборка прошла.
- Регрессии: LightingGpuSmoke, десять кадров BistroEffectsSmoke, Release App и
  verify_scene.py (4 tests OK) прошли. Логи `Build/probe-capture-{build,gpu}.log`
  и `Build/probe-{LightingGpuSmoke,BistroEffectsSmoke,App}-*.log`.
- Capture producer готов; применение проб к material/specular/SSR — следующий
  шаг. Budgeted faces снимаются в последовательных кадрах; широкая invalidation
  может перерисовывать лишнюю статику. Производительность ещё не измерялась.

### Пробы в PBR и SSSR

- Коммит `f67edfa`, `feat(render): apply probes to PBR and SSR`.
- Deferred, прозрачный forward и specular replacement используют общий probe
  sampler: box projection только внутри influence AABB, плавное смешивание
  перекрытий и global fallback. Capture остаётся global-only; diffuse IBL глобальный.
- Общие metadata/globals загружаются раз на frame slot после capture; capacity
  резервируется до записи команд. SRV опубликованных кубов не переписываются.
- Материальный вход SSSR теперь имеет RGB fallback и roughness в A; negative RGB
  оставляет штатный global path. Baked и sky маскируются roughness=1.
- Маленький воспроизводимый SDK patch подставляет fallback до hit blending и
  denoising, без чтения confidence из alpha. Это prefiltered approximation.
  Bootstrap применяет его идемпотентно; AMD MIT notice сохранён. Также сохранён
  уже имевшийся CMake compatibility patch; исходные чужие warning-cleanup правки
  вне затронутых файлов этой итерации не включаются.
- Release ProbeLightingGpuSmoke прошёл: axis/zero-direction components, box
  parallax (другой face против raw direction), outside/global, overlap, mips,
  Off, baked/sky mask, PBR/resolve DFG once, SSR miss-localfallback и frame slots.
- Первое raw-radiance ожидание на градиенте маленькой пробы не учитывало denoiser.
  Miss fixture изолирован на однородном ROI, без ослабления допусков; отдельные
  границы/overlap/parallax проверки сохранены. RGB SSSR на тёплых кадрах:
  `(8, 0.0000023, 0.0000045)`, затем `(7.59375, 0.0000017, 0.0000033)`.
- FidelityFXGpuSmoke сохранил ray-hit deltas 367.869/322.359; PostProcessGpuSmoke,
  ProbeGpuSmoke, LightingGpuSmoke, десять кадров BistroEffectsSmoke, Release App
  и verify_scene.py (4 tests OK) прошли. Логи `Build/probe-lighting-*-{build,gpu}.log`.
- GLSL callback обновлён согласованно, Vulkan не собирался: двигатель проверен с DX12.

### Упрощение reflection passes

- Коммит `298159e`, `refactor(render): trim probe camera uploads`.
- Четыре read-only проверки ce-simplify-code завершены. Два совпавших finding
  объединены: единый enabled/quality predicate; camera constants загружаются
  только для снимаемых в текущем кадре граней. Удалён один пересказ кода в комментарии.
- Поведение публикации, shadow camera и тестовые assertions сохранены.
  ProbeGpuSmoke (включая optional benchmark) и ProbeLightingGpuSmoke прошли
  после изменений с DX12 debug layer. Логи `Build/benchmark-*-{build,gpu}.log`.
- Первую сборку остановил Windows macro `min`; вызов std::min защищён скобками,
  повторные Release-сборки прошли. Отдельный выигрыш упрощения не измерялся.

### GPU timings базовых проходов

- Коммит `1e26802`, `test(render): benchmark base GPU techniques`.
- Добавлен TechniqueGpuBenchmark: фиксированный HDR checker, reverse-Z depth
  step, движение 64 px/frame, 1920×1080. ProbeGpuSmoke имеет optional `--benchmark`,
  исходные assertions по умолчанию сохранены и выполняются до замеров.
- RTX 3060 Laptop, Release, DX12 debug layer, timestamp только вокруг Execute,
  4 warmup + 16 samples на случай, flush каждого кадра. PrepareFrame/allocations
  исключены. Это синтетические проходы, не frame time Bistro и не FPS-прогноз.

| Проход | Low median, ms | Mid median, ms | High median, ms |
|---|---:|---:|---:|
| SSAO | 1.339392 | 2.201600 | 4.244480 |
| HBAO | 0.995328 | 1.571328 | 2.995200 |
| XeGTAO | 0.529920 | 0.592896 | 1.030144 |
| SSSR + denoiser | 2.556928 | 3.012096 | 4.058624 |
| Motion blur | 0.721920 | 1.233920 | 2.138112 |
| FidelityFX DoF | 0.311808 | 0.321024 | 0.454656 |
| Probe Dynamic, 6 faces | 0.199680 | 0.314368 | 1.101312 |
| Probe BakedStatic steady, 6 faces | 0.221696 | 0.342016 | 1.128448 |

- Пробы: одинаковые две плоскости и own sun shadow; face sizes 64/128/256,
  все шесть граней каждый кадр для одинаковой нагрузки. Статический cache reuse
  проверен; на таком маленьком fixture baked чуть дороже из-за copy color/depth.
- DXGI process-local usage в core run: 268910592–432373760 bytes, в probe run:
  52539392–73883648 bytes; budget 5479858176 bytes. Это весь процесс с inputs,
  IBL и накопленными live outputs, не изолированная память конкретного эффекта.
- Полные mean/median/min/max, workload и usage каждого случая:
  [technique-gpu.json](2026-11-01-technique-gpu.json). Все 24 случая завершены,
  timestamp конечны, SSSR history прогрета, DX12 debug errors отсутствуют.
  Release-сборки и оба probe regression smoke прошли. Логи `Build/benchmark-*`.
- Замер не заменяет ещё требуемое визуальное сравнение native/XeSS и стекла Bistro.

### SkyBox и SkyAtmosphere

- Коммит `fb1560e`, `feat(render): add sky components and atmosphere`.
- Выбор одного источника `Render.Sky.Source=0/1/2`: Off/SkyBox/SkyAtmosphere.
  Missing selected component выдаёт диагностику и neutral sky/IBL; дубликаты
  разрешаются по минимальному entity ID с диагностикой. Другой тип не включается.
- Текущее небо оформлено SkyBox на существующей Sun; новая сущность не создана.
  Отдельного SkyAtmosphere consumer в Bistro нет. Обе update chains и граф подключены.
- Hillaire LUT path: transmittance → 64-direction multiscattering → nonlinear
  sky view → sky/IBL, отдельные RGB aerial scattering/transmittance volumes;
  HDR integration после OIT перед XeSS. ABI 256 bytes с sizeof/offsetof asserts.
  Источник MIT закреплён на `183ead5bdacc701b3b626347a680a2f3cd3d4fbd`, notice
  и лицензия сохранены рядом с шейдером. Единицы cm/Y-up → km/Z-up согласованы.
- Box сохраняет исходные 1024²/11 mips; sRGB декодируется один раз, float HDR
  остаётся линейным. Intensity/rotation не наследуют Transform Sun. Видимый solar
  disk ограничен RGBA16F и исключён из IBL; sun direction/radiance общие с PBR.
- Source identity сбрасывает temporal history; отдельная lighting revision
  обновляет IBL/baked shading без перезапуска partial probe capture и SSSR.
  CVar enable/quality/params, компонентные настройки и Engine.ini/docs обновлены.
- Evidence: SkyGpuSmoke написан до production; worker не запускал shared builds,
  поэтому red-before-implementation не заявляется. Шесть изолированных DXC stages
  прошли, затем host Release App и реальные GPU smokes. Первую сборку остановил
  include DirectXTK, он исправлен. Старое требование BC1 output в LightingGpuSmoke
  заменено проверкой linear-light radiance против независимого DDS decode/sRGB
  oracle; dimensions/mips assertion сохранён. Обновлённый тест прошёл.
- SkyGpuSmoke прошёл: sources/missing/duplicates/Off black IBL, axis rotation,
  intensity once, LUT qualities, RGB volume bounds, six faces/zenith/vertical sun,
  outside atmosphere, exact Box aerial bypass, реальная aerial geometry,
  независимый 1 km Beer–Lambert matrix/depth oracle, 12 кадров warm SSSR при
  движении солнца/высоты и reset при source switch. Найденная unrepresentable
  thin-shell boundary закрыта adaptive ULP offset/validation и GPU-проверкой
  planet radius 100000 km; thin 0.001 km shell отклоняется до dispatch.
- ProbeGpuSmoke подтвердил publication при changing atmosphere и face budgets
  1/2. LightingGpuSmoke, FidelityFXGpuSmoke, ProbeLightingGpuSmoke, десять кадров
  BistroEffectsSmoke и verify_scene.py (4 OK) прошли. После boundary fix повторены
  SkyGpuSmoke и Release App; DX12 debug errors отсутствуют. Логи `Build/sky-*`.
- Sky timings: одинаковый fixture **65×49**, HDR/depth на 1 km, continuous sun,
  steady transmittance/multiscattering, refresh sky/IBL/aerial + integration,
  4 warmup/16 samples. Low/Mid/High median: **0.124928/0.532992/2.643968 ms**.
  Process-local usage 64155648/70250496/97054720 bytes; это весь процесс с inputs
  и live SSSR, не изолированная память sky. Полные rows добавлены в
  [technique-gpu.json](2026-11-01-technique-gpu.json); не сравнивать с 1080p core
  timings как с одинаковым разрешением. CPU allocation/PrepareFrame исключены.
- Ограничения: aerial использует opaque depth и не разделяет слои стекла;
  file hot reload не автоматический (SkyBox.Revision переотправляет текущий
  AssetManager cache). Визуальное сравнение native/XeSS Bistro ещё не выполнено.

### Grass и wind

- Коммит `24a021d`, `feat(render): add compact grass and wind`.
- Добавлены GrassRenderComponent/WindComponent и submission в обеих update chains.
  Новых entities нет; пустая submission сохраняет neutral counts и не выделяет
  generated vertex/previous-position buffers. Материал — существующий opaque/masked PBR.
- GPU расширяет только фактические triangles пучков с тремя distance LOD, atomically
  формирует index count и использует существующие main/CSM/local/probe indirect paths.
  Генерация не отсекается main frustum: off-camera geometry остаётся caster/capture.
- Общая generated arena сохраняет GPU-данные при новых static uploads, повторно
  использует split/coalesced ranges с локальными identity indices. Удалены дубли
  CPU vertex/index caches. Previous positions выделяются только для деформации.
  Vertex ABI 44 bytes сохранён, tangent offset исправлен 36→offsetof(Tangent)=32.
- Wind solver написан по Stable Fluids: один semi-Lagrangian advection/force,
  divergence, ping-pong Jacobi, projection. Старый Shadertoy-код не копируется.
  Previous field/world/shape сохраняют корреспонденцию пучков независимо от порядка
  append. Свежий dt потребляется один раз; отсутствие Submit на паузе даёт dt=0.
- Enabled/Quality/parameters и documented overrides добавлены в CVar/Engine.ini.
  Grass Off убирает draw/count, wind может продолжаться; Wind Off выдаёт neutral
  field, трава остаётся. Off после первого перехода не инвалидирует тени каждый кадр.
  Глобальные множители не переписывают компонентные значения.
- Deformation revision инвалидирует cached shadows и shading проб, не сбрасывая
  capture cursor. UploadBuffer growth сохраняет mapped upload bytes и синхронизирует
  tracking state с новым ресурсом: growing visibility buffers не наследуют старый
  INDIRECT/UAV state. Native geometry binding обновляется после роста арен.
- Evidence: fixture написан до production, shared red build не запускался worker;
  одиночные DXC stages прошли с -WX. Host Release App сначала выявил недостающий
  descriptor-heap include и narrow/wide shader-path mismatch; они исправлены.
  Первый GPU run выявил rounding в повторном расчёте reset pose (макс. 0.000005722 cm).
  Reset теперь копирует current position напрямую; exact equality assertion сохранён
  и прошёл. Проверки free-index reuse и previous-world lookup усилены до GPU draws
  и multiset previous-pose oracle, без зависимости от append ordering.
- GrassGpuSmoke прошёл: odd wind grid/canaries и снижение divergence, neutral Off,
  compact counts 0/1/17/1024, near/mid/far/max distance, quality/grid/reset switches,
  прежние author fields, paused/no-submit motion, owner/slot reuse, generated data
  после static AddMesh, nonzero BaseVertex indexed motion, split/coalesced draws,
  PBR roughness/masked clipping, cached local shadows, emissive probe contribution,
  six-frame publication при wind updates, behind-camera caster/probe retention.
- Release App, LightingGpuSmoke, ProbeGpuSmoke, ProbeLightingGpuSmoke,
  PostProcessGpuSmoke, десять кадров BistroEffectsSmoke и verify_scene.py (4 OK)
  прошли; DX12 debug errors отсутствуют. Логи `Build/grass-*`.
- Fixed workload: один near chunk, 1024 tufts, solver + compact generation только,
  без raster/shadow/capture timings; wind grids 64²/128²/256² и 12/24/40 iterations,
  output vertices 24576/98304/294912. 4 warmup/16 samples, debug layer, per-frame flush.
  Low/Mid/High median **0.104960/0.327680/0.933888 ms**, process-local usage
  **133738496/134131712/136491008 bytes**. Это весь fixture с retained arenas,
  не isolated component memory. Полные rows добавлены в technique-gpu.json.
- Ограничения: grass patch плоский в локальном XZ, wind — 2D world-XZ domain;
  maximum distance от основной камеры ограничивает также shadow/capture geometry.
  Симуляция ветра художественная, Jacobi projection приближённый; FPS не обещается.

### Упрощение sky/grass после интеграции

- Коммит `8a2f81f`, `refactor(render): trim sky and grass state`.
- Удалены write-only geometry-binding bookkeeping и sky level, повторная проверка
  enabled ветра. World transpose/inverse для травы вычисляются один раз на компонент,
  а не на каждый chunk. Используется существующий CopyResource wrapper.
- Именование azimuth fallback уточнено без изменения выражений; удалены пересказы
  кода в комментариях. Packed shader parameter mapping и правило исключения solar
  disk из IBL сохранены как контракт интерфейса/учёта энергии.
- Proof tests не менялись. SkyGpuSmoke, GrassGpuSmoke, десять кадров
  BistroEffectsSmoke и Release App прошли после упрощений с DX12 debug layer.
  Логи `Build/sky-grass-simplify-*`. Отдельного lint/typecheck runner нет;
  полная Release-сборка App проверяет затронутые C++ проекты.
  Выигрыш отдельных CPU-упрощений не измерялся.

### Invalidation материалов и wind grid

- Дополнительные проверки по реальным depth pixels сначала воспроизвели три
  ошибки: смена material slot на alpha-zero оставляла старую тень; reset wind
  grid на паузе менял геометрию без обновления тени; alpha edit в том же material
  slot также не обновлял cached shadow. Каждый случай проверен до своей правки.
- Grass caster signature теперь учитывает material binding и фактическую wind
  grid. Общая MaterialRevision меняется на первом GPU upload каждого dirty edit
  для устройства; propagation в остальные frame slots её повторно не меняет.
  CSM/local caches и shading запечённых проб учитывают эту ревизию.
- GrassGpuSmoke подтверждает удаление старой тени для alpha-zero материала,
  восстановление opaque shadow, материал edit без смены slot, очистку CSM,
  сохранение cache при frame-slot propagation и обновление paused-grid shadow.
  Использован существующий material factory без его Initialize/scene creation;
  новых production test hooks нет. Полный grass/wind GPU smoke прошёл.
- ProbeGpuSmoke подтвердил смену cached static radiance red→green→red через
  существующий UpdateMaterialCB без scene revision и отсутствие повторного bake
  при frame-slot propagation. Первое emissive ожидание для baked material было
  неверным: существующий baked path возвращает baked albedo. Oracle заменён
  фактической сменой diffuse texture; production baked semantics не менялись.
- LightingGpuSmoke, десять кадров BistroEffectsSmoke и Release App прошли.
  DX12 debug errors отсутствуют. Логи `Build/grass-cache-*` и `Build/caster-cache-*`.

## Оставшаяся работа

GPU particles, fluid surface/solver,
skinning/asset import и cloud shaders. Наличие отдельных новых файлов во время
работы не считается завершением этих пунктов без их callers и GPU-проверок.

## Среда сборки

В sandbox получен MSVC C1902; после host-сборки обнаружены старые повреждённые
LTCG debug objects (LNK1103). Проверенные прогоны использовали VS2026 host x64,
`_CL_=/Z7 /GL-` и один Rebuild локальных проектов. Настройки toolchain проекта
из-за этого не менялись. Внешние DirectXTex libs имеют предупреждения о missing PDB.
