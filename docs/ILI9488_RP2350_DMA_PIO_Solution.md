# ILI9488 + RP2350A: DMA/PIO Проблемы и Оптимальное Решение
## Глубокий анализ colour striping и washed out colors

---

## Контекст железа

- **MCU:** Raspberry Pi Pico 2 (RP2350A), разгон 300 МГц
- **Дисплей:** 3.5" IPS TFT ILI9488, 4-wire SPI, 480×320
- **Touch:** XPT2046, shared SPI bus
- **Пины:** SPI0 — SCK=GPIO18, MOSI=GPIO19, CS=GPIO17, DC=GPIO20
- **Ключевое ограничение:** ILI9488 в 4-wire SPI **строго требует RGB666** (18 бит = 3 байта на пиксель)

---

## Корень обеих проблем — одна фундаментальная причина

ILI9488 в 4-wire SPI **физически не поддерживает RGB565**.
Он принимает только **RGB666 (18 бит = 3 байта на пиксель)**.
Это не баг прошивки — это аппаратное ограничение контроллера.
Отсюда все беды обоих подходов.

---

## Проблема 1: Hardware SPI + DMA_SIZE_8 — Colour Striping

### Что происходит

Hardware SPI на RP2350 имеет **8-битный FIFO** (`SSPCR0 DSS=7` = 8-bit mode).
При DMA_SIZE_8 каждый байт помещается в FIFO отдельно.

RP2040/RP2350 имеет известный нюанс: DMA chaining срабатывает когда
последняя запись **выдана**, но не когда она **завершена**. Это значит:
при частичных обновлениях (не весь экран 480px), когда DMA заканчивает
передачу и CS поднимается/опускается — FIFO может содержать незафлашенный
остаток, и следующая транзакция начинается со смещением на 1–2 байта.

```
Нормальная последовательность:   [R][G][B][R][G][B][R][G][B]...
После desync на 1 байт:          [G][B][R][G][B][R][G][B][R]...
                                   ↑ Green отображается как Red и т.д.
```

### Почему нельзя починить Approach 1

DMA_SIZE_8 → hardware SPI → 24-bit поток для ILI9488 на RP2350
**принципиально ненадёжен** при частичных транзакциях.
Нет способа гарантировать выравнивание байт без перехода на другой метод.

---

## Проблема 2: PIO 24-bit — Washed Out / Pastel Colors

### Причина — bit endianness + неправильный autopull threshold

Это классическая ошибка в трёх возможных местах:

#### Место 1: Неправильная фаза SCK

```asm
; ПЛОХО — SCK начинается с HIGH (idle=1 = Mode 2/3):
; ILI9488 работает в Mode 0 (CPOL=0, CPHA=0)
; Если SCK idle = HIGH → дисплей семплирует не те биты

; ПРАВИЛЬНО — SCK idle = LOW:
; gpio_set_outover(pin_sck, GPIO_OVERRIDE_LOW); перед стартом SM
```

#### Место 2: Неправильный autopull threshold — САМАЯ ЧАСТАЯ ПРИЧИНА

```
Если autopull threshold = 32 (дефолт), а слово = (r<<24|g<<16|b<<8|0x00):
PIO выдаёт ВСЕ 32 бита включая мусорный младший байт → ILI9488 получает
лишний байт и слетает с синхронизации → цвета "плывут".

Если autopull threshold стоит не на 24 → MSB каждого канала теряется
→ яркость падает вдвое → washed out эффект.

РЕШЕНИЕ: sm_config_set_out_shift(&c, false, true, 24);
                                        ↑MSB  ↑auto ↑РОВНО 24 БИТА
```

#### Место 3: Неправильная упаковка пикселя

```c
// ПЛОХО — биты не туда:
uint32_t word = (r << 16) | (g << 8) | b;
// PIO с MSB first выдаёт биты 31..8, то есть [0x00][R][G] — Red потерян!

// ПРАВИЛЬНО — R в старших битах:
uint32_t word = ((uint32_t)r6 << 26)  // биты 31..26 = R (6 бит)
              | ((uint32_t)g6 << 20)  // биты 25..20 = G (6 бит)
              | ((uint32_t)b6 << 14); // биты 19..14 = B (6 бит)
// Биты 13..0 = мусор, PIO остановится после 24 бит (autopull=24)
```

---

## Рабочее Решение: PIO 24-bit (Pico SDK)

### PIO программа — точная, побитовая, для ILI9488

