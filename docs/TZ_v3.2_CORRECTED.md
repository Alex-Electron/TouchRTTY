# TECHNICAL SPECIFICATION (CORRECTED & REFINED)
# Autonomous Professional RTTY Digital Radio Terminal
## Version: 3.2 (Breadboard Validated, All Errors Fixed)
## Platform: Raspberry Pi Pico 2 (RP2350A) + ILI9488 480x320 Touch

---

## CHANGELOG v3.1 → v3.2 (ЧТО БЫЛО ИСПРАВЛЕНО)

```
КРИТИЧЕСКИЕ ОШИБКИ:
  [FIXED] §2.1   — Заголовок говорил ILI9341 320x240, железо — ILI9488 480x320
  [FIXED] §2.3   — Конденсатор указан на пине 35 (ADC_VREF), правильно — пин 35+33
                   Добавлена полная таблица 8 конденсаторов из Раздела 7
  [FIXED] §2.3   — Дублирование номера раздела (два §2.3)
  [FIXED] §2.3   — "8-bit byte-aligned DMA" для ILI9488 — это ненадёжный метод
                   (colour striping при частичных обновлениях). Исправлено на PIO.
  [FIXED] §2.3   — SPI clock 62.5MHz в заголовке таблицы — это предел ILI9488,
                   не рабочая частота. На макетке — 15-30 МГц.
  [FIXED] §3.1   — Размеры зон экрана: 16+128+16+128+32 = 320px, но экран 480x320.
                   Высоты зон не менялись, ширина неверно оставалась 480.
                   Исправлена раскладка под реальный 480x320.
  [FIXED] §5     — Phase 1 упоминает ILI9341 24MHz SPI — это не тот дисплей.

НЕТОЧНОСТИ:
  [FIXED] §1     — Упоминается FT8 и CW в названии, но в DSP pipeline нет ни слова
                   о них. Убраны из названия до реализации.
  [FIXED] §2.2   — VREG 1.30V при 300MHz — уточнено предупреждение о стабильности
  [FIXED] §2.3   — LED/BL подключён к 3.3V(OUT) напрямую — ток может быть высоким.
                   Добавлен обязательный резистор 33-47Ом.
  [FIXED] §4     — DSP Pipeline не упоминает oversampling ×4 и децимацию,
                   хотя это закреплено в предыдущих разделах ТЗ.
  [FIXED] §4     — 10kHz sample rate противоречит ранее согласованному 9600 Гц.
                   Унифицировано: физически 38400 Гц (×4), после децимации 9600 Гц.
```

---

## 1. PROJECT VISION & CORE MANDATES

The device is a **standalone digital mode RTTY decoder** matching the stability
and performance of professional SDR software (`fldigi`, `MMTTY`, `Phosphor RTTY`).
It processes real-time audio from a radio receiver's line output, decodes RTTY,
and provides a rich touch-based UI with a live waterfall and parallel decoding.

**Strict Mandates:**
- **"fldigi" Stability:** I/Q (Quadrature) envelope detection, matched FIR filters
  with adaptive windowing, and a Software PLL (Early-Late Gate) for precision
  bit-clock recovery. Three parallel demodulators with weighted voting.
- **Hardware Exploitation:** Aggressively utilize RP2350 features:
  Cortex-M33 DSP/SIMD instructions, FPU, dual-core, PIO state machines,
  DMA chaining, hardware interpolators (SIO INTERP).
- **Architecture:** Core 0 = all DSP (ADC DMA, FIR, PLL, Decoding).
  Core 1 = UI (PIO SPI LCD, Waterfall, Touch).
- **Quality First:** Decode quality > decode stability > UI frame rate.

---

## 2. HARDWARE & WIRING (BREADBOARD OPTIMIZED)

### 2.1 Component Models (Explicit Specifications)

| Component | Model | Key Parameters |
|:---|:---|:---|
| MCU | Raspberry Pi Pico 2 | RP2350A, Dual Cortex-M33, 300 MHz OC |
| Display | 3.5" IPS TFT LCD | **ILI9488**, 480×320, 4-wire SPI |
| Touch | Resistive | **XPT2046**, shared SPI1 bus |
| SD Card | Integrated slot | SPI1 shared bus, SCS=GPIO13 |
| LDO (onboard) | 662K | 3.3V регулятор на модуле дисплея |

