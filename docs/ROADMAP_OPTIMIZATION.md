# Roadmap: Оптимизация и Улучшение (Phase 3+)

*Обновлено: 2026-04-12, Build 218*

## 0. Рефакторинг кода (Build 189-206)

### Разделение main.cpp на модули

До Build 189 весь код (DSP, UI, serial, touch, state machines) находился в одном файле `main.cpp` (~1843 строки). В процессе развития проект был разбит на модули:

| Файл | Строк | Назначение |
|------|-------|-----------|
| `main.cpp` | 55 | Точка входа, инициализация HW, запуск Core 1 |
| `dsp_pipeline.cpp` | 703 | Core 0: ADC→AGC→I/Q→LPF→ATC→DPLL→Baudot→BAUD-DET→STOP-DET→auto-INV→auto-recovery |
| `ui_loop.cpp` | 915 | Core 1: FFT, SEARCH, спектр/водопад, touch, serial parser |
| `serial_commands.cpp` | 156 | Обработка 18+ serial-команд (BAUD, SHIFT, STOP, SEARCH и др.) |
| `settings_flash.cpp` | 103 | Чтение/запись AppSettings во Flash (2MB offset) |
| `app_state.hpp/.cpp` | 141+95 | Все shared volatile переменные и константы |
| `ui/UIManager.hpp` | 718 | Отрисовка: спектр, водопад, текстовая зона, top/bottom bar, попапы |
| **Итого** | **2745** | |

### Ключевые принципы рефакторинга

1. **Разделение по ядрам:** `dsp_pipeline.cpp` исполняется строго на Core 0, `ui_loop.cpp` — на Core 1. Это гарантирует отсутствие взаимных блокировок.

2. **Shared state как единая точка:** Все межъядерные переменные собраны в `app_state.hpp/cpp`. Volatile semantics, без mutex.

3. **State machines изолированы:** BAUD-DET, STOP-DET, auto-INV, auto-recovery — каждый со своими фазами и local state, все внутри `dsp_pipeline.cpp`.

4. **UI отделён от логики:** `UIManager.hpp` — чистая отрисовка, принимает параметры через аргументы, не читает shared state напрямую (кроме отрисовки баров).

## 0a. Оптимизация производительности (Build 189-194)

### Снижение загрузки Core 0 (DSP)

**До оптимизации (Build 188):** Core 0 = ~30%, Core 1 = ~70%.
**После оптимизации (Build 191+):** Core 0 = ~7%, Core 1 = ~25-35%.

Ключевые оптимизации:

1. **Strict Float Policy (Build 189):**
   - Полный аудит: все `double` → `float`, `sin()` → `sinf()`, `log10()` → `log10f()`
   - RP2350 имеет single-precision FPU; double-precision эмулируется софтово (~10x медленнее)
   - Эффект: Core 0 с ~30% до ~15%

2. **Compiler flags (Build 189):**
   ```cmake
   -O3 -ffast-math -funroll-loops
   -mfloat-abi=hard -mfpu=fpv5-sp-d16
   ```
   `-flto` **не** используется — несовместим с Pico SDK `__wrap_` символами.

3. **Hardware ADC FIFO (Build 190):**
   - `adc_fifo_setup()` + `adc_run(true)` для 10kHz без джиттера
   - `tight_loop_contents()` вместо `__wfe()` (WFE теряет сэмплы без ADC IRQ)

4. **fast_log2f() (Build 190):**
   - IEEE 754 bit-trick для логарифма
   - ~4x быстрее стандартного `log10f()`
   - Используется в расчёте dB для сигнала и SNR

5. **AGC precompute (Build 190):**
   - `1.0f / release` вычисляется один раз → умножение вместо деления в inner loop

6. **FFT на Core 1 (Build 191):**
   - FFT перенесён с Core 0 на Core 1 — он нужен только для отрисовки спектра и SEARCH
   - Core 0 освобождён от 1024-point FFT (~2ms per frame)
   - Эффект: Core 0 с ~15% до ~7%

7. **Ping-Pong DMA Buffers (Build 190):**
   - Двойная буферизация для SPI дисплея
   - Одна полоска рисуется пока вторая передаётся

### Снижение загрузки Core 1 (UI)

1. **Спрайтовая отрисовка (LovyanGFX):**
   - Top bar, bottom bar, текстовая зона — каждый в отдельном спрайте
   - Перерисовка только при изменении данных (not every frame)

2. **Waterfall оптимизация:**
   - Прямой SPI DMA для полосок водопада
   - 480px × 1 строка за один DMA transfer

3. **FFT rate limiting:**
   - FFT считается не на каждый frame, а при `new_data_ready` (каждые ~48ms = 480 сэмплов)

### Текущая загрузка (Build 218)
- **Core 0:** 5-8% (DSP idle) / 10-15% (BAUD-DET active, histogram + scoring)
- **Core 1:** 25-35% (зависит от display mode: спектр+водопад vs tuning lab)

## 1. Шрифтовая система (Roadmap Item #1)

### Этап 1: 4 режима шрифтов — DONE (Build 195)
- [x] BIG: Spleen 8×16 (9 строк, 55 символов)
- [x] MED: Bitocra 7×13 (11 строк, 62 символа)
- [x] SMALL: Font0 6×8 (15 строк, 73 символа)
- [x] TINY: Spleen 5×8 (17 строк, 90 символов) — Build 199
- [x] Конвертер `tools/bdf2gfx.py`
- [x] Автоматический line_width при переключении шрифта
- [x] Сохранение в flash

