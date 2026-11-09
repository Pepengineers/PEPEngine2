# U0. Offscreen ImGui

2026-11-10. Ветка `codex/mgpu-ui`, база `97f4fac4e4c4767c2495a0544c3cbce506780857`.

## Проверки

| Проверка | Результат | State в `Build/U0` |
| --- | --- | --- |
| Локальные зависимости, SHA-256 и размер LFS-объектов | PASS | `prepare-local-dependencies.json`, `lfs-hydration.json` |
| Debug/x64, `OffscreenUITests.vcxproj`, локальный /Z7 | PASS | `build-offscreen-ui-debug-r4.json` |
| D3D12 Debug Layer | PASS | `offscreen-ui-debug-layer-seeded-r4.json` |
| D3D12 GPU-Based Validation | PASS | `offscreen-ui-gbv-seeded-r4.json` |
| Task-only `git diff --check` | PASS | |

GPU readback проверил Win32 input, экранный fallback с тем же draw data и композицию premultiplied RGBA по всему кадру. Также проверены clipping после масштабирования, UNORM и sRGB bindings, отклонение ресурса другого устройства, шрифт после смены и уничтожения атласа, resize при заблокированном первом submission и backpressure с последующим reuse. Оба native прогона завершились без D3D12 ошибок. Фикстура выбирает второе физическое устройство при наличии, иначе WARP. Лог не фиксирует, какой из этих вариантов использован.

Первый запуск остановился на Bistro CVar `App.Water.HeightScale` в конфигурации standalone fixture. Повторные прогоны использовали локальный `[ConsoleVariables]` seed. Его SHA-256 `33395d44fd4eda9d2a0c6a0c7940663987c2e3ef91b52484884276c440b9cb8c`. На 1× и 2× glyph regions сравниваются попиксельно с обычным ImGui DX12-бэкендом на том же масштабе и фоне. На краях линейная фильтрация прозрачных texels атласа может уменьшать и RGB, и alpha. Фикстура проверяет частичную alpha, яркое ядро и различие coverage между ревизиями. Производственный код после первой сборки не менялся. Runner сохранил SHA-256 входов до и после каждого запуска.

Обычный App, ручное docking/settings взаимодействие и межадаптерный перенос UI в U0 не проверялись. GPU производительность и стоимость повторной загрузки атласа не измерялись.

## Граница ресурсов placed pass

`DeclareCompleteAccesses` охватывает внешние и разделяемые ресурсы. Граф видит выходной target, привязанные внешние текстуры, общие constants и transients, transfer views и алиасы. Их состояния и зависимости задаёт исполнитель графа.

Offscreen ImGui владеет слотами вершинных и индексных upload buffers, а также дескрипторами. Каждый recording создаёт собственные RGBA8 atlas textures и staging. Запись копирует их и переводит в `PIXEL_SHADER_RESOURCE` в том же DIRECT list, который растеризует UI. Эти ресурсы принадлежат одному batch. `KeepAlive` удерживает backend, буферы и дескрипторы, а кольцо запрещает повторную запись и перепривязку занятого слота до retirement. Recorder записывает команды в предоставленный list. Submit, Wait и Signal выполняет runtime.

Если атлас станет общим для нескольких invocations, graph должен видеть его upload и reads, а draw должен зависеть от готовности нужной версии. В U0 цена копирования атласа на каждую запись пока не измерялась.
