# Bistro grass/wind async и локальное взаимодействие

2026-11-07. Реализовано в текущем `codex/bistro-rendering` поверх `5a5ce57`, без worktree и push. Окончательный дизайн описан в [обновлённом плане](../plans/2026-11-07-grass-wind-interaction-proposal.md).

## Результат

Все люди публикуют компактные визуальные источники после завершения movement jobs. Поле 64×64 один раз за кадр объединяет текущую занятость и след движения. Трава читает current/previous историю с фиксированным числом выборок; Firefly читает только текущую занятость. Точные капсульные столкновения, Jolt-объекты и меши в интерфейсе отсутствуют. Общая система частиц получает нейтральный заимствованный field/SRV, с opt-in на emitter; только существующий Bistro Firefly включает его.

История поля в мировых координатах выбрана вместо состояния каждого корня: 192 КиБ float4-данных для presence/current trail/previous trail независимо от 60000 травинок и числа views. Она не адвектируется. Давление отделено от направления; восстановление `exp(-3*dt)`. Корень закреплён, вершина опускается и смещается вбок, сумма с ветром ограничена длиной стебля для обоих motion samples. Bounds учитывают новый вынос. Camera history reset сохраняет след и сбрасывает motion vectors; явный grass reset/смена roots/seed/settings сбрасывает поле. Пауза/dt=0 сохраняют состояние; offscreen simulation продолжает восстановление.

`Render.Grass.AsyncCompute=false` по умолчанию. Analytic остаётся режимом Bistro по умолчанию; его velocity теперь подготавливается один раз за кадр в сетке, вместо повторного вычисления синуса при каждом expansion. ProjectedGrid использует прежний решатель. Expansion/draw остаются DIRECT, с одной переиспользуемой arena. Граф запускает траву перед водой; environment/culling/обычный depth проходят до первого ожидания. [Combined DOT](2026-11-07-grass-interaction/combined-async.dot) фиксирует `Bistro grass simulation → join before Bistro depth grass`, тогда как вода ожидается только перед `Fluid surface`.

Межкадровая безопасность обеспечена существующим графом: COMMON handoff, queue fork/join и join перед frame retirement. Следующий COMPUTE fork ждёт DIRECT prefix, уже упорядоченный после предыдущего retirement. CPU upload/constants принадлежат frame slot, который приложение ждёт перед повторным использованием. Частных fences в реализации нет.

Основные файлы: `BistroCharacters.h`, `Components/BistroInteractionSource.h`, `BistroGrassGeometry.*`, `GDX12GrassRenderer.*`, `GrassInteraction.hlsl`, `GrassExpand.hlsl`, `WindField.hlsl`, `BistroRenderPipeline.cpp`; нейтральная сторона — `GDX12Particles.h`, `GDX12ParticleSimulation.cpp`, `ParticleSimulation.hlsl`, `InteractionField.hlsli`.

## Проверки

Release/x64, MSBuild 18 Professional, сборки последовательно `/m:1`:

```powershell
$msbuild = 'C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe'
foreach ($project in 'App','SecondGroupTests','BistroRenderGraphTests','RenderPipelineTests','BistroAdditionsTests') {
  & $msbuild "Apps/App/$project.vcxproj" /m:1 /p:Configuration=Release /p:Platform=x64 /v:minimal
  if ($LASTEXITCODE) { throw "Build failed: $project" }
}
```

Все пять проектов собраны. Финальные проверки использовали копии runtime в `Build/GrassInteraction/Runtime*`, уникальные executable names и сохранённый начальный INI. У engine-only fixtures из INI удалены `App.*`, которых эти executables не регистрируют. INI во время работы меняется штатным CVar persistence; неизменяемый initial-INI, executable и все затронутые shaders проверялись по SHA-256 до/после runner. [Verification metadata](2026-11-07-grass-interaction/verification.json) сохраняет hashes, exit codes и длительности.

| Проверка | Результат |
| --- | --- |
| `SecondGroupTests --gpu-validation` | PASS: новые численные проверки плюс существующие skin/wind/grass/water/particles tests; GPU-based validation без D3D12 errors |
| `BistroRenderGraphTests --grass-gpu-validation` | PASS: DIRECT/async toggles, Analytic→ProjectedGrid, queued 3 frame slots, совместные water/particles/AO, grass/wind off, seed reset, pause, odd resize |
| `RenderPipelineTests` | PASS: primitives/batches, pipeline composition, AO rebinding, rebuild и grass lifetime |
| `BistroAdditionsTests --routes-only` | PASS: полный плотный ряд, позиции snapshot после movement, отсутствие fox sources, одинаковые траектории на 1/4 worker threads, pause/time scale существующих маршрутов |
| `BistroAdditionsTests --arch-smoke` | PASS: настоящий App update, арка/трава/рой, confinement, finite HDR и debug layer |
| `BistroRenderGraphTests --grass-timing` | PASS: фиксированный A/B, 480 raw samples, без debug layer |

[Численный readback](2026-11-07-grass-interaction/numerical.csv) читает настоящие expanded vertices и GPU particle pool. Проверены Wind Off/Analytic/ProjectedGrid на DIRECT и COMPUTE, с побитным сравнением всех current/previous вершин и 64 байт particle state. Для контрольного стебля высотой 40 см два встречных источника дают 10.9315 см, dt=0 сохраняет это значение, через 2 с восстановления — 39.9279 см. Root XYZ фиксирован. Есть проверки отменяющихся направлений, 32 перекрывающихся источников, ограничения скорости и длины формы при ветре 500 см/с, телепорта без ложного отрезка, восстановления за кадром и camera reset без стирания следа. Первый подъём Firefly — 52.9965 см/с; после ухода людей presence сразу нулевой, скорость возвращается к обычной через drag. Положение остаётся в исходном swarm volume.

