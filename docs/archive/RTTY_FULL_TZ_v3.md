# ПОЛНОЕ ТЕХНИЧЕСКОЕ ЗАДАНИЕ
# Универсальный профессиональный RTTY-декодер
# Raspberry Pi Pico 2 (RP2350) + ILI9341 320×240 Touch
---
> Версия: 3.0 финал | Язык: C/C++ Pico SDK | Разгон: 300 МГц
> Все разделы собраны в одном документе

---

# ТЕХНИЧЕСКОЕ ЗАДАНИЕ
## Универсальный RTTY-декодер на Raspberry Pi Pico 2 (RP2350)
### Версия 1.0 | C/C++ Pico SDK | Дисплей ILI9341 320×240

---

## 1. ОБЗОР ПРОЕКТА

Устройство принимает аудиосигнал с радиоприёмника, декодирует RTTY (Radio Teletype) в реальном времени и отображает принятый текст на цветном TFT-дисплее 320×240 (ILI9341) с сенсорным управлением по SPI. Качество декодирования должно быть на уровне приложений **fldigi** и **MMTTY**.

---

## 2. АППАРАТНАЯ ЧАСТЬ

### 2.1 Микроконтроллер
- **Плата:** Raspberry Pi Pico 2
- **MCU:** RP2350 (Dual-core Cortex-M33, 150 MHz)
- **Flash:** 4 МБ QSPI
- **RAM:** 520 КБ SRAM

### 2.2 Аналоговый входной тракт (уже реализован)

```
Аудио вход (линейный, ~1 Vpp)
        │
    [Регулируемый делитель / сдвиг уровня]
        │  (смещение к середине питания 1.65 В)
        │
    [RC фильтр НЧ: R=1 кОм, C=43 нФ]
        │  Fc = 1/(2π·1000·43e-9) ≈ 3700 Гц
        │  (срез выше полосы RTTY, отсекает ВЧ-помехи)
        │
    GPIO26 (ADC0) — вход АЦП RP2350
```

**Параметры АЦП:**
- Разрядность: 12 бит (0–4095)
- Опорное напряжение: 3.3 В
- Частота дискретизации: **9600 Гц** (выбрана программно через DMA+таймер)
- Постоянное смещение: ~2048 отсчётов (1.65 В)

> **Важно:** RC-фильтр с Fc≈3700 Гц не режет полосу RTTY (300–3000 Гц) — это корректно.

### 2.3 Дисплей ILI9341 (320×240, SPI)

| Сигнал ILI9341 | GPIO Pico 2 | Описание              |
|----------------|-------------|-----------------------|
| SCK            | GPIO18      | SPI1 Clock            |
| MOSI           | GPIO19      | SPI1 TX               |
| MISO           | GPIO16      | SPI1 RX (touch)       |
| CS (LCD)       | GPIO17      | Chip Select дисплея   |
| DC             | GPIO20      | Data/Command          |
| RST            | GPIO21      | Reset                 |
| BL             | GPIO22      | Подсветка (PWM)       |
| T_CS           | GPIO15      | Chip Select touch     |
| T_IRQ          | GPIO14      | Touch interrupt       |

**SPI параметры:**
- Дисплей: SPI1, скорость **40 МГц** (запись)
- Touch (XPT2046): SPI1, скорость **2 МГц** (отдельный CS)

### 2.4 Кнопки / управление
- GPIO11 — кнопка MENU (подтяжка к VCC, активный LOW)
- GPIO12 — кнопка UP / увеличить скорость
- GPIO13 — кнопка DOWN / уменьшить скорость
- (Основное управление — сенсорный экран)

---

## 3. ПРОГРАММНАЯ АРХИТЕКТУРА

### 3.1 Структура проекта

```
rtty_decoder/
├── CMakeLists.txt
├── pico_sdk_import.cmake
├── src/
│   ├── main.c                  — инициализация, главный цикл
│   ├── adc_capture.c/h         — захват АЦП через DMA
│   ├── dsp/
│   │   ├── fir_filter.c/h      — FIR-фильтры (mark/space)
│   │   ├── goertzel.c/h        — детектор тонов Гёрцеля
│   │   ├── agc.c/h             — автоматическая регулировка усиления
│   │   └── rtty_demod.c/h      — демодулятор FSK, битовый декодер
│   ├── protocol/
│   │   ├── baudot.c/h          — таблица Baudot/ITA2, декодер символов
│   │   └── rtty_decoder.c/h    — фреймер, определение скорости
│   ├── display/
│   │   ├── ili9341.c/h         — драйвер дисплея ILI9341
│   │   ├── touch_xpt2046.c/h   — драйвер сенсора XPT2046
│   │   ├── font.c/h            — растровый шрифт 8×12, 6×8
│   │   └── ui.c/h              — интерфейс: waterfall, текст, меню
│   └── config.h                — все настраиваемые параметры
└── assets/
    └── font_8x12.h             — встроенный шрифт
```

### 3.2 Распределение по ядрам RP2350

```
┌─────────────────────────────┐  ┌──────────────────────────────┐
│         ЯДРО 0              │  │          ЯДРО 1              │
│                             │  │                              │
│  • Захват АЦП (DMA)         │  │  • Отрисовка дисплея         │
│  • DSP обработка сигнала    │  │  • Waterfall (спектр)        │
│  • Демодуляция FSK          │  │  • Вывод текста              │
│  • Декодирование Baudot     │  │  • Обработка touch           │
│  • Запись в кольц. буфер    │  │  • Меню настроек             │
│                             │  │  • Обновление UI             │
└─────────────────────────────┘  └──────────────────────────────┘
         ↕ mutex + ring buffer (multicore_fifo или FIFO)
```

---

## 4. DSP — АЛГОРИТМ ДЕМОДУЛЯЦИИ RTTY

### 4.1 Параметры сигнала RTTY

| Параметр        | Значение                              |
|-----------------|---------------------------------------|
| Модуляция       | FSK (Frequency Shift Keying)          |
| Mark (1)        | Выше по частоте                       |
| Space (0)       | Ниже по частоте                       |
| Shift (узкий)   | 170 Гц (любительский стандарт)        |
| Shift (широкий) | 850 Гц (коммерческий/военный)         |
| Скорости        | 45.45 / 50 / 75 / 100 / 110 / 150 Бод|
| Кодировка       | Baudot / ITA2 (5-битный код)          |
| Стоп-биты       | 1 или 1.5                             |

### 4.2 Конфигурации Mark/Space для стандартных режимов

```c
// config.h
typedef struct {
    float mark_freq;    // Гц
    float space_freq;   // Гц
    float baud_rate;    // Бод
    uint8_t stop_bits;  // 10 или 15 (×0.1)
} rtty_config_t;

// Преднастроенные режимы
static const rtty_config_t RTTY_PRESETS[] = {
    // Любительские (shift 170 Гц):
    { 2295.0f, 2125.0f, 45.45f, 15 },  // 45N15 — самый распространённый
    { 2295.0f, 2125.0f, 50.0f,  15 },  // 50N15
    { 2295.0f, 2125.0f, 75.0f,  15 },  // 75N15
    // Широкий shift 850 Гц:
    { 2125.0f, 1275.0f, 45.45f, 15 },  // широкий 45.45
    { 2125.0f, 1275.0f, 50.0f,  15 },  // широкий 50
    // Пользовательский (настраивается в меню):
    { 0, 0, 0, 0 }
};
```

### 4.3 Блок-схема DSP (Ядро 0)

```
АЦП 9600 Гц → [DC блокировка] → [AGC] → [Синхронный детектор]
                                              │
                          ┌───────────────────┴───────────────────┐
                          ↓                                         ↓
                  [BPF Mark FIR]                          [BPF Space FIR]
                  (узкополосный                          (узкополосный
                   фильтр, 80 Гц)                         фильтр, 80 Гц)
                          │                                         │
                   [Детектор огибающей]                  [Детектор огибающей]
                   (квадратурный)                        (квадратурный)
                          │                                         │
                          └──────────────┬────────────────────────-┘
                                         ↓
                              [Компаратор Mark/Space]
                              (mark_env - space_env → bit)
                                         │
                                  [Сглаживающий ФНЧ]
                                  (устранение дрожания)
                                         │
                              [Порог → 0/1 бит-поток]
                                         │
                           [Детектор синхронизации битов]
                           (PLL / zero-crossing timing)
                                         │
                           [Декодер Baudot: 5 бит → символ]
                                         │
                           [→ Кольцевой буфер символов → Ядро 1]
```

### 4.4 FIR-фильтры полосы Mark и Space

Для каждой частоты строим **полосовой FIR** методом оконного проектирования (окно Хэмминга):

```c
// Параметры фильтра
#define FIR_TAPS     64         // число коэффициентов
#define SAMPLE_RATE  9600.0f    // Гц

// Строим FIR BPF вокруг mark_freq или space_freq
// Полоса: ±(baud_rate * 0.7) Гц от центральной частоты
// Пример для mark=2295 Гц, baud=45.45:
//   нижняя = 2295 - 32 = 2263 Гц
//   верхняя = 2295 + 32 = 2327 Гц

void fir_design_bpf(float *coeffs, int taps,
                    float center_hz, float bw_hz,
                    float sample_rate);

// Применение (оптимизировано для Cortex-M33 с DSP инструкциями):
float fir_apply(const float *coeffs, float *delay_line,
                float input, int taps);
```

> **Оптимизация RP2350:** Использовать инструкции SIMD (Cortex-M33 DSP extensions) через CMSIS DSP Library — функция `arm_fir_f32()` из pico-extras.

### 4.5 Детектор огибающей (envelope)

```c
// Квадратурный (I/Q) детектор огибающей:
// 1. Умножаем сигнал на cos(2π·f·t) и sin(2π·f·t)
// 2. Прогоняем оба через ФНЧ
// 3. Огибающая = sqrt(I² + Q²) ≈ abs(I) + abs(Q) (быстрое приближение)

float envelope_detect_iq(float input, float freq,
                         float sample_rate, float *i_state,
                         float *q_state, float lpf_alpha);
```

### 4.6 НЛОС синхронизация битов (PLL)

Ключевой элемент для качественного декодирования — **фазовая автоподстройка тактирования**:

```c
typedef struct {
    float phase;          // текущая фаза 0..1
    float freq;           // ожидаемая частота = baud_rate / sample_rate
    float alpha;          // коэф. коррекции (0.01..0.05)
    float last_bit;       // предыдущий бит для детекции перехода
} bit_pll_t;

// Логика: при обнаружении перехода 0→1 или 1→0
// корректируем фазу чтобы выборка попадала в середину битового периода
void bit_pll_update(bit_pll_t *pll, float bit_signal,
                    int *bit_out, int *bit_clock);
```

### 4.7 Автоматическое определение скорости

```c
// Измеряем среднюю длину импульсов (run-length encoding)
// Минимальный импульс ≈ 1 битовый период
// По гистограмме длин → вычисляем baud_rate

float auto_detect_baud(uint32_t *pulse_lengths,
                       int count, float sample_rate);
// Возвращает ближайшее из: 45.45, 50, 75, 100, 110, 150 Бод
```

---

## 5. ДЕКОДЕР BAUDOT / ITA2

### 5.1 Таблица символов

```c
// ITA2 — 5-битный код, два регистра: буквы (LTRS) и цифры/знаки (FIGS)
static const char baudot_ltrs[32] = {
    '\0','E','\n','A',' ','S','I','U',
    '\r','D','R','J','N','F','C','K',
    'T','Z','L','W','H','Y','P','Q',
    'O','B','G','\0','M','X','V','\0'
};

static const char baudot_figs[32] = {
    '\0','3','\n','-',' ','\'','8','7',
    '\r','$','4','\a',',','!',':','(',
    '5','"',')','2','#','6','0','1',
    '9','?','&','\0','.','/',';','\0'
};

#define BAUDOT_LTRS  0x1F  // переключение в режим букв
#define BAUDOT_FIGS  0x1B  // переключение в режим цифр
#define BAUDOT_NULL  0x00
#define BAUDOT_DEL   0x1F
```

### 5.2 Фреймер

```c
// Формат RTTY-кадра:
// [1 стартовый бит = 0 (Space)] [5 бит данных, LSB первым] [1.5 стоп-бита = 1 (Mark)]

typedef enum { WAIT_START, RECV_DATA, RECV_STOP } frame_state_t;

char rtty_frame_decode(bit_pll_t *pll, float bit_signal,
                       frame_state_t *state, uint8_t *shift_reg,
                       int *bit_count, int stop_bits_x10);
```

---

## 6. ИНТЕРФЕЙС ПОЛЬЗОВАТЕЛЯ (Дисплей ILI9341 320×240)

### 6.1 Раскладка экрана (главный экран)

```
┌────────────────────────────────────────────┐
│ RTTY 45.45  170Hz  [MARK][SPACE]  SNR:12dB │  ← Строка статуса (16px)
├────────────────────────────────────────────┤
│                                            │
│  [  W A T E R F A L L  320×80 px  ]        │  ← Водопад FFT (80px)
│  Частотная шкала: 300..3000 Гц             │
│  Маркеры Mark/Space — жёлтые линии         │
│                                            │
├────────────────────────────────────────────┤
│                                            │
│ DE UA9XYZ QTH NOVOSIBIRSK RST 599 599 BK  │  ← Текстовое поле (128px)
│ CQ CQ CQ DE UA9XYZ K                      │  (16 строк × 40 символов)
│ _                                          │
│                                            │
├────────────────────────────────────────────┤
│ [CLEAR] [SAVE] [MODE▼] [SPEED▼] [MENU ☰] │  ← Панель кнопок (16px)
└────────────────────────────────────────────┘
```

### 6.2 Waterfall (водопад)

- **FFT:** 256 точек, оконная функция Хэмминга, fs=9600 Гц → разрешение 37.5 Гц/бин
- **Скорость скролла:** 10 строк/сек (каждые 100 мс новая строка сверху)
- **Цветовая карта:** чёрный (тишина) → синий → зелёный → жёлтый → красный (сигнал)
- **Маркеры:** две вертикальные линии (Mark и Space) — перетаскиваются пальцем
- **Автонастройка:** двойное нажатие на сигнал → автоматически позиционирует Mark/Space

```c
// Цветовая карта (16 уровней, RGB565):
static const uint16_t WATERFALL_COLORS[16] = {
    0x0000, 0x000F, 0x001F, 0x003F,  // чёрный → тёмно-синий
    0x007F, 0x03FF, 0x07EF, 0x07E0,  // синий → голубой → зелёный
    0x3FE0, 0x7FE0, 0xFFE0, 0xFFC0,  // зелёный → жёлто-зелёный → жёлтый
    0xFF80, 0xFF00, 0xF800, 0xF800   // жёлтый → оранжевый → красный
};
```

### 6.3 Текстовое поле

- Шрифт: **8×12 пикселей** (моноширинный, 40 символов в строке, 10 строк)
- Цвет текста: зелёный `#00FF00` на чёрном — классический телетайп
- Автопрокрутка снизу вверх при заполнении
- Новые символы появляются с мигающим курсором
- Символы LTRS/FIGS не отображаются (служебные)
- Отображение неверного фрейма: символ `·` красным цветом

### 6.4 Меню настроек (touch)

```
┌─ НАСТРОЙКИ ──────────────────┐
│ Скорость:    [45.45 Бод  ▼]  │
│ Shift:       [170 Гц     ▼]  │
│ Mark:        [2295 Гц  ±10]  │
│ Space:       [2125 Гц  ±10]  │
│ Стоп-биты:   [1.5  / 1.0  ] │
│ Автоскорость:[ВКЛ / ВЫКЛ   ] │
│ Автонастр.:  [ВКЛ / ВЫКЛ   ] │
│ AFC (авт.ч): [ВКЛ / ВЫКЛ   ] │
│ Яркость:     [████░░  60%  ] │
│                              │
│        [ПРИМЕНИТЬ]           │
└──────────────────────────────┘
```

### 6.5 Индикатор уровня сигнала

Две горизонтальные полоски (Mark / Space) в строке статуса:
- Зелёная = высокий уровень, серая = низкий
- Визуально показывает что декодируется

---

## 7. AFC — АВТОМАТИЧЕСКАЯ ПОДСТРОЙКА ЧАСТОТЫ

```c
// AFC ищет пик спектра вблизи ожидаемых Mark/Space
// Алгоритм: скользящее среднее позиции пика в waterfall

typedef struct {
    float center_freq;    // текущая центральная частота
    float afc_rate;       // скорость коррекции (Гц/сек)
    bool  enabled;
} afc_t;

void afc_update(afc_t *afc, float *fft_bins, int bins,
                float bin_hz, float mark_hz, float space_hz);
// Обновляет mark_hz и space_hz в конфиге декодера
```

---

## 8. ЗАХВАТ АЦП ЧЕРЕЗ DMA

```c
// Используем DMA с двойным буфером (ping-pong)
// Каждый буфер = 256 сэмплов = 26.7 мс при 9600 Гц

#define ADC_BUFFER_SIZE  256
#define ADC_SAMPLE_RATE  9600

uint16_t adc_buf[2][ADC_BUFFER_SIZE];
volatile int adc_buf_ready;  // 0 или 1 — какой буфер готов

void adc_dma_init(void) {
    // adc_init(), adc_gpio_init(26), adc_select_input(0)
    // adc_set_clkdiv(48000000 / ADC_SAMPLE_RATE - 1)
    // dma_channel_configure() × 2 (chain)
    // irq_set_exclusive_handler(DMA_IRQ_0, adc_dma_handler)
}

void adc_dma_handler(void) {
    // Переключаем буферы, уведомляем Ядро 0 через multicore_fifo_push
}
```

---

## 9. ОПТИМИЗАЦИЯ ПРОИЗВОДИТЕЛЬНОСТИ

### 9.1 Критические пути (Ядро 0, 9600 Гц)

| Операция             | Время (est.) | Метод оптимизации            |
|----------------------|--------------|------------------------------|
| FIR 64 tap (Mark)    | ~14 мкс      | arm_fir_f32 (CMSIS DSP)      |
| FIR 64 tap (Space)   | ~14 мкс      | arm_fir_f32 (CMSIS DSP)      |
| I/Q детектор ×2      | ~4 мкс       | inline, lookup sin/cos table |
| FFT 256pt (waterfall)| ~800 мкс     | kiss_fft или pico-fft        |
| Всего на 256 сэмплов | ~1.5 мс      | бюджет: 26.7 мс ✓            |

### 9.2 Таблицы sin/cos

```c
// Предвычисленные таблицы для всех рабочих частот
// При fs=9600 и f=2295 Гц период = 9600/2295 ≈ 4.18 — используем 
// таблицу на 9600 точек (один период 1 Гц) с интерполяцией

#define SIN_TABLE_SIZE 9600
static float sin_table[SIN_TABLE_SIZE];  // ~37 КБ — помещается в RAM

void sin_table_init(void);
static inline float fast_sin(float phase_0_1) {
    int idx = (int)(phase_0_1 * SIN_TABLE_SIZE) % SIN_TABLE_SIZE;
    return sin_table[idx];
}
```

