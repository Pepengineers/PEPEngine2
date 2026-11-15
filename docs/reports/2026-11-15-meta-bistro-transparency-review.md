# Проверка прозрачности перед переносом reflection probes

Проверены `069e9eb4` (перенос прозрачности), `75db74ad` (материалы), задание
`docs/tasks/2026-11-15-meta-bistro-transparency.md` и связанный код. Блокирующих
замечаний по переносу обычных прозрачных мешей не найдено. Это проверка исходников
и сохранённых артефактов, а не новая визуальная или performance-приёмка.

- `SceneLighting` передаёт deferred HDR в `SceneTransparency`, а результат — в
  существующий display continuation. Добавлены один All draw и один resolve.
- `SceneTransparency` использует transparent indirect stream 6/7 и
  `TRANSPARENT_UNBATCHED`: InstanceID остаётся исходным индексом. CPU-сортировки,
  нового обхода сцены, readback и Flush этот путь не добавляет.
- Accumulation/revealage очищаются в 0/1 каждый кадр, включая нулевое число draw.
  Reverse-Z GREATER_EQUAL, read-only DSV и DepthWriteMask ZERO сохранены.
  Alpha, вес и blending соответствуют App; пустой resolve сохраняет opaque HDR.
- Объявлены чтения geometry, indirect, constants, IBL, depth и shadow resources,
  записи render targets и чтения resolve. Ресурсы принадлежат кадровым слотам;
  входы обновляются перед записью, resize сохраняет размеры текущего HDR.
- Forward PBR получает `ShadowLights` того же слота с опубликованными slice/mask.
  Общий `LocalShadows.hlsli` учитывает per-face mask. Glass не использует
  opaque light grid или screen-space AO фона.
- Blended bounds теперь включены и в первичное построение BVH получателей теней,
  и в incremental membership. `CastsShadow` исключает layer 2; смена слоя
  инвалидирует старые/новые caster bounds и обновляет leaf без обязательной
  перестройки дерева. Камера сама по себе не запускает полный сбор входов сцены.
- Static-IBL shader variant исключает probe/atmosphere reads; All entrypoint
  не читает separator depth. Resolve компилируется с SKIP_OPAQUE_AERIAL.
  Старые App-варианты сохраняют прежние значения настроек по умолчанию.

Проверены `Build/Verification/meta-bistro-transparency/build-console.log`,
`ordinary-run.log`, `opaque-operations.txt` и `opaque-steady-operations.txt`.
Сборка выдала целевой exe (с известными LNK4099); запуск скомпилировал VS/PS
прозрачности и composition, штатно завершился, ERROR/FATAL отсутствуют.
В dump после lighting стоят только mesh transparency и WBOIT resolve перед display.
Визуальное наблюдение стекла и бутылки описано исполнителем в отчёте переноса;
здесь оно не воспроизводилось. `git diff --check ed5722af 75db74ad` прошёл.

`75db74ad` отдельно меняет `Paris_LiquorBottle_01_Glass1` (opacity 0.25 → 0.50,
Ns 5000 → 100) и убирает normal-map reference у placemat. Отчёт связывает это
с запросами пользователя. Поэтому утверждение ранней части отчёта «материалы
не менялись» относится к `069e9eb4`, а не к текущему HEAD. Для будущего сравнения
App/Meta нужно использовать одинаковую текущую ревизию assets. Исчезновение
полос на placemat остаётся отдельной непроверенной визуальной границей.

Новый запуск, сборка, тесты, PIX и замеры не выполнялись: повтор успешного smoke
не требуется по AGENTS.md. Resize/пустая выборка/смена слоя проверены только
по коду. Стоимость прозрачности и окончательную картинку принимает пользователь.

Следующий предложенный этап — локальные reflection probes, затем отдельно SSR.
Задание: `docs/tasks/2026-11-15-meta-bistro-reflection-probes.md`.
Обычный путь сейчас не создаёт SceneReflections. Его существующий диагностический
путь нельзя просто включить: он обращается к отсутствующему `geometry._shadows`,
делает безусловный readback опубликованных mip и полагается на полный scene
snapshot. Эти зависимости явно учтены в следующем задании.
