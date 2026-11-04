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

Полный Debug startup-тест отдельно завершился с кодом `0x87D`.
То же завершение воспроизведено с исходным `RenderModule.cpp` из `2dd50fa`;
эта существующая проблема широкого UI/FG теста текущей правкой не исправлялась.
