# GPU-частицы, жидкость и скелетная анимация

Дата проверки: 2026-10-31. Статус: исследование и рекомендации для обсуждения; код не менялся. Новые демонстрационные объекты в Bistro не предлагаются. Для облаков обсуждается только перенос шейдеров, без нового эффекта на сцене.

Уточнения, принятые после исследования (2026-11-01): нужна полноценная поверхность
воды с шейдером; дочерняя GPU-эмиссия по событиям отложена, но обычные emission/
simulation/sorting/draw остаются полностью на GPU. Трава, частицы, вода и анимация
поставляются как компоненты без добавления демонстрационных объектов в Bistro
или новую сцену. Облака также не добавляются в сцену. Нужны CVar включения,
параметров, качества и поддерживаемых реализаций; контракт — в основном документе.

## Что есть в локальном коде

- Старые обычные частицы: `D:/Project/Multi-GPU-Render-DX12/MGPU-Particles/Shaders/ComputeParticle.hlsl:27` и `Common/ParticleEmitter.cpp:276`. Есть pool, alive/dead counters, injection, время жизни, атлас, сила и интеграция. При смерти шейдер уменьшает alive counter, но использует индексированный alive list; этот контракт требует отдельной проверки. Копировать его как доказанно корректную compaction нельзя.
- Жидкость: `MGPU-FluidSimulation/Engine/Common/SharedFluidParticleEmitter.cpp:359,505` и `Shaders/FluidSimulation.hlsl:119,197,235,325,377` в том же старом репозитории. Есть substeps, spatial hash, reorder, density/near-density, pressure, viscosity и интеграция. `FluidParticleDraw.hlsl:43` показывает цветные billboard-квадраты по скорости, а не поверхность воды. Private `Engine/` несовместим с общей поддержкой старого фреймворка: это прямо оговорено в `MGPU-FluidSimulation/README.md:8`.
- Старый skinning: `MGPU-Animations/SkinnedData.cpp:192`, `Shaders/Default.hlsl:55`, `Shaders/VelocityDisplacement.hlsl:33,79`. Базовый LBS дополнен velocity displacement. В compute shader нет проверки vertex count; dispatch округляется вверх в `SkinnedMeshApp.cpp:2360`. В `SkinnedData.cpp:122,125` скорость делится на разность текущего времени и времени ключа, включая ноль на границе. Этот вариант не является готовой безопасной основой.
- Текущий `Engine/Engine.Core/src/IO/MeshImporter.cpp:640` импортирует статические позиции/нормали/UV/tangent и запекает node transform. `Engine/Engine.Core/Include/Engine.Core/Types/MeshTypes.h:14` не содержит joint indices/weights. Поиск в текущих importer/types не выявил импорта bones/clips. Assimp уже используется, но наличие зависимости не означает готовую поддержку анимации.

## GPU-частицы: рекомендуемый контракт

Это предложение по устройству, а не утверждение о текущем движке:

1. CPU задаёт emitter transform, параметры, seed, burst/start/stop и лимиты. CPU не создаёт массив новых частиц, не считает alive и не сортирует их каждый кадр.
2. GPU хранит particle pool, dead freelist, плотные alive lists A/B, visible list и counters. Spawn получает свободные слоты с проверкой вместимости; недостающие слоты ограничивают emission, не приводят к выходу за буфер. Rate accumulator и случайная генерация живут на GPU.
3. GPU симулирует живые частицы, возвращает умершие индексы во freelist и записывает выживших в новый плотный список. Счётчик выходного списка сбрасывается перед проходом. Простая append-compaction достаточна; wave-aggregated append/scan добавляется по измерениям.
4. GPU culling формирует visible list; сортируется только этот список индексов, не весь particle pool. GPU пишет indirect dispatch/draw arguments. Между UAV producers, SRV consumers и indirect arguments нужны корректные barriers/transitions.
5. Рендер читает индексы через `SV_InstanceID`, разворачивает billboard в VS и вызывает существующий путь `ExecuteIndirect`. Геометрический шейдер и CPU count readback не нужны для базового варианта. Асинхронный readback допустим для телеметрии/проверок, без зависимости следующего кадра от результата.

