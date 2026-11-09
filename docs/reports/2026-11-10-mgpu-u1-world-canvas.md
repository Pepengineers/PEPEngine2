# U1. Анимированные плакаты и качество UI

2026-11-10. Ветка `codex/mgpu-ui`, база U0 `b02083f496ddc9c327c8d9ae95a07c27705a3630`.

`Render.UI.Quality` задаёт Low (0, по умолчанию) и Medium (1). Atrium и Terrace имеют отдельные CanvasId, размеры и фазы. При смене размера или качества растёт SurfaceRevision. Оба профиля используют одинаковый animation clock для данного кадра.

| Canvas | Качество | Raster | Effects | Groups на compute dispatch | Звёзды | Радиус blur | RGBA8 bytes/update |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Atrium | Low | 384×216 | 96×54 | 12×7 | 20 | 2 | 331 776 |
| Atrium | Medium | 512×288 | 171×96 | 22×12 | 40 | 5 | 589 824 |
| Terrace | Low | 480×270 | 120×68 | 15×9 | 20 | 2 | 518 400 |
| Terrace | Medium | 640×360 | 214×120 | 27×15 | 40 | 5 | 921 600 |

Каждый кадр записывает градиент, горизонтальный blur и вертикальный blur с HP-шкалой и звёздами. Medium добавляет слой детали градиента и halo звёзд. Raster использует U0 и выдаёт непрозрачный линейный RGBA8 для обычного материала. В таблице логический размер payload без pitch padding. Размеры raster/effects и alpha проверены GPU readback.

Параметры raster и число звёзд следуют соотношениям со стр. 7–8 `2026-Вестник-UI.pdf`. Сетка эффектов, ядро blur и визуальный вид адаптированы для процедурных плакатов. [Закреплённый студенческий исходник](https://github.com/Zakgard/Multi-GPU-UI-Framework/tree/a0a80c03438c5315afccfc5cdf2e91cfa488dc9f/MGPU-IMGUI) · [Аудит источников](../research/2026-11-08-mgpu-source-audit.md).

## Проверки

| Проверка | Результат | State в `Build/U1` |
| --- | --- | --- |
| Debug/x64 и локальный /Z7 | PASS | `build-world-canvas-debug-r3.json` |
| Standalone INI seed | PASS | `seed-world-canvas-config.json` |
| CPU CVar, округление, clock и revisions | PASS | `world-canvas-cpu-r3.json` |
| D3D12 Debug Layer | PASS | `world-canvas-debug-layer-r3.json` |
| D3D12 GPU-Based Validation | PASS | `world-canvas-gbv-r3.json` |
| Task-only `git diff --check` | PASS | |

Readback показал анимацию обеих поверхностей, отличие результатов Low/Medium, яркие GPU-звёзды и текст ImGui. Для одной и той же Low-версии выходы effects и raster совпали побайтово между DIRECT producer и COMPUTE producer с DIRECT consumer. При смене качества фикстура удержала старый submission за GPU gate, выделила новые targets и прочитала оба размера после завершения. В финальных прогонах D3D12 errors отсутствовали.

Первый build обнаружил неверную ссылку на флаг CVar. Финальная регистрация использует default flag и `Count` sentinel для проверки диапазона и enum dropdown. Фикстура использовала локальный минимальный `Engine.ini` из U0. Runner сохранил SHA-256 входов и output executable. `processes-final.json` фиксирует ноль дочерних workloads.

## Подключение

Интегратор регистрирует WorldCanvas.h/.cpp, CanvasEffects.hlsl и BistroUICanvases.h, создаёт геометрию и opaque/emissive material bindings для двух плакатов. Каждый frame/slot удерживает собственный SRV binding, descriptor range и texture allocation до retirement читателей. DIRECT остаётся маршрутом по умолчанию. Отдельный `Render.UI.AsyncCompute` и GPU-зависимость COMPUTE → DIRECT принадлежат wiring runtime. Проверки U1 выполнялись в standalone fixture. Сцена Bistro, U2 transfer и производительность остаются за пределами этих прогонов.
