# Session Context: TouchRTTY Optimization (2026-04-01 - End of Session)

## Текущий статус
- **Branch:** `feature/rtty-dsp-improvements`
- **Build:** 187
- **Стабильность:** Подтверждена для Build 185 (текущий рабочий код).
- **Roadmap:** Добавлены цели по DRM (Фаза 8), Eye Diagram и глубокой оптимизации Cortex-M33.

## Обновленные Золотые Правила (Engineering Standards)
1. **Surgical Edits Only:** Избегать полной перезаписи `main.cpp`.
2. **Strict Float Policy (Rule #6):** Тотальный аудит `f` суффиксов. Запрещено использовать константы типа `3.14` (double), только `3.14f`. Использовать `sinf`, `cosf`, `fabsf`.
3. **RAM Execution:** Помечать DSP-функции `__time_critical_func`.
4. **Modulo Removal:** Исключить `%` из горячих циклов.
5. **Wait For Event (Rule #7):** Переход на ADC DMA + `__wfe()` для разгрузки шины памяти.
6. **Memory Barriers (Rule #9):** Использование `__dmb()` при передаче данных между ядрами (например, FFT буфер).
7. **Vectorized DSP:** Приоритет библиотекам CMSIS-DSP (`arm_fir_f32` и др.).

## План на следующую сессию (Build 188+)
1. **Интерфейс:** Внедрить циклический перебор 4-х шрифтов (Font2, Mono, Font0, Font4).
2. **Engineering Clean-up:**
   - Аудит `f` суффиксов в `main.cpp`.
   - Оптимизация цикла FIR (удаление `%`).
   - Внедрение `__dmb()` для FFT данных.
3. **Hardware Research:** Подготовка к внедрению HW Scroll и ADC DMA.

## Следующий шаг
Начать Build 188: Шрифты + первичная очистка математики по новым правилам.
