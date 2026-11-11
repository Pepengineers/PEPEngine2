# Runtime validation после rendering audit — 12 ноября 2026

## Результат

На NVIDIA GeForce RTX 4080 SUPER, driver 610.88, свежие Debug GPU-тесты
подтвердили Lighting/DXC, отдельный instancing batcher, AO, MotionBlur и
six-component integration. Полный InstancingTests не завершён: его загрузка Bistro
превысила 300 s. Это функциональные проверки, не сравнительный benchmark.

Сборки и запуски выполнялись последовательно после явной передачи GPU/BUILD
WINDOW чатом воды. Все пять проектов собраны обычным MSBuild VS2026, без
diagnostic object substitution. Исходное состояние — `c649b946` с уже подготовленным
water B1024 diff, впоследствии зафиксированным как `e97fa414` без изменения
содержимого. Существующая пользовательская правка `BistroAdditionsTests.cpp`
входит в сборку полного InstancingTests; она не изменялась и не коммитится здесь.

## Исправление тестового проекта

`Apps/App/SecondGroupTests.vcxproj` наследовал из App копирование общего
`Config/Engine.ini`. При первом запуске startup остановился с
`Unknown CVar: App.Water.HeightScale`: Bistro-specific CVar в этом fixture
не регистрируется. До GPU тест не дошёл.

Добавлен существующий в LightingGpuTests/AOTests target pattern:
`CopyEngineConfiguration` записывает в **runtime directory SecondGroupTests**
минимальный INI с `[ConsoleVariables]` и `Render.NumFrames = 3` при каждой сборке.
Общий `Config/Engine.ini` и пользовательские production settings не менялись.
Пересборка прошла за 2.82 s; отдельно проверены две ожидаемые строки INI.

Первый повтор запускался из bin directory и выявил относительный Fox asset path.
Окончательный запуск выполнен из `I:/PEPEngine2`, где существует
`Assets/Models/KhronosFox/glTF/Fox.gltf`. Ассеты не копировались и не редактировались.

## Выполненные проверки

Время в таблице — elapsed полного тестового процесса с запуском/компиляцией
шейдеров/проверками, не GPU pass time и не frame time.

| Executable / arguments | Результат | Elapsed, s | Проверенный scope |
|---|---|---:|---|
| LightingGpuTests | PASS, exit 0 | 84.06 | Debug layer, unsupported-model guard, DXC defines/errors, zero-draw culling, lighting/IBL/shadows/grid |
| InstancingTests | TIMEOUT, завершён runner | 301.68 | Полный Bistro путь не завершён; итоговый A/B image test не подтверждён |
| InstancingTests --batcher-only | PASS, exit 0 | 1.14 | GPU mixed commands, shuffled IDs, empty/growth, cascade counter offset; debug layer |
| AOTests --gpu-validation | PASS, exit 0 | 17.18 | GPU-based validation, algorithms/placed resources, resize/quality/range/odd grids |
| MotionBlurTests | PASS, exit 0 | 5.74 | Debug layer, identity/sign/shutter, sky, histories, slots; motion blur → XeSS → tone map |
| SecondGroupTests --gpu-validation, repo cwd | Assertions PASS, exit 0; runner **censored** из-за fixture INI | 45.82 | GPU-based validation, skinning/environment/grass/fluid/particles/composition; ошибок debug layer нет |

Lighting успешно инициализирует реальный адаптер и использует DXC/SM6 path.
`CheckShaderDefines` отклоняет объект устройства без поддерживаемого shader model
до компиляции, проверяет восемь defines и ошибочный shader. Это не физический
SM5-only GPU и не аппаратный retry/MGPU тест. Clustered lighting совпадает с
brute force для 0..769 lights на сетках до 1280×720.

SecondGroup прошёл точные cloud Coverage 0/1, enabled/disabled RGB+alpha assertions,
particle refill 1/255/256/257/250000/1000000 и mixed cohorts 257/514/771.
Также прошли water graph SPH/NRF, quality/reset/clear/simulation-only,
DIRECT/async transitions, grass interactions и final composition. Эти проверки
не заменяют отдельное доказательство water sort equivalence/timing из отчёта воды.