> ⚠️ **ВАЖНО:** ILI9488 в 4-wire SPI **не поддерживает RGB565**.
> Только RGB666 (18-bit, 3 байта на пиксель). Это аппаратное ограничение.

---

### 2.2 Microcontroller Configuration

- **MCU:** RP2350A
- **Overclock:** 300 МГц через `set_sys_clock_khz(300000, true)`
- **VREG:** 1.30V (`vreg_set_voltage(VREG_VOLTAGE_1_30)`) — вызвать **до** set_sys_clock_khz

```c
// Правильный порядок инициализации:
vreg_set_voltage(VREG_VOLTAGE_1_30);
sleep_ms(10);                          // дать VREG стабилизироваться
set_sys_clock_khz(300000, true);
sleep_ms(2);
stdio_init_all();                      // переинициализировать UART после смены частоты
spi_set_baudrate(SPI_PORT, SPI_FREQ); // переустановить SPI baud после смены clk
```

> ⚠️ **Стабильность:** Не все экземпляры RP2350 берут 300 МГц стабильно.
> При зависаниях — снизить до 280 или 250 МГц.

---

### 2.3 Analog Input (ADC)

**Signal Path:**
```
Аудио вход (линейный, ~1 Vpp)
    │
[10kΩ потенциометр] — регулировка уровня + смещение к 1.65V
    │
[RC LPF: R=1кОм, C=43нФ] — Fc ≈ 3700 Гц, срез ВЧ мусора
    │
[GPIO 26 = ADC0, физический пин 31]
```

**ADC Sample Rate:**
- Физическая частота АЦП: **38400 Гц** (oversampling ×4)
- После децимирующего FIR ×4: **9600 Гц** рабочая частота DSP
- DMA ring buffer, ping-pong буферы по 256 сэмплов

---

### 2.4 Конденсаторы — ОБЯЗАТЕЛЬНЫЙ СПИСОК (8 штук)

> ⚠️ Конденсатор работает ТОЛЬКО вплотную к точке установки.
> Макс. расстояние от ножки компонента: **1–2 см**.

```
№    Номинал    Тип           Место установки                   Пин Pico
─────────────────────────────────────────────────────────────────────────────
C1   100 нФ    Керамика X7R  ADC_VREF → AGND  ← ВПЛОТНУЮ       Пин 35 → 33
C2   10 мкФ   Тантал/Эл-т   ADC_VREF → AGND  рядом             Пин 35 → 33
C3   100 нФ    Керамика X7R  3.3V_OUT → GND   у Pico            Пин 36 → GND
C4   100 нФ    Керамика X7R  VCC → GND ILI9488  ← ВПЛОТНУЮ     У дисплея
C5   10 мкФ   Электролит    VCC → GND ILI9488  рядом            У дисплея
C6   1–4.7 нФ Керамика NP0  GPIO26 → GND      ← ВПЛОТНУЮ       Пин 31 → GND
C7   100 нФ    Керамика X7R  После R_iso 10Ом → GND             В аналог. цепи
C8   100 нФ    Керамика X7R  3.3V шина → GND  у LDO/USB        У источника
─────────────────────────────────────────────────────────────────────────────
```

> Пин 35 = ADC_VREF, Пин 33 = AGND — это **аналоговое** питание АЦП.
> C6: тип NP0/C0G обязателен (стабильная ёмкость, нет пьезоэффекта).

**Порядок монтажа:** Сначала все конденсаторы → потом подача питания.

---

### 2.5 Display, Touch & SD Card Wiring — Dual SPI Bus

#### Архитектура шин:
```
SPI0 (высокая скорость) → ТОЛЬКО ILI9488 дисплей
SPI1 (низкая скорость)  → XPT2046 touch + SD Card (shared, разные CS)
```

#### Таблица соединений:

