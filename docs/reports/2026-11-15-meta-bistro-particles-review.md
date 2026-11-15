# Проверка переноса частиц и кандидаты на оптимизацию

Проверен `d42d15f9` относительно `428f556e`, задание переноса и вызываемый shared
particle runtime. Ожидаемая нагрузка: восемь эмиттеров, общий бюджет 500000,
WeightedAlpha/Additive, primary GPU, paused world с отдельным временем частиц.
Незакоммиченные изменения травы и сцены в проверяемый diff не включены.

Перенос подключает существующую GPU-симуляцию и draw preparation, затем дописывает
weighted particles в общий WBOIT со стеклом и добавляет additive/sorted composition
после resolve. Блокирующих ошибок корректности по проверенному коду не найдено.
Ниже перечислена подтверждённая лишняя работа и кандидаты, выигрыш которых ещё
нужно измерить. Порядок — рекомендация для следующих итераций, не рейтинг по PIX.

## Стоит убрать лишнюю работу

### 1. Motion-проход без потребителя в текущем ordinary pipeline

Место: `Engine/Engine.RendererDX12/src/GDX12ParticlesPass.cpp:324,485–496`;
`Apps/App.MetaRenderGraph/src/Techniques/ScenePostUI.cpp:230`.

BeginFrame создаёт полноразмерную RGBA32F MotionTexture. DrawWeightedAndMotion
после цветовых weighted draws очищает её и повторно рисует все particle modes,
включая mesh. MotionPS снова вызывает Shade: depth/alpha/atlas работа повторяется.
Обычный ScenePostUI::AppendOpaque выводит только HDR; Motion Blur, reconstruction
и FG выключены. Потребителя ParticleMotion в этом пути сейчас нет.

Минимальная оптимизация — включать создание/очистку/рисование feature motion
по реальному запросу потребителя. Сохранять previous-state/history внутри
simulation/draw данных и существующий default App contract; при отключённом
выходе публиковать nullptr. При последующем переносе Motion Blur/reconstruction
включать output и делать необходимый reset, а не удалять motion поддержку.
Без изменения продолжается дополнительная растеризация видимых частиц каждый кадр.

### 2. Избыточные boundary states и слишком широкие accesses

Место: `Apps/App.MetaRenderGraph/src/Techniques/SceneParticles.cpp:49,113–124,139–151`.

Packet принудительно заменяет Required каждого ресурса на GENERIC_READ или
ALL_SHADER_RESOURCE. Например, после mesh WBOIT accumulation/revealage уже в RT,
но перед particle weighted packet graph переводит их в SRV; helper возвращает
в RT, а tracker.Finish снова возвращает в SRV. Для продолжения общего WBOIT
первый переход RT → SRV → RT не нужен. Это state transitions, не копирование pixels.

Кроме того, preparation импортирует весь NativeResources с ReadWrite, а тот же
набор переходит в обе draw-фазы вместе со всеми simulation pools. Draws читают
компактный Draws/Indices/Payloads, а не Pool/Dead/Alive. MotionTexture и immutable
Firefly VB попадают даже в этап culling; неподходящие write declarations создают
ложные зависимости и расширяют работу state tracker.

Минимальная оптимизация — точные списки ресурсов/режимов для init, simulation,
cull, weighted/motion и composite; сохранить заданные packet boundary states.
Weighted packet должен продолжать RT accumulation/revealage, resolve уже
объявляет их SRV чтение. Immutable VB — read, motion — write только в своём
проходе; pools нужны simulation/cull, не raster composition.
Не снимать OwnerRecording механически: helpers меняют ReadIndex/BurstTotal,
resource tracker и publication pointers. Для worker recording сначала нужны
неизменяемые подготовленные bindings и корректное владение CPU state.

## Кандидаты на измерение

### 3. Аппаратный depth test вместо только pixel-shader rejection

Место: `Engine/Engine.RendererDX12/src/GDX12ParticlesPass.cpp:72,475,508`;
`Engine/Engine.RendererDX12/Shaders/ParticleRaster.hlsl:92`.

Все particle PSO выключают DepthEnable и рисуют без DSV. Скрытый стеной фрагмент
всё равно входит в PS и читает SceneDepth, прежде чем discard. При этом проверка
расположена до дорогого atlas/material shading: нельзя утверждать, что скрытые
фрагменты сейчас всегда выполняют полный Shade.

