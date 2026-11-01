# SkyAtmosphere и SkyBox

Статус: запуск поручен центральному проверяющему ПОСЛЕ завершения проверки и исправлений первой группы. Сейчас не запускать.

Общий контракт: ../COMMON.md.

Отдельные компоненты SkyAtmosphere и SkyBox; ровно один активный источник видимого неба и global IBL. Текущее HDRI оформить через SkyBox на уже существующей сущности, не создавать entities. Atmosphere отключается при выбранном HDRI; явный Source Off/SkyBox/SkyAtmosphere, отсутствие выбранного компонента диагностируется без скрытого смешивания. Первичный ориентир Hillaire 2020 production-ready atmosphere LUTs, лицензированный официальный source; донор MGPU-Atmosphere. Нужны transmittance/multiple scattering/sky-view/aerial perspective, HDR/IBL/sun единицы и согласованные history/lighting revisions. Глобально только Enabled и Quality для соответствующей техники. Выбор активного источника неба — scene/component state, не новый глобальный Source CVar. Scattering/extinction/height/distance/anisotropy/sun/IBL, LUT budgets и остальные параметры — поля компонентов; не мутировать author fields глобальными настройками. Проверки neutral Off/HDRI, LUT и Beer-Lambert oracle, camera heights/zenith/наружу атмосферы, sun/history/IBL, качество/timings. Не добавлять объекты.

Авторы этой задачи сейчас добавляют код без сборок/тестов/benchmarks и отдельной ревизии. Приведённые проверки — критерии последующей централизованной проверки. Соблюдать code style и ponytail при написании кода.
