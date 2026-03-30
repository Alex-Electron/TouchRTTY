# ТЗ: ПРОФЕССИОНАЛЬНЫЙ RTTY ДЕКОДЕР — CORE 0
## Все скорости + служебный телетайп (DWD погода, SYNOP)
## RP2350A @ 300 МГц | Уровень fldigi | Версия 2.0

---

## 0. СПРАВОЧНИК РЕАЛЬНЫХ RTTY СИГНАЛОВ В ЭФИРЕ

Понимание реальных сигналов критично для правильного выбора параметров.

### 0.1 Любительский RTTY

| Скорость | Shift | Стоп-биты | Применение |
|:---|:---|:---|:---|
| 45.45 Бод | 170 Гц | 1.5 | Основной любительский стандарт |
| 50 Бод | 170 Гц | 1.5 | Европейские любители |
| 75 Бод | 170 Гц | 1.5 | Контесты, высокоскоростной |
| 100 Бод | 170 Гц | 1.0 | Редко |

### 0.2 Коммерческий и служебный RTTY

| Скорость | Shift | Станция | Частоты | Содержание |
|:---|:---|:---|:---|:---|
| 50 Бод | **450 Гц** | DDK2/DDH7/DDK9 (DWD) | 4583, 7646, 10100.8 кГц | SYNOP погода |
| 50 Бод | 85 Гц | DDH47 (DWD) | 147.3 кГц (LW) | SYNOP погода |
| 75 Бод | 425 Гц | Bracknell meteo | 4489 кГц | Погода UK |
| 75 Бод | 850 Гц | US mil/commercial | разные | Коммерция |

### 0.3 Ключевой факт по DWD SYNOP (главная цель)

SYNOP репорты передаются Deutscher Wetterdienst на коротких и длинных волнах
каждые 6 часов. **Скорость: строго 50 Бод**.

Параметры fldigi для приёма DWD SYNOP:
`MODEM:RTTY:450:50:5` — shift 450 Гц, 50 Бод, 5 бит.
Сигнал передаётся в **LSB** модуляции, поэтому требуется **инверсия** (REV:on).

**Это означает:** при приёме с SSB приёмника в режиме USB — Mark и Space
меняются местами. Декодер должен поддерживать автоматическое определение инверсии.

### 0.4 Матрица пресетов декодера

```c
// Все рабочие конфигурации в одной таблице:
typedef struct {
    const char *name;
    float baud;
    float shift_hz;
    float stop_bits;
    bool  rx_inverted;   // для LSB-станций
    const char *description;
} rtty_preset_t;

static const rtty_preset_t PRESETS[] = {
    // ── Любительские ───────────────────────────────────────────────
    {"HAM45",  45.45f, 170.0f, 1.5f, false, "Ham RTTY 45.45 Bd 170Hz"},
    {"HAM50",  50.0f,  170.0f, 1.5f, false, "Ham RTTY 50 Bd 170Hz"},
    {"HAM75",  75.0f,  170.0f, 1.5f, false, "Ham RTTY 75 Bd 170Hz"},
    {"HAM100", 100.0f, 170.0f, 1.0f, false, "Ham RTTY 100 Bd 170Hz"},
    // ── Служебные (DWD погода) ─────────────────────────────────────
    {"DWD450", 50.0f,  450.0f, 1.5f, true,  "DWD SYNOP 50Bd 450Hz (LSB→инверсия)"},
    {"DWD85",  50.0f,  85.0f,  1.5f, true,  "DWD LW 147.3kHz 50Bd 85Hz"},
    // ── Коммерческие ───────────────────────────────────────────────
    {"COM75",  75.0f,  425.0f, 1.5f, false, "Commercial 75Bd 425Hz"},
    {"COM75W", 75.0f,  850.0f, 1.5f, false, "Commercial 75Bd 850Hz"},
    // ── Пользовательский ──────────────────────────────────────────
    {"CUSTOM", 45.45f, 170.0f, 1.5f, false, "User defined"},
};
#define N_PRESETS (sizeof(PRESETS)/sizeof(PRESETS[0]))
```

---

## 1. АРХИТЕКТУРА СИСТЕМЫ

### 1.1 Core 0 — полный DSP конвейер

```
ADC DMA IRQ (38400 Гц физически)
     │
     ▼
[A] DC БЛОКИРОВКА          IIR HPF, Fc=20 Гц
     │
     ▼
[B] ДЕЦИМАЦИЯ ×4           FIR 48 tap, CMSIS → 9600 Гц рабочая
     │
     ▼
[C] AGC                    Атака 10 мс, спад 500 мс
     │
     ├─────────────────────────────────────────────────────┐
     ▼                                                     ▼
[D] FFT ВЕТКА                                   [E] ДЕМОДУЛЯЦИОННАЯ ВЕТКА
    FFT 512pt Welch                                  │
    → waterfall (Core1 via FIFO)                [E1] I/Q Mark детектор
    → SNR meter                                      │
    → AFC update                                [E2] I/Q Space детектор
    → auto-baud                                      │
    → SYNOP header detect                       [E3] ATC нормализатор
                                                     │
                                                [E4] DPLL синхронизатор
                                                     │
                                                [E5] Baudot фреймер
                                                     │
                                                [E6] Инверсия авто-детект
                                                     │
                                                [E7] → ring_buffer → Core1
```

### 1.2 Межъядерный обмен данными

