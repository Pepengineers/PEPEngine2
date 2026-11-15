# Проверка SSSR перед переносом травы и ветра

Проверен `5c29050c` относительно `b9b1cf1a`, связанный код SSSR/PBR/GBuffer,
camera history и сохранённые артефакты. Блокирующих замечаний в текущем обычном
Bistro-пути не найдено. Реализация следующей техники в этой проверке не начата.

- Prepare → SDK SSSR/denoiser → specular resolve стоят между opaque lighting
  и существующей mesh WBOIT. Прозрачность получает результат SSR, а не старый HDR.
- Static-IBL variant исключает probe include/sample, bindings t9/t10 и соответствующие
  accesses. SceneReflections и полный scene snapshot не включаются.
- Fallback использует существующий prefiltered environment; resolve сохраняет
  формулу замены specular IBL через radiance − fallback и один split-sum BRDF.
  Стекло остаётся вне opaque SSR и использует forward static IBL.
- OpaquePass записывает prevNDC − currentNDC, GPUDataUpdateSystem сохраняет
  предыдущую unjittered camera matrix. Статическая геометрия получает исходную
  PreviousPosition при загрузке. Пауза мира не требует обнуления camera motion.
- HistoryRevision изменяется при history/camera reset, а не обычном движении.
  Адреса вращающихся GBuffer slots не входят в reset condition Meta SSR.
  Один context обычной последовательности сохраняется между кадрами.
- Disabled-путь возвращается до allocation/dispatch/copy-through и отмечает
  reset при повторном включении. Resize, settings, environment и camera history
  сохраняют сбросы. DestroyContext дожидается DIRECT перед уничтожением SDK;
  ordinary-frame Flush не добавлен. Ограничение числа slots проверяется при
  создании SceneLighting; текущая конфигурация использует три слота.
- NativeWork по умолчанию пишет на DIRECT. Сохранены OwnerRecording первого
  SDK dispatch, зависимости ресурсов и GENERIC_READ для UPLOAD camera constants.
  Возврат после ordinary continuation исключает диагностический readback-хвост.

Просмотрены build-console.log, ordinary-run.log и operation dumps в
`Build/Verification/meta-bistro-ssr/`. Сборка выдала целевой exe, сохранив известные
LNK4099. В текущем run log нет ERROR/FATAL, есть штатное завершение. Dump содержит
prepare, SSSR + denoiser и composition перед двумя WBOIT-проходами и display.
`git diff --check b9b1cf1a 5c29050c` прошёл.

Отчёт исполнителя `docs/reports/2026-11-15-meta-bistro-ssr.md` фиксирует обычный
запуск, движение камеры и подтверждение пользователем визуальной проверки
«все окей». Здесь этот результат принят как уже полученный smoke по AGENTS.md;
новая сборка, запуск, тесты и профилирование не выполнялись. Disabled/reset/resize
проверены только по исходникам. Стоимость SSR отдельно не измерялась.

Следующий этап по согласованной очереди — трава и ветер:
`docs/tasks/2026-11-15-meta-bistro-grass-wind.md`. SSR остаётся частью текущей
основы; отметка о его откладывании в предыдущем плане более не описывает состояние.
