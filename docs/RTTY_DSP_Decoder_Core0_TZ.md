# ТЕХНИЧЕСКОЕ ЗАДАНИЕ: RTTY DSP ДЕКОДЕР
## RP2350 Core 0 — Профессиональный приём уровня fldigi
## Версия 1.0 | Основано на алгоритмах Kok Chen W7AY + fldigi

---

## ВВЕДЕНИЕ: ЧТО ДЕЛАЕТ FLDIGI ИНАЧЕ ЧЕМ ПРОСТЫЕ ДЕКОДЕРЫ

Fldigi использует дизайн основанный на теоретических работах Kok Chen W7AY.
Тоны Mark и Space переводятся на базовую частоту и фильтруются через
низкочастотный фильтр — вариант Enhanced Nyquist Filter Чена,
реализованный через Fast Overlap-and-Add FFT.

Оптимальное демодулирование FSK при AWGN использует Matched Filter.
Matched filter для прямоугольных импульсов широкополосен и восприимчив
к помехам от соседних сигналов. Поэтому фильтры часто реализуют узкополосными,
удовлетворяющими критерию Найквиста для избежания ISI.

ATC (Automatic Threshold Correction) — техника, оптимизирующая порог
решения в присутствии selective fading (избирательного замирания).
Детектор огибающей отслеживает амплитуду несущей при замирании.

**Три ключевых отличия профессионального декодера:**
1. **Квадратурный (I/Q) демодулятор** вместо простого BPF детектора
2. **ATC (Auto Threshold Correction)** для устойчивости при замираниях
3. **DPLL синхронизатор** с коррекцией по нулевым пересечениям

---

## ЧАСТЬ 1: АРХИТЕКТУРА CORE 0

### 1.1 Разделение задач между ядрами

```
CORE 0 (DSP — приоритет MAX):          CORE 1 (UI — приоритет LOW):
  ADC DMA IRQ handler                    Waterfall render
  Decimation FIR                         Touch polling
  AGC                                    Text display
  I/Q Demodulator (Mark + Space)         Menu
  ATC threshold corrector                ← получает данные из ring buffer
  DPLL bit synchronizer
  Baudot framer + ITA2 decode
  FFT for AFC (каждые 512мс)
  → пишет символы в ring buffer →
```

### 1.2 Межъядерная коммуникация

```c
// Кольцевой буфер символов Core0 → Core1:
#define CHAR_RING_SIZE 256
typedef struct {
    char     ch;        // ASCII символ
    uint32_t timestamp; // мс с запуска
    uint8_t  quality;   // 0-100% качество бита
} decoded_char_t;

static decoded_char_t char_ring[CHAR_RING_SIZE];
static volatile uint32_t char_ring_write = 0;
static volatile uint32_t char_ring_read  = 0;

// Core0 пишет:
void ring_push(char ch, uint8_t quality) {
    uint32_t next = (char_ring_write + 1) % CHAR_RING_SIZE;
    if (next != char_ring_read) {  // не переполнено
        char_ring[char_ring_write] = (decoded_char_t){ch, to_ms_since_boot(get_absolute_time()), quality};
        __dmb();  // memory barrier (RP2350 multi-core)
        char_ring_write = next;
    }
}

// Core1 читает:
bool ring_pop(decoded_char_t *out) {
    if (char_ring_read == char_ring_write) return false;
    __dmb();
    *out = char_ring[char_ring_read];
    char_ring_read = (char_ring_read + 1) % CHAR_RING_SIZE;
    return true;
}

// Состояние декодера (Core0 пишет, Core1 читает):
typedef struct {
    float    mark_hz, space_hz, center_hz, shift_hz;
    float    snr_db;
    float    mark_level, space_level;   // 0..1
    float    baud_rate;
    float    afc_error_hz;             // текущая ошибка AFC
    float    dpll_phase;               // фаза синхронизатора 0..1
    bool     signal_present;
    bool     afc_locked;
    char     quality_label[8];         // "POOR/FAIR/GOOD/EXCEL"
} decoder_state_t;

static volatile decoder_state_t g_state;
```

---

## ЧАСТЬ 2: ВХОДНОЙ ТРАКТ ADC + ДЕЦИМАЦИЯ

### 2.1 ADC DMA — Ping-Pong буферы

```c
#define ADC_FS_PHYSICAL   38400   // реальная частота АЦП (×4 oversample)
#define ADC_FS_WORK       9600    // после децимации
#define ADC_BLOCK_SIZE    256     // сэмплов на блок (26.7 мс при 9600 Гц)
#define ADC_PHYS_BLOCK    (ADC_BLOCK_SIZE * 4)  // 1024 сэмпла физически

static uint16_t adc_buf[2][ADC_PHYS_BLOCK];  // ping-pong
static volatile int  adc_buf_ready = -1;      // номер готового буфера
static int adc_dma_ch[2];

void adc_dma_init(void) {
    adc_init();
    adc_gpio_init(26);
    adc_select_input(0);
    adc_fifo_setup(true, true, 1, false, false);

    // Точное тактирование через PIO SM1:
    // При 300МГц: clkdiv = 300e6 / 38400 = 7812.5
    adc_set_clkdiv(7812.5f - 1.0f);

    for (int i = 0; i < 2; i++) {
        adc_dma_ch[i] = dma_claim_unused_channel(true);
        dma_channel_config dc = dma_channel_get_default_config(adc_dma_ch[i]);
        channel_config_set_transfer_data_size(&dc, DMA_SIZE_16);
        channel_config_set_read_increment(&dc, false);
        channel_config_set_write_increment(&dc, true);
        channel_config_set_dreq(&dc, DREQ_ADC);
        channel_config_set_chain_to(&dc, adc_dma_ch[i ^ 1]);  // chain к другому
        dma_channel_configure(adc_dma_ch[i], &dc,
            adc_buf[i], &adc_hw->fifo, ADC_PHYS_BLOCK, false);
        dma_channel_set_irq0_enabled(adc_dma_ch[i], true);
    }

    irq_set_exclusive_handler(DMA_IRQ_0, adc_dma_irq);
    irq_set_priority(DMA_IRQ_0, 0);  // НАИВЫСШИЙ приоритет
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_start(adc_dma_ch[0]);
    adc_run(true);
}

void adc_dma_irq(void) {
    for (int i = 0; i < 2; i++) {
        if (dma_channel_get_irq0_status(adc_dma_ch[i])) {
            dma_channel_acknowledge_irq0(adc_dma_ch[i]);
            adc_buf_ready = i;
            return;
        }
    }
}
```

