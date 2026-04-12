# TouchRTTY — Полный контекст разработки

*Дата: 2026-04-12, Build 218*

## 1. Что это

**TouchRTTY** — standalone RTTY декодер на микроконтроллере RP2350 (Raspberry Pi Pico 2) с сенсорным дисплеем ILI9488 480×320. Принимает аудио RTTY сигнал с приёмника (через ADC), демодулирует, декодирует Baudot ITA2 и выводит текст на экран.

**Репозиторий:** `https://github.com/Alex-Electron/TouchRTTY.git`  
**Ветка:** `feat/alex-cl-dev`  
**Путь:** `C:\Temp\TouchRTTY`

## 2. Архитектура

### 2.1 Аппаратная часть
- **MCU:** RP2350 (dual Cortex-M33, FPU, 150 MHz)
- **Дисплей:** ILI9488 480×320 TFT, SPI, тач GT911
- **Аудиовход:** ADC0 (GPIO26), 10 kHz sample rate
- **Сборка:** CMake + Ninja + Pico SDK 2.2, прошивка через picotool

### 2.2 Dual-core архитектура
- **Core 0 (DSP):** ADC DMA @ 10kHz → AGC → I/Q demod (Mark/Space) → Biquad LPF → ATC envelope → Discriminator D → DPLL framer → Baudot decode → char output. Также: BAUD-DET, STOP-DET, auto-inversion, auto-recovery.
- **Core 1 (UI):** FFT (1024-point) → спектр/водопад → SEARCH algorithm → touch → serial command parser → display render.

### 2.3 Межъядерный обмен
Через `volatile` shared-переменные в `app_state.hpp/.cpp` (без mutex, lock-free). Ключевые группы:

**Настройки протокола:**
- `shared_baud_idx` (0-3=fixed, 4=AUTO), `shared_baud_auto`, `shared_active_baud`
- `shared_shift_idx` (0-7=fixed, 8=AUTO), `shared_active_shift`
- `shared_stop_idx` (0-2=fixed, 3=AUTO), `shared_stop_auto`, `shared_active_stop`
- `shared_rtty_inv`, `shared_inv_auto`, `shared_inv_uncertain`

**Метрики сигнала:**
- `shared_err_rate` (0-100%), `shared_squelch_open`, `shared_snr_db`
- `shared_signal_db`, `shared_adc_clipping`, `shared_agc_gain`

**Автоматика (state machines):**
- `shared_search_state` (0=idle, 1=scanning, 2=found, 3=none)
- `shared_baud_detect_req/state` (0=idle, 1=histogram, 2=verify, 3=done)
- `shared_stop_detect_req/state` (0=idle, 1=measuring, 2=done→used internally, 3=done)
- `shared_chain_stop_after_baud` — флаг цепочки BAUD→STOP из SEARCH

**Буферы DSP→UI:**
- `shared_fft_mag[512]`, `shared_fft_ts[1024]`, `shared_adc_waveform[480]`
- `shared_mag_m[480]`, `shared_mag_s[480]` — mark/space power per bin
- `shared_eye_buf[16][256]` — eye diagram traces

## 3. Файловая структура

```
src/
  main.cpp              — точка входа, инициализация, запуск core1
  dsp_pipeline.cpp      — Core 0: весь DSP, DPLL framer, BAUD-DET, STOP-DET, auto-INV, auto-recovery
  ui_loop.cpp           — Core 1: FFT, SEARCH, UI logic, serial parsing, touch
  serial_commands.cpp   — обработка serial-команд
  settings_flash.cpp    — чтение/запись настроек во Flash
  app_state.hpp/.cpp    — все shared volatile переменные и константы
  ui/UIManager.hpp      — отрисовка: спектр, водопад, текстовая зона, top/bottom bar, попапы
  dsp/biquad.hpp        — Biquad IIR фильтр (LPF)
  dsp/fft.hpp           — FFT реализация (1024-point)
  display/ili9488_driver.h — SPI драйвер дисплея
  version.h             — BUILD_NUMBER (текущий: 218)
  fonts/                — Spleen 8x16, Bitocra 7x13, Spleen 5x8 (Adafruit GFX format)
tools/
  rtty_simulator.html   — браузерный генератор тестового RTTY (Web Audio API, Center/Mark mode)
  serial_cmd.ps1        — PowerShell утилита для serial-команд (try/finally/Dispose, DTR/RTS)
  bdf2gfx.py            — конвертер BDF шрифтов в Adafruit GFX header
  autotune.py           — Python автотюнинг DSP параметров через serial
docs/
  DEVELOPMENT_CONTEXT.md — этот файл
  PHASE3_RTTY_DSP_FINAL.md — детальная документация DSP и алгоритмов
  ROADMAP_OPTIMIZATION.md  — roadmap с TODO/DONE
```