```c
// ── Символьный буфер (Core0 → Core1) ─────────────────────────────
#define CHAR_RING_SIZE  512

typedef struct {
    char     ch;
    uint8_t  quality;      // 0-100: уверенность в бите
    uint32_t ts_ms;
} rx_char_t;

static rx_char_t  char_ring[CHAR_RING_SIZE];
static volatile uint32_t char_wr = 0, char_rd = 0;

void core0_char_push(char ch, uint8_t q) {
    uint32_t next = (char_wr + 1) % CHAR_RING_SIZE;
    if (next == char_rd) return;  // full — drop
    char_ring[char_wr] = (rx_char_t){ch, q,
        to_ms_since_boot(get_absolute_time())};
    __dmb();
    char_wr = next;
}

bool core1_char_pop(rx_char_t *out) {
    if (char_rd == char_wr) return false;
    __dmb();
    *out = char_ring[char_rd];
    char_rd = (char_rd + 1) % CHAR_RING_SIZE;
    return true;
}

// ── Состояние декодера (Core0 пишет атомарно, Core1 читает) ───────
typedef struct {
    // Частоты:
    float  mark_hz, space_hz, center_hz, shift_hz;
    // Качество:
    float  snr_db;
    float  mark_level, space_level;   // RMS огибающих 0..1
    float  ber_estimate;
    char   quality[8];                // "POOR/FAIR/GOOD/EXCEL"
    // Синхронизатор:
    float  dpll_phase;               // 0..1 (для eye diagram)
    float  dpll_freq_error;          // Гц
    // AFC:
    float  afc_error_hz;
    bool   afc_locked;
    // Режим:
    float  baud_rate;
    bool   inverted;
    bool   signal_present;
    char   preset_name[12];
    // Для экрана:
    float  mark_env_display;
    float  space_env_display;
} decoder_state_t;

static volatile decoder_state_t g_dec;
```

---

## 2. НЮАНСЫ АЦП — ОЦИФРОВКА АУДИОСИГНАЛА

### 2.1 Физическая частота дискретизации

```
RTTY сигнал занимает: 300 – 3000 Гц (аудио полоса)
Теорема Найквиста: нужно > 2 × 3000 = 6000 Гц

Почему 9600 Гц рабочая, а не 6000?
  - 9600 даёт запас для переходной полосы децимирующего фильтра
  - 9600 / 45.45 = 211.2 — достаточно сэмплов на символ для DPLL
  - 9600 / 75 = 128 — для 75 Бод тоже достаточно
  - FFT 512pt при 9600 → разрешение 18.75 Гц/бин — видно shift 85 Гц!

Почему oversample ×4 (физически 38400 Гц)?
  - +6 дБ к SNR (3 дБ на каждое удвоение при белом шуме)
  - Крутой антиалиасинговый фильтр: Найквист 4800 Гц
    (прямоугольная зона до 3700 Гц RC-фильтра → нет алиасинга)
  - Точность тайминга ADC: джиттер 1 такт / 38400 = 26 нс (vs 104 нс при 9600)
```

### 2.2 Критичный параметр: разрядность АЦП и уровень сигнала

```
RP2350 ADC: 12 бит → 4096 уровней
Шум квантования: ~0.5 МЗР ≈ ±1 отсчёт

Оптимальный входной уровень:
  Пик сигнала должен занимать 40-80% шкалы (1638..3277 из 4096)
  То есть размах ≈ 1638 отсчётов (от центра ~2048)
  Это соответствует ~0.6 Vpp при AVREF=3.3В

Признаки неправильного уровня:
  Слишком тихо (< 200 отсчётов размах):
    → AGC поднимет gain > 50x
    → шум квантования доминирует
    → SNR плохой даже при сильном сигнале

  Слишком громко (> 4000 отсчётов размах):
    → клиппинг (обрезание) на АЦП
    → гармоники 3-го порядка → помехи в полосе
    → Mark или Space подавляется

Цель: AGC gain ≈ 0.5..2.0 при нормальном уровне входа.
Если gain > 20 → повысить входной уровень потенциометром.
Если gain < 0.2 → убавить уровень.
```

### 2.3 DMA конфигурация с точным тактированием

```c
#define ADC_FS_PHYS   38400U
#define ADC_FS_WORK   9600U
#define ADC_OVERSAMP  4U
#define BLOCK_PHYS    1024U   // сэмплов в физическом блоке
#define BLOCK_WORK    256U    // после децимации

// clkdiv для АЦП при sysclk 300 МГц:
// ADC input clk = 48 МГц (фиксировано через USB PLL)
// clkdiv = 48e6 / 38400 - 1 = 1249.0
// Точно делится! → нет джиттера частоты дискретизации

void adc_precise_init(void) {
    adc_init();
    adc_gpio_init(26);
    adc_select_input(0);
    adc_fifo_setup(
        true,   // enable FIFO
        true,   // DMA dreq
        1,      // dreq threshold = 1 sample
        false,  // no ERR bit
        false   // full 12-bit (не 8-bit)
    );
    // clkdiv: ADC clk = 48 МГц (не зависит от sysclk!)
    // 48000000 / 38400 = 1250.0 → clkdiv = 1250-1 = 1249
    adc_set_clkdiv(1249.0f);
}
```

> **ВАЖНО:** ADC тактируется от 48 МГц USB PLL, а не от sysclk.
> Это значит точная частота дискретизации 38400 Гц не зависит от разгона.
> `adc_set_clkdiv(1249.0f)` → fs = 48000000 / 1250 = **38400.0 Гц точно**.

### 2.4 Децимирующий FIR — проектирование

