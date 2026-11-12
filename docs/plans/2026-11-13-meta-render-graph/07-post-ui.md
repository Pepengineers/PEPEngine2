---
title: "U7 Postprocessing UI and single GPU acceptance - Plan"
type: refactor
date: 2026-11-13
---

# U7 Postprocessing UI and single GPU acceptance - Plan

## Contract

Выполнить только U7 из [мета-задания](00-meta-plan.md), R1–R9; KTD1–KTD7. Требуется принятый U6. Это обязательная граница завершения однокарточного DIRECT-пути перед U8.

## Outcome

Экспериментальная сцена имеет весь действующий baseline pipeline, post/UI и корректный Present. Нет скрытых GPU-операций старого графа или прежних workers. CPU jobs записываются параллельно; GPU пока использует DIRECT.

## Files

Adapters `Apps/App.MetaRenderGraph/src/Techniques/Post*`, `UI*`, MetaRenderPipeline и hosting seam. Источники: CommonRenderPasses, `Apps/App/src/Rendering/BistroUIPosters.*`, `Engine/Engine.UI/`, RenderModule OnRender, renderer DepthOfField/MotionBlur/Reconstruction/OutputToScreen/FrameGeneration passes и их SDK integration.

Тесты: `Apps/App.MetaRenderGraph/tests/PostUiGpuTests.cpp`, `SingleGpuSceneTests.cpp`. Existing `OffscreenUITests`, `WorldCanvasTests`, `ReconstructionTests`, `FrameGenerationTests`, `MotionBlurTests`, `DepthOfFieldTests` — patterns. Отчёт `docs/reports/meta-render-graph/07-post-ui.md`, актуализировать `technique-inventory.md`.

## Approach

1. U7.1: подключить DoF, motion blur, reconstruction и output; сохранять render/output resolution, jitter, motion/depth history, reset и HDR/display transform. Сначала доступный native path, затем текущие provider bindings.
2. U7.2: offscreen UI/posters/world canvas и экранный Engine.UI. Учесть, что часть UI сейчас записывается после graph Execute в RenderModule. В новом пути эта работа и её ресурсные зависимости должны быть явными пакетами.
3. U7.3: frame-generation/SDK callbacks и Present. Не перенаправлять SDK-команды на неподдерживаемую очередь; неподконтрольный SDK command section имеет явный boundary и очередь, а не скрытый submit из worker. Проверить текущие правила frame generation, backbuffer и resource tagging.
4. U7.4: сверить весь U1 inventory, включая skinning/preparation и optional diagnostic/collision-view пути. Ни одна активная baseline техника не пропущена из-за того, что раньше исполнялась вне GDX12RenderGraph. Варианты без доступного hardware/SDK перечислить как integration complete/runtime pending отдельно.
5. Провести однокарточное сравнение legacy / new serial recording / new parallel recording. После этого заморозить проверенный интерфейс packets/accesses для U8. Не переключать production App на эксперимент.

## Test Scenarios

- Native rendering и каждый доступный reconstruction provider получают корректные color/depth/motion inputs и reset после camera cut/resize.
- UI виден на экране и в world/poster textures; обновление содержимого не приводит к чтению незавершённого upload.
- Postprocessing on/off, scene pause, resize и повторный запуск не сохраняют неверную history.
- Frame-generation supported/unsupported path явно различается; Present и reuse backbuffer соблюдают контракт текущего SDK.
- Все U4–U6 fixtures и общий scene run проходят после подключения post/UI; reference captures используют одинаковые settings/inputs.
- CPU trace подтверждает несколько независимых recording groups; GPU trace показывает только ожидаемую DIRECT-очередь и отсутствие внешнего legacy/offload исполнения.

## Acceptance and Handoff

В инвентаре у каждой обязательной техники есть packet/access path, fixture и результат проверки. Необъяснённые визуальные/численные расхождения блокируют принятие U7. Недоступный provider не выдаётся за проверенный; обязательный native pipeline должен быть полностью проверен. Отчёт фиксирует CPU recording, submission, GPU time и память без выдуманного порога FPS. Локальный коммит. Только после этой границы переходить к [U8](08-async-single-gpu.md).
