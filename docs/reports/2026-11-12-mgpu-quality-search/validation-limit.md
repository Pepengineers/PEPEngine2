# Изолированный отказ ExecuteIndirect под GBV

MisSpec, RTX 5050 `32.0.16.2114`, Arc B570 `32.0.101.8864`, Agility SDK 619.
`IndirectDrawProbe.cpp` использует native D3D12 без движка, графа, replica и межадаптерного переноса.
Ожидаемый результат — один пиксель R32_FLOAT со значением 0,75. Shader SM5.1 компилируется FXC.

| GPU | Draw | Arguments heap | Root words | Validation | Pixel | Exit |
|---|---|---|---:|---|---:|---:|
| Arc B570 | Direct | Default | 20 | GBV | 0,75 | 0 |
| Arc B570 | Indirect draw-only | Default | 20 | GBV | 0,75 | 0 |
| Arc B570 | Indirect constant + draw | Default | 20 | GBV | 0 | 1 |
| Arc B570 | Indirect constant + draw | Upload | 20 | GBV | 0 | 1 |
| Arc B570 | Indirect constant + draw | Default | 1 | GBV | 0 | 1 |
| Arc B570 | Indirect constant + draw | Upload | 1 | GBV | 0 | 1 |
| Arc B570 | Indirect constant + draw | Default | 20 | Debug layer | 0,75 | 0 |
| RTX 5050 | Indirect constant + draw | Default | 20 | GBV | 0,75 | 0 |

Это воспроизведение ограничения текущего runtime/driver/GBV сочетания. Какой именно внешний компонент ошибается, не установлено. Отказ не доказывает ошибку MGPU-переноса и не заменяет проверку production-пути с обычным debug layer. Приложение использует DXIL; успешные случаи минимального SM5.1 fixture не являются его полной проверкой.

Сборка из корня репозитория:

```powershell
MSBuild docs/reports/2026-11-12-mgpu-quality-search/IndirectDrawProbe.vcxproj /p:Configuration=Release /p:Platform=x64
```

Перед запуском скопировать каталог `D3D12` текущего приложения рядом с exe. CLI:

```text
IndirectDrawProbe.exe intel|nvidia direct|draw|constant upload|default 1|20 debug|gbv
```

В основном reflection fixture DRED зарегистрировал page fault `0xbf800000` на ExecuteIndirect первого secondary capture при GBV. Без GBV после исправления отсутствующей descriptor heap в primary filter прошли 1–4 probes, обе реализации, четыре расписания и пять масок размещения с покадровой проверкой requested/effective и сравнением radiance с primary при одинаковом качестве.

Shadow raster уже имел CPU loop с полностью подготовленными аргументами каждого draw. Он использует эквивалентный прямой DrawIndexedInstanced на выбранном адаптере; это не перенос работы на primary. GPU-generated indirect пути отражений не заменялись обходом ради прохождения GBV.

## Отдельный reproducer оптимизации CPU

`ShadowRegionResetMiscompile.cpp` воспроизводит игнорирование aggregate reset после копирования структуры в MSVC 14.52 `/O2`. `/Od` проходит; scalar invalidation ширины в основном fixture также проходит. Воспроизведение не требует GPU. Проект принимает `/p:ShadowReproOptimization=MaxSpeed` либо `Disabled`; production shadow layout ради этого не изменялся.