```c
// Требования к фильтру:
// Полоса пропускания: 0 .. 4500 Гц (нужна для shift=450 Гц + запас)
// Переходная полоса: 4500 .. 19200 Гц (до первого алиаса)
// Затухание в полосе задержания: > 60 дБ
// Количество отводов: 48 (баланс качество/скорость)

// Коэффициенты рассчитаны для окна Кайзера β=7:
// scipy.signal.firwin(48, 4500.0/38400.0*2, window=('kaiser', 7))

static const float DECIM_COEFFS[48] = {
    // ВСТАВИТЬ РЕАЛЬНЫЕ КОЭФФИЦИЕНТЫ из scipy:
    // python3 -c "
    // from scipy import signal
    // import numpy as np
    // h = signal.firwin(48, 4500.0/(38400/2), window=('kaiser', 7))
    // print(','.join(f'{x:.10f}f' for x in h))
    // "
    // Placeholder (симметричные — нужно заменить):
     0.0002f, 0.0006f, 0.0014f, 0.0027f,
     0.0047f, 0.0074f, 0.0108f, 0.0148f,
     0.0193f, 0.0240f, 0.0286f, 0.0328f,
     0.0362f, 0.0384f, 0.0393f, 0.0384f,
     0.0362f, 0.0328f, 0.0286f, 0.0240f,
     0.0193f, 0.0148f, 0.0108f, 0.0074f,
    // ... симметрично (48 коэффициентов)
};

// CMSIS-DSP arm_fir_decimate_f32: обрабатывает BLOCK_PHYS → BLOCK_WORK
static arm_fir_decimate_instance_f32 g_decim;
static float g_decim_state[48 + ADC_OVERSAMP - 1];

void decimation_init(void) {
    arm_fir_decimate_init_f32(&g_decim, 48, ADC_OVERSAMP,
        DECIM_COEFFS, g_decim_state, BLOCK_PHYS);
}
```

---

## 3. I/Q ДЕМОДУЛЯТОР — ДЕТАЛИ РЕАЛИЗАЦИИ

### 3.1 Почему I/Q лучше простого BPF

```
Простой BPF детектор:
  s(t) → [BPF Mark] → |.| → envelope_mark
  s(t) → [BPF Space] → |.| → envelope_space
  Решение: envelope_mark > envelope_space → Mark

  Недостаток: BPF фаза нелинейна → ISI (межсимвольная интерференция)
  При 45.45 Бод и узком BPF → "хвост" предыдущего символа влияет на текущий

I/Q квадратурный демодулятор (baseband):
  Переносим Mark тон на 0 Гц:
    I_m = s(t) × cos(2π × mark_hz × t)
    Q_m = s(t) × sin(2π × mark_hz × t)
    → LPF (низкочастотный, линейная фаза) → P_m = I_m² + Q_m²

  Преимущество: LPF имеет линейную фазу → нет ISI
  При Raised Cosine LPF: теоретически нулевая ISI
  Результат: приём при SNR на 2-3 дБ лучше чем BPF метод
```

### 3.2 Быстрый sin/cos через аппаратный интерполятор

```c
// RP2350 SIO INTERP0 для мгновенного доступа к таблице sin/cos:

#define SINCOS_TABLE_SIZE  4096   // степень 2 для быстрого маскирования
#define SINCOS_TABLE_BITS  12

static float g_sin_table[SINCOS_TABLE_SIZE];

void sincos_table_init(void) {
    for (int i = 0; i < SINCOS_TABLE_SIZE; i++)
        g_sin_table[i] = sinf(2.0f * M_PI * i / SINCOS_TABLE_SIZE);

    // Настройка INTERP0 для индексации по фазе:
    interp_config cfg = interp_default_config();
    interp_config_set_shift(&cfg, 32 - SINCOS_TABLE_BITS);
    interp_config_set_mask(&cfg, 0, SINCOS_TABLE_BITS - 1);
    interp_config_set_add_raw(&cfg, false);
    interp_set_config(interp0, 0, &cfg);
    interp0->base[1] = (uint32_t)g_sin_table;
}

// Фаза: uint32_t с полным 32-битным диапазоном (wraparound бесплатно)
typedef uint32_t phase_t;

// Шаг фазы для частоты f при fs=9600:
// phase_step = (uint32_t)(f / fs * (1ULL << 32))
#define PHASE_STEP(f, fs)  ((phase_t)((f) / (fs) * 4294967296.0))

static inline float fast_sin(phase_t ph) {
    interp0->accum[0] = ph;
    return *(float*)interp0->peek[1];
}

static inline float fast_cos(phase_t ph) {
    return fast_sin(ph + (phase_t)(SINCOS_TABLE_SIZE/4 * (1<<(32-SINCOS_TABLE_BITS))));
}
```

### 3.3 LPF bandwidth для разных скоростей

```
Ключевой параметр: Fc LPF = baud_rate × K

K=0.5 → Raised Cosine (минимум ISI, хуже при шуме, нужна точная настройка)
K=0.75 → Extended Raised Cosine (лучший компромисс) ← РЕКОМЕНДУЕТСЯ
K=1.0 → Matched Filter (оптимально при AWGN, больше ISI при drift)

Для разных скоростей:
  45.45 Бод: Fc = 45.45 × 0.75 = 34.1 Гц
  50 Бод:    Fc = 50   × 0.75 = 37.5 Гц  ← ДЛЯ DWD SYNOP
  75 Бод:    Fc = 75   × 0.75 = 56.3 Гц
  100 Бод:   Fc = 100  × 0.75 = 75.0 Гц

ВАЖНО для 450 Гц shift (DWD):
  Shift 450 Гц, 50 Бод → расстояние до соседнего тона 450 Гц
  LPF Fc = 37.5 Гц → затухание на 450 Гц: (450/37.5)² = 144x = -21 дБ
  Это достаточно для отделения Mark от Space при 450 Гц shift!

ПРОБЛЕМА при 85 Гц shift (DWD LW):
  Shift всего 85 Гц, 50 Бод → расстояние 85 Гц
  LPF Fc = 37.5 Гц → затухание на 85 Гц: (85/37.5)² = 5.1x = -7 дБ
  НЕДОСТАТОЧНО! Нужен более узкий фильтр:
  Fc = 85/4 = 21 Гц (K≈0.42) → хуже ISI но единственный вариант
  Для 85 Гц shift используем K=0.35..0.40
```

