# Проверка анимации/skinning и экранного UI

Проверен `69e7397d feat(meta): add Bistro animation and screen UI` и связанные
вызовы в текущей рабочей копии. Незакоммиченные изменения частиц и сцены сохранены.
Нагрузка: исходный Bistro, 10 people / 5 foxes, Mid skinning (30 Hz), три кадровых
слота, открытая Settings UI. Это source review, без новых runtime-прогонов.

## Что подключено корректно

- BistroCharacters обновляет маршруты при paused world через явный opt-in;
  skeletal pose имеет отдельный opt-in. App сохраняет прежние default semantics.
  Route jobs завершаются до render snapshot; физический модуль не включён.
- FreezeSkeletalMeshes передаёт работу в существующие COPY palette / COMPUTE
  deformation operations. Общие geometry resources объявлены перед depth/GBuffer
  и shadow consumers; статические geometry uploads не добавлены.
- DeformationRevision и instance journal уведомляют local shadow cache о смене
  позы даже при одинаковом bounds. Старые/новые bounds инвалидируют соответствующие
  карты/каскады; полный статический обход не возвращён в каждую pose update.
- Sun shader теперь правильно использует unbatched skin instance, а shared App
  shader сохраняет прежнюю ветку. Консервативный растущий sun fit документирован.
- Screen tail переиспользуется между двумя путями. UI идёт после display transform
  перед единственным PRESENT, slots удерживают draw/recording owners. Input capture
  проверяется для клавиатуры, мыши и колеса. GPU atlas reuse работает при заявленном
  контракте ordered submissions; обычные backend callers сохраняют discard semantics.

## Замечания

### 1. P2 — диагностические READBACK allocations в обычном кадре

`Apps/App.MetaRenderGraph/src/Techniques/SceneGeometry.cpp:175–200`.

При наличии skeletal batches создаётся BatchReadback, хотя обычный путь не выполняет
диагностическое чтение. Существеннее SceneReadback: его размер включает вершины
только текущего `s.Skin`. FreezeSkeletalMeshes возвращает только состояния с Dispatch;
при достаточно высокой частоте кадров между pose update и settle встречаются кадры
без skinning (например, 30 Hz poses при 120 FPS). Когда у повторно используемого
слота меняется размер этого списка, `SceneBytes`
меняется и CreateResource заново выделяет READBACK для всего snapshot, хотя
`diagnosticReadbacks=false` и соответствующая операция удалена из обычного плана.

Исправление: выделять BatchReadback/SceneReadback и вычислять диагностические offsets
только для диагностического пути, сохранив его существующую функциональность.
Без исправления обычная анимация может вызывать ненужные resource allocations
и расход системной памяти; величина затрат не измерялась.

### 2. P2 — кэш шрифта всё ещё обходит весь атлас на CPU каждый кадр

`Engine/Engine.UI/src/UI_test.cpp:283–284, 433–435`.

CaptureDrawData копирует весь RGBA font atlas в новый vector при каждом непустом UI
кадре. TryRecord затем сравнивает этот vector побайтно с CachedAtlas::Source.Pixels.
На cache hit GPU upload действительно отсутствует, но на неизменном атласе остаются
полная CPU-копия, allocation и линейное сравнение. Копирование существовало в backend,
но теперь выполняется и в обычном Meta Bistro с включённой Settings.

Исправление: переиспользовать immutable snapshot атласа по identity/generation,
обновляя его при реальном изменении ImGui texture; GPU cache сравнивает поколение,
а не pixels. Удерживать старое поколение до окончания кадров и сохранить общий
discard contract. Один mutable raw pointer не заменяет generation/lifetime.
Без исправления стоимость CPU UI зависит от площади атласа даже в неподвижной панели.

## Проверки и вывод

Прочитаны diff, callers, skinning publication/dirty journals, shadow invalidation,
Meta resource accesses, UI capture/backend/retirement и existing stage report.
Сохранённый `Build/Verification/meta-bistro-animation-ui/build.log` содержит успешный
выходной executable и LNK4099 про Jolt.pdb; актуальный сохранённый ordinary-run.log
не содержит ERROR/FATAL. Список операций содержит skin uploads/deformation и один
UI composite. Визуальная проверка персонажей, Settings/search/input/F1 описана
исполнителем в `2026-11-15-meta-bistro-animation-ui.md`; заново не воспроизводилась.

Подтверждённых функциональных блокеров не найдено. Перед завершением следующего
этапа исправить пункты 1–2; они внесены в его задание. Это вывод по коду, а не
замер FPS/CPU/GPU. Новые сборки, smoke, профилирование, проверки точности и матрицы
не выполнялись: успешная базовая проверка уже сохранена, код в этом review не менялся.
