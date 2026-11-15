# Bistro DoF в ordinary Meta — 2026-11-16

Основа: `e6ba6dfc`, ветка `codex/bistro-rendering`. Посторонние правки
`Apps/App/tests/BistroAdditionsTests.cpp`, `Temp/` и task animation/UI не включены.

## Порядок и настройки

Обычный `App.MetaRenderGraph`: итоговый RGBA16F HDR сцены с атмосферой,
opaque particle-water, стеклом и частицами → FidelityFX DoF → Motion Blur и complete
motion → reconstruction/display → output-sized screen UI → один PRESENT.
Полный diagnostic pipeline не включался. Вход depth — актуальный
`SceneLighting::PostDepth()` после композиции/воды; depth/motion DoF не меняет.
Feature motion частиц по-прежнему добавляется один раз в motion preparation.
Прозрачные материалы и частицы не дают отдельную полноценную глубину DoF:
это сохранённое ограничение App, поэтому фон может определять их размытие.

Startup: Enabled=true, Quality=2, FocusDistanceCm=500, FNumber=2.8,
SensorHeightMm=24, CocLimit=0, Rings=0, RingMerge=true. Эффективный Mid:
5 rings, CoC limit 0.05; FixedCocLimit сохраняет исходное правило (по умолчанию false),
положительный явный CocLimit имеет приоритет. Управление через существующие
`Render.DOF.*` в F1 Settings: FocusDistanceCm — плоскость фокуса в сантиметрах,
FNumber — диафрагма, Quality=0/1/2/3/4 — Off/Low/Mid/High/Epic.
Математика `CalculateDepthOfFieldCoc` неизменна: актуальные FOV/projection,
оптические параметры и фактическая высота render grid. Autofocus не добавлен.

## Bypass, состояния и lifetime

`IsActive()` сохраняет исходное правило pass: Enabled и Quality!=Off,
обе стороны grid >=16. Ordinary bypass передаёт исходный HDR без DoF packet,
dispatch и CopyResource. Blur получает тот же выбранный HDR, что объявлен в
AppendReconstruction. Native/NIS и disabled blur сохраняют прежние оптимизации.
DoF targets инициализируются независимо от blur, в том числе при позднем enable.

PrepareRecording вызывается владельцем перед записью; SDK dispatch записывается
Meta Executor на primary DIRECT. Packet объявляет HDR/depth reads и output write,
явную зависимость последующих потребителей и owner recording boundary.
Существующий tracker восстанавливает внешние states; pass сохраняет SRV/UAV
barriers, UAV barrier после dispatch и InvalidateCachedState после SDK.
В callback не перенесены allocation или Flush.

Targets/context сохраняются между кадрами. При resize или SR provider/grid change
владелец дренирует executor, затем inputs перепривязываются и targets/context
пересоздаются под grid. Rings/CocLimit/RingMerge используют существующее
DestroyContext с безопасным ожиданием DIRECT queue; FocusDistance/FNumber/FOV
не пересоздают context. Guard для FrameConstants.size()>FFX_MAX_QUEUED_FRAMES
сохранён: в bundled SDK лимит 4, ordinary Render.NumFrames=3, steady-state Flush
по этой причине отсутствует. Новых CPU readback/Flush не добавлено.
`External/FidelityFX/dof-hdr-precision.patch` и FP32 accumulation не менялись.

Сброс temporal SR при enable/disable и изменениях CoC перенесён в owner preparation,
до записи reconstruction, включая bypass без DoF packet. При неизменных
параметрах история не сбрасывается каждый кадр; движение солнца не меняет CoC.
Атмосфера, дневная дуга, размер диска, opaque water с temporal fix, остальные
техники/число объектов, paused world и XeSS/Default сохранены.

## Проверка

Release build успешен, MSBuild exit=0. Первая попытка в sandbox дала C1902
менеджера PDB; сборка вне sandbox прошла. Остались warnings C4834/C4244
в существующем коде и LNK4099 об отсутствующих PDB внешних библиотек.
Журнал сборки: `Build/meta-bistro-dof-build.log`.

Один обычный запуск без аргументов выполнен. Представлен кадр Bistro у входа,
взгляд на фасад Le Petit Coin/красные навесы: средний план читается чётче,
ближние столбы/брусчатка и дальние детали размыты; Settings и Reconstruction
остаются резкими. Эффективный XeSS, requested SDK default, render 985×616 →
output 1280×800. Startup.ini и сохранённый Config/Engine.ini подтверждают
перечисленные выше DOF-настройки. Операции displayed кадра содержат
`151 DoF SDK DIRECT recording boundary`, затем motion preparation/blur,
reconstruction, display, screen composition и один PRESENT handoff.
Виден GPU-результат; явных ошибок в кадре или ERROR/FATAL в текущей сессии
(с `2026-11-15T23:44:22.903Z`, 16 ноября по Москве) не обнаружено.
Это smoke и проверка порядка, не GPU profiling. Engine.log, operation lists и
выделенный `ordinary-session.log` сохранены в `Build/Verification/meta-bistro-dof/`.
В полном append-only Engine.log есть старые ошибки предыдущих запусков.
Обычный smoke не повторялся. После обнаружения пользовательского ввода
дальнейшее управление окном не выполнялось; FPS/CPU/GPU остаются пользователю.

Source review (ce-code-review, lite) завершён без замечаний; receipt:
`Build/Verification/ce-code-review/meta-bistro-dof-20261116`.
Проверка упрощений не потребовала изменений; `git diff --check` успешен.
Runtime-переключения enable/quality/CoC и resize проверяются только по исходникам.
Новые тесты не создавались: существующие DepthOfFieldTests/BistroDofSmoke относятся
к расширенной проверке App; по заданию замена — Release build и один обычный smoke.
Матрицы, точность, профилирование и длительные прогоны не запускались.
World-space UI canvas, clouds, probes, FG и физика не входят в изменение.
