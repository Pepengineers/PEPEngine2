# AMD FidelityFX Depth of Field

Статус: запуск отдельного чата Sol / high разрешён.

Сначала прочитай ../COMMON.md (относительный путь из first: `../COMMON.md`).

Реализуй готовый AMD FidelityFX DoF 1.1 из SDK v1.1.4 pin c6efa6bf7f2027b3ec94f28578bb5965eabb9e55, DX12 backend, без Cauldron. Источники: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK и https://gpuopen.com/manuals/fidelityfx_sdk/techniques/depth-of-field/ . Старый MGPU-DOF (depth bands/PatchMatch/clipped HDR) не выбран итоговой реализацией.

Нужен правильный физически согласованный CoC/thin-lens mapping: единицы сцены cm, sensor/focal length/FOV/focus/aperture, RH camera и reverse-Z. Обрабатывай foreground/background и HDR highlights, не превращай DoF в gaussian blur всей картинки. Минимально исправь HDR composition/final tone mapping и SDK resource-state restoration, если это необходимо. Положение относительно XeSS выбери по actual image/cost comparison: противоречивые общие рекомендации не заменяют проверку согласованных color/depth grids. Не добавляй произвольный resampling без проверки силуэта и CoC.

CVar: Render.DOF.Enabled, Quality Off/Low/Mid/High, focus distance (с единицами), aperture/F-number, CoC limit, реальный sample/ring budget и фактические доступные SDK параметры. No fake Implementation selector. Runtime focus/aperture/quality/enable обновляются из кода/INI; безопасный context recreation только когда реально требуется.

Проверки: in-focus точность/identity где допустимо алгоритмом, near/far blur, CoC sign и edge leakage, HDR bokeh, Off exact copy, разные FOV/разрешения, quality/override, frame slots/resize/re-enable, XeSS native/output grids и бутылки Bistro без новых объектов. Измерь same-workload качество/стоимость; документируй limitation single-layer depth для стекла.