### 3.4 Адаптивный выбор K (важно!)

```c
float compute_lpf_k(float shift_hz, float baud_rate) {
    float ratio = shift_hz / baud_rate;
    // ratio = shift/baud:
    //   85/50  = 1.7  → очень узкий shift → K=0.35
    //   170/45 = 3.7  → стандарт любит. → K=0.75
    //   450/50 = 9.0  → широкий → K=0.75
    //   850/75 = 11.3 → очень широкий → K=0.75

    if (ratio < 2.0f)  return 0.35f;   // 85 Гц shift
    if (ratio < 4.0f)  return 0.60f;   // 170 Гц shift
    return 0.75f;                       // 450+ Гц shift
}

// При смене пресета — пересчитать LPF:
void update_lpf_for_preset(iq_demod_t *d, float shift_hz) {
    float k = compute_lpf_k(shift_hz, d->baud_rate);
    float fc = d->baud_rate * k;
    design_lpf_biquad(fc, d->fs,
        &d->b0, &d->b1, &d->b2, &d->a1, &d->a2);
    // Сбросить состояния фильтров (избежать переходного процесса):
    d->mi_z1=d->mi_z2=d->mq_z1=d->mq_z2=0.0f;
    d->si_z1=d->si_z2=d->sq_z1=d->sq_z2=0.0f;
}
```

---

## 4. ATC — ДЕТАЛИ ДЛЯ СЛУЖЕБНОГО RTTY

### 4.1 Почему ATC критичен для DWD

```
DWD передаёт на SSB (LSB, F1B модуляция).
При приёме через SSB приёмник возникают:
  - Замирания от многолучёвости (selective fading)
  - Неравномерный уровень Mark и Space из-за разных частот

ATC корректирует порог так:
  disc = (env_mark / atc_mark) - (env_space / atc_space)
  Если Mark замирает в 4 раза → atc_mark падает в 4 раза
  → disc остаётся правильным даже при глубоком замирании

Без ATC: замирание Mark на 10 дБ → 30-40% ошибок
С ATC:   замирание Mark на 10 дБ → 2-5% ошибок (приемлемо)
```

### 4.2 Временные константы ATC для разных скоростей

```c
void atc_init_for_baud(atc_t *a, float baud_rate, float fs) {
    float T = fs / baud_rate;   // сэмплов на символ

    // Атака: 1-2 символа (быстро реагирует на рост амплитуды)
    // Спад: 8-16 символов (медленно при замирании)
    // При 45.45 Бод: T=211 сэмплов
    //   Атака: 2×211=422 → exp(-1/422) = 0.99763
    //   Спад: 16×211=3376 → exp(-1/3376) = 0.99970
    // При 75 Бод: T=128 сэмплов
    //   Атака: 2×128=256 → exp(-1/256) = 0.99609
    //   Спад: 16×128=2048 → exp(-1/2048) = 0.99951

    a->fast_tc  = expf(-1.0f / (2.0f  * T));
    a->slow_tc  = expf(-1.0f / (16.0f * T));

    // Дополнительно: клиппер для ограничения пиков:
    // Ограничивает дискриминатор на уровне 1.5× ожидаемый
    // Подавляет импульсные помехи на 3-5 дБ
    a->clip_level = 1.5f;
}
```

---

## 5. DPLL — СИНХРОНИЗАТОР БИТОВ: КРИТИЧНЫЕ НЮАНСЫ

### 5.1 Количество сэмплов на символ

```
45.45 Бод при 9600 Гц: 9600/45.45 = 211.2 сэмплов/символ
50   Бод при 9600 Гц: 9600/50    = 192.0 сэмплов/символ
75   Бод при 9600 Гц: 9600/75    = 128.0 сэмплов/символ
100  Бод при 9600 Гц: 9600/100   = 96.0  сэмплов/символ

Для 45.45 Бод: период 211.2 сэмплов → точность DPLL ±1 сэмпл = ±0.47%
Это достаточно для стабильного приёма (fldigi использует тот же подход)

Для 75 Бод: период 128 сэмплов → точность ±0.78%
При 1.5 стоп-бита: 128×1.5=192 сэмпла для стопа → достаточно надёжно

ПРОБЛЕМА 100 Бод при 9600 Гц: период 96 сэмплов — DPLL нестабилен!
РЕШЕНИЕ: для 100+ Бод увеличить рабочую частоту до 19200 Гц:
  Изменить clkdiv: 48000000/19200 = 2500 → clkdiv = 2499
  Или: использовать 2× oversample (×8 всего) с периодом 192 сэмпла
```

### 5.2 Полоса DPLL для разных скоростей

```c
// Полоса петли DPLL влияет на:
//   Узкая (alpha=0.01): медленный захват, но устойчивый
//   Широкая (alpha=0.05): быстрый захват, шумный

// Рекомендации из fldigi:
// alpha ≈ 0.01..0.04 для стабильных сигналов
// Для DWD (частотный drift от коротких волн): alpha=0.03

float dpll_alpha_for_baud(float baud_rate) {
    // Нормированная полоса: BnT ≈ 0.005..0.01
    // alpha ≈ 2π × BnT
    float BnT = 0.008f;   // хороший компромисс
    return 2.0f * M_PI * BnT;  // ≈ 0.050
    // Для 50 Бод можно 0.035 (стабильнее при drift КВ)
}
```

