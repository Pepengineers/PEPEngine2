# Итоговое ревью параллельных оптимизаций PEPEngine2

Дата: 12 ноября 2026. Ветка `codex/bistro-rendering`, общий checkout.
Ревьюер принимает отдельные frozen-итерации и создаёт их новые локальные коммиты;
уже созданные коммиты не переписываются. Push не выполняется.

## Решения

| Итерация | Коммит | Решение и подтверждённая проверка |
|---|---|---|
| Lighting/material/visibility | `2e26dc3a5e6bbc9881e7a3948b6df0166d6bdaaa` | Принята в заявленной области: независимое ревью callers, derivatives, culling/bindings и DXIL; повторены 3 shader-теста / 6 подслучаев, без skips; сохранённая матрица 98/98; diff check passed. |
| Effects shaders | `bfe18dee127a1b3ca2e920aad67f560e80d6e827` | Принята: Spawn reservation/PublishAlive, оба вызывающих пути, UAV barriers/partial groups, IBL basis и sampling/early returns согласованы. Исходная compile matrix: 131/144, все 13 отказов — прежний AO/FXC дефект XeGTAO. Финальные SecondGroup/GBV, AO/GBV и MotionBlur прошли; diff check passed. |
| Quality/Performance search | `9cdd897d` + `fb7297b6` | Принята после устранения P2 позднего pruning. Frozen hashes совпали; fresh CPU SearchTests, FlightChecks, исходный repro и дополнительные двухосевые finalist/fallback scenarios прошли. App.cpp отдельно скомпилирован; GPU search не проверен. |
| Диагностика runtime | `8a638552` | Отчёт принят и закоммичен отдельно. Cold-start smoke: 420 s startup / 440.73 s runner, без result.json; проверены 110 неизменных fingerprints, raw logs/JSON и build statuses. Исправлена неточность числа simplification entries: 31. Причина timeout не доказана, FPS отсутствует. |
| Hybrid water sort, B512/1024/2048 | `e97fa414` | Принята после дополнения harness. Блок 1024; CPU 4/4, shader compilation, штатная Debug-сборка и финальный GBV passed. Проверены точные pairs/offsets/A/B/density после 1+3+3 шагов, tracked/placed и Direct→Compute→Direct. Все 4896 CSV samples совпали с raw JSON; 68 fingerprints каждого запуска сохранились. |
| DXC / SM6-only | `c649b946` | Принята после исправления P2 gate-before-Streamline. Проверены 9 frozen-путей, 4 TU compile logs, настоящий DXC wrapper; повторены 3 shader-теста / 8 подслучаев и CPU startup/target checks. Финальный Lighting GPU smoke подтвердил startup на совместимом адаптере; физический unsupported-adapter retry не проверен. |
| Финальные GPU-регрессии | `0edbae1b` | Приняты пять свежих Debug builds, Lighting, Instancing --batcher-only, AO/GBV, MotionBlur и SecondGroup/GBV. SecondGroup assertions PASS при runner censored только из-за ожидаемой записи fixture INI; 114 прочих inputs и общий конфиг неизменны. Полный Instancing TIMEOUT на холодной загрузке Bistro. |

## Доработки

- Quality: одна ось, Performance, 768×480. Medium всегда 70 FPS, High всегда 65,
  Epic первые два измерения 75, третье и последующие 50. Поиск запрашивает
  Medium дважды, Epic трижды, затем подтверждает Medium; High не измеряется.
  Отсечка в `RequestNeighbor` остаётся в силе после провала `SubmitConfirmation`.
  Исправлено в `fb7297b6`: сохраняются только отсечённые сформированные варианты,
  после failed/invalid confirmation они возвращаются в ограниченный поиск.
  Регрессии обоих профилей покрывают поздний провал, invalid и отказ recovery;
  дополнительные двухосевые проверки сохраняют приоритет finalist и резерв.
- SM6-only: `CreateProxyDevice(primary)` устанавливал `_mainDeviceWasSet`
  раньше новой проверки SM6, а `Reset` не снимал этот флаг. Повторная
  инициализация совместимого primary без Streamline Shutdown могла остаться
  незарегистрированной. Исправлено в `c649b946`; регрессия точного префикса
  Initialize проверяет ветвление на mocks, аппаратно сценарий не воспроизводился.
- SecondGroup: общий конфиг содержал неизвестный fixture CVar
  `App.Water.HeightScale`, поэтому первый запуск завершался до GPU-проверок.
  В `0edbae1b` применён существующий target pattern Lighting/AO: при сборке
  создаётся минимальный INI в runtime-каталоге конкретного теста. Повтор из корня
  репозитория завершился со всеми assertions и без ошибок debug layer.

## Общая проверка и ограничения

- Три исходные итерации проверены независимо отдельными read-only ревьюерами;
  основной ревьюер проверил совокупный diff, контракт `CONTEXT.md`, evidence и
  границы файлов. Изменения компилятора и воды не включаются в оценку старых коммитов.