| Сигнал | GPIO | Физический пин | Описание |
|:---|:---|:---|:---|
| **SPI0 — Дисплей ILI9488** | | | |
| SCK (LCD) | GPIO 18 | 24 | SPI0 SCK |
| MOSI (LCD) | GPIO 19 | 25 | SPI0 TX |
| CS (LCD) | GPIO 17 | 22 | Chip Select дисплея |
| DC | GPIO 20 | 26 | Data/Command |
| RST | GPIO 21 | 27 | Reset дисплея |
| **SPI1 — Touch + SD (shared)** | | | |
| T_CLK / SD_CLK | GPIO 10 | 14 | SPI1 SCK |
| T_DIN / SD_DI | GPIO 11 | 15 | SPI1 TX |
| T_DO / SD_DO | GPIO 12 | 16 | SPI1 RX |
| T_CS | GPIO 15 | 20 | CS для XPT2046 |
| SD_CS | GPIO 13 | 17 | CS для SD Card |
| T_IRQ | GPIO 14 | 19 | Touch Interrupt (pull-up!) |
| **Питание** | | | |
| VCC/VDD | VBUS (5V) | 40 | **Строго 5V** (модуль имеет свой LDO) |
| LED/BL | GPIO 22 | 29 | ШИМ подсветки (через R=33-47 Ом) |
| GND | GND | 38 / любой GND | Общая земля |

> ⚠️ **T_IRQ:** обязательно `gpio_pull_up(14)` в коде.
> ⚠️ **LED/BL:** подключать через резистор 33–47 Ом, не напрямую.
>    На макетке: `gpio_put(PIN_BL, 1)` статически (без PWM).
>    PWM подсветки вводить только после отладки АЦП — PWM на кГц
>    попадает прямо в аудиополосу.
> ⚠️ **MISO дисплея:** ILI9488 не тристейтит SDO при CS=HIGH.
>    Не подключать MISO дисплея к SPI0 MISO если на шине есть другие устройства.

---

### 2.6 ILI9488 — Конфигурация цвета (КРИТИЧНО)

#### Правильный метод передачи пикселей: **PIO + DMA_SIZE_32**

```
МЕТОД                    РЕЗУЛЬТАТ            ВЕРДИКТ
────────────────────────────────────────────────────────
HW SPI + DMA_SIZE_8      Colour striping      ❌ Ненадёжно
PIO + autopull=32        Washed out colors    ❌ Неправильно
PIO + autopull=24        Правильные цвета     ✅ Использовать
```

**Инициализация ILI9488:**
```c
// Команда COLMOD (0x3A) — формат пикселей:
ili9488_write_cmd(0x3A);
ili9488_write_data(0x66); // 0x66 = 18-bit RGB666 (НЕ 0x55 = RGB565!)

// MADCTL (0x36) — ориентация landscape, BGR:
ili9488_write_cmd(0x36);
ili9488_write_data(0xE0); // landscape 480x320, BGR=0

// Display Inversion: OFF (0x20) для правильных цветов:
ili9488_write_cmd(0x20);  // INVOFF
```

**Упаковка пикселя RGB565 → uint32_t для PIO:**
```c
static inline uint32_t rgb565_to_pio_word(uint16_t rgb565) {
    uint8_t r5 = (rgb565 >> 11) & 0x1F;
    uint8_t g6 = (rgb565 >> 5)  & 0x3F;  // уже 6 бит
    uint8_t b5 =  rgb565        & 0x1F;
    // Расширение 5→6 бит (дублирование MSB в LSB):
    uint8_t r6 = (r5 << 1) | (r5 >> 4);
    uint8_t b6 = (b5 << 1) | (b5 >> 4);
    // R в биты [31..26], G в [25..20], B в [19..14]:
    return ((uint32_t)r6 << 26)
         | ((uint32_t)g6 << 20)
         | ((uint32_t)b6 << 14);
    // Биты [13..0] = мусор, PIO остановится после 24 бит (autopull=24)
}
```

**PIO программа (spi_tx_24bit.pio):**
```asm
.program spi_tx_24bit
.side_set 1              ; 1 бит = SCK

; Конфиг C: sm_config_set_out_shift(&c, false, true, 24)
;           gpio_set_outover(pin_sck, GPIO_OVERRIDE_LOW)  ← КРИТИЧНО
.wrap_target
    out pins, 1   side 0  [1]   ; MOSI = бит, SCK=LOW, setup time
    nop           side 1  [1]   ; SCK=HIGH → ILI9488 семплирует
.wrap
; autopull=24: PIO выдаёт ровно 24 бита, биты 13..0 игнорируются
```

