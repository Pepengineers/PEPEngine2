# SkyAtmosphere и SkyBox

Статус: ПОДГОТОВЛЕНО, НЕ ЗАПУСКАТЬ до отдельного указания пользователя.

Общий контракт: ../COMMON.md.

Отдельные компоненты SkyAtmosphere и SkyBox; ровно один активный источник видимого неба и global IBL. Текущее HDRI оформить через SkyBox на уже существующей сущности, не создавать entities. Atmosphere отключается при выбранном HDRI; явный Source Off/SkyBox/SkyAtmosphere, отсутствие выбранного компонента диагностируется без скрытого смешивания. Первичный ориентир Hillaire 2020 production-ready atmosphere LUTs, лицензированный официальный source; донор MGPU-Atmosphere. Нужны transmittance/multiple scattering/sky-view/aerial perspective, HDR/IBL/sun единицы и согласованные history/lighting revisions. CVars Enabled/Source/Quality, scattering/extinction/height/distance/anisotropy/sun/IBL и реальные LUT budgets; не мутировать author fields. Проверки neutral Off/HDRI, LUT и Beer-Lambert oracle, camera heights/zenith/наружу атмосферы, sun/history/IBL, качество/timings. Не добавлять объекты.