AO focused mode явно исключает vendor XeSS. MotionBlurTests отдельно успешно
исполняет XeSS chain. Числа GPU milliseconds, напечатанные fixture, не используются
здесь как доказательство производительности или сравнения изменений.

## Почему SecondGroup runner censored, хотя assertions прошли

Изменился единственный fingerprint path:
`I:/PEPEngine2/Build/bin/x64/Debug/SecondGroupTests/Config/Engine.ini`.
Остальные 114 exe/DLL/shader inputs идентичны до/после. Runner state сохранён как
`censored`, он не переименован в passed. Child exit code — 0, stdout содержит
`Six-component GPU integration passed with no debug-layer errors.`

Это ожидаемый побочный эффект CVar переключений, а не специальная тестовая
проверка Save: `ConsoleVariable::UpdateValue` вызывает `SaveConfigValue` после
startup. TestCPU переключает named SkyBox budgets, затем integration меняет
async, quality, draw/simulation и enable switches. Источники:
`Engine/Engine.Core/Include/Common/ConsoleVariables.h:165`,
`Apps/App/tests/SecondGroupTests.cpp`, `ParticleSimulationControlsChecks.h`
и `GrassInteractionChecks.h`.

До окончательного run INI содержал три строки: section, `Render.NumFrames = 3`,
`Render.SkyBox.PresetIBLBudgets = false`. Последняя строка осталась от TestCPU
предыдущей попытки с неверным cwd. До/после SHA256:

- Before: `f6a05b306a288e394f109de03b7d70d5a588635cc7c8cc5973ff11a8afd904d5`.
- After: `14f0a228975645c4eccb6ee5aa3af7a32efc0c1d850c90a70e0c55425e5ff5b8`.

Добавлены семь финальных значений: `Render.Particles.AsyncCompute=false`,
`Render.Fluid.AsyncCompute=false`, `Render.Fluid.Quality=2`,
`Render.Particles.DrawEnabled=true`, `Render.Particles.SimulationEnabled=true`,
`Render.Particles.Enabled=true`, `Render.Fluid.Enabled=false`.
Before snapshot реконструирован из первых трёх строк after и проверен по
сохранённому before hash. Оба INI сохранены в evidence.

AO также меняет свой runtime INI через fixture CVar operations; его INI был
сохранён отдельно от набора performance fingerprints. Это не меняет общего
project config. Во всей первой серии source hashes совпали; после исправления
отличается только `Apps/App/SecondGroupTests.vcxproj`.

## Воспроизведение и ограничения

Артефакты: `Build/RuntimeBottlenecks/integration/`. Включают `checkout.txt`,
`sources-before.json`, `sources-after.json`, `source-drift.json`, `summary.json`,
все `*-build.json`, `*-run.json`, stdout/stderr, before/after INI и
`InstancingTests-engine.log`. Первичные failed setup runs сохранены отдельно.

Сборка: `MSBuild Apps/App/<Test>.vcxproj /t:Build /p:Configuration=Debug
/p:Platform=x64 /m:4 /nr:false /v:minimal`. Затем запускать соответствующий
`Build/bin/x64/Debug/<Test>/<Test>.exe` из корня репозитория через bounded runner.
Общий config не изменять; SecondGroup fixture INI сбрасывается его build target.

Полный Instancing остановлен штатным timeout: журнал доходит до collision mesh
simplification при холодной загрузке Bistro. Стеков нет; цена конкретной функции
и игровой CPU/GPU bottleneck не установлены. Повтор долгой загрузки не делался.
Физический старый GPU, два адаптера, полный Bistro A/B image validation и
устойчивый frame benchmark остаются непроверенными.

После набора отсутствие собственных test/MSBuild/cl/link processes проверено,
GPU/BUILD WINDOW освобождено. Эта итерация меняет только тестовый MSBuild target
и данный отчёт; коммит выполняет назначенный общий ревьюер.
