# Двумерный процедурный облачный фон

Статус: запуск поручен центральному проверяющему ПОСЛЕ завершения проверки и исправлений первой группы. Сейчас не запускать.

Общий контракт: ../COMMON.md.

Перенести механизм shader capability из MGPU-Noise (2D simplex/fBM/ridged), не выдавать за volumetric clouds. Настраиваемый компонент/источник, пригодный к явному использованию через sky/material API, но **никакой установки на сцене**. Сохранять один активный sky+IBL contract; не включать второе небо автоматически. Лицензированный simplex/noise source с notices либо собственный алгоритм, проверять actual donor attribution. Реальные implementation choices fBM/ridged при наличии обоих путей. Глобальные CVar только Enabled и Quality. Implementation, scale/octaves/coverage/speed/colors и остальные shader parameters — поля cloud render component. Lazy resources без потребителей, dt0 при паузе, source revision vs continuous-lighting changes. Проверки seam/poles/finite/Off/reset/params/quality и стоимости на GPU. Объёмный raymarch и объекты в Bistro не входят.

Авторы этой задачи сейчас добавляют код без сборок/тестов/benchmarks и отдельной ревизии. Приведённые проверки — критерии последующей централизованной проверки. Соблюдать code style и ponytail при написании кода.
