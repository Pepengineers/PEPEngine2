# U2. Размещение UI на двух адаптерах

2026-11-10. Ветка `codex/mgpu-ui-u2`, принятый P4 `50ee64fb69868a7686064122946933cad4ddf3d8`, merge base рабочего дерева `48bbaac62fc80f6ffcfcadac473ab7665a106380`.

## Поведение

По умолчанию `Render.UI.Offload=false`. Whole выбирает экран и оба плаката. Canvas выбирает один стабильный CanvasId и сохраняет экран и второй плакат на primary. Screen всегда использует Exact. Для плакатов доступны Exact и Latest. Обе цепочки используют существующий P4 executor, COPY queues и snapshot bank.

Exact объединяет эффекты, raster, export, import и primary consumer в один placed plan. Screen composite использует premultiplied blend `ONE / INV_SRC_ALPHA`. Плакаты копируют RGBA8 в отдельный primary material target. Latest держит до трёх незавершённых публикаций на поток. Следующая стадия использует уже завершённый upstream point. Primary import выполняется после export, а сцена выбирает завершённый local snapshot.

`Render.UI.UpdateHz` принимает 0 или 1–240 Hz. Это верхняя граница для запуска secondary producer каждого плаката. В Exact пропуск запуска оставляет локальный кадр с той же фазой и профилем. `Render.UI.MaxAgeMs` принимает 0–60000. При превышении порога Latest сохраняет совместимый результат и выставляет OverAge по монотонному времени render owner. Quality и resize сохраняют TransferId и создают новый совместимый epoch. Старые readers и их allocations удерживаются до completion.

## Проверки

Primary NVIDIA GeForce RTX 3060 Laptop GPU, secondary AMD Radeon(TM) Graphics. Фикстура требует разные физические LUID.

| Проверка | Результат | State в `Build/U2` |
| --- | --- | --- |
| Debug/x64 и локальный /Z7 | PASS | `build-cross-adapter-ui-debug-r3.json` |
| Локальный minimal INI seed | PASS | `seed-cross-adapter-ui-config.json` |
| CPU routing, CVar ranges, cadence и elapsed age | PASS | `cross-adapter-ui-cpu-r3.json` |
| D3D12 Debug Layer на двух GPU | PASS | `cross-adapter-ui-debug-layer-r3.json` |
| D3D12 GPU-Based Validation на двух GPU | PASS | `cross-adapter-ui-gbv-r3.json` |
| Task-only `git diff --check` | PASS | |

Readback подтвердил экранный фон, alpha blend и глифы, opaque RGBA8 обоих плакатов и согласованность с локальным рендером. Межадаптерный критерий parity допускает разницу более 3 единиц UNORM менее чем для 1% RGB pixels. Exact отобразил экран и оба плаката. Selected проверен для обоих IDs, а неизвестный ID оставил primary route с диагностикой. Под gate secondary COMPUTE две Latest публикации одной поверхности остались в полёте. Cap остановил промежуточный admission, а экранный Exact и чтение предыдущего OverAge результата завершились до открытия gate. Нечётный resize и Medium создали новое разрешение 514×290 и новый epoch на том же stream, сохранив старый материал до завершения reader.

Первый build выявил недостающий include базового render pass. Первый Debug Layer запуск выявил несовместимое состояние composite SRV. Общий root signature объявляет видимость всех shader stages, поэтому placed pass использует `ALL_SHADER_RESOURCE`. Финальные Debug Layer и GBV прошли на обеих картах. Runner сохранил SHA-256 для исходников, исполняемого файла, INI и шейдеров, включая разрешённый путь за junction Build/bin. `processes-final.json` фиксирует проверку процессов и отсутствие дочерних workloads.

## Подключение

Интегратор добавляет production project registrations, два Bistro mesh/material bindings, stable frame slots, shared accessor к backbuffer tracker и вызовы в RenderModule/P2c. Standalone fixture проверяет native transfer и primary render targets. Живой ввод и Present в Bistro, production resize и показатели производительности в этом прогоне не проверялись.
