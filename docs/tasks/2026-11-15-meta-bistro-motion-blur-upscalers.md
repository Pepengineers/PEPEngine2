# Bistro: Motion Blur + реконструкция/апскейлеры

Рабочая копия: `D:\Project\PEPEngine2`, обычное приложение `App.MetaRenderGraph`.
Пользователь объединил этапы Motion Blur (11) и reconstruction (13).
Основа: `69e7397d` с анимацией и UI плюс текущие незакоммиченные оптимизации частиц.
До правок проверить HEAD/status и сохранить чужую работу. Это задание на следующий
этап; его реализация при подготовке документа не начиналась.

## 1. Закрыть замечания предыдущего review

Прочитать [review animation/UI](../reports/2026-11-15-meta-bistro-animation-ui-review.md).
Убрать диагностические READBACK allocations из ordinary SceneGeometry::Freeze
и повторные CPU copies/comparisons неизменного font atlas. Сохранить diagnostic
readbacks и общий UI discard/lifetime contract. Сделать отдельный локальный коммит
этих исправлений, чтобы перенос Motion Blur/SR можно было оценивать отдельно.

## 2. Подключить обычный postprocessing путь

Переиспользовать `Techniques/ScenePostUI`, `GDX12MotionBlurPass`,
`GDX12ReconstructionPass` и существующие XeSS/FSR/DLSS/NIS providers.
Сохранить App composition order:

`opaque + SSSR → mesh/particle transparency → Motion Blur + complete motion
→ reconstruction/native → HDR display transform → screen UI → PRESENT`.

Входы: HDR после всех текущих transparent/additive draws, актуальная opaque
mesh/grass depth, GBuffer velocity, particle feature motion. Skinning, движущаяся
трава и камера сохраняют свои velocities; sky camera motion дополняется существующим
Prepare kernel. Стекло сохраняет контракт App; новую transparent velocity систему
не изобретать. Вода отсутствует, поэтому полный SceneWaterParticles не нужен.

В `MetaRenderPipeline` сейчас ordinary branch вызывает AppendOpaque; UpdateFrameSettings
рано возвращает, Reconstruction binding равен nullptr, а ScenePostUI::Passes возвращает
только output. Подключить необходимые bindings/flags/settings/resize и запись
через узкий обычный путь. `_opaqueOnly=false` целиком не включать: это активирует
отложенные техники. Полный diagnostic Append должен сохранить поведение и общие helpers.

DoF, reflection probes, атмосфера/облака, вода/физика, world-space posters,
frame generation и multi-GPU остаются выключены. UI остаётся после реконструкции
в разрешении окна; на него не действуют jitter, размытие и пониженное render resolution.

## 3. Motion Blur и motion history

Перенести существующие Prepare/Tiles/Neighbors/Gather без изменения алгоритма,
shutter, samples, depth rejection и soft particle semantics. Сверить текущие
App defaults: Enabled=true, Quality=Mid, ShutterFraction=0.5, MaxRadius=40,
Samples=0 (quality), и явно записать фактические настройки Meta.
Сохранить все `Render.MotionBlur.*` CVars и рабочее переключение через Settings.

`SceneParticles` сейчас отключает MotionOutput, потому что потребителя не было.
Включить его до BeginFrame при наличии потребителя, импортировать его реальные
write/read accesses и передать в post owner с корректным временем жизни.
Не использовать сырой ParticleMotion от прошлого кадра. Цвет частиц не рисуется
повторно ради композиции; отдельная feature-motion запись остаётся по App контракту.

CompleteMotion требуется temporal SR и при выключенном blur. Не связывать
отключение gather с отключением motion preparation. Для Native/NIS без blur
переиспользовать входной HDR, когда подготовка motion никому не нужна: не добавлять
пустые tiles/gather, лишний motion raster и полноэкранный CopyResource.
Любые минимальные изменения shared helpers сохраняют default App поведение.