**SPI clock для PIO при 300 МГц:**
```
clk_div=2.5 → SCK = 300/(2.5×4) = 30 МГц  (рекомендуется для макетки)
clk_div=5.0 → SCK = 300/(5.0×4) = 15 МГц  (безопасно, длинные провода)
clk_div=1.5 → SCK = 300/(1.5×4) = 50 МГц  (для PCB)
```

---

### 2.7 Touch Controller XPT2046

**SPI параметры:**
```c
spi_set_baudrate(spi1, 2500000); // МАКС 2.5 МГц для XPT2046
```

**T_IRQ обработка:**
```c
gpio_pull_up(PIN_TOUCH_IRQ);  // ОБЯЗАТЕЛЬНО

// Определение касания: T_IRQ уходит LOW при касании
bool touch_is_pressed(void) {
    return !gpio_get(PIN_TOUCH_IRQ);
}
```

**Чтение координат (медианное усреднение 5 отсчётов):**
```c
typedef struct { int16_t x, y; } touch_point_t;

touch_point_t touch_read_median(void) {
    int16_t xs[5], ys[5];
    for (int i = 0; i < 5; i++) {
        // SPI транзакция XPT2046:
        gpio_put(PIN_TOUCH_CS, 0);
        spi_write_read_blocking(spi1, cmd_x, buf_x, 3);
        spi_write_read_blocking(spi1, cmd_y, buf_y, 3);
        gpio_put(PIN_TOUCH_CS, 1);
        xs[i] = (buf_x[1] << 5) | (buf_x[2] >> 3);
        ys[i] = (buf_y[1] << 5) | (buf_y[2] >> 3);
    }
    // Сортировка + медиана:
    sort5(xs); sort5(ys);
    return (touch_point_t){ xs[2], ys[2] };
}
```

---

### 2.8 Touch Calibration — 3-Point Affine Transform

Резистивные сенсоры XPT2046 имеют нелинейность, поворот и перекос
относительно LCD. Стандартный алгоритм Carlos E. Vidales (Texas Instruments).

**Математика:**
```
Xd = (A·Xs + B·Ys + C) / Divider
Yd = (D·Xs + E·Ys + F) / Divider
```

**Реализация на C:**
```c
typedef struct {
    int32_t A, B, C, D, E, F, Divider;
} touch_cal_t;

// touch_cal.h — вычисление матрицы по 3 точкам:
bool touch_calibrate(
    // Экранные координаты 3 точек (известны заранее):
    int16_t sx1, int16_t sy1,   // точка 1: ~(48,  48)
    int16_t sx2, int16_t sy2,   // точка 2: ~(432, 48)
    int16_t sx3, int16_t sy3,   // точка 3: ~(240, 272)
    // Сырые ADC координаты от XPT2046:
    int16_t tx1, int16_t ty1,
    int16_t tx2, int16_t ty2,
    int16_t tx3, int16_t ty3,
    touch_cal_t *cal)
{
    cal->Divider = (tx1 - tx3) * (ty2 - ty3)
                 - (tx2 - tx3) * (ty1 - ty3);
    if (cal->Divider == 0) return false;

    cal->A = (sx1 - sx3) * (ty2 - ty3)
           - (sx2 - sx3) * (ty1 - ty3);
    cal->B = (tx1 - tx3) * (sx2 - sx3)
           - (sx1 - sx3) * (tx2 - tx3);
    cal->C = (int32_t)sx1 * ((int32_t)tx3 * ty2 - (int32_t)tx2 * ty3)
           - (int32_t)sx2 * ((int32_t)tx3 * ty1 - (int32_t)tx1 * ty3)
           + (int32_t)sx3 * ((int32_t)tx2 * ty1 - (int32_t)tx1 * ty2);

    cal->D = (sy1 - sy3) * (ty2 - ty3)
           - (sy2 - sy3) * (ty1 - ty3);
    cal->E = (tx1 - tx3) * (sy2 - sy3)
           - (sy1 - sy3) * (tx2 - tx3);
    cal->F = (int32_t)sy1 * ((int32_t)tx3 * ty2 - (int32_t)tx2 * ty3)
           - (int32_t)sy2 * ((int32_t)tx3 * ty1 - (int32_t)tx1 * ty3)
           + (int32_t)sy3 * ((int32_t)tx2 * ty1 - (int32_t)tx1 * ty2);
    return true;
}

// Применение калибровки:
touch_point_t touch_apply_cal(touch_cal_t *cal, int16_t tx, int16_t ty) {
    touch_point_t p;
    p.x = (cal->A * tx + cal->B * ty + cal->C) / cal->Divider;
    p.y = (cal->D * tx + cal->E * ty + cal->F) / cal->Divider;
    return p;
}
```