### 9.3 SPI DMA для дисплея

```c
// Отправка пикселей waterfall через DMA, не блокируя CPU Ядра 1
void ili9341_send_dma(uint16_t *pixels, int count);
// Ядро 1 подготавливает следующую строку пока DMA отправляет текущую
```

---

## 10. КОНФИГУРАЦИОННЫЕ ПАРАМЕТРЫ (config.h)

```c
// === АЦП ===
#define ADC_GPIO          26
#define ADC_SAMPLE_RATE   9600
#define ADC_BUFFER_SIZE   256

// === DSP ===
#define FIR_TAPS          64
#define FFT_SIZE          256
#define AGC_ATTACK        0.01f
#define AGC_RELEASE       0.001f
#define BIT_PLL_ALPHA     0.02f
#define ENVELOPE_LPF      0.15f    // alpha ФНЧ огибающей

// === RTTY дефолт ===
#define DEFAULT_BAUD      45.45f
#define DEFAULT_MARK_HZ   2295.0f
#define DEFAULT_SPACE_HZ  2125.0f
#define DEFAULT_SHIFT_HZ  170.0f
#define DEFAULT_STOP_BITS 15       // 1.5 × 10

// === AFC ===
#define AFC_ENABLED       true
#define AFC_RANGE_HZ      100.0f
#define AFC_RATE          5.0f

// === ДИСПЛЕЙ ===
#define LCD_WIDTH         320
#define LCD_HEIGHT        240
#define WATERFALL_HEIGHT  80
#define TEXT_AREA_HEIGHT  128
#define STATUS_HEIGHT     16
#define BUTTON_HEIGHT     16

// === SPI ===
#define SPI_PORT          spi1
#define SPI_FREQ_LCD      40000000
#define SPI_FREQ_TOUCH    2000000
#define PIN_SCK           18
#define PIN_MOSI          19
#define PIN_MISO          16
#define PIN_LCD_CS        17
#define PIN_DC            20
#define PIN_RST           21
#define PIN_BL            22
#define PIN_TOUCH_CS      15
#define PIN_TOUCH_IRQ     14

// === ЦВЕТА (RGB565) ===
#define COLOR_BG          0x0000   // чёрный
#define COLOR_TEXT        0x07E0   // зелёный
#define COLOR_TEXT_ERR    0xF800   // красный
#define COLOR_MARK_LINE   0xFFE0   // жёлтый
#define COLOR_STATUS_BG   0x2104   // тёмно-серый
#define COLOR_BTN_BG      0x4208   // серый
#define COLOR_BTN_ACTIVE  0x001F   // синий
```

---

## 11. ЗАВИСИМОСТИ И СБОРКА

### 11.1 CMakeLists.txt (ключевые части)

```cmake
cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)

project(rtty_decoder C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(rtty_decoder
    src/main.c
    src/adc_capture.c
    src/dsp/fir_filter.c
    src/dsp/goertzel.c
    src/dsp/agc.c
    src/dsp/rtty_demod.c
    src/protocol/baudot.c
    src/protocol/rtty_decoder.c
    src/display/ili9341.c
    src/display/touch_xpt2046.c
    src/display/font.c
    src/display/ui.c
)

# Pico SDK библиотеки
target_link_libraries(rtty_decoder
    pico_stdlib
    pico_multicore
    hardware_adc
    hardware_dma
    hardware_spi
    hardware_pwm
    hardware_irq
    pico_float          # аппаратная FPU
)

# Оптимизация для DSP
target_compile_options(rtty_decoder PRIVATE
    -O2
    -mfpu=fpv5-sp-d16   # FPU Cortex-M33
    -mfloat-abi=hard
)

pico_add_extra_outputs(rtty_decoder)
```

### 11.2 Внешние библиотеки

| Библиотека  | Назначение         | Источник                        |
|-------------|--------------------|---------------------------------|
| kiss_fft    | FFT для waterfall  | включить в репозиторий (MIT)    |
| CMSIS DSP   | arm_fir_f32        | pico-extras или отдельно        |

---

## 12. ТЕСТИРОВАНИЕ И ОТЛАДКА

### 12.1 Тестовый генератор (без радио)

```c
// В config.h установить TEST_SIGNAL true
// Генерирует синтетический RTTY сигнал программно:
// "CQ CQ CQ DE TEST TEST K" на 45.45 бод, shift 170 Гц

#ifdef TEST_SIGNAL
float test_rtty_generate(float *phase_mark, float *phase_space,
                         int *bit_index, float t);
#endif
```

### 12.2 UART отладка

```c
// Через USB UART Pico (GPIO0/1 или USB CDC):
// Вывод: уровни Mark/Space, битовый поток, декодированные символы
// Формат: "M:0.82 S:0.21 BIT:1 CHAR:E"
```

### 12.3 Контрольные точки качества декодирования

- ✅ BER < 1% при SNR > 10 дБ (оценка через тестовые записи)
- ✅ Захват синхронизации < 2 секунды после начала сигнала
- ✅ AFC удерживает настройку при дрейфе ±50 Гц
- ✅ Автоопределение скорости за < 5 секунд
- ✅ Waterfall обновляется ≥ 10 FPS

---

## 13. ПОРЯДОК РЕАЛИЗАЦИИ (рекомендуемый)

**Этап 1 — Базовый захват и дисплей (1-2 дня)**
1. Инициализация ILI9341, вывод текста, заливка
2. Захват АЦП через DMA, буферизация
3. Вывод уровня сигнала на экран (простой осциллограф)

**Этап 2 — DSP и waterfall (2-3 дня)**
1. FFT 256pt → цветная waterfall
2. FIR фильтры Mark/Space
3. Детектор огибающей

**Этап 3 — Декодирование (2-3 дня)**
1. Компаратор Mark/Space → битовый поток
2. PLL синхронизации битов
3. Фреймер RTTY + декодер Baudot
4. Вывод текста на экран

**Этап 4 — Полировка (1-2 дня)**
1. AFC
2. Автоопределение скорости
3. Меню настроек через touch
4. Кнопки управления

---

## 14. РЕСУРСЫ И ССЫЛКИ

