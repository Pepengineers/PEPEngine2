# Runtime SR/FG с текущим SDR Reinhard output

Обсуждение от 2026-11-01 через `grill-with-docs`. Пользователь подтвердил реализацию и затем исключил HDR-вывод и ACES, оставив существующий Reinhard → sRGB. Эта редакция учитывает последнее указание и исправляет положение FG относительно тонмаппинга.

Реализация и фактические проверки от 2026-11-02: [отчёт](../reports/2026-11-02-runtime-reconstruction.md). Ниже сохранены согласованный объём и исходный baseline.

## Согласованный результат

- Сохраняется SDR output: существующий Reinhard → sRGB и RGBA8 swapchain. Внутренний scene-linear HDR сохраняется до тонмаппинга.
- SR работает до тонмаппинга. FG получает уже обработанный SDR-цвет после Reinhard/sRGB и выполняется через presentation integration перед Present; depth/motion передаются отдельно. Один общий порядок «SR и FG до тонмаппера» неверен.
- SR backend и FG provider выбираются независимо. Комбинации включаются только после проверки совместимости и доступности; присутствие DLL или header не означает поддержку.
- Backend, качество, FG и принудительный restart меняются без перезапуска приложения. Короткая пауза для ожидания GPU и пересоздания ресурсов допустима.
- При недоступном/неудачно запущенном SR применяется native rendering, при недоступном FG — FG Off. Причина отражается в логах и доступном из кода effective status.
- Defaults: SDR, XeSS Ultra Quality Plus, FG Off. Нельзя скрыто выбирать другой vendor вместо запрошенного.
- Обновляются актуальные публичные стабильные пакеты AMD/NVIDIA/Intel, с согласованными headers/libs/runtime, версиями и notices.

## Реальный исходный объём

До реализации граф исполнял только XeSS; SL был выключен, FG не была реализована. Поэтому работа включает ввод FG AMD/NVIDIA/Intel, а не только restart существующих контекстов. SR scope: native, XeSS, FSR, DLSS и существующий NIS. Начальный FG scope — один промежуточный кадр (2x), где backend/железо поддерживают; multi-frame generation и динамический множитель этим обсуждением не запрошены. [Граф после интеграции](../rendering/current-render-graph.md).

Старые FSR/DLSS/NIS paths нужно привести к текущим HDR/motion/history контрактам до включения. В частности, проверяются HDR flags, NDC velocity scaling, UAV allocation, resetHistory, native command-list state invalidation и обработка SDK errors.

## Изменения, которые должны дойти до всех потребителей

1. Runtime `Default` CVar с валидацией: SR provider/quality, FG provider и явный restart trigger. Отдельные requested/effective значения и причина отказа. Использовать существующий ConsoleModule, без нового UI/консольного фреймворка.
2. Применять изменения на границе кадра до camera/GPU-data jobs. Завершить все обращения GPU к старым ресурсам, отключить старый FG presentation, пересоздать затронутые SDK contexts/swapchain/resources, обновить links/flags и сбросить temporal histories. Сцену и ассеты не перезагружать.
3. Переиспользовать resize synchronization, но не вызывать повторно существующий `ConfigureRenderPipeline`: он добавляет passes в список и хранит связи по индексам. Выделить минимальные операции выбора/relink/reinitialization без универсального render-graph framework.
4. При смене render grid обновить все зависящие targets, camera jitter и history. Resize, minimize и повторное enable должны проходить тем же безопасным lifecycle.
5. Сохранить текущую кривую Reinhard и единственное sRGB encoding. Не обрезать scene-linear HDR до финального output pass.
6. FG: добавить требуемые vendor low-latency integrations (Reflex/XeLL), lifetime color/depth/motion через Present, callbacks/proxies и корректное выключение при resize. SR/FG независимы на уровне настроек, но effective комбинация ограничена реальными SDK/GPU возможностями. Не выгружать весь Streamline runtime под живыми proxies.
7. Синхронно обновить MSBuild dependencies, runtime deployment, notices, documentation и graph description; существующие тестовые ожидания Reinhard сохранить.

## SDK baseline и проверенные целевые релизы

Исходный baseline и цели, проверенные по официальным release pages 2026-11-01. Установленные версии, источники и hashes: [SDK manifest](../../External/ReconstructionSDKs.md).

| Семейство | До обновления | Проверенный стабильный релиз |
| --- | --- | --- |
| NVIDIA Streamline | 2.14.1; DLSS DLL 310.9.1.0 | [Streamline 2.14.16](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16), 2026-10-16; DLSS binary сверить с содержимым пакета |
| AMD FSR | Loader 2.3.0.2740; upscaler 4.1.1.2740 | [FSR SDK 2.4.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v2.4.0), 2026-10-28; Upscaling 4.1.2, ML FG 4.0.2 |
| Intel XeSS | SR DLL 2.0.2.68; FG/XeLL не подключены | [XeSS SDK 3.0.2](https://github.com/intel/xess/releases/tag/v3.0.2), 2026-07-24; номер SDK не равен номеру SR DLL, бинарное соответствие ещё сверить |
| AMD SSSR/DoF/denoiser | Отдельный patched FidelityFX SDK 1.1.4 | Сохранить отдельный воспроизводимый pin; новый FSR package не является drop-in replacement для этих эффектов |

Требования FG: [NVIDIA DLSS-G](https://github.com/NVIDIA-RTX/Streamline/blob/v2.14.16/docs/ProgrammingGuideDLSS_G.md), [Intel XeSS/XeLL](https://github.com/intel/xess/blob/v3.0.2/README.md). Версии компонентов AMD: [FSR SDK readme](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.4.0/readme.md).

## Приёмка

- Сборка App и затронутых fixtures в Release/Debug x64; CPU проверки CVar validation, requested/effective fallback, повторного restart и совместимости.
- GPU debug-layer checks: переходы native ↔ SR, смена качества, явный restart без смены settings, resize/odd sizes/minimize/re-enable; корректные HDR/depth/motion grids и history reset, отсутствие старых links/use-after-free.
- Сохранить Reinhard expectations в PBRMathTests, SSRTests, ReflectionProbeTests, DepthOfFieldTests и MotionBlurTests; HDR/reset/resize проверки AOTests и BistroDofSmoke.
- Проверить, что FG получает SDR display-color после единственного tone mapping/encoding и не применяет Reinhard повторно.
- Каждый FG provider считать проверенным только после запуска на совместимом GPU/driver/OS: enable/disable/restart, presentation/pacing, resize, независимый SR выбор и unsupported fallback. Сборка на одном GPU не подтверждает работоспособность всех трёх FG.

## Границы

HDR-вывод, ACES, новый UI, универсальный render graph, multiframe generation и переключение без паузы не входят в согласованный минимум. Рабочее дерево уже содержит другие изменения; они должны быть сохранены. На этапе обсуждения сборки, SDK downloads и аппаратные FG проверки не выполнялись.
