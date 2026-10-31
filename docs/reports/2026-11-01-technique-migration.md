# Реализация техник рендера

Требования: [согласованный документ](../research/2026-10-31-technique-migration.md).
Пользователь подтвердил реализацию 2026-11-01: сначала база и CVar, затем компоненты,
без новых объектов в сцене. Коммиты нужны для каждой проверенной итерации.
Разрешено включать затронутые файлы целиком вместе с прежними правками; остальные
незакоммиченные изменения не включаются автоматически.

## Проверенные итерации

### Настройки техник

- Коммит `ab061f9`, `feat(render): add validated technique settings`.
- Общие типы качества/выбора AO, параметры AO и motion blur; режимы Off, presets
  и ручные переопределения, которые не стираются изменением качества.
- `TechniqueTests` Release/x64 прошёл: все три AO, Off, монотонность бюджета,
  независимость override, отказ на неверных enum/радиусе/NaN/числе samples.
- Motion blur settings пока не означают готовый motion blur pass.

### Три AO и интеграция

- Реализованы отдельные SSAO, HBAO и Intel XeGTAO; единый выход visibility.
- Исправлены prefilter dispatch и reduction conditions старого XeGTAO, обработка
  denoise/packed scale, RH/world-normal/reverse-Z mapping и нечётных размеров.
- AO подключён после G-buffer к ambient lighting; direct/emissive/baked сохраняются.
  Прозрачный forward-путь пока использует neutral AO.
- `AOGpuSmoke` Release/x64 прошёл на RTX 3060 Laptop с DX12 debug layer:
  65x49, три frame slots, плоскость, ступень глубины, denoise 0/1/4, Off,
  конечные значения [0,1], отсутствие DX12 errors.
- Обнаруженное затемнение XeGTAO у угла незатенённой плоскости подтверждено
  независимым расчётом donor equations: 0.917647 при настройках smoke.
  Центр равен 1. Это исходная угловая аппроксимация, а не исправленная постфактум
  ошибка матрицы; тест отдельно проверяет нейтральный центр и известный baseline.
- `LightingGpuSmoke` прошёл: IBL, shadow depth/кэш, ExecuteIndirect и deferred
  lighting readback. Для него заполнены projection/common data нового AO.
- Release-сборка `Apps/App/App.vcxproj` прошла; `verify_scene.py`: 4 tests OK.
- Существующая Bistro загружена и отрисовала один настоящий кадр через уже
  имеющийся loading benchmark, exit 0. Report: `Build/ao-bistro-frame.json`.
  Время 42.37 s до готового кадра — единичный прогон, не performance baseline.
  Новые объекты для этого не создавались. Скриншот/длительное движение не проверялись.
- Логи: `Build/ao-gpu.log`, `Build/lighting-ao-gpu.log`, `Build/app-ao-build.log`.

## Подготовленная зависимость FidelityFX

- Реально загружен `v1.1.4`, commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`.
  Рабочий sparse checkout: `Build/FidelityFXSource`, не часть продуктового кода.
- Собраны Release SSSR, DoF, Reflections Denoiser, DX12 backend и shader permutations.
  CMake build: `Build/FidelityFXBuild`, log `Build/fidelityfx-build.log`.
- Это пока проверка сборки SDK, не доказательство интеграции SSR/DOF в App.

## Оставшаяся работа

SSSR/DOF integration и runtime CVar; motion blur; cubemap capture/cache/probes;
SkyBox/SkyAtmosphere, grass/wind, GPU particles, fluid surface/solver,
skinning/asset import и cloud shaders. Наличие отдельных новых файлов во время
работы не считается завершением этих пунктов без их callers и GPU-проверок.

## Среда сборки

В sandbox получен MSVC C1902; после host-сборки обнаружены старые повреждённые
LTCG debug objects (LNK1103). Проверенные прогоны использовали VS2026 host x64,
`_CL_=/Z7 /GL-` и один Rebuild локальных проектов. Настройки toolchain проекта
из-за этого не менялись. Внешние DirectXTex libs имеют предупреждения о missing PDB.
