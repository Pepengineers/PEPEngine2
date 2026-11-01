# Перезапуск реализации техник

Запрос 2026-11-01: отменить старую реализацию; одна техника — один отдельный чат **gpt-6.1-sol / high**. Писать код, внимательно проверить и упростить по code style + ponytail. Все параметры через runtime CVar + INI. Общий контракт: [COMMON.md](COMMON.md).

Первая группа, запуск сейчас: [AO: SSAO/HBAO/XeGTAO/CACAO](first/AO.md), [SSSR](first/SSR.md), [cubemap reflections](first/REFLECTION.md), [DoF](first/DOF.md), [Motion Blur](first/MOTION_BLUR.md).

Вторая группа, **не запущена**: [Sky](second/SKY.md), [Grass/Wind](second/GRASS_WIND.md), [GPU particles](second/PARTICLES.md), [Fluid/Water](second/FLUID.md), [Skinning](second/SKINNING.md), [2D clouds](second/CLOUDS.md).

Первая группа работает в отдельных local clones под ignored `Build/Features`, а не одновременно в одном checkout. Изменения не объединяются автоматически с основной веткой. По завершении каждой задачи её чат отдаёт проверенный код, commits и ограничения для последующего объединения.

Прежние исходники отменены отдельным обратным коммитом без переписывания чужой истории; полная копия затронутых файлов и исходных patches перед откатом находится в ignored `Build/TechniqueRestart/rollback-snapshot`. Исследовательские решения перенесены в новые задания. Посторонние правки загрузчика/Jobs/CVar/предупреждений сохраняются.

Откат: commit `409356f`; Release App после отката собран успешно. Проверка чужого исходного WIP побайтно прошла.
