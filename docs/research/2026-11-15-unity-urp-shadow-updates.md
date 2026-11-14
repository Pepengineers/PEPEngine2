# Как URP обновляет realtime-тени

Проверено по официальным исходникам Unity-Technologies/Graphics, 15 ноября 2026.
Движок PEPEngine2 не изменялся; сборки, Unity и GPU-проверки не запускались.

## Вывод

Для обычной камеры URP заново рисует выбранные realtime-тени при каждом её
рендере: spot — один срез, point — до шести граней, main directional — каскады.
Неподвижность лампы и объектов сама по себе не отключает этот рендер. Общего
межкадрового кэша глубины с проверкой изменений света/геометрии в исследованных
путях нет. Это вывод из вызова проходов, создания очищаемого атласа и последующих
draw-команд, а не из названий полей.

Проверены две конкретные ревизии:

| Источник | Commit | Версия пакета |
|---|---|---|
| `6000.3/staging` | `4064d5bb6a1e3748a7cdddc244b89934ad1c3145` | [URP 17.3.0, Unity 6000.3](https://github.com/Unity-Technologies/Graphics/blob/4064d5bb6a1e3748a7cdddc244b89934ad1c3145/Packages/com.unity.render-pipelines.universal/package.json#L1-L5) |
| `master` | `0efc28ea742b1966b9dd9cc4489e58b986a13faf` | [URP 17.7.0](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/package.json#L1-L5) |

Обе вершины датированы 26 октября 2026. Версия пакета в staging — характеристика
этой ветки, не утверждение о составе каждого выпущенного Unity Editor.

## Цепочка обычного рендера

В 17.3 RenderGraph-путь вызывает `Setup` и затем `Render` обоих shadow passes
при рендере камеры: [UniversalRendererRenderGraph.cs, 743–756](https://github.com/Unity-Technologies/Graphics/blob/4064d5bb6a1e3748a7cdddc244b89934ad1c3145/Packages/com.unity.render-pipelines.universal/Runtime/UniversalRendererRenderGraph.cs#L743-L756).
Compatibility-путь также вызывает setup и ставит проходы в очередь:
[UniversalRenderer.cs, 932–933](https://github.com/Unity-Technologies/Graphics/blob/4064d5bb6a1e3748a7cdddc244b89934ad1c3145/Packages/com.unity.render-pipelines.universal/Runtime/UniversalRenderer.cs#L932-L933),
[1120–1124](https://github.com/Unity-Technologies/Graphics/blob/4064d5bb6a1e3748a7cdddc244b89934ad1c3145/Packages/com.unity.render-pipelines.universal/Runtime/UniversalRenderer.cs#L1120-L1124).

Для локальных теней `Render` создаёт атлас с очисткой и depth-write attachment;
исполнение обходит выбранные срезы и вызывает `RenderShadowSlice`:
[AdditionalLightsShadowCasterPass.cs, 1018–1052](https://github.com/Unity-Technologies/Graphics/blob/4064d5bb6a1e3748a7cdddc244b89934ad1c3145/Packages/com.unity.render-pipelines.universal/Runtime/Passes/AdditionalLightsShadowCasterPass.cs#L1018-L1052),
[877–908](https://github.com/Unity-Technologies/Graphics/blob/4064d5bb6a1e3748a7cdddc244b89934ad1c3145/Packages/com.unity.render-pipelines.universal/Runtime/Passes/AdditionalLightsShadowCasterPass.cs#L877-L908).
Для main light действует аналогичный очищаемый атлас и рендер каскадов:
[MainLightShadowCasterPass.cs, 482–512](https://github.com/Unity-Technologies/Graphics/blob/4064d5bb6a1e3748a7cdddc244b89934ad1c3145/Packages/com.unity.render-pipelines.universal/Runtime/Passes/MainLightShadowCasterPass.cs#L482-L512).

В master вызовы организованы через `ShadowPassGroup`; helper реально выполняет
`DrawRendererList` после установки viewport и матриц:
[ShadowPassGroup.cs, 21–44](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/Passes/ShadowPassGroup.cs#L21-L44),
[ShadowUtils.cs, 364–375](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/ShadowUtils.cs#L364-L375).

## Когда срезы не рисуются

Это отбор текущего рендера, не сохранение старой карты до dirty-события:

- Realtime shadows отключены для pipeline/камеры, либо используется специальный
  offscreen depth target.
- Источник не входит в выбранные visible lights, тени выключены или strength
  равен нулю. Локальный атлас обслуживает spot/point; main light исключён.
- Нет подходящих shadow casters по `GetShadowCasterBounds`, срез невалиден либо
  источник не поместился в атлас.
- Baked/mixed lighting может использовать заранее запечённую shadowmask даже
  при пустом realtime-проходе. Это не кэш realtime depth-карты.

Проверки: [AdditionalLightsShadowCasterPass.cs, 347–365](https://github.com/Unity-Technologies/Graphics/blob/4064d5bb6a1e3748a7cdddc244b89934ad1c3145/Packages/com.unity.render-pipelines.universal/Runtime/Passes/AdditionalLightsShadowCasterPass.cs#L347-L365),
[487–565](https://github.com/Unity-Technologies/Graphics/blob/4064d5bb6a1e3748a7cdddc244b89934ad1c3145/Packages/com.unity.render-pipelines.universal/Runtime/Passes/AdditionalLightsShadowCasterPass.cs#L487-L565);
[ShadowUtils.cs, 988–1009](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/ShadowUtils.cs#L988-L1009).

Поле `m_ShadowRequestsHashes` не задаёт cadence. В 17.3 вычисление hash ограничено
Editor/Development и при изменении только сбрасывает флаги диагностических
предупреждений: [373–393](https://github.com/Unity-Technologies/Graphics/blob/4064d5bb6a1e3748a7cdddc244b89934ad1c3145/Packages/com.unity.render-pipelines.universal/Runtime/Passes/AdditionalLightsShadowCasterPass.cs#L373-L393).
Повторное использование массивов, constant buffers или размещения RTHandle также
не означает сохранение содержимого теней.

## Исключение master: XR QuadViews

В 17.7 есть настоящая ветка `ReuseCachedAtlas`, но её флаг задаётся конкретно для
XR `TwoPassQuadViews`: caching включён для этого layout, reuse — для его inner
pass. Это повторное использование карты между XR-проходами, а не обнаружение
неподвижной сцены между кадрами:
[UniversalRenderPipeline.cs, 1189–1224](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/UniversalRenderPipeline.cs#L1189-L1224).

Первый проход сохраняет карту, inner pass импортирует её без очистки и только
восстанавливает globals вместо draw; после reuse допускается discard:
[AdditionalLightsShadowCasterPass.cs, 527–535](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/Passes/AdditionalLightsShadowCasterPass.cs#L527-L535),
[1292–1312](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/Passes/AdditionalLightsShadowCasterPass.cs#L1292-L1312),
[1348–1368](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/Passes/AdditionalLightsShadowCasterPass.cs#L1348-L1368).
Для main light импорт устроен так же:
[MainLightShadowCasterPass.cs, 499–519](https://github.com/Unity-Technologies/Graphics/blob/0efc28ea742b1966b9dd9cc4489e58b986a13faf/Packages/com.unity.render-pipelines.universal/Runtime/Passes/MainLightShadowCasterPass.cs#L499-L519).

## Значение для переноса

Межкадровый кэш статичных локальных теней PEPEngine2 может быть собственной
оптимизацией, но его нельзя обосновывать как повторение обычного URP. Изученные
исходники URP не дают готового алгоритма dirty/invalidation для такого кэша.

Граница проверки: исследован публичный C#/RenderGraph-код выбранных ревизий.
Закрытые native-реализации Unity culling/renderer lists и custom Renderer Features
не исследовались. Вывод относится к штатным realtime shadow passes, а не к HDRP,
запечённым теням или всем историческим/будущим версиям Unity.