### 5.3 Обнаружение потери синхронизации

```c
typedef struct {
    // ... поля из предыдущего ТЗ ...
    float  phase_error_rms;     // RMS ошибки фазы (индикатор качества синхр.)
    int    transitions_in_window; // переходов за последние N символов
    bool   sync_lost;
    int    sync_lost_count;
} dpll_t;

// Синхронизация потеряна если:
// 1. Нет переходов за последние 20 символов (тишина или несущая без данных)
// 2. RMS ошибки фазы > 0.3 (нестабильный сигнал)

void dpll_check_sync(dpll_t *p) {
    p->sync_lost = (p->transitions_in_window < 2 ||
                    p->phase_error_rms > 0.30f);
    if (p->sync_lost) {
        p->sync_lost_count++;
        if (p->sync_lost_count > 50) {
            // Сброс: начать поиск синхронизации заново
            p->phase = 0.5f;
            p->freq_error = 0.0f;
            p->sync_lost_count = 0;
        }
    } else {
        p->sync_lost_count = 0;
    }
}
```

---

## 6. BAUDOT ФРЕЙМЕР — НЮАНСЫ ДЛЯ СЛУЖЕБНОГО RTTY

### 6.1 Проблема FIGS/LTRS в SYNOP потоке

```
DWD SYNOP состоит из групп цифр: "00000 11111 22222..."
Декодер часто находится в режиме FIGS.
Передача ошибок может сдвинуть регистр LTRS/FIGS неожиданно.

fldigi решает это через "Unshift on Space":
При получении пробела (код 0x04) → автоматически переключиться в FIGS.
Это восстанавливает правильный режим для числовых групп.
```

```c
typedef struct {
    frame_state_t state;
    uint8_t  shift_reg;
    int      bit_count;
    bool     figs_mode;
    float    stop_acc;
    int      stop_samples;
    int      stop_needed;
    // SYNOP настройки:
    bool     unshift_on_space;   // для DWD: всегда true
    bool     show_figs_ltrs;     // показывать служебные символы
    // Статистика:
    uint32_t total_chars;
    uint32_t error_chars;       // ошибки стоп-бита
    uint32_t ltrs_switches;
    uint32_t figs_switches;
} baudot_framer_t;

char baudot_framer_push(baudot_framer_t *f, float bit_value) {
    int bit = (bit_value > 0.0f) ? 1 : 0;

    switch (f->state) {
        case FRAME_WAIT_START:
            if (bit == 0) {     // стартовый бит = Space
                f->state = FRAME_RECV_DATA;
                f->shift_reg = 0;
                f->bit_count = 0;
                f->stop_acc = 0;
                f->stop_samples = 0;
            }
            break;

        case FRAME_RECV_DATA:
            f->shift_reg |= (bit << f->bit_count);
            if (++f->bit_count >= 5) {
                f->state = FRAME_RECV_STOP;
                f->stop_needed = (f->stop_bits >= 1.45f) ? 2 : 1;
            }
            break;

        case FRAME_RECV_STOP: {
            f->stop_acc += bit_value;
            f->stop_samples++;

            if (f->stop_samples < f->stop_needed) break;

            f->state = FRAME_WAIT_START;
            f->total_chars++;

            // Проверить стоп-бит (должен быть Mark = положительный):
            if (f->stop_acc / f->stop_samples < -0.1f) {
                f->error_chars++;
                return '\x01';  // маркер ошибки фрейма
            }

            // Декодировать:
            uint8_t code = f->shift_reg & 0x1F;

            if (code == BAUDOT_LTRS) {
                f->figs_mode = false;
                f->ltrs_switches++;
                return 0;  // не выводим служебный
            }
            if (code == BAUDOT_FIGS) {
                f->figs_mode = true;
                f->figs_switches++;
                return 0;
            }

            char ch = f->figs_mode ?
                      baudot_figs[code] : baudot_ltrs[code];

            // Unshift on Space (для SYNOP):
            if (f->unshift_on_space && ch == ' ')
                f->figs_mode = false;

            return ch ? ch : 0;
        }
    }
    return 0;
}
```

### 6.2 Таблица ITA2 — расширенная с русскими символами (ГОСТ)

```c
// Стандартная ITA2 (Международная)
static const char baudot_ltrs_ita2[32] = {
    '\0','E','\n','A',' ','S','I','U',
    '\r','D','R','J','N','F','C','K',
    'T', 'Z','L', 'W','H','Y','P','Q',
    'O', 'B','G','\0','M','X','V','\0'
};
static const char baudot_figs_ita2[32] = {
    '\0','3','\n','-',' ','\'','8','7',
    '\r','$','4','\a',',','!',':','(',
    '5', '"',')','2','#','6','0','1',
    '9', '?','&','\0','.','/',';','\0'
};

// Russian RTTY (50 Бод, 200 Гц shift — встречается в эфире)
// Код отличается в FIGS таблице
static const char baudot_figs_rus[32] = {
    '\0','3','\n','-',' ','\0','8','7',
    '\r','$','4','\0',',','\0',':','(',
    '5', '"',')','2','=','6','0','1',
    '9', '?','+','\0','.','/',';','\0'
};
```

---

## 7. АВТО-ДЕТЕКТ ИНВЕРСИИ (КРИТИЧНО ДЛЯ DWD)

```
Проблема: DWD передаёт в LSB (F1B).
При приёме на USB приёмнике: Mark и Space меняются местами.
Без инверсии: видим в эфире, но декодируем мусор.

Алгоритм автодетекта:
1. Принимаем 20-50 символов в прямом режиме
2. Считаем: сколько символов валидны (есть в ITA2), сколько нет
3. Если invalid > 70% → пробуем инверсию
4. После инверсии повторяем подсчёт
5. Оставляем режим с лучшим соотношением valid/invalid
```

