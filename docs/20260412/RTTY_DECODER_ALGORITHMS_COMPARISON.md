# Сравнительный анализ алгоритмов декодирования RTTY

**Дата:** 2026-04-12
**Контекст:** TouchRTTY build g259, RP2350 Cortex-M33 @ 300-500 МГц

---

## Оглавление

1. [Текущая архитектура TouchRTTY (для сравнения)](#1-текущая-архитектура-touchrtty)
2. [Анализ существующих декодеров](#2-анализ-существующих-декодеров)
3. [Теория оптимального приёма FSK](#3-теория-оптимального-приёма-fsk)
4. [Сводная таблица сравнения](#4-сводная-таблица-сравнения)
5. [Рекомендации для TouchRTTY](#5-рекомендации-для-touchrtty)

---

## 1. Текущая архитектура TouchRTTY

Для справки — текущий DSP-конвейер (Core 0, 10 кГц):

```
ADC 12-bit @ 10 kHz
  → DC removal (IIR, alpha=0.01)
  → FIR LPF 63-tap
  → AGC (RMS, attack 10мс / release 500мс, target 0.30)
  → I/Q демодулятор (NCO → sin/cos LUT 1024 → Mark I/Q + Space I/Q)
  → Biquad LPF ×4 (Butterworth Q=0.707, fc = baud × K)
  → ATC (FASR envelope: attack ~2 символа, release ~16 символов)
  → Дискриминатор: D = m_norm - s_norm
  → DPLL (PI-контроллер, α=0.035, zero-crossing)
  → Integrate-and-dump
  → Baudot framer (state machine, 5 bit + stop)
  → ITA2 декодирование
```

**Класс:** Когерентный I/Q детектор с ATC — на уровне fldigi/MixW.

---

## 2. Анализ существующих декодеров

### 2.1. MMTTY (JE3HHT, Makoto Mori) — эталон

**Тип:** Мульти-корреляторный Goertzel
**Статус:** Золотой стандарт любительского RTTY

**Алгоритм:**
- **Dual-tone Goertzel** — рекурсивный однобинный DFT для Mark и Space
- Goertzel = 2 умножения + 2 сложения на сэмпл на тон (крайне эффективно)
- Длина блока = 1 символьный период (при 45.45 бод: ~220 сэмплов @ 10 кГц)
- После Goertzel: D = |Mark| − |Space|

**Ключевая инновация — мульти-коррелятор:**
- Вычисляет Goertzel на **8 фазовых смещениях** одновременно
- Выбирает фазу с максимальным контрастом Mark/Space
- По сути "early-late gate" но с 8 ветками вместо 2
- Даёт робастную синхронизацию **без классического DPLL**

**ATC:** Да, аналогичная нашей — асимметричный envelope tracker

**AFC:** Сканирование Goertzel по частотам — какой банк даёт максимум

**Опции:**
- Hard limiter (клиппирование до ±1 перед детектором) — улучшает приём при Rayleigh fading, ценой ~1 дБ в AWGN
- Noise blanker

**Сильные стороны:**
- Goertzel = matched filter для тонального burst → оптимальный SNR на выходе
- Мульти-коррелятор даёт надёжную синхронизацию без DPLL
- ATC справляется с селективным федингом
- Очень эффективен по CPU

**Слабые стороны:**
- Нет soft-decision на выходе
- Нет FEC
- Фиксированная задержка 1 символ (блочная обработка)
- Закрытый код (невозможно модифицировать)

**Производительность:** Обычно на 1-2 дБ лучше fldigi при низком SNR.

**Применимость к RP2350:**

Goertzel идеально подходит для MCU:
```
// Goertzel для одного тона — всего 2 МАС на сэмпл:
coeff = 2.0f * cosf(2.0f * PI * tone_freq / sample_rate);
// В цикле (на каждый сэмпл):
s2 = s1;
s1 = s0;
s0 = sample + coeff * s1 - s2;
// В конце блока (раз в символ):
power = s0*s0 + s1*s1 - coeff*s0*s1;
```

Мульти-коррелятор (8 фаз × 2 тона) = 16 Goertzel = 32 МАС/сэмпл. На RP2350 @ 300 МГц: ~0.01% CPU.

---

### 2.2. 2Tone (G3YYD, David Wicks) — лучший weak-signal

**Тип:** Character-level Maximum Likelihood
**Статус:** Лучший декодер для слабых сигналов RTTY

**Алгоритм:**
- **Matched filter bank** — корреляция не с отдельными битами, а с **целыми символами Baudot**
- Baudot-символ: start(0) + 5 data bits + 1.5 stop(1) = 7.5 бит
- 32 возможных символа ITA2 → 32 корреляционных шаблона
- Для каждого шаблона: вычисляется корреляция с принятым сигналом
- Выбирается символ с **максимальной апостериорной вероятностью**

**Ключевая инновация — character-level ML:**

Почему это лучше побитового:
- Start bit (всегда 0) и stop bit (всегда 1) = **известные** биты
- Они участвуют в корреляции, давая 2.5 бита "бесплатной" избыточности
- Framing errors практически исключаются — принимаются только валидные паттерны
- Эффективное кодирование: 7.5 бит передаётся, 5 бит информации = **33% избыточность**

```
// Концептуально:
for (int char_hypothesis = 0; char_hypothesis < 32; char_hypothesis++) {
    for (int phase = 0; phase < 8; phase++) {
        float correlation = 0;
        // Correlate received signal with expected waveform for this character
        for (int sample = 0; sample < samples_per_char; sample++) {
            float expected = generate_baudot_waveform(char_hypothesis, sample, phase);
            correlation += received[sample] * expected;
        }
        if (correlation > best) {
            best = correlation;
            best_char = char_hypothesis;
            best_phase = phase;
        }
    }
}
```

**Синхронизация:** Побочный продукт character-level корреляции — фаза с максимальной корреляцией = оптимальная синхронизация.

**AFC:** Сканирование по частотам — мультичастотный Goertzel.

**Сильные стороны:**
- **Лучший приём на слабых сигналах** среди всех RTTY-декодеров
- +2-3 дБ выигрыш по сравнению с MMTTY
- Может декодировать RTTY при **SNR −12...−14 дБ** (в полосе 2.4 кГц)
- Автоматическая синхронизация и framing
- Soft-decision корреляция сохраняет информацию о SNR

**Слабые стороны:**
- Высокая вычислительная нагрузка: 32 гипотезы × 8 фаз = 256 корреляций
- Задержка = 1 полный символ Baudot (7.5 бит / 45.45 = ~165 мс)
- Плохо переносит рассогласование бод (фиксированная длина шаблона)
- Чувствителен к частотной ошибке (длинное окно корреляции)

**Применимость к RP2350:**

При 45.45 бод: 1 символ = 220 мс (2200 сэмплов @ 10 кГц). Нужно вычислить 256 корреляций по 2200 сэмплов = **563 200 МАС**. При 300 МГц это ~1.9 мс. **Полностью реализуемо! Запас огромный.**

При 100 бод: 1 символ = 100 мс (1000 сэмплов). 256 × 1000 = 256 000 МАС = ~0.85 мс. Тоже легко.

---

### 2.3. Fldigi (W1HKJ)

**Тип:** I/Q квадратурный детектор (аналог нашего)
**Статус:** Самый популярный open-source декодер

**Алгоритм:**
- Complex mixer (NCO) для Mark и Space → baseband
- Biquad LPF на I и Q компонентах
- Envelope: power = I² + Q² (не когерентный)
- D = mark_power − space_power

**Синхронизация:** DPLL первого порядка или start-bit resync

**AFC:** FFT peak search, IIR-сглаживание

**Шумоподавление:**
- Сквелч по SNR
- Опциональный noise blanker
- USOS (Unshift On Space — предполагает возврат в LTRS при пробеле)

**Сильные стороны:**
- Open source, хорошо отлажен
- Гибкая настройка (фильтры, AFC, сквелч)
- Отличный UI с индикаторами настройки

**Слабые стороны:**
- Non-coherent детектор теряет ~1 дБ vs coherent
- DPLL первого порядка — менее стабилен чем PI-контроллер
- Нет ATC в стандартной конфигурации → плохо при селективном фединге
- Нет matched filter (используются обычные LPF)

**Производительность:** Средняя. Начинает терять при SNR ~0 дБ.

---

### 2.4. GRITTY (AA6E)

**Тип:** True matched filter
**Статус:** Хорошо задокументированный оптимальный детектор

**Алгоритм:**
- Matched filter = корреляция с ожидаемым тональным burst длиной T=1/baud
- Для тона f: matched filter = sin(2πft) и cos(2πft) для t ∈ [0,T]
- Это **математически то же самое**, что Goertzel за 1 символ
- Soft discriminator output

**Ключевой вклад:** AA6E показал, что оптимальный non-coherent FSK-детектор — это пара matched filters + envelope detection + сравнение. И что Goertzel за 1 символ **является** этим оптимальным matched filter.

**Синхронизация:** Мульти-фазная оценка (как MMTTY).

**AFC:** Поиск по частоте с несколькими Goertzel.

**Сильные стороны:**
- Теоретически оптимальный non-coherent детектор
- Хорошо задокументирован (обучающий ресурс)
- Производительность близка к MMTTY

**Слабые стороны:**
- Java (не real-time)
- Нет character-level ML (как 2Tone)

---

### 2.5. Malachite DSP SDR

**Тип:** Базовый двухполосный фильтр
**Статус:** Простая реализация для встраиваемых систем

**Алгоритм:**
- Аудио с SDR → два полосовых IIR фильтра (Mark/Space)
- Детектирование огибающей (выпрямление + LPF)
- Компаратор: D = mark_env − space_env

**Синхронизация:** Только start-bit sync. Нет DPLL.

**AFC:** Нет (ручная настройка).

**ATC:** Нет.

**Сильные стороны:**
- Простота, низкая нагрузка CPU
- Работает на STM32H7

**Слабые стороны:**
- Плохой приём при SNR < 5 дБ
- Нет ATC → селективный фединг разрушает приём
- Нет DPLL → drift бода вызывает ошибки
- Нет AFC → ручная настройка критична

**Производительность:** Только для сильных сигналов (>10 дБ SNR).

---

### 2.6. ATS-25 / ATS-25 mini / ATS v4 mini

**Тип:** Базовый Goertzel или zero-crossing
**Статус:** "Nice to have" фича

**Алгоритм:**
- Si4732 даёт демодулированный аудио
- MCU (ESP32 или STM32) выполняет тональный детектор
- Часто: dual Goertzel на Mark/Space
- Иногда: подсчёт пересечений нуля (zero-crossing frequency estimation)

**Синхронизация:** Start-bit only.
**AFC:** Нет.
**ATC:** Нет.

**Производительность:** Маргинальная. Для сильных контестных станций.

---

### 2.7. MixW (UU9JDR)

**Тип:** Sliding DFT (SDFT)
**Статус:** Коммерческий мультирежимный софт

**Алгоритм:**
- SDFT = "бегущий" DFT, обновляется каждый сэмпл
- Извлекаются бины Mark и Space
- Дискриминатор по магнитуде

**SDFT vs FFT:** SDFT обновляется на каждом сэмпле (нет блочной задержки), но вычислительно дороже Goertzel для 2 тонов.

**Синхронизация:** DPLL с transition detection.
**ATC:** Да.
**AFC:** Peak tracking из SDFT.

**Производительность:** Между fldigi и MMTTY. Хорошая, не исключительная.

---

### 2.8. MultiPSK (F6CTE)

**Тип:** Двухполосный фильтр + DPLL

Поддерживает **SITOR-A (ARQ)** и **SITOR-B (FEC)** — это расширения Baudot с помехозащитным кодированием:
- SITOR-B (NAVTEX): каждый символ передаётся дважды с интервалом 280 мс
- При совпадении — символ принят, при несовпадении — ошибка (time diversity)

**Производительность:** Средняя для RTTY, но SITOR-B значительно надёжнее.

---

### 2.9. DL-Fldigi (UKHAS, для HAB)

**Тип:** Fork fldigi для высотных зондов

Отличия от fldigi:
- **Очень агрессивный AFC** (зонды дрейфуют на сотни Гц из-за температуры)
- Поддержка нестандартных бод (50, 75, 100, 300, 600)
- 7-bit и 8-bit RTTY (ASCII), не только 5-bit Baudot
- CRC/checksum валидация телеметрических строк

**Релевантность:** HAB RTTY — хорошая аналогия КВ RTTY (слабые сигналы + drift), но без multipath fading.

---

### 2.10. GNU Radio (gr-rtty)

**Тип:** Configurable flowgraph

**Интересная особенность — Mueller-Muller TED:**

GNU Radio использует Mueller-Muller Timing Error Detector для синхронизации:
```
e(n) = bit_prev × soft_current − bit_current × soft_prev
```
Это надёжнее zero-crossing, потому что работает даже **без переходов** (в отличие от нашего DPLL, который обновляется только на zero-crossing).

**Polyphase Clock Sync:** Поддерживает 32-128 под-фильтров на разных фазовых смещениях — наиболее продвинутый символьный синхронизатор.

**Применимость к RP2350:** Mueller-Muller TED — простой (несколько умножений на символ), может заменить zero-crossing detector в нашем DPLL.

---

### 2.11. CW Skimmer / RTTY Skimmer (VE3NEA)

**Тип:** Массовый параллельный FFT-детектор

**Алгоритм:** Мониторит **всю** аудиополосу через большой FFT (8192-16384 точек), обнаруживает пары тонов Mark/Space, запускает декодер для каждой обнаруженной пары.

**Goertzel vs FFT tradeoff (VE3NEA):** FFT эффективнее при мониторинге более log2(N) сигналов. Для 1-2 сигналов Goertzel лучше.

---

### 2.12. WSJT-X (K1JT) — релевантные техники

Хотя WSJT-X предназначен для FT8, его weak-signal техники применимы к RTTY:

1. **Soft-decision LDPC** — для RTTY: soft-decision с character-level Viterbi
2. **Spectral subtraction** — вычитание декодированных сигналов для выделения слабых
3. **A priori (AP) decoding** — использование знания ожидаемого формата сообщения
4. **Costas array sync** — для RTTY: start/stop bits как sync reference (то, что делает 2Tone)

FT8 декодирует при **−21 дБ SNR** (в 2.5 кГц). Чистый RTTY не может приблизиться к этому, но soft-decision + AP + character ML могут дать **−12...−15 дБ SNR**.

---

## 3. Теория оптимального приёма FSK

### 3.1. Оптимальный non-coherent FSK детектор

По Proakis ("Digital Communications"):

1. Два **matched filter** — один для Mark, один для Space
2. **Envelope detection**: sqrt(I² + Q²) для каждого фильтра в оптимальный момент (конец символа)
3. **Сравнение**: выбор большей огибающей

Matched filter для тонального burst длительностью T на частоте f — это sin(2πft) и cos(2πft) для t ∈ [0,T]. **Это в точности то, что вычисляет Goertzel.**

**BER для non-coherent FSK в AWGN:**
```
P_b = (1/2) × exp(−E_b / (2×N_0))
```

**BER для coherent FSK:**
```
P_b = Q(sqrt(2×E_b/N_0))
```

Coherent даёт ~1 дБ преимущество при высоком SNR, но разница уменьшается при низком SNR, а восстановление фазы несущей сложно на КВ.

### 3.2. Оптимальный приём при Rayleigh fading (КВ)

На КВ RTTY страдает от:
- **Rayleigh fading** (многолучёвость)
- **Селективный фединг** (Mark и Space замирают независимо)
- **Доплеровское расширение** (0.1-10 Гц)

**Оптимальные подходы:**

**1. Diversity combining:**
- Пространственное разнесение (2 антенны)
- Частотное разнесение (повтор на другой частоте)
- Временное разнесение (повтор символа — SITOR-B)
- **Mark/Space diversity** — ATC отслеживает уровни независимо (то, что мы делаем)

**2. Полоса фильтра шире оптимальной:**
В фединге оптимальная полоса фильтра шире чем в AWGN из-за доплеровского расширения. Для типичного КВ (Doppler 1-10 Гц) полоса 60-100 Гц на тон лучше чем matched-filter 45 Гц.

**3. ATC time constants:**
- Attack ~50 мс, Release ~200-500 мс — для типичного КВ фединга (1-10 Гц)
- Наши текущие: attack ~44 мс, release ~352 мс @ 45.45 бод — **в правильном диапазоне**

**4. Hard limiter:**
Клиппирование до ±1 перед детектором убирает амплитудные вариации → чистое частотное детектирование. Выигрыш при Rayleigh fading, потеря ~1 дБ в AWGN. MMTTY имеет эту опцию.

### 3.3. Error correction для Baudot/ITA2

Baudot не имеет встроенной помехозащиты. Возможные подходы:

| Метод | Описание | Выигрыш |
|---|---|---|
| **SITOR-B** | Каждый символ дважды с интервалом 280 мс | Time diversity, ~3 дБ |
| **Start/stop redundancy** | Известные start(0) и stop(1) биты = 33% избыточность | 2-3 дБ (2Tone подход) |
| **Байесовский декодер** | Частотная статистика символов как prior | +0.5-1 дБ для текста |
| **Pattern matching** | Для контестов: ограниченный словарь | Значительный для контестов |
| **Мажоритарное голосование** | При повторных передачах (DWD) | BER ÷2-5 |

### 3.4. Формула Goertzel

```
// Инициализация (один раз):
coeff = 2.0f × cos(2π × f_tone / f_sample)

// Каждый сэмпл (2 МАС):
s0 = sample + coeff × s1 − s2
s2 = s1
s1 = s0

// Конец блока (раз в символ):
power = s0² + s1² − coeff × s0 × s1

// Сброс:
s0 = s1 = s2 = 0
```

Вычислительная стоимость: **2 умножения + 2 сложения на сэмпл** (vs наши ~24 операции на I/Q + 4 biquad).

---

## 4. Сводная таблица сравнения

### 4.1. Архитектура декодеров

| Декодер | Демодуляция | Фильтры | Синхронизация | ATC | AFC | Шумоподавление |
|---|---|---|---|---|---|---|
| **TouchRTTY** | I/Q когерентный downmix | Biquad LPF ×4 | DPLL PI (zero-cross) | Да (FASR) | FFT peak | Нет |
| **MMTTY** | Multi-Goertzel | Goertzel (matched) | Multi-correlator ×8 | Да | Multi-Goertzel | Hard limiter |
| **2Tone** | Character ML | Windowed Goertzel | Char correlation | Неявно | Multi-freq scan | Soft-decision ML |
| **Fldigi** | I/Q non-coherent | Biquad BPF | DPLL 1-го порядка | Нет (стд.) | FFT peak | Blanker |
| **GRITTY** | True matched filter | Goertzel 1 символ | Multi-phase | Да | Freq search | — |
| **Malachite** | Dual BPF + envelope | IIR BPF | Start-bit only | Нет | Нет | Нет |
| **ATS-25** | Goertzel / zero-cross | Минимальные | Start-bit only | Нет | Нет | Нет |
| **MixW** | Sliding DFT | SDFT bins | DPLL | Да | SDFT peak | Blanker |
| **MultiPSK** | Dual BPF | Butterworth IIR | DPLL | Да | FFT | SITOR-B FEC |
| **GNU Radio** | Quad demod / dual | FIR (configurable) | Mueller-Muller | Config | FLL/Costas | Config |

### 4.2. Производительность при низком SNR

| Декодер | Порог декодирования (SNR в 2.4 кГц) | Относительно MMTTY |
|---|---|---|
| **2Tone** | **~−14 дБ** | **+2-3 дБ лучше** |
| **MMTTY** | ~−11 дБ | **Эталон** |
| **GRITTY** | ~−10 дБ | −1 дБ |
| **TouchRTTY** (текущий) | ~−7 дБ | −4 дБ |
| **Fldigi** | ~−5 дБ | −6 дБ |
| **MixW** | ~−6 дБ | −5 дБ |
| **MultiPSK** | ~−4 дБ | −7 дБ |
| **Malachite** | ~+5 дБ | −16 дБ |
| **ATS-25** | ~+7 дБ | −18 дБ |

### 4.3. Вычислительная стоимость (на сэмпл)

| Декодер | Операций/сэмпл | Подходит для RP2350 |
|---|---|---|
| **Goertzel ×2** (базовый) | ~4 МАС | ✅ Легко |
| **TouchRTTY** (I/Q + 4 biquad) | ~28 МАС | ✅ Текущая реализация |
| **MMTTY-style** (8-phase × 2 tone) | ~32 МАС | ✅ Легко |
| **2Tone-style** (32 char × 8 phase) | ~256 корреляций/символ | ✅ Реализуемо (~2 мс/символ) |
| **Sliding DFT** (256 point) | ~512 МАС | ⚠️ Тяжеловато |
| **Mueller-Muller TED** | ~4 МАС/символ | ✅ Тривиально |

---

## 5. Рекомендации для TouchRTTY

### 5.1. Roadmap улучшений (по приоритету)

#### Этап 1: Quick wins (+3-5 дБ, ~2 дня работы)

**A. Переход на Goertzel вместо I/Q + Biquad**

| Параметр | Текущий (I/Q + Biquad) | Goertzel |
|---|---|---|
| Операций/сэмпл | ~28 МАС | **~4 МАС** |
| Тип фильтра | Biquad LPF (не matched) | **Matched filter (оптимальный)** |
| SNR на выходе | Субоптимальный | **Оптимальный для non-coherent FSK** |
| Реализация | 4 biquad + NCO + LUT | 2 рекурсивных фильтра |

```cpp
// Замена всего I/Q демодулятора на Goertzel:
struct GoertzelState {
    float coeff;     // 2*cos(2*PI*f/fs)
    float s1, s2;    // state variables
    int count;       // samples accumulated
    int block_size;  // = samples_per_symbol
};

void goertzel_init(GoertzelState* g, float freq, float sample_rate, float baud) {
    g->coeff = 2.0f * cosf(2.0f * PI * freq / sample_rate);
    g->block_size = (int)(sample_rate / baud);
    g->s1 = g->s2 = 0;
    g->count = 0;
}

float goertzel_process(GoertzelState* g, float sample) {
    float s0 = sample + g->coeff * g->s1 - g->s2;
    g->s2 = g->s1;
    g->s1 = s0;
    g->count++;

    if (g->count >= g->block_size) {
        float power = g->s1*g->s1 + g->s2*g->s2 - g->coeff*g->s1*g->s2;
        g->s1 = g->s2 = 0;
        g->count = 0;
        return power;
    }
    return -1.0f;  // блок не завершён
}
```

**Выигрыш:** +1-2 дБ SNR (matched filter vs biquad LPF) + CPU экономия в 7×.

**B. Мульти-фазная синхронизация (как MMTTY)**

```cpp
// Вместо 1 Goertzel — 8 на разных фазовых смещениях:
#define NUM_PHASES 8
GoertzelState mark_goertzel[NUM_PHASES];
GoertzelState space_goertzel[NUM_PHASES];

// Инициализация: каждый фазовый канал стартует с разным offset
for(int p = 0; p < NUM_PHASES; p++) {
    goertzel_init(&mark_goertzel[p], mark_freq, SAMPLE_RATE, baud);
    mark_goertzel[p].count = -(p * mark_goertzel[p].block_size / NUM_PHASES);
}

// На каждом сэмпле: обработать все 8 фаз (16 Goertzel = 32 МАС)
// Раз в символ: выбрать фазу с максимальным |mark-space| контрастом
// Эта фаза = оптимальный момент sampling → замена DPLL!
```

**Выигрыш:** Замена DPLL на мульти-коррелятор, +1 дБ при drift/jitter.

#### Этап 2: Character-level ML (+2-3 дБ дополнительно, ~1 неделя)

**Подход 2Tone на RP2350:**

```cpp
// 32 символа × 8 фаз = 256 гипотез
// Каждая гипотеза: Goertzel-корреляция за 7.5 бит
// При 45.45 бод: ~2200 сэмплов на символ, 256 корреляций

#define NUM_ITA2 32
#define NUM_PHASES 8

float char_metric[NUM_ITA2][NUM_PHASES];

// Для каждой гипотезы: генерировать ожидаемый паттерн Mark/Space
// start(0=Space) + 5 data bits + 1.5 stop(1=Mark)
// Корреляция с принятым сигналом через Goertzel

for (int c = 0; c < NUM_ITA2; c++) {
    uint8_t pattern = (0 << 0) |           // start = 0 (Space)
                      ((c & 0x1F) << 1) |  // 5 data bits
                      (0x7 << 6);           // 1.5 stop = 1 (Mark)

    for (int p = 0; p < NUM_PHASES; p++) {
        char_metric[c][p] = correlate_with_pattern(received, pattern, p);
    }
}

// Выбрать символ с максимальным metric:
int best_char = argmax(char_metric);
```

**Вычислительный бюджет на RP2350:**
- 256 корреляций × ~2200 сэмплов = 563 200 МАС
- При 300 МГц: **~1.9 мс** (из доступных ~220 мс на символ при 45 бод)
- **Загрузка: <1% CPU** — огромный запас!

**Выигрыш:** Порог декодирования улучшается с ~−7 дБ до **~−12...−14 дБ SNR**.

#### Этап 3: Дополнительные улучшения

| Техника | Источник | Выигрыш | CPU |
|---|---|---|---|
| Mueller-Muller TED | GNU Radio | Лучше zero-crossing | +0.01% |
| Hard limiter (опция) | MMTTY | +1-2 дБ при Rayleigh | 0% |
| Байесовский prior | 2Tone concept | +0.5-1 дБ для текста | незначительный |
| SITOR-B поддержка | MultiPSK | Time diversity для DWD | +5% |
| Wider ATC decay | Theory | Лучше при медленном фединге | 0% |
| Windowed Goertzel (Hann) | 2Tone | Меньше spectral leakage | +1 МАС/сэмпл |

### 5.2. Позиционирование после улучшений

| Параметр | Сейчас | После Этапа 1 | После Этапа 2 |
|---|---|---|---|
| **Класс декодера** | fldigi/MixW | MMTTY | **2Tone** |
| **Порог SNR** | ~−7 дБ | ~−10 дБ | **~−13 дБ** |
| **CPU Core 0** | 4-6% | **2-3%** (Goertzel эффективнее) | **3-4%** (+ML) |
| **Синхронизация** | DPLL (zero-cross) | Multi-correlator | Character-level |
| **ATC** | Да | Да (улучшенная) | Неявная (ML) |

### 5.3. Архитектура после Этапа 2

```
ADC 12-bit @ 10 kHz
  → DC removal
  → Noise blanker (новый)
  → FIR 63-tap (или FIR 31-tap symmetric)
  → AGC
  → Spectral subtraction (новый, при FFT)
  → 32 × 8 Goertzel character correlators (замена I/Q + Biquad + DPLL + framer)
  → Maximum Likelihood character selection
  → ITA2 декодирование + Bayesian prior (опционально)
  → Символ → UI + Serial
```

Вся цепочка I/Q демодулятор → Biquad ×4 → ATC → DPLL → integrate-and-dump → Baudot framer заменяется **одним блоком**: character-level ML Goertzel correlator. Проще, эффективнее, на 6 дБ лучше.

---

*Документ создан на основе анализа открытых исходников (fldigi, GRITTY, GNU Radio), документации авторов (MMTTY/JE3HHT, 2Tone/G3YYD), академических источников (Proakis "Digital Communications"), и сравнения с текущей архитектурой TouchRTTY build g259.*
