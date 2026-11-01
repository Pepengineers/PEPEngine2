# Локальные cubemap-пробы: dynamic и baked static + dynamic

Статус: запуск отдельного чата Sol / high разрешён.

Сначала прочитай ../COMMON.md (относительный путь из first: `../COMMON.md`).

Добавь настраиваемый reflection-probe component и полноценную интеграцию рендера без новых probes/entities в Bistro. Два режима: полностью dynamic cubemap всей opaque/masked геометрии; запечённые **цвет и matching depth** статики с восстановлением обоих и дорисовкой только динамических объектов. Донор: MGPU-Reflection/ReflectionRenderer.cpp и ReflectionRenderer.h; box projection в MGPU-SSR/Shaders/Reflections.hlsl. Проверяй actual donor source, не переносить его framework/multi-GPU.

Нужны PBR/HDR capture, roughness GGX prefilter с качеством/samples, собственная корректная политика теней (main-camera CSM не универсален для шести faces), box projection/influence и сочетание с глобальным IBL без double energy. Обновление static geometry/material/light/sky и movement классификации должны корректно инвалидировать cache. Публикуй полностью готовые шесть faces и roughness mips согласованно; при бюджете нескольких faces нельзя показывать незавершённое поколение или сбрасывать cursor каждым динамическим изменением. Не усложняй ресурсную модель сверх требуемого atomic publication.

CVar: Render.Reflection.Enabled, Quality Off/Low/Mid/High, Implementation (0 Dynamic /1 BakedStaticDynamicOverlay), capture resolution, faces/update budget, GGX prefilter samples и необходимые параметры обновления. Настройки individual probe и global preset/override не перетирают author values. Dynamic classification существующих объектов разрешена, новые объекты запрещены.

Интегрируй в opaque и transparent PBR. С текущим global IBL корректно смешивай покрытие probes; SSR-агент работает отдельно, поэтому опиши точный seam для SSSR miss fallback, не создавая второй specular resolve. Укажи limitations transparent capture и multi-layer glass честно.

Проверки: static depth реально скрывает dynamic объект, dynamic движение реально меняет radiance, cached/static режим не перерисовывает статику без причины, actual материал edit, dirty/cut/world replacement, partial publication, quality/resolution/mode runtime switches, parallax/overlap/roughness, shadows с off-camera casters, odd/frame resources, GPU states и timings. Компоненты не должны автоматически создавать содержимое сцены.