Кандидат — read-only opaque DSV с reverse-Z GREATER_EQUAL, без depth writes,
и DEPTH_READ | shader-read state. Сохранить sampled depth для soft particles и
тот же контракт перекрытия. Это может уменьшить PS invocation/overdraw на
скрытых частицах; эффект зависит от реальной сцены и требует замера. Само по себе
включение DSV не отменяет soft fade и не должно менять blending/картинку.

### 4. Два global atomics на каждую видимую частицу в culling

Место: `Engine/Engine.RendererDX12/Shaders/ParticleRender.hlsl:35,57`.

Каждый прошедший frustum/size/alpha tests particle резервирует один Draws index
и один blend-mode index через InterlockedAdd. Dispatch выполняется на emitter,
поэтому blend/geometry mode внутри него одинаковы. При большой видимой части
бюджета это кандидат на contention; точная стоимость без GPU timeline неизвестна.

Минимальный вариант — WavePrefixCountBits/WaveActiveCountBits для прошедших lanes,
одна резервация каждого счётчика на wave и prefix offsets; сохранить non-WaveOps
fallback. Начать именно с culling, не менять RNG, spawn или simulation scheduling.
Проверить сохранение связей Draws/Indices/Keys/Payloads и global sort contract.
Не заменять это уменьшением CountScale или остановкой невидимой simulation.

### 5. Local-only Firefly mesh остаётся в UPLOAD

Место: `Engine/Engine.RendererDX12/src/GDX12ParticlesPass.cpp:103–110`.

SceneParticles включает SetLocalExecution(true). В этой ветке неизменный
FireflyVertices создаётся как EBufferType::Upload, что в GDX12UploadBuffer
соответствует D3D12_HEAP_TYPE_UPLOAD. В обычной другой ветке уже используется
DEFAULT + staging. CopyData выполняется один раз, не каждый кадр, но GPU затем
читает VB из upload heap при цветовой и motion отрисовке светлячков.

Перенести local-only VB в DEFAULT с одним upload packet в Meta, без отдельного
внутреннего submit/Flush. Сохранить staging до fence. Меш маленький (120 vertices),
поэтому cache может почти скрывать стоимость: не обещать заметного FPS gain.

## Что не является обнаруженной проблемой

- `particle B append/global SortedAlpha radix` — имя packet, а не доказательство
  лишней сортировки. В текущих восьми authored effects нет SortedAlpha; State::Sorted
  false, Sort вызывается только под `if (state.Sorted)`. Global finalization нужна
  для indirect args всех групп.
- Pool initialization packet планируется каждый кадр, но InitializePools пропускает
  уже initialized pool с той же ResetRevision. Нет повторной полной очистки
  500000 частиц. Убрать пустые init packets можно позже как небольшую CPU-правку.
- Emit(count) не означает повторное создание всего бюджета: PrepareSpawn ограничивает
  spawn числом dead slots, а dispatch и culling используют GPU counters/indirect args.
- Pools пересоздаются при изменении topology/capacity; steady frame не вызывает
  соответствующий Flush. Render constants обновляются для восьми emitters,
  не для 500000 частиц. Перенос не возвращает обход всей статической Bistro.
- Сохранены отдельные DrawEnabled/SimulationEnabled, emitter pause, opt-in обход
  только world pause, общий WBOIT без повторной очистки, opaque depth после травы,
  зависимость fireflies от interaction field. CPU sorting/readback не добавлены.

## Проверки и вывод

Проверены сохранённые `Build/Verification/meta-bistro-particles/`: успешная сборка
и bounds-build, ordinary-run без ERROR/FATAL, операции simulation A/B, preparation,
weighted append, один WBOIT resolve и HDR composite. Отчёт переноса фиксирует
подтверждение движения частиц пользователем; после исправления dust/steam bounds
визуальное подтверждение в отчёте ещё не зафиксировано. Не выдавать это за новую
визуальную приёмку. `git diff --check 428f556e d42d15f9` прошёл.

Новых сборок/запусков, тестов, PIX/Nsight, измерений и изменений runtime-кода
не выполнялось. Предлагаемый порядок: 1 → 2 → измерить 3/4; 5 — небольшая
правка размещения ресурса. После каждого изменения сохранять scene fidelity и
проверять эффект отдельно. Работоспособность переноса по исходникам/имеющимся
свидетельствам подтверждена; количественная эффективность ещё не измерена.
