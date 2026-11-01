# Интеграция второй группы — 2026-11-01

Все шесть реализаций объединены обычными merge-коммитами в `codex/bistro-rendering`, в единственной рабочей копии `D:/Project/PEPEngine2`. Исходные коммиты агентов остаются в истории текущей ветки. Удаление объединённых feature-веток удаляет только их имена, не код и не коммиты.

| Фича | Merge |
|---|---|
| SkyBox / SkyAtmosphere | `6ff383a` |
| Grass / wind | `d4be556` |
| GPU particles | `1e9bfa8` |
| SPH fluid / water | `e1f5aef` |
| Skeletal animation | `de5ce69` |
| Procedural 2D clouds | `d556c0f` |

Исправления интеграции: `35b05cb`. Исходная ancestry первой группы, ранее перенесённой и исправленной, объединена `66ecc94` без повторного применения устаревшего кода.

## Совмещение и исправления

- Объединены ECS-компоненты, project items, CVar и сбор данных кадра. Skeletal deformation и grass preparation выполняются до prepass; тени остаются после prepass/выбранного раннего AO. Материалы загружаются после lazy skeletal resource creation.
- В transparency разведены sky t11..t13, water/opaque depth t14/t15 и material textures t16. Вода сохраняет behind/front glass partition и собственные depth/motion для DoF, motion blur и XeSS.
- Front composition имеет отдельный HDR target, чтобы не читать и писать одну текстуру. При отсутствии воды front accumulation очищается для активных частиц; без front receivers результат проходит по identity link. Aerial perspective применяется к opaque background один раз.
- Clouds используют единый sky/IBL pass через явный `SetCloudSource`; автоматического второго неба нет. Непрерывная анимация обновляет IBL без постоянного temporal reset; смена источника/implementation сбрасывает историю.
- Исправлены зарезервированные HLSL-имена, повреждённое начало заголовка, недостающее forward declaration, неправильные shared_ptr/ComPtr вызовы и повторный import Assimp.
- Grass capture получил отсутствующий comparison sampler. Scratch SRV grass/fluid/particles имеют явный `DATA_VOLATILE` контракт. LUT атмосферы не привязываются для чтения до заполнения. Graphics SRV имеют корректные состояния; particles Composite устанавливает descriptor heap до привязки SRV и работает на новой command list.

## Проверки

Release/x64 App и AOTests, SSRTests, ReflectionProbeTests, DepthOfFieldTests, MotionBlurTests, ObjectDataJobsTests, SecondGroupTests собраны MSBuild с `_CL_=/Z7 /GL-`, `/m:1 /nr:false`. Все пять регрессионных CPU/GPU-наборов первой группы прошли; ReflectionProbe повторно подтвердил оба prepass пути и отсутствие записи normals в позднем режиме.

`SecondGroupTests` прошёл обычный debug-layer запуск и `--gpu-validation` с реальными GPU readbacks:

- CPU: component budgets, invalid input rejection, реальный Fox hierarchy/три clips и меняющаяся animation pose.
- GPU LBS: 128 bones, пять influences, zero-weight rigid attachment, current/previous positions против независимого translation oracle.
- Atmosphere/Clouds: Off, конечный ненулевой atmosphere cube, fBM/ridged, animated IBL, source switching, раздельные continuous/discontinuous revisions.
- Grass/Wind: projected-grid solver, compact indirect grass depth и caster presence.
- Fluid: hash/sort/SPH, NRF/filter/merged depth, finite water resolve, reset и Off/empty depth identity.
- Particles: GPU burst/indirect, реальные weighted/additive/sorted pixels, motion publication, reset/removal.

Во всех этих component runs нет ошибок D3D12 debug layer/GBV. GPU fixtures сериализованы mutex `Local\PEPEngine2FeatureGpuValidation`. Логи: `Build/SecondGroupIntegration`.

`ObjectDataJobsTests --render-bistro` прошёл 36 кадров реальной Bistro с 346 существующими props: camera/object motion, оба AO/prepass режима, переключение эффектов, camera cut и odd resize. Debug layer чист. Новых компонентов/объектов в сцену не добавлено.

После проверок удалены 13 полностью объединённых локальных веток: 11 `codex/feature-*`, `codex/review-render-techniques` и `MoreRenderFeatures`. Перед каждым `git branch -d` проверена ancestry; после удаления повторно проверена достижимость всех 13 исходных HEAD из текущей ветки. `main` сохранена с её отдельной историей. Точные имена/HEAD — в RUNS.json.

## Границы проверки

Это проверка интеграции и активных GPU путей, а не полная приёмка всех визуальных/физических критериев исходных заданий. Долговременная устойчивость, большие нагрузки, все комбинации presets и визуальное качество второй группы не измерены. Нет полного CPU all-pairs SPH/NRF oracle или проверки Fox reload через весь ECS lifecycle. Частицы используют объединённую глубину воды и отсекаются за её поверхностью; преломление частиц через воду не реализовано. Новые компоненты не установлены в Bistro, файлы сцены не изменены. Исходные авторские документы ниже сохраняют ограничения реализации; их старые заявления «не запускалось» описывают момент передачи агентом.