- Lighting сохраняет material sampling до нового baked-return; удалённые VS
  exports не потребляются PS и не используются stream output. Disabled shadow
  commands отсекаются до instance/material loads. Новых wave assumptions нет.
- Effects сохраняет границы зарезервированных частиц и барьеры; новые spawn lanes
  занимают диапазон после survivors. IBL basis остаётся per-texel; implicit
  derivatives не перенесены за новую divergent-ветку. Новых group/wave assumptions нет.
- Shader compile и уменьшение DXIL не доказывают GPU-ускорение, численную либо
  визуальную эквивалентность. У принятых shader-итераций нет измеренного FPS claim.
  Финальные функциональные GPU-тесты приняты отдельно от performance evidence.
  Полный Bistro A/B image test не завершён из-за startup timeout.
- `Build/RuntimeBottlenecks/build-frozen.json` фиксирует успешную Debug-сборку.
  SHA-256 сверка `frozen-source-manifest.json` подтверждает все затронутые shader
  sources, App.cpp и BenchmarkFlight текущих коммитов. Однако
  `BistroQualitySearch.h` и `BistroQualitySearchTests.cpp` отличаются: эту сборку
  нельзя считать проверкой точной финальной версии `fb7297b6`. Свежие CPU-тесты
  и отдельная компиляция App.cpp проверяют финальные исходники Quality.
- Диагностика после финальных регрессий освободила GPU/build. Неуспешный
  cold-start smoke не доказывает bottleneck конкретной подсистемы: CPU-активность
  при загрузке без stack evidence недостаточна для такой атрибуции.
- Условия приёмки воды: парные GPU-замеры B512/1024/2048 с исходной сетью,
  readback пар/offsets и обеих particle buffers/densities после нескольких шагов,
  partial tiles, повторяющиеся ключи, межблочные взаимодействия, состояния
  ресурсов и честное описание покрытия DIRECT/COMPUTE/PlacedUav/MGPU. Меньшее
  количество dispatch само по себе не является ускорением.
- Эти условия воды выполнены на RTX 4080 SUPER: 3 COMPUTE и 3 DIRECT запуска,
  по 51 парному раунду на набор, включая реальные Hash keys тестовой SPH-решётки.
  На fluid_hash/51975 итоговые медианы исходная→B1024: COMPUTE
  0.231424→0.063008 ms, DIRECT 0.211968→0.057344 ms. Изолированная сортировка
  примерно на 73% короче; весь solver/FPS не измерен. DIRECT fluid_hash имеет
  две точные ничьи медиан B1024/B2048, строгого преимущества там не заявляется.
  Реальные cross-adapter Exact/Latest, overlap очередей, visual A/B и soak остаются
  непроверенными. Подробности и raw samples сохранены в water-sort-gpu отчёте.
- Для финального runtime-набора независимо сверены совпадающие manifest
  исходников (556 путей), шесть успешных build states включая пересборку fixture,
  raw run states/stdout и SHA-256 общего INI. Изменившийся runtime INI SecondGroup
  вызван обычным `SetFromString` → `UpdateValue` → `SaveConfigValue`.
  Его семь добавленных значений и before/after hashes сохранены в отчёте;
  статус runner censored не переименован в passed. Это не performance run.
- Чужие изменения `Apps/App/tests/BistroAdditionsTests.cpp`,
  `External/External.Libraries/External.Libraries.vcxproj` и dirty submodules
  inih/rapidyaml сохраняются. Перед каждым новым коммитом проверяются точные
  frozen-пути и staged/unstaged diff; используется `git commit --only`.

Подробные evidence и локальные Build-артефакты перечислены в отчётах
`2026-11-12-shader-lighting-scalarization.md`,
`2026-11-12-shader-effects-scalarization.md`,
`2026-11-12-quality-search-audit.md`, `2026-11-12-dxc-only.md`,
`2026-11-12-water-sort-gpu.md`, `2026-11-12-runtime-bottlenecks.md` и
`2026-11-12-runtime-validation.md` в этом каталоге.

## Следующая задача по поручению пользователя

После завершения этого ревью отдельный Astra-чат оценивает производительность
текущей принятой версии и устанавливает CPU/GPU bound по измерениям. Холодная
загрузка и стабильный игровой кадр оцениваются раздельно; startup timeout
не считается frame bottleneck. Требуются CPU stacks/scopes, ожидания и GPU
timestamps при фиксированных сцене, камере, конфиге и simulation workload.

По подтверждённым результатам тот чат создаёт отдельные чаты исправлений и
оптимизаций с воспроизведением, baseline, владельцами файлов и проверяемыми
критериями приёмки. GPU/build запускается последовательно. Результаты возвращаются
общему ревьюеру для проверки и локальных коммитов; push не разрешён.