Проследить единицы/знак velocity (существующий previous-minus-current NDC),
unjittered matrices, reverse-Z и перевод радиуса в output pixels. Jitter не должен
создавать искусственное размытие неподвижных объектов. Previous poses/transform/time
относятся к предыдущему представленному render frame, а не к предыдущему использованию
кадрового слота. Reset/cut/resize/spawn/re-enable не оставляют старые trails.
Флаги reset очищаются после потребителей, включая Native path, без вечного reset.

## 4. Провайдеры, разрешения и переключения

Подключить существующие `Render.Upscaler.Provider`, `.Quality` и
`Render.Reconstruction.Restart`, статус requested/effective/fallback reason в UI.
Состав определяется текущим CreateProvider, без нового SDK или собственного TAA:

| Provider | Контракт |
| --- | --- |
| Native | Passthrough, render grid равен output, самостоятельного TAA нет |
| XeSS | Текущий SDK и поддерживаемые presets, включая AA |
| FSR | Текущий SDK и presets, включая NativeAA |
| DLSS | Текущий Streamline/provider и presets, включая DLAA |
| NIS | Пространственное масштабирование, без temporal AA |

Неподдерживаемая комбинация SDK/adapter/preset использует существующий явный fallback
со статусом и причиной. Не выдавать fallback Native за работающий запрошенный SR.
Для стартового обычного запуска выбрать App source default XeSS/Default, зафиксировать
фактический effective provider/quality и оба разрешения. Остальные providers доступны
через те же controls, их runtime-проверка не требуется матрицей в этом этапе.

QueryRenderTargetResolution/ApplySettings должны определять размер до создания
geometry/GBuffer/depth/AO/SSSR/transparency/grass/particle/post targets и CameraCB.
Сохранить размеры отдельного display/UI. Правильные jitter flags/offsets должны
попадать в камеру при активном temporal provider; Native/NIS не получают ненужный
temporal jitter. Провайдер получает актуальные color/depth/motion каждого frame slot
через SetInputs, а history SDK сохраняется между соседними кадрами.

При смене provider/quality/restart и resize выполнить существующий безопасный drain,
пересоздать только требуемые размеры/контексты, сбросить history SSSR и temporal
consumers и заново связать inputs. Steady-state Flush/Wait, повторное создание SDK
на каждый слот и смена разрешения задним числом во время записи запрещены.
Не включать FG bindings/Prepare/tagging ради доступа к reconstruction.

## 5. Meta resources и стоимость кадра

Начать с существующего DIRECT recording SDK boundary. Объявить реальные
reads/writes/states, resource identity и producer → consumer dependencies через Meta;
сохранить owner recording у изменяющих внутреннее состояние helpers. Команды
не запускают private submit/Flush/legacy Execute. Runtime tile resize в Motion Blur
готовится владельцем до записи; долгоживущие resources/descriptors не уничтожаются
до fence их последних пользователей. Учесть, что disabled/shared outputs могут
alias входной HDR: не объявлять противоречивые accesses одного ресурса.

Сохранить actor/particle/grass budgets, качество соседних техник, локальные shadow
caches и dirty uploads. Постоянные данные Bistro повторно не копируются; UI atlas
и static geometry не становятся per-frame uploads. Нельзя получать FPS снижением
числа объектов или отключением техник вне этого задания.

## Приёмка и передача

AGENTS.md и Verification Contract в `docs/plans/2026-11-13-meta-render-graph/00-meta-plan.md`
определяют объём проверок: Release-сборка и один короткий обычный запуск без `--smoke`.
Видны сцена/персонажи/частицы, Motion Blur работает при движении, UI остаётся чётким;
зафиксировать effective SR и render/output sizes, GPU выполняет работу, явных ошибок нет.
Если SDK недоступен, честно записать fallback и непроверенный provider.
Уже успешный smoke не повторять. Матрицы presets/providers, точность, стресс,
профилирование/PIX/Nsight и новые тесты не входят в это задание.

Обновить README, migration plan и краткий отчёт с настройками, порядком композиции,
history/reset/resize ownership, итогом исправлений review, проверками и ограничениями.
Создать локальные task-owned коммиты, без push, сообщить hashes и executable.
Остановиться на пользовательскую проверку FPS/CPU/GPU. Следом по согласованному
порядку остаются локальные reflection probes; их здесь не начинать.
