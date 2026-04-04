# Roadmap: Оптимизация и Улучшение (Phase 3+)

Этот документ фиксирует стратегические цели по оптимизации производительности и улучшению пользовательского интерфейса проекта TouchRTTY на базе RP2350.

## 1. Идеальные Шрифты (Roadmap Item #1)
**Статус: DONE (Build 188)**
- [x] Отказ от дробного масштабирования. Использование нативных шрифтов (1:1).
- [x] Режим NORM (Font2, 8x16, line_h=17px) и NARW (Font0, 6x8, line_h=10px).

## 2. Аппаратное Ускорение Рендеринга (Roadmap Item #2)
- [ ] **Hardware Scroll (ILI9488):** Использование регистров `VSCRSADD` (37h) для водопада.
- [ ] **SIO INTERP Colormap:** Мгновенное преобразование `float -> RGB565`.
- [x] **Ping-Pong DMA Buffers:** Двойная буферизация для SPI (Build 190).

## 3. Оптимизация под архитектуру RP2350 (Cortex-M33)
- [x] **Strict Float Policy:** Тотальный аудит. Все double→float, sin→sinf (Build 189).
- [x] **Hardware ADC FIFO:** `adc_fifo_setup()` + `adc_run(true)` для 10kHz без джиттера (Build 190).
  - **Важно:** `__wfe()` нельзя использовать без ADC IRQ — приводит к потере сэмплов. Использовать `tight_loop_contents()`.
- [ ] **Memory Barriers:** `__dmb()` для гарантированной целостности при межъядерном обмене.
- [ ] **Vectorized DSP (CMSIS-DSP):** `arm_fir_f32` и `arm_biquad_f32` для замены скалярных циклов.

## 4. Оптимизация UI и Интерфейса (Roadmap Item #3)
- [ ] **Selective Redraw:** Перерисовка только изменившихся элементов.
- [ ] **Widget Framework:** Рефакторинг UI с переходом на структуры объектов.
- [ ] **Вертикальный скролл текста:** Оптимизация прокрутки с LovyanGFX спрайтами.
- [x] **Глазковая диаграмма (Eye Diagram):** Phosphor persistence, DPLL-синхронизированная X-ось, 240x64 в Tuning Lab (Build 194).
- [x] **Error Rate Indicator:** 100-символьное скользящее окно, бар ERR в верхней панели (Build 191).
- [x] **3 тонких бара:** SIG, AGC, ERR в верхней панели (Build 191).

## 5. Глубокая оптимизация DSP
- [ ] **CMSIS-DSP:** Переход на `arm_fir_f32` и `arm_biquad_cascade_df2T_f32`.
- [ ] **NCO Interp:** Аппаратная генерация синусоид на `interp0`.
- [x] **fast_log2f():** IEEE 754 bit-trick, ~4x быстрее log10f (Build 190).
- [x] **AGC precompute:** 1/release (умножение вместо деления) (Build 190).

## 6. CMake & Compiler Flags
- [x] `-O3`, `-ffast-math`, `-funroll-loops` (Build 189).
- [x] `-mfloat-abi=hard`, `-mfpu=fpv5-sp-d16` (Build 189).
- **Примечание:** `-flto` несовместим с Pico SDK `__wrap_` символами. Не использовать.

## 7. Serial Command Interface (Build 194)
- [x] 15 команд полного дистанционного управления (ALPHA, BW, SQ, FREQ, BAUD, SHIFT, STOP, INV, AFC, AGC, DIAG, STATUS, SAVE, CLEAR, HELP).
- [x] Компактный диагностический поток `[D]` каждые ~500мс.

## 8. SDR и DRM (Phase 8 - Future)
- [ ] **Dual-Channel ADC (I/Q Input):** Для Belka-DX 40 kHz.
- [ ] **Complex FFT & Panorama:** Широкополосный водопад.
- [ ] **I2S DAC Audio Output:** PCM5102 для вывода звука.
- [ ] **AAC Audio Decoding:** HE-AAC v2 / xHE-AAC (FDK AAC).

---
*Статус: В разработке (Ветка feat/alex-cl-dev, Build 194)*
