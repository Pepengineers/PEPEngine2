# Bistro: захваты порядка систем и стоимости теней

Дата: 2026-11-02. Исследование без изменения runtime-кода и настроек сцены.
Задание на исправление: [render-preparation-and-shadow-efficiency.md](../tasks/render-preparation-and-shadow-efficiency.md).

## База и воспроизведение

- Репозиторий при исследовании: `33ca359`, merge в 20:07:29 +03:00.
- Запущен существующий `Build/bin/x64/Release/App/App.exe`, время сборки 20:02:10 +03:00.
  Точное соответствие EXE этому merge-коммиту не доказано. Перед A/B заново собрать baseline.
- EXE SHA-256: `D43FEA2B68A746F6536047C1B8954FD8864B2F7F44D16D3C632FB3F54FAB8662`.
- PDB SHA-256: `4BD369ABA23C666F37F4F19B521430B0BE178CCC23C204D87A34B22EBAFEAC67`.
- RTX 3060 Laptop GPU, драйвер 610.88; Release x64, окно 1280×800.
- Runtime INI содержит только `Render.NumFrames = 3`; остальные настройки — defaults этой сборки.
  В CPU-стеках присутствует XeSS. Счётчик GPC Frequency в трёх исследованных shadow-интервалах — около 900 MHz.
- Bistro стартует у входа; камера неподвижна, игровые объекты и частицы продолжают симуляцию.
  Это один сценарий, не результат для всех сцен и GPU.

Файлы находятся локально в `D:\Project\PEPEngine2\Build\Captures\2026-11-02-system-order`.
Большие захваты игнорируются Git. В этой папке сохранены `evidence.json`, `ngfx-actions.json`,
`cpu-summary.json`, PIX-запросы, стеки и небольшие скрипты анализа.

| Захват | Назначение и качество |
| --- | --- |
| `bistro-clean-12frames.ngfx-gputrace` | Основной GPU-захват: запуск через native Nsight MCP, trigger frame 180, 12 кадров, Top-Level Triage, PM bandwidth 1000. Нет overflow; удалены отсутствующие метрики трёх шейдеров. PC sampling выключен. |
| `bistro-warm-timing.wpix` | PIX attach к прогретому обычному запуску, PID 37324, главный поток 47224. Длительность файла 7092,4649 мс, потеряно 0 ETW events / buffers. Есть CPU sampling, scheduling и GPU API work; нет CPU PIX scopes и GPU frame definitions. |
| `bistro.rdc` | RenderDoc D3D12, первый отрисованный кадр: 43 823 draw-события, 44 882 action-tree события. Indirect-вызовы раскрыты в дочерние draws; это не число CPU draw-вызовов. |
| `bistro-warm-top-level.ngfx-gputrace` | Длинный 120-frame захват переполнил event/timestamp buffers; исключён из количественных выводов. |
| `bistro-timing.wpix` | Неудачный ранний PIX launch: только 1649,8342 мс загрузки. Для анализа стабильных кадров не использован. |

Основной Nsight SHA-256: `24F774DF91A97985263F97875994B02E4D15CB6C17BBC38B8B45A3CD63BF3E7F`.
Прогретый PIX SHA-256: `B4EBFA04355B1B2AD93A0C8A27A300881E43BC9FE28192DEB9B593F5F4C59F49`.
RenderDoc SHA-256: `7894490066F7F030CA38EF25FAB75CDA3CE584962E25D827B8FAAFAE69DF7C5A`.

## Что измерено

Native Nsight trace `gpu-trace-edc7d66d5f8e7d57`: device 0, graphics queue 0,
диапазон 0–522,129280 мс, 12 кадров. Интервалы кадров 38,605–50,631 мс,
среднее 43,511 мс. Это интервалы под профилировщиком, не чистый benchmark FPS.

| Метрика за весь GPU-захват | Значение |
| --- | ---: |
| GR Engine Active | 98,72% |
| SM Throughput | 21,34% |
| L2 Throughput | 23,26% |
| VRAM Throughput | 24,95% |
| PCIe Throughput | 14,52% |
| Sync Compute in Flight | 46,48% |

Низкий SM throughput при занятом GR engine не доказывает конкретный вид stall.
Однако постоянное голодание GPU из-за CPU не объясняет этот участок: command-buffer
интервалы почти непрерывны. В trace 9540 actions, 11916 barriers, 12 command buffers,
24 submits включая Present. На кадр — в среднем 795 actions и 993 barriers.

Самая заметная повторяющаяся последовательность соответствует локальным теням:
`CopyTextureRegion → Dispatch(clear) → Dispatch(cull) → ExecuteIndirect`.
Атрибуция основана на порядке API и исходнике local-shadow pass; именованных Release-маркеров нет.

