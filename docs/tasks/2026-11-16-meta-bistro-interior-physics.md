# Bistro: ограждённая симуляция интерьера по качеству

Запрос пользователя 2026-11-16, `D:\Project\PEPEngine2`, ordinary Meta.
Основа подготовки `5e836132`; сверить live HEAD/status и сохранить чужие правки,
включая незавершённую воду. Начать после приёмки
[cubemap-сфер](2026-11-16-meta-bistro-probe-spheres.md).

## Категории и качество

Симулируются исходные предметы внутри кафе, ограждённого невидимой статической
коллизией от внешнего мира. Меши/материалы сохраняются; меняются physics ownership
и transforms. Число dynamic bodies определяется накопительными категориями:

| App.Physics.Quality | Динамические предметы |
| --- | --- |
| 0 Off | Нет; все исходные предметы остаются видимыми и статическими. |
| 1 Low | Только стаканы/бокалы. |
| 2 Medium | Low + тарелки. |
| 3 High | Medium + столовые приборы. |
| 4 Epic | High + стулья, столы, салфетки/подставки, бутылки, корзинки и остальные подвижные интерьерные props. |

Добавить quality CVar, если его ещё нет. Старт Meta: Low,
App.Physics.SimulationEnabled=true, CountScale=1. Предметы не размножать.
Неактивные категории остаются видимыми со статическими colliders. Архитектура,
светильники, картины/UI, персонажи и preview-сферы не входят во «всё остальное».
Терраса/экстерьер статичны. Салфетки — rigid proxies, cloth не входит в этап.

Инвентаризировать SourceNodeName/object groups и вывести фактические counts
static/dynamic/участников impulses по категориям для всех presets. Числа брать
из сцены; material submeshes одного предмета не считать разными телами.
IsBistroTableware сейчас смешивает стаканы/бутылки/тарелки/приборы/подставки,
а SelectDynamicProps включает мебель даже при малой доле tableware: это заменить
явной категорийной классификацией. Исключить ложные совпадения `table`/архитектуры.
Неопознанные группы оставить статическими с отчётом до подтверждения категории.

Качество применять через существующую restart-world семантику props, показывать
requested/effective quality и необходимость reload. Обновить BistroQualityPresets
и quality UI: новый preset задаёт категории, а не ActiveTablewareFraction.
Другие техники не меняют качество вместе с физикой. Старые knobs не должны
тайно активировать мебель на Low: задокументировать их приоритет/неприменимость
в новом режиме. Legacy App benchmark сохранить через явный opt-in нового поведения.

## Ограждение и формы

Переиспользовать BistroProps.h, ConfigureBistroProps в BistroScene.h,
BistroStaticCollision::BuildRoom, PhysicsModule/JoltPhysicsBackend,
ColliderComponent/RigidBodyComponent и PhysicsTypes.h.
BuildRoom уже описывает пол, потолок и стены в повёрнутой системе координат.
Сверить оболочку с интерьером и замкнуть выходы/дверные/оконные проёмы для props.
Сохранить сложную форму комнаты, не большой AABB с террасой. Невидимые стены
не закрывают проёмы визуально и не блокируют свободную камеру.

Dynamic colliders — аналитические shapes либо небольшие compound:
стакан cylinder/capsule; тарелка тонкий cylinder/box; прибор capsule/box;
бутылка capsule/cylinder; салфетка тонкий box; мебель — boxes/capsules по
сиденью/спинке/столешнице/ножкам. Sphere — для округлых предметов.
Строить по локальным bounds/оси группы, с положительными размерами, разумной
толщиной и правильным COM/orientation/scale в cm. Dynamic triangle mesh/дорогую
convex decomposition из старого PropShape не переносить. Shapes кешируются
при создании, не вычисляются каждый кадр. Статика использует существующие proxies.

Разделить interior mesh на архитектуру и object groups, сохранив все материалы
и submeshes ровно один раз. Dynamic предмет удаляется из прежнего static batch/
collider, без визуального и физического дубля в исходной позе. Неактивная мебель
поддерживает посуду; на Epic её static collider заменяется dynamic body.

## Время, импульсы и обновления

MetaApplication сейчас делает world.SetPaused(true). Включить штатный fixed-step
PhysicsModule для этого режима с обычной pause/time-scale семантикой, без второй
интеграции. Сохранить accumulator, catch-up limits и учёт physics debt, не
отбрасывать время ради FPS. Вода/частицы/skin/grass с explicit time paths
продвигаются один раз, без удвоения после снятия world pause.

«Летают» — существующий BistroWaves::Controller/BeforePhysicsStep: периодический
lift и scatter impulses, затем гравитация/столкновения. В waves регистрировать
только dynamic категории текущего качества. Сохранить seed, timing, speed caps
и CCD для маленьких быстрых тел. Учесть толщину оболочки/формы при этих скоростях;
не скрывать утечки тел регулярным teleport в центр. Reset/world destruction
снимает callbacks/участников до удаления/reuse entities.

Physics → ECS transforms → renderer dirty journals → shadows/probe inputs:
обновлять реально изменённые тела/instances. Sleeping bodies не перепаковывают
всю сцену. Previous transform — предыдущий отображённый кадр, не substep;
сохранить motion vectors для MB/SR. Подвижные props видны в тенях и dynamic
capture. ReflectionDynamic остаётся true во сне, static bake не содержит их
дублей. Quality/reload меняет membership/topology и нужные caches. Dirty events
раздать всем consumers, не отбирать вторым destructive ConsumeChangedElements.

## Приёмка и передача

AGENTS.md/Verification Contract: Release build и один короткий ordinary smoke
на Low, достаточный для одной существующей wave. Стаканы летают/сталкиваются
внутри, тарелки/приборы/мебель остаются на месте, экстерьер статичен. UI/облака/
сферы/остальные эффекты сохраняются, кадр без явных ошибок. Другие категории
проверить по инвентарю/коду, без runtime матрицы, новых тестов, profiling,
long soak или точностных экспериментов. Короткий smoke не доказывает долгосрочное
отсутствие утечек предметов; честно отметить предел проверки.

Отчёт: counts presets, классификация, shapes, границы комнаты, clocks,
dirty/motion/capture contracts, effective quality и реальные проверки.
Обновить README/план; локальный task-owned коммит без push, hash/executable
пользователю и остановка на оценку FPS/CPU/GPU. Не начинать FG, общий offload
или итоговое сравнение App/Meta автоматически.
