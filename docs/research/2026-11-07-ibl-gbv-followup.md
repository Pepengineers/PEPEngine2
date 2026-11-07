# Follow-up: нерешённый IBL-prefix GBV failure

2026-11-07. **Доказанного renderer fix нет. Общий SecondGroup GPU validation suite нельзя считать стабильно чистым.** Этот follow-up уточняет [исходный отчёт травы](2026-11-07-grass-interaction-results.md). Изменены только test modes и документация; производственный IBL полностью возвращён к `f694338`.

## Сравнение исходных прогонов

`final-features2` прошёл (51.10 с, stderr пустой), независимый `parent-features` завершился exit 1 (52.50 с). Executable SHA-256 совпал: `8a1d2bcef50f6cdeb055666dcb1ea113a90d8d8c6a61c2f29b8723016f40093b`. Начальный INI тоже совпал: `9fae06fb3527b1c2fb8d58d70814a81dd6c4a8b79d3d87f37480c3b36400a2d5`. Различие shaders — только уже задокументированная пустая EOF-строка `GrassInteraction.hlsl`, optimized DXIL до/после одинаков. В независимом прогоне все numerical grass/particle assertions прошли, но InfoQueue содержала incompatible texture barrier layout COMMON для UAV, root parameter 9, DIRECT dispatch 0. [Полный stderr](2026-11-07-ibl-validation/parent-full.stderr.txt) сохранён без исключения сообщений.

Повтор исходного executable `investigate-features1` прошёл (44.69 с). Это подтвердило непостоянство, а не отсутствие дефекта.

## Доказанная локализация

В `SecondGroupTests` добавлен `--environment-gpu-validation`: 128 циклов исходного Off→Atmosphere→Cloud→animated Cloud→другая реализация Cloud→SkyBox. Вся InfoQueue проверяется после каждого шага; mode завершается **до создания grass renderer и particle simulation**. Обычный полный suite также сохраняет все проверки и получает раннюю проверку после environment steps. Message-ID filters и `ClearStoredMessages` не добавлялись.

Режим воспроизвёл ошибку при COMMON→UAV варианте до новой функциональности: `investigate-environment1` — step 108, `investigate-environment-names1` — step 708; окончательный mode без временного instrumentation — **step 528, exit 1, 19.65 с**, [stdout](2026-11-07-ibl-validation/environment-repro.stdout.txt), [stderr](2026-11-07-ibl-validation/environment-repro.stderr.txt).

Root parameter 9 в sky root — UAV3 (`2 CBV + 4 SRV + 4 UAV`), то есть source-cube generation в `GDX12EnvironmentIBLPass::Execute`. Временный bounded ring trace связал реальный pointer при `CreateUnorderedAccessView` mip0, параметры переданного `ResourceBarrier` и сообщение GBV. [Полный trace и сообщения](2026-11-07-ibl-validation/barrier-descriptor-trace.stderr.txt):

```text
CreateUAV mip0 resource=000001BB23A85FD0 generation=149 descriptor=111
Record transition resource=000001BB23A85FD0 generation=149 before=0 after=8 descriptor=111
Environment step 438: Resource 0x000001BB23A85FD0 'IBL source 1 generation 149', UAV descriptor 111, layout COMMON
```

Для этого failure исключён вариант «барьер на A, descriptor на B». Wrapper `GDX12CommandList::ResourceBarrier` не отбрасывает переход 0→8. Trace подтверждает переданные CPU-параметры; native API/GPU command capture не снимался. **Почему записанный transition не отражён в GBV layout, не установлено.** Нельзя называть это доказанным driver/SDK bug или установленной ошибкой приложения.

Исходник `GDX12EnvironmentIBLPass.cpp` имеет один blob `cf9e46e34752222c0979088a00408ad843ff2d31` в `5a5ce57`, `f694338` и итоговом дереве. Это подтверждает неизменность файла, но **baseline executable до grass-задачи не запускался**. Поэтому вывод ограничен формулировкой «до исполнения новой grass/particle функциональности, в неизменённых исходниках IBL»; предсуществование сбоя в старом binary не доказано.

## Проверенные и отклонённые варианты

- Initial UAV state с согласованным legacy CPU tracker: два свежих процесса по 128 циклов прошли (24.66 и 24.06 с), третий упал на step 12 (4.83 с) с той же ошибкой COMMON. [stderr](2026-11-07-ibl-validation/initial-uav-failed.stderr.txt). Это не fix; изменение откатили.
- Новые source-UAV descriptor slots при каждом пересоздании с удержанием прежних descriptors на время нового allocation: failure на step 588 (20.45 с), [stderr](2026-11-07-ibl-validation/descriptor-experiment-failed.stderr.txt). Это тоже не fix; изменение откатили. Все lifetime/reuse механизмы этим опытом не исключены.

Все временные имена ресурсов, ring trace и экспериментальные renderer изменения удалены. SDK, driver, environment variables и validation flags не изменялись.

## Отдельная проверка новой функциональности

`SecondGroupTests --grass-interaction-gpu-validation` запускает существующий numerical GPU тест непосредственно с тестовым depth, **не создавая/исполняя environment pass**. Проверяются depression/recovery, stationary/opposing/dense sources, dt0, offscreen recovery, camera reset, teleport, предел длины стебля с сильным ветром, Firefly response/volume, все три wind modes и точная DIRECT/async parity. После теста проверяется вся накопленная InfoQueue с тем же severity criterion; сообщения не фильтруются и не очищаются.

Три свежих процесса прошли: **24.06, 22.05 и 22.45 с**, exit 0, пустые stderr. [Пример stdout](2026-11-07-ibl-validation/grass-only.stdout.txt). Последующий исходный полный `--gpu-validation` тоже прошёл один раз (44.49 с), [stdout](2026-11-07-ibl-validation/full-followup.stdout.txt). Этот PASS не отменяет ранее воспроизведённый environment failure. Isolated mode подтверждает новую функциональность в пределах своего scope, а не чистоту всей pipeline composition.

Тест записывает фактически загруженный module path. Подтверждён `RuntimeFeatures/D3D12/D3D12SDKLayers.dll`, FileVersion `1.619.6.0.20260914.1`, SHA-256 `7ab48d56d1c18708ebcf16c7c9259b2d783e2b935b85add4a244573b9f03cab4`. Ни обновление SDK наугад, ни отключение resource-state validation не выполнялись.

[Metadata](2026-11-07-ibl-validation/verification.json) сохраняют pass/fail, exit codes, длительности и input/executable/shader hashes всех ключевых прогонов. Исходные побайтовые логи и executable находятся в `Build/GrassInteraction/`. Репозиторные txt-копии нормализованы только по trailing whitespace/переводам строк; все сообщения и их содержимое сохранены.

```powershell
# После Release/x64 сборки SecondGroupTests; runner восстанавливает fixture INI.
& ./Build/GrassInteraction/run.ps1 -Kind Features -Mode '--grass-interaction-gpu-validation' -Name grass-only-check
& ./Build/GrassInteraction/run.ps1 -Kind Features -Mode '--environment-gpu-validation' -Name environment-repro-check
& ./Build/GrassInteraction/run.ps1 -Kind Features -Mode '--gpu-validation' -Name full-check
```

Производственный fix IBL не заявлен. Требуются отдельное исследование механизма на GPU/native API timeline и/или baseline execution; это оставлено за границей данного ограниченного follow-up. Numerical readback и HDR smoke подтверждают поведение травы/роя, а **художественное качество анимации не оценивалось**.