### 2.2 DC Блокировка + Децимация

```c
// DC блокировка — IIR first-order HPF, Fc ≈ 20 Гц:
static float dc_state = 0.0f;
#define DC_ALPHA  0.99580f   // = 1 - 2π×20/38400

static inline float dc_block(float x) {
    float y = x - dc_state;
    dc_state = dc_state * DC_ALPHA + x * (1.0f - DC_ALPHA);
    return y;
}

// Децимирующий FIR ×4 (CMSIS-DSP):
// 48 коэффициентов, окно Кайзера β=7, Fc = 4800/38400 = 0.125
// Проектируется offline и хранится в Flash:
static const float decim_coeffs[48] = {
    // коэффициенты рассчитать через scipy.signal.firwin:
    // scipy.signal.firwin(48, 0.125, window=('kaiser', 7))
    // и вставить здесь
};

static arm_fir_decimate_instance_f32 decim_inst;
static float decim_state[48 + 4 - 1];

void decimation_init(void) {
    arm_fir_decimate_init_f32(&decim_inst, 48, 4,
        decim_coeffs, decim_state, ADC_PHYS_BLOCK);
}

// Обработка одного физического блока → decimated блок:
void process_adc_block(uint16_t *raw, float *decimated) {
    float float_buf[ADC_PHYS_BLOCK];

    // ADC 12-бит → float [-1, +1], DC блокировка:
    for (int i = 0; i < ADC_PHYS_BLOCK; i++) {
        float s = ((float)raw[i] - 2048.0f) / 2048.0f;
        float_buf[i] = dc_block(s);
    }

    // Децимация ×4 → ADC_BLOCK_SIZE float сэмплов:
    arm_fir_decimate_f32(&decim_inst, float_buf,
                          decimated, ADC_PHYS_BLOCK);
}
```

---

## ЧАСТЬ 3: ЦИФРОВОЙ AGC

```c
// Профессиональный AGC с раздельными константами атаки/спада
// Атака быстрая (не допускает перегрузки фильтров)
// Спад медленный (не "помпирует" на паузах)

typedef struct {
    float gain;
    float target;     // целевой RMS = 0.3 (-10 дБFS)
    float attack;     // exp(-1 / (0.010 × fs)) — 10 мс
    float release;    // exp(-1 / (0.500 × fs)) — 500 мс
    float rms;        // скользящий RMS
    float rms_tc;     // TC для RMS: 50 мс
} agc_t;

void agc_init(agc_t *a, float fs) {
    a->gain    = 1.0f;
    a->target  = 0.30f;
    a->attack  = expf(-1.0f / (0.010f * fs));
    a->release = expf(-1.0f / (0.500f * fs));
    a->rms_tc  = expf(-1.0f / (0.050f * fs));
    a->rms     = 0.01f;
}

float agc_process(agc_t *a, float x) {
    float out = x * a->gain;
    // Обновить RMS (скользящее среднеквадратическое):
    a->rms = a->rms * a->rms_tc +
             out * out * (1.0f - a->rms_tc);
    float rms_now = sqrtf(a->rms + 1e-10f);

    // Регулировка усиления:
    if (rms_now > a->target) {
        a->gain *= a->attack;    // перегруз — быстро снижаем
    } else {
        a->gain /= a->release;   // тишина — медленно поднимаем
    }
    a->gain = fmaxf(0.01f, fminf(a->gain, 200.0f));
    return out;
}
```

---

## ЧАСТЬ 4: КВАДРАТУРНЫЙ FSK ДЕМОДУЛЯТОР (ЯДРО АЛГОРИТМА fldigi)

### 4.1 Принцип — Baseband I/Q демодуляция

Квадратурный смешанный baseband подход часто используется для программных
FSK демодуляторов — детектор близок к оптимальному. Сигнал умножается на
опорные sin и cos целевой частоты, затем проходит через LPF,
вычисляется мощность P = I² + Q².

```
Входной сигнал s(t) — аудио после AGC
        │
   ┌────┴────┐
   ▼         ▼
×cos(2π·fm·t)  ×sin(2π·fm·t)    ← Mark гетеродин
   │         │
 [LPF]     [LPF]               ← Enhanced Nyquist Filter
   │         │
  I_m       Q_m
   │         │
   └────┬────┘
        ▼
   P_mark = I_m² + Q_m²        ← мощность Mark тона
   ENV_mark = sqrt(P_mark)     ← огибающая

   (аналогично для Space частоты → ENV_space)
        │
        ▼
   diff = ENV_mark - ENV_space  ← дискриминатор
        │
      [ATC]                    ← коррекция порога
        │
        ▼
   bit = diff > 0 ? MARK : SPACE
```

### 4.2 Структура демодулятора

```c
typedef struct {
    // Фазы гетеродинов (накопленная фаза):
    float mark_phase;
    float space_phase;
    float mark_phase_step;   // = 2π × mark_hz / fs
    float space_phase_step;

    // I/Q состояния LPF (Mark канал):
    float mi_z1, mi_z2;   // biquad состояния I
    float mq_z1, mq_z2;   // biquad состояния Q
    // I/Q состояния LPF (Space канал):
    float si_z1, si_z2;
    float sq_z1, sq_z2;

    // Biquad коэффициенты LPF (Raised Cosine / Enhanced Nyquist):
    float b0, b1, b2, a1, a2;

    // ATC огибающие (медленные):
    float mark_env;        // текущая огибающая Mark
    float space_env;       // текущая огибающая Space
    float mark_atc;        // ATC уровень Mark (slow follower)
    float space_atc;       // ATC уровень Space (slow follower)
    float atc_attack;      // быстрая атака огибающей ATC
    float atc_release;     // медленный спад ATC

    // Параметры:
    float mark_hz, space_hz;
    float baud_rate, fs;
} iq_demod_t;
```

