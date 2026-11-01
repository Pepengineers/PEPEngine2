# Двумерный процедурный облачный фон

Статус: ПОДГОТОВЛЕНО, НЕ ЗАПУСКАТЬ до отдельного указания пользователя.

Общий контракт: ../COMMON.md.

Перенести механизм shader capability из MGPU-Noise (2D simplex/fBM/ridged), не выдавать за volumetric clouds. Настраиваемый компонент/источник, пригодный к явному использованию через sky/material API, но **никакой установки на сцене**. Сохранять один активный sky+IBL contract; не включать второе небо автоматически. Лицензированный simplex/noise source с notices либо собственный алгоритм, проверять actual donor attribution. Реальные implementation choices fBM/ridged при наличии обоих путей. CVars Enabled/Quality/Implementation, scale/octaves/coverage/speed/colors и реальные shader parameters. Lazy resources без потребителей, dt0 при паузе, source revision vs continuous-lighting changes. Проверки seam/poles/finite/Off/reset/params/quality и стоимости на GPU. Объёмный raymarch и объекты в Bistro не входят.
