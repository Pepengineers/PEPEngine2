# SSR: Nau Engine, Unity HDRP и доступный донор для PEPEngine2

Дата проверки: 2026-10-31. Статус: пользователь выбрал готовый FidelityFX SSSR с Reflections Denoiser; детали интеграции остаются проектным предложением. Рендер-код не менялся, GPU-проверки не выполнялись. Решение: [ADR-0006](../adr/0006-fidelityfx-sssr-and-dof.md).

## Область решения

По заданию рассматривается первая очередь основного рендера на одном GPU: корректный PBR SSR с учётом шероховатости, устойчивостью во времени и fallback на окружение. Старый fullscreen lerp SSR из Multi-GPU-Render-DX12 не считается готовым решением этого требования. Другие эффекты и перенос MGPU транспорта сюда не входят.

## Результат сравнения

| Кандидат | Подтверждённый результат | Пригодность |
|---|---|---|
| NauEnginePublic | В проверенных публичных main/dev не найден исполняемый SSR-путь; найдено cubemap IBL | Пока не подтверждён как SSR-донор |
| Unity HDRP PBR Accumulation | Иерархическая трассировка, GGX VNDF, reprojection, temporal accumulation | Технический ориентир; прямое копирование ограничено Unity Companion License |
| AMD FidelityFX SSSR 1.5 + Reflections Denoiser | Выделенный SSR-компонент и DX12 backend; MIT | Выбран пользователем |

## Nau Engine: что действительно найдено