### 4.3 Проектирование LPF — Enhanced Nyquist / Raised Cosine

Raised Cosine фильтр удовлетворяет критерию Найквиста для ISI-free демодуляции.
Для 45.45 бод фильтр с наименьшей полосой — Raised Cosine с отсечкой 22.7 Гц.
На практике нужен баланс между подавлением ISI и отклонением помех соседнего канала.

Extended Nyquist Filters имеют полосу между Matched Filter и Raised Cosine.
2nd order Extended Raised Cosine практически совпадает с Matched Filter по
характеристике ошибок, но с лучшей устойчивостью к помехам.

```c
// Полоса пропускания LPF после квадратурного смешивания:
// Fc = baud_rate × K, где K:
//   K = 0.5   — Raised Cosine (минимум ISI, хуже при шуме)
//   K = 0.75  — Extended RC 2nd order (хороший компромисс) ← ВЫБРАТЬ
//   K = 1.0   — Matched Filter (оптимально при AWGN)

#define LPF_K  0.75f

float lpf_cutoff_hz(float baud_rate) {
    return baud_rate * LPF_K;
}

// Biquad LPF проектирование (Butterworth 2nd order):
void design_lpf_biquad(float fc, float fs,
                        float *b0, float *b1, float *b2,
                        float *a1, float *a2) {
    float w0    = 2.0f * M_PI * fc / fs;
    float alpha = sinf(w0) / (2.0f * 0.7071f); // Q = 1/√2 (Butterworth)
    float cosw0 = cosf(w0);

    float B0 =  (1.0f - cosw0) * 0.5f;
    float B1 =   1.0f - cosw0;
    float B2 =  (1.0f - cosw0) * 0.5f;
    float A0 =   1.0f + alpha;
    float A1 =  -2.0f * cosw0;
    float A2 =   1.0f - alpha;

    *b0 = B0 / A0;  *b1 = B1 / A0;  *b2 = B2 / A0;
    *a1 = A1 / A0;  *a2 = A2 / A0;
}

// Применение biquad (Direct Form II Transposed — минимум состояний):
static inline float biquad_df2t(float x,
                                  float b0, float b1, float b2,
                                  float a1, float a2,
                                  float *z1, float *z2) {
    float y = b0 * x + *z1;
    *z1 = b1 * x - a1 * y + *z2;
    *z2 = b2 * x - a2 * y;
    return y;
}
```

### 4.4 Инициализация и перестройка демодулятора

```c
void iq_demod_init(iq_demod_t *d,
                    float mark_hz, float space_hz,
                    float baud_rate, float fs) {
    d->mark_hz    = mark_hz;
    d->space_hz   = space_hz;
    d->baud_rate  = baud_rate;
    d->fs         = fs;

    // Шаги фазы гетеродинов:
    d->mark_phase_step  = 2.0f * M_PI * mark_hz  / fs;
    d->space_phase_step = 2.0f * M_PI * space_hz / fs;
    d->mark_phase = d->space_phase = 0.0f;

    // LPF проектирование:
    float fc = lpf_cutoff_hz(baud_rate);
    design_lpf_biquad(fc, fs,
        &d->b0, &d->b1, &d->b2, &d->a1, &d->a2);

    // Сброс состояний:
    d->mi_z1 = d->mi_z2 = 0.0f;
    d->mq_z1 = d->mq_z2 = 0.0f;
    d->si_z1 = d->si_z2 = 0.0f;
    d->sq_z1 = d->sq_z2 = 0.0f;

    // ATC времянные константы (период символа):
    // Атака: ~2 символа; Спад: ~8 символов
    float sym_period = fs / baud_rate;
    d->atc_attack  = expf(-1.0f / (2.0f * sym_period));
    d->atc_release = expf(-1.0f / (8.0f * sym_period));
    d->mark_env = d->space_env = 0.1f;
    d->mark_atc = d->space_atc = 0.1f;
}

// Вызывать при смене частот или скорости:
void iq_demod_retune(iq_demod_t *d,
                      float mark_hz, float space_hz,
                      float baud_rate) {
    iq_demod_init(d, mark_hz, space_hz, baud_rate, d->fs);
}
```

### 4.5 Обработка одного сэмпла

```c
// Возвращает: +1.0 = Mark, -1.0 = Space, 0 = неопределённо (до ATC)
float iq_demod_process(iq_demod_t *d, float sample) {
    float ci, cq, mi, mq, si, sq;

    // === MARK КАНАЛ ===
    // Умножаем на cos и sin гетеродина Mark:
    ci = cosf(d->mark_phase);   // в продакшне — lookup table
    cq = sinf(d->mark_phase);
    mi = sample * ci;
    mq = sample * cq;
    d->mark_phase += d->mark_phase_step;
    if (d->mark_phase > 2.0f * M_PI) d->mark_phase -= 2.0f * M_PI;

    // LPF для I и Q:
    mi = biquad_df2t(mi, d->b0,d->b1,d->b2, d->a1,d->a2,
                     &d->mi_z1, &d->mi_z2);
    mq = biquad_df2t(mq, d->b0,d->b1,d->b2, d->a1,d->a2,
                     &d->mq_z1, &d->mq_z2);
    float mark_power = mi*mi + mq*mq;

    // === SPACE КАНАЛ ===
    ci = cosf(d->space_phase);
    cq = sinf(d->space_phase);
    si = sample * ci;
    sq = sample * cq;
    d->space_phase += d->space_phase_step;
    if (d->space_phase > 2.0f * M_PI) d->space_phase -= 2.0f * M_PI;

    si = biquad_df2t(si, d->b0,d->b1,d->b2, d->a1,d->a2,
                     &d->si_z1, &d->si_z2);
    sq = biquad_df2t(sq, d->b0,d->b1,d->b2, d->a1,d->a2,
                     &d->sq_z1, &d->sq_z2);
    float space_power = si*si + sq*sq;

    // Огибающие (без sqrt — используем мощность напрямую):
    d->mark_env  = mark_power;
    d->space_env = space_power;

    return mark_power - space_power;  // до ATC
}
```