```asm
; spi_24bit.pio
; Отправляет ровно 24 бита из 32-битного слова (биты 31..8, MSB first)
; SPI Mode 0: SCK idle = LOW (CPOL=0), семплирование на rising edge (CPHA=0)
; side-set: 1 бит = SCK

.program spi_tx_24bit
.side_set 1              ; 1 бит side-set = SCK

; Конфигурация C (обязательно):
; sm_config_set_out_shift(&c, false, true, 24); // MSB first, autopull=24
; sm_config_set_out_pins(&c, PIN_MOSI, 1);
; sm_config_set_sideset_pins(&c, PIN_SCK);
; gpio_set_outover(pin_sck, GPIO_OVERRIDE_LOW); // SCK idle = LOW

.wrap_target
    out pins, 1   side 0  [1]  ; бит на MOSI, SCK=LOW, задержка setup time
    nop           side 1  [1]  ; SCK=HIGH (rising edge) — ILI9488 семплирует
.wrap

; autopull threshold=24: после 24 бит OSR автоматически обновляется
; Биты 13..0 слова НЕ выдаются — autopull сбрасывает OSR
; Частота SCK = sys_clock / clk_div / 4 (2 инструкции × 2 такта delay)
```

### Инициализация PIO в C

```c
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "spi_24bit.pio.h"  // сгенерировано из .pio файла

void spi24_init(PIO pio, uint sm, uint offset,
                uint pin_mosi, uint pin_sck, float clk_div) {

    pio_sm_config c = spi_tx_24bit_program_get_default_config(offset);

    // MOSI: выход
    pio_gpio_init(pio, pin_mosi);
    sm_config_set_out_pins(&c, pin_mosi, 1);
    pio_sm_set_consecutive_pindirs(pio, sm, pin_mosi, 1, true);

    // SCK: side-set, ОБЯЗАТЕЛЬНО начинается с LOW
    pio_gpio_init(pio, pin_sck);
    sm_config_set_sideset_pins(&c, pin_sck);
    pio_sm_set_consecutive_pindirs(pio, sm, pin_sck, 1, true);
    gpio_set_outover(pin_sck, GPIO_OVERRIDE_LOW);  // ← КРИТИЧНО

    // OSR: MSB first, autopull РОВНО при 24 битах
    sm_config_set_out_shift(&c, false, true, 24);  // ← КРИТИЧНО

    // Объединяем FIFO только TX (8-word глубина):
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

    // Делитель тактовой:
    // При 300 МГц и clk_div=2.5 → SCK = 300/(2.5×4) = 30 МГц
    sm_config_set_clkdiv(&c, clk_div);

    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}
```

### Правильная упаковка пикселя RGB565 → RGB666 → uint32_t

```c
// Конвертация для буфера пикселей:
static inline uint32_t rgb565_to_pio_word(uint16_t rgb565) {
    // Извлекаем каналы из RGB565:
    uint8_t r5 = (rgb565 >> 11) & 0x1F;  // 5 бит
    uint8_t g6 = (rgb565 >> 5)  & 0x3F;  // 6 бит (уже 6!)
    uint8_t b5 = rgb565 & 0x1F;           // 5 бит

    // RGB565 → RGB666: расширяем 5-бит каналы до 6 бит
    // Метод: дублируем MSB в LSB (правильная линейная интерполяция)
    uint8_t r6 = (r5 << 1) | (r5 >> 4);  // 00000→000000, 11111→111111
    uint8_t b6 = (b5 << 1) | (b5 >> 4);
    // g6 уже 6 бит — не трогаем

    // Пакуем: R в биты [31..26], G в [25..20], B в [19..14]
    // Биты [13..0] = мусор (PIO остановится после 24 бит)
    return ((uint32_t)r6 << 26)
         | ((uint32_t)g6 << 20)
         | ((uint32_t)b6 << 14);
}

// Подготовка буфера для DMA:
void prepare_pixel_buffer(uint16_t *src_rgb565,
                           uint32_t *dst_pio,
                           int pixel_count) {
    for (int i = 0; i < pixel_count; i++) {
        dst_pio[i] = rgb565_to_pio_word(src_rgb565[i]);
    }
    // Для waterfall: можно конвертировать прямо при вычислении цвета
    // → избегаем двойного прохода по памяти
}
```

### DMA → PIO TX FIFO

