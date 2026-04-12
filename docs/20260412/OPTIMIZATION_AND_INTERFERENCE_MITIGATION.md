# TouchRTTY — Оптимизация и помехоустойчивость

**Дата:** 2026-04-12
**Build:** g259 (Phase 3)
**Платформа:** RP2350 Pico 2, Cortex-M33 @ 300 МГц (OC), 520 КБ SRAM + 8 МБ PSRAM

---

## Оглавление

1. [Оптимизации DSP Pipeline (Core 0)](#1-оптимизации-dsp-pipeline-core-0)
2. [Помехоустойчивость RTTY](#2-помехоустойчивость-rtty)
3. [Оптимизации UI/Display (Core 1)](#3-оптимизации-uidisplay-core-1)
4. [Сводная таблица приоритетов](#4-сводная-таблица-приоритетов)
5. [Детальный анализ текущего кода](#5-детальный-анализ-текущего-кода)

---

## 1. Оптимизации DSP Pipeline (Core 0)

Текущая загрузка Core 0: 4.0–6.4% @ 300 МГц. Оптимизации освободят 2-5% CPU для будущих режимов (CW, FT8, DRM).

### 1.1. ATC: expf() вызывается на каждом сэмпле

**Файл:** `main.cpp:900-901`
**Выигрыш:** ~0.2-0.3% CPU (50-60 тактов/сэмпл)
**Сложность:** 30 мин

**Проблема:** `expf()` вычисляется 10 000 раз/сек, хотя зависит только от baud rate (меняется редко).

```cpp
// СЕЙЧАС (каждый сэмпл — 25-30 тактов каждый вызов):
atc_fast = expf(-1.0f / (2.0f * ((float)SAMPLE_RATE / baud)));
atc_slow = expf(-1.0f / (16.0f * ((float)SAMPLE_RATE / baud)));
```

**Исправление:**

```cpp
// Предвычислить при смене baud rate (один раз):
static float cached_atc_fast = 0.0f;
static float cached_atc_slow = 0.0f;
static float cached_atc_fast_inv = 0.0f;
static float cached_atc_slow_inv = 0.0f;

if (baud != current_baud) {
    cached_atc_fast = expf(-1.0f / (2.0f * (SAMPLE_RATE / baud)));
    cached_atc_slow = expf(-1.0f / (16.0f * (SAMPLE_RATE / baud)));
    cached_atc_fast_inv = 1.0f - cached_atc_fast;
    cached_atc_slow_inv = 1.0f - cached_atc_slow;
}

// В горячем цикле (1 такт вместо 25-30):
float coeff = (new_m > atc_mark_env) ? cached_atc_fast : cached_atc_slow;
float coeff_inv = (new_m > atc_mark_env) ? cached_atc_fast_inv : cached_atc_slow_inv;
atc_mark_env = atc_mark_env * coeff + new_m * coeff_inv;
```

---

### 1.2. FIR: обратная итерация + нет использования симметрии

**Файл:** `main.cpp:848-850`
**Выигрыш:** ~0.3-0.5% CPU (FIR на ~50% быстрее)
**Сложность:** 1 час

**Проблема:** 63 умножения при обратной итерации буфера (плохая кэш-локальность). FIR-фильтр симметричный — можно использовать свойство coeff[i] == coeff[62-i].

```cpp
// СЕЙЧАС (63 умножения, обратная итерация):
int bi = fi;
for(int i = 0; i < 63; i++) {
    f_out += fir_coeffs[i] * fb[bi];
    bi--;
    if(bi < 0) bi = 62;  // дорогой branch + modulo
}
fi = (fi + 1) % 63;
```

**Исправление:**

```cpp
// Используем симметрию: 63 tap → 32 умножения, прямая итерация
// Размер буфера = степень двойки для маски вместо modulo
#define FIR_BUF_SIZE 64  // следующая степень двойки после 63
#define FIR_BUF_MASK 63

static float fb[FIR_BUF_SIZE];
static int fi = 0;

fb[fi] = s;

// Центральный tap
float f_out = fir_coeffs[31] * fb[(fi - 31) & FIR_BUF_MASK];

// Симметричные пары (32 умножения вместо 63)
for(int i = 0; i < 31; i++) {
    f_out += fir_coeffs[i] * (fb[(fi - i) & FIR_BUF_MASK] +
                               fb[(fi - 62 + i) & FIR_BUF_MASK]);
}

fi = (fi + 1) & FIR_BUF_MASK;
```

**Дополнительно:** При переходе на CMSIS-DSP можно использовать `arm_fir_f32()` с аппаратной оптимизацией.

---

### 1.3. Убрать sqrtf() из горячего пути

**Файл:** `main.cpp:897-898, 235`
**Выигрыш:** ~30 тактов/сэмпл
**Сложность:** 1 час

**Проблема:** Три вызова `sqrtf()` на каждом сэмпле (~15 тактов каждый):

```cpp
float new_m = sqrtf(mark_power + 1e-10f);   // ATC mark
float new_s = sqrtf(space_power + 1e-10f);   // ATC space
rms_now = sqrtf(a->rms + 1e-10f);            // AGC
```

**Исправление:** Работать в домене мощности (power domain), без извлечения корня:

```cpp
// ATC: нормализация в домене мощности
float m_pwr = mi*mi + mq*mq;  // mark power (уже вычислено)
float s_pwr = si*si + sq*sq;  // space power
// Сравнение m_pwr vs s_pwr не требует sqrt
// Envelope tracking: atc_mark_env отслеживает power, не amplitude

// AGC: сравнивать target² вместо target
static const float target_sq = 0.30f * 0.30f;  // = 0.09
if (a->rms > target_sq) { ... }
// sqrt нужен ТОЛЬКО для отображения на экране (не 10 kHz, а 2 Hz)
```

---

### 1.4. memcpy/memmove на горячем пути

**Файл:** `main.cpp:1036-1038`
**Выигрыш:** ~0.5-1% CPU
**Сложность:** 2 часа

**Проблема:** 6 КБ memcpy + 2 КБ memmove каждые 52 мс.

```cpp
// СЕЙЧАС:
memcpy((void*)shared_fft_ts, ts, sizeof(ts));          // 4 КБ
memcpy((void*)shared_adc_waveform, tw, sizeof(tw));     // 1.92 КБ
memmove(ts, &ts[480], (FFT_SIZE-480)*sizeof(float));    // 2.18 КБ сдвиг
```

**Исправление:** Кольцевой буфер с указателем записи:

```cpp
static int ts_write_ptr = 0;

// Запись сэмпла (в горячем цикле):
ts[ts_write_ptr] = sample;
ts_write_ptr = (ts_write_ptr + 1) & (FFT_SIZE - 1);

// Для FFT: копировать с разворотом кольцевого буфера
// (только перед FFT, раз в 52 мс — не в горячем цикле 10 кГц)
for(int i = 0; i < FFT_SIZE; i++) {
    fft_input[i] = ts[(ts_write_ptr + i) & (FFT_SIZE - 1)];
}
// memmove больше не нужен
```

---

### 1.5. FFT: самописный вместо CMSIS-DSP

**Файл:** `dsp/fft.hpp`
**Выигрыш:** FFT на 30-50% быстрее
**Сложность:** 3 часа

**Проблема:** Самописный Radix-2 FFT без аппаратных оптимизаций Cortex-M33.

**Исправление:**

```cmake
# CMakeLists.txt:
target_link_libraries(TouchRTTY pico_stdlib hardware_adc ... cmsis_dsp)
```

```cpp
#include "arm_math.h"

static arm_cfft_instance_f32 fft_inst;
arm_cfft_init_f32(&fft_inst, FFT_SIZE);

// Вместо fft_compute():
arm_cfft_f32(&fft_inst, fft_buffer, 0, 1);  // in-place, forward, bit-reversal
arm_cmplx_mag_f32(fft_buffer, mag_buffer, FFT_SIZE/2);
```

**Примечание:** CMSIS-DSP использует SIMD-инструкции M33, оптимизированный bit-reversal, loop unrolling. Прямая замена.

---

### 1.6. AGC: деление вместо умножения

**Файл:** `main.cpp:237-240`
**Выигрыш:** ~5 тактов/сэмпл
**Сложность:** 15 мин

```cpp
// СЕЙЧАС:
a->gain /= release;  // деление — 10-15 тактов

// ЛУЧШЕ:
static float release_inv;  // предвычислить при инициализации
release_inv = 1.0f / release;
// В цикле:
a->gain *= release_inv;  // умножение — 3 такта
```

---

### 1.7. Twiddle factors FFT: float32 precision

**Файл:** `dsp/fft.hpp:28-30`
**Выигрыш:** Качественный (точнее FFT, лучше waterfall)
**Сложность:** 15 мин

**Проблема:** Twiddle factors вычисляются в float32. Ошибка квантования ~1e-7 компаундируется через 10 стадий FFT.

**Исправление:** Вычислять в double, хранить в float:

```cpp
// СЕЙЧАС:
tw_re[i] = cosf(2.0f * PI * i / FFT_SIZE);
tw_im[i] = -sinf(2.0f * PI * i / FFT_SIZE);

// ЛУЧШЕ:
tw_re[i] = (float)cos(2.0 * M_PI * (double)i / (double)FFT_SIZE);
tw_im[i] = (float)(-sin(2.0 * M_PI * (double)i / (double)FFT_SIZE));
```

---

### 1.8. DC removal: слишком медленная атака

**Файл:** `main.cpp:848`
**Выигрыш:** Качественный (быстрее убирает DC offset)
**Сложность:** 15 мин

```cpp
// СЕЙЧАС (tau = 100 сэмплов @ 10 кГц = 10 мс реакция):
dc = dc * 0.99f + s * 0.01f;
s -= dc;

// ЛУЧШЕ (tau = 50 сэмплов = 5 мс, быстрее адаптация):
dc = dc * 0.98f + s * 0.02f;
s -= dc;
```

---

## 2. Помехоустойчивость RTTY

### 2.1. Импульсный бланкер (Noise Blanker)

**Приоритет:** КРИТИЧНО
**Выигрыш:** Радикальное улучшение при грозовых помехах (QRN)
**Сложность:** 1 час
**CPU:** ~5 тактов/сэмпл (~0.002%)

**Проблема:** Атмосферные разряды (QRN), импульсные помехи от электроники проходят прямо в демодулятор, вызывая битовые ошибки.

**Реализация (вставить после DC removal, перед FIR):**

```cpp
// === Noise Blanker ===
static float nb_avg = 0.0f;        // среднее абсолютное значение
static float nb_peak = 0.0f;       // пиковое значение (медленное)
static int   nb_blank_count = 0;   // счётчик бланкирования
static float nb_last_good = 0.0f;  // последний "хороший" сэмпл

const float NB_ATTACK = 0.001f;    // быстрая оценка среднего
const float NB_RELEASE = 0.0001f;  // медленный возврат пика
const float NB_THRESHOLD = 6.0f;   // порог (× среднего)
const int   NB_HOLDOFF = 3;        // сэмплов бланкирования после импульса

float abs_s = fabsf(s);
nb_avg = nb_avg * (1.0f - NB_ATTACK) + abs_s * NB_ATTACK;
float threshold = nb_avg * NB_THRESHOLD;

if (abs_s > threshold && nb_avg > 0.01f) {
    // Импульс обнаружен — бланкировать
    s = nb_last_good;  // заменить на последний хороший сэмпл
    nb_blank_count = NB_HOLDOFF;
} else if (nb_blank_count > 0) {
    // Holdoff — продолжаем бланкировать
    s = nb_last_good;
    nb_blank_count--;
} else {
    nb_last_good = s;  // сохранить как "хороший"
}
```

**Альтернатива:** Медианный фильтр (3 или 5 точек) — убирает одиночные выбросы без детектора:

```cpp
// Медианный фильтр 3 точки (сортировка 3 элементов):
static float nb_z1 = 0, nb_z2 = 0;
float a = nb_z2, b = nb_z1, c = s;
float median = fmaxf(fminf(a,b), fminf(fmaxf(a,b), c));
nb_z2 = nb_z1; nb_z1 = s;
s = median;  // задержка 1 сэмпл
```

---

### 2.2. Перцентильная оценка noise floor

**Приоритет:** КРИТИЧНО
**Выигрыш:** Правильный SNR → точный сквелч
**Сложность:** 1 час
**CPU:** ~0.1% (выполняется раз в 52 мс при FFT)

**Проблема (`main.cpp:419`):** SNR считается как `peak - average`, где average включает сигнальные бины Mark и Space.

```cpp
// СЕЙЧАС:
float avg_noise = sm / (FFT_SIZE / 2);  // все бины усредняются
```

**Исправление — гистограммный метод (быстрее сортировки):**

```cpp
// Гистограмма из 64 уровней для оценки 25-го перцентиля
#define NF_BINS 64
static int nf_hist[NF_BINS];
memset(nf_hist, 0, sizeof(nf_hist));

float mag_max = 0.0f;
for(int i = 1; i < FFT_SIZE/2; i++) {
    if(mag[i] > mag_max) mag_max = mag[i];
}

float bin_width = (mag_max + 1e-6f) / NF_BINS;
for(int i = 1; i < FFT_SIZE/2; i++) {
    int b = (int)(mag[i] / bin_width);
    if(b >= NF_BINS) b = NF_BINS - 1;
    nf_hist[b]++;
}

// 25-й перцентиль
int target = (FFT_SIZE/2) / 4;  // 25%
int cumsum = 0;
float noise_floor = 0.0f;
for(int i = 0; i < NF_BINS; i++) {
    cumsum += nf_hist[i];
    if(cumsum >= target) {
        noise_floor = (i + 0.5f) * bin_width;
        break;
    }
}

// Использовать:
shared_snr_db = 10.0f * log10f((peak_mag / (noise_floor + 1e-10f)));
```

---

### 2.3. AFC: суб-бинная интерполяция

**Приоритет:** Высокий
**Выигрыш:** Разрешение AFC ~0.5 Гц вместо 10 Гц, меньше рыскания DPLL
**Сложность:** 1 час
**CPU:** незначительный (раз в 52 мс)

**Проблема (`main.cpp:430-435`):** Частота определяется как целый номер FFT-bin = 9.77 Гц разрешение.

**Исправление — параболическая интерполяция:**

```cpp
// После нахождения пикового bin (best_m_bin):
int pk = best_m_bin;
if (pk > 0 && pk < FFT_SIZE/2 - 1) {
    float a = mag[pk - 1];
    float b = mag[pk];
    float c = mag[pk + 1];
    float denom = a - 2.0f * b + c;
    float delta = 0.0f;
    if (fabsf(denom) > 1e-10f) {
        delta = 0.5f * (a - c) / denom;  // суб-бинное смещение [-0.5, +0.5]
    }
    float precise_bin = (float)pk + delta;
    float precise_freq = precise_bin * ((float)SAMPLE_RATE / FFT_SIZE);
    // Использовать precise_freq вместо pk * bin_hz
}
```

Аналогично для Space-тона (best_s_bin).

---

### 2.4. DPLL: адаптивная полоса

**Приоритет:** Высокий
**Выигрыш:** +1-2 дБ при QSB (замирание), быстрый захват + стабильное слежение
**Сложность:** 2 часа

**Проблема (`main.cpp:913-914`):** `alpha = 0.035` фиксирован. При захвате нужна широкая полоса, при слежении — узкая.

**Исправление:**

```cpp
// Критерий захвата: |phase_error| < 0.03 в течение lock_count символов
static int lock_counter = 0;
static bool dpll_locked = false;

const float ALPHA_ACQUIRE = 0.08f;   // широкая полоса для захвата
const float ALPHA_TRACK   = 0.020f;  // узкая полоса для слежения
const int   LOCK_THRESHOLD = 10;     // символов для подтверждения lock

if (fabsf(phase_error) < 0.03f) {
    lock_counter++;
    if (lock_counter > LOCK_THRESHOLD) {
        dpll_locked = true;
    }
} else {
    lock_counter = 0;
    if (fabsf(phase_error) > 0.15f) {
        dpll_locked = false;  // потеря захвата
    }
}

float alpha = dpll_locked ? ALPHA_TRACK : ALPHA_ACQUIRE;
float beta = alpha * alpha / 2.0f;

// Плавный переход (опционально):
// static float alpha_smooth = ALPHA_ACQUIRE;
// alpha_smooth = alpha_smooth * 0.95f + alpha * 0.05f;
```

---

### 2.5. Matched filter вместо integrate-and-dump

**Приоритет:** Высокий
**Выигрыш:** +1-3 дБ SNR (оптимальный приём при AWGN)
**Сложность:** 2 часа
**CPU:** ~0.1% (предвычисленные коэффициенты)

**Проблема (`main.cpp:945-946`):** Простое усреднение сигнала за символ — субоптимально.

```cpp
// СЕЙЧАС:
integrate_acc += D;  // равновесное усреднение всех сэмплов в символе
```

**Исправление — raised cosine matched filter:**

```cpp
// Предвычислить при смене baud (один раз):
static float matched_filter[256];  // макс. samples_per_symbol
static int matched_filter_len = 0;

void compute_matched_filter(float baud, int sample_rate) {
    int sps = (int)(sample_rate / baud);
    matched_filter_len = sps;
    for(int i = 0; i < sps; i++) {
        float t = (float)i / sps - 0.5f;
        matched_filter[i] = 0.5f + 0.5f * cosf(2.0f * PI * t);  // Hann window
        // Альтернатива: raised cosine
        // matched_filter[i] = cosf(PI * t * rolloff);
    }
    // Нормализация:
    float sum = 0;
    for(int i = 0; i < sps; i++) sum += matched_filter[i];
    for(int i = 0; i < sps; i++) matched_filter[i] /= sum;
}

// В горячем цикле:
static int sample_in_symbol = 0;
integrate_acc += D * matched_filter[sample_in_symbol];
sample_in_symbol++;
if (symbol_phase >= 1.0f) {
    // Решение по integrate_acc
    sample_in_symbol = 0;
    integrate_acc = 0.0f;
}
```

**Почему это лучше:** Matched filter максимизирует SNR на выходе по теореме Неймана-Пирсона. Сэмплы в центре символа получают больший вес, сэмплы на границах (где inter-symbol interference) — меньший.

---

### 2.6. Diversity combining (soft decision)

**Приоритет:** Высокий
**Выигрыш:** +2-4 дБ при селективном фединге
**Сложность:** 2 часа

**Проблема:** Решение Mark/Space принимается жёстко: `mark_power > space_power`. При селективном фединге один тон может временно пропасть, а жёсткое решение даёт 100% ошибку.

**Исправление — ratio-based soft decision:**

```cpp
// Вместо жёсткого бинарного решения:
float m_pwr = mi*mi + mq*mq;
float s_pwr = si*si + sq*sq;
float total = m_pwr + s_pwr + 1e-10f;

// Soft decision в диапазоне [-1, +1]:
//   +1 = уверенный Mark
//    0 = неопределённость
//   -1 = уверенный Space
float soft_bit = (m_pwr - s_pwr) / total;

// Подавать в integrate-and-dump / matched filter:
integrate_acc += soft_bit * matched_filter[sample_in_symbol];

// Для DPLL zero-crossing — использовать знак soft_bit:
bool d_sign = (soft_bit > 0);

// Для framer — решение по знаку accumulated soft_bit:
bool bit = (integrate_acc > 0.0f);
```

**Почему это лучше:** При равной мощности Mark и Space (фединг на один тон) soft_bit → 0, и интегратор не принимает неверное решение, а "ждёт" подтверждения от следующих сэмплов.

---

### 2.7. Спектральное вычитание шума

**Приоритет:** Высокий
**Выигрыш:** +3-5 дБ эффективного SNR
**Сложность:** 2 часа
**CPU:** ~2% Core 0 (выполняется при каждом FFT)

**Реализация:**

```cpp
// === Spectral Subtraction ===
static float noise_profile[FFT_SIZE / 2];
static bool noise_profile_valid = false;
static int noise_update_count = 0;

const float NOISE_LEARN_RATE = 0.05f;    // скорость обучения
const float OVERSUBTRACTION = 2.0f;       // агрессивность вычитания
const float SPECTRAL_FLOOR = 0.05f;       // минимум (не обнулять полностью)

// 1. Обновление шумового профиля (в паузах):
if (!shared_squelch_open) {
    for(int i = 0; i < FFT_SIZE/2; i++) {
        noise_profile[i] = noise_profile[i] * (1.0f - NOISE_LEARN_RATE)
                         + mag[i] * NOISE_LEARN_RATE;
    }
    noise_update_count++;
    if (noise_update_count > 20) noise_profile_valid = true;  // ~1 сек обучения
}

// 2. Вычитание шума при приёме:
if (noise_profile_valid && shared_squelch_open) {
    for(int i = 0; i < FFT_SIZE/2; i++) {
        float clean = mag[i] - noise_profile[i] * OVERSUBTRACTION;
        mag[i] = fmaxf(clean, mag[i] * SPECTRAL_FLOOR);
    }
}
```

**Примечание:** Спектральное вычитание применяется к FFT-магнитудам. Для более глубокого подавления можно применять к I/Q сигналу через overlap-add метод (требует больше CPU).

---

### 2.8. Адаптивный notch filter (подавление бёрди)

**Приоритет:** Средний
**Выигрыш:** Удаление мешающей несущей без влияния на полезный сигнал
**Сложность:** 3 часа
**CPU:** ~0.5% (1 biquad = 5 умножений/сэмпл)

**Реализация:**

```cpp
// === Automatic Notch Filter ===
static Biquad notch_filter;
static bool notch_active = false;
static float notch_freq = 0.0f;

// Обнаружение (в FFT-обработке, раз в 52 мс):
// Искать тон >15 дБ над шумом, не совпадающий с Mark/Space ±20 Гц
for(int i = 1; i < FFT_SIZE/2 - 1; i++) {
    float excess = mag[i] - noise_floor * 30.0f;  // >15 дБ над шумом
    if (excess > 0) {
        float freq = i * ((float)SAMPLE_RATE / FFT_SIZE);
        float dist_m = fabsf(freq - mark_freq);
        float dist_s = fabsf(freq - space_freq);
        if (dist_m > 20.0f && dist_s > 20.0f) {
            // Это бёрди — включить notch
            notch_freq = freq;
            notch_active = true;
            design_notch(&notch_filter, notch_freq, SAMPLE_RATE, 30.0f);
            break;
        }
    }
}

// В горячем цикле (после DC removal, перед FIR):
if (notch_active) {
    s = process_biquad(&notch_filter, s);
}
```

**Дополнительно:** Нужна функция `design_notch()` — аналогична `design_lpf()` но с notch-топологией:

```cpp
void design_notch(Biquad* f, float fc, float fs, float Q) {
    float w0 = 2.0f * PI * fc / fs;
    float alpha = sinf(w0) / (2.0f * Q);
    float cosw0 = cosf(w0);
    float A0 = 1.0f + alpha;
    f->b0 = 1.0f / A0;
    f->b1 = -2.0f * cosw0 / A0;
    f->b2 = 1.0f / A0;
    f->a1 = -2.0f * cosw0 / A0;
    f->a2 = (1.0f - alpha) / A0;
    f->z1 = f->z2 = 0.0f;
}
```

---

### 2.9. Контекстная коррекция ITA2

**Приоритет:** Средний
**Выигрыш:** Снижение видимого BER в 2-5 раз для осмысленного текста
**Сложность:** 4 часа
**CPU:** незначительный (выполняется на уровне символов, 45-100 раз/сек)

**Метод 1 — Валидация ITA2:**

```cpp
// Фильтрация заведомо невалидных последовательностей:
bool validate_ita2_char(uint8_t code, bool is_figs, uint8_t prev_code) {
    // Двойной FIGS/LTRS подряд — второй лишний
    if (code == 27 && prev_code == 27) return false;  // FIGS+FIGS
    if (code == 31 && prev_code == 31) return false;  // LTRS+LTRS

    // NULL (0x00) в середине текста — вероятно ошибка
    if (code == 0 && prev_code != 0) return false;

    // В режиме FIGS: коды без назначения
    if (is_figs) {
        char c = ita2_figs[code];
        if (c == 0 && code != 0) return false;  // нет символа для этого кода
    }
    return true;
}
```

**Метод 2 — Мажоритарное голосование для повторных передач (DWD):**

```cpp
// DWD передаёт каждое сообщение 2-3 раза
// Накапливать строки, при обнаружении повтора — голосовать

struct LineBuffer {
    char lines[8][80];   // последние 8 строк
    float conf[8][80];   // soft-bit confidence per char
    int count;
};

// При получении новой строки:
// 1. Сравнить с предыдущими (Levenshtein distance < 3 = повтор)
// 2. Посимвольное голосование: символ с наибольшей confidence побеждает
```

---

### 2.10. Stop bit: улучшенная валидация

**Приоритет:** Средний
**Выигрыш:** Лучшая синхронизация при 1.5/2.0 стоп-битах
**Сложность:** 1 час

**Проблема (`main.cpp:1022-1029`):** Для 1.5/2.0 стоп-битов валидация упрощена.

**Исправление:**

```cpp
// Для 1.5 стоп-бит: проверять интеграл за 1.5 символьных периода
// Для 2.0 стоп-бит: проверять интеграл за 2.0 периода
if (baudot_state == 7) {
    stop_accumulator += D;
    stop_samples++;

    float stop_progress = (float)stop_samples / samples_per_stop;
    if (stop_progress >= 1.0f) {
        float stop_quality = stop_accumulator / stop_samples;
        if (stop_quality > -0.3f) {
            // Валидный стоп-бит (порог с запасом для шума)
            // Перейти к следующему символу
        } else {
            // Framing error — сбросить
            shared_framing_errors++;
        }
    }
}
```

---

## 3. Оптимизации UI/Display (Core 1)

### 3.1. Блокирующий DMA → Ping-Pong

**Файл:** `ili9488_driver.c:118,143,179`
**Выигрыш:** -30-40% нагрузки Core 1
**Сложность:** 3 часа

**Проблема:** `dma_channel_wait_for_finish_blocking()` на каждой строке = Core 1 простаивает ~12.8 мс на водопад (64 строки × 200 мкс).

**Исправление:**

```c
// Двойная буферизация с перекрытием подготовки и передачи:
static uint32_t buf[2][480];
static int current_buf = 0;

for(int row = 0; row < h; row++) {
    // Запустить DMA для текущей строки:
    dma_channel_configure(dma_chan, &dma_conf,
        &pio_inst->txf[pio_sm], buf[current_buf], w, true);

    // Пока DMA передаёт — готовить следующую строку в другом буфере:
    current_buf ^= 1;
    if (row + 1 < h) {
        prepare_row(buf[current_buf], data, row + 1, w);
    }

    // Ждать завершения DMA только если следующая строка готова:
    dma_channel_wait_for_finish_blocking(dma_chan);
}
```

**Эффект:** Подготовка строки (expand_color_dynamic × 480 = ~2 мкс) происходит параллельно с DMA-передачей (~200 мкс). CPU-время сокращается на ~95% в цикле.

---

### 3.2. Инкрементальный рендеринг текста

**Файл:** `UIManager.hpp:1004-1045`
**Выигрыш:** В 10 раз меньше пикселей при обновлении текста
**Сложность:** 2 часа

**Проблема:** Каждый новый символ RTTY → `drawRTTY(true)` → перерисовка всего спрайта 480×160 пикселей.

**Исправление:**

```cpp
void drawRTTY(bool force_full) {
    if (force_full || font_changed) {
        // Полная перерисовка — только при смене шрифта или init
        _spr_text.fillSprite(COLOR_BG);
        redraw_all_lines();
        ili9488_push_colors(0, UI_Y_TEXT, 480, UI_TEXT_ZONE_H,
            (uint16_t*)_spr_text.getBuffer());
    } else if (new_line_added) {
        // Скроллинг: сдвинуть спрайт вверх на line_h, нарисовать только новую строку
        _spr_text.scroll(0, -line_h);
        int y = UI_TEXT_ZONE_H - line_h;
        _spr_text.fillRect(0, y, 480, line_h, COLOR_BG);
        drawTextLine(&_spr_text, fid, current_line.c_str(), 5, y, ...);
        ili9488_push_colors(0, UI_Y_TEXT, 480, UI_TEXT_ZONE_H,
            (uint16_t*)_spr_text.getBuffer());
    } else if (chars_added_to_current_line) {
        // Только перерисовать текущую строку:
        int y = current_line_y;
        _spr_text.fillRect(0, y, 480, line_h, COLOR_BG);
        drawTextLine(&_spr_text, fid, current_line.c_str(), 5, y, ...);
        // Push только изменённую полосу (480 × line_h вместо 480 × 160):
        ili9488_push_colors(0, UI_Y_TEXT + y, 480, line_h,
            (uint16_t*)_spr_text.getBuffer() + y * 480);
    }
}
```

---

### 3.3. Color expansion: LUT вместо per-pixel вычисления

**Файл:** `ili9488_driver.c:30-37`
**Выигрыш:** ~2 мс на водопад
**Сложность:** 1 час

**Проблема:** `expand_color_dynamic()` вызывается на каждый пиксель (30K+ раз на водопад).

**Исправление — LUT для waterfall palette (256 записей):**

```c
// Предвычислить при инициализации:
static uint32_t wf_color_lut[256];  // 1 КБ

void init_waterfall_lut(const uint16_t* palette) {
    for(int i = 0; i < 256; i++) {
        wf_color_lut[i] = expand_color_dynamic(palette[i]);
    }
}

// В ili9488_push_waterfall_lut — прямой lookup вместо вычисления:
for(int col = 0; col < w; col++) {
    buf[current_buf][col] = wf_color_lut[lut_data[row * w + col]];
}
```

---

### 3.4. Sprite memory: уменьшить глубину цвета

**Файл:** `main.cpp:327-328`, `UIManager.hpp:541-543`
**Выигрыш:** ~150 КБ RAM
**Сложность:** 2 часа

**Проблема:** Все спрайты используют 16-bit color depth. Текстовый спрайт 480×160 = 153.6 КБ.

**Вариант:** Для текстового спрайта (2-3 цвета: фон, текст, курсор) использовать 4-bit или 8-bit:

```cpp
_spr_text.setColorDepth(8);  // 8-bit = 76.8 КБ вместо 153.6 КБ
// или:
_spr_text.setColorDepth(4);  // 4-bit = 38.4 КБ (16 цветов достаточно)
```

**Экономия:** 76-115 КБ RAM → больше места для PSRAM-буферов или будущих декодеров.

---

## 4. Сводная таблица приоритетов

### Приоритет 1 — Критично (реализовать первыми)

| # | Оптимизация | Тип | Выигрыш | Время |
|---|---|---|---|---|
| 2.1 | **Импульсный бланкер** | Помехоустойч. | Убирает QRN | 1 ч |
| 2.2 | **Noise floor перцентильный** | Помехоустойч. | Правильный SNR/сквелч | 1 ч |
| 1.1 | **ATC: кэш expf()** | CPU | 0.2% CPU | 30 мин |
| 1.2 | **FIR: симметрия + буфер** | CPU | 0.3-0.5% CPU | 1 ч |
| 2.3 | **AFC суб-бинная** | Качество | Разрешение 0.5 Гц | 1 ч |

### Приоритет 2 — Высокий

| # | Оптимизация | Тип | Выигрыш | Время |
|---|---|---|---|---|
| 2.4 | **DPLL адаптивная полоса** | Помехоустойч. | +1-2 дБ при QSB | 2 ч |
| 2.5 | **Matched filter** | Помехоустойч. | +1-3 дБ SNR | 2 ч |
| 2.6 | **Soft decision (ratio)** | Помехоустойч. | +2-4 дБ при фединге | 2 ч |
| 2.7 | **Спектральное вычитание** | Помехоустойч. | +3-5 дБ | 2 ч |
| 1.3 | **Убрать sqrtf()** | CPU | 30 тактов/сэмпл | 1 ч |
| 2.8 | **Adaptive notch** | Помехоустойч. | Убирает бёрди | 3 ч |

### Приоритет 3 — Средний

| # | Оптимизация | Тип | Выигрыш | Время |
|---|---|---|---|---|
| 3.1 | **Ping-pong DMA display** | Core 1 | -30-40% UI | 3 ч |
| 3.2 | **Инкрементальный рендер** | Core 1 | -90% пикселей текста | 2 ч |
| 1.4 | **Кольцевой FFT буфер** | CPU | 0.5-1% CPU | 2 ч |
| 1.5 | **CMSIS-DSP FFT** | CPU | FFT на 30-50% быстрее | 3 ч |
| 2.9 | **Контекстная коррекция ITA2** | Помехоустойч. | BER ÷2-5 | 4 ч |
| 2.10 | **Stop bit валидация** | Качество | Лучшая синхр. 1.5/2.0 | 1 ч |
| 3.3 | **Color expansion LUT** | Core 1 | -2 мс на водопад | 1 ч |
| 3.4 | **Sprite color depth** | RAM | -76-115 КБ | 2 ч |

---

## 5. Детальный анализ текущего кода

### 5.1. Найденные проблемы (bugs / correctness)

| Файл | Строка | Проблема | Серьёзность |
|---|---|---|---|
| `main.cpp` | 848 | DC removal слишком медленный (tau=100 сэмплов, 10 мс) — DC offset проходит в FIR | Средняя |
| `main.cpp` | 419 | SNR = peak − average (не настоящий SNR, включает сигнальные бины) | Средняя |
| `main.cpp` | 900-901 | expf() на горячем пути 10 кГц — пустая трата CPU | Средняя |
| `main.cpp` | 413 | signal_db корректируется на AGC gain, но AGC уже применён к сигналу (двойной учёт) | Низкая |
| `dsp/fft.hpp` | 28-30 | Twiddle factors в float32 — кумулятивная ошибка через 10 стадий FFT | Низкая |
| `main.cpp` | 237-240 | AGC: деление вместо умножения на release (медленнее) | Низкая |
| `main.cpp` | 445 | AFC lock condition: жёсткий порог 2.0 дБ (не адаптивный) | Низкая |

### 5.2. Отсутствующие DSP-техники для RTTY

| Техника | Текущий статус | Потенциал | Приоритет |
|---|---|---|---|
| Импульсный бланкер | ❌ Отсутствует | Радикально при QRN | **Критичный** |
| Спектральное вычитание | ❌ Отсутствует | +3-5 дБ SNR | Высокий |
| Matched filter | ❌ Integrate-and-dump | +1-3 дБ SNR | Высокий |
| Soft-bit решение | ❌ Жёсткий порог | +2-4 дБ при фединге | Высокий |
| Adaptive notch | ❌ Отсутствует | Убирает бёрди | Высокий |
| Адаптивная DPLL полоса | ❌ Фиксированная | +1-2 дБ при QSB | Высокий |
| Суб-бинная AFC | ❌ Целый bin | Меньше рыскания | Высокий |
| Перцентильный noise floor | ❌ Простое среднее | Правильный SNR | Критичный |
| Контекстная ITA2 коррекция | ❌ Отсутствует | BER ÷2-5 | Средний |
| Costas loop (фазовое) | ❌ Отсутствует | +1-2 дБ | Средний |
| Decision-directed equalizer | ❌ Отсутствует | Для многолучёвости | Низкий |
| Viterbi soft decoding | ❌ Отсутствует | +3-4 дБ (нет FEC в Baudot) | Не применимо |

### 5.3. Оценка суммарного выигрыша

**CPU (Core 0):**
- Текущая загрузка: 4.0-6.4%
- После оптимизаций 1.1-1.6: **2.5-4.0%** (экономия ~2% CPU)
- Экономия = запас для CW, FT8, DRM, шумоподавления

**Помехоустойчивость RTTY:**
- Текущий порог декодирования: ~SNR +5 дБ
- После внедрения бланкера + спектр. вычитания + matched filter + soft decision:
  - **Порог: ~SNR -3...-5 дБ** (улучшение на 8-10 дБ)
  - При чистом сигнале: BER снижается в 5-10 раз

**Core 1 (UI):**
- Текущая пиковая загрузка: 101%
- После ping-pong DMA + инкрементальный рендер: **~50-60%**
- Экономия = запас для GUI FT8/DRM экранов

---

*Документ создан на основе анализа исходного кода TouchRTTY build g259. Все номера строк соответствуют текущей версии.*