---

## ЧАСТЬ 5: ATC — AUTOMATIC THRESHOLD CORRECTION

ATC корректирует любой дисбаланс амплитуд Mark и Space
до порогового детектора. Реализация: пара детекторов огибающей отслеживает
амплитуду несущей при замирании. Константы времени выбраны достаточно
медленными чтобы огибающая не отслеживала отдельные биты данных.

```c
// ATC нормализует дискриминатор так чтобы Mark и Space
// имели одинаковую «ожидаемую» амплитуду.
// Это позволяет копировать сигнал при selective fading:
// когда один тон замирает — ATC сдвигает порог.

typedef struct {
    float mark_env;    // медленная огибающая Mark  (Fast Attack, Slow Release)
    float space_env;   // медленная огибающая Space
    float fast_tc;     // = exp(-1 / (2 × T_symbol))  — атака
    float slow_tc;     // = exp(-1 / (16 × T_symbol)) — спад
    float clip_level;  // уровень ограничителя (опц., улучшает ~0.5 дБ)
} atc_t;

void atc_init(atc_t *a, float baud_rate, float fs) {
    float T = fs / baud_rate;
    a->fast_tc  = expf(-1.0f / (2.0f  * T));
    a->slow_tc  = expf(-1.0f / (16.0f * T));
    a->mark_env = a->space_env = 0.01f;
    a->clip_level = 1.5f;  // ограничитель ~3.5 дБ над нормой
}

// ATC обработка дискриминатора:
// raw_diff = mark_power - space_power (из iq_demod)
float atc_process(atc_t *a, float mark_power, float space_power) {
    // Обновить медленные огибающие (FASR: Fast Attack, Slow Release):
    float new_m = sqrtf(mark_power  + 1e-10f);
    float new_s = sqrtf(space_power + 1e-10f);

    if (new_m > a->mark_env)
        a->mark_env = a->mark_env * a->fast_tc + new_m * (1.0f - a->fast_tc);
    else
        a->mark_env = a->mark_env * a->slow_tc + new_m * (1.0f - a->slow_tc);

    if (new_s > a->space_env)
        a->space_env = a->space_env * a->fast_tc + new_s * (1.0f - a->fast_tc);
    else
        a->space_env = a->space_env * a->slow_tc + new_s * (1.0f - a->slow_tc);

    // Нормализованный дискриминатор:
    // Вместо (mark - space), вычисляем:
    // (mark/mark_env - space/space_env)
    // = отношения к ожидаемым уровням → инвариантен к fading
    float m_norm = new_m / (a->mark_env  + 1e-6f);
    float s_norm = new_s / (a->space_env + 1e-6f);

    float disc = m_norm - s_norm;

    // Опциональный ограничитель (улучшает ~0.5 дБ SNR):
    // disc = tanhf(disc × a->clip_level); // мягкое ограничение
    disc = fmaxf(-a->clip_level, fminf(a->clip_level, disc));

    return disc;  // >0 = Mark, <0 = Space
}
```

---

## ЧАСТЬ 6: DPLL — СИНХРОНИЗАТОР БИТОВ

DPLL алгоритм используется для битовой синхронизации, позволяя
делать решение в середине бита. Алгоритм использует счётчик DPLLBitPhase.
Синхронизация: при обнаружении смены бита проверяется произошла ли она
в начале или конце символа. Если смена раньше середины — увеличиваем счётчик
(поздно), если позже — уменьшаем (рано).

```c
// DPLL (Digital Phase-Locked Loop) для синхронизации битов
// Работает на выходе ATC дискриминатора (float поток)

typedef struct {
    float   phase;          // накопленная фаза 0..1
    float   period;         // период символа в сэмплах = fs / baud
    float   alpha;          // gain петлевого фильтра (0.01..0.05)
    float   beta;           // integral gain (alpha²/2)
    float   freq_error;     // накопленная частотная ошибка
    float   prev_disc;      // предыдущий дискриминатор (для детекта перехода)
    int     sample_count;   // счётчик сэмплов
    bool    bit_ready;      // флаг — сэмпл в середине бита готов
    float   bit_sample;     // значение в момент решения
} dpll_t;

void dpll_init(dpll_t *p, float baud_rate, float fs) {
    p->period      = fs / baud_rate;
    p->phase       = 0.5f;   // начать с середины первого бита
    p->alpha       = 0.035f; // ширина петли захвата
    p->beta        = p->alpha * p->alpha / 2.0f;
    p->freq_error  = 0.0f;
    p->prev_disc   = 0.0f;
    p->sample_count = 0;
    p->bit_ready   = false;
}

// Обработка одного сэмпла дискриминатора:
// Возвращает true когда накоплен полный символ (делать решение)
bool dpll_process(dpll_t *p, float disc, float *bit_out) {
    p->phase += 1.0f / p->period + p->freq_error;
    p->bit_ready = false;

    // Детект перехода бита (zero-crossing):
    bool transition = (disc * p->prev_disc < 0.0f);
    p->prev_disc = disc;

    if (transition) {
        // Ошибка тайминга: насколько фаза отличается от 0 или 0.5
        // (переход должен быть на краях символа: phase ≈ 0 или ≈ 1)
        float phase_error;
        if (p->phase < 0.5f) {
            phase_error =  p->phase;         // слишком рано
        } else {
            phase_error = p->phase - 1.0f;   // слишком поздно
        }

        // Ограничить коррекцию (не рывками):
        phase_error = fmaxf(-0.1f, fminf(0.1f, phase_error));

        // Петлевой фильтр (P + I):
        p->phase       -= p->alpha * phase_error;
        p->freq_error  -= p->beta  * phase_error;

        // Ограничить частотную ошибку (±5%):
        p->freq_error = fmaxf(-0.05f / p->period,
                        fminf( 0.05f / p->period, p->freq_error));
    }

    // Решение в середине символа (phase переходит через 1.0):
    if (p->phase >= 1.0f) {
        p->phase -= 1.0f;
        *bit_out = disc;      // значение дискриминатора в центре символа
        p->bit_ready = true;
        return true;
    }
    return false;
}
```

