# Правила ревью кода

## Как читать эту памятку

Раздел **Действующие контракты** описывает то, что подтверждено текущими исходниками и тестами. **Рекомендации ревью** — проверочные вопросы для изменений, не дополнительный runtime-контракт. Пути даны относительно корня репозитория. Основные опорные файлы: [`src/overlays/OverlayDelta.h`](../src/overlays/OverlayDelta.h), [`src/overlays/delta_logic.h`](../src/overlays/delta_logic.h), [`tests/test_delta_logic.cpp`](../tests/test_delta_logic.cpp), [`src/iracing/iracing.h`](../src/iracing/iracing.h).

## Действующие контракты Delta

### Dynamic scale: floor и лестница

- `OverlayDelta::onConfigChanged()` трактует `delta_range_sec` как минимум масштаба, ограниченный снизу `0.05`, а не неизменный полный диапазон. `range_steps` фильтруется до положительных чисел и сортируется; `delta::buildLadder()` гарантирует не пустую возрастающую лестницу с `front() == minRange`.
- `buildLadder()` отбрасывает ступени `<= floor`; ближайшая большая сохраняется рядом с floor при отношении `stepHigh/floor >= 1.2`, иначе только `stepHigh` сливается во floor, последующие ступени строго выше неё остаются. Тесты: `buildLadder: floor drops rungs at or below it`, `...keeps both when the gap...`, `...merges the smallest rung...` в [`tests/test_delta_logic.cpp`](../tests/test_delta_logic.cpp).
- `delta::pickTargetRung()` считает `needed = max(lapMaxAbs, ghostMaxAbs) * (1 + headroom)`, выбирает первую ступень `>= needed`; если `needed` выше последней ступени, результатом остаётся `ladder.back()` (saturation), а не `ladder.front()`. Это важный review guard для регрессии: при лестнице `{1, 1.5, 2, 3, 5, 10}` и амплитуде `50` (или амплитуде `10` с headroom 8%) ожидается `10`, проверено тестом `pickTargetRung: above the top rung clips to the top`. Замена начального значения `target = ladder.back()` на `ladder.front()` сломает именно over-top случай; добавить/сохранить регрессионное утверждение вместе с любым изменением выбора ступени.
- Масштаб кэшируется в конфигурации, целевой rung выбирается каждый update; `easeRange()` быстро увеличивает диапазон и медленнее уменьшает. 30 Hz compensation предназначена для обновления оверлея через кадр; тесты фиксируют коэффициенты и направление.

### Delta trace, цвет, геометрия

- Положительное delta означает потерю времени и отображается выше нулевой линии в loss-цвете; ноль относится к gain-отрезку (`value > 0` — loss predicate). См. комментарий `signRuns()` и тесты `signRuns` в [`tests/test_delta_logic.cpp`](../tests/test_delta_logic.cpp).
- Точки хранятся по дистанционным bucket-ам, forward gap заполняется линейной интерполяцией; данные до первой валидной точки не записывают диапазон. Разрешение ограничено 16…5000. Смотреть `OverlayDelta::onUpdate()` и config initialization.
- Пересечение видимого ±range обрезает линию точно по границе (`delta::edgeCrossT`); линия исчезает за рамкой, а заполненная область остаётся прижатой к краю через ограничение `yOf()`. `edgeCrossT` вызывается для пересекающего сегмента. Изменение клиппинга должно сохранить линии без искусственного плоского хвоста на краях.

### Reference, invalid lap и ghost

- Целевые delta-каналы — `ir_LapDeltaToBestLap`/`_OK` или `ir_LapDeltaToSessionBestLap`/`_OK`, в зависимости от `m_useBest` (`OverlayDelta::onUpdate()`). Не показывать невалидное delta как валидный sample: обновление буфера и числовой readout зависят от `deltaOk`.
- Header session-best использует `ir_CarIdxBestLapTime` для действительного `driverCarIdx` при времени `> 0`, удерживая последнее положительное значение при временном `-1`. Header для `best` использует `ir_LapBestLapTime`. Не заменять эти значения бездумно на `ir_LapLastLapTime`: raw last lap включает invalid laps, а sim best — validity-filtered.
- На rollover `captureGhostCandidate()` вызывается до `resetLap()`, проверяет не менее 90% покрытия и сверяет `ir_LapLastLapTime` с выбранным validity-filtered best в пределах 0.001 s. Проверка `delta::bestImproved()` требует строгое улучшение сверх epsilon; совпавший best, но не улучшивший baseline, не считается новым best. Не трактовать быстрый invalid lap либо внешний best как круг, ставший best у игрока.
- `ghost=last` сохраняет последний пригодный почти полный круг. `ghost=best` и `ghost=session_best` в текущей реализации эквивалентны: сохраняют только реально проехавший круг, совпавший с выбранным target-best и улучшивший прежний ghost-best. На настоящем новом target-best best ghost схлопывается к центру; pulse запускается только при наличии prior best и включённом `new_best_pulse`. Новый session очищает соответствующие snapshots/анимацию.
- Сектор считается пройденным для section bar лишь когда диапазон sampled bucket-ов покрывает его start и end; непройденная/частично покрытая секция — undriven, не delta относительно искусственного нуля.

## Рекомендации ревью

Это рекомендации, а не новые требования поведения:

1. При правках выбора шкалы проверьте три случая: ниже floor → `front()`, между rung-ами → следующий rung вверх, выше top с любым headroom → `back()`. Сохраните адресный regression test `pickTargetRung: above the top rung clips to the top`.
2. При изменениях telemetry проследите парность каналов значения и `_OK`, источник выбранного target, валидность best-времени и порядок rollover → capture → reset. Явно проверяйте, что invalid laps не двигают target, ghost-best или pulse.
3. При правках ghost проверьте полноту круга, `last` отдельно от `best`, session/config reset и collapse-вклад в autoscale. `best`/`session_best` одинаковы в реализации, хотя пользовательские названия различаются.
4. При переносе вычислений оцените стоимость на каждом кадре: `m_resolution` ограничен, но geometry/sink создаются в `drawClippedLine()` и на каждый sign-run fill; ladder parse/sort и DirectWrite/StrokeStyle ресурсы намеренно создаются на config-change, snapshot peak проход — при capture. Не выдавайте рекомендацию о кешировании за доказанное улучшение без измерения.
5. Если меняется семантика пользовательского параметра Delta или графического поведения, синхронизируйте [`README.md`](../README.md), который документирует target/ghost, floor/steps/headroom и управляемые функции оверлея.

## Тестовая опора

`tests.vcxproj` включает `tests/test_delta_logic.cpp`; тот тестирует чистую математическую логику без оконного DirectX-renderer. Он подтверждает перечисленные контракты функций, но сам по себе не проверяет end-to-end чтение iRacing telemetry, рендеринг Direct2D или поведение live invalid lap. Не делать более широких заявлений о покрытии, чем фактически есть в тестах.
