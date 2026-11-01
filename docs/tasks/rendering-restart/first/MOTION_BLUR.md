# Motion blur по Fast and Stable Feature-Aware

Статус: запуск отдельного чата Sol / high разрешён.

Сначала прочитай ../COMMON.md (относительный путь из first: `../COMMON.md`).

Реализуй Guertin/McGuire/Nowrouzezahrai, Fast and Stable Feature-Aware Motion Blur, HPG 2014, финальная статья и Appendix A. Найди первичный paper/author implementation и сохрани требуемую атрибуцию. Донор MGPU-MBlur полезен только как отправная структура velocity→TileMax→NeighborMax→gather: его расчёты, bounds и dispatch не считать корректными.

Используй существующий motion buffer App: документируй и проверь convention prevNDC-currentNDC, масштаб в pixels, shutter fraction и centered exposure. Движение камеры и объектов, depth-aware foreground/background contributions, знак скорости, различающиеся направления, background/sky rotation должны быть корректны. Не заменяй feature-aware gather простым directional blur. Матрицы/jitter/output grids согласуй с actual XeSS pipeline. При cut/resize/re-enable не показывай старое размытие. Положение до/после XeSS обоснуй actual image и GPU cost.

CVar: Render.MotionBlur.Enabled, Quality Off/Low/Mid/High, shutter angle/fraction с единицами, max radius в pixels, samples и другие реальные параметры техники. Выбор одного алгоритма не требует fake Implementation selector. Presets/overrides работают из кода и INI, без скрытого перезаписывания.

Проверки: stationary exact identity/Off, смена знака симметричной экспозиции, foreground occlusion/feature spreading, разнонаправленное движение, sky rotation vs translation, камера/существующие flying props, odd sizes и TileMax bounds, frame slots, quality/shutter/radius overrides, cut/resize/re-enable, GPU states, native/XeSS image comparison и timings. Не добавляй scene entities; isolated GPU fixtures разрешены.
