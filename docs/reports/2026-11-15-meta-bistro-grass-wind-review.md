# Проверка травы и ветра перед переносом частиц

Проверены `234fd2a4`, `4f5e3798`, `879afae6` относительно `7497551a` и связанный
код grass/shadows. С учётом уточнения пользователя о намеренном отсечении травы
блокирующих замечаний не осталось. Runtime-код в этой проверке не менялся.

## Что проверено

- Создаются только grass/wind additions. Opt-in advanceWhilePaused действует
  в обычном Meta, не снимая паузу остальной симуляции и не меняя default App.
- Root uploads сохраняют DEFAULT-буферы; обычный путь исключает диагностические
  probe views и readback. PrepareGrass не вызывает полный CollectReflectionScene.
- Последняя оптимизация строит full/reduced/card vertices один раз на patch/кадр;
  views выбирают indices. Depth/GBuffer повторно используют camera visibility,
  для дополнительных patches предусмотрены отдельные index/argument buffers.
  Shadow views имеют свою видимость и не используют camera HiZ/distance cutoff.
- HiZ строится из текущей mesh opaque depth до grass depth. Reverse-Z reduction
  использует min; culling проверяет ближайшую глубину bounds относительно дальнего
  occluder, near-plane crossing оставляет видимым. Пирамида и её mips объявлены
  в Meta accesses; переходы отделяют UAV-записи от shader reads.
- Трава записывает depth и пять GBuffer targets до AO/SSR; previous world/eye,
  wind и interaction берутся из BeginFrame history, shadow views его не продвигают.
- Sun mesh bounds остаются кэшированными, grass bounds объединяются отдельно.
  Local cache инвалидирует только MapsValid затронутых старых/новых grass faces,
  не сбрасывая static ListsValid. Деформация при постоянных bounds не пересчитывает
  receiver selection. Grass рисуется после очистки и mesh draw, до publication.
  Каскадные grass draws используют соответствующий viewport atlas region.

## Согласованная граница отсечения

В `879afae6` CPU patch culling использует `GrassLocalBounds(source,false)`
(`SceneGrass.cpp:103,177`), а GPU CullCS строит bounds прямого стебля без
wind/contact displacement (`GrassExpand.hlsl:223–227`). Сгенерированная согнутая
геометрия может выходить за них: возможны отсечения стеблей и теней возле границы
frustum, а также ложное occlusion rejection рядом с препятствием.

Изначально это было отмечено как ошибка консервативности culling. Пользователь
уточнил, что такое отсечение было его пожеланием. В текущем задании это считается
принятой границей качества, не блокером и не поручением исправить/расширить bounds.
Не откатывать оптимизацию и не требовать её переделки перед частицами.

## Свидетельства и ограничения

`Build/Verification/meta-bistro-grass-wind/` содержит успешные build/run и
operation dumps первоначального переноса, до двух последующих оптимизаций.
Отчёт исполнителя фиксирует визуальное подтверждение пользователем этого этапа;
называть эти старые dumps проверкой build-once/HiZ было бы неверно.

Дополнительно проверены свежие `Build/bin/x64/Release/App.MetaRenderGraph/Logs/`:
Engine.log содержит запуск 2026-11-15 18:13–18:14 UTC, компиляцию GrassExpand/GrassHiZ;
opaque-steady-operations.txt содержит `build all LOD vertices`, отдельные
`cull indices`, grass shadow draws и последующие AO/SSR/WBOIT. В проверенном
интервале запуска нет ERROR/FATAL. Начальная камера не видит patch, поэтому
этот dump не доказывает фактическое выполнение camera HiZ на видимой траве.
В свежем логе не зафиксировано штатное завершение; оно здесь не утверждается.

`git diff --check 7497551a 879afae6` прошёл. Новые сборки, запуски, тесты,
профилирование и матрицы не выполнялись. Текущую картинку, точность motion vectors,
альтернативные режимы и performance новой оптимизации независимо не измеряли.

Следующий этап: `docs/tasks/2026-11-15-meta-bistro-particles.md`.