---

## ЧАСТЬ 7: BAUDOT ФРЕЙМЕР + ITA2 ДЕКОДЕР

```c
// Автомат состояний приёма символа:
// [ждём старт] → [принимаем 5 бит] → [проверяем стоп] → [декодируем]

typedef enum {
    FRAME_WAIT_START = 0,
    FRAME_RECV_DATA,
    FRAME_RECV_STOP
} frame_state_t;

typedef struct {
    frame_state_t state;
    uint8_t       shift_reg;   // накапливаем 5 бит
    int           bit_count;
    bool          figs_mode;   // текущий регистр: буквы или цифры
    float         stop_acc;    // накопитель стопового бита
    int           stop_count;
    float         baud_rate;
    float         stop_bits;   // 1.0 или 1.5
} baudot_framer_t;

// Полная таблица ITA2:
static const char baudot_ltrs[32] = {
    '\0','E','\n','A',' ','S','I','U',
    '\r','D','R','J','N','F','C','K',
    'T', 'Z','L', 'W','H','Y','P','Q',
    'O', 'B','G','\0','M','X','V','\0'
};
static const char baudot_figs[32] = {
    '\0','3','\n','-',' ','\'','8','7',
    '\r','$','4','\a',',','!',':','(',
    '5', '"',')','2','#','6','0','1',
    '9', '?','&','\0','.','/',';','\0'
};
#define BAUDOT_LTRS  0x1F
#define BAUDOT_FIGS  0x1B

void baudot_framer_init(baudot_framer_t *f,
                         float baud_rate, float stop_bits) {
    f->state     = FRAME_WAIT_START;
    f->shift_reg = 0;
    f->bit_count = 0;
    f->figs_mode = false;
    f->stop_acc  = 0.0f;
    f->stop_count = 0;
    f->baud_rate = baud_rate;
    f->stop_bits = stop_bits;
}

// Принять один бит (из DPLL):
// bit_value: float >0 = Mark(1), <0 = Space(0)
// Возвращает ASCII символ или 0 если ещё не готов
char baudot_framer_push(baudot_framer_t *f, float bit_value) {
    int bit = (bit_value > 0.0f) ? 1 : 0;

    switch (f->state) {
        case FRAME_WAIT_START:
            // Стартовый бит = Space (0):
            if (bit == 0) {
                f->state     = FRAME_RECV_DATA;
                f->shift_reg = 0;
                f->bit_count = 0;
            }
            break;

        case FRAME_RECV_DATA:
            // LSB first:
            f->shift_reg |= (bit << f->bit_count);
            f->bit_count++;
            if (f->bit_count >= 5) {
                f->state      = FRAME_RECV_STOP;
                f->stop_acc   = bit_value;  // первый стоп-бит
                f->stop_count = 1;
            }
            break;

        case FRAME_RECV_STOP:
            // Накапливаем стоп-биты (1 или 1.5):
            f->stop_acc += bit_value;
            f->stop_count++;

            int stop_needed = (f->stop_bits >= 1.5f) ? 2 : 1;
            if (f->stop_count >= stop_needed) {
                f->state = FRAME_WAIT_START;

                // Проверить стоп-бит:
                if (f->stop_acc / f->stop_count < 0.0f) {
                    return '?';  // ошибка фрейма
                }

                // Декодировать символ:
                uint8_t code = f->shift_reg & 0x1F;
                char ch = 0;

                if (code == BAUDOT_LTRS) {
                    f->figs_mode = false;
                    return 0;   // служебный — не выводим
                } else if (code == BAUDOT_FIGS) {
                    f->figs_mode = true;
                    return 0;
                } else {
                    ch = f->figs_mode ?
                         baudot_figs[code] :
                         baudot_ltrs[code];
                    return ch ? ch : 0;
                }
            }
            break;
    }
    return 0;
}
```

---

## ЧАСТЬ 8: AFC — АВТОМАТИЧЕСКАЯ ПОДСТРОЙКА ЧАСТОТЫ

### 8.1 Принцип AFC на фазовой ошибке I/Q