## 4. DSP Pipeline (Core 0, dsp_pipeline.cpp, main loop @ 10kHz)

```
ADC (DMA, 10kHz) → AGC (attack/release envelope) → I/Q demodulation:
  Mark: f_out × cos/sin(mark_freq) → Biquad LPF → mark_power = mi²+mq²
  Space: f_out × cos/sin(space_freq) → Biquad LPF → space_power = si²+sq²
  → ATC envelope (fast attack / slow release)
  → D = mark_norm - space_norm (clamp ±1.5)
  → if INV: D = -D
  → d_sign = (D > 0)
```

### DPLL Framer (PI-регулятор)
```
Continuous DPLL: symbol_phase += baud/sample_rate + freq_error
На каждом transition (d_sign change):
  phase_error = symbol_phase (если < 0.5) или symbol_phase - 1.0
  phase_error clamped to ±0.1
  symbol_phase -= alpha × phase_error
  freq_error -= beta × phase_error  (beta = alpha²/2)
  freq_error clamped to ±0.05 × phase_inc

Baudot state machine:
  0: idle (wait Mark→Space transition = start bit)
  1: start bit (verify space, else false start → state 0)
  2-6: data bits 0-4 (integrate D over symbol, threshold > 0 = mark = 1)
  7: stop bit (bit=1 → valid, decode ITA2; bit=0 → error, ERR counter++)
     → arm stop_gap_state7_end_us для STOP-DET
     → if stop_bits ≤ 1.0: immediately enter state 1 (continuous stream)
     → else: state 0 (wait for next transition)
```

### Ключевые DSP параметры (настраиваемые)
| Параметр | Диапазон | Default | Описание |
|----------|----------|---------|----------|
| `tuning_dpll_alpha` | 0.005-0.2 | 0.035 | Коэффициент P петли DPLL |
| `tuning_lpf_k` | 0.2-2.0 | 0.75 | Множитель частоты среза LPF = baud × k |
| `tuning_sq_snr` | 0-20 dB | 6.0 | Порог squelch по SNR |

## 5. Автоматика приёма (Build 196-218)

### 5.1 SEARCH — автопоиск сигнала (Core 1, ui_loop.cpp)

**Запуск:** кнопка SEARCH на экране или serial `SEARCH`.

**Алгоритм (полный поиск, Build 216+):**

1. **FFT спектр** — берёт `smooth_mag[512]` (сглаженный FFT).

2. **Нахождение локальных максимумов:**
   - Пик: `smooth_mag[i] > smooth_mag[i-1]` И `smooth_mag[i] > smooth_mag[i+1]`
   - SNR > 4 дБ (над средним уровнем шума)
   - Topлист: 16 лучших пиков, сортировка по SNR
   - Debug вывод: `SEARCH-DBG noise_avg=X top peaks: freq(bin,snr) ...`

3. **Перебор всех 8 шифтов** (85/170/200/340/425/450/500/850 Hz):
   - Для каждой пары пиков (lo, hi) проверяет: расстояние ≈ shift ± tolerance
   - Оба SNR > min_snr (8 дБ), imbalance < 20 дБ
   - Оба — локальные максимумы (3-bin window)

4. **Parabolic peak interpolation (Build 216):**
   ```
   delta = 0.5 × (a - c) / (a - 2b + c)   // a,b,c = mag[k-1], mag[k], mag[k+1]
   refined_k = k + delta                     // sub-bin precision
   ```
   Даёт точность ±5 Hz вместо ±10 Hz (один бин = SAMPLE_RATE/FFT_SIZE ≈ 9.77 Hz).

5. **Scoring:**
   ```
   base = min(snr_lo, snr_hi) + 0.5 × |snr_lo - snr_hi|
   dist_penalty = |actual_bins - ideal_bins| × 2.5
   score = base - dist_penalty
   ```
   Где `actual_bins` вычисляется по refined positions, `ideal_bins = shift × FFT_SIZE / SAMPLE_RATE`.

6. **Массив кандидатов** (128 max, с вытеснением самого слабого при переполнении).

