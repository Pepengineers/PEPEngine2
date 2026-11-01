# Ambient occlusion: SSAO, HBAO, XeGTAO, CACAO

Статус: запуск отдельного чата Sol / high разрешён.

Сначала прочитай ../COMMON.md (относительный путь из first: `../COMMON.md`).

Реализуй четыре настоящих алгоритма с runtime выбором: SSAO, HBAO, Intel XeGTAO и **AMD FidelityFX CACAO**. CACAO добавлен пользователем последним сообщением, это обязательная четвёртая реализация, а не альтернативное название SSAO. Предпочти официальный AMD SDK и официальный XeGTAO, соблюдай лицензии. Старый донор содержит SSAO/HBAO/XeGTAO, но ошибки и громоздкую адаптацию предыдущего чата не переносить.

Первичные источники: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK , https://gpuopen.com/fidelityfx-cacao/ , https://github.com/GameTechDev/XeGTAO . Предыдущий выбор AMD SDK v1.1.4 pin c6efa6bf7f2027b3ec94f28578bb5965eabb9e55 относится к согласованным SSSR/DoF; проверь CACAO в этой версии и используй совместимую воспроизводимую зависимость. Без Cauldron framework.

Донор: MGPU-SSAO/SharedXeGTAO.cpp, Common/SharedHBAO.cpp, Common/SharedSSAO.cpp. Текущий App уже имеет depth/world normals и deferred PBR. AO применяется только к соответствующему непрямому освещению; не затемняй direct light/emissive и baked color повторно. Согласуй normal spaces и сантиметры/reverse-Z. Сохрани neutral AO=1 для Off и неподдерживаемых transparent paths.

Обязательные CVars: Render.AO.Enabled, Quality, Implementation (0 SSAO/1 HBAO/2 XeGTAO/3 CACAO), radius/intensity и фактически поддерживаемые настройки sampling/denoise/bias/fade/sharpness и алгоритм-специфические параметры. Не своди разные алгоритмы к одному fake sample loop. Качество независимо от implementation; overrides сохраняются. У CACAO адаптируй реальные native settings и depth/normal preparation, не только UI selector.

Проверки: настоящая плоскость и depth step, все четыре реализации, corner/edge behavior с обоснованной допустимой погрешностью, odd dimensions, range/finite output, denoise, качественные пресеты, переключения из C++/INI, Off, frame slots, GPU resource states, PBR energy invariants. Сравни изображение и GPU cost. Не добавляй AO-only scene entities.
