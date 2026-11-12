---
title: "U6 Local water particles and transparency - Plan"
type: refactor
date: 2026-11-13
---

# U6 Local water particles and transparency - Plan

## Contract

Выполнить только U6 из [мета-задания](00-meta-plan.md), R2–R3, R5–R9; KTD2–KTD5. Требуется принятый U5. В эксперименте вода и все частицы считаются на основной DIRECT. Существующие production offload-workers не запускать.

## Outcome

Текущие вода, частицы и слои прозрачности работают через новый план на одной карте. Есть отдельные simulation/render packets и CPU/GPU evidence, пригодные для async/MGPU этапов без изменения физической модели.

## Files

Adapters `Apps/App.MetaRenderGraph/src/Techniques/Water*`, `Particles*`, `Transparency*` по выбранной структуре. Источники: `Apps/App/src/Rendering/Water/GDX12FluidSolver.h`, `GDX12Fluid.cpp`, `GDX12FluidPass.*`, `BistroWaterRendering.*`; `Engine/Engine.RendererDX12/src/GDX12ParticleSimulation.cpp`, `GDX12ParticlesPass.cpp`, `GDX12ParticleResources.h`, соответствующие HLSL; Bistro LinkInputs/BuildGraph.

Тесты: `Apps/App.MetaRenderGraph/tests/WaterGpuTests.cpp`, `ParticlesGpuTests.cpp`, `TransparencyGpuTests.cpp`. Existing `FluidSortGpuTests`, `SecondGroupTests`, `WaterOffloadBistroTests` — исходные fixtures для адаптации. Отчёт `docs/reports/meta-render-graph/06-water-particles.md`.

## Approach

1. U6.1: подключить existing water solver без изменения численных kernels, затем surface/filter/resolve. Учесть, что вода заменяет depth/motion входы последующих техник и делит прозрачность на части перед/за поверхностью.
2. U6.2: отдельно подключить particle simulation pools, spawn/death/bursts, depth collision и interaction field. Сбор inputs и reset — до recording; dispatch/initialization — явные packets. Сохранить current seed/paused/time-scale behavior.
3. U6.3: подключить culling/render data, общую SortedAlpha сортировку, WBOIT/additive, particle motion и композицию с водой/сценой. Поддержать shared outputs нескольких производителей, не выполнять Clear накопителей в каждой подгруппе.
4. Для каждого внутреннего участка зафиксировать запись команд без скрытого submission и shared mutable tracker. Если existing pass совмещает preparation и execution, извлечь необходимую запись с сохранением legacy callers, без переноса App-owned воды в generic Engine ради reuse.
5. Подготовить явное разбиение эмиттеров по группам данных/inputs для U8, но не создавать COMPUTE/COPY queues и не исполнять реальный MGPU в этом этапе.

## Test Scenarios

- Одинаковые seed/inputs/dt/реальные шаги дают тот же water/particle state в serial и parallel recording; water solver mathematics сохраняется.
- Birth/death, пустой emitter, burst, reset во время pause, moving source и time scale сохраняют семантику.
- Particle DepthCollision, box/volume collision и interaction field читают правильные локальные входы.
- Частицы разных групп с перемешанными глубинами правильно сортируются вместе; общий WBOIT resolve и clear выполняются в правильных границах.
- Вода корректно перекрывает/преломляет прозрачность, передаёт depth/motion в downstream; motion соответствует предыдущему показанному кадру.
- Resize/reset/recreate и отключение техник не оставляют старые indirect counts, history либо ссылки на устаревшие resources.

## Acceptance and Handoff

Численные/readback и визуальные проверки именно нового executor, debug layer без ошибок на fixtures. В отчёте — stage boundaries, нужные render payload поля и inputs для последующих переносов, без преждевременного MGPU runtime. Локальный коммит. Следующее задание: [U7](07-post-ui.md).