### Этап 2: Font Lab — TODO
Отдельный экран для тонкой настройки шрифтов (размер, spacing, line_height).

### Этап 3: Скины и цветовые схемы — TODO
- Classic Green (текущая)
- SDR Warm (тёмно-синий фон, тёплая палитра водопада)

## 2. Интеллектуальная автоматика приёма — DONE

### Авто-инверсия Mark/Space — DONE (Build 196-202)
- [x] Сравнительный алгоритм (ERR before/after flip, ±3% порог)
- [x] Индикатор NOR?/INV? при неопределённости
- [x] SEARCH сбрасывает INV → NOR

### SEARCH (автопоиск) — DONE (Build 198-216)
- [x] FFT-based, все 8 шифтов, multi-signal (до 8)
- [x] Parabolic peak interpolation (Build 216)
- [x] Shift-proportional dedup tolerance (Build 216)
- [x] dist_penalty = 2.5 (Build 216)
- [x] Cycling (< 10s между нажатиями)

### Авто-определение шифта — DONE (Build 200-203)
- [x] 8 стандартных шифтов, режим SHIFT AUTO (idx=8)
- [x] Popup 3×3

### BAUD-DET (автоопределение скорости) — DONE (Build 206)
- [x] Symbol Duration Histogram + Harmonic Scoring
- [x] Fallback: ERR verify (sequential test)
- [x] 4 baud rates: 45.45 / 50 / 75 / 100
- [x] Popup 3×2

### STOP-DET (автоопределение стоп-бита) — DONE (Build 205-218)
- [x] Direct gap measurement (state-7-end → next start-bit)
- [x] Warmup 1.5s, idle filter 1.25T, bin boundaries 0.25/0.85T (Build 218)
- [x] Chain BAUD→STOP через shared_chain_stop_after_baud (Build 217)
- [x] Popup 2×2

### Полный pipeline — DONE (Build 217)
- [x] SEARCH → SHIFT → BAUD (chain) → STOP → INV
- [x] Автоматическая цепочка, STOP ждёт завершения BAUD
- [ ] Итоговый экран "Found: 50 Baud, 450 Hz shift, 1.5 stop"

### Auto-Recovery — DONE (Build 217)
- [x] ERR > 15% для 3s → BAUD-DET → chain → STOP-DET
- [x] Защита от конфликта с auto-INV

### Clipping Indicator — DONE (Build 216)
- [x] SIG bar мигает red/white при ADC clipping
- [x] Текст "CLIP!" мигает синим
- [x] Latch 1.5 секунды

## 3. Аппаратное Ускорение Рендеринга
- [ ] Hardware Scroll (ILI9488 VSCRSADD)
- [ ] SIO INTERP Colormap
- [x] Ping-Pong DMA Buffers (Build 190)

## 4. Оптимизация под RP2350
- [x] Strict Float Policy (Build 189)
- [x] Hardware ADC FIFO (Build 190)
- [x] fast_log2f() IEEE 754 bit-trick (Build 190)
- [x] AGC precompute (Build 190)
- [x] FFT на Core 1 (Build 191)
- [ ] Memory Barriers (__dmb())
- [ ] CMSIS-DSP (arm_fir_f32, arm_biquad_f32)

## 5. UI оптимизация
- [ ] Selective Redraw
- [ ] Widget Framework
- [x] Eye Diagram с phosphor persistence (Build 194)
- [x] Error Rate Indicator, 3 thin bars (Build 191)

## 6. Compiler Flags
- [x] `-O3`, `-ffast-math`, `-funroll-loops` (Build 189)
- [x] `-mfloat-abi=hard`, `-mfpu=fpv5-sp-d16` (Build 189)
- **Примечание:** `-flto` несовместим с Pico SDK `__wrap_`

## 7. Serial Command Interface
- [x] 18+ команд (Build 194-206)
- [x] Диагностический поток `[D]` (Build 194)
- [x] serial_cmd.ps1 с try/finally/Dispose + DTR/RTS (Build 217)

## 8. Планируемые фичи

### SITOR-B / NAVTEX FEC — TODO (приоритет)
- [ ] Framer: 7 data bits + 1 stop
- [ ] CCIR 476 lookup (35 valid codewords, ratio 4:3)
- [ ] Time diversity buffer (5 символов)
- [ ] Phasing sync (DX/RX signals)
- [ ] Auto-detect: 100/170 → try SITOR-B

### Встроенный автотюнинг — TODO
- [ ] Кнопка AUTO в Tuning Lab
- [ ] Hill-climb: ALPHA → BW → SQ
- [ ] Score = -5×ERR + SNR - 1000×|FE| + SQ_bonus

### Прочее
- [ ] Итоговый экран "Found: ..." после SEARCH
- [ ] Мультиплатформенность (ILI9341 320×240)
- [ ] SD карта (DWD SYNOP parser)
- [ ] CW Декодер (K-Means)
- [ ] I2S DAC Audio Output

---
*Статус: Build 218, ветка feat/alex-cl-dev*