```c
static int dma_chan_display = -1;

void display_dma_init(PIO pio, uint sm) {
    dma_chan_display = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(dma_chan_display);

    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32); // 32-bit слова
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    // DREQ от PIO TX FIFO — DMA ждёт пока FIFO не освободится:
    channel_config_set_dreq(&dc, pio_get_dreq(pio, sm, true));

    dma_channel_set_config(dma_chan_display, &dc, false);
}

void display_send_pixels(PIO pio, uint sm,
                          uint32_t *pixel_buf, uint32_t count) {
    // Дождаться завершения предыдущей передачи:
    dma_channel_wait_for_finish_blocking(dma_chan_display);

    dma_channel_transfer_from_buffer_now(
        dma_chan_display,
        pixel_buf,
        count   // число пикселей = число uint32_t слов
    );
}
```

### Пример: отправка прямоугольника

```c
// Буфер пикселей в RAM (статический для DMA):
static uint32_t pio_pixel_buf[480 * 320];  // 480×320 × 4 байта = 600 КБ
// Для RP2350 (520 КБ RAM) — слишком много! Используем построчно:
static uint32_t pio_line_buf[480];  // одна строка = 480 × 4 = 1.92 КБ

void fill_rect(PIO pio, uint sm,
               int x, int y, int w, int h,
               uint16_t color_rgb565) {

    uint32_t pio_word = rgb565_to_pio_word(color_rgb565);

    // Заполнить буфер одной строки:
    for (int i = 0; i < w; i++)
        pio_line_buf[i] = pio_word;

    // Установить окно на дисплее:
    ili9488_set_window(x, y, x + w - 1, y + h - 1);
    ili9488_write_cmd(0x2C);  // Memory Write

    gpio_put(PIN_DC, 1);   // Data mode
    gpio_put(PIN_CS, 0);   // CS активен

    // Отправить h строк через DMA:
    for (int row = 0; row < h; row++) {
        display_send_pixels(pio, sm, pio_line_buf, w);
    }
    dma_channel_wait_for_finish_blocking(dma_chan_display);

    gpio_put(PIN_CS, 1);   // CS неактивен
}
```

---

## Рабочее Решение: LovyanGFX (рекомендация если нужен быстрый старт)

LovyanGFX правильно обрабатывает RGB565→RGB666 конверсию внутри,
управляет выравниванием DMA и поддерживает ILI9488 на RP2350 нативно.

```cpp
// platformio.ini:
// lib_deps = lovyan03/LovyanGFX @ ^1.1.12

#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9488  _panel_instance;
    lgfx::Bus_SPI        _bus_instance;
    lgfx::Touch_XPT2046  _touch_instance;

public:
    LGFX(void) {
        // SPI шина:
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host    = 0;           // SPI0
            cfg.spi_mode    = 0;           // Mode 0 (CPOL=0, CPHA=0)
            cfg.freq_write  = 40000000;    // 40 МГц (стабильно на макетке)
            cfg.freq_read   = 6000000;     // 6 МГц чтение
            cfg.pin_sclk    = 18;          // GPIO18
            cfg.pin_mosi    = 19;          // GPIO19
            cfg.pin_miso    = 16;          // GPIO16
            cfg.dma_channel = 1;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        // Панель ILI9488:
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs       = 17;
            cfg.pin_rst      = 21;
            cfg.pin_busy     = -1;
            cfg.panel_width  = 480;
            cfg.panel_height = 320;
            cfg.offset_rotation = 0;
            cfg.readable     = false;
            cfg.invert       = false;
            cfg.rgb_order    = false;
            cfg.dlen_16bit   = false;
            cfg.bus_shared   = false;
            _panel_instance.config(cfg);
        }
        // Touch XPT2046 (shared SPI):
        {
            auto cfg = _touch_instance.config();
            cfg.pin_cs   = 15;
            cfg.pin_int  = 14;
            cfg.freq     = 2500000;       // 2.5 МГц (макс для XPT2046)
            cfg.x_min    = 300;
            cfg.x_max    = 3900;
            cfg.y_min    = 300;
            cfg.y_max    = 3900;
            cfg.bus_shared = true;        // shared с дисплеем
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
        setPanel(&_panel_instance);
    }
};

LGFX display;

void setup() {
    display.init();
    display.setRotation(1);  // landscape
    display.fillScreen(TFT_BLACK);
}
```

---

## Частота SCK — Расчёт для PIO при 300 МГц