```c
typedef struct {
    int  valid_chars;
    int  invalid_chars;
    int  ltrs_count;
    int  figs_count;
    int  window;           // размер окна оценки (символов)
    bool trying_inversion; // в процессе теста инверсии
    int  test_chars;
    int  test_valid;
} inversion_detector_t;

bool is_valid_ita2_char(char ch) {
    if (ch >= 'A' && ch <= 'Z') return true;
    if (ch >= '0' && ch <= '9') return true;
    if (ch == ' ' || ch == '\n' || ch == '\r') return true;
    if (ch == '.' || ch == ',' || ch == '-' || ch == '?') return true;
    if (ch == ':' || ch == '(' || ch == ')') return true;
    return false;
}

bool inversion_update(inversion_detector_t *d, char ch,
                       bool *apply_inversion) {
    if (ch == 0 || ch == '\x01') return false;

    bool valid = is_valid_ita2_char(ch);
    d->valid_chars   += valid;
    d->invalid_chars += !valid;

    int total = d->valid_chars + d->invalid_chars;
    if (total < d->window) return false;

    float valid_rate = (float)d->valid_chars / total;

    // Сброс окна:
    d->valid_chars = d->invalid_chars = 0;

    if (valid_rate < 0.30f) {
        // Меньше 30% валидных → попробовать инверсию
        *apply_inversion = true;
        return true;
    }
    return false;
}
```

---

## 8. AFC — ПОДСТРОЙКА ЧАСТОТЫ

### 8.1 Особенность AFC для 450 Гц shift

```
При shift 450 Гц и 50 Бод:
  FFT разрешение 18.75 Гц/бин
  Параболическая интерполяция → ±2 Гц точность
  Допустимый drift: ±30 Гц от настроенной частоты

  Два пика отстоят на 450/18.75 = 24 бина — легко видны
  Поиск обоих пиков одновременно (как для 170 Гц shift)

При shift 85 Гц:
  85/18.75 = 4.5 бина между пиками — трудно различить!
  Нужен FFT большего размера: 1024pt → разрешение 9.375 Гц/бин
  85/9.375 = 9.1 бина → уже надёжно
  
  ИЛИ: накапливать больше кадров Welch (16 вместо 8)
  → эффективное разрешение улучшается
```

```c
#define AFC_FFT_SIZE_NORMAL  512   // для shift > 150 Гц
#define AFC_FFT_SIZE_NARROW  1024  // для shift <= 150 Гц (85 Гц)

void afc_select_fft_size(float shift_hz) {
    if (shift_hz < 150.0f) {
        // Узкий shift — нужен FFT 1024 для различения пиков
        g_afc_fft_size = AFC_FFT_SIZE_NARROW;
        g_welch_frames = 16;  // больше усреднения
    } else {
        g_afc_fft_size = AFC_FFT_SIZE_NORMAL;
        g_welch_frames = 8;
    }
}
```

### 8.2 Предотвращение "слипания" маркеров

```c
// При drift КВ станции маркеры могут сместиться в сторону
// и "прилипнуть" к соседнему сигналу.
// Защита: проверяем что найденный shift близок к ожидаемому.

bool afc_is_plausible(float found_mark, float found_space,
                       float expected_shift) {
    float found_shift = fabsf(found_mark - found_space);
    float ratio = found_shift / expected_shift;
    // Допустимое отклонение: ±20%
    return (ratio > 0.80f && ratio < 1.20f);
}
```

---

## 9. ОТОБРАЖЕНИЕ СЛУЖЕБНОЙ ИНФОРМАЦИИ НА ЭКРАНЕ

### 9.1 Debug-панель (видна поверх основного UI)

```
Экран 480×320, нижняя часть текстовой зоны или отдельный экран:

┌─────────────────────────────────────────────────────────────────┐
│ DSP DEBUG                                          [×]CLOSE     │
├────────────────────────────┬────────────────────────────────────┤
│ ADC                        │ DPLL                               │
│  Level:  ████░░  68%       │  Phase:  ████████░░  0.82         │
│  AGC:    ×1.24             │  FreqErr: -0.3 Гц                  │
│  DC off: +0.12             │  Transitions: 18/20               │
│  Clip:   NO                │  Sync: LOCKED ●                   │
├────────────────────────────┼────────────────────────────────────┤
│ DEMOD                      │ AFC                                │
│  Mark: ████████  0.91      │  Error: +1.2 Гц                   │
│  Space:███████░  0.78      │  Lock:  YES ●                     │
│  ATC_M: 0.84               │  Steps: 847                       │
│  ATC_S: 0.79               │  Drift: +0.08 Гц/мин             │
├────────────────────────────┼────────────────────────────────────┤
│ FRAMER                     │ QUALITY                            │
│  State: RECV_DATA          │  SNR:   14.2 дБ  GOOD             │
│  Bits:  3/5                │  BER:   0.003                     │
│  Errs:  2 (0.8%)           │  Valid: 97.2%                     │
│  FIGS:  ON                 │  Invert: NO                       │
└────────────────────────────┴────────────────────────────────────┘
```

### 9.2 Индикатор Eye Diagram (важен для DPLL)

```
Простой eye diagram на основе DPLL phase:
Накапливаем дискриминатор в буфере размером "1 символ":

  ┌──────────────────────┐
  │ EYE                  │
  │  +1 ████████████████ │  ← Mark выборки
  │   0 ─────────────── │  ← нулевая линия
  │  -1 ████████████████ │  ← Space выборки
  └──────────────────────┘
  Открытый глаз = хорошая синхронизация
  Закрытый = плохой сигнал или неправильная скорость
```

