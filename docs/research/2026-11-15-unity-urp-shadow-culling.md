# Как URP отбирает геометрию для теней

Проверено по официальным исходникам Unity, 15 ноября 2026. Этот разбор касается
отсечения текущего кадра. Оптимизация межкадровым кэшем отложена по указанию
пользователя; новый temporal cache здесь не предлагается.

Исходники Graphics закреплены на `0efc28ea742b1966b9dd9cc4489e58b986a13faf`
(master, URP 17.7.0); UnityCsReference — на
`cf583fbe6da6b9fb11e3a1ac03f23df4eb120e9b` (5 ноября 2026).
Основной путь spot/point сверён также с URP 17.3.0 из предыдущего разбора.

## Главное различие

«Нарисовать realtime-тени каждый кадр» не значит «рисовать всю сцену для каждой
лампы». URP сначала выбирает светильники, затем отдельно выполняет shadow-caster
culling для их срезов и рисует соответствующие renderer lists. Список объектов
Opaque, видимых основной камерой, не подставляется вместо этого отбора.

## Что видно в исходниках

1. `ScriptableRenderContext.Cull` возвращает `CullingResults`, включая
   `visibleLights`. URP ограничивает maximumVisibleLights в зависимости от пути
   рендера и передаёт `shadowDistance`. Дальность теней камеры равна минимуму
   настройки pipeline и camera far clip; нулевая дальность отключает запрос
   shadow casters. Сам алгоритм исходного scene/light culling — native.
   [URP SetupCullingParameters, 679–711](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/UniversalRenderer.cs#L679-L711),
   [maxShadowDistance, 2150–2152](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/UniversalRenderPipeline.cs#L2150-L2152),
   [Cull → Internal_Cull](https://github.com/Unity-Technologies/UnityCsReference/blob/cf583fbe6da6b9fb11e3a1ac03f23df4eb120e9b/Runtime/Export/RenderPipeline/ScriptableRenderContext.cs#L308-L312).

2. Из visible lights в локальный атлас попадают поддерживаемые shadow-casting
   источники с ненулевой strength. Main light исключён из этого атласа. Создаются
   запросы срезов и проверяется возможность размещения в атласе. Это отбор ламп,
   ещё не отбор мешей.
   [AdditionalLightsShadowAtlasLayout, 113–154](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/Passes/AdditionalLightsShadowAtlasLayout.cs#L113-L154),
   [ShadowUtils eligibility / slice count, 988–1009](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/ShadowUtils.cs#L988-L1009).

3. Для spot строится один shadow view; для point — до шести отдельных views по
   cubemap face. URP получает для них матрицы и `ShadowSplitData` через
   `ComputeSpotShadowMatricesAndCullingPrimitives` и
   `ComputePointShadowMatricesAndCullingPrimitives`. У point учитывается запас
   FOV для фильтрации/bias. `ShadowSplitData` содержит плоскости отсечения,
   culling sphere, matrix и near plane, а не только матрицу рисования.
   [ShadowCulling, 86–145](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/ShadowCulling.cs#L86-L145),
   [ShadowUtils, 195–197 и 233–235](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/ShadowUtils.cs#L195-L235),
   [ShadowSplitData layout](https://github.com/Unity-Technologies/UnityCsReference/blob/cf583fbe6da6b9fb11e3a1ac03f23df4eb120e9b/Runtime/Export/RenderPipeline/ShadowSplitData.cs#L13-L51).

4. URP собирает все split data и диапазон срезов каждого источника в общий
   запрос `ShadowCastersCullingInfos`, затем вызывает
   `context.CullShadowCasters(cullResults, infos)`. Managed binding передаёт
   split buffer и per-light info в `Internal_CullShadowCasters`.
   [URP вызов, 19–28](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/ShadowCulling.cs#L19-L28),
   [per-light splitRange, 148–164](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/ShadowCulling.cs#L148-L164),
   [managed → native, 384–397](https://github.com/Unity-Technologies/UnityCsReference/blob/cf583fbe6da6b9fb11e3a1ac03f23df4eb120e9b/Runtime/Export/RenderPipeline/ScriptableRenderContext.cs#L384-L397).

5. Для срезов создаются `ShadowDrawingSettings(cullResults, visibleLightIndex)` и
   shadow renderer lists. Если rendering layers включены в asset, включается
   `useRenderingLayerMaskTest`: сопоставляются rendering-layer masks источника
   и renderer. Источники без casters и невалидные срезы отбрасываются. Само рисование
   — `RenderShadowSlice` → `DrawRendererList`, с viewport и матрицами среза.
   [Setup / GetShadowCasterBounds, 672–753](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/Passes/AdditionalLightsShadowCasterPass.cs#L672-L753),
   [InitRendererLists, 1250–1264](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/Passes/AdditionalLightsShadowCasterPass.cs#L1250-L1264),
   [RenderShadowSlice, 364–375](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/ShadowUtils.cs#L364-L375),
   [rendering-layer test contract](https://docs.unity.com/en-us/engine/6000.0/script-reference/unityengine/rendering/shadowdrawingsettings/userenderinglayermasktest).

Важная деталь: URP оставляет `ShadowDrawingSettings.splitIndex = -1`. Это не
отключение отсечения по граням: по публичному контракту Unity автоматически
увеличивает split index при последовательном создании renderer lists одного
источника с одинаковыми фильтрами.
[Конструктор settings](https://github.com/Unity-Technologies/UnityCsReference/blob/cf583fbe6da6b9fb11e3a1ac03f23df4eb120e9b/Runtime/Export/RenderPipeline/ShadowDrawingSettings.cs#L85-L96),
[официальный контракт splitIndex](https://docs.unity.com/en-us/engine/6000.7/script-reference/unityengine/rendering/shadowdrawingsettings/splitindex).

## Фильтры и граница публичного кода

У renderer есть `shadowCastingMode` — Off/On/TwoSided/ShadowsOnly; режим приёма
теней не заменяет режим отбрасывания. Материал рисует глубину через
`LightMode=ShadowCaster`, сохраняя свои alpha-cutout и другие правила прохода.
`Light.cullingMask` — layer mask влияния света; URP дополнительно умеет отдельный
rendering-layer test для shadow lists. Точные сочетания масок при выборе caster
не следует восстанавливать догадками из одного поля C#.
[Renderer.shadowCastingMode](https://docs.unity.com/en-us/engine/6000.3/script-reference/unityengine/renderer/shadowcastingmode),
[Light.cullingMask](https://docs.unity.com/en-us/engine/6000.3/script-reference/unityengine/light/cullingmask),
[URP ShadowCaster tag](https://docs.unity.com/en-us/engine/6000.0/manual/materials-and-shaders/shaders/writing-custom-shaders-urp/use-built-in-shader-methods/urp-shaderlab-pass-tags).

Конкретные native-тесты bounds и внутреннее spatial acceleration дерево Unity
здесь не опубликованы: `Compute*ShadowMatricesAndCullingPrimitives`, culling и
создание shadow renderer list — extern/bindings. Поэтому нельзя утверждать по
этому исследованию, что обычный URP использует именно octree/BVH, конкретный
AABB-тест или выполняет этот отбор именно на GPU.
[CullingResults native bindings, 45–60](https://github.com/Unity-Technologies/UnityCsReference/blob/cf583fbe6da6b9fb11e3a1ac03f23df4eb120e9b/Runtime/Export/RenderPipeline/CullingResults.bindings.cs#L45-L60),
[native context bindings](https://github.com/Unity-Technologies/UnityCsReference/blob/cf583fbe6da6b9fb11e3a1ac03f23df4eb120e9b/Runtime/Export/RenderPipeline/ScriptableRenderContext.bindings.cs#L37-L41),
[renderer-list binding](https://github.com/Unity-Technologies/UnityCsReference/blob/cf583fbe6da6b9fb11e3a1ac03f23df4eb120e9b/Runtime/Export/RenderPipeline/ScriptableRenderContext.cs#L461-L467).

## Что из этого нужно статичному Bistro

Следующие пункты — вывод для нашего дизайна, а не утверждение о закрытом
алгоритме Unity:

- Сначала выбирать лампы, способные осветить видимую область в пределах
  дальности теней. Проверять объём влияния, а не только положение центра лампы.
- Затем выбирать casters для конкретного spot-frustum / point-face, с учётом
  bounds, `CastsShadows`, масок и материала. Не рисовать исходный полный набор
  indirect commands для каждой лампы/грани.
- GPU должен компактировать подходящие команды и счётчики, а ExecuteIndirect
  использовать полученный count. Для этого не нужен CPU readback.
- Нельзя просто взять camera-visible opaque list: объект вне кадра может
  перекрывать путь от света к видимому receiver. Его тень должна сохраниться.
  Дальность теней также нельзя безоговорочно реализовать проверкой расстояния
  только до центра caster: важна область влияния тени на receivers.
- Исходный `LocalShadowPass.hlsl` уже содержит GPU-проверку `CastsShadows`,
  фильтры материала и `ShadowBoxVisible` для матрицы конкретной грани. Это
  существующая основа для переноса; broad-phase выбора нужных ламп и организация
  списков/граней остаются отдельной частью работы.

Движок не изменён. Сборки, тесты, Unity, profiling и GPU-captures не запускались.
GPU Resident Drawer/BRG не исследовались: они не нужны для доказательства этой
цепочки и не определяют весь штатный shadow renderer-list путь.