Основа для адаптации — [AMD GPUParticles11](https://gpuopen.com/learn/gpuparticles11-directx-11-sdk-sample/) и [его исходник](https://github.com/GPUOpen-LibrariesAndSDKs/GPUParticles11/blob/master/gpuparticles11/src/GPUParticleSystem.cpp): sample демонстрирует GPU emission/simulation, counters, optional sorting, indirect billboard drawing и отдельный tiled compute renderer. CPU readback counters в нём ограничен debug; DX11 API необходимо адаптировать к DX12. Лицензия sample — MIT; сохранять notices при заимствовании.

### Рендер и сортировка

| Вариант | Когда брать | Ограничение |
|---|---|---|
| Instanced billboard + additive | Искры/свечение | Не моделирует поглощение/закрытие дальнего слоя |
| Instanced billboard + существующий WBOIT | Базовый несортируемый прозрачный путь | Приближённый цвет и слои; сортировка перед WBOIT не превращает его в alpha blending |
| GPU sort + premultiplied alpha | Эффекту нужен выраженный порядок слоёв | Сортировка центров частиц не решает все пересечения; порядок с бутылками/другими прозрачными объектами требует отдельного решения |
| Half-resolution + depth-aware upscale | Большие мягкие дым/туман | Может терять тонкие силуэты; резкие осколки оставлять full-resolution |
| Tiled compute particle rendering | Отдельно измеренный тяжёлый overdraw | Дополнительные bins/lists, контроль overflow, compositing и собственная стоимость; не объявлять быстрее без сравнения на целевой GPU |

[Авторское описание WBOIT](https://casual-effects.blogspot.com/2015/03/implemented-weighted-blended-order.html) объясняет accumulation/revealage, отсутствие сортировки и приближения: близкие слои различаются хуже; weight требует настройки под диапазон глубин. Вес из OpenGL-примера нельзя переносить буквально в reverse-Z.

[GPU Gems 3, глава 23](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-23-high-speed-screen-particles) обосновывает рендер мягких частиц в уменьшенное разрешение с уменьшенным depth и последующей композицией. Метод полезен при fill-rate/overdraw, но менее пригоден для высокочастотных деталей. Это вариант качества по эффекту, а не автоматическое правило для всех частиц.

Для сортировки есть два практичных источника:

- Локальный MiniEngine bitonic путь уже используется в `D:/Project/Multi-GPU-Render-DX12/MGPU-Grass/GrassSorter.cpp:13`; небольшие/умеренные списки можно начать с него, проверив лицензии вложенных исходников.
- [FidelityFX Parallel Sort](https://gpuopen.com/fidelityfx-parallel-sort/) — MIT radix sort, DX12/Vulkan, SM6+, 32-bit key+payload, direct/indirect execution. Для больших меняющихся visible lists это предпочтительный кандидат на сравнение. Переносить только нужные kernels/интеграцию, а не добавлять целый demo framework. Выбор победителя требует GPU timings, одних асимптотик недостаточно.

Предлагаемый первый набор настроек: capacity, spawn rate/burst, lifetime, начальные position/velocity distributions, color/size over life, force/drag, atlas, blend mode, optional sorting и resolution scale. Это параметры одной GPU-системы, не узловой редактор VFX. Какие из них обязательны в первой поставке, решает пользователь.

## Жидкость: симуляция и поверхность отдельно

Симуляцию старого sample адаптировать к ресурсам/очередям текущего движка; не тащить его private Engine. Spatial-hash sort нужен самой симуляции для поиска соседей и не заменяет depth sort обычных частиц.

Рекомендуемый отдельный surface renderer: sphere-impostor depth + additive thickness → depth smoothing → reconstruction normals → HDR shading с Fresnel/reflection/refraction и absorption → композиция с цветом/глубиной сцены. Диагностические particles/speed/density оставить режимами просмотра. Стартовые визуальные настройки: particle radius, smoothing scale/iterations, IOR, absorption/tint, roughness и render resolution; solver settings (dt/substeps/density/pressure/viscosity) держать отдельно.

Для smoothing рекомендую алгоритм [Truong/Yuksel, A Narrow-Range Filter for Screen-Space Fluid Rendering, 2018](https://ttnghia.github.io/posts/narrow-range-filter/), [полный текст](https://www.cemyuksel.com/research/papers/narrowrangefilter.pdf). Он сглаживает непосредственно depth particles и специально обрабатывает разрывы, сохраняя границы. Сферический particle depth — достаточная первая версия; anisotropic kernels повышают качество тонких областей ценой дополнительной работы. Результат зависит от камеры и не даёт настоящую 3D surface mesh.

У авторов есть [reference implementation](https://github.com/ttnghia/RealTimeFluidRendering), но её [licenseheader](https://github.com/ttnghia/RealTimeFluidRendering/blob/master/FluidRendering.licenseheader) говорит All rights reserved. Разрешительная лицензия в просмотренных материалах не подтверждена: реализовать описанный в статье алгоритм самостоятельно, не копировать reference code без выяснения прав.

Поверхность воды с полноценным шейдером выбрана пользователем; диагностические частицы её не заменяют. Sphere/anisotropic варианты, пересечение с OIT-стеклом и прозрачными частицами, а также расширение существующих box collisions требуют инженерной проверки по выбранной сцене. Дополнительные виды коллизий сами по себе не следуют из запроса водного шейдера. Обычный screen-space refraction не решает многослойное стекло и отражения невидимых объектов.

## Скелетная анимация: рекомендуемая первая версия

Использовать существующий Assimp для skin/clip import, сохраняя hierarchy, inverse bind matrices, joint indices/weights и локальные TRS-каналы вместо static world-bake. glTF/GLB удобен как первый проверяемый формат; FBX остаётся дополнительным источником через тот же importer. Контракты формата сверять с [glTF specification: skins и animations](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html): не все узлы hierarchy обязаны быть joints; порядок joints и inverse binds должен совпадать.

Предложение: CPU jobs вычисляют animation pose/blending и joint matrices; GPU compute выполняет LBS один раз на активный mesh instance, сохраняет текущие/предыдущие позиции для main, shadow и motion-vector passes. Подход reuse обоснован в [Khronos: Advanced Skeletal & Compute Skinning](https://docs.vulkan.org/tutorial/latest/Advanced_glTF/Skeletal_Compute_Skinning/01_introduction.html). Для одного персонажа VS skinning проще; при многих shadow views текущего приложения повторный skinning каждого pass — измеримый аргумент за compute, а не гарантия выигрыша.

Обязательные проверки импорта: finite transforms, валидные indices, нормализация ненулевых weights, явная политика дополнительных influences (не терять их молча), duration/loop boundaries, отсутствие NaN на нулевой длительности ключа. Нельзя наследовать лимит 96 bones без причины. Нужны conservative animated bounds; bind-pose bounds недостаточны. Normals/tangents должны соответствовать деформации; предыдущая поза сбрасывается при spawn/teleport/clip reset. Velocity displacement — отдельный художественный эффект после правильной базовой анимации, с отдельным выбором пользователя.

### Готовые модели

| Источник | Что подтверждено | Пригодность |
|---|---|---|
| Локальный `Multi-GPU-Render-DX12/MGPU-Animations/Models/soldier.m3d` | Header: 58 bones, 1 clip, 13748 vertices; используется old app | Хороший локальный регрессионный ориентир, но M3D-specific importer и происхождение/лицензия модели не установлены |
| Локальный `VFXGraph_Siggraph-2021_Share/Assets/VFXGraph/11/Mesh Sampling/SkinnedMesh/Armature.fbx` | FBX содержит Deformer/Cluster/LimbNode/AnimationStack; `.meta:885` задаёт humanoid rig, `.meta:82` отключает Unity animation import | Кандидат для технической проверки. Это не доказательство готового работающего clip; отдельная лицензия данного FBX не найдена |
| Локальные StarterAssets animation FBX в `VFXGraph_Siggraph-2021_Share/Assets/Misc/StarterAssets/ThirdPersonController/Character/Animations` | Idle/walk/run/jump; `.meta` включает animation import; `StarterAssets/license.txt` указывает Unity Companion License | Не считать свободными для custom DX12 engine: [условия Unity Companion License](https://unity.com/legal/licenses/unity-companion-license) привязывают разрешённое использование к Unity Engine License; происхождение и применимость к данному проекту требуют выяснения |
| `PEPEngine2/Assets/Models/Svidetel.fbx`, старый `Data/Common/Objects/StoneGolem/Stonefbx.fbx` | Файлы существуют; простой поиск FBX markers не выявил skin/animation markers | Наличие FBX не доказывает rig; полноценная проверка Assimp не проводилась |
| [Khronos Fox](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/main/Models/Fox/README.md) | Готовый skin, Survey/Walk/Run | Рекомендованный воспроизводимый первый asset: [metadata](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/main/Models/Fox/metadata.json) задаёт CC0 для model, CC-BY-4.0 для rig/animation и conversion; сохранить attribution всех перечисленных авторов |
| [Khronos CesiumMan](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/main/Models/CesiumMan/README.md) | Textured animated skinned character | Дополнительная humanoid проверка; CC-BY-4.0, отдельная запись о Cesium logo/trademark. Fox проще как первый нейтральный baseline |

Модели не скачивались и не добавлялись в сцену. Локальные FBX не прогонялись через новый importer; внешние лицензии описаны по страницам владельцев и локальным notices, а не выведены из факта наличия файлов.

## Минимальная приёмка перед объявлением готовности

- GPU particles: zero/full capacity, burst при нехватке slots, массовая смерть, повторный spawn, неизменность particle IDs через optional sort; counters/args без CPU roundtrip; debug layer и GPU readback по малому детерминированному seed.
- Transparency: сравнительные captures additive/WBOIT/sorted alpha, пересечение с opaque depth и бутылками; full/half-resolution; GPU timings spawn/sim/cull/sort/draw по одинаковому workload.
- Fluid: solver invariants/finite values и reset; отдельно depth/thickness/normals, силуэт/разрывы surface, пересечение с геометрией и glass. Убедиться, что renderer не влияет на solver.
- Animation: bind-pose identity, ключи/loop/zero duration, больше 96 joints в синтетическом тесте, совпадающие main/shadow deformation и motion vectors; сравнение Fox с reference viewer.

Это будущие критерии, а не выполненные проверки. Выбор визуальных режимов, бюджета частиц/персонажей и обязательных solver collisions остаётся за пользователем.