```c
// Накопление eye diagram:
#define EYE_SAMPLES  64   // сэмплов на период символа (нормировано)
static float g_eye_buf[EYE_SAMPLES];
static float g_eye_accum[EYE_SAMPLES];
static int   g_eye_count = 0;

void eye_push_sample(float disc, float dpll_phase) {
    int idx = (int)(dpll_phase * EYE_SAMPLES) % EYE_SAMPLES;
    g_eye_accum[idx] += disc;
    if (++g_eye_count >= 100) {
        // Передать Core1 для отображения:
        for (int i = 0; i < EYE_SAMPLES; i++)
            g_eye_buf[i] = g_eye_accum[i] / 100.0f;
        memset(g_eye_accum, 0, sizeof(g_eye_accum));
        g_eye_count = 0;
    }
}
```

### 9.3 Отображение скорости приёма в реальном времени

```c
// Считаем принятые символы за последние 60 секунд:
typedef struct {
    uint32_t chars_per_min;
    uint32_t errors_per_min;
    float    char_rate;     // симв/сек
    float    ber_realtime;  // реальный BER за минуту
} rx_statistics_t;

// На экране в статус-баре:
// "45.45Bd | 170Hz | 4.5 symb/s | BER:0.8% | GOOD"
```

---

## 10. АВТОДЕТЕКТ СКОРОСТИ

```c
// Гистограмма длин импульсов:
// При RTTY минимальный импульс = 1 битовый период
// Накапливаем длины в сэмплах, строим гистограмму

#define HIST_SIZE  512

static uint32_t g_baud_hist[HIST_SIZE];
static int      g_prev_bit = 0;
static int      g_run_len  = 0;

void baud_hist_push(int bit) {
    if (bit == g_prev_bit) {
        g_run_len++;
    } else {
        if (g_run_len < HIST_SIZE)
            g_baud_hist[g_run_len]++;
        g_run_len = 1;
        g_prev_bit = bit;
    }
}

float baud_detect_from_hist(float fs) {
    // Найти первый значимый пик (самый короткий импульс = 1 бит):
    // Пропускаем первые 5 бинов (шум)
    int peak = 5;
    for (int i = 6; i < HIST_SIZE; i++)
        if (g_baud_hist[i] > g_baud_hist[peak]) peak = i;

    if (peak == 0 || g_baud_hist[peak] < 50) return 0;  // нет данных

    float measured = fs / peak;

    // Snap к стандартным скоростям:
    static const float STD[] = {45.45f,50.f,75.f,100.f,110.f,150.f,200.f,300.f};
    float best = STD[0];
    for (int i = 1; i < 8; i++)
        if (fabsf(measured - STD[i]) < fabsf(measured - best))
            best = STD[i];

    // Принять только если ошибка < 5%:
    if (fabsf(measured - best) / best > 0.05f) return 0;

    memset(g_baud_hist, 0, sizeof(g_baud_hist));
    return best;
}
```

---

## 11. ПОЛНЫЙ ГЛАВНЫЙ ЦИКЛ CORE 0

```c
void core0_main(void) {
    // Инициализация:
    sincos_table_init();
    adc_precise_init();
    adc_dma_init();
    decimation_init();

    // Загрузить пресет (по умолчанию или из Flash):
    rtty_preset_t *p = &PRESETS[0];  // HAM45
    agc_init(&g_rtty.agc, ADC_FS_WORK);
    iq_demod_init(&g_rtty.demod, p->mark_hz_default,
                   p->space_hz_default, p->baud, ADC_FS_WORK);
    update_lpf_for_preset(&g_rtty.demod, p->shift_hz);
    atc_init_for_baud(&g_rtty.atc, p->baud, ADC_FS_WORK);
    dpll_init(&g_rtty.dpll, p->baud, ADC_FS_WORK);
    baudot_framer_init(&g_rtty.framer, p->baud, p->stop_bits);
    g_rtty.framer.unshift_on_space = true;  // всегда полезно
    g_rtty.inverted = p->rx_inverted;

    // Главный цикл:
    while (true) {
        // Ждём DMA IRQ:
        while (adc_buf_ready < 0) tight_loop_contents();
        int buf_idx = adc_buf_ready;
        adc_buf_ready = -1;

        // Проверить команды от Core1 (смена пресета, TUNE и т.д.):
        check_core1_commands();

        // Децимация и AGC:
        float decimated[BLOCK_WORK];
        process_adc_block(adc_buf[buf_idx], decimated);

        // FFT ветка (каждые 8 блоков = 213 мс):
        fft_update(decimated, BLOCK_WORK);

        // Основной DSP блок — посэмпловая обработка:
        for (int i = 0; i < BLOCK_WORK; i++) {
            float s = decimated[i];

            // I/Q демодуляция с аппаратными sin/cos:
            phase_t mp = g_rtty.mark_phase;
            phase_t sp = g_rtty.space_phase;
            g_rtty.mark_phase  += g_rtty.mark_phase_step;
            g_rtty.space_phase += g_rtty.space_phase_step;

            float mi = biquad_df2t(s * fast_cos(mp), ...);
            float mq = biquad_df2t(s * fast_sin(mp), ...);
            float si = biquad_df2t(s * fast_cos(sp), ...);
            float sq = biquad_df2t(s * fast_sin(sp), ...);

            float mp_pow = mi*mi + mq*mq;
            float sp_pow = si*si + sq*sq;

            // ATC:
            float disc = atc_process(&g_rtty.atc, mp_pow, sp_pow);
            if (g_rtty.inverted) disc = -disc;

            // Eye diagram:
            eye_push_sample(disc, g_rtty.dpll.phase);

            // Baud autodectect:
            // (работает на raw бит-потоке до фреймера)

            // DPLL:
            float bit_val;
            if (dpll_process(&g_rtty.dpll, disc, &bit_val)) {
                // Baud histogram:
                baud_hist_push(bit_val > 0.f ? 1 : 0);

                // Baudot framer:
                char ch = baudot_framer_push(&g_rtty.framer, bit_val);

                if (ch > 0) {
                    // Проверка инверсии:
                    inversion_update(&g_rtty.inv_det, ch,
                                     &g_rtty.try_invert);
                    uint8_t quality = (uint8_t)
                        fminf(100.f, fabsf(bit_val) / 1.5f * 100.f);
                    core0_char_push(ch, quality);
                }
            }
        }

        // Обновить состояние для экрана (каждые 10 блоков = 267 мс):
        if (++g_rtty.state_update_count >= 10) {
            g_rtty.state_update_count = 0;
            update_display_state();
        }
    }
}
```

