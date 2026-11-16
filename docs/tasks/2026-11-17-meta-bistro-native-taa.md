# Bistro: отдельный Native TAA

## Цель и границы

Репозиторий `D:\Project\PEPEngine2`. База задания: `0ffc71fa`, ветка
`codex/bistro-rendering`; перед реализацией сверить текущий HEAD и AGENTS.md.
Пользователь запросил задание после проверки состава техник. Этот документ
готовит следующий этап; при его создании реализация TAA не начиналась.

Добавить собственное temporal anti-aliasing в полном разрешении в обычный
App.MetaRenderGraph/Bistro через общий reconstruction provider. Сохранить Native
без сглаживания для сравнения, а XeSS/FSR/DLSS/NIS — с прежним поведением.
TAA выполняется один раз вместо другого provider, без дополнительного temporal
прохода поверх SDK. Это новая техника: в текущем App собственного Native TAA нет.

Этап local-only, primary GPU, запись compute shader на существующей DIRECT-границе
postprocessing через Meta Executor. FG, TAAU, sharpening, новые зависимости,
перераспределение очередей/GPU и итоговый App/Meta benchmark сюда не входят.
Сохранить текущие scene budgets, физику, воду, probes, UI и качество других техник.
Посторонние изменения в checkout оставить владельцам.

## 1. Подключение и controls

Основные точки входа:

- `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/GDX12Reconstruction.h`;
- `Engine/Engine.RendererDX12/src/GDX12Reconstruction.cpp` и
  `GDX12ReconstructionPass.cpp` — enum/CVars/factory, grid, stable output, settings;
- `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/` и
  `Engine/Engine.RendererDX12/Shaders/` — provider и shader;
- `Apps/App.MetaRenderGraph/src/Techniques/ScenePostUI.cpp` —
  `AppendReconstruction`, motion preparation, resource declarations, publication;
- `Apps/App.MetaRenderGraph/src/MetaRenderPipeline.cpp` и `.h` — settings,
  retirement, frame submission; текущий ordinary путь называется `_opaqueOnly`;
- `Apps/App.MetaRenderGraph/Configs/Startup.ini`, проектные `.vcxproj` и текущие
  metadata/UI reconstruction — настройки, компиляция и выбор provider.

Добавить `GDX12UpscalerProvider::TAA = 5` перед Count, сохранив номера 0..4.
UI называет его `Native TAA`; `Native = 0` остаётся passthrough. Проследить
все switch/metadata/диапазоны enum и requested/effective/fallback status.
Default и NativeAA для TAA означают один native-resolution режим; остальные
upscaling presets использовать с существующим явным unsupported/fallback контрактом.
Не подменять неподдержанный запрос работающим TAA без отражения в статусе.

Для передачи установить в Meta Startup `Render.Upscaler.Provider = 5`,
`Render.Upscaler.Quality = 0`; общий App default XeSS не менять. В общем provider
render width/height равны output width/height. Этим штатно изменятся размеры
входных render targets относительно прежнего XeSS — это не performance A/B.

Один переключатель — выбор provider; отдельный Enabled не нужен. Начальные
настройки `Render.TAA.HistoryWeight = 0.9` и `Render.TAA.VarianceGamma = 1.0`
экспонировать как документированные CVars в существующей системе. Это стартовые
значения, не результат подбора качества. Вес конечен и в [0,1), gamma конечна
и положительна; невалидные значения обработать по принятому CVar-контракту.
Использовать `Render.Reconstruction.Restart` для ручного сброса истории.

Результат шага: provider выбирается существующим UI/CVar, сообщает effective TAA,
не меняет смысл старых INI и доступен через общий reconstruction wrapper App/Meta.

## 2. Входы, jitter и алгоритм

Сохранить существующий порядок этого этапа:

`композиция сцены → DoF → motion preparation / Motion Blur → TAA
→ HDR display transform → screen UI → PRESENT`.

TAA читает линейный HDR на входе reconstruction, актуальные composition depth
и complete motion, предыдущие color/depth history. Учитывать воду и сферы:
сырой opaque depth уже не всегда соответствует конечной композиции.
Screen UI остаётся после TAA в разрешении окна; world-space UI проходит через
сцену как материал. Анимация текстур world UI не имеет собственного движения
геометрии — это ограничение учитывать подавлением истории при изменении цвета.

Использовать общий camera jitter/history путь и флаг `RENDER_PASS_FLAG_USE_JITTER`.
Найти фактический генератор jitter и владельца frame index: не вводить второй
независимый счётчик/последовательность. Jitter применяется к raster projection;
motion остаётся из unjittered transforms. Вывести и записать соглашение о знаке,
единицах, перевороте Y и поправке current/previous jitter именно для выбранных
историй; не переносить формулу из SDK без сверки с `MotionBlur.hlsl` и CameraCB.

