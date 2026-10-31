# DOF и motion blur: выбор реализации

Дата: 2026-10-31. Статус: пользователь выбрал готовый FidelityFX DoF; конкретные изменения pipeline ещё обсуждаются. Код рендера не менялся. Решение: [ADR-0006](../adr/0006-fidelityfx-sssr-and-dof.md).

## Рекомендация

- **Motion blur:** реализовать фильтр Guertin, McGuire, Nowrouzezahrai, **A Fast and Stable Feature-Aware Motion Blur Filter, HPG 2014**, по финальной статье и Appendix A. Из старого проекта использовать проверенные ресурсы/редукции как заготовку, ядро реконструкции заменить.
- **DOF:** взять **FidelityFX DoF 1.1.0 из SDK `v1.1.4`** как конкретную исходную реализацию. Это отдельный компонент с открытым HLSL и MIT-лицензией, а не старый DepthOfFieldFX для DX11. Предпочтителен штатный context + DX12 backend; перенос одних алгоритмов в собственные passes потребует воспроизвести больше контрактов и проверок.
- Сначала получить корректный **native-resolution HDR** вариант и сравнимые тестовые кадры. Расположение эффектов относительно XeSS требует отдельного решения: готовые инструкции AMD и Intel исходят из разных условий. Одной перестановки проходов недостаточно.

## Что именно требует исправления в старом коде

Локальный донор: `D:/Project/Multi-GPU-Render-DX12`, текущее содержимое файлов на дату исследования; commit через Git в этой сессии прочитать не удалось.

| Файл | Наблюдение | Следствие |
| --- | --- | --- |
| `MGPU-DOF/Shaders/DOF/DOF_CoC_CS.hlsl:38-42` | `abs(linearZ - FocusDistance) / FocusRange`, ограниченный `MaxCoC`; знак ближнего/дальнего размытия теряется | Это художественная зона фокуса; переход к thin-lens CoC и разделённым слоям меняет ядро алгоритма |
| `MGPU-DOF/Shaders/DOF/DOF_Gather_CS.hlsl:57-119` | Диск выборок берётся по CoC центрального пикселя; видимый/заполненный цвет смешивается одним центральным `WFiltered`; внутри есть sRGB encoding и saturate | Нельзя считать такую сборку полноценной моделью распространения foreground bokeh; encoding нужно вынести к финальному выводу HDR pipeline |
| `MGPU-MBlur/Shaders/MotionBlur.hlsl:53-90` | Вектор реконструируется из depth и camera matrices; нет истории трансформаций движущегося объекта | Использовать существующий объектный motion buffer PEPEngine2 |
| `MGPU-MBlur/Shaders/MotionBlur.hlsl:211-272` | В `CalcMotionBlur` выборки идут только вдоль `vn`, `noiseValue=0`, присутствуют множители `*5` | Это не полный двухнаправленный фильтр HPG 2014; увеличение taps не возвращает пропущенную модель |

## Motion blur: конкретный ориентир

