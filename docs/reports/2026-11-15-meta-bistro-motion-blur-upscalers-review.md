# Review: Motion Blur и апскейлеры

Проверены `c94a5083` (readbacks/atlas), `ba3b4f5d` (postprocessing) и их
интеграция с частицами в текущем HEAD `66dedd66`. Это source review, код не менялся.
Посторонняя staged правка SceneManagerModule сохранена.

## Вывод

Подтверждённых замечаний, требующих исправления перед следующим этапом, не найдено.
Предыдущие два CPU-замечания закрыты. Следующий этап — локальные reflection probes,
с остановкой на пользовательскую приёмку. Это не утверждение о runtime-проверке
всех SDK, presets и переключений.

## Проверенные контракты

- SceneGeometry больше не создаёт ordinary Image/Batch/Scene READBACK и offsets.
  После добавления техник readback resources удаляются с remap accesses/bindings;
  исходные operation IDs для записи сохранены.
- UI переиспользует immutable AtlasSnapshot по identity, UniqueID, dimensions и
  generation; GPU cache сравнивает snapshot, не pixels. В bundled ImGui NewFrame
  устанавливает TexIsBuilt=true, реальные dirty rect/pixel изменения сбрасывают его.
  Старые snapshots/resources удерживаются кадрами; generic discard contract сохранён.
- HDR после transparency/additive, depth, GBuffer velocity и явно переданный
  particle feature motion идут в motion/blur → SR/native → display → UI → PRESENT.
  DoF, вода и FG не включены.
- Native/NIS без blur пропускают motion preparation, gather, particle motion и
  HDR CopyResource. Temporal SR получает complete motion даже при выключенном blur.
  Output links обновляются при bypass/reset; Native LinkOutput обновляется каждый
  кадр, reset flags очищаются и без SDK. Alias accesses не конфликтуют.
- Перед заменой SDK/tile targets дренируется исполнитель. Resize определяет render
  grid, затем inputs связываются заново; SetInputs доходит до provider на каждом слоте.
  SDK history сохраняется между кадрами, а не создаётся на каждый frame slot.
- Provider flags участвуют в camera jitter, Native/NIS не выставляют USE_JITTER;
  GBuffer motion использует unjittered matrices. UI остаётся в output resolution.
- Сохранены Native/XeSS/FSR/DLSS/NIS CreateProvider, preset validation и fallback
  status. NIS реально читает только color: отсутствие depth/motion accesses в его
  packet соответствует коду Execute.

## Evidence и ограничения

Прочитаны diff/callers, shared MotionBlur/Reconstruction/providers, camera history,
SceneParticles, SceneGeometry remap и ImGui atlas lifecycle. Сохранённые
`Build/Verification/meta-bistro-motion-blur-upscalers/build.log` и `ordinary-run.log`
подтверждают сборку executable и обычный запуск без ERROR/FATAL. Предупреждения —
Jolt.pdb/LNK4099 и бюджеты импорта OBJ. Steady operations содержат motion preparation,
reconstruction, display, один UI composite и PRESENT, без DoF/FG.

В [отчёте реализации](2026-11-15-meta-bistro-motion-blur-upscalers.md) зафиксированы
effective XeSS без fallback, render 985×616 / output 1280×800 и пользовательское
подтверждение blur/чёткого UI. Это сохранённая визуальная приёмка, не новое наблюдение
reviewer. Успешный smoke по AGENTS.md не повторялся.

Новые сборки/тесты, матрица providers/presets, resize/toggle stress, точность и
профилирование не выполнялись. Runtime evidence относится к XeSS/Default;
FSR/DLSS/NIS и прочие режимы проверены только по коду. Прирост FPS/CPU/GPU не заявляется.
