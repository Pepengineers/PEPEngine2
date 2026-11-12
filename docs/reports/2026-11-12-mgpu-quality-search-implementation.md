# MGPU Quality / Performance: реализация и проверки

Дата начала: 12 ноября 2026. Основание: утверждённый `docs/plans/2026-11-12-mgpu-quality-search-design.md`, ADR-0013 и разрешение реализации в текущем чате.

## Исходное состояние

- Ветка `codex/bistro-rendering`, исходный HEAD `7acaf950`. Более новые изменения относительно плана `8deb1496` сохранены.
- Посторонние изменения: `Apps/App/tests/BistroAdditionsTests.cpp`, `External/External.Libraries/External.Libraries.vcxproj`, рабочие деревья `inih` и `rapidyaml`, `tools/tests/__pycache__/`. В коммиты задачи не включаются.
- Целевая пара: RTX 5050 primary / Arc B570 secondary на MisSpec. Прежние измерения из отчёта 2026-11-11 относятся к другому сценарию и не доказывают пользу нового поиска.

## Выполнение

| Этап | Состояние |
|---|---|
| Модель кандидата, приёмка измерений, профиль, вода/трава | Код и CPU-тесты готовы; проверка интеграции продолжается |
| Солнечные и индивидуальные локальные тени | Код интегрирован; direct mesh raster прошёл two-GPU/GBV; завершается cleanup |
| Частицы secondary / отображение primary | CPU и two-GPU/GBV fixture прошли; полная сцена и FPS ещё не проверены |
| Независимые оси отражений, расписание, near/far и перенос граней | Код интегрирован; 1–4 probes/masks/schedules проходят two-GPU debug; GBV indirect ограничен внешним воспроизведённым отказом |
| Совместный поиск, replay и последовательные A/B на MisSpec | Совместный поиск и профиль реализованы, CPU проходят; аппаратный поиск/replay и A/B ещё не выполнены |

## Проверки и ограничения

- Прочитаны план, ADR, CONTEXT и отчёт MisSpec; проверены ветка, index и рабочее дерево.
- Прежний host Codex недоступен. Пользователь запустил подготовленный скрипт OpenSSH; вход по отдельному ключу проверен, отпечаток сервера совпал с присланным пользователем. Пароль и ключи находятся вне репозитория.
- MisSpec: NVIDIA RTX 5050, драйвер `32.0.16.2114`; Intel Arc B570, `32.0.101.8864`. Также присутствуют iGPU и виртуальные адаптеры; они не входят в целевую пару. Удалённый исходный HEAD `d04ea7d0`; непроиндексированный `Apps/App/App.vcxproj.user` сохраняется.
- До новых runtime-правок прошли Release-сборки `Engine.RendererDX12` и `App.Base` (VS 2026, v145). Логи: `Build/mgpu-quality-renderer-baseline.log`, `Build/mgpu-quality-base-baseline.log`.
- CPU `BistroQualitySearchTests`: MSVC 14.52, C++17, `/W4 /WX`, успешно. Покрыты идентичность пары, 81 сочетание маршрутов/частоты воды и травы, fixed-target freshness, fallback, потерянные входы, отсутствие обязательных измерений и рост долга. Это синтетическая проверка логики, не аппаратный FPS.
- CPU `ReflectionScheduleTests`: успешно. Проверены 1/4/6/24-кадровые циклы, ближайшая точка box, R/1,2R, reset, обе near/far-политики, маски граней, эквивалентность расписаний одной пробы и отсутствие голодания дальних.
- Обновлённый `GDX12ReflectionProbePass.cpp` отдельно скомпилирован. Полная сборка интеграции, GPU/GBV, визуальная приёмка и измерения ещё предстоят.
- Single-GPU/MGPU выигрыш не установлен. Числовые проверки возраста не заменяют визуальную приёмку.

## Продолжение после остановки сессии

- Текущий HEAD перед продолжением `674bca2e`; новые perf-коммиты сохранены. 34 чужих staged-файла SPH-отчёта не включаются в коммиты MGPU.
- Повторный SSH к MidSpec успешен; удалённая исходная ветка остаётся на `d04ea7d0`, пользовательский `App.vcxproj.user` сохранён.
- Release `MGPUShadowRoutingTests` и `MGPUShadowSelectionTests` собраны. Routing `--cpu` и Selection прошли. DepthTransfer `--cpu` сначала отклонил проверку partial publication; после диагностической пересборки прошёл, причина ещё исследуется. GPU-тест не запускался.
- CPU regression совместного поиска сначала воспроизвёл отсутствие независимых Sun/local/particle placement, затем прошёл после перехода к измеряемым соседним сочетаниям. Модель и интеграция этого поиска входят в следующий этап.
- Source compile восьми App/water/grass translation units прошёл; имеется прежнее предупреждение C4244 в BistroBounds.h.
- CPU ReflectionScheduleTests повторно прошёл: добавлено разделение полного возраста грани и дополнительного запаздывания относительно неизменяемого номинального расписания.
- Реальные two-GPU/GBV, визуальная приёмка, A/B и replay ещё не выполнены. Числа FPS и выигрыш не заявляются.