7. **Дедупликация (Build 216):**
   - Tolerance пропорционален шифту: `max(3, shift_bins / 8)`
   - Для 850 Hz → 10 бинов; для 85 Hz → 3 бина
   - Перекрытие: если `|lo_i - lo_j| ≤ tol` ИЛИ `|hi_i - hi_j| ≤ tol` → дубликат
   - При дублировании оставляется кандидат с лучшим score

8. **Адаптивный порог:** отсекает кандидаты с score < 40% от лучшего.

9. **Предпочтение широкого шифта:** если 2 кандидата перекрываются и score ≥ 70% от лучшего, выбирается кандидат с более широким шифтом.

10. **Результат:** список `found_signals[]` (до 8), отсортированный по центральной частоте.

**Cycling (Build 205+):**
- Первое нажатие (или > 10с с последнего): полный FFT rescan
- Повторное нажатие < 10с: циклический переход по кэшу `found_signals[]`
- При cycling тоже запускается autodetect pipeline (BAUD/STOP/INV)

**Pipeline после SEARCH (Build 217+):**
```
SEARCH → SHIFT (всегда, переключает в AUTO) →
  if (baud_auto && stop_auto):
    BAUD-DET → [chain] → STOP-DET  // последовательно!
  else if (baud_auto):
    BAUD-DET
  else if (stop_auto):
    STOP-DET
  → INV AUTO reset (NOR + uncertain=false)
```
Ключевой момент: STOP-DET **никогда** не запускается параллельно с BAUD-DET. Флаг `shared_chain_stop_after_baud` гарантирует последовательность.

### 5.2 BAUD-DET — автоопределение скорости (Core 0, dsp_pipeline.cpp)

**Метод:** Symbol Duration Histogram + ERR verify fallback.

**Фаза 1 — Histogram (5s, минимум 50 transitions):**
1. На каждом transition `d_sign` считает интервал (в сэмплах) от предыдущего transition
2. Записывает в `baud_hist[interval]++` (bins 0..511)
3. Минимум 5 секунд и 50+ transitions для надёжности

**Фаза 2 — Scoring:**
Для каждого baud rate (45.45/50/75/100):
```
bit_period = SAMPLE_RATE / baud  (напр. 220 для 45.45)
score = Σ hist[k × bit_period ± 8] × weight(k, distance)
  k = 1..5 (основная + гармоники)
  weight = harmonic_decay × distance_decay
```

**Решение:**
- `best_score > 1.5 × second_best` → **clear winner**, применяем сразу
- Иначе → **ambiguous**, переходим к ERR verify

**ERR verify (фаза 3):**
- Последовательно тестирует каждый baud rate (2 секунды на каждый)
- Переключает framer, очищает ERR history, считает `shared_err_rate`
- Выбирает baud с минимальным ERR%
- На реальных сигналах: правильный baud даёт 0-1% ERR, остальные 7-14%

**Результат:** `shared_baud_detect_state = 3`, `shared_active_baud` установлен.

### 5.3 STOP-DET — автоопределение стоп-бита (Core 0, dsp_pipeline.cpp)

**Метод:** Direct gap measurement с histogram voting (Build 217-218).

**Принцип:** Измеряет время от конца 1-го стоп-бита (state-7-end в DPLL framer) до следующего mark→space перехода (start bit). Эта разница ("gap") линейно зависит от реальной длины стоп-бита:
- **1.0 stop:** gap ≈ 0T (start bit начинается сразу)
- **1.5 stop:** gap ≈ 0.5T (полбита паузы)
- **2.0 stop:** gap ≈ 1.0T (целый бит паузы)

**Permissive mode:** Во время детекции framer работает с `stop_bits_expected = 2.0`, чтобы не терять символы при любом реальном stop bit.

**Фильтрация (Build 218):**
1. **Warmup 1.5s** — первые 1.5 секунды после старта STOP-DET гапы игнорируются (DPLL ещё не стабилизировался, фазовый шум завышает измерения)
2. **Idle filter 1.25T** — гапы > 1.25T помечаются как idle/break и не голосуют (межфреймовые паузы, пустые строки)
3. **Только при ERR < 10%** — гапы считаются только когда framer стабильно декодирует

**Bin boundaries (Build 218):**
```
gap < 0.25T  → bin 0 (1.0 stop)
gap < 0.85T  → bin 1 (1.5 stop)
gap ≥ 0.85T  → bin 2 (2.0 stop)
```
Граница 0.85T выбрана по данным тестов: 2.0 stop даёт gap ~0.93-1.03T, 1.5 stop — ~0.4-0.55T (на стабильном DPLL).

