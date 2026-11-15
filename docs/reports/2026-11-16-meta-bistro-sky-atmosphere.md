# Bistro: SkyAtmosphere в ordinary Meta

Реализация на `codex/bistro-rendering`, исходный HEAD `e313d2f9`.
Используются существующие SkyAtmosphere solver/shaders, SceneEnvironment и Sun
из Bistro. Default обычного App не изменён.

## Параметры и солнце

`Meta.Sky.Source`: `2` SkyAtmosphere (startup), `1` исходный SkyBox, `0` Off.
Существующие `Render.SkyAtmosphere.Enabled`, `.Quality` и настройки
SkyAtmosphereComponent остаются действующими. Никакой второй directional light
не создаётся. Направление к солнцу равно `rotation.Forward()` исходного Sun;
direct/shadows используют противоположное направление распространения света.
Изменение положения light не влияет на бесконечно удалённое солнце; влияет поворот.
Illuminance, angular radius, medium и luminance scale берутся из компонента,
сохраняются единицы cm/km и lux.
После пользовательской оценки диск увеличен в 2,5 раза: начальный angular radius
0.0116875 rad (~1,34° видимого диаметра). Solar illuminance не менялась.

По дополнительному запросу пользователя включён дневной цикл:
`Meta.Sky.SunCycle.Enabled=true`, `Meta.Sky.SunCycle.PeriodSeconds=120`.
Pitch исходного Sun проходит 0 → 90 → 180 градусов (рассвет → зенит → закат),
затем возвращается к рассвету. Ночного участка нет. Yaw определяет плоскость
дуги. При отключении цикла можно вручную поворачивать тот же light.
Фаза продвигается временем кадра независимо от paused physics.

Mid/quality 2: transmittance 256×64 / 64 samples; MS 32×32 / 32 directions ×
20 samples; SkyView 256×128 / 32 samples; aerial 32³ / 16 samples; IBL cube
256 / 64 filter samples. IBL capture position фиксирована компонентом,
а aerial следует актуальной камере, jittered projection и SR render grid.

## Зависимости и ресурсы

- Transmittance: состав/высоты среды, радиусы, разрешение и samples интегрирования.
- MS: transmittance, ground albedo и собственные budgets/resolution.
- Source cube/irradiance/prefilter: среда/MS, направление/illuminance солнца,
  фиксированная capture position, параметры SkyView и IBL.
- Камерный SkyView: положение камеры и состояние атмосферы; поворот камеры не
  меняет всенаправленную LUT. После fixed capture восстанавливается камерная LUT.
- Aerial radiance/transmittance: положение камеры, jittered ViewProjection,
  атмосфера, дистанция, grid и budgets.

Ключи сравнивают только входы соответствующих стадий. Ресурсы persistent;
изменение размера пересоздаёт только затронутые LUT. Замену ресурсов/descriptors
предваряет Meta Executor Drain через retirement callback. Shared App сохраняет
свой прежний fallback Flush. Native recording читает подготовленные данные;
barriers и зависимости остаются у графа. Все потребители объявляют LUT/IBL reads;
Off/SkyBox используют нейтральные atmosphere bindings.

Движение камеры не перефильтровывает IBL. При движении солнца fixed capture/IBL
обновляются, поскольку освещение действительно меняется. Плавное направление
обновляется через `UpdateSunDirection` без configuration revision: это сохраняет
temporal history и SSSR context. Смена source/quality/settings и скачок закат →
рассвет обновляют ревизию/reset. Пересборка камерных LUT не сбрасывает историю.

## Потребители и композиция

Deferred использует background SkyView/solar disk, атмосферную transmittance для
direct light и общий IBL. SSSR fallback использует тот же prefiltered environment;
отсутствие probes не отключает атмосферу. `_opaqueOnly` сохранён.
В mesh transparency отдельно включена atmosphere capability при выключенных probes:
SkyCB и transmittance/aerial LUT привязаны к фактическому shader variant.

Deferred/SSR остаются без aerial до WBOIT resolve. Этот resolve применяет aerial
к opaque HDR ровно один раз по текущей глубине после воды. Стекло получает свою
aerial в transparent shader. Текущая вода сохранена как opaque velocity-colored
particles (`9da1611c`), её color/depth проходят через тот же opaque aerial resolve;
неактивный fluid optics shader не требует новых atmosphere bindings.
Temporal fix `83755c8c` сохранён. Порядок weighted и SortedAlpha/additive частиц
соответствует App; дополнительный particle fog не вводился. Экранный UI идёт
после post/reconstruction и не получает туман. Неактивный Near resolve повторно
opaque aerial не применяет.

## Проверка

Финальная Release-сборка успешна (известные linker warnings LNK4099 об отсутствующем
Jolt.pdb). Обычный запуск без diagnostic arguments открыт; текущий журнал не
содержит ERROR/FATAL. Source=2, quality=2, daylight cycle=1, period=120 записаны
в журнале. Engine.log и операции сохранены в
`Build/Verification/meta-bistro-sky-atmosphere/`.
Первый осмотр атмосферы: вид над крышами Bistro, атмосферное небо и освещённые
фасады; XeSS, render 1477×777 → output 1920×1008 (окно развёрнуто пользователем).
До добавления цикла стартовые операции содержали atmosphere kernels 0/1/2/4, irradiance/prefilter mips,
BRDF, восстановление камерного SkyView и aerial kernel 3; в первом reused slot
остался только aerial kernel 3, без IBL filters. Это проверка деклараций операций,
не профилирование GPU. После включения цикла reused slot закономерно содержит
capture/filter/SkyView/aerial, но не transmittance/MS: меняется солнце, а не среда.
Управление окном остановлено физической Escape пользователя; дальнейшие действия
камерой не выполнялись. Пользователь подтвердил работу финального дневного цикла
и изменения солнца/теней; затем попросил увеличить слишком маленький диск.

Source review выявил и исправил постоянный temporal reset при анимации солнца.
Финальные correctness/standards/testing/maintainability/performance source-проверки
не оставили подтверждённых замечаний. `git diff --check` успешен. Новые тесты, матрицы
режимов, профилирование и длительный прогон полного цикла не выполнялись по
контракту задачи. Runtime-переключения source/quality и ручной поворот Sun
проверены только по коду. Нагрузка/FPS остаются на пользовательскую оценку.

`Meta.Clouds.Enabled=false`, probes, DoF, FG, world-space canvas и физическая
симуляция не включались. Следующий этап автоматически не запускался.
