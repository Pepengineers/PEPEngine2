---
title: "U1 Experimental application - Plan"
type: feat
date: 2026-11-13
---

# U1 Experimental application - Plan

## Contract

Выполнить только U1 из [мета-задания](00-meta-plan.md). Требования R1–R3; решения KTD1. Предшествующих этапов нет. Прочитать актуальный AGENTS.md и статус checkout; не трогать незавершённые изменения particle-offload чата.

## Outcome

Отдельный проект App.MetaRenderGraph собирается и запускается с собственными output/config/log paths. Текущий App остаётся самостоятельным эталоном. Есть карта подключения техник и проверенный путь к следующему этапу, без реализации нового графа в U1.

## Files

Создать `Apps/App.MetaRenderGraph/App.MetaRenderGraph.vcxproj`, `src/App.cpp`, `Configs/` и необходимые project props. Изменить `PEPEngine2.sln`, при необходимости `MSBuild/Props/` по существующей системе модулей. При необходимости для smoke можно использовать `Apps/App.MetaRenderGraph/tests/ApplicationSmokeTests.cpp`; отдельный test target не обязателен. Отчёты: `docs/reports/meta-render-graph/01-project.md`, `technique-inventory.md`.

Ориентиры: `Apps/App.Benchmark/App.Benchmark.vcxproj`, `GlobalSettings.props`, `Apps/App/App.vcxproj`, `Apps/App/WaterOffloadBistroTests.vcxproj`, `Apps/App.Base/src/RenderModule.cpp`, `Apps/App.Base/Include/App.Base/Rendering/IRenderPipeline.h`.

## Approach

1. Создать дополнительный executable с отдельным entry point, GUID и путями артефактов; включить Debug/Release x64. Переиспользовать Engine/App.Base и минимально необходимую часть App sources, не копировать весь Bistro и не линковать второй App.cpp. Изолировать startup config, включая adapters и отключение offload в эксперименте.
2. На этом этапе допустим явно обозначенный reference-only запуск существующего pipeline для baseline. Это не считается новым executor и не остаётся единственным рабочим режимом после U2. Если тяжёлый Bistro недоступен, обеспечить маленькую procedural scene тем же host; не записывать её как проверку всего Bistro.
3. Проследить RenderModule OnRender/RebuildRenderGraph, frame-slot wait, PrepareFrame, Present, resize и SDK callbacks. Зафиксировать минимальный opt-in seam, через который U2 подключит новое исполнение, не меняя default App. Не начинать general renderer rewrite.
4. Составить инвентарь техник/вариантов: owning files, inputs/outputs, hidden submits/CPU waits, shared mutable state, существующие fixtures, этап U4/U5/U6/U7/U8/U10/U11. Включить grass/interaction, probes, clouds, water partitions, particles, UI, reconstruction и frame-generation hooks. Каждая актуальная техника получает владельца этапа.
5. Записать выбранную конфигурацию запуска и известные ограничения. Ничего не утверждать о производительности нового исполнителя, которого ещё нет.

## Smoke-check

Применяется [общий порядок проверки](00-meta-plan.md#verification-contract). Приложение открывает окно и показывает результат reference pipeline. Достаточно обычного короткого запуска без явных ошибок; уже полученный результат можно использовать повторно без нового прогона.

## Acceptance and Handoff

Зафиксировать пути проекта, инвентарь техник и seam для U2. Кратко записать commit, выбранный режим и итог: работает / не работает / не запускалось, с причиной при наличии. Локальный коммит только своих изменений. Следующее задание: [U2](02-direct-plan.md); переходить только в разрешённых пользователем пределах.
