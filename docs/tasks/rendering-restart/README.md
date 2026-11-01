# Перезапуск реализации техник

Запрос 2026-11-01: отменить старую реализацию; одна техника — один отдельный чат **gpt-6.1-sol / high**. Авторы добавляют код с code style + ponytail без самостоятельной верификации; отдельный проверяющий проверяет первую группу и чинит сам. Постэффекты управляются runtime CVar + INI; у второй группы глобально только Enabled/Quality, остальные параметры внутри рендер-компонентов. Общий контракт: [COMMON.md](COMMON.md).

Первая группа, запуск сейчас: [AO: SSAO/HBAO/XeGTAO/CACAO](first/AO.md), [SSSR](first/SSR.md), [cubemap reflections](first/REFLECTION.md), [DoF](first/DOF.md), [Motion Blur](first/MOTION_BLUR.md).

Вторая группа, **запущена после проверки первой, Sol/high**: [Sky](second/SKY.md), [Grass/Wind](second/GRASS_WIND.md), [GPU particles](second/PARTICLES.md), [Fluid/Water](second/FLUID.md), [Skinning](second/SKINNING.md), [2D clouds](second/CLOUDS.md).

Первая группа работает в отдельных local clones под ignored `Build/Features`, а не одновременно в одном checkout. Изменения не объединяются автоматически с основной веткой. По завершении каждой задачи её чат отдаёт код, commits и непроверенные ограничения для последующего объединения.

Прежние исходники отменены отдельным обратным коммитом без переписывания чужой истории; полная копия затронутых файлов и исходных patches перед откатом находится в ignored `Build/TechniqueRestart/rollback-snapshot`. Исследовательские решения перенесены в новые задания. Посторонние правки загрузчика/Jobs/CVar/предупреждений сохраняются.

Откат: commit `409356f`; Release App после отката собран успешно. Проверка чужого исходного WIP побайтно прошла.

Центральная интеграция первой группы завершена в отдельной REVIEW-копии на `codex/review-render-techniques`: [отчёт](../../rendering-review.md). Добавлен Depth Prepass; `Render.AO.ExecutionPoint=0/1` выбирает ранний AO с normals в препассе или поздний AO с depth-only препассом. Основные тени идут после препасса. Корневой checkout не объединён с REVIEW. Запуск второй группы от проверенной базы разрешён и отражается в RUNS.json.

Вторая группа фактически запущена в шести отдельных local clones от `353a585`; все шесть чатов подтвердили active/inProgress через wait_threads. IDs, paths и branch names — в RUNS.json и отчёте REVIEW. Авторы добавляют только код; глобально Enabled/Quality, остальные поля на экземпляре компонента.