```c
// Точная AFC через измерение частотной ошибки демодулятора:
// Если Mark не точно на mark_hz → I и Q не равны нулю одновременно
// Фазовая ошибка = atan2(Q, I) после LPF — прямо пропорциональна Δf

// ДОПОЛНИТЕЛЬНЫЙ AFC на основе FFT (каждые 512мс):
// Накапливаем 4800 сэмплов (0.5 сек при 9600 Гц)
// Берём FFT 4096pt (разрешение 9600/4096 = 2.34 Гц/бин)
// Параболическая интерполяция → точность ±0.5 Гц

typedef struct {
    float   mark_hz, space_hz;
    float   max_step_hz;     // макс. коррекция за шаг = 2.0 Гц
    float   bandwidth_hz;    // полоса захвата = shift/2
    float   alpha;           // сглаживание коррекции
    float   accum_error;     // накопленная ошибка
    bool    enabled;
    bool    locked;
    uint32_t lock_count;     // счётчик символов после lock
} afc_t;

void afc_init(afc_t *a, float mark_hz, float space_hz) {
    a->mark_hz      = mark_hz;
    a->space_hz     = space_hz;
    a->max_step_hz  = 2.0f;
    a->bandwidth_hz = (mark_hz - space_hz) / 2.0f;
    a->alpha        = 0.1f;
    a->accum_error  = 0.0f;
    a->enabled      = true;
    a->locked       = false;
    a->lock_count   = 0;
}

// Вызывать из FFT-ветки каждые 512мс:
// peak_mark_hz, peak_space_hz — найденные параболической интерполяцией
void afc_update_from_fft(afc_t *a, float peak_mark_hz,
                           float peak_space_hz,
                           float snr_db,
                           iq_demod_t *demod, atc_t *atc) {
    if (!a->enabled || snr_db < 4.0f) return;

    // Проверить разумность shift:
    float found_shift = peak_mark_hz - peak_space_hz;
    float expected_shift = a->mark_hz - a->space_hz;
    if (fabsf(found_shift - expected_shift) > expected_shift * 0.15f)
        return;  // shift слишком отличается — не применяем

    // Ошибка = средняя ошибка Mark и Space:
    float err_m = peak_mark_hz  - a->mark_hz;
    float err_s = peak_space_hz - a->space_hz;
    float error = (err_m + err_s) * 0.5f;

    // Сглаживание:
    a->accum_error = a->accum_error * (1.0f - a->alpha) +
                     error * a->alpha;

    // Ограничить шаг:
    float step = fmaxf(-a->max_step_hz,
                 fminf( a->max_step_hz, a->accum_error));

    // Применить коррекцию:
    a->mark_hz  += step;
    a->space_hz += step;

    // Перестроить демодулятор (LPF коэфф. не меняются, только фазы):
    demod->mark_phase_step  = 2.0f * M_PI * a->mark_hz  / demod->fs;
    demod->space_phase_step = 2.0f * M_PI * a->space_hz / demod->fs;

    a->locked = (fabsf(a->accum_error) < 2.0f);
    if (a->locked) a->lock_count++;

    // Обновить глобальное состояние:
    g_state.mark_hz    = a->mark_hz;
    g_state.space_hz   = a->space_hz;
    g_state.center_hz  = (a->mark_hz + a->space_hz) * 0.5f;
    g_state.afc_locked = a->locked;
    g_state.afc_error_hz = a->accum_error;
}
```

---

## ЧАСТЬ 9: SNR ИЗМЕРИТЕЛЬ

```c
// Профессиональная оценка SNR через спектральный анализ (Welch):
// Мощность в полосе Mark+Space / мощность вне полосы

typedef struct {
    float  accum[512/2];    // накопитель FFT
    int    frame_count;
    float  snr_db;
    float  mark_level;
    float  space_level;
    char   quality[8];
} snr_meter_t;

void snr_update(snr_meter_t *s, float *fft_mag, int bins,
                float mark_hz, float space_hz,
                float baud_rate, float bin_hz) {

    int m_lo = (int)((mark_hz  - baud_rate * 0.7f) / bin_hz);
    int m_hi = (int)((mark_hz  + baud_rate * 0.7f) / bin_hz);
    int s_lo = (int)((space_hz - baud_rate * 0.7f) / bin_hz);
    int s_hi = (int)((space_hz + baud_rate * 0.7f) / bin_hz);

    float sig_power = 0, noise_power = 0;
    int   noise_count = 0, sig_count = 0;

    for (int i = 1; i < bins; i++) {
        float p = fft_mag[i] * fft_mag[i];
        if ((i >= m_lo && i <= m_hi) || (i >= s_lo && i <= s_hi)) {
            sig_power += p;
            sig_count++;
        } else {
            noise_power += p;
            noise_count++;
        }
    }
    if (noise_count == 0 || sig_count == 0) return;

    // Нормализовать шум к той же полосе:
    float noise_norm = noise_power / noise_count * sig_count;
    float snr = sig_power / (noise_norm + 1e-10f);
    s->snr_db = 10.0f * log10f(snr);

    // Mark и Space уровни:
    float m_sum = 0, s_sum = 0;
    for (int i = m_lo; i <= m_hi && i < bins; i++) m_sum += fft_mag[i];
    for (int i = s_lo; i <= s_hi && i < bins; i++) s_sum += fft_mag[i];
    s->mark_level  = m_sum / (m_hi - m_lo + 1);
    s->space_level = s_sum / (s_hi - s_lo + 1);

    if      (s->snr_db > 15) strcpy(s->quality, "EXCEL");
    else if (s->snr_db > 10) strcpy(s->quality, "GOOD");
    else if (s->snr_db >  6) strcpy(s->quality, "FAIR");
    else                     strcpy(s->quality, "POOR");

    g_state.snr_db       = s->snr_db;
    g_state.mark_level   = s->mark_level;
    g_state.space_level  = s->space_level;
    memcpy(g_state.quality_label, s->quality, 8);
}
```

---

## ЧАСТЬ 10: ГЛАВНЫЙ ЦИКЛ CORE 0