После GPU/performance прогонов удалена одна пустая строка EOF в `GrassInteraction.hlsl`; optimized DXIL до/после побитно одинаков (SHA-256 `a30021ddcf4bfaa8a670ed6e90a01e519797325bba632dc3bab28e7a6fd4f38f`). Метаданные сохраняют исходный tested-source hash.

Логи и launcher: `Build/GrassInteraction/`; успешные состояния `final-features2.json`, `final-bistro.json`, `final-cpu.json`, `final-routes2.json`, `final-arch.json`, `final-timing.json`. Сборки: `build-final.log`, `build-shape.log`, `build-routes.log`. Sandbox MSVC дал C1902/PDB service failure; host builds прошли. Сохранились vendor LNK4099 и LNK4020 о недоступных/повреждённых debug symbols; исполняемые проверки прошли, качество PDB не подтверждено. В процессе исправлены runtime shader reserved-name error и неполная аллокация при переключении analytic/projected одинакового разрешения; итоговый прогон включает это переключение. Routes-only CPU branch перенесён до не относящегося к нему старого assertion о default видимости reflection spheres.

## Стоимость и A/B

RTX 3060 Laptop 6 GiB, NVIDIA 617.42. Один процесс, native rendering, настроенное окно 1280×800, камера `GrassPosition+(450,200,-450)` смотрит на `GrassPosition+(0,70,0)`, seed травы 7919, один patch 60000 blades, 10000 particles суммарно, 32 детерминированных источника, dt=1/60, 3 frame slots. Физика и реальная NPC mesh animation не продвигаются этим GPU fixture. Для timing вода выключена, particles/AO DIRECT, меняется только `Render.Grass.AsyncCompute` и отдельно алгоритм ветра. В combined validation вода и прочие async-техники включены.

Для каждого алгоритма порядок D→A→A→D, 16 warmup + 60 measured frames на серию. Перед каждым следующим measured frame выполняется явный CPU drain через `WaitForUploads`; CPU submit измерен до drain. Это **GPU CPU-drain latency**, а не pipelined FPS. [Raw CSV](2026-11-07-grass-interaction/timing.csv), [summary с min/max/IQR](2026-11-07-grass-interaction/timing-summary.json).

| Ветер | Серия | Queue | GPU median, мс (IQR) | COMPUTE sum median, мс | CPU submit median, мс |
| --- | --- | --- | --- | --- | --- |
| Analytic | 0 | DIRECT | 15.4322 (15.3462–15.4906) | — | 5.6409 |
| Analytic | 1 | COMPUTE | 16.3277 (16.2629–16.3970) | 0.0399 | 4.8613 |
| Analytic | 2 | COMPUTE | 16.3605 (16.3102–16.4810) | 0.0410 | 4.9405 |
| Analytic | 3 | DIRECT | 15.5300 (15.4458–15.5732) | — | 4.5650 |
| ProjectedGrid | 0 | DIRECT | 15.5781 (15.5280–15.6332) | — | 4.8511 |
| ProjectedGrid | 1 | COMPUTE | 16.4557 (16.3561–16.5591) | 0.1697 | 5.1047 |
| ProjectedGrid | 2 | COMPUTE | 16.4306 (16.3679–16.5273) | 0.1703 | 4.9261 |
| ProjectedGrid | 3 | DIRECT | 15.6273 (15.5678–15.6710) | — | 4.6596 |

Среднее двух series medians: Analytic DIRECT 15.4811 → async 16.3441 мс (+5.57%), ProjectedGrid 15.6027 → 16.4432 мс (+5.39%). **Ускорение не получено; default-off сохранён.** CPU submit имеет заметный дрейф между сериями; надёжный CPU выигрыш не установлен. Наличие overlap и малое время COMPUTE не компенсировали полную стоимость queue handoff/submissions в этом сценарии.

64×64 gather стоит O(4096*A+B+P) по основным потребителям. Для A=32 это 131072 проверок текущего присутствия и столько же swept контактов в подготовке. У каждого expansion — 4 current + 4 previous чтения history; у реагирующей частицы — 4 чтения presence один раз перед substeps, без source loop. COMPUTE marker 0.040 мс для Analytic включает gather/history, два analytic wind dispatch и подготовку roots; 0.170 мс для ProjectedGrid также включает существующий pressure solver. Это измерение полного preparation pass, **не отдельный замер gather**. Предыдущая версия без взаимодействия не замерялась: стоимость всей новой функциональности до/после этим A/B не определяется.

## Пределы

Это локальное 2D-поле у одной арки с height gate, не точное столкновение и не система всех игровых зон. Gather линейно растёт с числом ближайших людей; экстремальная толпа требует отдельного замера перед tiles/spatial indexing. Дополнительные patches вне поля не получают воздействия людей. Wind Analytic теперь дискретизируется сеткой; субъективная калибровка радиуса/высоты/силы в движении может потребовать настройки.

Проверки на другом GPU, долгий soak, pipelined FPS benchmark, полный performance matrix всех async-комбинаций и pixel oracle с движущейся толпой не выполнялись. Queued validation подтверждает корректность нескольких кадров в полёте, а не их throughput. Художественное качество подтверждено numerical readback и arch HDR smoke; отдельного просмотра видео движения не было. MGPU reference не изменён.