## Аппаратная интеграция после восстановления SSH

- Этап 1 зафиксирован локальным коммитом `485daaa3`. Последующие этапы пока не завершены: остаются ошибки аппаратной корректности; их нельзя считать принятыми по одной сборке.
- На MisSpec создан отдельный checkout `C:/Users/MidSpec/Documents/PEPEngine2-MGPU` на основе `674bca2e`, с общими Assets/External и зависимостями. Исходный checkout и пользовательский `.vcxproj.user` сохранены. Передаваемые исходники перечислены с SHA256 в `Build/mgpu-quality/source-manifest.json`; ключи SSH вне репозитория.
- Полная локальная Release-сборка App прошла. На MisSpec прошли Release-сборки ShadowDepthTransfer, ShadowRouting, Particles, ReflectionProbe и BistroStartup fixtures. Имеются предупреждения отсутствующих PDB внешних библиотек; они не считаются результатом GPU-проверки.
- CPU: Search `/W4 /WX`, ReflectionSchedule `/W4 /WX`, ParticleTime, ShadowRouting/Selection и ShadowDepthTransfer прошли. Добавлены regression на выбор стабильного primary при нестабильном secondary и на сохранение номинальных deadlines probes при задержке нескольких циклов.
- ShadowDepthTransfer на RTX 5050 / Arc B570 с GBV прошёл: typed packing, cross-adapter copy, восстановление SV_Depth. ShadowRouting не прошёл: у point-light 8 на втором кадре все шесть secondary slices остаются 1 вместо 0,25 ещё до переноса. Readback подтвердил корректные indirect arguments, instances, transforms, materials, vertices и CPU frustum; исследуется исполнение draw.
- Particle fixture на целевой паре с GBV прошёл после исправления writable boundary и публикации последнего simulated state перед сменой владельца. Проверены Exact/Latest, cadence, время, input history, reset и возврат primary. Это не замер ускорения.
- ReflectionProbe primary `--debug` на RTX 4080 SUPER прошёл прежний набор radiance/cache/baked/dynamic/depth/filter/parallax/overlap/resize/queued-slots. Целевой `--mgpu` на Arc не прошёл: DRED показывает device removal/page fault на ExecuteIndirect первого вторичного захвата; проверка останавливается с ошибкой, fallback не засчитывается. Дамп и DRED-лог находятся в `Build/mgpu-quality` изолированного checkout.
- В CPU-тесте invalid partial shadow layout выявлен воспроизводимый дефект оптимизации MSVC 14.52: aggregate reset после копирования структуры игнорируется при `/O2`, но работает при `/Od`; scalar width invalidation устраняет ложное падение fixture. Минимальный reproducer и asm находятся в `Build/mgpu-quality/ShadowRegionResetMiscompile*`. Production layout для обхода не менялся.
- Профиль сохраняет schema/scenario/world seed/probe seed и пару LUID; replay отклоняет несовместимый сценарий/пару. Полная проверка apply/cancel/reset/replay на целевой сцене ещё предстоит.
- Обычных Release A/B, полного Quality/Performance поиска, визуальной приёмки движущейся сцены и аппаратного replay победителя пока нет. Single-GPU/MGPU выигрыш не установлен. Отладочные GBV/DRED прогоны не используются как показатели FPS.

### Уточнение по indirect draw

- После прямого mesh draw ShadowRouting прошёл весь two-GPU/GBV fixture. Mesh raster уже имел CPU loop и готовые draw arguments; качество и выбранная GPU сохранены.
- Reflection --mgpu-debug прошёл 1–4 probes, оба режима, все расписания и маски. Исправлена отсутствующая descriptor heap при primary-фильтрации полностью импортированной пробы.
- Минимальный native D3D12 reproducer подтвердил отказ constant+draw ExecuteIndirect только с GBV на Arc, независимо от Upload/Default и root size 1/20; RTX 5050 проходит. Матрица и исходники: [validation-limit.md](2026-11-12-mgpu-quality-search/validation-limit.md). Это внешний блокер полной GBV-проверки отражений, а не доказанный production-дефект драйвера.
- BistroStartupTests --cvars-only на MisSpec прошёл: registry, полный round-trip/rollback профиля и benchmark flight.

### Локальная фиксация этапов

- Этап 2: bcbecf0e. Cleanup прямого mesh draw пересобран на MisSpec; ShadowRouting --gpu-validation повторно прошёл. Полная удалённая Release-сборка App прошла.
- Этап 3: CPU ParticleTime и целевой MGPUParticleTests --gpu-validation прошли; checkpoint, полный учёт времени, смена владельца и displayed motion history включены в код. Проверка полной сцены остаётся общей частью этапа 5.
- Reflection near/far GPU fixture: оба размещения, R/1,2R и возврат из незавершённого дальнего цикла прошли с debug layer.