```c
// Все модули в единой структуре состояния:
typedef struct {
    iq_demod_t     demod;
    atc_t          atc;
    dpll_t         dpll;
    baudot_framer_t framer;
    agc_t          agc;
    afc_t          afc;
    snr_meter_t    snr;
    arm_rfft_fast_instance_f32 fft;
    float          decimated[ADC_BLOCK_SIZE];
    float          fft_buf[512];
    float          fft_mag[256];
    float          welch_accum[256];
    int            welch_count;
    uint32_t       last_afc_ms;
    uint32_t       last_snr_ms;
    bool           inverted;   // инверсия Mark/Space
} rtty_core_t;

static rtty_core_t g_rtty;

// Инициализация на Core 0:
void core0_init(float mark_hz, float space_hz, float baud_rate) {
    const float FS = 9600.0f;

    agc_init   (&g_rtty.agc,    FS);
    iq_demod_init(&g_rtty.demod, mark_hz, space_hz, baud_rate, FS);
    atc_init   (&g_rtty.atc,    baud_rate, FS);
    dpll_init  (&g_rtty.dpll,   baud_rate, FS);
    baudot_framer_init(&g_rtty.framer, baud_rate, 1.5f);
    afc_init   (&g_rtty.afc,    mark_hz, space_hz);
    decimation_init();
    adc_dma_init();

    arm_rfft_fast_init_f32(&g_rtty.fft, 512);
    memset(g_rtty.welch_accum, 0, sizeof(g_rtty.welch_accum));
    g_rtty.welch_count = 0;
}

// Обработка одного блока (вызывается из IRQ или tight_loop_contents):
void core0_process_block(int buf_idx) {
    const float FS = 9600.0f;

    // 1. Децимация физического блока:
    process_adc_block(adc_buf[buf_idx], g_rtty.decimated);

    // 2. Поблочный AGC (один gain на весь блок — экономия вычислений):
    float agc_gain = g_rtty.agc.gain;
    for (int i = 0; i < ADC_BLOCK_SIZE; i++) {
        g_rtty.decimated[i] = agc_process(&g_rtty.agc, g_rtty.decimated[i]);
    }

    // 3. FFT для waterfall и AFC (Welch усреднение):
    // Hann окно:
    static float hann[ADC_BLOCK_SIZE];
    static bool hann_init = false;
    if (!hann_init) {
        for (int i = 0; i < ADC_BLOCK_SIZE; i++)
            hann[i] = 0.5f * (1.0f - cosf(2*M_PI*i/(ADC_BLOCK_SIZE-1)));
        hann_init = true;
    }

    float windowed[512] = {0};
    for (int i = 0; i < ADC_BLOCK_SIZE; i++)
        windowed[i] = g_rtty.decimated[i] * hann[i];

    // FFT 512pt:
    float fft_complex[512];
    arm_rfft_fast_f32(&g_rtty.fft, windowed, fft_complex, 0);
    arm_cmplx_mag_f32(fft_complex, g_rtty.fft_mag, 256);

    // Welch накопление (8 кадров):
    arm_add_f32(g_rtty.welch_accum, g_rtty.fft_mag,
                g_rtty.welch_accum, 256);
    g_rtty.welch_count++;
    if (g_rtty.welch_count >= 8) {
        // Отдать waterfall и AFC:
        float avg_fft[256];
        arm_scale_f32(g_rtty.welch_accum, 1.0f/8.0f, avg_fft, 256);
        memset(g_rtty.welch_accum, 0, sizeof(g_rtty.welch_accum));
        g_rtty.welch_count = 0;

        // Обновить SNR:
        snr_update(&g_rtty.snr, avg_fft, 256,
                   g_rtty.afc.mark_hz, g_rtty.afc.space_hz,
                   g_rtty.demod.baud_rate, FS / 512.0f);

        // AFC из FFT:
        float pm = find_peak_parabolic(avg_fft, 256,
                       g_rtty.afc.mark_hz,  80.0f, FS/512.0f);
        float ps = find_peak_parabolic(avg_fft, 256,
                       g_rtty.afc.space_hz, 80.0f, FS/512.0f);
        if (pm > 0 && ps > 0)
            afc_update_from_fft(&g_rtty.afc, pm, ps,
                                g_rtty.snr.snr_db,
                                &g_rtty.demod, &g_rtty.atc);

        // Отдать FFT ядру 1 (waterfall):
        // multicore_fifo_push_blocking((uint32_t)avg_fft);
        // (Core 1 читает и рисует водопад)
    }

    // 4. ПОПОБЛОЧНАЯ DSP обработка — основной декодер:
    for (int i = 0; i < ADC_BLOCK_SIZE; i++) {
        float s = g_rtty.decimated[i];

        // I/Q квадратурный демодулятор:
        // (вычисляем обе мощности)
        float ci = cosf(g_rtty.demod.mark_phase);
        float cq = sinf(g_rtty.demod.mark_phase);
        float mi = biquad_df2t(s*ci, g_rtty.demod.b0, g_rtty.demod.b1,
                               g_rtty.demod.b2, g_rtty.demod.a1,
                               g_rtty.demod.a2,
                               &g_rtty.demod.mi_z1, &g_rtty.demod.mi_z2);
        float mq = biquad_df2t(s*cq, g_rtty.demod.b0, g_rtty.demod.b1,
                               g_rtty.demod.b2, g_rtty.demod.a1,
                               g_rtty.demod.a2,
                               &g_rtty.demod.mq_z1, &g_rtty.demod.mq_z2);
        g_rtty.demod.mark_phase += g_rtty.demod.mark_phase_step;
        if (g_rtty.demod.mark_phase > 2*M_PI) g_rtty.demod.mark_phase -= 2*M_PI;

        ci = cosf(g_rtty.demod.space_phase);
        cq = sinf(g_rtty.demod.space_phase);
        float si = biquad_df2t(s*ci, g_rtty.demod.b0, g_rtty.demod.b1,
                               g_rtty.demod.b2, g_rtty.demod.a1,
                               g_rtty.demod.a2,
                               &g_rtty.demod.si_z1, &g_rtty.demod.si_z2);
        float sq = biquad_df2t(s*cq, g_rtty.demod.b0, g_rtty.demod.b1,
                               g_rtty.demod.b2, g_rtty.demod.a1,
                               g_rtty.demod.a2,
                               &g_rtty.demod.sq_z1, &g_rtty.demod.sq_z2);
        g_rtty.demod.space_phase += g_rtty.demod.space_phase_step;
        if (g_rtty.demod.space_phase > 2*M_PI) g_rtty.demod.space_phase -= 2*M_PI;

        float mark_power  = mi*mi + mq*mq;
        float space_power = si*si + sq*sq;

        // ATC нормализация:
        float disc = atc_process(&g_rtty.atc, mark_power, space_power);

        // Инверсия (если нужна):
        if (g_rtty.inverted) disc = -disc;

        // DPLL синхронизация:
        float bit_val;
        if (dpll_process(&g_rtty.dpll, disc, &bit_val)) {
            // Получили бит — отдать фреймеру:
            char ch = baudot_framer_push(&g_rtty.framer, bit_val);
            if (ch > 0 && ch != '\0') {
                uint8_t quality = (uint8_t)(fabsf(bit_val) / 1.5f * 100.0f);
                quality = quality > 100 ? 100 : quality;
                ring_push(ch, quality);
                g_state.signal_present = true;
            }
        }

        // Обновить DPLL фазу в состоянии (для осциллографа в UI):
        g_state.dpll_phase = g_rtty.dpll.phase;
    }
}

// Главный цикл Core 0:
void core0_main(void) {
    core0_init(2295.0f, 2125.0f, 45.45f); // дефолтные параметры

    while (true) {
        // Ждём готовности ADC блока (IRQ выставит флаг):
        while (adc_buf_ready < 0) tight_loop_contents();
        int idx = adc_buf_ready;
        adc_buf_ready = -1;

        core0_process_block(idx);
    }
}
```

