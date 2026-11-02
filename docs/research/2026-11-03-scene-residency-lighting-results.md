# Bistro: DEFAULT scene inputs и группы deferred lighting 8×8

Дата: 2026-11-03. База: `bc5d5ea`, ветка `codex/bistro-rendering`.
Выполнены пункты 1 и 3 [анализа occupancy](2026-11-03-render-occupancy.md).
Пакетирование local-shadow faces и изменение алгоритма теней не выполнялись.

## Изменения

- Main GPU culling, CSM compute/draw, opaque depth prepass/G-buffer и WBOIT
  читают уже существующие frame-local DEFAULT mirrors: ShadowTemplates,
  ShadowInstances, ShadowMaterials, ShadowTransforms; WBOIT lights — ShadowLights.
  Новых buffers, allocations, copies или runtime settings не добавлено.
- CPU caches остаются источниками обновлений. Существующие
  PrepareLocalShadowData/UploadLocalShadowData вызываются безусловно перед
  render graph, после CPU scene preparation и ожидания frame-slot fence.
  Зависимости от включения local shadows не появилось.
- DeferredLighting CS использует 8×8 threads вместо 16×16; production dispatch
  и dispatch scalar/brute-force тестовых PSO согласованы. Старый независимый
  DepthAdaptiveLighting baseline в тесте сохраняет группы 16×16.
- Проверен вариант с поздним чтением emissive: 1,025840 против 1,025744 мс
  у простого 8×8. Дополнительного выигрыша не показал и удалён из итогового diff.
  Порядок вычислений lighting в финальном HLSL сохранён.

Источники:
[frame snapshot](../../Engine/Engine.RendererDX12/Include/Engine.RendererDX12/GDX12FrameConstants.h),
[culling](../../Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12GPUCullingPass.h),
[CSM](../../Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12CascadedShadowPass.h),
[opaque](../../Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12OpaquePass.h),
[WBOIT](../../Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12WBOITTransparencyPass.h),
[lighting dispatch](../../Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12DeferredLightingPass.h),
[HLSL](../../Engine/Engine.RendererDX12/Shaders/DeferredLighting.hlsl).

## Сопоставимые GPU-запуски

RTX 3060 Laptop, драйвер 610.88, Release x64 /O2 /GL /MD /Zi, PROFILE_BUILD,
1280×800, XeSS, defaults Bistro, неподвижная камера у входа, продолжающаяся
симуляция исходных 346 props и бюджета 250000 частиц.
Runtime INI — Render.NumFrames=3.

Native Nsight GTAPI: Ampere GA10x, Top-Level Triage, PM bandwidth 1000,
trigger frame 180, 12 кадров/запуск, PC sampling off, terminate after session.
Для baseline и финала — по три независимых процесса, для промежуточного DEFAULT —
два процесса. Итого основное сравнение — 96 кадров.
Один дополнительный baseline-PC trace содержит 6 кадров; вариант late emissive —
12 кадров и в основное сравнение не включён.

Baseline EXE SHA-256:
`7121D886841F960CF80607EFEF09AEDD2E3FF26C8F7F707DA184AE9CFC8A02D2`.
DEFAULT-only:
`77EA14B619B7BD88954F19182B05D5E9FADB300D493DA9DA895D483AE3754525`.
Final:
`C58F0CBCB3A07C82972DCA20F987F964729AB036DBF57ADFF951C411F37577B5`.
Final DeferredLighting.hlsl SHA-256:
`10C4EA5D37C6F7495624EE51A096B470C6B1F7EE648174490F7F6D6E6A6C372F`.

HLSL компилируется при запуске. На время повторных baseline/DEFAULT-only запусков
восстанавливался baseline HLSL 16×16; перед final запуском — точная копия final 8×8.
После всех экспериментов её SHA-256 проверен повторно.
Использованы сохранённые EXE соответствующих вариантов с одинаковым working directory.

Во всех восьми основных trace — 12 кадров и нет overflow warnings.
Единственное предупреждение — удалённые отсутствующие per-shader metrics трёх
шейдеров, как в исходном исследовании; они не использованы.
GPC frequency в whole-trace native queries первого baseline/final —
899,999989 / 899,999984 MHz.
Это короткие окна под Nsight, не обычный FPS benchmark.

### Время

Ниже — медиана медиан отдельных запусков; строки проходов нельзя складывать
для получения медианы кадра.

| Метрика, мс | Baseline, 3 запуска | DEFAULT-only, 2 запуска | Final 8×8, 3 запуска |
| --- | ---: | ---: | ---: |
| Full frame | 33,849824 | 33,717400 | 33,926656 |
| Main culling | 0,284848 | 0,020784 | 0,021488 |
| CSM | 7,876592 | 7,534768 | 7,692128 |
| Deferred lighting | 1,077728 | 1,076552 | 1,025744 |