Зафиксированы `main` commit `8840fb3cb7ad69045f2d9f64cd7bfffc8ddc0b82` и `dev` commit `e2bb55227cde1f0e7575cb9569407351a9715252`. Через GitHub API просмотрены полные, не усечённые деревья `engine/core` обеих ревизий и дерево `project_templates`; проверены deferred resolve и его C++ binding. Это ограниченный результат поиска в конкретных публичных ревизиях, а не утверждение об отсутствии SSR во всех версиях Nau/Dagor. [main](https://github.com/NauEngine/NauEnginePublic/tree/8840fb3cb7ad69045f2d9f64cd7bfffc8ddc0b82), [dev](https://github.com/NauEngine/NauEnginePublic/tree/e2bb55227cde1f0e7575cb9569407351a9715252).

`project_templates/empty/content/shaders/src/deferred_resolve.hlsl` объявляет `TextureCube reflectionMap`, выбирает mip по roughness и применяет environment BRDF. `engine/core/modules/graphics/src/render/deferredRenderer.cpp` передаёт именно cubemap в resolve. Это specular IBL, не screen-space ray tracing. Файл `cs_gen_reflection_map.hlsl` по названию нельзя считать SSR. [Shader](https://github.com/NauEngine/NauEnginePublic/blob/8840fb3cb7ad69045f2d9f64cd7bfffc8ddc0b82/project_templates/empty/content/shaders/src/deferred_resolve.hlsl), [binding](https://github.com/NauEngine/NauEnginePublic/blob/8840fb3cb7ad69045f2d9f64cd7bfffc8ddc0b82/engine/core/modules/graphics/src/render/deferredRenderer.cpp).

Лицензия Nau — BSD 3-Clause; root notice отдельно относит Dagor к BSD 3-Clause. Разрешительная лицензия не заменяет отсутствующий конкретный донор: если имеется в виду другая ветка/репозиторий, сначала нужен её действительный SSR-путь. [LICENSE](https://github.com/NauEngine/NauEnginePublic/blob/8840fb3cb7ad69045f2d9f64cd7bfffc8ddc0b82/LICENSE).

## Unity HDRP: полноценный алгоритм, существенные зависимости

Проверена ревизия Graphics `0efc28ea742b1966b9dd9cc4489e58b986a13faf`, файл `Packages/com.unity.render-pipelines.high-definition/Runtime/Lighting/ScreenSpaceLighting/ScreenSpaceReflections.compute`. В нём есть отдельные tracing/reprojection/accumulation kernels, PBR sampling через GGX VNDF, обход depth pyramid по mip-уровням и фильтрация истории. Это отличается от режима `SSR_APPROX`, который также присутствует в том же файле. [Compute source](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.high-definition/Runtime/Lighting/ScreenSpaceLighting/ScreenSpaceReflections.compute).

Входы/зависимости включают normal+roughness decoding, depth pyramid и offsets, motion vectors, предыдущую color pyramid, accumulation history, blue-noise sampling, camera matrices, HDRP shader variables, stencil receive mask и BRDF helpers. Простое копирование compute-файла не создаёт работающий независимый pass. В `GetHitNDC` остаётся TODO про depth-history проверку occlusion/disocclusion; наличие temporal accumulation само по себе не доказывает отсутствие ghosting. [Тот же compute source](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.high-definition/Runtime/Lighting/ScreenSpaceLighting/ScreenSpaceReflections.compute).

Лицензионное ограничение конкретно для прямого переноса: root `LICENSE.md` назначает Unity Companion License для Unity-dependent projects. Текст UCL v1.4 связывает использование с Engine License и исключает создание конкурирующего продукта. Поэтому для самостоятельного PEPEngine2 прямой перенос HDRP-кода не рекомендован без отдельного подходящего разрешения. Это вывод из опубликованного текста лицензии, не принятое пользователем решение. [License в проверенной ревизии](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/LICENSE.md), [действующий текст UCL](https://unity.com/legal/licenses/unity-companion-license).

## AMD: выбранный донор

Рассматривается SSSR **1.5.0 из FidelityFX SDK `v1.1.4`**, не старый standalone sample. Версия проверена в `sdk/include/FidelityFX/host/ffx_sssr.h`. Хост `sdk/src/components/sssr/ffx_sssr.cpp` создаёт и вызывает Reflections Denoiser, использует backend interface и проверяет его SDK-версию. [API header](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/sdk/include/FidelityFX/host/ffx_sssr.h), [host implementation](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/sdk/src/components/sssr/ffx_sssr.cpp).

Алгоритм: depth hierarchy/SPD → tile classification → blue noise → indirect arguments → GGX stochastic intersection → denoising. DX12 требует CS 6.2, отдельные варианты CS 6.6. Предусмотрены reverse-Z flag, unpack normal, канал/смысл roughness и motion scale. Входы — HDR scene, depth, normals, material roughness, motion, environment cubemap и BRDF LUT. Выход — отдельный reflections target. [Официальная техника и интеграция](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/stochastic-screen-space-reflections/).

Компонент и SDK в проверенной версии опубликованы с MIT notice. Для встраивания предпочтительны SSSR, denoiser и DX12 backend, а не перенос всего Cauldron/sample framework. Лицензии и notices выбранных файлов сохраняются. [SDK LICENSE](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/LICENSE.txt).

## Наименьшая полноценная интеграция: предложение

Следующие пункты — проектное предложение на основании доноров и текущего кода, не уже выполненные изменения.

1. Добавить один renderer pass, владеющий SSSR context и его lifetime/resize; использовать существующие `GDX12RenderPass`, command list, descriptor heap и texture wrappers. SDK backend записывает работу в текущий DX12 command list; MGPU resources/fences не нужны.
2. Использовать имеющиеся G-buffer depth, world normal/roughness и motion. `OpaquePass.hlsl` пишет `prevNDC-currentNDC`; начальный кандидат преобразования для AMD — `motionVectorScale=(0.5,-0.5)`, с обязательной проверкой знака/Y на тестовом движении. `roughnessChannel=3` соответствует `.w` normal buffer; perceptual/squared смысл сверить с текущим BRDF. Включить inverted-depth и проверить matrix transpose/handedness. [Локальный shader](../../Engine/Engine.RendererDX12/Shaders/OpaquePass.hlsl), [донорский контракт](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/sdk/include/FidelityFX/host/ffx_sssr.h).
3. Разделить specular environment contribution и остальное освещение в общем lighting helper. Сейчас `ShadeSunAndIBL` возвращает direct + diffuse IBL + specular IBL + emissive одним цветом. Нельзя прибавить SSR поверх уже посчитанного specular IBL: нужен единый specular resolve с fallback и материалом F0/roughness. Конкретный контракт radiance/BRDF output проверить по выбранному SDK sample перед кодированием. [Текущий helper](../../Engine/Engine.RendererDX12/Shaders/PBRLighting.hlsli), [deferred output](../../Engine/Engine.RendererDX12/Shaders/DeferredLighting.hlsl).
4. SSR trace/denoise/resolve выполнять в линейном HDR до WBOIT composition и tone mapping. В текущем `CompositionPass.hlsl` tone map+sRGB уже выполняются перед XeSS, а XeSS context имеет `LDR_INPUT_COLOR`. SSR можно встроить перед этим существующим переходом; перевод всего XeSS pipeline на HDR является отдельным решением. [Composition](../../Engine/Engine.RendererDX12/Shaders/CompositionPass.hlsl), [XeSS wrapper](../../Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12XeSSUpscalePass.h).
5. Первая версия: opaque/masked SSR, fallback на существующее глобальное окружение; локальные probes можно подключить позже. Нужны сброс/отбрасывание истории при camera cut, resize, смене режима; отсутствие явного reset поля в проверенном `FfxSssrDispatchDescription` требует выбрать корректный механизм управления историей при реализации. Это нельзя молча заменить продолжением старой истории.

## Первичные публикации для независимой реализации

- McGuire & Mara, **Efficient GPU Screen-Space Ray Tracing**, JCGT 2014: perspective-correct DDA для depth-buffer ray tracing. Это основа пересечения, не готовый roughness/temporal SSR целиком. [Статья](https://jcgt.org/published/0003/04/04/).
- Stachowiak, **Stochastic Screen-Space Reflections**, SIGGRAPH 2015: importance sampling и spatiotemporal filtering; авторская страница содержит slides. [Автор](https://h3.gd/stochastic-ssr/).
- Heitz, **Sampling the GGX Distribution of Visible Normals**, JCGT 2018: источник VNDF sampling, на который ссылается AMD. [Публикация](https://jcgt.org/published/0007/04/01/).

Независимая реализация по публикациям остаётся реальной альтернативой, но её нельзя выдавать за дешёвый перенос готовой техники. Требуется реализовать и проверить весь путь, включая filtering и fallback.

## Решения, которые остаются за пользователем

1. Выбор донора закрыт: пользователь принял готовые компоненты FidelityFX SSSR + Reflections Denoiser. Для масштабируемых параметров требуются Off/Low/Mid/High; фиксированная цель FPS не установлена.
2. Локальные probes уже приняты пользователем для первой группы. Промежуточный opaque/masked SSR с global fallback допустим как шаг разработки, но не завершает всю группу; интеграция SSR/probes/IBL обязательна. Качество отражений на прозрачных материалах и ограничения OIT нужно проверить отдельно.
3. Принять проверку качества как критерий завершения: зеркальная и шероховатая поверхности, offscreen miss, thin geometry, camera cut, движение камеры/объекта, resize и XeSS. GPU time и память измеряются на целевом железе; преимущество в миллисекундах заранее не заявляется.

Сборку доноров, runtime-качество, целевой GPU/driver и конечный memory/performance budget не проверяли. Главные риски интеграции — двойной specular IBL, несовпадение motion/matrix conventions и неверная история при disocclusion/camera cut.
