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

## Вторая группа: сохранена в ветках, чаты остановлены

Это непроверенные реализации, не часть принятой первой группы. Все шесть авторов подтвердили остановку. Работа, Git commits и LFS assets перенесены в тот же верхнеуровневый Git; отдельных проектов для них больше нет.

| Техника | Ветка в основном Git | HEAD |
|---|---|---|
| [SKY](second/SKY.md) | `codex/feature-sky` | `5a95aa4` |
| [GRASS_WIND](second/GRASS_WIND.md) | `codex/feature-grass-wind` | `c146dcd` |
| [PARTICLES](second/PARTICLES.md) | `codex/feature-particles` | `47f5f23` |
| [FLUID](second/FLUID.md) | `codex/feature-fluid` | `38294c1` |
| [SKINNING](second/SKINNING.md) | `codex/feature-skinning` | `fba7b3d` |
| [CLOUDS](second/CLOUDS.md) | `codex/feature-clouds` | `74666b6` |

Глобально у компонентов только Enabled/Quality; все остальные параметры принадлежат экземплярам компонентов. Следующая стадия — отдельная центральная проверка этих веток и их согласованная интеграция. Не запускать автоматически старые чаты с удалёнными путями.

Историю видно обычными командами из корня:

```powershell
git log --oneline --graph --all
git show codex/feature-sky --stat
git branch --list 'codex/feature-*'
```

Прежняя отменённая реализация остаётся в Git-истории; её код не восстановлен. Старые временные snapshots не являются рабочими проектами.
