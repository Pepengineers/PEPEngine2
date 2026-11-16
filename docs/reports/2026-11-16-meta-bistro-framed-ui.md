# UI в оригинальных картинах Bistro — 2026-11-16

Основа `766b492c`. По уточнению пользователя удалено свободное размещение UI
и облаков у входа. Посторонние BistroAdditionsTests.cpp, Temp/ и task animation/UI
не затронуты. Название второго canvas — ASTRA латиницей, по последнему сообщению.

## Поверхности и оформление

Исходные SubMesh.SourceNodeName и material Paris_Paintings определяют только
три полотна внутри Interior_interior. Металлические рамки Paris_Painting_Metal
и вся остальная геометрия сохранены в прежнем mesh, с фильтром SubMeshIndices.
Копируются только два треугольника выбранного полотна, без новых координат
размещения; исходные позиции/нормали/tangents сохранены, UV атласа нормализованы.
Двойной слой старого изображения не остаётся. Assets/OBJ/textures не изменены.

| Исходная картина | Центр, см | Содержимое |
| --- | --- | --- |
| Paris_Painting_B | (1210.20,235.42,-150.63) | NEON DISTRICT |
| Paris_Painting_F | (1089.03,236.98,92.21) | ASTRA |
| Paris_Painting_C | (977.93,236.94,295.99) | Облака через image-only UI |

Референс неона: [Cyberpunk Bar Sign, Natalie Riccardi](https://sketchfab.com/3d-models/cyberpunk-bar-sign-5921cab512484fa780c34f418ddaa2d8).
Использована палитра cyan/magenta и тёмная подложка; чужая модель/текстуры
не импортируются. UI создаёт движущуюся надпись, бегущую строку, пульсирующую
двойную окантовку, equalizer и animated grid/scanlines. ASTRA — многослойные
наклонённые буквы, цветные обводки, spray и движущиеся потёки.

UI raster сначала пишет полный canvas с текстом в Raw RT, затем separable
Gaussian с нормализованными exp-весами размывает весь raster. Neon composite
смешивает raw и glow; ASTRA смешивает sharp/blur в полосе шириной 50% canvas.
Центр полосы движется слева направо и обратно по cos(Time*0.75), период около
8.38 s; видимая линия/ручки обозначают slider. Low radius=8, Medium=17.
Это настоящий post-raster Gaussian, не прежний blur фона до добавления текста.
Low logical/raster UI: 768×512 для двух вывесок, 1024×512 для облачного image UI.
Medium сохраняет существующий raster scale 4/3. UI targets остаются RGBA8 UNORM.

## Время, облака и lifetime

SceneCloudPicture больше не создаёт mesh/material и не владеет размещением.
Он предоставляет texture отдельного MGPU-Noise producer; третий WorldCanvas
строит image draw command, OffscreenRenderer читает эту texture и пишет canvas
внутри исходной рамки. Local/cross-adapter transport, независимые grid/adapter
и donor noise defaults сохранены. Порядок: cloud producer/import → image UI
raster → GBuffer material → обычный scene post → screen UI.

Один render-tick accumulator увеличивается на неотрицательный DeltaTime один
раз в PrepareMaterialBindings. Его снимок получают обе вывески и cloud producer;
пауза физического мира не влияет на этот clock. CloudPicture::Prepare по-прежнему
записывает dispatch каждый enabled frame. Для диагностики времени добавлены
ограниченные log записи sequence=1 и 61. Визуальное подтверждение движения облаков
после этой правки не получено — см. ограничения ниже; причина сообщения о
статическом изображении не выдаётся за установленный runtime факт.

Raw/final targets, Gaussian scratch/descriptors и recording owners persistent
по frame slot. Чтения исходной cloud texture принадлежат image UI, а GBuffer
читает финальный UI target. Внешние состояния RT/UAV/SRV объявлены Meta packet;
внутренние scratch возвращаются в UAV, borrowed state tracker восстанавливает
границы. При disable скрывается только cloud canvas; его material binding
сбрасывается и публикуется заново при enable. Независимые renderer owners сохраняют
font atlas каждого canvas. Shared App caller без cloud input сохраняет исходную
третью картину. Начальная Meta camera: (600,230,-80), rotation=(0,70,0), к стене
с тремя картинами. Оптические настройки DoF и остальные техники не менялись.

## Проверка

Release C++ build успешен: Build/meta-bistro-framed-ui-build.log.
Первый обычный запуск подтвердил обнаружение B/F/C, затем остановился на
runtime HLSL compilation: имя line конфликтовало с keyword HLSL. Имя исправлено
на gridLine. Все пять используемых entry (GenerateGradient, BlurHorizontal,
FinishCanvas, GaussianVertical, ComposePost) после исправления успешно
скомпилированы SDK DXC в cs_6_8. Это компиляция shader, не GPU/runtime приёмка.
Артефакты и first-launch.log — Build/Verification/meta-bistro-framed-ui/.

Повторный запуск для осмотра остановлен физической Escape пользователя через
Computer Use. Дальнейшие UI-действия/альтернативный запуск не выполнялись.
Представленный кадр нового UI, итоговое оформление, Gaussian slider, покадровое
движение облаков и отсутствие runtime state ошибок ПОСЛЕ shader fix не проверены.
Не считать этот этап визуально принятым. Новые тесты, матрицы quality/adapter,
профилирование и длительные прогоны не выполнялись по AGENTS. Проверка исходников
и git diff --check выполнены; performance claims отсутствуют.
