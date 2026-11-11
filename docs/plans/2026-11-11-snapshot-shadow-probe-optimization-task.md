# Задание: дешёвая подготовка snapshot, теней и reflection probes

## Полномочия и результат

Пользователь поручил реализацию отдельному чату Sol medium, а исходному чату — независимую проверку и итерации до приемлемого результата. Реализовать изменения, выполнить проверки и локально закоммитить только свои файлы; push не требуется. Работать в текущем checkout `D:\Project\PEPEngine2`, сохранив чужие изменения (на старте изменён `Apps/App/tests/BistroAdditionsTests.cpp`). Никаких одновременных benchmarks/builds с проверяющим: до финальной передачи ресурс измерений принадлежит исполнителю. После финальной передачи не запускать новые измерения до следующего сообщения.

Результат — корректные инкрементальные данные рендера и существенно более дешёвая CPU-подготовка static/dynamic shadow/probe state, подтверждённые тестами и PIX. Это не задача переписывания всего render graph, physics backend или GPU water solver.

## Проверенные исходные данные

- HEAD перед заданием `a24ce7f1`; перепроверить actual HEAD/worktree перед изменениями.
- Отчёт: `docs/reports/2026-11-11-cpu-gpu-bound-analysis.md`.
- PIX baseline `Build/BoundAnalysis/bistro-timing.wpix`: PID 12960, main TID 31336, рабочий диапазон 1000–10000 ms. Snapshot 10,137 ms; Scene preparation 16,069 ms (включает snapshot), CPU light grid 1,506 ms, shadow visibility wait 0,455 ms. CPU frame cadence 65,684 ms; GPU marked span 43,289 ms. Не складывать вложенные интервалы.
- Ordinary baseline `Build/BoundAnalysis/{high2,low1,high3}.json`: примерно 59–60 ms/frame; physics debt растёт. Стационарная камера, seed 12345, 120 warmup + 600 samples, полная сцена. Режим Quality route отличается от этого workload.
- Замороженный executable `Build/bin/x64/Debug/App/App.BoundAnalysis.exe`, SHA256 `eb5de81696b4c7a4d5aceb68507ed160bfdfdba86b41e2009a404aff5df6385c`. Сохрани baseline executable/PDB/config/нужные DLL до rebuild; не перезаписывай evidence.
- Overlay `Build/BoundAnalysis/high.ini` задаёт 1280×800. В baseline PIX есть XeSS reconstruction: не называть профиль native и не менять reconstruction/FG для выгодного сравнения.

## Уже существующие механизмы — расширять, а не дублировать

1. `Apps/App.Base/src/RenderModule.cpp`, `CollectReflectionScene`: полный обход StaticMeshRenderComponent/submeshes с сравнением кешей matrix/material/instance, затем primitive reductions и копирование static casters в общий список. `StaticMeshRenderComponent` НЕ означает неподвижность. Snapshot также влияет на солнечные каскады и отражения.
2. `Apps/App.Base/Include/App.Base/Systems/ShadowVisibilitySystem.h`: каждый кадр разделяет общий caster list на static/dynamic и сравнивает всю статику; per-light static visibility уже кэшируется. Единственное static изменение сейчас сбрасывает расчёт для всех lights.
3. `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/GDX12LocalLights.h`: BuildLocalShadowVisibility умеет дополнить cached static signatures динамическими casters.
4. `.../RenderPasses/GDX12LocalShadowPass.h`: static depth cache, dirty faces, dynamic command subset уже существуют. Измеренные 13 ms GPU Local shadows нельзя объявить расходами исключительно CPU visibility.
5. `Engine/Engine.RendererDX12/src/GDX12ReflectionProbePass.cpp`: static baked + dynamic и frozen modes уже существуют. Инвалидация зависит от static/environment revisions и capture settings. Сохранить семантику режимов: baked static receivers сейчас имеют static-only shadows; не вводить новые визуальные компромиссы.

## Обязательная работа

1. Проследить все writers/readers состояния, от которого зависит snapshot: ECS lifetime/generation, world lifecycle, transforms, geometry/material revisions, visibility, caster flags, static/dynamic classification, skeletal/procedural geometry, probes, lights и environment. Найти существующие dirty/revision/event механизмы и использовать их. Сначала записать короткий перечень источников инвалидации.
2. Убрать повторную дорогую обработку неизменившейся статики из steady state: persistent membership/data и точечное обновление изменившихся записей. Обеспечить удаления/повторное использование entity ID и корректное уменьшение bounds. Прямые изменения через поддерживаемые API/публичные поля, которые существующие тесты считают допустимыми, должны продолжить работать: нельзя молча требовать новый setter и оставить старых callers.
3. Передавать/обслуживать static и dynamic shadow data так, чтобы наличие cache не сопровождалось полным пересозданием/сравнением static list каждый кадр. Измерить необходимость и стоимость более локальной invalidation lights/probes; реализовать её, если глобальная invalidation остаётся существенным источником повторной работы. Корректность раньше локальности.
4. Reflection revisions и выбор updates должны отражать реальные изменения геометрии, материалов, света и environment; изменения не должны теряться при cached frame slots, world reset или paused scene. Не менять частоту updates и качество ради ускорения.
5. Измерить получившийся профиль. Если один этап только перенёс полный scan/hash в другой этап, продолжить до устранения причины или предъявить измеренное обоснование остаточной стоимости.

