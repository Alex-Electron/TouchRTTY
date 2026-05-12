# Фаза 3: Профессиональный DSP Демодулятор RTTY (Итоги)

*Обновлено: 2026-04-12, Build 218*

Этот документ описывает архитектуру и алгоритмы программного RTTY демодулятора на микроконтроллере RP2350.

## 1. Архитектура конвейера (Core 0, dsp_pipeline.cpp)

Весь DSP-конвейер работает на Ядре 0 с жестким таймингом ровно 10 000 итераций в секунду (10 кГц). FFT-расчёты и отрисовка графики вынесены на Ядро 1 (ui_loop.cpp).

1. **АЦП (ADC):** Считывание сэмплов с разрешением 12 бит через DMA FIFO.
2. **AGC (Automatic Gain Control):** Атака/отпускание огибающей для нормализации уровня.
3. **I/Q Demodulator:** Входящий сигнал умножается на локально генерируемые синусы и косинусы частот Mark и Space, переводя полезный сигнал на baseband.
4. **Biquad LPF:** Фильтрация I и Q ветвей для каждого тона. Частота среза = `baud × tuning_lpf_k` (default K=0.75).
5. **ATC (Automatic Threshold Correction):** Детекторы огибающей с быстрой атакой и медленным спадом (FASR) для компенсации замираний (fading).
6. **Дискриминатор:** `D = mark_norm - space_norm` (clamp ±1.5). Если INV: `D = -D`.
7. **DPLL (Digital Phase-Locked Loop):** ПИ-регулятор для битовой синхронизации.
8. **Baudot Framer:** Конечный автомат, 8 состояний (idle, start, 5×data, stop).
9. **Автоматика:** BAUD-DET, STOP-DET, auto-INV, auto-recovery — все на Core 0.

## 2. DPLL — цифровая фазовая автоподстройка

### 2.1 ПИ-регулятор (Proportional-Integral)

```
symbol_phase += baud/SAMPLE_RATE + freq_error    // фаза бита [0..1)
На каждом transition (mark↔space):
  if (baudot_state > 0):   // только когда framer захвачен
    phase_error = symbol_phase       // если < 0.5
                  symbol_phase - 1.0 // если ≥ 0.5
    clamp(phase_error, -0.1, +0.1)   // ограничение
    symbol_phase -= alpha × phase_error      // P-звено (мгновенная коррекция)
    freq_error -= beta × phase_error         // I-звено (частотная подстройка)
    clamp(freq_error, ±0.05 × phase_inc)     // anti-windup
```

- **alpha** (default 0.035): ширина петли, определяет скорость захвата
- **beta = alpha²/2**: интегральный коэффициент, компенсирует drift генератора
- Anti-windup: `max_fe = 0.05 × phase_inc` предотвращает насыщение при отсутствии сигнала

### 2.2 Проблема 1.0 стоп-бита (Continuous Streaming)

При 1.0 стоп-бите следующий стартовый бит начинается мгновенно. Решение — Continuous DPLL: после state 7 (stop) если `stop_bits_expected ≤ 1.05`, framer сразу входит в state 1 (start), устанавливая `symbol_phase = 0` и `integrate_acc = D`.

### 2.3 Eye Diagram

DPLL-синхронизированная глазковая диаграмма (240×64 px, 16 traces, phosphor persistence):
```
spb = SAMPLE_RATE / baud  // samples per bit
ex = (int)(symbol_phase × (spb - 1))
shared_eye_buf[eye_idx][ex] = D × 127  // int8 quantization
```

## 3. Baudot Framer (ITA2)

### Конечный автомат (8 состояний)

| State | Описание | Действие |
|-------|----------|----------|
| 0 | Idle | Ждёт Mark→Space transition |
| 1 | Start bit | Интегрирует D; если > 0 (mark) → false start → state 0 |
| 2-6 | Data bits 0-4 | Интегрирует D; `bit = (integrate_acc > 0)` |
| 7 | Stop bit | `bit=1` → valid (decode ITA2); `bit=0` → ERR |

### Декодирование ITA2
- 5-битный код → 2 таблицы: `ita2_ltrs[]` (буквы) и `ita2_figs[]` (цифры/символы)
- Код 27 (11011) → FIGS shift; код 31 (11111) → LTRS shift
- Пробел (code 4) автоматически возвращает в LTRS (радиотелетайпная конвенция)

### Error Rate (скользящее окно)
```
err_hist[100]  // кольцевой буфер: 0=OK, 1=ERR
err_sum = Σ err_hist[i]
shared_err_rate = err_sum  // 0-100%
```

## 4. SEARCH — алгоритм поиска сигнала (Core 1)

### 4.1 Обнаружение пиков FFT

1. Вычислить средний уровень шума: `noise_avg = mean(smooth_mag[1..511])`
2. Найти локальные максимумы: `mag[i] > mag[i-1]` И `mag[i] > mag[i+1]` И `SNR > 4 дБ`
3. Топ-16 пиков по SNR (для debug-вывода)