**Завершение:** минимум 20 голосов И > 3 секунд, или hard timeout 8 секунд. Выбирается bin с максимальным количеством голосов (default = 1.5 при ничьей).

### 5.4 Chain BAUD→STOP (Build 217)

**Проблема:** Если BAUD-DET и STOP-DET запускаются одновременно, STOP-DET классифицирует gap_fraction с дефолтным baud (45.45), что даёт неправильные bin-голоса для других скоростей.

**Решение:** Флаг `shared_chain_stop_after_baud`:
1. SEARCH (ui_loop.cpp) ставит `shared_chain_stop_after_baud = true` и запускает только BAUD-DET
2. В dsp_pipeline.cpp (рядом с auto-recovery chain) проверка:
   ```
   if (shared_chain_stop_after_baud && baud_detect_phase == 0
       && !shared_baud_detect_req && shared_baud_detect_state == 3
       && stop_detect_phase == 0 && !shared_stop_detect_req) {
       → fire STOP-DET
       → shared_chain_stop_after_baud = false
   }
   ```
3. STOP-DET запускается с уже корректным значением `baud` → gap_fraction вычисляется правильно.

### 5.5 Auto-Inversion (Build 196+, Core 0)

- Триггер: `shared_err_rate > 12%` + squelch open + 0.8s подряд
- Сохраняет текущий ERR, переключает `shared_rtty_inv`, ждёт 0.8s
- Сравнивает: ERR упал > 3% → оставляет; вырос > 3% → откатывает; ±3% → откатывает + `inv_uncertain = true`
- `inv_uncertain` гаснет при ERR < 5% + squelch open

### 5.6 Auto-Recovery (Core 0, dsp_pipeline.cpp)

При `shared_stop_idx == 3` (AUTO) + `shared_baud_auto`:
- Если `shared_err_rate > 15%` устойчиво > 3 секунд → запускает BAUD-DET
- Флаг `auto_recovery_pending_stop` → после BAUD-DET запускает STOP-DET
- Работает аналогично chain из SEARCH, но по таймауту ошибок

## 6. Индикация (UIManager.hpp)

### Top bar (0-40px)
```
Row 1: [SIG bar] dB  SNR=xx  M:xxxx S:xxxx  C0:xx% C1:xx%
Row 2: SH:170  ST:1.5(A)  NOR/INV/?
Row 3: BD:50(A)  [ERR bar] xx%
```

**Clipping indicator (Build 216):**
- При `shared_adc_clipping`: SIG bar заполняется полностью, мигает red/white (200ms)
- Текст "CLIP!" мигает синим рядом с SIG bar
- Задержка (latch) 1.5 секунды после последнего клипа

**Цветовая индикация:**
- Ручные параметры: голубой (cyan)
- Авто-определённые: зелёный
- В процессе детекции: жёлтый с ".."
- Ошибка/uncertain: с вопросительным знаком

### Bottom bar (252-320px)
Touch-кнопки: `B:45` `S:170` `ST:1.5` `AFC` `MENU`/`SRCH`

### Попапы (touch overlays на текстовой зоне)
- **Baud popup** (3×2): 45 / 50 / 75 / 100 / AUTO
- **Shift popup** (3×3): 85/170/200/340/425/450/500/850/AUTO
- **Stop popup** (2×2): 1.0 / 1.5 / 2.0 / AUTO

## 7. Serial Interface (COM27, 115200)

18+ команд через USB CDC:
```
ALPHA [val]  — DPLL alpha (0.005-0.2)
BW [val]     — LPF bandwidth multiplier
SQ [val]     — Squelch SNR threshold
FREQ [val]   — Center frequency
BAUD 0-4|AUTO — Baud rate (0=45, 1=50, 2=75, 3=100, 4/AUTO=auto)
SHIFT 0-8|AUTO  — Shift (0=85...7=850, 8/AUTO=auto)
STOP 0-2|AUTO   — Stop bits (0=1.0, 1=1.5, 2=2.0, AUTO)
INV AUTO|NOR|INV — Inversion mode
AFC ON|OFF   — Automatic frequency control
AGC ON|OFF   — Automatic gain control
SCALE EXP|LIN — Spectrum scale
WIDTH [30-120]  — Characters per line
DIAG ON|OFF  — Diagnostic stream
SEARCH       — Trigger signal search
STATUS       — Show current state
SAVE         — Save settings to flash
CLEAR        — Clear text & DSP
HELP         — Command list
```