| Кадр Nsight | Весь кадр, мс | Local-shadow sequence, мс | Dispatch внутри неё, мс | Draw внутри неё, мс | Copies, мс | Views |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 40,539 | 17,629 | 9,429 | 4,637 | 2,621 | 168 |
| 1 | 38,605 | 15,516 | 7,314 | 4,633 | 2,622 | 168 |
| 2 | 49,832 | 26,658 | 18,483 | 4,616 | 2,617 | 168 |

Для кадра 0 границы sequence — 9,505120–27,133920 мс, actions 11–682.
В 12 кадрах 167–168 views, 334–336 clear/cull dispatches на кадр. Стоимость sequence
15,493–26,761 мс. Трёхкадровый ритм повторяется; почти вся разница находится в dispatch,
а draw и copy близки. Причина ритма и связь с frame-slot memory требуют A/B.
В local-shadow интервалах кадров 0/1/2 PCIe throughput 22,99/19,52/31,32%,
SM throughput 4,62/5,26/3,06%; GPC frequency практически одинакова.

PIX: в диапазоне 0–5000 мс главного потока получены 982 CPU-сэмпла,
826 уникальных stack IDs. Расшифрованы 825 ненулевых IDs, локальные PDB разрешены
через `tc_set_symbol_load_options(resolveUncachedSymbols=true)`.

| Функция присутствует в стеке | Сэмплы | Доля от 982 |
| --- | ---: | ---: |
| `RenderModule::OnRender` | 895 | 91,1% |
| `GDX12RenderGraph::Execute` | 648 | 66,0% |
| `GDX12LocalShadowPass::Execute` | 365 | 37,2% |
| `GDX12CommandList::ResourceBarrier` | 172 | 17,5% |
| `RenderModule::CollectReflectionScene` | 90 | 9,2% |

Это inclusive sampling, включая драйверные дочерние вызовы, с пересекающимися долями.
Проценты нельзя складывать, считать wall time кадра или временем блокировки fence.

## Что подтверждено кодом

1. В `Apps/App/src/App.cpp:71` Bistro переопределяет `Update`, пропуская `RenderModule::BeginFrame`.
   Базовый `Apps/App.Base/src/App.cpp:497` вызывает его. Пропущены lifecycle frame-generation,
   токены/маркеры и обработка нужного recreate swapchain. Поломка FG отдельно runtime не воспроизведена.
2. `Apps/App.Base/src/RenderModule.cpp:911` выполняет scene collection и CPU shadow visibility
   в `OnRender`, а `GDX12DeferredLightingPass.h:64` строит CPU light grid внутри `Execute`.
   Граница «готовый CPU snapshot → запись команд» сейчас не соблюдается.
3. `GDX12LocalShadowPass.h:146–172` повторяет culling для каждой dirty face по полному
   `IndirectCommandsCache`; `LocalShadowPass.hlsl` отбрасывает неподходящий static/dynamic класс
   только после чтения команды и instance. RenderDoc показывает общий capacity 24 777.
4. `GDX12DeviceResources.cpp:74` создаёт indirect templates в `UPLOAD`.
   `GDX12FrameConstants.cpp` аналогично размещает instance/material/light/transform buffers.
   `GDX12UploadBuffer.h` действительно создаёт `D3D12_HEAP_TYPE_UPLOAD`.
   Многократные GPU-чтения этих данных — проверяемый кандидат на лишний PCIe-трафик,
   но величина выигрыша от `DEFAULT` ещё не измерена.
5. CPU collection каждый кадр пересчитывает подписи и собирает casters полным обходом.
   `ResourceBarrier` создаёт временный `std::vector` при каждом вызове.
6. Текущее GPU culling → barriers → indirect draw сохраняет зависимость данных.
   Перенос GPU culling в CPU gameplay или перестановка всех cull перед всеми shadow draws
   небезопасны: local-shadow pass переиспользует Commands/Counter для следующей face.

Для буферного A/B есть прямое основание в документации Microsoft:
[`UPLOAD` рекомендуют для write-once/read-once; многократные чтения нужно профилировать против `DEFAULT`](https://learn.microsoft.com/en-us/windows/win32/direct3d12/uploading-resources).
Вариант GPU upload heap и его ограничения описаны
[NVIDIA](https://developer.nvidia.com/blog/optimizing-dx12-resource-uploads-to-the-gpu-using-gpu-upload-heaps/).

## Границы проверки

RenderDoc прочитал action/resource metadata и не вернул medium/high сообщений, но replay
в этом MCP-сеансе не дал GPU counters и не смог выгрузить финальную текстуру; pipeline state
оказался пустым. Это ограничение инструмента/сеанса, не доказательство исправности рендера.
Screenshot из Nsight подтверждает готовую сцену с частицами; visual equivalence патча не проверялась.
Community Rust analyzer отвергает текущий WRPV v11, поэтому численные GPU-запросы выполнены
официальным native MCP. Все запущенные для исследования экземпляры App завершены.