---

## ЧАСТЬ 11: НАСТРОЙКА С ГЕНЕРАТОРОМ (ПРОЦЕДУРА)

Для калибровки используешь эталонный генератор RTTY на телефоне.

```
РЕКОМЕНДУЕМЫЕ ПРИЛОЖЕНИЯ-ГЕНЕРАТОРЫ:
  Android: "DroidRTTY" или "IZ8BLY Hamscope" → режим TEST TONE
  iOS:     "SignaLink" или audio RTTY generator

ПРОЦЕДУРА:
1. Установить на телефоне: 45.45 Бод, Shift 170 Гц,
   Mark=2295 Гц, Space=2125 Гц
2. Подключить аудиовыход телефона к аудиовходу Pico
3. Установить уровень сигнала потенциометром:
   AGC gain должен быть близок к 1.0 (g_rtty.agc.gain ≈ 0.8..1.5)
   Слишком тихо → gain > 50 → поднять уровень
   Слишком громко → gain < 0.1 → убавить уровень

4. Запустить декодер — должны появляться символы "RYRYRY..." или
   "VVV DE TEST TEST" в текстовой зоне

5. Проверить AFC lock:
   g_state.afc_locked == true через ~3-5 секунд
   g_state.afc_error_hz < 2.0 Гц

6. Проверить SNR:
   g_state.snr_db > 15 дБ при хорошем уровне входа

7. Тест инверсии:
   Если символы нечитаемые — попробовать g_rtty.inverted = true

ОТЛАДОЧНЫЙ ВЫВОД (через USB UART):
  printf("M:%d S:%d SNR:%.1f AFC:%.1f LOCK:%d BAUD:%.2f\n",
         (int)g_state.mark_hz, (int)g_state.space_hz,
         g_state.snr_db, g_state.afc_error_hz,
         g_state.afc_locked, g_state.baud_rate);
```

---

## ЧАСТЬ 12: ПАРАМЕТРЫ НАСТРОЙКИ (config.h)

```c
// === ADC ===
#define ADC_GPIO            26
#define ADC_FS_PHYSICAL     38400
#define ADC_FS_WORK         9600
#define ADC_BLOCK_SIZE      256      // сэмплов после децимации

// === RTTY дефолт ===
#define DEFAULT_MARK_HZ     2295.0f
#define DEFAULT_SPACE_HZ    2125.0f
#define DEFAULT_BAUD        45.45f
#define DEFAULT_STOP_BITS   1.5f
#define DEFAULT_INVERTED    false

// === AGC ===
#define AGC_TARGET          0.30f    // RMS цель (-10 дБFS)
#define AGC_ATTACK_MS       10.0f
#define AGC_RELEASE_MS      500.0f

// === ДЕМОДУЛЯТОР ===
#define DEMOD_LPF_K         0.75f    // полоса LPF = baud × K

// === ATC ===
#define ATC_ATTACK_SYMS     2.0f     // в символах
#define ATC_RELEASE_SYMS    16.0f
#define ATC_CLIP_LEVEL      1.5f

// === DPLL ===
#define DPLL_ALPHA          0.035f   // ширина петли
#define DPLL_MAX_FREQ_ERR   0.05f   // ±5% от baud rate

// === AFC ===
#define AFC_ENABLED         true
#define AFC_MAX_STEP_HZ     2.0f
#define AFC_UPDATE_MS       512
#define AFC_ALPHA           0.10f
#define AFC_MIN_SNR_DB      4.0f

// === FFT / Welch ===
#define FFT_SIZE            512
#define WELCH_FRAMES        8
#define FFT_BIN_HZ          (ADC_FS_WORK / FFT_SIZE)  // 18.75 Гц/бин
```

---

## ЧАСТЬ 13: ЧЕКЛИСТ ЗАПУСКА

```
ЭТАП 1 — Железо:
  [ ] Конденсаторы C1..C8 установлены вплотную (раздел 2.4 ТЗ)
  [ ] Потенциометр 10кОм обеспечивает смещение 1.65V на GPIO26
  [ ] RC фильтр 1кОм + 43нФ на входе АЦП
  [ ] BL/LED через резистор 33-47Ом (или gpio_put статически)

ЭТАП 2 — ADC проверка (без DSP):
  [ ] Осциллограф через UART: вывод raw adc_buf значений
  [ ] При подаче тона 1кГц — должна быть синусоида ~300..3700 уровня ADC
  [ ] Без сигнала — шум ~2048 ± 20 отсчётов (не ± 200!)

ЭТАП 3 — Децимация:
  [ ] Вывод decimated[] через UART — синусоида при 1кГц тоне
  [ ] AGC gain ≈ 1.0 при нормальном уровне входа

ЭТАП 4 — I/Q демодулятор:
  [ ] Подать Mark тон 2295 Гц: mark_power >> space_power
  [ ] Подать Space тон 2125 Гц: space_power >> mark_power
  [ ] Подать оба тона (RTTY): дискриминатор переключается ±

ЭТАП 5 — DPLL:
  [ ] Вывод dpll.phase — должна быть стабильной, не "скачет"
  [ ] Биты приходят ровно с частотой baud_rate (осциллограф по битовому потоку)

ЭТАП 6 — Полный декодер:
  [ ] Генератор: RYRYRY → на экране "RYRYRYRYRY"
  [ ] Генератор: "VVV DE TEST" → декодируется правильно
  [ ] AFC lock через 3-5 сек после начала сигнала
  [ ] SNR > 15 дБ при хорошем входном сигнале
```

---

*ТЗ DSP Decoder v1.0 | RP2350 Core 0 @ 300 МГц*
*Алгоритмы: Kok Chen W7AY (ATC, Nyquist), fldigi (I/Q baseband), DPLL*
