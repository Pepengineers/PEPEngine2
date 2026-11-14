---
title: "U10 Water through the MGPU executor - Plan"
type: refactor
date: 2026-11-13
---

# U10 Water through the MGPU executor - Plan

## Contract

Выполнить только U10 из [мета-задания](00-meta-plan.md), R3, R7–R12, R14; KTD9–KTD11. Требуется принятый U9 и local water U6. Сохранить текущую модель воды; проект DX12 не является обязательной физической моделью.

## Outcome

Вода в экспериментальном приложении использует secondary COMPUTE, отдельный COPY export, основной COPY import и primary surface/render через единый новый executor. Основная сцена и симуляция продвигаются независимо. Production App и его текущий water offload сохраняются.

## Files

Изменить experimental Water adapters и Graph hooks U9. При необходимости отделить reusable record/pack/display части из `Apps/App/src/Rendering/Water/GDX12WaterOffload.*`, `GDX12FluidSolver.h`, `GDX12FluidPass.*`, `Apps/App/Shaders/WaterTransfer.hlsl`, сохранив legacy callers.

Пути fixtures как ориентиры (создание и расширение не требуются): `Apps/App.MetaRenderGraph/tests/WaterOffloadGpuTests.cpp`, расширение `SingleGpuSceneTests.cpp` либо отдельный `WaterOffloadSceneTests.cpp`. Ориентиры: существующие `Apps/App/tests/WaterOffloadGpuTests.cpp`, `WaterOffloadBistroTests.cpp`, `docs/reports/2026-11-12-water-only-mgpu.md`. Отчёт `docs/reports/meta-render-graph/10-mgpu-water.md`.

## Approach

1. Перенести solver state на выбранную secondary; settings/transform/dt/реальные step IDs фиксируются в inputs и publication metadata. Не менять численные kernels и параметры ради переноса.
2. Solver и Pack записываются в COMPUTE packets, Export — COPY packet. Использовать packed slot, который не перезаписывается до чтения export queue; следующий solver может работать с другим безопасным выходом. Не оставить прежний CPUWaitForFenceValue после каждого batch как обязательный цикл нового executor.
3. Передавать только необходимые render данные; нынешний water payload — позиции float3, но сверить фактического потребителя. Density/keys/velocities/scratch остаются у secondary, кроме диагностического readback для тестов.
4. Import идёт на основной COPY, обновление display history — допустимым DIRECT/COMPUTE packet основной. Surface/resolve используют выбранный готовый display. Initial seed доступен до первой публикации; повторный показ обновляет history по показанным кадрам.
5. В experimental path не запускать GDX12WaterOffload::Start/Run рядом с новым producer loop. Обычный запуск должен использовать новый executor.

## Smoke-check

Применяется [общий порядок проверки](00-meta-plan.md#verification-contract). Вода рассчитывается на второй карте и отображается на основной. Достаточно обычного короткого запуска без явных ошибок; уже полученный результат можно использовать повторно без нового прогона.

## Acceptance and Handoff

Указать путь water packets через новый executor. Кратко записать commit, выбранный режим и итог: работает / не работает / не запускалось, с причиной при наличии. Локальный коммит только своих изменений. Следующее задание: [U11](11-mgpu-particles.md); переходить только в разрешённых пользователем пределах.