**Утилита:** `tools/serial_cmd.ps1` — PowerShell скрипт с try/finally/Dispose, DTR/RTS enabled.

## 8. Flash Settings (AppSettings)

Структура в `app_state.hpp`, сохраняется по offset `2MB + FLASH_SECTOR_SIZE`:
```cpp
struct AppSettings {
    uint32_t magic;
    int baud_idx, shift_idx, stop_idx;
    bool rtty_inv;
    int display_mode;
    bool exp_scale, auto_scale;
    float filter_k, sq_snr, target_freq;
    bool serial_diag;
    int line_width;
    bool afc_on;
    int font_mode;
    float dpll_alpha;
    bool inv_auto, stop_auto;
};
```

Auto-save: `flag_settings_change()` → 5-секундный debounce → write to flash.

## 9. Сборка и прошивка

```bash
# Сборка
cd C:\Temp\TouchRTTY
cmake --build build

# Прошивка (устройство подключено по USB, NOT в BOOTSEL mode)
C:/Users/Lavrinovich/.pico-sdk/picotool/2.2.0-a4/picotool/picotool.exe load -f -x build/TouchRTTY.uf2

# Serial monitor
# COM27, 115200, No parity, 8 data bits, 1 stop bit
# PowerShell: tools\serial_cmd.ps1 -cmd "STATUS" -port "COM27"
```

## 10. Тестирование

### Тестовый генератор
`tools/rtty_simulator.html` — открыть в браузере, подключить аудиовыход к ADC.
- **Input modes:** Center frequency или Mark frequency (direct)
- Baud: 45.45 / 50 / 75 / 100
- Shift: 85-850 Hz + Custom
- Stop bits: 1.0 / 1.5 / 2.0
- Inversion: Normal / Inverted
- Текст: "RYRYRY THE QUICK BROWN FOX JUMPS OVER 1234567890" (loop)

### Тестовая матрица Build 218 (все пройдены ✓)

| Комбинация | Shift | Baud | Stop | Декод |
|---|---|---|---|---|
| 45/170/1.0 | 170 ✓ | 45.45 ✓ | 1.0 ✓ | OK |
| 100/850/1.0 | 850 ✓ | 100 ✓ | 1.0 ✓ | OK |
| 45/170/1.5 | 170 ✓ | 45.45 ✓ | 1.5 ✓ | OK |
| 50/450/1.5 | 450 ✓ | 50 ✓ | 1.5 ✓ | OK |
| 75/425/1.5 | 425 ✓ | 75 ✓ | 1.5 ✓ | OK |
| 100/850/1.5 | 850 ✓ | 100 ✓ | 1.5 ✓ | OK |
| 45/170/2.0 | 170 ✓ | 45.45 ✓ | 2.0 ✓ | OK |
| 100/850/2.0 | 850 ✓ | 100 ✓ | 2.0 ✓ | OK |

### Реальный эфир (WebSDR, 2026-04-12)

| Частота | Станция | Параметры | Результат |
|---|---|---|---|
| 4583 кГц | DWD (DDK2) | 50/450/1.5 | Чисто, полный текст |
| 10100 кГц | DWD (DDK9) | 50/~425/1.5 | Корректно, шумнее |
| 7646 кГц | DWD (DDH7) | 50/450/1.5 | Зашумлён, корректно |
| 12579 кГц | SITOR-B | 100/170/1.0 | Параметры верны, FEC не декодируется |

## 11. Известные проблемы / TODO

### Нерешённые
1. **425 vs 450 shift** — FFT разрешение ~10 Hz/бин, разница 25 Hz (2.5 бина). При FSK keying спектральное размытие делает их неразличимыми.
2. **SEARCH на 14 МГц** — при нескольких близких сигналах выбирает shift=85 (ложный пик от пар гармоник). Нужна фильтрация ложных кандидатов.
3. **Memory barriers** — нет `__dmb()` между ядрами, теоретически race condition.

### Планируемые фичи
1. **SITOR-B / NAVTEX FEC** — декодер 100/170, CCIR 476, time diversity (7 бит, ratio 4:3)
2. **Итоговый экран** "Found: 50 Baud, 450 Hz shift, 1.5 stop"
3. Font Lab — подменю настройки шрифтов
4. Скины / цветовые схемы
5. Встроенный автотюнинг DSP (без ПК)
