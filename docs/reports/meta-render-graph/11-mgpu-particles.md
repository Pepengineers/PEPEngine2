# U11 — общая передача частиц через Meta executor

Статус: реализация и проверка исходников; сборка и runtime **не запускались** по указанию пользователя. База ветки: aabb2744. Перенос Bistro не входит в этот этап; обычный host остаётся процедурной сценой.

Meta.Particles.Enabled=true включает secondary для всех SharedSimulation=true && DepthCollision=false эмиттеров. DepthCollision=true остаётся на primary. Meta.Particles.SecondaryAdapter выбирает вторую карту; при включённой Meta.Water значение -1 использует её адаптер. Явно разные адаптеры воды и частиц отклоняются. Meta.Particles.Selection: 0 — StrictPublication, 1 — LatestCompleted. Default выключен; production Render.Particles/Fluid secondary остаются -1.

SimulationDomain владеет одним secondary device и одним QueueSubmission для воды и частиц. ParticleTransfer записывает production particle kernels через Meta Executor, сохраняет solver pools между шагами и имеет собственный producer loop, независимый от Present и публикаций воды. Legacy GDX12ParticleOffload worker не запускается. Пакет всех eligible emitters содержит header, IDs/revisions, capacities/counts/offsets и render payload. Каждая публикация имеет одну contiguous COPY export и одну COPY import; CPU readback count отсутствует.

Interaction field экспортируется отдельным общим snapshot с GPU token и ограниченными input slots. Secondary COPY получает реальные клетки, solver читает локальный snapshot. Packed/shared/received slots защищены выданными completion tokens; LatestCompleted сохраняет готовый Display, StrictPublication ждёт конкретный уже отправленный import. RepeatDisplay/seed не касаются pending received buffer. Imported и local particles проходят общий cull, SortedAlpha sort и WBOIT resolve. История использует production identity-aware display shader.

Изменение состава/размещения дожидается старых потребителей. Неизменённые secondary pools сохраняются; moved pool сбрасывается с сохранением consumed burst identity. Layout change пересоздаёт display ranges и сбрасывает motion history. Удалённые/сброшенные emitter publications не переиспользуются в новом epoch.

Обычная сцена теперь содержит видимую воду и четыре непрерывных emitter группы рядом с камерой: local DIRECT, local COMPUTE при async, eligible secondary и marked DepthCollision fallback. Это обычные scene entities; новый test harness не создавался. Existing CPU target exclusions обновлены только для нового translation unit.

Проверка исходников: подключения caller/project, shared device ownership, один export/import, отдельный input snapshot, общий render path и completion/lifetime boundaries просмотрены. git diff --check выполнен. Новые tests, precision comparisons, матрицы, captures и профилирование не добавлялись и не запускались.