---

## 12. ПРОЦЕДУРА НАСТРОЙКИ С ГЕНЕРАТОРОМ

```
ЦЕЛЬ: убедиться что каждый этап DSP работает правильно
перед подключением реального приёмника.

ШАГ 1 — Проверка АЦП:
  Генератор: тон 1000 Гц, уровень средний
  Смотрим: raw adc_buf через UART или на экране
  Ожидаем: синусоида, размах 1000-3000 отсчётов
  Если размах < 200: поднять уровень потенциометром

ШАГ 2 — Проверка AGC:
  Генератор: тот же тон
  Смотрим: g_dec.mark_level на экране (или UART)
  Ожидаем: g_rtty.agc.gain ≈ 0.5..3.0
  Если gain > 20: уровень слишком низкий
  Если gain < 0.1: уровень слишком высокий

ШАГ 3 — Проверка I/Q демодулятора:
  Генератор: Mark тон 2295 Гц
  Смотрим: g_dec.mark_level vs g_dec.space_level
  Ожидаем: mark_level >> space_level (ratio > 10:1)
  Затем: Space тон 2125 Гц → space_level >> mark_level

ШАГ 4 — Проверка RTTY:
  Генератор: RTTY 45.45 Бод, 170 Гц shift, "RYRYRY..."
  Ожидаем: символы R и Y чередуются на экране
  Признак работы: SNR > 15 дБ, DPLL locked, символы стабильны

ШАГ 5 — Тест DWD SYNOP (после ШАГ 4):
  Генератор: RTTY 50 Бод, 450 Гц shift, числовые группы
  Установить пресет: DWD450
  Ожидаем: числовые группы вида "00000 11111" на экране
  Если мусор: нажать INVERT → должны пойти числа
  AFC должен lock за 3-5 секунд

ШАГ 6 — Тест устойчивости:
  Постепенно убавлять уровень генератора
  Фиксируем: при каком SNR начинаются ошибки
  Хорошо: ошибки появляются при SNR < 6 дБ
  Плохо: ошибки при SNR > 12 дБ (что-то не так)
```

---

## 13. ПАРАМЕТРЫ В config.h

```c
// === ADC ===
#define ADC_GPIO             26
#define ADC_FS_PHYS          38400U    // Гц физически (×4 oversample)
#define ADC_FS_WORK          9600U     // Гц рабочая (после децимации)
#define ADC_PHYS_BLOCK       1024U     // сэмплов DMA блока
#define ADC_WORK_BLOCK       256U      // сэмплов после децимации
#define ADC_CLKDIV           1249.0f   // 48МГц / 1250 = 38400 Гц точно

// === AGC ===
#define AGC_TARGET_RMS       0.30f     // цель = -10 дБFS
#define AGC_ATTACK_MS        10.0f
#define AGC_RELEASE_MS       500.0f

// === ДЕМОДУЛЯТОР ===
#define DEMOD_LPF_K_NORMAL   0.75f    // shift > 150 Гц
#define DEMOD_LPF_K_NARROW   0.38f    // shift 85 Гц (DWD LW)

// === ATC ===
#define ATC_ATTACK_SYMS      2.0f
#define ATC_RELEASE_SYMS     16.0f
#define ATC_CLIP             1.5f

// === DPLL ===
#define DPLL_ALPHA           0.035f
#define DPLL_BETA_FACTOR     0.5f      // beta = alpha² × factor
#define DPLL_MAX_FREQ_PCT    0.05f     // ±5% baud rate

// === AFC ===
#define AFC_MAX_STEP_HZ      2.0f
#define AFC_ALPHA            0.10f
#define AFC_MIN_SNR_DB       4.0f
#define AFC_UPDATE_BLOCKS    8         // каждые N блоков

// === FFT / Welch ===
#define FFT_SIZE_NORMAL      512
#define FFT_SIZE_NARROW      1024      // для shift < 150 Гц
#define WELCH_FRAMES         8

// === ИНВЕРСИЯ ===
#define INV_DETECT_WINDOW    40        // символов для оценки
#define INV_DETECT_THRESHOLD 0.30f     // < 30% valid → инвертировать

// === BAUD AUTODECT ===
#define BAUD_HIST_MIN_SAMPLES 200
```

---

*ТЗ DSP Decoder v2.0 | RP2350A Core 0 @ 300 МГц*
*Все скорости: 45.45 / 50 / 75 / 100 Бод*
*Все режимы: HAM 170 Гц / DWD 450 Гц / DWD 85 Гц / Commercial 850 Гц*
*Алгоритмы: Kok Chen W7AY ATC + fldigi I/Q baseband + DPLL*
