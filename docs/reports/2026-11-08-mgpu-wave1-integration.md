# MGPU: интеграция первой волны

2026-11-08. Worktree: `D:\Project\PEPEngine2\.tmp\mgpu-feature-2026-11-08\integrator`, branch `codex/mgpu-integrator`.

## Принятые пакеты

- P0: `79c6dc3fde0688715c3669375b09b71c7bd85b16`, [общий контракт и baseline](2026-11-08-mgpu-p0-contract.md).
- P1: source `31a1cf29710994bc6d02b8506931f1a8ee55cf9d`, cherry-pick `a237b2bff6349cee40e2f53530f181c2eaaefd3c`, без конфликтов. До добавления этого отчёта деревья source commit и integrator HEAD совпали полностью. [Отчёт P1](2026-11-08-mgpu-p1-queues.md).
- P2a и P3a: commits ещё не получены; общая интеграция первой волны не завершена.

## Статическая проверка P1

`git diff HEAD^ HEAD --check` прошёл. Прочитаны изменения device/queue/list API, реализации identity/Signal/Wait/import/reset/retirement и fixture project. Сигнатуры P0 сохранены; прежние Execute и WaitForQueue callers сохранены. Shared fence export остаётся явным opt-in. Legacy raw-fence rendezvous не включён; новые graph/runtime/technique paths не подключены. Блокеров статической интеграции не обнаружено.

Ведущий принял проверки исходного P1 worktree: queue debug/GBV на NVIDIA RTX 3060 Laptop (`LUID 0:62942`) и AMD Radeon Graphics (`LUID 0:57700`), graph regression debug/GBV; совпадение всех девяти build fingerprints подтверждено ведущим. Это переданная проверка P1, не новый запуск в integrator. Здесь после cherry-pick **не запускались builds, tests, generators или captures**: lease принадлежит P2a. Повторная проверка объединённых commits требует отдельного grant.

## Конфигурация и следующая регистрация

Реальные MGPU techniques пока не wired; новых technique CVars и no-op switches нет. Existing async CVars `Render.Fluid.AsyncCompute`, `Render.Grass.AsyncCompute`, `Render.Particles.AsyncCompute`, `Render.AO.AsyncCompute` сохраняют `false`. При дальнейшем подключении каждая новая MGPU technique switch обязана иметь `false` default; correctness fixtures включают её явно и восстанавливают default при teardown. Автоматической смены качества/частоты нет.

После получения P3a commit проверить фактические пути и зарегистрировать ожидаемые файлы:

- `Engine.RendererDX12.vcxproj`: `ClInclude Include="Include\Engine.RendererDX12\GDX12SnapshotProtocol.h"` и `ClCompile Include="src\GDX12SnapshotProtocol.cpp"`.
- `Engine.RendererDX12.vcxproj.filters`: тот же header в `Header Files`, cpp в `Source Files`.
- Проверить standalone `Apps/App/MGPUSnapshotProtocolTests.vcxproj`: до общей регистрации он компилирует protocol cpp напрямую; не допустить двойного включения этого cpp в один executable после смены linkage. Не линковать fake model queue с настоящим queue типом.

Сейчас existing project/filters/solution не изменены. P1/P3a владеют только своими новыми test projects. P2b/P3b и техники не начаты.
