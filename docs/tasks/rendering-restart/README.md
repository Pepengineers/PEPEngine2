# Графические техники: один проект и обычные Git-коммиты

Рабочая копия: `D:/Project/PEPEngine2`, текущая ветка `codex/bistro-rendering`. Полные вложенные копии проекта отменены по указанию пользователя. Новые clones/worktrees не создавать без отдельного запроса. Общий контракт: [COMMON.md](COMMON.md); точные HEAD/статусы: [RUNS.json](RUNS.json).

Проверенная первая группа объединена в текущую ветку коммитом `a7f5781` с полной историей feature-коммитов и исправлениями `353a585`. Агентские warning/MSBuild правки — `ffb529e`, исследование аллокатора — `b021641`. Обычная рабочая копия должна оставаться чистой после законченной работы.

| Техника в текущей ветке | Интеграционные коммиты |
|---|---|
| [AO: SSAO/HBAO/XeGTAO/CACAO](first/AO.md) | `d12ca7a`, `711fe4e`, `353a585` |
| [FidelityFX SSSR + denoiser](first/SSR.md) | `329f537`, `353a585` |
| [Reflection probes](first/REFLECTION.md) | `6ea913b`, `271e44f`, `353a585` |
| [FidelityFX DoF](first/DOF.md) | `beb75f4`, `42ddc6d`, `353a585` |
| [Motion Blur HPG2014](first/MOTION_BLUR.md) | `d639b4d`, `353a585` |
| Depth Prepass / ранний или поздний AO | `353a585` |

`Render.AO.ExecutionPoint=0`: prepass depth+normals → AO → тени. `=1` (default): depth-only prepass с alpha-test → тени → G-buffer → AO перед lighting. Во втором режиме normals и прочие G-buffer данные в препассе не считаются. [Отчёт проверки](../../rendering-review.md); сохранённые logs/images — `Build/RenderingReview`.

## Вторая группа: объединена в текущую ветку

SKY, GRASS_WIND, PARTICLES, FLUID, SKINNING и CLOUDS объединены отдельными merge-коммитами с сохранением исходной истории. Все шесть компонентов прошли активные CPU/GPU integration checks и GPU-based validation. [Коммиты, исправления, команды и границы проверки](../../rendering/second-group-integration.md).

Глобально у компонентов только Enabled/Quality; остальные параметры принадлежат экземплярам. Новые компоненты не добавлены в Bistro. Полная визуальная/физическая приёмка второй группы и длительные stress tests ещё не выполнены.

Объединённые `codex/feature-*` и `codex/review-render-techniques` удалены после проверки достижимости их HEAD из текущей ветки. Их исходные имена/коммиты сохранены в RUNS.json; все коммиты доступны через обычный `git log --graph` и `git show <commit>`. Исходные авторские ветки первой группы закрыты отдельным ancestry merge: их изменения раньше перенесены и исправлены центральной интеграцией, повторно старый код не применяется.

Новые clones/worktrees и автоматический перезапуск старых чатов не допускаются без прямого запроса. Вся дальнейшая работа выполняется последовательно в корневом проекте и оформляется коммитами.
