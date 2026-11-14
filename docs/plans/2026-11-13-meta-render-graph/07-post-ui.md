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

Пути fixtures как ориентиры (создание и расширение не требуются): `Apps/App.MetaRenderGraph/tests/PostUiGpuTests.cpp`, `SingleGpuSceneTests.cpp`. Existing `OffscreenUITests`, `WorldCanvasTests`, `ReconstructionTests`, `FrameGenerationTests`, `MotionBlurTests`, `DepthOfFieldTests` — patterns. Отчёт `docs/reports/meta-render-graph/07-post-ui.md`, актуализировать `technique-inventory.md`.

## Approach

1. U7.1: подключить DoF, motion blur, reconstruction и output; сохранять render/output resolution, jitter, motion/depth history, reset и HDR/display transform. Текущий объём — Native и FSR; остальные провайдеры отложены.
2. U7.2: offscreen UI/posters/world canvas и экранный Engine.UI. Учесть, что часть UI сейчас записывается после graph Execute в RenderModule. В новом пути эта работа и её ресурсные зависимости должны быть явными пакетами.
3. U7.3: подключить Present с явными границами владения backbuffer. FG и остальные провайдеры отложены по указанию пользователя; существующие hooks сохраняются.
4. U7.4: сверить весь U1 inventory, включая skinning/preparation и optional diagnostic/collision-view пути. Ни одна активная baseline техника не пропущена из-за того, что раньше исполнялась вне GDX12RenderGraph. Фактически отложенные и недоступные варианты отметить в инвентаре.
5. Зафиксировать интерфейс packets/accesses для U8 по коду и обычному запуску. Не переключать production App на эксперимент.

## Smoke-check

Применяется [общий порядок проверки](00-meta-plan.md#verification-contract). Сцена с post/UI отображается в Native и FSR; FG и остальные провайдеры исключены из текущей проверки. Достаточно обычного короткого запуска без явных ошибок; уже полученный результат можно использовать повторно без нового прогона.

## Acceptance and Handoff

Обновить инвентарь post/UI и зафиксировать интерфейс packets/accesses для U8. Кратко записать commit, выбранный режим и итог: работает / не работает / не запускалось, с причиной при наличии. Локальный коммит только своих изменений. Следующее задание: [U8](08-async-single-gpu.md); переходить только в разрешённых пользователем пределах.