**Сохранение калибровки в Flash:**
```c
// Сохраняем touch_cal_t в последний сектор Flash (4096 байт):
// Используем pico SDK flash_range_erase + flash_range_program
#define CAL_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

void cal_save(touch_cal_t *cal) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(CAL_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CAL_FLASH_OFFSET, (uint8_t*)cal, sizeof(touch_cal_t));
    restore_interrupts(ints);
}

void cal_load(touch_cal_t *cal) {
    const uint8_t *ptr = (uint8_t*)(XIP_BASE + CAL_FLASH_OFFSET);
    memcpy(cal, ptr, sizeof(touch_cal_t));
    // Проверка валидности (Divider != 0 и != 0xFFFFFFFF):
    if (cal->Divider == 0 || cal->Divider == -1)
        memset(cal, 0, sizeof(touch_cal_t)); // требует перекалибровки
}
```

---

## 3. UI ARCHITECTURE — WATERFALL & TOUCH (480×320)

### 3.1 Screen Layout (480×320 Landscape)

```
X=0                                                         X=479
Y=0   ┌───────────────────────────────────────────────────────┐
      │  СТАТУС: 45.45Bd  170Hz  M:2295  S:2125  SNR:14dB  AFC│ 16px
Y=16  ├───────────────────────────────────────────────────────┤
      │                                                       │
      │           W A T E R F A L L  (480px ширина)          │ 128px
      │    S░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░M              │
      │    │              ┆                    │              │
      │  голуб         пунктир               жёлт            │
Y=144 ├───────────────────────────────────────────────────────┤
      │ S:2125  ◆:2210  M:2295 │SH:170Hz│45.45Bd│SNR:14dB│AFC│ 16px
Y=160 ├───────────────────────────────────────────────────────┤
      │                                                       │
      │  DE UA9XYZ QTH NOVOSIBIRSK RST 599 599 K             │
      │  CQ CQ CQ DE UA9XYZ K                                │ 128px
      │  █                                                    │ (~9 строк)
      │                                                       │
Y=288 ├───────────────────────────────────────────────────────┤
      │ [TUNE] [AFC●] [CLR] [45.45▼] [170Hz▼] [LOCK] [  ☰ ] │ 32px
Y=320 └───────────────────────────────────────────────────────┘

Сумма: 16 + 128 + 16 + 128 + 32 = 320px ✓
```

| Зона | Y от | Y до | px | Назначение |
|:---|:---|:---|:---|:---|
| Status Bar | 0 | 16 | 16 | Параметры, SNR, AFC индикатор |
| Waterfall | 16 | 144 | 128 | FFT водопад 300–3300 Гц |
| Info Panel | 144 | 160 | 16 | S/CTR/M частоты, shift, baud, SNR |
| Text Area | 160 | 288 | 128 | Декодированный текст |
| Toolbar | 288 | 320 | 32 | Кнопки управления |

---

### 3.2 Waterfall — FFT параметры

```c
#define WF_FFT_SIZE     512         // точек FFT
#define WF_SAMPLE_RATE  9600        // Гц (после децимации)
// Разрешение: 9600/512 = 18.75 Гц/бин
// Диапазон отображения: 300..3300 Гц → ширина 480px
// Масштаб: 480px / 3000Гц = 0.16 px/Гц

#define WF_FREQ_MIN     300.0f
#define WF_FREQ_MAX     3300.0f
#define WF_PX_WIDTH     480
#define WF_PX_HEIGHT    128

#define FREQ_TO_PX(f) \
    ((int)(((f) - WF_FREQ_MIN) * WF_PX_WIDTH / (WF_FREQ_MAX - WF_FREQ_MIN)))
#define PX_TO_FREQ(x) \
    (WF_FREQ_MIN + (x) * (WF_FREQ_MAX - WF_FREQ_MIN) / WF_PX_WIDTH)
```

Waterfall RAM: `128 строк × 480 пикс × 2 байта = 122.9 КБ`
Размещать в SRAM3 (отдельный банк от DSP буферов).

---

### 3.3 Три маркера (Mark / Center / Space)