## Проверки и критерии приёмки

- Сохранить scene counts, визуальное качество, физику 60 Hz/CCD/solver, shadow resolutions, probe modes/update intervals и native/MGPU semantics. Не удалять объекты, не ставить сцену на паузу в ordinary замере, не скрывать отставание физики сбросом accumulator.
- Regression coverage: unchanged repeat; changed transform/material/mesh/visibility; add/remove/ID reuse; static↔dynamic; light movement/direction/range; dynamic caster enters/exits; world reset; bounds shrink; probe invalidation on relevant changes. Сверять incremental result с полным reference scan, включая unchanged frames и смену frame slots. Добавить малое число содержательных тестов в подходящие существующие suites.
- Использовать текущие `RenderPipelineTests`, `ObjectDataJobsTests`, `LightingGpuTests` и подходящие reflection/Bistro reset проверки; выбирать конкретные switches по исходникам, не выдумывать команды. Debug build и релевантные CPU/GPU checks обязательны. Сохранять логи/exit codes.
- PIX до/после на одинаковом workload; ordinary A/B/A или B/A/B с неизменными hashes/config/seed/camera/counts. Прогоны строго последовательные. GPU temperature/clocks/thermal flags, если доступны; physics requested/ticks/debt рядом с FPS. Добавить низкозатратные CPU markers только если существующих недостаточно для атрибуции.
- Рабочая цель snapshot: mean <=2 ms при baseline около10 ms на этом железе; это целевой бюджет, не право урезать работу или критерий на синтетическом входе. Если цель не достигнута, передать остаточные timings и конкретную причину; проверяющий решит следующую итерацию. Также ожидать заметного снижения Scene preparation вне разброса повторов и отсутствие регрессии whole-frame p95/debt, обусловленной изменением.
- Структурная приёмка: неизменная статика не вызывает повторный полный submesh/material/matrix scan/hash, либо оставшийся проход доказан дешёвым и необходимым для совместимости. Измерить counts реально посещённых/обновлённых записей; не оставлять дорогой telemetry в production по умолчанию.
- Отдельно доказать, что shadow/probe filling не стал следующим значительным CPU hotspot после snapshot. Не обещать отсутствие всех bottlenecks приложения: GPU тени/вода и physics вне этой задачи остаются отдельными направлениями.

## PIX и запуск: известные ловушки

`peengine-perf` skill и существующий runner предпочтительны. PowerShell аргументы `'-Quality.Overlay=...'`, `'-Loading.BenchmarkOutput=...'` и остальные с точками обязательно заключать в кавычки. Самостоятельный Start-Process из sandbox ненадёжен для последующего attach/остановки: использовать живой runner, проверять actual PID. Не запускать вторую копию до подтверждённого завершения первой вне sandbox при необходимости.

PIX capture tools доступны. Старый pix analysis bridge отключён, но штатный `C:\Program Files\Microsoft PIX\2610.28\PixTimingCapturesMcp.exe` читает timing captures. Если динамические query tools не появятся в чате, локальный клиент `Build/BoundAnalysis/query_pix.py` и `pix-schemas.json` показывают рабочий протокол; клиенту потребовалась разрешённая elevated execution. Capture path в скрипте/аргументе явно менять на нужный. Capture callbacks/time ranges читать по фактической схеме: timing MCP использует ms, другие PIX tools могут ns. Ответы по eventId дополнительно фильтровать по времени; проверять truncation и lost ETW events. Второй старый `bistro-launched-timing.wpix` загрязнён параллельным процессом, не использовать.

## Передача на независимую проверку

Вернуть: commit hashes; список изменённых контрактов/callers; отчёт с baseline/candidate mean/p95 и разбросом, physics debt и counts; точные команды build/tests; пути raw benchmark/PIX capture; оставшиеся риски и hotspots. Production-патч должен быть минимальным и без неоправданной инфраструктуры. После проверки исходный чат может прислать конкретные замечания; выполнять доработки тем же способом. Не считать эту передачу окончательной пользовательской приёмкой.
