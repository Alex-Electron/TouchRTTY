# TouchRTTY: Master Session Context (Save State)
**Дата последнего обновления:** 2026-04-04
**Текущая ветка:** `feature/rtty-dsp-improvements`
**Текущий билд:** 190 (Stable Base: 185)

## 1. ТЕКУЩИЙ СТАТУС ПРОЕКТА
- **Интерфейс:** Оптимизирована система шрифтов (F2:NORM и F0:NARW), удалено дробное масштабирование.
- **Функции:** Добавлена кнопка **AFC**, реализован **Smart Newline**.
- **Шрифты:** Оставлены только фавориты — **F2 (Standard)** и **F0 (Narrow)**. Высота NARW = 10px.
- **Инструменты:** Веб-симулятор `tools/ui_prototype.html` полностью синхронизирован с прошивкой (Build 188).
- **DSP:** FFT перенесён на Core 0, hardware ADC FIFO, ping-pong DMA для дисплея.

## 2. ИНЖЕНЕРНЫЕ ПРАВИЛА (ОБЯЗАТЕЛЬНО К ИСПОЛНЕНИЮ)
1. **Surgical Edits Only:** Не перезаписывать `main.cpp` целиком.
2. **Strict Float Policy:** ТОЛЬКО `float` константы (`f`) и функции (`sinf()`, `cosf()`).
3. **RAM Execution:** DSP-функции с атрибутом `__time_critical_func`.
4. **Modulo Removal:** Запрещено использовать `%` в горячих циклах Core 0.
5. **Прошивка через picotool:** `~/.pico-sdk/picotool/2.2.0-a4/picotool/picotool.exe load build/TouchRTTY.uf2 -f && picotool reboot`

## 3. ИСТОРИЯ БИЛДОВ (последние)
### Build 190 (текущий): Fix FPS drop + optimize DSP
- **Фикс:** `__wfe()` → `tight_loop_contents()` в ADC wait loop Core 0 (причина FPS 22→14: FIFO overflow из-за отсутствия ADC IRQ для пробуждения __wfe)
- **Фикс:** Убран drain overflow (`while (adc_fifo_get_level() > 4)`) — выбрасывал сэмплы
- **Фикс:** `sleep_us(50)` → `tight_loop_contents()` на Core 1 idle
- **Оптимизация (из Build 189 uncommitted):**
  - FFT перенесён с Core 1 на Core 0 (освобождает ~500μs/фрейм для будущего DRM)
  - Hardware ADC FIFO (adc_fifo_setup + adc_run) вместо software busy-wait
  - Ping-pong double buffering в ili9488_push_colors
  - fast_log2f() через IEEE 754 bit tricks (~4x быстрее log10f)
  - AGC: precomputed 1/release (умножение вместо деления)
  - Кеширование expf() для ATC envelope (10000 вызовов/сек → только при смене baud)
  - Bitmask phosphor fade для Lissajous (uint32 shift+mask)
  - Sin/cos lookup table для Lissajous
  - Комментарий: -flto несовместим с Pico SDK __wrap_ символами

### Build 189: Advanced DSP Optimization (committed)
### Build 188: Hardware-accurate color rendering (committed)

## 4. ПЛАНЫ И ЗАДАЧИ (ROADMAP)
### Phase 3 (Текущая): Оптимизация
- [x] **Build 188:** Удалить лишние шрифты, оставить F2 и F0.
- [x] **Build 189:** Первый этап `f` суффиксов и `__time_critical_func`.
- [x] **Build 190:** ADC FIFO + FFT на Core 0 + ping-pong DMA + fix FPS.
- [ ] **Build 191:** Аппаратный скролл ILI9488 (разгрузка Core 1).

### Phase 8 (Будущее): SDR и DRM
- [ ] **Color Fidelity:** Дизеринг для RGB565 градиентов.
- [ ] **I/Q Input:** Belka-DX (40 кГц IQ) через ADC0/ADC1.
- [ ] **Audio:** Декодирование HE-AAC v2 через FDK AAC.
- [ ] **Hardware:** I2S ЦАП (PCM5102).

## 5. ИНСТРУКЦИЯ ДЛЯ НОВОЙ СЕССИИ
*При старте нового чата загрузи этот файл и скажи: "Проанализируй SESSION_CONTEXT.md, ветку feature/rtty-dsp-improvements и билд 190. Мы готовы продолжать."*
