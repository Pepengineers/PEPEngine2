# D3D12 Debug Layer

Приложения с `RenderModule` по умолчанию не включают D3D12 Debug Layer —
ни в Debug, ни в Release. Для диагностики запускайте приложение с отдельным
аргументом:

```text
App.exe -d3ddebug
```

В Rider добавьте `-d3ddebug` в Program arguments конфигурации запуска.
Параметр обрабатывается до создания D3D12 device и работает в обеих сборках.
Если debug layer недоступен, запуск с этим параметром завершится ошибкой;
успешное включение записывается в `Logs/Engine.log`.

Это обычный D3D12 debug layer. GPU-based validation параметр не включает.
GPU-тесты сохраняют собственное явное включение debug layer/GBV.
При внешнем включении проверок через инструменты Windows или профайлер
отсутствие `-d3ddebug` не отключает их принудительно.

Проверка реального startup/device без ручного включения проверок тестом:

```text
BistroStartupTests.exe --check-debug-layer
BistroStartupTests.exe --check-debug-layer -d3ddebug
```

Тест проверяет наличие `ID3D12InfoQueue` у созданного GPU device.

Проверено 2026-11-04: сборки App/BistroStartupTests x64 Debug и Release;
в обеих конфигурациях default Off, явный On и игнорирование `-d3ddebugger`.
Сам Debug App также успешно отрисовал несколько кадров без флага и с флагом.
Логи проверок: `Build/DebugLayerOptIn/`.

Полный startup-тест первоначально нестабильно завершался с кодом `0x87D`
в Debug и Release. Дамп показал освобождение занятого GPU ресурса внутри
`xefgSwapChainDestroy` при переключении Intel FG → Off. Причина была в
`GDX12CommandQueue::Flush`: он ставил новый fence после всей работы очереди,
но ожидал старый fence последней отправки движка, пропуская более позднюю
работу XeSS. Теперь `Flush` ожидает собственный новый fence.

Регрессия в `MemoryAllocatorTests` блокирует native-очередь внешним fence
и проверяет, что `Flush` не возвращается до её разблокировки — как до первой
отправки движка, так и после неё. Исходная реализация эту проверку не проходит.
Это исправление сохраняет явное включение Debug Layer и runtime-переключение FG.
Порядок остановки XeSS также описан в
[Intel XeSS-FG Developer Guide](https://github.com/intel/xess/blob/main/doc/xess_fg_developer_guide_english.md#shutdown).

Проверка исправления 2026-11-04: App x64 Debug/Release собран;
`MemoryAllocatorTests` прошёл в обеих конфигурациях;
`BistroStartupTests` прошёл три обычных запуска Debug и три Release без ошибок
Debug Layer. Release `FrameGenerationTests` также прошёл AMD/Intel FG,
restart/pause/resize и переключение SR; NVIDIA FG штатно недоступен на RTX 3060.
Логи и коды завершения: `Build/startup-verified-*`,
`Build/startup-verified-results.json`. Длительный прогон и multi-GPU не проверялись.