### 4.2 Parabolic Peak Interpolation (Build 216)

Повышает точность определения частоты пика с ±10 Hz (ширина бина) до ±2-5 Hz:
```
a = mag[k-1], b = mag[k], c = mag[k+1]
denom = a - 2b + c
delta = 0.5 × (a - c) / denom     // смещение от целого бина
refined_k = k + clamp(delta, -1, +1)
refined_freq = refined_k × SAMPLE_RATE / FFT_SIZE
```

### 4.3 Кандидаты и scoring

Для каждого из 8 шифтов перебирает все пары пиков:
```
ideal_bins = shift × FFT_SIZE / SAMPLE_RATE
actual_bins = hi_refined - lo_refined
dist_penalty = |actual_bins - ideal_bins| × 2.5   // штраф за отклонение
score = min(snr_lo, snr_hi) + 0.5 × |snr_lo - snr_hi| - dist_penalty
```

`dist_penalty = 2.5` — повышен (было 1.5) для лучшей дискриминации близких шифтов (425 vs 450).

### 4.4 Дедупликация (Build 216)

Проблема: FSK keying создаёт спектральное размытие → один сигнал может дать 6+ кандидатов с разными парами пиков.

Решение — shift-proportional tolerance:
```
shift_bins = shift × FFT_SIZE / SAMPLE_RATE
tol = max(3, shift_bins / 8)   // для 850 Hz → 10 bins, для 85 Hz → 3 bins
```
Кандидаты с `|lo_i - lo_j| ≤ max(tol_i, tol_j)` ИЛИ `|hi_i - hi_j| ≤ max(tol_i, tol_j)` считаются дубликатами; остаётся лучший по score.

### 4.5 Multi-signal и Cycling

- `found_signals[8]` — кэш найденных сигналов
- Первое нажатие: полный rescan, выбор по best score
- Повторное < 10 секунд: `found_current = (found_current + 1) % found_count` (cycling)
- > 10 секунд: новый полный rescan

## 5. BAUD-DET — автоопределение скорости (Core 0)

### 5.1 Symbol Duration Histogram

```
На каждом D-sign transition:
  interval = sample_count - last_transition
  if (interval > 0 && interval < 512):
    baud_hist[interval]++
```

Минимум: 5 секунд, 50+ transitions.

### 5.2 Harmonic Scoring

Для каждого baud rate (45.45/50/75/100):
```
bit_period = SAMPLE_RATE / baud     // 220/200/133/100 samples
score = 0
for k = 1..5:                       // основная + 4 гармоники
  center = k × bit_period
  for offset = -8..+8:
    if hist[center + offset] > 0:
      dist_weight = 1.0 - |offset| / 16.0
      harm_weight = 1.0 / k          // гармоники менее значимы
      score += hist[center + offset] × dist_weight × harm_weight
```

### 5.3 Decision Logic

```
if best_score > 1.5 × second_best → immediate apply ("clear winner")
else → ERR verify: test each baud 2s, select minimum ERR%
```

ERR verify на реальных сигналах:
- Правильный baud: ERR = 0-1%
- Неправильный baud: ERR = 7-14%

## 6. STOP-DET — автоопределение стоп-бита (Core 0)

### 6.1 Gap Measurement

Метод основан на прямом измерении зазора между концом 1-го стоп-бита и началом следующего start-бита:

```
State 7 (stop bit) заканчивается → stop_gap_state7_end_us = time_us_32()
...
Следующий Mark→Space transition (start bit):
  gap_us = time_us_32() - stop_gap_state7_end_us
  gap_fraction = gap_us / (1000000 / baud)   // в долях бит-периода
```

### 6.2 Gap Model

| Real stop | Measured gap | Bin |
|-----------|-------------|-----|
| 1.0 | ≈ 0T (0-0.06T) | 0 |
| 1.5 | ≈ 0.5T (0.3-0.8T) | 1 |
| 2.0 | ≈ 1.0T (0.85-1.1T) | 2 |

### 6.3 Фильтрация (Build 218)

1. **Warmup 1.5s** — первые 1.5 секунды после старта STOP-DET игнорируются. DPLL ещё не стабилизировался после переключения framer на permissive mode, первые измерения систематически завышены.

2. **Idle filter 1.25T** — гапы > 1.25T ��тбрасываются как межфреймовые паузы. На реальных сигналах видны гапы 2-6T между строками/фреймами. Порог 1.25T выбран так, чтобы пропускать максимальные гапы 2.0 stop (~1.1T) но отсекать минимальные idle (~1.3T).

3. **ERR gate** — гапы считаются только при `shared_err_rate < 10%` и `shared_squelch_open`.

### 6.4 Bin Boundaries

