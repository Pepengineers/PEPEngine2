# Скелетная анимация и готовый asset

Статус: ПОДГОТОВЛЕНО, НЕ ЗАПУСКАТЬ до отдельного указания пользователя.

Общий контракт: ../COMMON.md.

Ассimp import полного hierarchy/clips/skin data, CPU pose evaluation и GPU compute LBS с общими current/previous vertices для main/shadow/probe/motion. Не лимитировать 96 bones или 4 influences из донора M3D. Использовать существующие SceneAsset/SceneHandle/MeshRegistry и локальную geometry subasset, не добавлять параллельные registries/fake handles. Inverse bind per-mesh; все hierarchy nodes, отсутствующие tracks bind pose, exact unanimated bind matrices, zero weights rigid attachment, корректные normals/nonuniform scale и conservative bounds. Материалы и многомешевые attachments реальны, без новых childentities. Проверить unload/reload lifetime actual registry (Unload не всегда меняет generation).

Готовая модель Khronos Fox, pin 4995a638c96dbc94376d3d6164e70fee3ac7f7a2: https://github.com/KhronosGroup/glTF-Sample-Assets/tree/4995a638c96dbc94376d3d6164e70fee3ac7f7a2/Models/Fox . Model CC0; rig/animations/conversion CC-BY4 — сохранить LICENSE, metadata и attribution. Survey/Walk/Run,24 joints. Проверить units/node transforms фактически; не домножать слепо на100. Модель для isolated tests, не в Bistro. CVars enable/freeze, Quality с реальным update budget, playback speed/clip/time/pose controls через code/INI. Skinning Off замораживает согласованную main/shadow/motion pose, предыдущая поза не размывает неподвижное состояние. CPU bind/interpolation и actual Fox/GPU oracle, >4influences/>96joints/multiple meshes/zero weights/reuse/reload и timings.