Main culling baseline → final: **−92,46%**, экономия около **0,263 мс**.
Lighting DEFAULT-only → final: **−4,72%**; baseline → final **−4,82%**,
экономия около **0,052 мс**.
Для CSM направление улучшения есть, но величина меняется между процессами.
Prepass/G-buffer не показали существенного ускорения:
1,155872 → 1,165200 и 2,349184 → 2,340000 мс соответственно.

| Запуск | Baseline full frame | Final full frame | Baseline lighting | Final lighting |
| --- | ---: | ---: | ---: | ---: |
| 1 | 33,810592 | 33,656400 | 1,079936 | 1,025744 |
| 2 | 33,957008 | 33,926656 | 1,076480 | 1,027104 |
| 3 | 33,849824 | 33,941808 | 1,077728 | 1,025520 |

**Ускорение полного кадра / FPS не подтверждено.**
Full-frame median выросла на 0,23%. Стоимость Particles между вариантами —
2,854912 → 3,100272 мс, Front composition с sorted/additive particles —
1,679696 → 2,028032 мс. В более быстром до этого участке сценарий достигает
frame 180 при другой фазе симуляции; одинакового particle/prop state и
одинакового wall-clock момента эти окна не гарантируют.
Эти различия не устанавливают причинную регрессию частиц от новых bindings,
но и не позволяют объявить измеренное сокращение времени отдельных проходов
приростом FPS. Настройки или симуляция для улучшения цифр не отключались.

### Occupancy и регистры

Whole-trace native aggregate первого baseline/final:
**27,083567 → 27,256287%** (VTG + PS + CS).
SM throughput: 26,4251 → 27,8535%; PCIe throughput: 6,9979 → 6,7966%.

Для одного среднего кадра (frame 6) соответствующих trace:

| Метрика lighting | Baseline 16×16 | DEFAULT-only 16×16 | Final 8×8 |
| --- | ---: | ---: | ---: |
| CS warp occupancy, % | 59,2672 | 59,0433 | 59,3506 |
| CS register file allocation, % | 88,9245 | 88,5777 | 89,0201 |
| SM throughput, % | 73,0673 | 73,0316 | 74,8390 |

Рост occupancy или снижение register allocation не подтверждены.
Выигрыш 8×8 установлен по времени lighting, а не выведен из процента occupancy.
Статистики frame 6 не являются средними всех кадров или per-shader compiled
register count. Снижение числа регистров и отсутствие driver spills не заявляются.

## Проверки

- Release x64 App и LightingGpuTests успешно собраны; также собраны
  ReflectionProbeTests, BistroStartupTests, BistroRenderGraphTests.
- Регрессия snapshot в LightingGpuSmoke сначала упала:
  после upload DEFAULT templates CPU command обнуляется; старый CSM читает
  изменённый UPLOAD и теряет caster. После смены bindings тот же depth readback проходит.
  Начальный CSM тест выполняется до создания локальных lights.
- LightingGpuTests с DX12 debug layer прошёл на DEFAULT-only и final 8×8:
  frame-slot template edits/growth/deletion, CSM/local shadow depth,
  непрозрачный/прозрачный HDR, освещение, AO.
  Brute-force, scalar и независимый legacy baseline совпали в допуске
  на 1×1, 17×19, 127×73, 320×180, 1280×720; 0..769 lights,
  sky/baked/mixed/depth layers, jitter и infinite reverse-Z.
- ReflectionProbeTests --debug: depth/normal prepass и Equal G-buffer,
  opaque/transparent PBR, HDR, XeSS resize/reset, frame slots,
  dynamic/baked probes и runtime switches — pass.
- BistroStartupTests: исходная сцена, 346 props/250000 particle budget,
  90 frames, focus/minimize/resize — pass без debug-layer errors.
- BistroRenderGraphTests: pooled/dedicated HDR identity, оба AO placements,
  нечётный resize и native/XeSS switch — pass.
- Проверка SHA-256 восстановленного HLSL, численной сводки и git diff --check — pass.
- Focused ce-code-review: собственная проверка correctness и независимый
  локальный adversarial reviewer; замечаний нет. Внешняя модель не использовалась.
- Сборки сохраняют существующие C4267/C4018/C4828 и сторонние LNK4099 warnings,
  наблюдавшиеся также при red-сборке до production changes.

Артефакты локально, исключены Git:
`Build/Captures/2026-11-03-scene-residency/`.
Там trace, EXE/HLSL snapshots, analysis-initial.json, analysis-final.json,
manifest.json с hashes и итоговой сводкой, repeat-runs.json и review/review.json.
Build/run logs: `Build/occupancy-*.log`.

Другие GPU, полный набор тестов проекта, Debug-конфигурация,
длительный soak и детерминированный benchmark полного кадра не проверялись.