```
Space: Голубая линия  (Cyan,   0x07FF)
Center: Белая пунктирная (White, 0xFFFF)
Mark:  Жёлтая линия  (Yellow, 0xFFE0)
Зона между S и M: полупрозрачный зелёный оверлей
```

**Touch-зоны:**

| Касание | Действие |
|:---|:---|
| ±12px от линии Space | Drag только Space |
| ±12px от линии Mark | Drag только Mark |
| Между маркерами | Drag весь блок |
| Вне маркеров | Умный поиск пары пиков |
| Long press >500мс | Режим раздельной подстройки |
| Double tap | Сброс маркеров |

---

### 3.4 TUNE Algorithm (Parabolic Interpolation)

```c
// 1. Накопить 8 FFT кадров (Welch усреднение, ~500мс)
// 2. Найти пик в ±150 Гц от маркера
// 3. Параболическая интерполяция по 3 точкам вершины:

float parabolic_peak_offset(float y0, float y1, float y2) {
    // y1 = максимум, y0 = левый сосед, y2 = правый сосед
    float denom = y0 - 2.0f * y1 + y2;
    if (fabsf(denom) < 1e-6f) return 0.0f;
    return 0.5f * (y0 - y2) / denom;
    // Возвращает смещение в бинах: -0.5 .. +0.5
    // Точная частота: (peak_bin + offset) * bin_hz
}

// 4. Проверить SNR обоих пиков > 6 дБ
// 5. Проверить shift между пиками: 80 < shift < 1200 Гц
// 6. Анимированно переместить маркеры (ease-out, 300мс)
// 7. Применить к DSP: rtty_set_frequencies(mark_hz, space_hz)
```

---

### 3.5 AFC (Automatic Frequency Control)

```c
// AFC активна только при активном приёме символов (> 10 симв/сек)
// Максимальная коррекция: 2 Гц за шаг (каждые 500мс)
// Поиск пика: в ±30 Гц от текущих маркеров
// Алгоритм: скользящее среднее позиции пика (сглаживание × 4 кадра)
```

---

## 4. DSP PIPELINE (CORE 0)

```
АЦП 38400 Гц (oversampling ×4, DMA ring buffer)
        │
[1] DC блокировка — IIR Fc≈20 Гц (убирает постоянную составляющую)
        │
[2] Децимирующий FIR ×4 → 9600 Гц
    32 отвода, окно Кайзера β=6, Fc=4800 Гц
    (CMSIS-DSP: arm_fir_decimate_f32)
        │
[3] Цифровой AGC
    Атака: 10 мс, Спад: 500 мс
    Цель: RMS уровень 0.3 (−10 дБFS)
        │
        ├─────────────────────────────────────────────────┐
        ▼                                                 ▼
[4a] FFT 512pt (Welch, перекрытие 50%)          [4b] Три параллельных детектора:
     Окно Блэкман-Харрис                          ├── Коррелятор I/Q Mark+Space
     dB шкала (fast_log2_ieee754)                 ├── Дифференциальный детектор
     → Waterfall (Core 1 via FIFO)                └── PLL FSK детектор
     → SNR измеритель                                        │
     → AFC / TUNE                                [5] Взвешенное голосование (3→1)
     → Автодетект baud rate                                  │
                                                 [6] Early-Late Gate
                                                     (синхронизация битов)
                                                             │
                                                 [7] Integrate & Dump
                                                     (оптимальная выборка символа)
                                                             │
                                                 [8] Baudot фреймер
                                                     старт бит + 5 бит + стоп
                                                             │
                                                 [9] Детектор инверсии (авто)
                                                             │
                                                 [10] ITA2 → ASCII символ
                                                             │
                                                 [11] → Ring buffer → Core 1
```

### Adaptive FIR Window Selection (по SNR):

```c
window_type_t select_window(float snr_db, float adj_signal_db) {
    if (adj_signal_db > 30.0f) return WINDOW_BLACKMAN_HARRIS; // помеха рядом
    if (snr_db < 6.0f)         return WINDOW_KAISER_8;        // слабый сигнал
    if (snr_db < 15.0f)        return WINDOW_BLACKMAN;
    return WINDOW_HAMMING;                                     // норма
}
```

---

## 5. RP2350 HARDWARE UTILIZATION