```
gap < 0.25T  → bin 0 (голос за 1.0 stop)
gap < 0.85T  → bin 1 (голос за 1.5 stop)
gap ≥ 0.85T  → bin 2 (голос за 2.0 stop)
```

Граница 0.25T: отделяет "мгновенные" переходы (1.0 stop) от ненулевых пауз.
Граница 0.85T: отделяет 1.5 stop (~0.5T) от 2.0 stop (~1.0T). Выбрана по эмпирическим данным: при 2.0 stop минимальные гапы = 0.93T; при 1.5 stop максимальные стабильные гапы = 0.76T.

### 6.5 Voting и Decision

```
stop_gap_hist[3] = {votes_1.0, votes_1.5, votes_2.0}
Минимум: 20 голосов И > 3 секунд (или hard timeout 8с)
Результат: bin с максимальными голосами (default = 1.5 при ничьей)
```

## 7. Chain BAUD→STOP (Build 217)

### Проблема

STOP-DET вычисляет `gap_fraction = gap_us / bit_period_us`, где `bit_period = 1/baud`. Если baud ещё не определён (дефолт 45.45), gap_fraction будет ошибочным. Пример: реальный 100 baud, gap = 5ms → при baud=45.45: gap_fraction = 0.23T (bin 0 = 1.0 stop); при baud=100: gap_fraction = 0.5T (bin 1 = 1.5 stop).

### Решение

Флаг `shared_chain_stop_after_baud` (volatile bool):

1. **ui_loop.cpp** (SEARCH): если `baud_auto && stop_auto`:
   - Запускает ТОЛЬКО BAUD-DET
   - Ставит `shared_chain_stop_after_baud = true`

2. **dsp_pipeline.cpp** (main loop): проверяет chain condition:
   ```
   if (shared_chain_stop_after_baud
       && baud_detect завершён (phase=0, state=3)
       && stop_detect не активен)
     → fire STOP-DET
     → clear flag
   ```

3. **STOP-DET** запускается с корректным `baud` → измерения точные.

## 8. Auto-Inversion (Build 196+)

### Алгоритм сравнительной инверсии

```
Триггер: ERR > 12% + squelch open, устойчиво > 0.8s
  → Сохранить auto_inv_pre_err
  → Flip shared_rtty_inv
  → Очистить ERR history (чистый замер)
  → Ждать 0.8s
  → Сравнить:
    new_err < pre_err - 3% → оставить (ERR упал)
    new_err > pre_err + 3% → откатить (ERR вырос)
    иначе → откатить + inv_uncertain = true
```

`inv_uncertain` = true показывает "?" в индикаторе (NOR? / INV?). Гаснет при ERR < 5%.

## 9. Auto-Recovery (Build 217+)

Автоматическое пере-измерение при длительных ошибках:

```
if (stop_auto && baud_auto && ERR > 15% устойчиво > 3s
    && squelch open && no active detections):
  → BAUD-DET
  → auto_recovery_pending_stop = true
  → после BAUD-DET → STOP-DET (та же chain-логика)
```

Защита: ждёт 3 секунды, чтобы auto-INV получил шанс сначала.

## 10. Squelch

Логарифмический расчет SNR (в дБ) с гистерезисом:
- **Открытие:** Signal > -60 dB И SNR > tuning_sq_snr (default 6 dB)
- **Закрытие:** Signal < -65 dB ИЛИ SNR < (tuning_sq_snr - 2.0) dB
- Гистерезис 5 dB по signal и 2 dB по SNR предотвращает "дребезг" на пороге

## 11. Критические проблемы и их решения (историческая справка)

### Дрейф генератора (DPLL Integral Windup)
Разная реальная скорость передачи (74.8 vs 75.0 Бод). P-звено не справлялось. Добавлен I-звено `freq_error` (PI-регулятор).

### Красный водопад (ILI9488 DMA Quirk)
SPI дисплей ILI9488 в Mode 11 (BGR, 16-bit swapped). Решение: `color565(b, g, r)` + аппаратный swap `(c >> 8) | (c << 8)`.

### Режим LSB и Инверсия
Mark = нижняя частота, Space = верхняя (стандарт FSK/LSB).

### SEARCH cycle-leak (Build 215)
`found_current` из предыдущего теста вызывал вход в cycle path вместо full rescan. Решение: cycle path только при `found_count > 1 && found_current >= 0 && < 10s`.

### SEARCH ложные кандидаты для широких шифтов (Build 216)
850 Hz shift → 6 кандидатов из-за спектрального размытия. Решение: shift-proportional dedup tolerance.

### STOP-DET race с BAUD-DET (Build 217)
Параллельный запуск давал stale baud для gap classification. Решение: chain flag.

### STOP-DET false votes (Build 218)
Межфреймовые паузы (5-55 ms) голосовали за bin=2 (2.0 stop). DPLL warmup noise завышал первые измерения. Решение: warmup 1.5s + idle filter 1.25T + adjusted bin boundaries.