Точная публикация установлена по [странице авторов](https://casual-effects.com/research/Guertin2014MotionBlur/index.html) и [финальному PDF HPG 2014](https://casual-effects.com/research/Guertin2014MotionBlur/Guertin2014MotionBlur-small.pdf). Доступный NVIDIA report 2013 — предшествующая версия, а не выбранная редакция.

Нужны TileMax/NeighborMax, реконструкция вдоль доминирующего и локального направлений, обработка малого локального движения, velocity-aware веса переднего/заднего плана, jitter выборок и обращения к соседним tiles. Appendix A содержит псевдокод. Статья допускает равное распределение выборок между направлениями: variance-based распределение авторы отключили в основных интерактивных результатах. Это разумная минимальная версия без потери главного свойства. Авторы отмечают шум и ограничения линейного движения; после motion blur DOF в их ограниченном сравнении давал лучший результат. Это не доказательство универсально правильного порядка. [Источник: §§4–6, Appendix A](https://casual-effects.com/research/Guertin2014MotionBlur/Guertin2014MotionBlur-small.pdf).

Первую реализацию предлагается написать по статье, с собственной проверкой формул и единиц, без копирования стороннего движка. На странице авторов найден PDF/видео, но готовый авторский DX12/HLSL комплект не установлен. Наличие чужого шейдера с названием Guertin не подтверждает точность реализации.

В PEPEngine2 `Engine/Engine.RendererDX12/Shaders/OpaquePass.hlsl:82-91` уже выдаёт **prevNDC − currentNDC** без jitter и с `PrevWorld`. Для UV displacement преобразование — множитель `(0.5, -0.5)`, затем размер выбранного изображения для перехода в pixels. Shutter exposure задаётся отдельно от frame displacement; низкий FPS не должен неявно превращаться в произвольный multiplier. Не перезаписывать общий velocity texture ради отдельного эффекта.

## DOF: почему FidelityFX

Просмотрены именно исходники тега **`v1.1.4`**, а не только описание продукта:

| Исходник AMD | Проверенный контракт |
| --- | --- |
| [host/ffx_dof.h](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/sdk/include/FidelityFX/host/ffx_dof.h) | Версия 1.1.0; пять compute passes; input color/depth/output; reverse depth; quality и CoC limit; context требует `FfxInterface` backend |
| [dof/ffx_dof_blur.h](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/sdk/include/FidelityFX/gpu/dof/ffx_dof_blur.h) | Раздельные near/far накопители, signed CoC, ring buckets с occlusion, специальная обработка центральной выборки, tile classification, варианты near-only/far-only/combined |
| [dof/ffx_dof_composite.h](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/sdk/include/FidelityFX/gpu/dof/ffx_dof_composite.h) | Фильтрация промежуточных near/far, проверка весов foreground перед интерполяцией, реконструкция и финальная композиция |
| [components/dof/ffx_dof.cpp](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/sdk/src/components/dof/ffx_dof.cpp) | Создание внутренних ресурсов/PSO через callbacks backend; управление ресурсами и context; это не drop-in вызов имеющегося FSR API loader |

Все перечисленные файлы содержат MIT notice; его нужно сохранить. Уже имеющийся `External/External.Libraries/ffx-api` содержит loader/upscale/framegeneration API, но не найдено DOF host/backend: интеграция потребует добавить соответствующий компонент и сборку. Cauldron sample framework переносить не предлагается.

[Инструкция AMD](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/depth-of-field/) задаёт thin-lens CoC, положительный near и отрицательный far, radius в half-resolution pixels, reverse-depth flag. Алгоритм использует bilateral color downsample, tile min/max, dilation, near/far blur и composite. Ограничения: single-layer depth не описывает translucent/volumetric coverage; предсобранные shaders дают круглый bokeh. Рекомендуются стабильный вход после TAA и render-resolution DOF до upscale. Эти оговорки нужно сохранить в критериях готовности.

**Ближайшая альтернатива:** [Guillaume Abadie, A Life of a Bokeh, SIGGRAPH 2018](https://advances.realtimerendering.com/s2018/index.htm) описывает гибрид scatter-as-gather и sprites для ярких бликов, background occlusion и foreground hole filling. Это качественный ориентир для дальнейших тестов. Копирование Unreal/HDRP renderer ради первого DOF здесь не обосновано: увеличивается перенос окружения, а преимущество на нашей сцене ещё не измерено. Рекомендация FidelityFX основана на доступном компактном компоненте и проверенных контрактах, а не на заявлении, что это абсолютный максимум качества.

## HDR, XeSS и прозрачные бутылки

Сейчас `Apps/App.Base/src/RenderModule.cpp:845-889` связывает composition → XeSS. В `Shaders/CompositionPass.hlsl:29-31` composition одновременно делает Reinhard и sRGB. `GDX12XeSSUpscalePass.h:184-186` включает `XESS_INIT_FLAG_LDR_INPUT_COLOR`; `:90` всегда задаёт `resetHistory=false`.

Нужно отделить linear-HDR opaque/OIT composition от tone mapping. DOF и motion blur должны получать согласованные цвет, depth и velocity; переходы света, яркие bokeh и нормализацию нельзя оценивать после преждевременного clipping/encoding. Это предложение для нашего pipeline.

[Intel рекомендует](https://www.intel.com/content/www/us/en/developer/articles/guide/xe-super-sampling-developer-guide.html) ранний XeSS до tone mapping, linear-HDR FP16 color и FP16 velocity, корректную exposure; предупреждает против quarter-resolution effects и шумного сигнала перед реконструкцией. Это противоречит простому применению совета AMD «DOF до upscale» без учёта характера XeSS.

Предлагаемый **quality-first** вариант после native reference: `HDR composition → XeSS → motion blur → DOF → tone mapping/output`. Для него обязательны явно подготовленные depth/velocity на выходной сетке, с сохранением границ объектов и учётом jitter; простая bilinear растяжка depth не является достаточным решением. Это инженерный вывод, не готовый рецепт из AMD. Альтернатива с меньшей стоимостью — `HDR composition → motion blur → DOF → XeSS → tone mapping`; она требует отдельно доказать temporal quality и сохранение bokeh на Bistro. Не объявлять оба режима качественными до сравнений.

Прозрачный OIT сейчас не выдаёт отдельный слой глубины/скорости стекла: blurred composed color с opaque depth не может дать правильный фокус и движение бутылки отдельно от фона. Для первой версии нужно явно принять это ограничение либо включить отдельную обработку translucent в объём. Ошибку нельзя исправить коэффициентом CoC.

## Проверка реализации, когда выбор будет утверждён

1. Native HDR reference: резкая плоскость фокуса; foreground silhouette на ярком фоне; несколько пересекающихся глубин; тонкие детали; HDR точечные блики; радиусы меньше пикселя и очень большие; экранные границы.
2. Motion blur: камера неподвижна/движется; объект движется отдельно; встречные и перпендикулярные скорости; тонкие быстро движущиеся детали; нулевые скорости; длинные кадры; camera cut; resize.
3. Сравнить native и XeSS в движении, а не только один screenshot: ghosting, края blur, мерцание bokeh, tiles, экспозиция. На измеренной GPU зафиксировать стоимость каждого прохода и память при заданном разрешении и quality.
4. DX12 debug layer и readback: конечные значения, отсутствие NaN/Inf/OOB; постоянный цвет сохраняется; нулевая выдержка сохраняет вход; CoC равен нулю на focus plane. Сброс history при camera cut/resize обязателен для temporal компонентов.

## Решения для интервью

- Выбор донора закрыт: принят готовый FidelityFX DoF 1.1.0 `v1.1.4`. Для масштабируемых параметров требуются Off/Low/Mid/High. Возвращение старого упрощённого MGPU-DOF пользователь не запрашивал.
- Нужен ли круглый bokeh первой версии либо уже требуются форма диафрагмы и отдельные highlight sprites? Рекомендация: круглый до появления конкретного визуального требования.
- Принять native HDR reference и quality-first обработку после XeSS с дополнительной depth/velocity подготовкой либо выбрать более дешёвый pre-XeSS путь и его ограничения? Рекомендация: native reference, затем quality-first, решение о финальном порядке подтвердить сравнением.
- Допустимо ли ограничение single-layer depth на OIT-стекле первой версии? Если нет, отдельный translucent path — обязательная работа, а не поздняя косметическая настройка.
- Пользователь выбрал настраиваемые уровни качества и последующий ручной подбор вместо фиксированной цели GPU/разрешение/FPS. Замеры нужны для обоснования пресетов; число миллисекунд из статьи 2014 не является оценкой для этого приложения.

Не выполнялись сборка внешнего DOF, GPU замеры и визуальные сравнения. Ограничения single-layer depth и linear-motion reconstruction остаются даже после аккуратного переноса.
