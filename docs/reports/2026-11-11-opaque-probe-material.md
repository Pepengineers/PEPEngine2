# Opaque cubemap material и per-object shadows

## Результат

Preview-сферы Bistro используют обычный opaque culling, depth prepass и GBuffer. Deferred lighting возвращает HDR радиацию собственной cubemap, выбранной через one-based slot в FP16 emissive alpha. Освещение, AO и PBR не применяются к этому custom material. F0Baked alpha исключает SSR. Capture mode 2 остаётся frozen static.

Отдельные GDX12ReflectionPreviewPass, ReflectionPreview.hlsl, ReflectionPreviewDraws и их pipeline/build references удалены. Нормали, depth и motion теперь публикуются до AO/lighting в тех же проходах, что у остальной opaque geometry.

StaticMeshRenderComponent.CastsShadows и SkeletalMeshRenderComponent.GetCastsShadows/SetCastsShadows управляют отбрасыванием теней отдельного объекта, default true. Static mesh поддерживает CastsShadows в YAML и обнаруживает изменение public поля без DirtyFlag. Instance data C++/HLSL содержит отдельный UINT CastsShadows, stride 52 bytes. Флаг проходит в CPU primitive snapshots, sun/local GPU culling и raster, shadow capture внутри probes и dynamic shadow command selection. Native shadow snapshot уже исключает primitives с CastsShadows=false до транспорта. Bistro-сферы явно устанавливают false. Обычная видимость и receiving остаются частью обычного material/depth pipeline.

## Проверки

Последовательная Debug x64 сборка MSVC v145. После изменения layout полностью пересобраны Engine.RendererDX12, Engine.UI, App.Base и App, затем owning test projects. Основные flags приведены ниже.

```powershell
$msbuild = 'C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/amd64/MSBuild.exe'
& $msbuild <project> /t:Rebuild /p:BuildProjectReferences=false /p:Configuration=Debug /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /nr:false /nologo /v:minimal
```

Все перечисленные сборки и запуски завершились с exit 0.

| Suite | Проверка |
|---|---|
| ReflectionProbeTests | Полный запуск с GBV. HDR >1, cube slice/mip, invalid/unpublished slots, pure color при освещении и AO, оба early/late prepass routes, normals/depth/motion readback, движущаяся opaque geometry перед/за preview, WBOIT depth, self-capture exclusion и фактические dynamic updates в Implementation 0/1, frozen 2 |
| RenderPipelineTests | Primitive и prepared geometry contracts |
| BaseRenderPipelineTests | Default pipeline graph/resize |
| ObjectDataJobsTests | CastsShadows без DirtyFlag, все frame slots, static cache invalidation, visibility и полный CPU reference scan |
| LightingGpuTests | D3D12 debug layer, реальные sun/local depth readbacks при CastsShadows 0/1, static и dynamic local cache invalidation |
| InstancingTests | Полный Bistro runtime, skeletal flag в трёх slots, shared-material peer, actual batching и individual animation |
| BistroRenderGraphTests | Обычный Bistro graph и prepared geometry sources |
| MGPUShadowSceneTests --gpu-validation | Native replica layout/residency/skin palette/grass material checks с GBV на одном адаптере |

ReflectionProbeTests запускался из собственного runtime с минимальным INI, содержащим Render.NumFrames=3. Такой же минимальный конфиг применён к BaseRenderPipelineTests, ObjectDataJobsTests и MGPUShadowSceneTests. Исходные существовавшие runtime INI восстановлены. Полный последовательный harness сохранён в Build/OpaqueProbeMaterial/validate.ps1.

## Raw evidence

Все пути относительно корня checkout.

- Build/OpaqueProbeMaterial/rebuild-exits.log и rebuild-*.log — согласованная пересборка нового layout.
- Build/OpaqueProbeMaterial/reflection-r1.json, reflection-r1.stdout.log, reflection-r1.stderr.log — полный reflection GBV и opaque cubemap readback.
- Build/OpaqueProbeMaterial/test-exits.log — сводка остальных семи suites.
- Build/OpaqueProbeMaterial/test-<Suite>.json и test-<Suite>.stdout.log/.stderr.log — individual runner states и вывод.
- Build/OpaqueProbeMaterial/build-<Suite>.log — полные build logs.

## Ограничения

Screenshot-specific temporal smear визуально не воспроизведён. Поздний preview pass устранён, а GPU readbacks покрывают перекрытия и общие depth/normal/motion данные. Release и offload на двух физических адаптерах не проверялись. Сохранение World в YAML в проекте остаётся существующим stub, добавлено чтение static mesh CastsShadows и API для skeletal component. Чужой BistroAdditionsTests.cpp не редактировался.