Complete motion готовится и при выключенном blur, как для temporal SDK.
Переиспользовать sky/camera motion, skinning, траву, физические тела, particle
feature motion и preview spheres. Particle motion consumer должен видеть TAA
до подготовки текущего кадра. Полноценную velocity/depth для mesh transparency
не добавлять: сохранить текущий контракт и честно описать его ограничение.

Минимальный resolve: reprojection предыдущей истории, проверка валидности
координат/данных, отбрасывание истории на disocclusion по глубине, ограничение
history color текущим 3×3 neighborhood (variance clipping) и temporal blend.
Репроецированную history фильтровать; начать с bilinear без отдельного sharpen.
Базовый вес — верхняя граница доверия: уменьшать при несовпадении цвета/движении,
чтобы стекло, частицы и анимированные картины не оставляли устойчивый след.
Использовать reverse-Z корректно; сравнивать глубины в согласованном пространстве
с учётом движения камеры, а не сырой current depth с previous depth по одному UV.
Для sky определить отдельный валидный путь при отсутствии поверхности.

При невалидной истории вернуть текущий цвет и записать новую history, не читая
неинициализированные texels. Результат HDR конечен, без отрицательных выбросов
от ограничителя; threshold/формат history и blend convention описать в отчёте.
Принципиальная справка: [Microsoft Temporal AntiAliasing sample](https://learn.microsoft.com/en-us/samples/microsoft/xbox-gdk-samples/temporalantialiasing/).
Использовать как справку по jitter/history/reprojection/clipping, без переноса
sample host, GDK-инфраструктуры и чужой модели ресурсо-владения.

## 3. History и Meta ownership

History относится к предыдущему успешно отправленному render frame, а не
предыдущему использованию `CurrFrameConstantsIndex`. Один последовательный
temporal stream на viewport; не создавать три независимые истории по slots.
Достаточен persistent ping-pong color/depth при доказанном порядке DIRECT и
сохранении ресурсов до завершения всех читателей, включая display последующих
packets. Отдельные constants/descriptors защищаются frame-slot lifetime.

Определять read/write history и output binding на owner до записи команд.
`SetInputs` получает актуальные входы кадра, `LinkOutput` — именно его output.
Worker callback только записывает команды: не переключает общий output для
других packets. Индекс history продвигать один раз после успешного исполнения/
отправки; отмена/исключение не должны публиковать незаписанный кадр как valid.
При частичном submission использовать существующий error/retirement контракт.

Каждый history resource, вход, выход и camera constants объявить в Meta accesses
с фактическими states; одной записи output недостаточно для скрытых temporal
reads/writes. Общий App provider также должен иметь корректный resource-state
контракт. Переиспользовать общий shader/recording, не писать второй TAA для Meta.
Без private submit, steady-state Flush/CPU wait, readback или history CopyResource
ради перекладывания между slots. Reset и запись history по возможности совмещать
с resolve; новые GPU allocations/descriptor replacement в steady frame не нужны.

Сбрасывать history при первом кадре, camera cut/world reload, resize,
provider/quality/restart и общих history invalidation событиях. Связать это
с существующими reset flags/revision до их очистки reconstruction wrapper.
Обычное движение объектов/камеры, обновление probes и солнца не должны сбрасывать
всю историю каждый кадр. Смена настроек/размеров использует существующий retirement;
minimized/нулевой размер не запускает dispatch. После возврата к TAA используется
новая valid history, а Native/NIS получают прежние jitter-free flags и bypass.

Результат шага: три кадра в полёте сохраняют единую последовательность истории,
кадровые bindings не перезаписываются до fence, graph видит все зависимости.

## Приёмка и передача

Следовать AGENTS.md и Verification Contract. Release-сборка затронутых проектов
и один короткий обычный запуск Meta без `--smoke`: effective TAA, render=output,
видимая сцена, GPU выполняет resolve, экранный UI чёткий, явных ошибок нет.
В рамках этого запуска достаточно краткого движения камеры и остановки для
обнаружения грубых шлейфов/невалидной истории. Уже успешный smoke не повторять.
Автоматические тесты и режимные матрицы не создавать/не расширять; профилирование,
PIX/Nsight, точность, стресс и A/B — только по отдельному запросу пользователя.

По коду проверить Native bypass, TAA без blur, reset/resize и общий App caller;
отделить source review от фактически запущенных режимов. Не объявлять отсутствие
ghosting во всех материалах доказанным одним smoke. Особо записать ограничения
прозрачности, анимированных текстур и bilinear history reconstruction.

Обновить README/план переноса и добавить краткий отчёт: controls, фактические
размеры, порядок post, соглашение motion/jitter, ресурсы/lifetime и проверки.
Создать локальный коммит только своих изменений, сообщить hash, путь EXE и
незапущенные проверки; без push. Остановиться на пользовательской оценке
картинки/FPS/CPU/GPU, не начинать FG или следующий этап.