- [Pico SDK docs](https://raspberrypi.github.io/pico-sdk-doxygen/)
- [fldigi исходный код (GPL)](https://sourceforge.net/p/fldigi/fldigi/ci/master/tree/) — эталон алгоритмов
- [ITA2 Baudot таблица](https://en.wikipedia.org/wiki/Baudot_code#ITA2)
- [RTTY на KB3CV](http://www.kb3cv.com/rtty/) — практические примеры
- [kiss_fft](https://github.com/mborgerding/kissfft) — лёгкая FFT библиотека
- [ILI9341 datasheet](https://cdn-shop.adafruit.com/datasheets/ILI9341.pdf)

---

*ТЗ составлено для RP2350, Pico SDK, C/C++. Версия 1.0*
# РАЗДЕЛ 6 (РАСШИРЕННЫЙ): WATERFALL, TOUCH UI И СИСТЕМА ПОДСТРОЙКИ
## RTTY Decoder RP2350 — Детальная спецификация интерфейса
### Версия 2.0

---

## 6.1 РАСКЛАДКА ЭКРАНА — ГОРИЗОНТАЛЬНАЯ ОРИЕНТАЦИЯ 320×240

```
X=0                                                        X=319
Y=0  ┌──────────────────────────────────────────────────────┐
     │  СТАТУС: 45.45Bd  170Hz  M:2295  S:2125  SNR:14dB   │ 14px
Y=14 ├──────────────────────────────────────────────────────┤
     │                                                      │
     │         W A T E R F A L L    (FFT 3000 Гц)          │
     │                                                      │ 96px
     │   |     |M|          |S|                    |        │
     │  300   1000         2000                   3000 Гц   │
Y=110├──────────────────────────────────────────────────────┤
     │ Частотная шкала: |300|500|1000|1500|2000|2500|3000|  │ 10px
Y=120├──────────────────────────────────────────────────────┤
     │                                                      │
     │ DE UA9XYZ QTH NOVOSIBIRSK OP SERGEY RST 599 599 K   │
     │ CQ CQ DE UA9XYZ TEST 73 K                           │ 96px
     │ █ (курсор)                                          │
     │                                                      │
Y=216├──────────────────────────────────────────────────────┤
     │ [TUNE] [AFC●] [CLR] [45.45▼] [170Hz▼] [LOCK] [☰]  │ 24px
Y=240└──────────────────────────────────────────────────────┘
```

**Зоны экрана:**

| Зона          | Y от | Y до | Высота | Назначение                    |
|---------------|------|------|--------|-------------------------------|
| Статус-бар    | 0    | 14   | 14 px  | Параметры, SNR, индикаторы    |
| Waterfall     | 14   | 110  | 96 px  | FFT водопад + маркеры         |
| Частотная шкала| 110 | 120  | 10 px  | Подписи Hz (статичная)        |
| Текстовая зона| 120  | 216  | 96 px  | Декодированный текст          |
| Тулбар        | 216  | 240  | 24 px  | Кнопки управления сенсором    |

---

## 6.2 WATERFALL — ДЕТАЛЬНАЯ РЕАЛИЗАЦИЯ

### 6.2.1 Параметры FFT

```c
#define WATERFALL_FFT_SIZE    512      // точек FFT
#define WATERFALL_SAMPLE_RATE 9600     // Гц
// Разрешение: 9600/512 = 18.75 Гц/бин — достаточно для shift 170 Гц
// Диапазон отображения: 0..3000 Гц = бины 0..160
// Ширина экрана waterfall: 320 пикселей
// Масштаб: 320px / 3000Гц = 0.1067 px/Гц, или 9.375 Гц/px

#define WF_FREQ_MIN   300.0f   // Гц — левый край
#define WF_FREQ_MAX   3300.0f  // Гц — правый край (3000 Гц полоса)
#define WF_PX_WIDTH   320      // пикселей по горизонтали
#define WF_PX_HEIGHT  96       // пикселей по вертикали (строки истории)
#define WF_Y_TOP      14       // Y-координата верхней строки

// Перевод частоты → пиксель X:
// px = (freq - WF_FREQ_MIN) * WF_PX_WIDTH / (WF_FREQ_MAX - WF_FREQ_MIN)
#define FREQ_TO_PX(f)  ((int)(((f) - WF_FREQ_MIN) * WF_PX_WIDTH / (WF_FREQ_MAX - WF_FREQ_MIN)))

// Перевод пикселя X → частота:
// freq = WF_FREQ_MIN + x * (WF_FREQ_MAX - WF_FREQ_MIN) / WF_PX_WIDTH
#define PX_TO_FREQ(x)  (WF_FREQ_MIN + (x) * (WF_FREQ_MAX - WF_FREQ_MIN) / WF_PX_WIDTH)
```

### 6.2.2 Скролл-буфер водопада

```c
// Кольцевой буфер из WF_PX_HEIGHT строк по WF_PX_WIDTH пикселей (RGB565)
// Каждая строка = одна новая FFT (100 мс при обновлении 10 Гц)
// Хранится в RAM: 96 × 320 × 2 байта = 61.4 КБ — помещается в RP2350

static uint16_t wf_buffer[WF_PX_HEIGHT][WF_PX_WIDTH];
static int wf_write_row = 0;  // куда пишем следующую строку

void waterfall_push_row(float *fft_mag, int fft_bins) {
    // 1. Рассчитать новую строку пикселей из fft_mag[]
    for (int px = 0; px < WF_PX_WIDTH; px++) {
        float freq = PX_TO_FREQ(px);
        int bin = (int)(freq * WATERFALL_FFT_SIZE / WATERFALL_SAMPLE_RATE);
        // Интерполяция между соседними бинами для сглаживания:
        float frac = (freq * WATERFALL_FFT_SIZE / WATERFALL_SAMPLE_RATE) - bin;
        float mag = fft_mag[bin] * (1.0f - frac) + fft_mag[bin+1] * frac;
        // Перевод в цвет:
        wf_buffer[wf_write_row][px] = mag_to_color(mag);
    }
    wf_write_row = (wf_write_row + 1) % WF_PX_HEIGHT;
}
```

### 6.2.3 Цветовая карта (тепловая)

```c
// 32-ступенчатая цветовая карта в стиле "phosphor/thermal":
// чёрный → тёмно-синий → синий → голубой → зелёный → жёлтый → красный → белый

static const uint16_t WF_COLORMAP[32] = {
    // RGB565: R=5 бит [15:11], G=6 бит [10:5], B=5 бит [4:0]
    0x0000, // 0  — чёрный (тишина)
    0x0001, // 1  — почти чёрный
    0x0003, // 2  — очень тёмно-синий
    0x0007, // 3  — тёмно-синий
    0x000F, // 4
    0x001F, // 5  — синий
    0x001B, // 6
    0x0218, // 7  — синий с фиолетом
    0x0438, // 8
    0x0639, // 9
    0x043F, // 10 — голубой
    0x07FF, // 11 — ярко-голубой
    0x07EF, // 12
    0x07E0, // 13 — чистый зелёный
    0x3FE0, // 14
    0x7FE0, // 15 — жёлто-зелёный
    0xAFE0, // 16
    0xFFE0, // 17 — жёлтый
    0xFFC0, // 18
    0xFF80, // 19 — оранжево-жёлтый
    0xFF40, // 20
    0xFF20, // 21 — оранжевый
    0xFF00, // 22
    0xFE00, // 23 — красно-оранжевый
    0xFC00, // 24 — красный
    0xF800, // 25 — ярко-красный
    0xF80C, // 26
    0xF81F, // 27 — малиновый
    0xFC1F, // 28
    0xFE1F, // 29
    0xFF9F, // 30 — розово-белый
    0xFFFF, // 31 — белый (максимум)
};

uint16_t mag_to_color(float mag_db) {
    // mag_db: ожидается диапазон от -80 до 0 дБ
    // Нормализуем к 0..31
    float normalized = (mag_db + 80.0f) / 80.0f;  // 0..1
    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;
    int idx = (int)(normalized * 31.0f);
    return WF_COLORMAP[idx];
}
```

### 6.2.4 Отрисовка waterfall на дисплей

```c
void waterfall_draw(void) {
    // Рисуем строки от новой к старой (новая — снизу, старая — сверху)
    // Это классический "водопад" — сигнал течёт сверху вниз

    ili9341_set_window(0, WF_Y_TOP, WF_PX_WIDTH - 1,
                       WF_Y_TOP + WF_PX_HEIGHT - 1);
    ili9341_start_write();

    for (int row = 0; row < WF_PX_HEIGHT; row++) {
        // Строка row=0 — самая верхняя (самая старая)
        // Строка row=WF_PX_HEIGHT-1 — самая нижняя (самая новая)
        int buf_row = (wf_write_row - WF_PX_HEIGHT + row + WF_PX_HEIGHT)
                      % WF_PX_HEIGHT;

        // Отправляем строку через DMA
        ili9341_send_row_dma(wf_buffer[buf_row], WF_PX_WIDTH);
    }
    ili9341_end_write();

    // Поверх водопада рисуем маркеры Mark/Space
    waterfall_draw_markers();
}
```

### 6.2.5 Частота обновления

```c
// Ядро 1 обновляет водопад в цикле:
// - Новая FFT строка из Ядра 0 приходит каждые 26.7 мс (37.5 строк/сек)
// - Мы обновляем экран со скоростью ~15 строк/сек (67 мс/строку)
// - Это создаёт плавный скролл без мерцания

// Можно настроить: WF_UPDATE_MS 66 (15fps) или 33 (30fps)
#define WF_UPDATE_MS  66
```

---

## 6.3 МАРКЕРЫ MARK/SPACE — TOUCH УПРАВЛЕНИЕ

### 6.3.1 Структура маркеров

```c
typedef struct {
    float  mark_hz;       // текущая частота Mark (Гц)
    float  space_hz;      // текущая частота Space (Гц)
    float  shift_hz;      // разность = mark_hz - space_hz
    bool   lock_shift;    // если true — при движении одного двигается и другой
    int    drag_which;    // 0=никто не тащится, 1=Mark, 2=Space, 3=оба(центр)
    int    drag_start_x;  // X координата начала касания
    float  drag_start_mark_hz;
    float  drag_start_space_hz;
} marker_state_t;

// Отображение маркеров:
// Mark  — вертикальная линия ЖЁЛТОГО цвета    с меткой "M" сверху
// Space — вертикальная линия ГОЛУБОГО цвета   с меткой "S" сверху
// Между ними — полупрозрачная заливка зелёным (показывает shift-полосу)
```

### 6.3.2 Визуальный вид маркеров

```
  Y=14  ┌────────┬──────┬──────────────────────────────┐
        │        │M     │S                             │
        │        │█████ │                              │  ← метки сверху
        │        │      │                              │
        │      ~~│~~~~~~│~~  ← полупрозрачная полоса   │
        │        │      │    между Mark и Space        │
        │     ~~~│~~~~~~│~~~                           │
  Y=110 └────────┴──────┴──────────────────────────────┘
                 ↑жёлтая↑голубая
```

**Реализация полупрозрачной полосы (псевдо-alpha):**
```c
// Для каждого пикселя в полосе между Mark и Space:
// pixel = blend(pixel, GREEN, 0.25)
// В RGB565 быстрое смешивание:
uint16_t blend_25_green(uint16_t pixel) {
    // Добавляем 25% зелёного к существующему пикселю
    uint16_t r = (pixel >> 11) & 0x1F;
    uint16_t g = (pixel >> 5)  & 0x3F;
    uint16_t b = pixel & 0x1F;
    g = (g * 3 + 63) / 4;  // +25% к зелёному каналу
    return (r << 11) | (g << 5) | b;
}
```

### 6.3.3 Обработка Touch для маркеров

```c
// Зона касания маркера: ±12 пикселей от линии маркера
#define MARKER_TOUCH_RADIUS  12

void waterfall_touch_handler(int touch_x, int touch_y,
                             touch_event_t event,
                             marker_state_t *markers) {

    if (touch_y < WF_Y_TOP || touch_y > WF_Y_TOP + WF_PX_HEIGHT)
        return;  // касание не в зоне водопада

    int mark_px  = FREQ_TO_PX(markers->mark_hz);
    int space_px = FREQ_TO_PX(markers->space_hz);

    switch (event) {
        case TOUCH_DOWN:
            // Определяем — тащим Mark, Space или ни один
            if (abs(touch_x - mark_px) <= MARKER_TOUCH_RADIUS) {
                markers->drag_which = 1;  // тащим Mark
            } else if (abs(touch_x - space_px) <= MARKER_TOUCH_RADIUS) {
                markers->drag_which = 2;  // тащим Space
            } else {
                // Касание между маркерами — тащим весь блок вместе
                int center_px = (mark_px + space_px) / 2;
                if (abs(touch_x - center_px) <= (abs(mark_px - space_px)/2 + 10)) {
                    markers->drag_which = 3;  // тащим оба
                } else {
                    markers->drag_which = 0;
                }
            }
            markers->drag_start_x = touch_x;
            markers->drag_start_mark_hz  = markers->mark_hz;
            markers->drag_start_space_hz = markers->space_hz;
            break;

        case TOUCH_MOVE:
            if (markers->drag_which == 0) break;
            float delta_hz = PX_TO_FREQ(touch_x) - PX_TO_FREQ(markers->drag_start_x);

            if (markers->drag_which == 1) {
                // Тащим только Mark
                markers->mark_hz = markers->drag_start_mark_hz + delta_hz;
                if (markers->lock_shift) {
                    // Shift зафиксирован — тащим и Space
                    markers->space_hz = markers->mark_hz - markers->shift_hz;
                }
            } else if (markers->drag_which == 2) {
                // Тащим только Space
                markers->space_hz = markers->drag_start_space_hz + delta_hz;
                if (markers->lock_shift) {
                    markers->mark_hz = markers->space_hz + markers->shift_hz;
                }
            } else if (markers->drag_which == 3) {
                // Тащим весь блок
                markers->mark_hz  = markers->drag_start_mark_hz  + delta_hz;
                markers->space_hz = markers->drag_start_space_hz + delta_hz;
            }

            // Обновляем shift_hz
            markers->shift_hz = markers->mark_hz - markers->space_hz;

            // Ограничения:
            clamp_float(&markers->mark_hz,  WF_FREQ_MIN + 50, WF_FREQ_MAX - 50);
            clamp_float(&markers->space_hz, WF_FREQ_MIN + 50, WF_FREQ_MAX - 50);

            // Немедленно применяем к декодеру:
            rtty_set_frequencies(markers->mark_hz, markers->space_hz);
            break;

        case TOUCH_UP:
            // После отпускания — если перемещение было небольшим,
            // запускаем TUNE (автоподстройку)
            if (abs(PX_TO_FREQ(touch_x) - PX_TO_FREQ(markers->drag_start_x)) < 30.0f) {
                // Скорее тап, чем drag — запускаем TUNE
                tune_snap_to_peaks(markers);
            }
            markers->drag_which = 0;
            break;
    }
}
```

---

## 6.4 АЛГОРИТМ TUNE — ТОЧНАЯ ПОДСТРОЙКА НА ПИКИ

Это ключевой алгоритм. Пользователь грубо ставит маркеры пальцем, затем нажимает **TUNE** (или делает тап на водопаде) — прибор сам находит точные пики спектра.

### 6.4.1 Принцип работы TUNE

```
Текущие Mark_hz и Space_hz (заданы грубо пользователем)
              │
              ↓
[Взять усреднённый спектр за последние 500 мс]
(накапливаем 7-8 FFT кадров и усредняем — убирает шум)
              │
              ↓
[Поиск пика Mark: сканируем ±AFC_SEARCH_RANGE Гц от Mark_hz]
[Поиск пика Space: сканируем ±AFC_SEARCH_RANGE Гц от Space_hz]
              │
              ↓
[Параболическая интерполяция — точность до 1-2 Гц]
              │
              ↓
[Проверка: найденные пики имеют смысл?]
  - оба пика выше порога шума?
  - shift между ними разумный (100..1000 Гц)?
  - соотношение уровней Mark/Space близко к 1:1?
              │
   ДА         │         НЕТ
   ↓          │          ↓
[Применить]  [Сообщить: "NO SIGNAL" в статус-баре]
[анимация маркеров "скользит" на новую позицию]
```

### 6.4.2 Реализация

```c
#define AFC_SEARCH_RANGE  150.0f  // Гц — зона поиска от текущего маркера
#define AFC_SMOOTH_FRAMES  8      // кол-во FFT кадров для усреднения
#define TUNE_MIN_SNR_DB    6.0f   // минимальный SNR пика для принятия решения

// Накопление спектра для TUNE/AFC:
static float afc_spectrum_accum[WATERFALL_FFT_SIZE / 2];
static int   afc_accum_count = 0;

void afc_accumulate(float *fft_mag, int bins) {
    for (int i = 0; i < bins; i++)
        afc_spectrum_accum[i] += fft_mag[i];
    afc_accum_count++;
    if (afc_accum_count >= AFC_SMOOTH_FRAMES) {
        // нормализуем
        for (int i = 0; i < bins; i++)
            afc_spectrum_accum[i] /= AFC_SMOOTH_FRAMES;
        afc_accum_count = 0;  // готов к использованию
    }
}

// Параболическая интерполяция вершины пика:
// По трём точкам [y0, y1, y2] где y1 — максимум:
// peak_offset = 0.5 * (y0 - y2) / (y0 - 2*y1 + y2)
// Возвращает смещение в бинах (-0.5 .. +0.5)

float parabolic_peak(float y0, float y1, float y2) {
    float denom = y0 - 2.0f * y1 + y2;
    if (fabsf(denom) < 1e-6f) return 0.0f;
    return 0.5f * (y0 - y2) / denom;
}

// Найти точный пик вблизи target_hz в спектре:
typedef struct {
    float freq_hz;   // найденная частота
    float power_db;  // уровень пика в дБ
    bool  valid;     // пик найден?
} peak_result_t;

peak_result_t find_peak_near(float *spectrum, int bins,
                              float target_hz, float search_range_hz,
                              float sample_rate, int fft_size) {
    peak_result_t result = {0};
    float bin_hz = sample_rate / fft_size;

    int bin_lo = (int)((target_hz - search_range_hz) / bin_hz);
    int bin_hi = (int)((target_hz + search_range_hz) / bin_hz);
    bin_lo = MAX(1, bin_lo);
    bin_hi = MIN(fft_size/2 - 2, bin_hi);

    // Найти максимум в диапазоне:
    int peak_bin = bin_lo;
    float peak_val = spectrum[bin_lo];
    for (int i = bin_lo + 1; i <= bin_hi; i++) {
        if (spectrum[i] > peak_val) {
            peak_val = spectrum[i];
            peak_bin = i;
        }
    }

    // Проверить что это настоящий пик (не просто шум):
    float noise_floor = estimate_noise_floor(spectrum, bins);
    float snr_db = 20.0f * log10f(peak_val / (noise_floor + 1e-10f));
    if (snr_db < TUNE_MIN_SNR_DB) {
        result.valid = false;
        return result;
    }

    // Параболическая интерполяция:
    float y0 = spectrum[peak_bin - 1];
    float y1 = spectrum[peak_bin];
    float y2 = spectrum[peak_bin + 1];
    float offset = parabolic_peak(y0, y1, y2);  // -0.5 .. +0.5

    result.freq_hz  = (peak_bin + offset) * bin_hz;
    result.power_db = snr_db;
    result.valid    = true;
    return result;
}

// Оценка уровня шума (медиана нижних 30% бинов):
float estimate_noise_floor(float *spectrum, int bins) {
    // Берём среднее по полосам вне Mark/Space (упрощённо):
    float sum = 0; int count = 0;
    for (int i = 5; i < bins * 0.3f; i++) {
        sum += spectrum[i];
        count++;
    }
    return count > 0 ? sum / count : 1.0f;
}

// Главная функция TUNE:
tune_result_t tune_snap_to_peaks(marker_state_t *markers) {
    tune_result_t res = {0};

    if (afc_accum_count > 3) {
        // Используем то что есть (не ждём полного накопления)
        // нормализуем:
        float spec[WATERFALL_FFT_SIZE/2];
        for (int i = 0; i < WATERFALL_FFT_SIZE/2; i++)
            spec[i] = afc_spectrum_accum[i] / afc_accum_count;

        peak_result_t mark_peak = find_peak_near(
            spec, WATERFALL_FFT_SIZE/2,
            markers->mark_hz, AFC_SEARCH_RANGE,
            WATERFALL_SAMPLE_RATE, WATERFALL_FFT_SIZE);

        peak_result_t space_peak = find_peak_near(
            spec, WATERFALL_FFT_SIZE/2,
            markers->space_hz, AFC_SEARCH_RANGE,
            WATERFALL_SAMPLE_RATE, WATERFALL_FFT_SIZE);

        if (mark_peak.valid && space_peak.valid) {
            float new_shift = mark_peak.freq_hz - space_peak.freq_hz;

            // Проверка разумности shift:
            if (new_shift > 80.0f && new_shift < 1200.0f) {
                // Анимировать плавное перемещение маркеров:
                animate_markers_to(markers,
                                   mark_peak.freq_hz,
                                   space_peak.freq_hz);
                res.success = true;
                res.mark_hz  = mark_peak.freq_hz;
                res.space_hz = space_peak.freq_hz;
                res.snr_db   = (mark_peak.power_db + space_peak.power_db) / 2.0f;
            }
        } else {
            // Сигнал не найден:
            ui_show_status_message("NO SIGNAL", COLOR_RED, 2000);
        }
    }

    // Сбросить накопитель:
    memset(afc_spectrum_accum, 0, sizeof(afc_spectrum_accum));
    afc_accum_count = 0;

    return res;
}
```

### 6.4.3 Анимация перемещения маркеров

```c
// Маркеры плавно "скользят" от старой позиции к новой за 300 мс
// Это даёт визуальный фидбек что TUNE сработал

#define TUNE_ANIM_MS  300
#define TUNE_ANIM_STEPS  15  // кадров анимации

void animate_markers_to(marker_state_t *markers,
                         float target_mark_hz,
                         float target_space_hz) {
    float start_mark  = markers->mark_hz;
    float start_space = markers->space_hz;

    for (int step = 0; step <= TUNE_ANIM_STEPS; step++) {
        float t = (float)step / TUNE_ANIM_STEPS;
        // Ease-out: t = 1 - (1-t)^2
        float ease = 1.0f - (1.0f - t) * (1.0f - t);

        markers->mark_hz  = start_mark  + (target_mark_hz  - start_mark)  * ease;
        markers->space_hz = start_space + (target_space_hz - start_space) * ease;

        // Перерисовать только маркеры (не весь waterfall):
        waterfall_draw_markers_only();
        sleep_ms(TUNE_ANIM_MS / TUNE_ANIM_STEPS);
    }

    markers->mark_hz  = target_mark_hz;
    markers->space_hz = target_space_hz;
    markers->shift_hz = target_mark_hz - target_space_hz;

    // Применить к декодеру:
    rtty_set_frequencies(markers->mark_hz, markers->space_hz);
}
```

---

## 6.5 AFC — НЕПРЕРЫВНОЕ СЛЕЖЕНИЕ

После TUNE (грубая + точная подстройка) AFC держит настройку при небольших уходах частоты передатчика.

```c
// AFC работает только когда декодер активно принимает символы
// (чтобы не уходить на помехи в тишине)

#define AFC_MAX_CORRECTION_HZ  2.0f   // макс. коррекция за один шаг
#define AFC_ACTIVE_THRESHOLD   10     // символов/сек минимум для AFC
#define AFC_UPDATE_INTERVAL_MS 500    // как часто обновляем

typedef struct {
    bool  enabled;
    float rate_hz_per_s;    // скорость коррекции (Гц/сек)
    uint32_t last_update_ms;
} afc_state_t;

void afc_update_tick(afc_state_t *afc, marker_state_t *markers,
                     float *spectrum, int bins, int chars_per_sec) {
    if (!afc->enabled) return;
    if (chars_per_sec < AFC_ACTIVE_THRESHOLD) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - afc->last_update_ms < AFC_UPDATE_INTERVAL_MS) return;
    afc->last_update_ms = now;

    // Найти пики в узком диапазоне ±30 Гц от текущих маркеров:
    peak_result_t mp = find_peak_near(spectrum, bins,
                                       markers->mark_hz, 30.0f,
                                       WATERFALL_SAMPLE_RATE, WATERFALL_FFT_SIZE);
    peak_result_t sp = find_peak_near(spectrum, bins,
                                       markers->space_hz, 30.0f,
                                       WATERFALL_SAMPLE_RATE, WATERFALL_FFT_SIZE);

    if (mp.valid && sp.valid) {
        float delta_mark  = mp.freq_hz - markers->mark_hz;
        float delta_space = sp.freq_hz - markers->space_hz;
        float delta_avg = (delta_mark + delta_space) / 2.0f;

        // Ограничить скорость коррекции:
        delta_avg = CLAMP(delta_avg, -AFC_MAX_CORRECTION_HZ, AFC_MAX_CORRECTION_HZ);

        markers->mark_hz  += delta_avg;
        markers->space_hz += delta_avg;

        rtty_set_frequencies(markers->mark_hz, markers->space_hz);

        // Обновить маркеры на экране (без анимации — тихое движение):
        waterfall_draw_markers_only();
    }
}
```

---

## 6.6 ТУЛБАР — КНОПКИ УПРАВЛЕНИЯ (Y=216..240)

```
X=0     X=46    X=92    X=138   X=184   X=230   X=276  X=319
┌───────┬───────┬───────┬───────┬───────┬───────┬──────┐
│ TUNE  │ AFC●  │  CLR  │45.45▼ │ 170▼  │ LOCK  │  ☰   │
└───────┴───────┴───────┴───────┴───────┴───────┴──────┘
  46px    46px    46px    46px    46px    46px    43px
```

### 6.6.1 Описание кнопок тулбара

| Кнопка    | Функция                                                          |
|-----------|------------------------------------------------------------------|
| **TUNE**  | Запустить точную подстройку на пики (алгоритм 6.4)              |
| **AFC●**  | Вкл/выкл AFC. ● = зелёный если вкл, серый если выкл             |
| **CLR**   | Очистить текстовую зону                                          |
| **45.45▼**| Выпадающий список скоростей (45.45/50/75/100/110/150 Бод)       |
| **170▼**  | Выпадающий список shift (170/200/425/450/850/1000 Гц)           |
| **LOCK**  | Зафиксировать shift между маркерами (при drag двигаются вместе) |
| **☰**     | Открыть полное меню настроек                                     |

### 6.6.2 Выпадающий список (dropdown)

```c
// При нажатии на кнопку со стрелкой ▼ — появляется список над тулбаром:

typedef struct {
    const char *items[8];
    int count;
    int selected;
    int x, y_bottom;  // позиция (список растёт ВВЕРХ от тулбара)
    bool visible;
} dropdown_t;

// Список скоростей:
static dropdown_t baud_dropdown = {
    .items = {"45.45", "50", "75", "100", "110", "150"},
    .count = 6, .selected = 0,
    .x = 138, .y_bottom = 216
};

// Список shift:
static dropdown_t shift_dropdown = {
    .items = {"170 Hz", "200 Hz", "425 Hz", "450 Hz", "850 Hz", "1000 Hz"},
    .count = 6, .selected = 0,
    .x = 184, .y_bottom = 216
};

void dropdown_draw(dropdown_t *dd) {
    if (!dd->visible) return;
    int item_h = 20;
    int total_h = dd->count * item_h;
    int y_top = dd->y_bottom - total_h;

    // Фон с тенью:
    ili9341_fill_rect(dd->x - 2, y_top - 2, 62, total_h + 4, 0x2104);
    ili9341_fill_rect(dd->x, y_top, 58, total_h, 0x3186);

    for (int i = 0; i < dd->count; i++) {
        uint16_t bg = (i == dd->selected) ? COLOR_BTN_ACTIVE : 0x3186;
        ili9341_fill_rect(dd->x, y_top + i*item_h, 58, item_h, bg);
        ili9341_draw_string(dd->x + 4, y_top + i*item_h + 4,
                            dd->items[i], COLOR_TEXT, bg);
    }
}
```

---

## 6.7 ПОЛНОЕ МЕНЮ НАСТРОЕК (кнопка ☰)

```
┌─ НАСТРОЙКИ ──────────────────────────────────────────┐
│                                                       │
│  Скорость:    [●45.45] [○50] [○75] [○100] [○150]     │
│                                                       │
│  Shift:       [●170Hz] [○425Hz] [○850Hz] [○Custom]   │
│                                                       │
│  Mark:   [◄] [──────2295────────] [►]  2295 Гц       │
│  Space:  [◄] [──────2125────────] [►]  2125 Гц       │
│                    (слайдер ±500 Гц)                  │
│  Стоп-биты:   [●1.5] [○1.0]                          │
│                                                       │
│  AFC:         [●ВКЛ] [○ВЫКЛ]   Диапазон: [±50Гц▼]   │
│                                                       │
│  Яркость:     [◄] [████████░░░░] [►]  75%            │
│                                                       │
│  Инверсия:    [○ Норм (Mark>Space)] [●Инв(Space>Mark)]│
│                                                       │
│  [  ПРИМЕНИТЬ  ]              [  ОТМЕНА  ]           │
└───────────────────────────────────────────────────────┘
```

```c
// Слайдер (пример для Mark частоты):
typedef struct {
    float *value;
    float  min, max;
    int    x, y, width;
    bool   dragging;
} slider_t;

void slider_draw(slider_t *s) {
    float t = (*s->value - s->min) / (s->max - s->min);
    int knob_x = s->x + (int)(t * s->width);

    // Трек:
    ili9341_fill_rect(s->x, s->y + 6, s->width, 4, 0x4208);
    // Активная часть:
    ili9341_fill_rect(s->x, s->y + 6, knob_x - s->x, 4, COLOR_BTN_ACTIVE);
    // Ручка:
    ili9341_fill_circle(knob_x, s->y + 8, 8, COLOR_TEXT);
}
```

---

## 6.8 СТАТУС-БАР (Y=0..14)

```
┌──────────────────────────────────────────────────────┐
│ 45.45Bd │ 170Hz │ M:2295 S:2125 │ [▓▓▓▒▒] SNR:12dB  │
└──────────────────────────────────────────────────────┘
```

```c
typedef struct {
    float baud_rate;
    float shift_hz;
    float mark_hz;
    float space_hz;
    float snr_db;
    float mark_level;   // 0..1 — уровень тона Mark
    float space_level;  // 0..1 — уровень тона Space
    bool  afc_active;
    bool  signal_lock;  // есть ли сигнал
} status_bar_t;

void status_bar_draw(status_bar_t *s) {
    ili9341_fill_rect(0, 0, 320, 14, COLOR_STATUS_BG);

    char buf[64];
    snprintf(buf, sizeof(buf), "%.2fBd %dHz M:%d S:%d SNR:%.0fdB",
             s->baud_rate, (int)s->shift_hz,
             (int)s->mark_hz, (int)s->space_hz, s->snr_db);
    ili9341_draw_string(2, 3, buf, COLOR_TEXT, COLOR_STATUS_BG);

    // Индикатор уровней Mark/Space (мини-барграф):
    // Рисуем 2 горизонтальные полоски 20×4px в правой части
    int bar_x = 260;
    draw_level_bar(bar_x, 2,  20, 4, s->mark_level,  0xFFE0);  // жёлтый = Mark
    draw_level_bar(bar_x, 8,  20, 4, s->space_level, 0x07FF);  // голубой = Space

    // Точка AFC:
    if (s->afc_active)
        ili9341_draw_string(292, 3, "AFC", 0x07E0, COLOR_STATUS_BG);
}
```

---

## 6.9 ТЕКСТОВАЯ ЗОНА (Y=120..216)

```c
#define TEXT_COLS   40    // символов в строке (шрифт 8px × 40 = 320)
#define TEXT_ROWS   8     // строк (шрифт 12px × 8 = 96px)
#define TEXT_FONT_W  8
#define TEXT_FONT_H  12
#define TEXT_Y_TOP  120

static char text_buffer[TEXT_ROWS][TEXT_COLS + 1];
static int  text_cur_col = 0;
static int  text_cur_row = TEXT_ROWS - 1;  // текущая строка (снизу)

void text_append_char(char c) {
    if (c == '\n' || c == '\r') {
        text_scroll_up();
        text_cur_col = 0;
        return;
    }
    if (c < 0x20 || c > 0x7E) return;  // фильтр непечатаемых

    text_buffer[text_cur_row][text_cur_col] = c;
    // Отрисовать только один символ (не перерисовывать весь экран):
    text_draw_char(text_cur_col, text_cur_row, c, COLOR_TEXT);

    text_cur_col++;
    if (text_cur_col >= TEXT_COLS) {
        text_scroll_up();
        text_cur_col = 0;
    }
    // Мигающий курсор:
    text_draw_cursor(text_cur_col, text_cur_row);
}

void text_scroll_up(void) {
    // Сдвигаем строки вверх:
    memmove(text_buffer[0], text_buffer[1],
            (TEXT_ROWS - 1) * (TEXT_COLS + 1));
    memset(text_buffer[TEXT_ROWS - 1], 0, TEXT_COLS + 1);
    // Перерисовать всю текстовую зону:
    text_redraw_all();
}
```

---

## 6.10 ИТОГОВЫЙ АЛГОРИТМ ВЗАИМОДЕЙСТВИЯ ПОЛЬЗОВАТЕЛЯ

```
Пользователь включает прибор
        │
        ↓
Водопад показывает спектр 300..3300 Гц
Маркеры Mark(жёлт)/Space(голуб) стоят на дефолтных 2295/2125 Гц
        │
        ↓
Пользователь видит сигнал на водопаде (две яркие полоски)
        │
    ┌───┴────────────────────────────────┐
    │ Вариант A:                         │ Вариант B:
    │ Тащит маркеры пальцем              │ Нажимает TUNE
    │ примерно на нужные полоски         │ (если сигнал близко)
    └───┬────────────────────────────────┘
        │
        ↓
Нажимает TUNE → алгоритм 6.4 ищет пики ±150 Гц
→ параболическая интерполяция → точность ±2 Гц
→ маркеры анимированно скользят на точные пики
→ статус: "TUNED M:2297 S:2128 SNR:14dB"
        │
        ↓
AFC включён → тихо держит настройку
Текст появляется в текстовой зоне
        │
        ↓
Хочет поменять shift → нажимает [170▼] → выбирает 850 Hz
→ shift меняется, маркеры раздвигаются, снова жмёт TUNE
        │
        ↓
Нажимает [☰] → полное меню → меняет скорость/инверсию/AFC
```

---

*Раздел 6 v2.0 — Waterfall, Touch UI, TUNE/AFC алгоритмы*
# РАЗДЕЛ 6C: МАРКЕРЫ В СТИЛЕ FLDIGI — ТРИ ЛИНИИ + ИНФОПАНЕЛЬ
## Две боковые линии (Mark/Space) + центральная + данные о сигнале

---

## ВИЗУАЛЬНАЯ КОНЦЕПЦИЯ — КАК В FLDIGI

```
Водопад 320×96px, ось X = 300..3300 Гц:

Y=14 ┌──────────────────────────────────────────────────────────────┐
     │                   ░░░░░░░░░░░░░░░░░░                        │
     │                   ░ ██          ██ ░                        │
     │                   ░ ██          ██ ░  ← зелёная полупрозр.  │
     │                   ░ ████      ████ ░    зона между S и M    │
     │                   ░ ████      ████ ░                        │
Y=110└──┬──────────────┬─┴─┬──────────┬─┴─┬──────────────────────-┘
        │              │   │          │   │
       300 Гц         S   CTR        M  3300 Гц
                    голуб. белая   жёлтая
                    2125   2210    2295 Гц

Y=110 ──────────────────────────────────────────────────────────────
       |    |    |    |    |    |    |    |    |    |    |    |
      300  500  700  900  1100 1300 1500 1700 1900 2100 2300 2500 Гц
Y=120 ──────────────────────────────────────────────────────────────

Y=120 ┌──────────────────────────────────────────────────────────────┐
      │ S:2125  CTR:2210  M:2295  │ SHIFT:170Hz │ 45.45Bd │ SNR:14dB│
Y=134 └──────────────────────────────────────────────────────────────┘
```

---

## 6C.1 СТРУКТУРА ТРЁХ МАРКЕРОВ

```c
typedef struct {
    // Три частоты:
    float space_hz;     // левая линия  — голубая  (Space / нижний тон)
    float center_hz;    // центральная  — белая    (середина сигнала)
    float mark_hz;      // правая линия — жёлтая   (Mark  / верхний тон)

    // Производные параметры:
    float shift_hz;     // = mark_hz - space_hz

    // Состояние:
    bool  active;       // маркеры установлены?
    bool  manual_mode;  // режим ручного перетаскивания
    int   drag_which;   // 0=нет, 1=Space, 2=Center(весь блок), 3=Mark

    // Для анимации:
    float anim_space_hz;
    float anim_mark_hz;
} rtty_markers_t;

// center_hz всегда = (mark_hz + space_hz) / 2.0f
// При перемещении центра — Mark и Space двигаются симметрично
// При перемещении Mark или Space — центр пересчитывается
```

---

## 6C.2 ОТРИСОВКА ТРЁХ ЛИНИЙ НА ВОДОПАДЕ

```c
void waterfall_draw_markers(rtty_markers_t *m) {

    if (!m->active) {
        draw_tap_hint();  // подсказка "ТАП НА СИГНАЛ"
        return;
    }

    int px_space  = FREQ_TO_PX(m->space_hz);
    int px_center = FREQ_TO_PX(m->center_hz);
    int px_mark   = FREQ_TO_PX(m->mark_hz);
    int y_top = WF_Y_TOP;
    int y_bot = WF_Y_TOP + WF_PX_HEIGHT;  // Y=110

    // ── 1. Зелёная полупрозрачная зона между Space и Mark ──────────
    // Читаем из wf_buffer и подмешиваем зелёный 20%:
    for (int x = px_space + 1; x < px_mark; x++) {
        for (int y = y_top; y < y_bot; y++) {
            uint16_t orig = wf_pixel_get(x, y);
            // RGB565 blend: +20% зелёного
            uint8_t r = (orig >> 11) & 0x1F;
            uint8_t g = (orig >> 5)  & 0x3F;
            uint8_t b = (orig)       & 0x1F;
            g = MIN(63, g + 8);  // +20% зелёного канала
            ili9341_draw_pixel(x, y, (r<<11)|(g<<5)|b);
        }
    }

    // ── 2. Линия Space — голубая, 1px ──────────────────────────────
    ili9341_draw_vline(px_space, y_top, WF_PX_HEIGHT, 0x07FF);

    // ── 3. Линия Mark — жёлтая, 1px ───────────────────────────────
    ili9341_draw_vline(px_mark, y_top, WF_PX_HEIGHT, 0xFFE0);

    // ── 4. Центральная линия — белая пунктирная ────────────────────
    for (int y = y_top; y < y_bot; y += 4) {
        ili9341_draw_pixel(px_center, y,     0xFFFF);
        ili9341_draw_pixel(px_center, y + 1, 0xFFFF);
        // y+2 и y+3 — пропуск (пунктир)
    }

    // ── 5. Треугольные маркеры-ручки на нижней границе водопада ────
    // Рисуем ▼ треугольники прямо на Y=109 (нижний пиксель водопада)
    // Space: голубой ▼
    draw_marker_handle(px_space,  y_bot - 1, 0x07FF);
    // Center: белый ▼
    draw_marker_handle(px_center, y_bot - 1, 0xFFFF);
    // Mark: жёлтый ▼
    draw_marker_handle(px_mark,   y_bot - 1, 0xFFE0);
}

// Треугольная "ручка" маркера — 7×5 пикселей:
//    ███████
//     █████
//      ███
//       █
void draw_marker_handle(int cx, int y_base, uint16_t color) {
    for (int row = 0; row < 4; row++) {
        int half = 3 - row;
        for (int dx = -half; dx <= half; dx++)
            ili9341_draw_pixel(cx + dx, y_base - row, color);
    }
}
```

---

## 6C.3 ИНФОПАНЕЛЬ — СТРОКА ДАННЫХ О СИГНАЛЕ (Y=120..134)

```
┌──────────────────────────────────────────────────────────────┐
│ S:2125  ◆:2210  M:2295  │ SH:170Hz │ 45.45Bd │ SNR:14dB  AFC│
└──────────────────────────────────────────────────────────────┘
  голуб.  белый   жёлт.     белый      зелёный    белый     зелён.
```

```c
// Инфопанель занимает Y=120..133 (14px высота)
// Фон тёмно-серый 0x1082

#define INFO_Y       120
#define INFO_HEIGHT  14
#define INFO_BG      0x1082   // тёмно-серый
#define INFO_DIV     0x4208   // разделитель — серый

void infopanel_draw(rtty_markers_t *m, rtty_status_t *s) {
    ili9341_fill_rect(0, INFO_Y, 320, INFO_HEIGHT, INFO_BG);

    if (!m->active) {
        // Нет сигнала — показать подсказку:
        draw_string_small(4, INFO_Y + 3,
                          "ТАП НА СИГНАЛ ДЛЯ НАСТРОЙКИ",
                          0x8410, INFO_BG);
        return;
    }

    char buf[64];
    int x = 2;

    // S: частота Space — голубым:
    snprintf(buf, sizeof(buf), "S:%d", (int)m->space_hz);
    x += draw_string_small(x, INFO_Y + 3, buf, 0x07FF, INFO_BG);

    // Разделитель:
    x += draw_string_small(x, INFO_Y + 3, " \x07 ", 0x4208, INFO_BG);

    // CTR: центральная частота — белым:
    snprintf(buf, sizeof(buf), "\x0F:%d", (int)m->center_hz);
    x += draw_string_small(x, INFO_Y + 3, buf, 0xFFFF, INFO_BG);

    // Разделитель:
    x += draw_string_small(x, INFO_Y + 3, " \x07 ", 0x4208, INFO_BG);

    // M: частота Mark — жёлтым:
    snprintf(buf, sizeof(buf), "M:%d", (int)m->mark_hz);
    x += draw_string_small(x, INFO_Y + 3, buf, 0xFFE0, INFO_BG);

    // Вертикальный разделитель:
    ili9341_draw_vline(x + 2, INFO_Y + 1, INFO_HEIGHT - 2, INFO_DIV);
    x += 6;

    // SH: ширина (shift):
    snprintf(buf, sizeof(buf), "SH:%dHz", (int)m->shift_hz);
    x += draw_string_small(x, INFO_Y + 3, buf, 0xFFFF, INFO_BG);

    ili9341_draw_vline(x + 2, INFO_Y + 1, INFO_HEIGHT - 2, INFO_DIV);
    x += 6;

    // Скорость — зелёным:
    snprintf(buf, sizeof(buf), "%.2fBd", s->baud_rate);
    x += draw_string_small(x, INFO_Y + 3, buf, 0x07E0, INFO_BG);

    ili9341_draw_vline(x + 2, INFO_Y + 1, INFO_HEIGHT - 2, INFO_DIV);
    x += 6;

    // SNR:
    snprintf(buf, sizeof(buf), "SNR:%ddB", (int)s->snr_db);
    x += draw_string_small(x, INFO_Y + 3, buf, 0xFFFF, INFO_BG);

    // AFC индикатор (если включён):
    if (s->afc_enabled) {
        draw_string_small(x + 4, INFO_Y + 3, "AFC",
                          s->afc_locked ? 0x07E0 : 0xFFE0, INFO_BG);
    }

    // Индикатор уровней Mark/Space — мини-барграф справа:
    // Два вертикальных столбика 4×10px в правом углу
    int bx = 298;
    draw_level_vbar(bx,     INFO_Y + 2, 4, 10, s->space_level, 0x07FF);
    draw_level_vbar(bx + 6, INFO_Y + 2, 4, 10, s->mark_level,  0xFFE0);
}
```

---

## 6C.4 ИТОГОВАЯ РАСКЛАДКА ЭКРАНА (ОБНОВЛЁННАЯ)

```
X=0                                                            X=319
Y=0   ┌──────────────────────────────────────────────────────────┐
      │ ██ RTTY DECODER  45.45Bd  170Hz  SNR:14dB  [AFC●][LOCK] │ 14px статус
Y=14  ├──────────────────────────────────────────────────────────┤
      │                                                          │
      │         W A T E R F A L L   (96 пикселей)               │
      │                                                          │ 96px
      │          S░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░M               │
      │          │         ┆                    │                │
      │        голуб    пунктир               жёлт               │
Y=110 ├──────────────────────────────────────────────────────────┤
      │  ▼голуб        ▼белый              ▼жёлт                 │ ← ручки
      │ 300|  500|  700| 1000| 1300| 1700| 2000| 2300| 2600|3000│ 10px шкала
Y=120 ├──────────────────────────────────────────────────────────┤
      │ S:2125 ◆:2210 M:2295 │SH:170Hz│45.45Bd│SNR:14dB│AFC ▌▌ │ 14px инфо
Y=134 ├──────────────────────────────────────────────────────────┤
      │                                                          │
      │  DE UA9XYZ QTH NOVOSIBIRSK OP SERGEY RST 599 599 K      │
      │  CQ CQ CQ DE UA9XYZ K                                   │ 82px текст
      │  █                                                       │ (~6 строк)
      │                                                          │
Y=216 ├──────────────────────────────────────────────────────────┤
      │ [TUNE] [AFC●] [CLR] [45.45▼] [170Hz▼] [LOCK] [  ☰  ]   │ 24px тулбар
Y=240 └──────────────────────────────────────────────────────────┘
```

---

## 6C.5 ДЕТАЛИ ЧАСТОТНОЙ ШКАЛЫ (Y=110..120)

```c
// 10px высота, статичная, перерисовывается только при смене диапазона
// Засечки каждые 200 Гц, подписи каждые 500 Гц

#define SCALE_Y       110
#define SCALE_HEIGHT  10
#define SCALE_BG      0x0841   // очень тёмно-серый

void freq_scale_draw(void) {
    ili9341_fill_rect(0, SCALE_Y, 320, SCALE_HEIGHT, SCALE_BG);

    for (int freq = 300; freq <= 3300; freq += 200) {
        int px = FREQ_TO_PX(freq);
        if (px < 0 || px >= 320) continue;

        bool major = (freq % 500 == 0);  // крупная засечка
        uint16_t color = major ? 0x8410 : 0x4208;

        // Вертикальная засечка:
        int tick_h = major ? 5 : 3;
        ili9341_draw_vline(px, SCALE_Y, tick_h, color);

        // Подпись для крупных засечек:
        if (major && px > 10 && px < 310) {
            char buf[8];
            if (freq >= 1000)
                snprintf(buf, sizeof(buf), "%dk", freq/1000);
            else
                snprintf(buf, sizeof(buf), "%d", freq);
            draw_string_tiny(px - 6, SCALE_Y + 2, buf, 0x6B4D, SCALE_BG);
        }
    }

    // Отрисовать позиции маркеров на шкале (треугольные ручки):
    if (markers.active) {
        draw_marker_handle(FREQ_TO_PX(markers.space_hz),
                           SCALE_Y + SCALE_HEIGHT - 1, 0x07FF);
        draw_marker_handle(FREQ_TO_PX(markers.center_hz),
                           SCALE_Y + SCALE_HEIGHT - 1, 0xFFFF);
        draw_marker_handle(FREQ_TO_PX(markers.mark_hz),
                           SCALE_Y + SCALE_HEIGHT - 1, 0xFFE0);
    }
}
```

---

## 6C.6 ЗОНЫ TOUCH ДЛЯ ТРЁХ МАРКЕРОВ

```
Зоны касания на водопаде (Y=14..120):

  ◄──────10px──►◄──────────────────────►◄──────10px──►
  ┌────────────┐                        ┌────────────┐
  │  ЗОНА      │    ЗОНА ЦЕНТРА         │  ЗОНА      │
  │  SPACE     │    (drag всего блока)  │  MARK      │
  │  ±12px     │                        │  ±12px     │
  └────────────┘                        └────────────┘
       ↑                  ↑                   ↑
   голубая           белая пунктир         жёлтая
   линия              линия               линия

Логика обработки:
  - Касание ±12px от Space-линии  → drag только Space
  - Касание ±12px от Mark-линии   → drag только Mark
  - Касание между маркерами       → drag всего блока (Mark+Space вместе)
  - Касание вне маркеров          → новый тап → поиск сигнала
  - Long press в любой зоне       → режим точной подстройки пальцем
```

```c
touch_zone_t get_touch_zone(int tx, int ty, rtty_markers_t *m) {
    if (ty < WF_Y_TOP || ty > SCALE_Y + SCALE_HEIGHT)
        return ZONE_OUTSIDE;

    int px_s = FREQ_TO_PX(m->space_hz);
    int px_m = FREQ_TO_PX(m->mark_hz);

    #define MARKER_HIT 12

    if (abs(tx - px_s) <= MARKER_HIT) return ZONE_SPACE;
    if (abs(tx - px_m) <= MARKER_HIT) return ZONE_MARK;

    // Между маркерами:
    int lo = MIN(px_s, px_m);
    int hi = MAX(px_s, px_m);
    if (tx > lo && tx < hi)           return ZONE_CENTER;

    // Вне маркеров — поиск нового сигнала:
    return ZONE_NEW_SIGNAL;
}
```

---

## 6C.7 ОБНОВЛЕНИЕ ИНФОПАНЕЛИ В РЕАЛЬНОМ ВРЕМЕНИ

```c
// Инфопанель обновляется каждые 200 мс (5 Гц) — не мерцает
// Обновляем только изменившиеся поля (сравниваем с предыдущими значениями)

typedef struct {
    float space_hz, center_hz, mark_hz, shift_hz;
    float baud_rate, snr_db;
    float mark_level, space_level;
    bool  afc_enabled, afc_locked;
} rtty_status_t;

static rtty_status_t prev_status = {0};

void infopanel_update(rtty_status_t *cur) {
    // Обновляем только если что-то изменилось:
    if (fabsf(cur->space_hz   - prev_status.space_hz)   > 1.0f ||
        fabsf(cur->mark_hz    - prev_status.mark_hz)    > 1.0f ||
        fabsf(cur->snr_db     - prev_status.snr_db)     > 0.5f ||
        fabsf(cur->baud_rate  - prev_status.baud_rate)  > 0.1f ||
        cur->afc_enabled != prev_status.afc_enabled     ||
        cur->afc_locked  != prev_status.afc_locked) {

        infopanel_draw(&markers, cur);
        prev_status = *cur;
    }
}
```

---

*Раздел 6C v1.0 — Три маркера в стиле fldigi + инфопанель*
# РАЗДЕЛ 6B: УТОЧНЁННАЯ КОНЦЕПЦИЯ МАРКЕРОВ И TOUCH-УПРАВЛЕНИЯ
## Один тап на сигнал → прибор сам находит Mark и Space

---

## КОНЦЕПЦИЯ ВЗАИМОДЕЙСТВИЯ

Пользователь видит на водопаде **две яркие вертикальные полоски** — это RTTY сигнал.
Нужно просто **нажать на левую полоску** (или правую — неважно, прибор разберётся).
Прибор сам определяет обе частоты.

```
Водопад до нажатия:
┌──────────────────────────────────────────────────────────────┐
│                    ██                  ██                    │
│                    ██                  ██                    │
│                   ████                ████                   │
│                   ████                ████                   │
│  300 Гц                 ↑              ↑              3300 Гц │
│                    Space(?)          Mark(?)                  │
└──────────────────────────────────────────────────────────────┘
                      ☝ пользователь тапает сюда

Водопад после тапа + TUNE:
┌──────────────────────────────────────────────────────────────┐
│                S   ██              M   ██                    │
│                ┃   ██              ┃   ██                    │
│               ▓┃▓ ████            ▓┃▓ ████                   │
│               ▓┃▓ ████            ▓┃▓ ████                   │
│  300 Гц      2125 Гц            2295 Гц              3300 Гц │
└──────────────────────────────────────────────────────────────┘
  Маркеры встали точно. Текст пошёл.
```

---

## АЛГОРИТМ "УМНОГО ТАПА" — ПОДРОБНО

### Шаг 1: Пользователь тапает на водопад

```c
void waterfall_on_tap(int tap_x, int tap_y) {
    // Переводим пиксель X → частота
    float tap_freq = PX_TO_FREQ(tap_x);

    // Запускаем поиск двух пиков вблизи тапа
    find_rtty_pair_near(tap_freq);
}
```

### Шаг 2: Поиск пары пиков (RTTY = всегда два пика)

```c
// Логика: пользователь тапнул рядом с одним из пиков.
// Ищем: этот пик + второй пик на типичном RTTY-расстоянии от него.

void find_rtty_pair_near(float tap_freq_hz) {
    float *spec = afc_get_averaged_spectrum();  // усреднённый спектр

    // 1. Найти ближайший пик к месту тапа (±200 Гц):
    peak_result_t first_peak = find_peak_near(spec, tap_freq_hz, 200.0f);

    if (!first_peak.valid) {
        ui_show_message("Сигнал не найден", COLOR_RED);
        return;
    }

    // 2. Перебрать типичные RTTY shift'ы и найти второй пик:
    static const float RTTY_SHIFTS[] = {
        170.0f, 200.0f, 425.0f, 450.0f, 850.0f, 1000.0f
    };

    peak_result_t best_second = {0};
    float best_shift = 0;
    float best_score = 0;

    for (int i = 0; i < 6; i++) {
        float sh = RTTY_SHIFTS[i];

        // Ищем второй пик на расстоянии sh выше и ниже:
        peak_result_t above = find_peak_near(spec,
                                              first_peak.freq_hz + sh, 40.0f);
        peak_result_t below = find_peak_near(spec,
                                              first_peak.freq_hz - sh, 40.0f);

        // Оцениваем качество пары: сумма SNR обоих пиков
        if (above.valid) {
            float score = first_peak.power_db + above.power_db;
            if (score > best_score) {
                best_score  = score;
                best_second = above;
                best_shift  = sh;
            }
        }
        if (below.valid) {
            float score = first_peak.power_db + below.power_db;
            if (score > best_score) {
                best_score  = score;
                best_second = below;
                best_shift  = sh;
            }
        }
    }

    if (!best_second.valid) {
        // Второй пик не найден — возможно нестандартный shift
        // Просто ставим маркер на найденный пик, ждём TUNE
        ui_show_message("Найден 1 сигнал. Нажмите TUNE", COLOR_YELLOW);
        marker_set_center(first_peak.freq_hz, config.shift_hz);
        return;
    }

    // 3. Определить какой из них Mark (выше по частоте), какой Space:
    float freq_high = MAX(first_peak.freq_hz, best_second.freq_hz);
    float freq_low  = MIN(first_peak.freq_hz, best_second.freq_hz);

    // В стандартном RTTY: Mark = выше, Space = ниже
    // (для инвертированного — наоборот, это настраивается)
    if (config.inverted) {
        markers.mark_hz  = freq_low;
        markers.space_hz = freq_high;
    } else {
        markers.mark_hz  = freq_high;
        markers.space_hz = freq_low;
    }
    markers.shift_hz = best_shift;

    // 4. Анимировать установку маркеров:
    animate_markers_to(&markers, markers.mark_hz, markers.space_hz);

    // 5. Применить к декодеру:
    rtty_set_frequencies(markers.mark_hz, markers.space_hz);
    config.baud_rate = auto_detect_baud_from_spectrum(spec);

    // 6. Включить AFC:
    afc.enabled = true;

    ui_show_message("OK — декодирование...", COLOR_GREEN);
}
```

### Шаг 3: Параболическая интерполяция пика (точность ±1 Гц)

```c
// FFT даёт разрешение 18.75 Гц/бин.
// Параболическая интерполяция по трём точкам вершины:
// уточняет позицию пика до ±1-2 Гц без увеличения FFT.

peak_result_t find_peak_near(float *spectrum, float target_hz,
                              float search_range_hz) {
    peak_result_t result = {.valid = false};
    float bin_hz = (float)SAMPLE_RATE / FFT_SIZE;

    int bin_lo = MAX(1, (int)((target_hz - search_range_hz) / bin_hz));
    int bin_hi = MIN(FFT_SIZE/2 - 2,
                     (int)((target_hz + search_range_hz) / bin_hz));

    // Найти максимальный бин:
    int peak_bin = bin_lo;
    for (int i = bin_lo+1; i <= bin_hi; i++)
        if (spectrum[i] > spectrum[peak_bin]) peak_bin = i;

    // Оценить шум и SNR:
    float noise = estimate_noise_floor(spectrum, FFT_SIZE/2);
    float snr   = 20.0f * log10f(spectrum[peak_bin] / (noise + 1e-9f));
    if (snr < 5.0f) return result;  // слишком слабый сигнал

    // Параболическая интерполяция:
    float y0 = spectrum[peak_bin - 1];
    float y1 = spectrum[peak_bin];
    float y2 = spectrum[peak_bin + 1];
    float offset = 0.5f * (y0 - y2) / (y0 - 2.0f*y1 + y2 + 1e-9f);
    // offset в диапазоне -0.5 .. +0.5 бина

    result.freq_hz  = (peak_bin + offset) * bin_hz;
    result.power_db = snr;
    result.valid    = true;
    return result;
}
```

---

## ВИЗУАЛЬНОЕ ОТОБРАЖЕНИЕ МАРКЕРОВ

### До тапа — нет маркеров, только подсказка

```
┌──────────────────────────────────────────────────────┐
│                                                      │
│         [  ТАП НА СИГНАЛ ДЛЯ НАСТРОЙКИ  ]           │
│              (мигающая подсказка)                    │
│                                                      │
└──────────────────────────────────────────────────────┘
```

### После тапа — маркеры с метками

```
        S              M
        ┃              ┃
   ─────╂──────────────╂─────
   ░░░░░▓░░░░░░░░░░░░░░▓░░░░░   ← полупрозрачная зелёная полоса
   ─────╂──────────────╂─────
        ┃              ┃
     голубой         жёлтый
    2125 Гц         2295 Гц
```

```c
void waterfall_draw_markers(void) {
    if (!markers.active) {
        // Нет маркеров — рисуем подсказку
        draw_hint_text("ТАП НА СИГНАЛ");
        return;
    }

    int mark_px  = FREQ_TO_PX(markers.mark_hz);
    int space_px = FREQ_TO_PX(markers.space_hz);
    int y_top    = WF_Y_TOP;
    int y_bot    = WF_Y_TOP + WF_PX_HEIGHT;

    // Полупрозрачная полоса между маркерами:
    int x_lo = MIN(mark_px, space_px);
    int x_hi = MAX(mark_px, space_px);
    for (int x = x_lo; x <= x_hi; x++) {
        for (int y = y_top; y < y_bot; y++) {
            // Читаем текущий пиксель из буфера и смешиваем с зелёным:
            uint16_t px = wf_buffer_get_pixel(x, y);
            wf_draw_pixel_blended(x, y, px, 0x07E0, 0.2f);
        }
    }

    // Линия Space (голубая):
    for (int y = y_top; y < y_bot; y++)
        ili9341_draw_pixel(space_px, y, 0x07FF);

    // Линия Mark (жёлтая):
    for (int y = y_top; y < y_bot; y++)
        ili9341_draw_pixel(mark_px, y, 0xFFE0);

    // Метки S и M над линиями (на частотной шкале):
    ili9341_draw_char(space_px - 3, WF_Y_TOP + WF_PX_HEIGHT + 1,
                      'S', 0x07FF, COLOR_BG);
    ili9341_draw_char(mark_px  - 3, WF_Y_TOP + WF_PX_HEIGHT + 1,
                      'M', 0xFFE0, COLOR_BG);

    // Частотные подписи под маркерами (малый шрифт 6×8):
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", (int)markers.space_hz);
    draw_small_text(space_px - 12, WF_Y_TOP + WF_PX_HEIGHT + 10,
                    buf, 0x07FF);
    snprintf(buf, sizeof(buf), "%d", (int)markers.mark_hz);
    draw_small_text(mark_px  - 12, WF_Y_TOP + WF_PX_HEIGHT + 10,
                    buf, 0xFFE0);
}
```

---

## РУЧНАЯ КОРРЕКЦИЯ ПОСЛЕ ТАПА

Если TUNE встал неточно — пользователь может подкорректировать:

```
Жест                    Действие
─────────────────────────────────────────────────────
Тап на водопад          Умный поиск пары пиков → TUNE
Drag по водопаду        Сдвинуть весь блок Mark+Space
Long press (>500 мс)    Войти в режим раздельного
                        перетаскивания маркеров
Double tap              Сбросить маркеры, начать заново
```

```c
// Long press → режим раздельного перетаскивания:
// В этом режиме появляются две "ручки" на маркерах:

void waterfall_enter_manual_mode(void) {
    markers.manual_mode = true;

    // Рисуем треугольные "ручки" на маркерах:
    //   Space-маркер:  ▼ голубой треугольник
    //   Mark-маркер:   ▼ жёлтый треугольник
    // Пользователь тащит ручку — меняет только эту частоту

    ui_show_message("Режим ручной настройки. TUNE для подстройки.", 0xFFFF);
}

// Выход из ручного режима — тап на кнопку TUNE или вне водопада
```

---

## КНОПКА TUNE В ТУЛБАРЕ

После любого ручного перемещения — нажать TUNE для точной подстройки:

```
Состояния кнопки TUNE:
┌─────────┐   Серая, неактивна   — нет сигнала
│  TUNE   │
└─────────┘

┌─────────┐   Синяя              — готова к использованию
│  TUNE   │
└─────────┘

┌─────────┐   Мигает жёлтым      — идёт поиск пиков (~300мс)
│  TUNE   │
└─────────┘

┌─────────┐   Зелёная вспышка    — подстройка выполнена успешно
│  TUNE   │
└─────────┘

┌─────────┐   Красная вспышка    — пики не найдены (нет сигнала)
│  TUNE   │
└─────────┘
```

---

## ИТОГОВАЯ ПОСЛЕДОВАТЕЛЬНОСТЬ РАБОТЫ

```
1. Включение
   └─ Водопад показывает спектр
   └─ Мигает подсказка "ТАП НА СИГНАЛ"

2. Пользователь видит два ярких пятна на водопаде
   └─ Тапает на любое из них (или между ними)

3. Прибор за ~300 мс:
   а) Накапливает и усредняет спектр
   б) Находит ближайший пик
   в) Перебирает стандартные shift'ы (170/200/425/450/850 Гц)
   г) Находит второй пик
   д) Параболическая интерполяция → точность ±1-2 Гц
   е) Анимированно ставит маркеры

4. AFC включается автоматически
   └─ Тихо держит настройку при дрейфе передатчика

5. Текст появляется в нижней части экрана

6. Если нужна коррекция:
   └─ Drag: сдвинуть весь блок
   └─ Long press: раздельная настройка маркеров
   └─ TUNE: повторная точная подстройка
```

---

*Раздел 6B v1.0 — Уточнённая концепция маркеров*
# РАЗДЕЛ 7: МИНИМИЗАЦИЯ ПОМЕХ ОТ SPI-ДИСПЛЕЯ НА АЦП
## Критически важный раздел — электромагнитная совместимость на макетной плате

---

## 7.1 ПОЧЕМУ ЭТО СЕРЬЁЗНАЯ ПРОБЛЕМА

SPI дисплей ILI9341 работает на частоте 40 МГц.
АЦП RP2350 измеряет аудиосигнал 300–3000 Гц с разрядностью 12 бит.

```
Источники помех от дисплея:
┌─────────────────────────────────────────────────────┐
│  SPI SCK 40 МГц         → гармоники до 400+ МГц    │
│  SPI MOSI (данные)      → широкополосный шум        │
│  Строчный скан ILI9341  → ~15 кГц периодический    │
│  DMA burst-передача     → импульсные помехи         │
│  Ток подсветки (PWM BL) → если PWM попадает в АФ   │
└─────────────────────────────────────────────────────┘

Пути проникновения помех в АЦП:
  1. По цепи питания AVDD (общая шина 3.3В)
  2. Через общий GND (падение напряжения на индуктивности дорожек)
  3. Прямая ёмкостная/индуктивная связь на макетке (провода рядом)
  4. Через GPIO — RP2350 имеет общий субстрат
```

**На макетной плате всё это усугубляется в 5–10 раз** по сравнению с правильной PCB — длинные провода, нет полигонов GND, всё рядом.

---

## 7.2 ПРОГРАММНЫЕ МЕТОДЫ (САМЫЕ ВАЖНЫЕ ДЛЯ МАКЕТКИ)

### 7.2.1 ГЛАВНЫЙ МЕТОД — Синхронизация АЦП и SPI (временное разделение)

Идея: **никогда не передавать данные по SPI в момент выборки АЦП.**
RP2350 сам управляет обоими — значит мы полностью контролируем расписание.

```
Временная диаграмма (один цикл 26.7 мс = 256 сэмплов при fs=9600):

│◄────────── 26.7 мс ──────────────────────────────────►│
│                                                        │
│ [АЦП активен — DMA захват 256 сэмплов = 26.7 мс]      │
│ ████████████████████████████████████████████████████  │
│                                                        │
│ [SPI дисплей — ЗАПРЕЩЁН во время захвата АЦП]          │
│ ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  │

НО: АЦП работает непрерывно — нельзя останавливать!
```

**Решение: микровременно́е разделение внутри периода АЦП:**

```c
// АЦП делает выборку каждые 1/9600 = 104 мкс
// Выборка длится ~2 мкс (время преобразования АЦП)
// В остальные ~102 мкс SPI может работать

// Реализация через callback после каждой выборки:

// В RP2350 АЦП с DMA:
// - Устанавливаем прерывание на завершение каждого сэмпла (DREQ)
// - В прерывании: SPI РАЗРЕШЁН
// - За 10 мкс до следующей выборки: SPI ЗАПРЕЩЁН

// Упрощённая версия для макетки:
// Передаём данные на дисплей МЕЖДУ блоками АЦП (ping-pong буфер)

volatile bool adc_block_ready = false;

// Ядро 0: захват АЦП
void core0_adc_loop(void) {
    while (1) {
        // Ждём готовности блока (DMA завершил 256 сэмплов):
        while (!adc_block_ready) tight_loop_contents();
        adc_block_ready = false;

        // Обрабатываем DSP — SPI в это время тоже идёт на Ядре 1
        // Но DMA АЦП уже пишет в СЛЕДУЮЩИЙ буфер — помехи в него,
        // а мы читаем УЖЕ ГОТОВЫЙ буфер
        process_dsp_block(adc_buf_ready ^ 1);
    }
}
```

### 7.2.2 Аппаратное разделение через управление SPI CS и MOSI

```c
// Перед критическими моментами АЦП — принудительно глушим SPI линии:

static inline void spi_force_idle(void) {
    // Переводим MOSI, SCK в состояние driven-low — минимум излучения
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SIO);
    gpio_put(PIN_MOSI, 0);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SIO);
    gpio_put(PIN_SCK, 0);
    gpio_put(PIN_LCD_CS, 1);  // CS высокий = дисплей отключён
}

static inline void spi_restore(void) {
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
}
```

### 7.2.3 PWM подсветки — КРИТИЧНО для АЦП

**Проблема:** PWM на BL-пине создаёт периодические помехи. Если частота PWM попадёт в звуковой диапазон — катастрофа.

```c
// НЕПРАВИЛЬНО — PWM 1 кГц попадает прямо в аудиополосу:
// pwm_set_wrap(slice, 4800);  // 100 МГц / 4800 = ~20 кГц — ещё можно
// pwm_set_wrap(slice, 48000); // 100 МГц / 48000 = 2 кГц — ПЛОХО!

// ПРАВИЛЬНО — PWM как можно выше, вне звуковой полосы:
void backlight_pwm_init(uint8_t brightness_percent) {
    gpio_set_function(PIN_BL, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PIN_BL);

    // Частота PWM = 100 МГц / wrap = максимально высокая:
    // wrap=100 → 1 МГц PWM — далеко от аудио, RC фильтр задавит
    pwm_set_wrap(slice, 100);
    pwm_set_chan_level(slice, pwm_gpio_to_channel(PIN_BL),
                       brightness_percent);
    pwm_set_enabled(slice, true);
}

// ИЛИ — полностью статическое управление (нет PWM вообще):
// gpio_put(PIN_BL, 1);  // просто HIGH = 100% яркость
// Это лучший вариант для макетки!
#define BL_USE_STATIC  1  // раскомментировать для макетки
```

### 7.2.4 Программный усреднитель АЦП (oversampling)

```c
// Вместо одной выборки на сэмпл — берём 4 подряд и усредняем
// Это даёт +6 дБ подавления случайного шума (в т.ч. цифровых помех)
// Цена: нужно в 4 раза больше производительности АЦП (не проблема)

// При fs=9600 Гц и oversampling×4:
// Физическая частота АЦП = 38400 Гц
// Каждые 4 отсчёта → 1 итоговый сэмпл 9600 Гц

#define OVERSAMPLING  4

uint16_t adc_read_oversampled(void) {
    uint32_t sum = 0;
    for (int i = 0; i < OVERSAMPLING; i++) {
        sum += adc_read();
    }
    return (uint16_t)(sum / OVERSAMPLING);
}

// С DMA — конфигурируем DMA на OVERSAMPLING*BUFFER_SIZE отсчётов,
// затем усредняем блоками по OVERSAMPLING в обработчике
```

### 7.2.5 Цифровая фильтрация — удаление остаточных помех

```c
// После АЦП — каскад цифровых фильтров:

// 1. DC-блокировка (убирает постоянное смещение и медленный дрейф):
float dc_block(float x, float *state) {
    // IIR фильтр высоких частот с полюсом 0.995 (Fc ≈ 24 Гц при fs=9600)
    float y = x - *state;
    *state = *state * 0.995f + x * 0.005f;
    return y;
}

// 2. Notch-фильтры на известные частоты помех:
// Если в спектре видна постоянная помеха (например 15.6 кГц строчная
// развёртка ILI9341 — алиасится при fs=9600 как 9600 - 15600%9600 = 6000 Гц?
// Нет, 15600 > 4800 (Найквист), значит алиасинг:
// alias = |15600 - k*9600| для ближайшего k
// k=1: |15600-9600| = 6000 Гц > Найквист — не попадает
// k=2: |15600-19200| = 3600 Гц — МОЖЕТ ПОПАСТЬ если есть гармоники!

// На всякий случай — notch на 3.6 кГц (выше аудиополосы RTTY):
typedef struct { float b0,b1,b2,a1,a2,x1,x2,y1,y2; } biquad_t;

float biquad_process(biquad_t *f, float x) {
    float y = f->b0*x + f->b1*f->x1 + f->b2*f->x2
                      - f->a1*f->y1  - f->a2*f->y2;
    f->x2=f->x1; f->x1=x;
    f->y2=f->y1; f->y1=y;
    return y;
}

// Инициализация notch на частоту f0 (Гц):
void biquad_notch_init(biquad_t *f, float f0, float Q,
                        float sample_rate) {
    float w0 = 2.0f * M_PI * f0 / sample_rate;
    float alpha = sinf(w0) / (2.0f * Q);
    float b0 =  1.0f;
    float b1 = -2.0f * cosf(w0);
    float b2 =  1.0f;
    float a0 =  1.0f + alpha;
    float a1 = -2.0f * cosf(w0);
    float a2 =  1.0f - alpha;
    f->b0=b0/a0; f->b1=b1/a0; f->b2=b2/a0;
    f->a1=a1/a0; f->a2=a2/a0;
    f->x1=f->x2=f->y1=f->y2=0;
}
```

### 7.2.6 Снижение частоты SPI для макетки

```c
// На PCB 40 МГц — нормально.
// На макетной плате длинные провода → большая индуктивность → выбросы!
// Снизить SPI до 10-20 МГц — меньше помех, стабильнее работа:

#define SPI_FREQ_LCD_BREADBOARD  10000000  // 10 МГц для макетки

// Компенсация снижения скорости:
// При 10 МГц передача одной строки 320px (640 байт) = 512 мкс
// Полный экран 240 строк = 122 мс → ~8 FPS
// Для waterfall достаточно обновлять только изменившиеся строки!
// → обновляем только 1 новую строку waterfall за раз = 640 байт = 512 мкс
// Это приемлемо.
```

---

## 7.3 АППАРАТНЫЕ МЕТОДЫ ДЛЯ МАКЕТНОЙ ПЛАТЫ

### 7.3.1 Схема питания — САМОЕ ВАЖНОЕ

```
ПРАВИЛЬНАЯ схема питания на макетке:

USB 5В
  │
  ├─[AMS1117-3.3 или аналог]─────┐
  │                               │
  │         ┌─ 100нФ керамика ─┐  │
  │         │                  │  │
  │    ┌────┤ 3.3В ЦИФРОВОЕ   ├┤  │  ← для RP2350, SPI дисплея
  │    │    │  (DVDD)          │  │
  │    │    └──────────────────┘  │
  │    │                          │
  │    │    ┌─────────────────┐   │
  │    │    │ ДРОССЕЛЬ        │   │
  │    │    │ 10-100 мкГн     │   │
  │    │    │ (или ферритовая │   │
  │    │    │  бусина BLM)    │   │
  │    │    └────────┬────────┘   │
  │    │             │            │
  │    │    ┌────────┴────────┐   │
  │    │    │ 10мкФ танталовый│   │
  │    │    │ + 100нФ керамика│   │
  │    │    └─────────────────┘   │
  │    │             │            │
  │    └─────────────┤            │
  │              3.3В АНАЛОГОВОЕ  │  ← только для АЦП (GPIO26) и RC цепи
  │              (AVDD)           │
  └───────────────────────────────┘

Дроссель/феррит создаёт барьер между цифровым и аналоговым питанием.
Это самая эффективная мера для макетки!

Если дросселя нет — хотя бы резистор 10-22 Ом в линии AVDD.
```

### 7.3.2 Разводка GND на макетке

```
ПЛОХО (типичная ошибка):
  RP2350 GND ────────────────────── GND ILI9341
      │                                   │
      └──── GND RC-фильтра АЦП ───────────┘
                   ↑
        Ток дисплея течёт через GND АЦП
        → падение напряжения на индуктивности → шум

ХОРОШО (звезда GND):
  Точка звезды GND (один общий провод к USB GND)
         │
    ┌────┴────┬──────────────┐
    │         │              │
  RP2350    ILI9341       RC-фильтр АЦП
   GND        GND          GND
   (короткий провод к точке звезды)

На макетной плате: используй отдельные провода к одной точке,
НЕ используй шину GND макетки как общую магистраль
(она имеет значительное сопротивление и индуктивность).
```

### 7.3.3 Физическое расположение на макетке

```
┌────────────────────────────────────────────────────────┐
│  МАКЕТНАЯ ПЛАТА (вид сверху)                           │
│                                                        │
│  ┌──────────┐          ┌──────────────┐                │
│  │ RC фильтр│          │  ILI9341     │                │
│  │ АЦП вход │          │  дисплей     │                │
│  │ (GPIO26) │          │              │                │
│  └────┬─────┘          └──────────────┘                │
│       │  МАКСИМАЛЬНО                                   │
│       │  УДАЛИТЬ                                       │
│       │  ДРУГ ОТ ДРУГА!                                │
│  ┌────┴──────────────────────────────┐                 │
│  │     Raspberry Pi Pico 2           │                 │
│  │     RP2350                        │                 │
│  └───────────────────────────────────┘                 │
│                                                        │
│  ← аудио вход                         SPI дисплей →   │
│    слева                                     справа    │
└────────────────────────────────────────────────────────┘

Правила:
  ✓ Аудиовход и RC-фильтр — с одного края макетки
  ✓ Дисплей и его SPI провода — с другого края
  ✓ Провода SPI — короткие, не идут параллельно аудио проводам
  ✓ GPIO26 (АЦП) — короткий провод, не рядом с SPI
  ✗ НЕ тянуть SPI провода через всю макетку
  ✗ НЕ класть SPI и аудио провода рядом и параллельно
```

### 7.3.4 Конденсаторы развязки — ПОЛНАЯ СПЕЦИФИКАЦИЯ ДЛЯ МАКЕТКИ

#### КРИТИЧЕСКОЕ ПРАВИЛО МОНТАЖА:
**Керамический конденсатор работает ТОЛЬКО если стоит вплотную к точке установки.**
Провод 5 см на макетке имеет индуктивность ~25 нГн — на частоте 40 МГц
это сопротивление ~6 Ом, что полностью убивает эффект развязки.
**Максимальное расстояние от ножки до конденсатора: 1–2 см.**

---

#### ТАБЛИЦА КОНДЕНСАТОРОВ — ВСЕ 8 ШТУК

```
№    Номинал     Тип              Место установки                 Пины Pico 2    Зачем
─────────────────────────────────────────────────────────────────────────────────────────────
C1   100 нФ     Керамика X7R     AVDD → GND, ВПЛОТНУЮ к Pico     Пин 36 → GND   Подавление ВЧ помех на АЦП питании
C2   10 мкФ     Танталовый/Эл-т  AVDD → GND, рядом с Pico        Пин 36 → GND   Резервуар энергии, НЧ развязка
C3   100 нФ     Керамика X7R     DVDD → GND, ВПЛОТНУЮ к Pico     Пин 36 → GND   ВЧ развязка цифрового питания
C4   100 нФ     Керамика X7R     VCC → GND ILI9341, ВПЛОТНУЮ     У дисплея      Подавление ВЧ броска тока дисплея
C5   10 мкФ     Электролит       VCC → GND ILI9341, рядом         У дисплея      Резервуар при burst SPI передаче
C6   1-4.7 нФ   Керамика NP0/C0G GPIO26 → GND                    Пин 31 → GND   Доп. ВЧ фильтр входа АЦП
C7   100 нФ     Керамика X7R     После резистора 10 Ом → GND     В аналог. цепи RC фильтр аналогового питания
C8   100 нФ     Керамика X7R     3.3В шина → GND у регулятора    У LDO/USB      Общая развязка шины питания
─────────────────────────────────────────────────────────────────────────────────────────────
Итого: 8 конденсаторов
```

---

#### СХЕМА РАССТАНОВКИ НА МАКЕТКЕ

```
                    [USB / LDO 3.3В]
                          │
                    C8: 100нФ к GND  ← здесь у источника питания
                          │
              ┌───────────┴───────────────────────────┐
              │                                        │
    [R_iso: 10 Ом]                            [Цифровая шина 3.3В]
              │                                        │
    C7: 100нФ к GND                          C3: 100нФ к GND (у Pico, пин 36)
              │                                        │
    [Аналоговая шина]                         [RP2350 DVDD]
              │
    C2: 10мкФ к GND ──┐
    C1: 100нФ к GND ──┘  ← ОБА вплотную к пину 36 Pico
              │
    [RP2350 AVDD — пин 36]
              │
    [RC фильтр: 1кОм + 43нФ] ← твоя уже готовая цепь
              │
    C6: 1-4.7нФ к GND ← прямо у пина GPIO26 (пин 31)
              │
    [GPIO26 = АЦП вход]


    [ILI9341 VCC] ──── C5: 10мкФ к GND ──┐
                   └── C4: 100нФ к GND ──┘  ← ОБА вплотную к дисплею
```

---

#### ЧТО КУПИТЬ (конкретные типы для магазина):

```
C1, C3, C4, C7, C8:  100 нФ, 0.1 мкФ, керамика, X7R, 50В
                      (любой производитель: Murata, KEMET, Samsung)
                      Типоразмер: 0805 (для пайки) или через отверстие

C2:                   10 мкФ, танталовый, 10В минимум, тип А или В
                      ИЛИ электролит 10 мкФ 16В (хуже но дешевле)

C5:                   10 мкФ, электролит, 10В, малогабаритный

C6:                   1 нФ, NP0/C0G керамика, 50В
                      (NP0/C0G важен — стабильная ёмкость, нет пьезоэффекта)
```

---

#### ПОРЯДОК УСТАНОВКИ НА МАКЕТКЕ (важен!):

```
Шаг 1: Сначала поставить C1+C2 у пина 36 Pico — ДО включения питания
Шаг 2: C4+C5 у дисплея — ДО подключения дисплея
Шаг 3: C6 прямо у пина GPIO26 — ДО подачи аудиосигнала
Шаг 4: C3, C7, C8 — в цепь питания
Шаг 5: Подключить питание и проверить напряжения
Шаг 6: Только после этого — подавать аудиосигнал и включать прошивку
```

---

#### ПРОВЕРКА ЭФФЕКТИВНОСТИ:

После сборки — в прошивке запустить тест из раздела 7.4:
```
Шум без SPI (дисплей выключен):  должно быть < -50 дБ
Шум с SPI   (дисплей работает):  должно быть < -40 дБ
Деградация:                       норма < 10 дБ
```
Если деградация > 10 дБ — проверить расположение C1 (скорее всего далеко от пина 36).

### 7.3.5 Феррит / дроссель в линии питания

```
Если есть ферритовая бусина (например BLM18PG221SN1):
  3.3В шина → [феррит] → AVDD RP2350 (AGND отдельно!)

Если нет феррита — резистор 10 Ом:
  3.3В шина → [10 Ом] → [10 мкФ + 100 нФ к GND] → AVDD
  Потеря: падение 10 Ом при токе АЦП (мкА) = микровольты — незаметно
  Выигрыш: фильтр RC с паразитной ёмкостью по питанию

На Pico 2 AVDD (аналоговое питание АЦП) — это пин 33 (AGND) и 36 (3V3).
Отдельного AVDD вывода нет — питание общее.
Поэтому: ставим LC/RC фильтр во внешней схеме перед RC-цепью аудиотракта.
```

---

## 7.4 ПРОГРАММНЫЙ WATCHDOG ДЛЯ КОНТРОЛЯ КАЧЕСТВА АЦП

```c
// Детектор помех: если спектр вдруг показывает нереально
// высокий уровень на нехарактерных частотах — это помехи

void adc_noise_monitor(float *spectrum, int bins) {
    float bin_hz = (float)SAMPLE_RATE / FFT_SIZE;

    // Проверяем "пустые" зоны — там где RTTY сигнала быть не должно:
    // Полоса 3500-4800 Гц (выше аудиополосы):
    int bin_lo = (int)(3500.0f / bin_hz);
    int bin_hi = (int)(4800.0f / bin_hz);
    bin_hi = MIN(bin_hi, FFT_SIZE/2 - 1);

    float noise_sum = 0;
    for (int i = bin_lo; i <= bin_hi; i++)
        noise_sum += spectrum[i];
    float noise_avg = noise_sum / (bin_hi - bin_lo + 1);

    // Если шум выше порога — предупреждение на экране:
    if (noise_avg > NOISE_WARNING_THRESHOLD) {
        ui_show_noise_warning(noise_avg);
        // Опционально: временно снизить скорость SPI
        spi_set_baudrate(spi1, SPI_FREQ_LCD_BREADBOARD / 2);
    }
}
```

---

## 7.5 ИТОГОВЫЙ ЧЕКЛИСТ ДЛЯ МАКЕТКИ

### Аппаратные меры (приоритет по эффективности):

```
Приоритет 1 — ОБЯЗАТЕЛЬНО:
  ☐ Конденсатор 100 нФ прямо у AVDD/AGND RP2350
  ☐ Конденсатор 100 нФ у питания ILI9341
  ☐ GPIO26 (АЦП) физически далеко от SPI линий
  ☐ GND звезда — все GND к одной точке, не по шине макетки
  ☐ PWM подсветки — 1 МГц или статический HIGH

Приоритет 2 — ОЧЕНЬ ЖЕЛАТЕЛЬНО:
  ☐ Резистор 10 Ом + 10 мкФ в линии питания аналоговой части
  ☐ SPI провода на дисплей — короткие (< 10 см)
  ☐ Аудиовход и дисплей — разные стороны макетки
  ☐ Снизить SPI до 10-20 МГц (spi_set_baudrate)

Приоритет 3 — ЖЕЛАТЕЛЬНО:
  ☐ Феррит в линии питания АЦП
  ☐ Витая пара для GND+SPI сигналов дисплея
  ☐ Доп. конденсатор 1-4.7 нФ на GPIO26 к GND
```

### Программные меры (реализовать в прошивке):

```
  ☐ PWM подсветки ≥ 500 кГц (лучше статический HIGH)
  ☐ Oversampling АЦП × 4
  ☐ DC-блокировка после АЦП
  ☐ SPI частота 10 МГц для макетки (не 40!)
  ☐ Notch фильтры на известные частоты помех
  ☐ Монитор шума АЦП с предупреждением на экране
```

---

## 7.6 КАК ПРОВЕРИТЬ ЧТО ПОМЕХИ ПОБЕЖДЕНЫ

```c
// Тест 1: Спектр без сигнала (вход закорочен на GND через 1кОм)
// На водопаде должно быть равномерно тёмно, без ярких полос

// Тест 2: Включить/выключить дисплей программно
void test_spi_interference(void) {
    printf("=== Тест помех от SPI ===\n");

    // Шаг 1: Дисплей выключен (SPI idle)
    spi_force_idle();
    sleep_ms(100);
    float noise_off = measure_noise_floor();
    printf("Шум без SPI: %.1f dB\n", noise_off);

    // Шаг 2: Дисплей активно обновляется
    spi_restore();
    // Запустить непрерывный waterfall redraw на 500 мс
    sleep_ms(500);
    float noise_on = measure_noise_floor();
    printf("Шум с SPI:   %.1f dB\n", noise_on);

    float degradation = noise_on - noise_off;
    printf("Деградация:  %.1f dB %s\n",
           degradation,
           degradation < 3.0f ? "(НОРМА)" :
           degradation < 10.0f ? "(ПРИЕМЛЕМО)" : "(ПЛОХО — нужны доп. меры)");
}
```

---

*Раздел 7 v1.0 — EMC для макетной платы, аппаратные и программные меры*
# ПОЛНОЕ ТЕХНИЧЕСКОЕ ЗАДАНИЕ v3.0
## Профессиональный RTTY-декодер на RP2350
## Максимальное использование железа + профессиональные DSP алгоритмы

---

# ЧАСТЬ I: АППАРАТНАЯ ПЛАТФОРМА RP2350 — ПОЛНОЕ ИСПОЛЬЗОВАНИЕ

---

## 1.1 РАЗГОН ПРОЦЕССОРА

```c
// RP2350 штатно: 150 МГц
// Разгон до 300 МГц — стабильно поддерживается большинством экземпляров
// При 300 МГц DSP-бюджет удваивается

void system_overclock_init(void) {
    // Установить системную частоту 300 МГц:
    set_sys_clock_khz(300000, true);

    // Проверить что частота установилась:
    uint32_t actual = clock_get_hz(clk_sys);
    if (actual < 290000000) {
        // Не взял 300 — попробовать 280:
        set_sys_clock_khz(280000, true);
    }

    // Обновить UART/SPI после смены частоты:
    stdio_init_all();
    spi_set_baudrate(SPI_PORT, SPI_FREQ_LCD);
}

// При 300 МГц:
// - FIR 64 tap занимает ~4 мкс вместо ~8 мкс
// - FFT 512pt занимает ~400 мкс вместо ~800 мкс
// - Бюджет на 256 сэмплов АЦП: 26.7 мс → можно делать FFT каждый блок
```

---

## 1.2 CORTEX-M33 DSP РАСШИРЕНИЯ — АППАРАТНЫЕ ИНСТРУКЦИИ

RP2350 = Cortex-M33 с DSP extension + FPU (single precision).

```c
// CMakeLists.txt — обязательные флаги:
target_compile_options(rtty_decoder PRIVATE
    -O3                          // максимальная оптимизация
    -mfpu=fpv5-sp-d16            // FPU single precision
    -mfloat-abi=hard             // аппаратная FP ABI
    -mcpu=cortex-m33+nodsp       // или +dsp если поддерживается SDK
    -ffast-math                  // агрессивная FP оптимизация
    -funroll-loops               // разворачивание циклов
    -finline-functions           // инлайнинг
)

// Доступные DSP инструкции Cortex-M33:
// SMLAD  — умножение с накоплением двух пар 16-бит
// SMULBB — перемножение 16-бит операндов
// QADD   — сложение с насыщением
// USAT   — беззнаковое насыщение

// Использование CMSIS-DSP библиотеки (оптимизирована под M33):
#include "arm_math.h"

// Все FIR фильтры → arm_fir_f32()
// Все FFT         → arm_rfft_fast_f32()
// Все IIR         → arm_biquad_cascade_df2T_f32()
// Скалярные       → arm_dot_prod_f32(), arm_power_f32()
```

---

## 1.3 АППАРАТНЫЙ ИНТЕРПОЛЯТОР (SIO INTERP) — УНИКАЛЬНАЯ ФИЧА RP2350

```c
// RP2350 имеет два аппаратных интерполятора в SIO (Single-cycle I/O)
// Выполняют за 1 такт: умножение + сдвиг + маскирование
// Идеально для: sin/cos lookup таблиц, colormap waterfall, AGC

// Использование INTERP0 для быстрого доступа к sin-таблице:
void interp_sin_table_init(void) {
    interp_config cfg = interp_default_config();
    interp_config_set_shift(&cfg, 32 - SIN_TABLE_LOG2); // нормализация фазы
    interp_config_set_mask(&cfg, 0, SIN_TABLE_LOG2 - 1);
    interp_config_set_add_raw(&cfg, true);
    interp_set_config(interp0, 0, &cfg);
    interp0->base[0] = SIN_TABLE_STEP; // шаг фазы
    interp0->base[1] = (uint32_t)sin_table; // база таблицы
}

// После init — получить sin за 2 такта:
static inline float fast_sin_interp(void) {
    return *(float *)interp0->peek[1]; // адрес = base + (phase >> shift)
}

// Использование INTERP1 для colormap waterfall:
// Преобразование float magnitude → uint16_t RGB565 цвет за 1 такт
void interp_colormap_init(uint16_t *colormap, int size) {
    interp_config cfg = interp_default_config();
    interp_config_set_shift(&cfg, 23);  // float экспонента → индекс
    interp_config_set_mask(&cfg, 0, 4); // 32 цвета = 5 бит
    interp_set_config(interp1, 0, &cfg);
    interp1->base[1] = (uint32_t)colormap;
}
```

---

## 1.4 PIO (PROGRAMMABLE I/O) — STATE MACHINES

RP2350 имеет 3 блока PIO по 4 state machine = 12 SM всего.

### PIO SM0: Сверхбыстрый SPI для дисплея ILI9341

```c
// Стандартный SPI через hardware SPI — хорошо.
// PIO SPI — лучше: можно сделать 8-битную передачу без overhead,
// добавить автоматическое управление DC пином внутри SM.

// PIO программа для SPI + DC:
// .program spi_dc
// .side_set 2  ; SCK, DC
//   out pins, 8  side 0b01  ; данные + SCK=0, DC=1(data)
//   nop          side 0b11  ; SCK=1

// Это даёт: автоматический переход команда/данные без CPU overhead
// Скорость: до clock/2 = 150 МГц при разгоне → 75 МГц SPI
```

### PIO SM1: Захват АЦП с точным тактированием

```c
// Стандартный захват АЦП через DMA имеет джиттер ~несколько нс.
// PIO может тактировать запуск АЦП с точностью до 1 такта (3.3 нс при 300 МГц).
// Это критично для качества спектра FFT (равномерность выборок).

// .program adc_trigger
// .wrap_target
//   set pins, 1  [SAMPLE_PERIOD - 2]  ; триггер АЦП каждые N тактов
//   set pins, 0  [1]
// .wrap

// Частота выборки = sys_clock / SAMPLE_PERIOD
// При 300 МГц и SAMPLE_PERIOD=31250: fs = 9600.0 Гц точно
```

### PIO SM2: Touch controller XPT2046

```c
// XPT2046 требует медленный SPI (2 МГц) с особым протоколом.
// PIO SM выполняет опрос touch в фоне без CPU:
// каждые 10 мс опрашивает X,Y координаты и пишет в FIFO.
// Ядро 1 читает из FIFO когда удобно.
```

---

## 1.5 DMA — МНОГОКАНАЛЬНАЯ КОНВЕЙЕРНАЯ ОБРАБОТКА

```c
// RP2350: 12 каналов DMA
// Используем цепочку каналов для нулевого CPU overhead:

// Канал 0: АЦП → RAM буфер A (256 сэмплов)
// Канал 1: АЦП → RAM буфер B (ping-pong)
// Канал 0 завершился → автоматически запускает Канал 1 и наоборот
// CPU никогда не ждёт АЦП — просто берёт готовый буфер

// Канал 2: RAM (waterfall строка) → SPI TX FIFO (дисплей)
// Канал 3: SPI TX → управление CS/DC (через GPIO)
// Цепочка 2→3 отправляет строку дисплея полностью без CPU

// Канал 4: scratch DMA для FFT twiddling (внутренний)

void dma_pipeline_init(void) {
    // АЦП ping-pong:
    int ch_a = dma_claim_unused_channel(true);
    int ch_b = dma_claim_unused_channel(true);

    dma_channel_config ca = dma_channel_get_default_config(ch_a);
    channel_config_set_transfer_data_size(&ca, DMA_SIZE_16);
    channel_config_set_read_increment(&ca, false);
    channel_config_set_write_increment(&ca, true);
    channel_config_set_dreq(&ca, DREQ_ADC);
    channel_config_set_chain_to(&ca, ch_b);  // по завершении → ch_b
    dma_channel_configure(ch_a, &ca,
        adc_buf[0], &adc_hw->fifo,
        ADC_BUFFER_SIZE, false);

    dma_channel_config cb = dma_channel_get_default_config(ch_b);
    channel_config_set_transfer_data_size(&cb, DMA_SIZE_16);
    channel_config_set_read_increment(&cb, false);
    channel_config_set_write_increment(&cb, true);
    channel_config_set_dreq(&cb, DREQ_ADC);
    channel_config_set_chain_to(&cb, ch_a);  // по завершении → ch_a
    dma_channel_configure(ch_b, &cb,
        adc_buf[1], &adc_hw->fifo,
        ADC_BUFFER_SIZE, false);

    // IRQ только при завершении блока:
    dma_channel_set_irq0_enabled(ch_a, true);
    dma_channel_set_irq0_enabled(ch_b, true);
    irq_set_exclusive_handler(DMA_IRQ_0, adc_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    dma_channel_start(ch_a);
}
```

---

## 1.6 РАСПРЕДЕЛЕНИЕ ПАМЯТИ RP2350

```c
// RP2350: 520 КБ SRAM, разделена на банки
// Ключевой момент: DMA и CPU могут обращаться к разным банкам одновременно

// Распределение:
// SRAM0 (64 КБ) — стек Ядра 0 + DSP буферы (FIR, FFT)
// SRAM1 (64 КБ) — стек Ядра 1 + UI буферы
// SRAM2 (64 КБ) — АЦП DMA буферы ping-pong
// SRAM3 (64 КБ) — Waterfall frame buffer (61.4 КБ)
// SRAM4 (64 КБ) — sin/cos таблицы + коэффициенты FIR
// SRAM5 (64 КБ) — текстовый буфер + очередь символов

// Атрибуты размещения в памяти:
#define SRAM_BANK(n) __attribute__((section(".sram" #n)))

uint16_t adc_buf[2][ADC_BUFFER_SIZE]     SRAM_BANK(2);
uint16_t wf_buffer[WF_HEIGHT][WF_WIDTH]  SRAM_BANK(3);
float    sin_table[SIN_TABLE_SIZE]       SRAM_BANK(4);
float    fir_coeffs_mark[FIR_TAPS]      SRAM_BANK(4);
float    fir_coeffs_space[FIR_TAPS]     SRAM_BANK(4);
```

---

# ЧАСТЬ II: ПРОФЕССИОНАЛЬНЫЕ DSP АЛГОРИТМЫ

---

## 2.1 АЦП — ПРОФЕССИОНАЛЬНЫЙ ВХОДНОЙ ТРАКТ

### Oversampling + Decimation (как в профессиональных SDR)

```c
// Профессиональный подход: не семплировать прямо на 9600 Гц,
// а семплировать на 4× выше (38400 Гц) и затем децимировать.
// Это даёт:
//   +6 дБ к SNR (3 дБ на каждое удвоение)
//   Лучшее подавление алиасинга
//   Более крутая граница фильтра Найквиста

#define ADC_OVERSAMPLE    4
#define ADC_FS_PHYSICAL   38400   // реальная частота АЦП
#define ADC_FS_DECIMATED  9600    // рабочая частота после децимации

// Децимирующий фильтр (anti-aliasing + decimation):
// FIR низкочастотный, Fc = 4800 Гц (Найквист декоматора)
// 32 коэффициента — быстро, достаточно подавление

static arm_fir_decimate_instance_f32 decimate_fir;
static float decimate_coeffs[32]; // спроектировать заранее
static float decimate_state[32 + ADC_OVERSAMPLE - 1];

void decimation_init(void) {
    // Рассчитать коэффициенты низкочастотного FIR:
    // Fc = 4800 Гц, fs = 38400 Гц, 32 отвода, окно Кайзера β=6
    design_fir_lowpass(decimate_coeffs, 32,
                       4800.0f / 38400.0f, WINDOW_KAISER_6);
    arm_fir_decimate_init_f32(&decimate_fir, 32, ADC_OVERSAMPLE,
                               decimate_coeffs, decimate_state,
                               ADC_BUFFER_SIZE * ADC_OVERSAMPLE);
}
```

### AGC — Автоматическая регулировка усиления (цифровая)

```c
// Профессиональный AGC с раздельными константами атаки и спада
// (как в трансиверах ICOM, Kenwood)

typedef struct {
    float gain;           // текущий коэффициент усиления
    float target_level;   // целевой RMS уровень (0.5 = -6 дБFS)
    float attack_tc;      // постоянная атаки (быстро при перегрузке)
    float release_tc;     // постоянная спада  (медленно при тишине)
    float max_gain;       // ограничение максимального усиления
    float min_gain;
} agc_t;

// Параметры как в профессиональных SDR приёмниках:
// Attack:  ~10 мс  (быстро реагирует на сильный сигнал)
// Release: ~500 мс (медленно отпускает — нет "помпажа")

void agc_init(agc_t *agc, float fs) {
    agc->gain        = 1.0f;
    agc->target_level = 0.3f;
    agc->attack_tc   = expf(-1.0f / (0.010f * fs));  // 10 мс
    agc->release_tc  = expf(-1.0f / (0.500f * fs));  // 500 мс
    agc->max_gain    = 100.0f;  // +40 дБ максимум
    agc->min_gain    = 0.01f;   // -40 дБ минимум
}

float agc_process(agc_t *agc, float sample) {
    float output = sample * agc->gain;
    float level  = fabsf(output);

    if (level > agc->target_level) {
        // Атака — быстро снижаем усиление:
        agc->gain *= agc->attack_tc;
        // Или через RMS: agc->gain *= (agc->target_level / level) * (1 - attack_tc);
    } else {
        // Спад — медленно поднимаем:
        agc->gain /= agc->release_tc;
    }

    agc->gain = CLAMP(agc->gain, agc->min_gain, agc->max_gain);
    return output;
}
```

---

## 2.2 FSK ДЕМОДУЛЯЦИЯ — ПРОФЕССИОНАЛЬНЫЕ МЕТОДЫ

### Метод 1: Дифференциальный детектор (используется в MMTTY)

```c
// Классический FSK детектор через дифференцирование фазы.
// Работает без знания точных частот Mark/Space — нужна только полоса.
// Хорошо работает при слабых сигналах и при дрейфе частоты.

typedef struct {
    float prev_i, prev_q;  // предыдущие I/Q значения
    float lpf_state;       // состояние ФНЧ на выходе
    float lpf_alpha;       // коэффициент ФНЧ
} diff_detector_t;

float diff_fsk_detect(diff_detector_t *d, float i, float q) {
    // Дифференциальная фаза = Im(z × conj(z_prev))
    // = I*Q_prev - Q*I_prev
    float diff = i * d->prev_q - q * d->prev_i;

    d->prev_i = i;
    d->prev_q = q;

    // ФНЧ сглаживание:
    d->lpf_state += d->lpf_alpha * (diff - d->lpf_state);
    return d->lpf_state;
    // >0 → Mark, <0 → Space
}
```

### Метод 2: Квадратурный коррелятор (используется в fldigi)

```c
// Два согласованных фильтра — один настроен на Mark, другой на Space.
// Оптимально по критерию максимального правдоподобия.
// Лучше всего работает при известных точных частотах.

typedef struct {
    // I/Q гетеродины для Mark и Space:
    float mark_phase,  mark_phase_step;
    float space_phase, space_phase_step;
    // ФНЧ огибающих:
    float mark_i_lpf,  mark_q_lpf;
    float space_i_lpf, space_q_lpf;
    float lpf_alpha;
} correlator_t;

void correlator_init(correlator_t *c, float mark_hz, float space_hz,
                     float baud_rate, float fs) {
    c->mark_phase_step  = 2.0f * M_PI * mark_hz  / fs;
    c->space_phase_step = 2.0f * M_PI * space_hz / fs;
    c->mark_phase = c->space_phase = 0;
    // Полоса ФНЧ = 0.7 * baud_rate (оптимально для FSK):
    c->lpf_alpha = 1.0f - expf(-2.0f * M_PI * 0.7f * baud_rate / fs);
    c->mark_i_lpf = c->mark_q_lpf = 0;
    c->space_i_lpf = c->space_q_lpf = 0;
}

float correlator_process(correlator_t *c, float sample) {
    // Mark канал:
    float mi = sample * arm_cos_f32(c->mark_phase);
    float mq = sample * arm_sin_f32(c->mark_phase);
    c->mark_i_lpf += c->lpf_alpha * (mi - c->mark_i_lpf);
    c->mark_q_lpf += c->lpf_alpha * (mq - c->mark_q_lpf);
    float mark_env = sqrtf(c->mark_i_lpf  * c->mark_i_lpf +
                           c->mark_q_lpf  * c->mark_q_lpf);
    c->mark_phase += c->mark_phase_step;
    if (c->mark_phase > 2*M_PI) c->mark_phase -= 2*M_PI;

    // Space канал:
    float si = sample * arm_cos_f32(c->space_phase);
    float sq = sample * arm_sin_f32(c->space_phase);
    c->space_i_lpf += c->lpf_alpha * (si - c->space_i_lpf);
    c->space_q_lpf += c->lpf_alpha * (sq - c->space_q_lpf);
    float space_env = sqrtf(c->space_i_lpf * c->space_i_lpf +
                            c->space_q_lpf * c->space_q_lpf);
    c->space_phase += c->space_phase_step;
    if (c->space_phase > 2*M_PI) c->space_phase -= 2*M_PI;

    // Дискриминатор: Mark - Space
    return mark_env - space_env;
    // >0 → Mark (1), <0 → Space (0)
}
```

### Метод 3: PLL FSK детектор (используется в профессиональных модемах)

```c
// ФАПЧ отслеживает мгновенную частоту сигнала.
// Выход ФАПЧ = мгновенная частота → FSK демодулятор.
// Исключительно устойчив к шуму и многолучёвости.

typedef struct {
    float phase;         // накопленная фаза
    float freq;          // текущая оценка частоты
    float center_freq;   // центральная частота (Mark+Space)/2
    float kp, ki;        // коэффициенты пропорционального и интегрального
    float integrator;    // интегратор ошибки
    float prev_sample;
} pll_demod_t;

void pll_demod_init(pll_demod_t *p, float center_hz,
                    float bw_hz, float fs) {
    p->center_freq = 2.0f * M_PI * center_hz / fs;
    p->freq = p->center_freq;
    p->phase = p->integrator = 0;
    // Полоса захвата = bw_hz (обычно shift/2):
    float wn = 2.0f * M_PI * bw_hz / fs;
    p->kp = 2.0f * 0.707f * wn;   // zeta = 0.707 (критическое демпфирование)
    p->ki = wn * wn;
}

float pll_demod_process(pll_demod_t *p, float sample) {
    // Фазовый детектор (перемножение):
    float ref_i = arm_cos_f32(p->phase);
    float ref_q = arm_sin_f32(p->phase);
    // Ошибка фазы = Q-компонента после перемножения:
    float error = sample * ref_q;  // упрощённо

    // Петлевой фильтр PI:
    p->integrator += p->ki * error;
    float correction = p->kp * error + p->integrator;

    p->freq  = p->center_freq + correction;
    p->phase += p->freq;
    if (p->phase > M_PI)  p->phase -= 2*M_PI;
    if (p->phase < -M_PI) p->phase += 2*M_PI;

    // Выход: correction > 0 → выше центра → Mark
    //        correction < 0 → ниже центра → Space
    return correction;
}
```

### КОМБИНИРОВАННЫЙ ДЕМОДУЛЯТОР — ВСЕ ТРИ МЕТОДА ПАРАЛЛЕЛЬНО

```c
// Профессиональное решение: запускаем все три детектора параллельно,
// берём взвешенное решение по голосованию — как в Phosphor RTTY

typedef struct {
    diff_detector_t  diff;
    correlator_t     corr;
    pll_demod_t      pll;
    float            weights[3];   // вес каждого детектора
    float            confidence;   // уверенность в текущем решении
} combined_demod_t;

float combined_demod_process(combined_demod_t *d,
                              float i, float q, float sample) {
    float out_diff = diff_fsk_detect(&d->diff, i, q);
    float out_corr = correlator_process(&d->corr, sample);
    float out_pll  = pll_demod_process(&d->pll,  sample);

    // Нормализация каждого в [-1, +1]:
    float sum = d->weights[0] * SIGN(out_diff)
              + d->weights[1] * SIGN(out_corr)
              + d->weights[2] * SIGN(out_pll);

    // Уверенность = насколько все три согласны:
    d->confidence = fabsf(sum) / 3.0f;

    return sum;  // >0 → Mark, <0 → Space
}
```

---

## 2.3 FIR ФИЛЬТРЫ — ПРОФЕССИОНАЛЬНОЕ ПРОЕКТИРОВАНИЕ

### Банк полосовых фильтров с несколькими окнами

```c
// Разные окна для разных условий приёма:
// - Прямоугольное:  максимальная крутизна, но боковые лепестки -13 дБ
// - Хэмминг:       хороший компромисс (-42 дБ), используется в fldigi
// - Кайзер β=8:    профессиональный стандарт (-80 дБ), медленнее
// - Блэкман-Харрис: -92 дБ, для слабых сигналов рядом с сильными

typedef enum {
    WINDOW_HAMMING,
    WINDOW_HANN,
    WINDOW_BLACKMAN,
    WINDOW_BLACKMAN_HARRIS,
    WINDOW_KAISER_6,
    WINDOW_KAISER_8,
    WINDOW_KAISER_10,
} window_type_t;

// Адаптивный выбор окна в зависимости от SNR:
window_type_t select_optimal_window(float snr_db, float adjacent_signal_db) {
    if (adjacent_signal_db > 30.0f)
        return WINDOW_BLACKMAN_HARRIS;  // сильная помеха рядом
    if (snr_db < 6.0f)
        return WINDOW_KAISER_8;         // слабый сигнал — минимум боковых
    if (snr_db < 15.0f)
        return WINDOW_BLACKMAN;
    return WINDOW_HAMMING;              // нормальные условия
}

// Оптимальное число отводов FIR для бода и частоты дискретизации:
// N = (A - 8) / (2.285 * Delta_w)
// A = затухание в дБ, Delta_w = ширина переходной полосы
// Для shift=170 Гц, baud=45.45: переходная полоса ≈ 50 Гц
// N = (60 - 8) / (2.285 * 2*pi*50/9600) ≈ 70 отводов

#define FIR_TAPS_NARROW   96    // для shift 170 Гц
#define FIR_TAPS_WIDE     48    // для shift 850 Гц
```

### Адаптивный FIR (LMS алгоритм) — как в профессиональных НЧ-фильтрах

```c
// LMS (Least Mean Squares) адаптивный фильтр подстраивает свои
// коэффициенты под условия приёма в реальном времени.
// Использование: подавление периодических помех, эхо-компенсация.

typedef struct {
    float weights[LMS_TAPS];
    float delay[LMS_TAPS];
    float mu;              // шаг адаптации (learning rate)
    int   tap_index;
} lms_filter_t;

#define LMS_TAPS  32

float lms_filter_process(lms_filter_t *f, float input,
                          float desired) {
    // Сдвиговый регистр:
    f->delay[f->tap_index] = input;
    f->tap_index = (f->tap_index + 1) % LMS_TAPS;

    // Вычислить выход:
    float output = 0;
    for (int i = 0; i < LMS_TAPS; i++)
        output += f->weights[i] *
                  f->delay[(f->tap_index + i) % LMS_TAPS];

    // Ошибка и обновление весов:
    float error = desired - output;
    float mu_norm = f->mu / (arm_dot_prod_energy(f->delay, LMS_TAPS) + 1e-6f);
    for (int i = 0; i < LMS_TAPS; i++)
        f->weights[i] += mu_norm * error *
                          f->delay[(f->tap_index + i) % LMS_TAPS];

    return output;
}
```

---

## 2.4 СИНХРОНИЗАЦИЯ БИТОВ — ПРОФЕССИОНАЛЬНЫЕ МЕТОДЫ

### Early-Late Gate (используется во всех профессиональных модемах)

```c
// Классический метод синхронизации символов.
// Вместо PLL — вычисляем энергию в "раннем" и "позднем" окне.
// Разность энергий → ошибка тайминга.
// Используется в MMTTY, fldigi, профессиональных RTTY декодерах.

typedef struct {
    float   symbol_period;  // в сэмплах (fs / baud_rate)
    float   phase;          // текущая фаза 0..1
    float   phase_error;    // накопленная ошибка
    float   loop_bw;        // полоса петли (обычно 0.01..0.05)
    float   d1, d2;         // задержки для early/late
    float   early_energy;
    float   late_energy;
    float   on_time_energy;
    int     samples_per_sym;
} early_late_t;

#define EL_GATE_WIDTH  0.25f  // ширина ворот = 25% символьного периода

int early_late_process(early_late_t *el, float sample,
                        float *symbol_out) {
    int bit_clock = 0;
    el->phase += 1.0f / el->symbol_period;

    // Early (фаза 0.25..0.50):
    if (el->phase > 0.25f && el->phase < 0.50f)
        el->early_energy += sample * sample;

    // On-time (фаза 0.45..0.55 — центр символа):
    if (el->phase > 0.45f && el->phase < 0.55f)
        el->on_time_energy += sample * sample;

    // Late (фаза 0.50..0.75):
    if (el->phase > 0.50f && el->phase < 0.75f)
        el->late_energy += sample * sample;

    if (el->phase >= 1.0f) {
        // Конец символа — вычислить ошибку:
        float timing_error = el->early_energy - el->late_energy;

        // Обновить фазу через петлевой фильтр:
        el->phase -= 1.0f;
        el->phase -= el->loop_bw * timing_error;
        el->phase  = CLAMP(el->phase, -0.1f, 0.1f) + 0.0f;

        // Выход: знак on-time накопления
        *symbol_out = el->on_time_energy;
        el->early_energy = el->late_energy = el->on_time_energy = 0;
        bit_clock = 1;
    }
    return bit_clock;
}
```

### Оптимальный коррелятор символов (matched filter)

```c
// Согласованный фильтр для прямоугольного символа FSK:
// Интегрируем (накапливаем) демодулированный сигнал ровно T секунд
// (один битовый период), затем делаем решение и сбрасываем.
// Это оптимально по SNR для AWGN канала.

typedef struct {
    float   accumulator;
    int     count;
    int     period_samples;  // = round(fs / baud_rate)
    float   last_decision;
} integrate_dump_t;

int integrate_dump(integrate_dump_t *id, float demod_out,
                    float *bit_out) {
    id->accumulator += demod_out;
    id->count++;

    if (id->count >= id->period_samples) {
        *bit_out = id->accumulator;
        id->accumulator = 0;
        id->count = 0;
        return 1;  // готов бит
    }
    return 0;
}
```

---

## 2.5 SNR ИЗМЕРИТЕЛЬ И ОЦЕНКА КАЧЕСТВА КАНАЛА

```c
// Профессиональная оценка SNR через спектральный анализ:
// SNR = мощность в полосе Mark+Space / мощность вне полосы

typedef struct {
    float snr_db;         // текущий SNR
    float ber_estimate;   // оценка BER из EVM
    float frequency_error; // смещение центральной частоты (AFC ошибка)
    float amplitude_imbalance; // дисбаланс уровней Mark/Space
    float timing_offset;   // смещение тайминга в долях символа
    char  quality_label[8]; // "POOR" / "FAIR" / "GOOD" / "EXCEL"
} signal_quality_t;

void measure_signal_quality(float *spectrum, int bins,
                              float mark_hz, float space_hz,
                              float baud_rate, float fs,
                              signal_quality_t *q) {
    float bin_hz = fs / (bins * 2);

    // Полоса сигнала: от (space_hz - baud) до (mark_hz + baud):
    int sig_lo = MAX(0, (int)((space_hz - baud_rate) / bin_hz));
    int sig_hi = MIN(bins-1, (int)((mark_hz  + baud_rate) / bin_hz));

    float signal_power = 0, noise_power = 0;
    int noise_count = 0;

    for (int i = 0; i < bins; i++) {
        if (i >= sig_lo && i <= sig_hi)
            signal_power += spectrum[i] * spectrum[i];
        else {
            noise_power += spectrum[i] * spectrum[i];
            noise_count++;
        }
    }
    noise_power /= noise_count;  // нормировка
    noise_power *= (sig_hi - sig_lo + 1);  // к той же полосе

    float snr_linear = signal_power / (noise_power + 1e-10f);
    q->snr_db = 10.0f * log10f(snr_linear);

    // Дисбаланс Mark/Space:
    float mark_power  = spectrum_band_power(spectrum,
                            mark_hz  - baud_rate*0.5f,
                            mark_hz  + baud_rate*0.5f, bin_hz, bins);
    float space_power = spectrum_band_power(spectrum,
                            space_hz - baud_rate*0.5f,
                            space_hz + baud_rate*0.5f, bin_hz, bins);
    q->amplitude_imbalance = 20.0f * log10f(
        sqrtf(mark_power / (space_power + 1e-10f)));

    // Оценка BER (из SNR по формуле для FSK):
    // BER ≈ 0.5 * erfc(sqrt(SNR/2))
    q->ber_estimate = 0.5f * erfcf(sqrtf(snr_linear * 0.5f));

    // Метка качества:
    if      (q->snr_db > 15) strcpy(q->quality_label, "EXCEL");
    else if (q->snr_db > 10) strcpy(q->quality_label, "GOOD");
    else if (q->snr_db >  6) strcpy(q->quality_label, "FAIR");
    else                     strcpy(q->quality_label, "POOR");
}
```

---

## 2.6 FFT — ПРОФЕССИОНАЛЬНЫЙ СПЕКТРАЛЬНЫЙ АНАЛИЗ

### Усреднение спектра (Welch метод)

```c
// Метод Уэлча: усредняем N перекрывающихся FFT — подавление дисперсии.
// Перекрытие 50% (стандарт) — вдвое больше FFT но гладкий спектр.
// Используется во всех профессиональных спектроанализаторах.

#define WELCH_FRAMES     8     // количество усредняемых кадров
#define WELCH_OVERLAP    0.5f  // перекрытие 50%

typedef struct {
    float   accum[FFT_SIZE/2];  // накопитель
    int     frame_count;
    float   hop_buffer[FFT_SIZE]; // буфер с перекрытием
    int     hop_pos;
} welch_t;

void welch_push_samples(welch_t *w, float *samples, int n,
                         float *window, float *fft_out) {
    // Скользящее окно с перекрытием 50%:
    int hop = (int)(FFT_SIZE * (1.0f - WELCH_OVERLAP));

    memcpy(w->hop_buffer, w->hop_buffer + hop,
           (FFT_SIZE - hop) * sizeof(float));
    memcpy(w->hop_buffer + FFT_SIZE - hop, samples,
           MIN(hop, n) * sizeof(float));

    // Применить окно и FFT:
    float windowed[FFT_SIZE];
    arm_mult_f32(w->hop_buffer, window, windowed, FFT_SIZE);

    float fft_complex[FFT_SIZE * 2];
    arm_rfft_fast_f32(&rfft_instance, windowed, fft_complex, 0);

    float magnitude[FFT_SIZE/2];
    arm_cmplx_mag_f32(fft_complex, magnitude, FFT_SIZE/2);

    // Накопление:
    arm_add_f32(w->accum, magnitude, w->accum, FFT_SIZE/2);
    w->frame_count++;

    if (w->frame_count >= WELCH_FRAMES) {
        // Нормировать и выдать:
        arm_scale_f32(w->accum, 1.0f/WELCH_FRAMES, fft_out, FFT_SIZE/2);
        memset(w->accum, 0, sizeof(w->accum));
        w->frame_count = 0;
    }
}
```

### Логарифмирование для водопада (dB шкала)

```c
// Профессиональный водопад всегда в дБ — линейный масштаб плохо
// показывает слабые сигналы рядом с сильными

void spectrum_to_db(float *linear_mag, float *db_out, int n) {
    // Векторное логарифмирование через CMSIS-DSP:
    // Нет прямой arm_log10 для массива — делаем через arm_vlog
    for (int i = 0; i < n; i++) {
        // Быстрый log2 через IEEE754 трюк (2 такта на M33):
        db_out[i] = fast_log2_ieee754(linear_mag[i]) * 3.321928f; // ×log10(2)×20
        // Ограничить диапазон:
        db_out[i] = CLAMP(db_out[i], -80.0f, 0.0f);
    }
}

// IEEE754 быстрый log2:
static inline float fast_log2_ieee754(float x) {
    union { float f; uint32_t i; } u = {x};
    float exp_part = (float)((int)(u.i >> 23) - 127);
    u.i = (u.i & 0x007FFFFF) | 0x3F800000;  // мантисса в [1, 2)
    // Аппроксимация log2(мантиссы):
    float m = u.f - 1.0f;
    return exp_part + m * (1.4142f - 0.4142f * m);
}
```

---

## 2.7 ДЕКОДЕР BAUDOT — РАСШИРЕННЫЙ

```c
// Полная таблица ITA2 включая:
// - WRU (Who are you) символ
// - BEL (звонок) символ
// - LTRS/FIGS переключение
// - Обработка ошибочных символов (отображение как '?')

// Детектор ошибок фрейма:
typedef struct {
    int     start_bit_errors;   // ошибки стартового бита
    int     stop_bit_errors;    // ошибки стопового бита
    int     total_chars;
    int     error_chars;
    float   ber_measured;       // измеренный BER
} frame_stats_t;

// Детектор инверсии (иногда сигнал приходит инвертированным):
typedef struct {
    int  ltrs_count;   // сколько раз встретился LTRS символ
    int  figs_count;   // сколько раз встретился FIGS символ
    int  valid_chars;  // валидные символы
    int  invalid_chars;// невалидные
    bool inverted;     // текущее предположение об инверсии
} inversion_detector_t;

// Если invalid_chars >> valid_chars → попробовать инверсию
void check_and_fix_inversion(inversion_detector_t *d,
                               rtty_config_t *config) {
    if (d->invalid_chars > d->valid_chars * 2 && d->valid_chars > 10) {
        config->inverted ^= true;
        d->valid_chars = d->invalid_chars = 0;
        ui_show_message("Инверсия обнаружена — исправлено", COLOR_YELLOW);
    }
}
```

---

## 2.8 ПОЛНЫЙ DSP КОНВЕЙЕР — ПОРЯДОК ОБРАБОТКИ

```
АЦП 38400 Гц (×4 oversample)
        │
        ▼
[1] DC блокировка (IIR Fc=20 Гц)
        │
        ▼
[2] Децимирующий FIR ×4 → 9600 Гц
        │
        ▼
[3] Цифровой AGC (атака 10мс, спад 500мс)
        │
        ├──────────────────────────────────────────────────┐
        ▼                                                  ▼
[4a] FFT 512pt (Уэлча)                            [4b] DSP демодуляция
     Окно Блэкман-Харрис                                   │
     dB шкала                                     ┌────────┤
     → Waterfall                                  │        │
     → SNR измерение                              │        │
     → AFC/TUNE                                   ▼        ▼
     → Автодетект скорости            [5a] Корр.детектор  [5b] Дифф.детектор
                                      Mark/Space           (резервный)
                                           │
                                           ▼
                                   [6] Комбинированное решение
                                       (взвешенное голосование)
                                           │
                                           ▼
                                   [7] Early-Late Gate
                                       (синхронизация битов)
                                           │
                                           ▼
                                   [8] Integrate & Dump
                                       (оптимальная выборка)
                                           │
                                           ▼
                                   [9] Baudot фреймер
                                       (детект старт/стоп)
                                           │
                                           ▼
                                   [10] Детектор инверсии
                                           │
                                           ▼
                                   [11] ITA2 → ASCII
                                           │
                                           ▼
                                   [12] → Кольцевой буфер
                                        → Ядро 1 → Дисплей
```

---

## 2.9 АВТОМАТИЧЕСКОЕ ОПРЕДЕЛЕНИЕ СКОРОСТИ

```c
// Профессиональный метод: анализ гистограммы длительностей импульсов
// (run-length encoding статистика)

typedef struct {
    uint32_t histogram[512]; // гистограмма длин (в сэмплах)
    int      last_bit;
    int      run_length;
    int      total_runs;
} baud_detector_t;

float detect_baud_rate(baud_detector_t *d, float fs) {
    // Найти пик гистограммы — это длина одного битового периода:
    int peak_pos = 0;
    uint32_t peak_val = 0;
    for (int i = 1; i < 512; i++) {
        if (d->histogram[i] > peak_val) {
            peak_val = d->histogram[i];
            peak_pos = i;
        }
    }

    if (peak_pos == 0 || peak_val < 20) return 0;  // нет данных

    float measured_baud = fs / peak_pos;

    // Найти ближайший стандартный:
    static const float STANDARD_BAUDS[] =
        {45.45f, 50.0f, 75.0f, 100.0f, 110.0f, 150.0f, 300.0f};
    float best = STANDARD_BAUDS[0];
    float best_err = fabsf(measured_baud - best);
    for (int i = 1; i < 7; i++) {
        float err = fabsf(measured_baud - STANDARD_BAUDS[i]);
        if (err < best_err) { best_err = err; best = STANDARD_BAUDS[i]; }
    }

    // Принять только если ошибка < 5%:
    return (best_err / best < 0.05f) ? best : 0;
}
```

---

# ЧАСТЬ III: ИТОГОВАЯ КОНФИГУРАЦИЯ СИСТЕМЫ

---

## 3.1 ПРИОРИТЕТЫ СИСТЕМЫ

```
ПРИОРИТЕТ 1 — КАЧЕСТВО ДЕКОДИРОВАНИЯ:
  • Oversampling ×4 + децимация
  • Три параллельных детектора + голосование
  • Early-Late Gate синхронизация
  • Integrate & Dump оптимальная выборка
  • AGC с профессиональными константами
  • Адаптивный выбор FIR окна по SNR

ПРИОРИТЕТ 2 — СТАБИЛЬНОСТЬ ПРИЁМА:
  • ФАПЧ детектор (устойчив к замираниям)
  • AFC с мягкой коррекцией
  • Детектор инверсии
  • Автоопределение скорости
  • Welch усреднение спектра

ПРИОРИТЕТ 3 — СКОРОСТЬ ЭКРАНА:
  • Waterfall обновляется только если есть свободное время Ядра 1
  • Текстовая зона — приоритет выше waterfall
  • SPI DMA — не блокирует DSP ни на такт
```

## 3.2 ЗАГРУЗКА ПРОЦЕССОРА (оценка при 300 МГц)

```
Ядро 0 (DSP):                        Время / блок 26.7мс
  Decimation FIR ×4 (32 tap)          ~1.2 мс
  AGC                                  ~0.1 мс
  Correlator Mark×Space                ~2.0 мс
  Diff detector                        ~0.5 мс
  PLL demod                            ~0.8 мс
  Early-Late Gate                      ~0.3 мс
  Integrate & Dump                     ~0.1 мс
  Baudot decoder                       ~0.1 мс
  FFT 512pt (Welch)                    ~1.5 мс
  Misc overhead                        ~0.5 мс
  ─────────────────────────────────────────────
  Итого:                               ~7.1 мс из 26.7 мс
  Запас:                               ~73% — ОТЛИЧНЫЙ ЗАПАС

Ядро 1 (UI):
  Waterfall 1 строка (320px DMA)       ~0.5 мс
  Инфопанель обновление (частичное)    ~0.2 мс
  Touch опрос (PIO)                    ~0.1 мс
  Текстовый символ                     ~0.1 мс
  ─────────────────────────────────────────────
  Итого на цикл UI (100 мс):           ~9 мс из 100 мс
  Запас:                               ~91%
```

---

*ТЗ v3.0 — Профессиональные DSP алгоритмы + полное использование RP2350*
*RP2350 @ 300 МГц | Cortex-M33 DSP | PIO | DMA chain | Hardware interp*
