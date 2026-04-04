# Roadmap: Оптимизация и Улучшение (Phase 3+)

Этот документ фиксирует стратегические цели по оптимизации производительности и улучшению пользовательского интерфейса проекта TouchRTTY на базе RP2350.

## 1. Шрифтовая система (Roadmap Item #1)

### Этап 1: 4 режима шрифтов — DONE (Build 195)
- [x] Отказ от дробного масштабирования. Все шрифты нативные (1:1 пиксель).
- [x] BIG: Spleen 8×16 (импортирован из BDF, 9 строк, 55 символов)
- [x] MED: Bitocra 7×13 (импортирован из BDF, 11 строк, 62 символа)
- [x] SMALL: Font0 6×8 (встроенный LovyanGFX, 15 строк, 73 символа)
- [x] TINY: TomThumb 3×5 (встроенный LovyanGFX, 22 строки, 120 символов)
- [x] Конвертер `tools/bdf2gfx.py` (BDF → Adafruit GFX header)
- [x] Автоматический line_width при переключении шрифта
- [x] Сохранение выбора шрифта в flash
- [x] Упрощение DIAG экрана: убраны DUMP, W-/W+ (дублировали Tuning Lab), оставлены FONT + RST

### Этап 2: Font Lab (подменю настройки шрифтов) — TODO
- [ ] Подменю Font Lab (аналог Tuning Lab) с визуальным предпросмотром
- [ ] Выбор шрифта из списка для каждого слота (BIG/MED/SMALL/TINY)
- [ ] Масштабирование X/Y (дробное, для тонкой подстройки)
- [ ] Настройка межстрочного интервала (line_h)
- [ ] Настройка горизонтальных и вертикальных отступов
- [ ] Настройка количества символов в строке (line_width)
- [ ] Тестовая строка для предпросмотра: `ABCDE 12345 THE QUICK BROWN FOX`
- [ ] Сохранение пресетов в flash

### Этап 3: Скины и цветовые схемы — TODO
- [ ] Цветовые схемы: Green-on-Black (default), Amber, White, Blue, Red
- [ ] Полный пресет: шрифты + цвета + яркость
- [ ] Импорт скинов с SD-карты (JSON/INI формат)

### Кандидаты на импорт шрифтов (оценены визуально):
| Шрифт | Размер | Стиль | Ссылка |
|-------|--------|-------|--------|
| Terminus | 8×14, 8×16 | Терминальная классика | terminus-font.sourceforge.net |
| Proggy Clean | 7×11 | Программистский | github.com/bluescan/proggyfonts |
| Dina | 8×10..8×16 | Чистый, компактный | dcmembers.com/jibsen |
| Spleen | 5×8..32×64 | Современный bitmap | github.com/fcambus/spleen |
| Bitocra | 7×13 | Мелкий, чёткий | github.com/ninjaaron/bitocra |
| Creep | 4×6 | Ультрамелкий | github.com/romeovs/creep |
| Scientifica | 5×11 | Научный стиль | github.com/NerdyPepper/scientifica |
| Unscii | 8×8, 8×16 | Retro/pixel-art | viznut.fi/unscii |
| Robey 4×6 | 4×6 | Экстра-компактный | robey.lag.net |

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

## 7. Serial Command Interface (Build 194+)
- [x] 16 команд дистанционного управления: ALPHA, BW, SQ, FREQ, BAUD, SHIFT, STOP, INV, AFC, AGC, SCALE, DIAG, STATUS, SAVE, CLEAR, HELP.
- [x] Компактный диагностический поток `[D]` каждые ~500мс.
- [x] Python autotune.py: автоподстройка ALPHA/BW/SQ через serial (Phase 1 + Phase 2 fine-tuning).

## 7a. Встроенный автотюнинг на приборе — TODO
Автоподстройка DSP параметров без ПК, прямо на экране устройства.
- [ ] Кнопка **AUTO** в Tuning Lab запускает встроенный автотюнинг
- [ ] Алгоритм: hill-climb sweep ALPHA → BW → SQ (аналог autotune.py)
- [ ] Метрика качества (Score): `-5×ERR + SNR - 1000×|DPLL_FE| + SQ_bonus`
- [ ] Диапазон Score: -999 (нет данных) .. 70+ (идеально, ERR~0%)
- [ ] Прогресс на экране: текущий параметр, значение, прогресс-бар, лучший Score
- [ ] Замер ~3 сек на точку (настраиваемо)
- [ ] Все данные уже доступны через shared-переменные (ERR, SNR, DPLL_FE, SQ) — serial не нужен
- [ ] По завершении: показать результат (было→стало), автосохранение в flash
- [ ] Возможность прервать тачем (кнопка STOP)
- [ ] Опционально: fine-tuning (Phase 2) с половинным шагом вокруг лучших значений

## 8. Мультиплатформенность дисплеев — TODO
- [ ] **ILI9341 320×240:** Вариант прошивки для меньшего экрана
- [ ] Условная компиляция (`#ifdef DISPLAY_320x240`) или автоопределение по ID чипа
- [ ] Адаптация UI зон: top bar, DSP zone, text zone, bottom bar — пропорционально
- [ ] TINY шрифт как основной для 320×240 (максимум информации на маленьком экране)
- [ ] Адаптация touch-зон и размеров кнопок меню

## 9. SDR и DRM (Phase 9 - Future)
- [ ] **Dual-Channel ADC (I/Q Input):** Для Belka-DX 40 kHz.
- [ ] **Complex FFT & Panorama:** Широкополосный водопад.
- [ ] **I2S DAC Audio Output:** PCM5102 для вывода звука.
- [ ] **AAC Audio Decoding:** HE-AAC v2 / xHE-AAC (FDK AAC).

---
*Статус: В разработке (Ветка feat/alex-cl-dev, Build 194)*