| Ресурс | Использование |
|:---|:---|
| **Core 0** | DSP: ADC DMA, децимация, AGC, 3× детектор, FFT, Baudot |
| **Core 1** | UI: PIO SPI, Waterfall DMA, Touch poll, Text render |
| **PIO SM0** | spi_tx_24bit — передача пикселей ILI9488 (24-bit MSB) |
| **PIO SM1** | Точное тактирование АЦП (джиттер < 3.3 нс при 300 МГц) |
| **PIO SM2** | XPT2046 опрос в фоне (результат в FIFO для Core 1) |
| **DMA ch0/1** | АЦП ping-pong, chain, IRQ при завершении блока |
| **DMA ch2** | Waterfall строка → SPI PIO TX FIFO |
| **DMA ch3** | XPT2046 TX/RX команды |
| **INTERP0** | Sin/cos lookup table (1 такт доступа) |
| **INTERP1** | Waterfall colormap (float mag → RGB565, 1 такт) |
| **FPU** | arm_fir_decimate_f32, arm_rfft_fast_f32, все float DSP |
| **Flash** | Последний сектор (4 КБ) — хранение touch_cal_t |

---

## 6. IMPLEMENTATION PLAN (REVISED)

### Phase 1: Display & Touch Bring-up (ТЕКУЩАЯ)
```
[ ] Инициализация ILI9488: COLMOD=0x66 (RGB666), MADCTL=0xE0
[ ] PIO spi_tx_24bit, autopull=24, SCK idle=LOW
[ ] Заливка цветом — проверить правильность цветов (красный/зелёный/синий)
[ ] Базовый текст на экране (шрифт 8×12)
[ ] T_IRQ pull-up, чтение сырых XPT2046 координат
[ ] Процедура 3-точечной калибровки touch с сохранением в Flash
[ ] Тест: тап на экран → крестик в точке касания
```

### Phase 2: UI Foundation
```
[ ] Раскладка 480×320: Status / Waterfall / InfoPanel / Text / Toolbar
[ ] Три маркера (три линии) + зелёный оверлей
[ ] Частотная шкала 300..3300 Гц
[ ] Touch-зоны маркеров (drag, smart tap)
[ ] Кнопки тулбара (TUNE, AFC, CLR, dropdown)
```

### Phase 3: DSP Engine
```
[ ] ADC DMA ring buffer (38400 Гц, ping-pong)
[ ] Децимирующий FIR ×4 (CMSIS-DSP)
[ ] AGC
[ ] FFT 512pt → Waterfall
[ ] Correlator I/Q детектор
[ ] Early-Late Gate + Integrate&Dump
[ ] Baudot декодер → текст на экране
[ ] TUNE (Parabolic Interpolation) + AFC
```

### Phase 4: SD Card Logging
```
[ ] SPI1 SD Card init (SDHC/SDXC, FAT32/exFAT)
[ ] Запись декодированного текста с timestamp
[ ] UI: индикатор SD карты в статус-баре
```

### Phase 5: Advanced DSP
```
[ ] Три параллельных детектора + голосование
[ ] Автодетект baud rate (гистограмма)
[ ] Детектор инверсии
[ ] SNR измеритель + BER оценка
[ ] CW декодер (параллельно в полосе 300-3300 Гц)
```

---

## 7. KNOWN ISSUES & BREADBOARD TIPS

```
ПРОБЛЕМА                     ПРИЧИНА              РЕШЕНИЕ
────────────────────────────────────────────────────────────────────────
Washed out цвета             autopull=32 в PIO    → autopull=24
Colour striping              HW SPI DMA_SIZE_8    → PIO метод
Зависание при 300 МГц        Нестабильный экземпляр → 280 МГц
Touch не отвечает            Нет pull-up на T_IRQ → gpio_pull_up(14)
Шум АЦП при работе дисплея  Нет C1+C2 у пин 35   → Добавить вплотную
PWM помеха в АЦП             BL через PWM на кГц  → gpio_put(BL, 1)
Нет инициализации UART       UART init до OC      → stdio_init_all() ПОСЛЕ
Неправильные цвета ILI9488   COLMOD=0x55 (RGB565) → COLMOD=0x66 (RGB666)
```

---

*ТЗ v3.2 — Исправлено и выверено. RP2350A @ 300 МГц, ILI9488 480×320, XPT2046*
