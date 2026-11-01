# Runtime reconstruction: результат 2026-11-02

Реализованы независимые runtime SR/FG с GPU wait на смене режима, явным restart и requested/effective status. Defaults: XeSS Ultra Quality Plus, FG Off. Недоступный SR переходит в Native, FG — в Off, с причиной в статусе и логе. Сцена не перезагружается.

HDR-вывод и ACES исключены по последнему указанию пользователя. Существующие Reinhard и sRGB сохранены: `scene-linear HDR → Native/XeSS/FSR/DLSS/NIS → Reinhard/sRGB → FG presentation → Present`. Это SDR RGBA8 output; внутренние float HDR buffers остаются частью освещения. AMD/NVIDIA/Intel FG интегрированы как 2x presentation backends, с требуемыми Reflex/PCL или XeLL и передачей combined depth/feature motion.

Стабильные links переживают смену SR. На границе кадра обновляются render grids, jitter и histories; pause/minimize/resize останавливают FG с ожиданием обращений к старым ресурсам. Повторный вызов pause/resume с тем же состоянием не сбрасывает history. Без компонентов CPU-проверки skinning/grass/fluid остаются, но соответствующая GPU-деформация, генерация травы, симуляция/рисование воды пропускаются; fluid links возвращают opaque depth/motion.

Обновлены согласованные headers/libs/runtime: Streamline 2.14.16 (DLSS 310.9.16.0), FSR SDK 2.4.0 и XeSS SDK 3.0.2. Intel SR 2.0.2.68 уже совпадал с пакетом. Отдельный patched FidelityFX 1.1.4 для SSSR/DoF/denoiser сохранён. Deployment копирует DLL и notices независимо от timestamp архива; проверено совпадение байтов исходных пакетов и Debug/Release App outputs.

## Проверки

Аппаратный стенд: RTX 3060 Laptop, D3D12 debug layer.

- App Release/Debug x64 собраны MSBuild 18 с HostX64. Для проверки использованы `/Z7 /GL-`; глобальные настройки toolchain не менялись. Существующие compiler conversion и vendor PDB/linker warnings остаются.
- Новые ReconstructionTests и FrameGenerationTests собраны в Release/Debug x64; CPU validation прошла в обеих конфигурациях. GPU проверки ниже выполнялись в Release.
- `ReconstructionTests --cpu`: валидация CVar и совместимости quality/provider.
- Существующий `PBRMathTests`: assert-проверки исходной Reinhard/sRGB кривой и PBR math прошли.
- `ReconstructionTests` (GPU mode по умолчанию): все пять SR providers, quality/restart, HDR readback, native fallback и odd resize; Streamline включён, все пять работали без fallback.
- `FrameGenerationTests --cpu` и GPU mode по умолчанию: provider/restart/pause/resize и смена всех SR при выбранном FG. AMD: 33 успешных generation-dispatch frames; Intel: 32 дополнительных presented frames. D3D12 validation без warnings/errors. NVIDIA FG корректно остался Off с `eErrorNoSupportedAdapterFound`.
- `ObjectDataJobsTests --render-bistro`: 36 полных кадров Bistro, все пять SR providers, независимые AMD/Intel FG с ненулевой генерацией, принудительный restart, camera cut, odd resize, AO и motion blur transitions, 346 существующих движущихся props. D3D12 validation без warnings/errors. Для FG proxy убран избыточный clear backbuffer: финальный fullscreen output перезаписывает его целиком.
- Проверка source lifecycle и integration двумя независимыми reviewers: подтверждённых существенных ошибок не найдено.
- XML MSBuild проектов и `git diff --check` прошли. Существовавшие до задачи пользовательские изменения сохранены.

Логи находятся в игнорируемом `Build/SDKUpdate`: `build-App-{Debug,Release}.log`, `gpu-ReconstructionTests-SL.log`, `gpu-FrameGenerationTests-final.log`, `gpu-BistroReconstruction.log`. Тест Bistro выполняется с видимым окном: Intel status считает дополнительные показанные кадры, а не только dispatches.

## Ограничения проверки

DLSS-G на совместимом RTX 40/50 не запускался: этот GPU подтверждает только capability fallback. Синтетические проверки и короткий Bistro smoke не подтверждают качество интерполяции в движении, длительную стабильность и pacing под реальной нагрузкой. Все аппаратные комбинации SR/FG, драйверы, sleep/monitor transitions не покрыты. HDR-вывод, ACES, UI и MFG не добавлялись.

[Полный граф](../rendering/current-render-graph.md) · [CVar/API](../console-variables.md#апскейлеры-и-генерация-кадров) · [SDK manifest и hashes](../../External/ReconstructionSDKs.md) · [согласованный объём](../tasks/render-output-reconstruction.md) · [решение](../adr/0009-independent-display-transform-and-reconstruction.md)