```
PIO программа: 2 инструкции, каждая с [1] delay = 2 такта каждая
Итого 1 период SCK = 4 такта PIO

SCK = sys_clock / clk_div / 4

clk_div = 1.0  → SCK = 300/1/4  = 75  МГц  (выше спецификации ILI9488!)
clk_div = 1.25 → SCK = 300/1.25/4 = 60 МГц (предел спецификации)
clk_div = 1.5  → SCK = 300/1.5/4  = 50 МГц (рекомендуется)
clk_div = 2.5  → SCK = 300/2.5/4  = 30 МГц (для макетки с длинными проводами)
clk_div = 5.0  → SCK = 300/5/4    = 15 МГц (безопасно для любой макетки)

Рекомендация для макетки: clk_div = 2.5..5.0 (15..30 МГц)
Рекомендация для PCB:     clk_div = 1.25..1.5 (50..60 МГц)
```

---

## Touch XPT2046 — отдельный PIO SM на том же SPI

```c
// XPT2046 требует SPI 2.5 МГц (vs 60 МГц дисплея)
// Решение: второй PIO SM с другим clk_div, тот же MOSI/SCK
// CS раздельный: PIN_LCD_CS и PIN_TOUCH_CS

// Алгоритм чередования (как в Dmitry Grinberg's driver):
// SM0: рисует N пикселей → поднимает LCD_CS → сигнализирует SM1
// SM1: опрашивает XPT2046 → сигнализирует SM0
// SM0: опускает LCD_CS → продолжает

// Для нашего RTTY проекта упрощённо (touch через CPU каждые 50мс):
void touch_read_xpt2046(int *x, int *y) {
    // Временно остановить DMA дисплея:
    dma_channel_abort(dma_chan_display);
    gpio_put(PIN_LCD_CS, 1);    // LCD off

    gpio_put(PIN_TOUCH_CS, 0);  // Touch on
    // ... SPI транзакция на 2.5 МГц ...
    gpio_put(PIN_TOUCH_CS, 1);  // Touch off

    gpio_put(PIN_LCD_CS, 0);    // LCD on
    // Перезапустить DMA:
    dma_channel_transfer_from_buffer_now(...);
}
```

---

## Сравнение подходов

| Подход | Цвета | Alignment | CPU % | Рекомендация |
|--------|-------|-----------|-------|--------------|
| HW SPI + DMA_SIZE_8 | ✅ Верные | ❌ Нестабильный | ~0% | ❌ Не использовать |
| PIO 24-bit, autopull=32 (неправильно) | ❌ Washed out | ✅ Стабильный | ~0% | ❌ Не использовать |
| **PIO 24-bit, autopull=24 (правильно)** | ✅ Верные | ✅ Стабильный | ~0% | ✅ **Pico SDK** |
| **LovyanGFX** | ✅ Верные | ✅ Стабильный | ~0% | ✅ **Быстрый старт** |
| Битбанг CPU | ✅ Верные | ✅ Стабильный | 100% | ❌ Только для отладки |

---

## Чеклист — Быстрая Диагностика Проблемы

```
Симптом: цвета вымыты / пастельные
  → Проверить: sm_config_set_out_shift(&c, false, true, 24)
                                                        ↑ должно быть 24, не 32
  → Проверить: упаковку R в биты [31..26], не [23..18]
  → Проверить: SCK idle = LOW (gpio_set_outover LOW перед стартом)

Симптом: правильные цвета но полосы / stripes при частичном обновлении
  → Это DMA_SIZE_8 с hw SPI → переходи на PIO решение

Симптом: правильный оттенок но интенсивность 50%
  → autopull threshold = 25 вместо 24, или MSB смещён на 1 бит
  → Проверить: r6 << 26 (не << 25 и не << 24)

Симптом: полностью неверные цвета (каналы перепутаны)
  → shift_right=true вместо false (LSB first вместо MSB first)
  → Исправить: sm_config_set_out_shift(&c, false, ...)
                                              ↑ false = MSB first
```

---

## Ресурсы

- [LovyanGFX GitHub](https://github.com/lovyan03/LovyanGFX) — поддержка ILI9488 + RP2350
- [Dmitry Grinberg PIO LCD Driver](https://dmitry.gr/?r=06.%20Thoughts&proj=09.ComplexPioMachines) — эталонный CPU-free драйвер
- [ILI9488 Datasheet](https://www.crystalfontz.com/controllers/Ilitek/ILI9488/) — RGB666 SPI протокол
- [RP2350 Datasheet §3.5](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf) — PIO, DMA chaining woes
- [pico-feedback issue #321](https://github.com/raspberrypi/pico-feedback/issues/321) — DMA chaining bug (актуально для RP2350 тоже)

---

*Анализ составлен для RP2350A @ 300 МГц, ILI9488 480×320, XPT2046 touch*
*Версия 1.0*
