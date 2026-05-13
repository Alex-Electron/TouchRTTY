# Roadmap: Оптимизация и Улучшение (Phase 3+)

> 🇬🇧 [Read in English](ROADMAP_OPTIMIZATION.md)

*Обновлено: 2026-05-12, **Build 265 / v2.0.0** released*

> **Status:** Phase 9 ушёл в production вместе с TinyML NN классификатором.
> Стратегическая цель раздела 8 («лучше 2Tone») **достигнута** —
> multi-run AWGN bench показал, что TouchRTTY делает 2Tone в 3–6 раз по
> реальной CER на SNR −12..−22 dB. Путь оказался отличным от изначально
> запланированного (dual-IQ + LLR вместо Goertzel + Character-ML), но
> результат тот же. Подробности — раздел 8 ниже.

## 0. Рефакторинг кода (Build 189-206) — DONE

### Разделение main.cpp на модули — DONE

До Build 189 весь код (DSP, UI, serial, touch, state machines) находился в одном файле `main.cpp` (~1843 строки). В процессе развития проект был разбит на модули:

| Файл | Строк | Назначение |
|------|-------|-----------|
| `main.cpp` | 55 | Точка входа, инициализация HW, запуск Core 1 |
| `dsp_pipeline.cpp` | 703 | Core 0: ADC→AGC→I/Q→LPF→ATC→DPLL→Baudot→BAUD-DET→STOP-DET→auto-INV→auto-recovery |
| `ui_loop.cpp` | 915 | Core 1: FFT, SEARCH, спектр/водопад, touch, serial parser |
| `serial_commands.cpp` | 223 | 40+ serial-команд (B265) |
| `settings_flash.cpp` | 103 | Чтение/запись AppSettings во Flash (2MB offset) |
| `app_state.hpp/.cpp` | 144+96 | Все shared volatile переменные и константы |
| `ui/UIManager.hpp` | 1100+ | Отрисовка: спектр, водопад, текст, top/bottom bar, меню, Tuning Lab |
| **Итого** | **~3500** | |

### Ключевые принципы рефакторинга

1. **Разделение по ядрам:** `dsp_pipeline.cpp` исполняется строго на Core 0, `ui_loop.cpp` — на Core 1. Это гарантирует отсутствие взаимных блокировок.

2. **Shared state как единая точка:** Все межъядерные переменные собраны в `app_state.hpp/cpp`. Volatile semantics, без mutex.

3. **State machines изолированы:** BAUD-DET, STOP-DET, auto-INV, auto-recovery — каждый со своими фазами и local state, все внутри `dsp_pipeline.cpp`.

4. **UI отделён от логики:** `UIManager.hpp` — чистая отрисовка, принимает параметры через аргументы.

## 0a. Оптимизация производительности (Build 189-194) — DONE

### Снижение загрузки Core 0 (DSP)

**До оптимизации (Build 188):** Core 0 = ~30%, Core 1 = ~70%.
**После оптимизации (Build 191+):** Core 0 = ~7%, Core 1 = ~25-35%.

Ключевые оптимизации:

1. **Strict Float Policy (Build 189):**
   - Полный аудит: все `double` → `float`, `sin()` → `sinf()`, `log10()` → `log10f()`
   - RP2350 имеет single-precision FPU; double-precision эмулируется софтово (~10x медленнее)
   - Эффект: Core 0 с ~30% до ~15%

2. **Compiler flags (Build 189):**
   ```cmake
   -O3 -ffast-math -funroll-loops
   -mfloat-abi=hard -mfpu=fpv5-sp-d16
   ```
   `-flto` **не** используется — несовместим с Pico SDK `__wrap_` символами.

3. **Hardware ADC FIFO (Build 190):**
   - `adc_fifo_setup()` + `adc_run(true)` для 10kHz без джиттера
   - `tight_loop_contents()` вместо `__wfe()` (WFE теряет сэмплы без ADC IRQ)

4. **fast_log2f() (Build 190):**
   - IEEE 754 bit-trick для логарифма
   - ~4x быстрее стандартного `log10f()`
   - Используется в расчёте dB для сигнала и SNR

5. **AGC precompute (Build 190):**
   - `1.0f / release` вычисляется один раз → умножение вместо деления в inner loop

6. **FFT на Core 1 (Build 191):**
   - FFT перенесён с Core 0 на Core 1 — он нужен только для отрисовки спектра и SEARCH
   - Core 0 освобождён от 1024-point FFT (~2ms per frame)
   - Эффект: Core 0 с ~15% до ~7%

7. **Ping-Pong DMA Buffers (Build 190):**
   - Двойная буферизация для SPI дисплея
   - Одна полоска рисуется пока вторая передаётся

### Снижение загрузки Core 1 (UI)

1. **Спрайтовая отрисовка (LovyanGFX):** перерисовка только при изменении данных.
2. **Waterfall оптимизация:** прямой SPI DMA для полосок водопада.
3. **FFT rate limiting:** каждые ~48ms = 480 сэмплов.
4. **Waterfall LUT + circular history buffer** (Build 219) — 480×64 uint8 вместо 61KB sprite, Core 1 нижняя граница 60%→39%.

### Текущая загрузка (Build 265)

- **Core 0:** 5-8% (DSP idle) / 10-15% (BAUD-DET active) / +1-2% когда NN gate открыт
- **Core 1:** 25-35% (зависит от display mode)

## 1. Шрифтовая система (Roadmap Item #1)

### Этап 1: 4 режима шрифтов — DONE (Build 195)

- [x] BIG: Spleen 8×16 (9 строк, 55 символов)
- [x] MED: Bitocra 7×13 (11 строк, 62 символа)
- [x] SMALL: Font0 6×8 (15 строк, 73 символа)
- [x] TINY: Spleen 5×8 (17 строк, 90 символов) — Build 199
- [x] Конвертер `tools/bdf2gfx.py`
- [x] Автоматический line_width при переключении шрифта
- [x] Сохранение в flash

### Этап 2: Font Lab — TODO

Отдельный экран для тонкой настройки шрифтов (размер, spacing, line_height).

### Этап 3: Скины и цветовые схемы — TODO

- Classic Green (текущая)
- SDR Warm (тёмно-синий фон, тёплая палитра водопада)

## 2. Интеллектуальная автоматика приёма — DONE

### Авто-инверсия Mark/Space — DONE (Build 196-202)

- [x] Сравнительный алгоритм (ERR before/after flip, ±3% порог)
- [x] Индикатор NOR?/INV? при неопределённости
- [x] SEARCH сбрасывает INV → NOR

### SEARCH (автопоиск) — DONE (Build 198-216)

- [x] FFT-based, все 8 шифтов, multi-signal (до 8)
- [x] Parabolic peak interpolation (Build 216)
- [x] Shift-proportional dedup tolerance (Build 216)
- [x] dist_penalty = 2.5 (Build 216)
- [x] Cycling (< 10s между нажатиями)

### Авто-определение шифта — DONE (Build 200-203)

- [x] 8 стандартных шифтов, режим SHIFT AUTO (idx=8)
- [x] Popup 3×3

### BAUD-DET (автоопределение скорости) — DONE (Build 206)

- [x] Symbol Duration Histogram + Harmonic Scoring
- [x] Fallback: ERR verify (sequential test)
- [x] 4 baud rates: 45.45 / 50 / 75 / 100
- [x] Popup 3×2

### STOP-DET (автоопределение стоп-бита) — DONE (Build 205-218)

- [x] Direct gap measurement (state-7-end → next start-bit)
- [x] Warmup 1.5s, idle filter 1.25T, bin boundaries 0.25/0.85T (Build 218)
- [x] Chain BAUD→STOP через shared_chain_stop_after_baud (Build 217)
- [x] Popup 2×2

### Полный pipeline — DONE (Build 217)

- [x] SEARCH → SHIFT → BAUD (chain) → STOP → INV
- [x] Автоматическая цепочка, STOP ждёт завершения BAUD
- [ ] Итоговый экран "Found: 50 Baud, 450 Hz shift, 1.5 stop"

### Auto-Recovery — DONE (Build 217)

- [x] ERR > 15% для 3s → BAUD-DET → chain → STOP-DET
- [x] Защита от конфликта с auto-INV

### Clipping Indicator — DONE (Build 216)

- [x] SIG bar мигает red/white при ADC clipping
- [x] Текст "CLIP!" мигает синим
- [x] Latch 1.5 секунды

## 3. Аппаратное Ускорение Рендеринга

- [ ] Hardware Scroll (ILI9488 VSCRSADD)
- [ ] SIO INTERP Colormap
- [x] Ping-Pong DMA Buffers (Build 190)
- [x] PIO-driven SPI at 60 MHz (Build 191+) — заменил программный SPI на PIO state machine

## 4. Оптимизация под RP2350

- [x] Strict Float Policy (Build 189)
- [x] Hardware ADC FIFO (Build 190)
- [x] fast_log2f() IEEE 754 bit-trick (Build 190)
- [x] AGC precompute (Build 190)
- [x] FFT на Core 1 (Build 191)
- [ ] Memory Barriers (__dmb())
- [ ] CMSIS-DSP (arm_fir_f32, arm_biquad_f32)

## 5. UI оптимизация

- [ ] Selective Redraw
- [ ] Widget Framework
- [x] Eye Diagram с phosphor persistence (Build 194)
- [x] Error Rate Indicator, 3 thin bars (Build 191)
- [x] Tuning Lab с live ALPHA/K/SQ настройкой (Build 194)
- [x] Inline NOTCH/VIT toggles в menu (Build 263, выкинули popup)
- [x] Красная `*` для [ERR] на экране (Build 263)

## 6. Compiler Flags

- [x] `-O3`, `-ffast-math`, `-funroll-loops` (Build 189)
- [x] `-mfloat-abi=hard`, `-mfpu=fpv5-sp-d16` (Build 189)
- **Примечание:** `-flto` несовместим с Pico SDK `__wrap_`

## 7. Serial Command Interface

- [x] 40+ команд (Build 194-265)
- [x] Диагностический поток `[D]` (Build 194)
- [x] serial_cmd.ps1 с try/finally/Dispose + DTR/RTS (Build 217)
- [x] **B265 DUMP FRAMES** — per-frame soft-bit dump для NN training capture

## 8. Гибридный декодер RTTY — **CEЛЬ ДОСТИГНУТА** (v2.0.0)

**Стратегическая цель:** Порог декодирования **−15..−16 дБ SNR** — лучше, чем у любого существующего декодера RTTY в мире.

**Что получилось vs изначальный план:**

Архитектура отличается от изначального Goertzel + Character-ML плана. я пошёл путём **dual-IQ + LLR fusion + TinyML NN классификатор**, и это сработало. Поэтому статус подэтапов ниже отражает фактический путь, не оригинальный.

### Архитектура (фактическая, Phase 9)

```
                              ┌─ Path A (narrow FIR + I/Q + DPLL) ─┐
ADC → AGC → BPF → LMS notch ─┤                                     ├─ LLR fusion ─→ NN gate ─→ Baudot
                              └─ Path B (wide FIR + I/Q + DPLL) ────┘    (B264)
                                                                          │
                                                                          ▼
                                                                 Soft-Viterbi
                                                                 frame gate
```

### Фактические результаты (multi-run AWGN, 3 seeds × дрелл 30s)

| Декодер | Заявленный порог | Реальная CER на −16 dB SNR |
|---|---:|---:|
| fldigi | ~−5 dB | (не бенчили head-to-head) |
| MMTTY | ~−9 dB | (не бенчили head-to-head) |
| 2Tone (current best) | ~−13 dB | **~58 pp real errors** |
| **TouchRTTY v2.0.0** | ~**−16 dB** | **~9 pp real errors** ✓ |

См. `RELEASE_v2.0.0.md` и `datasets/logs/multirun_summary.md`.

### Этап 1: Dual-Goertzel Matched Filter — **N/A** (architecture changed)

Изначально планировался Goertzel filter параллельно I/Q. Вместо этого реализована **dual-IQ архитектура** — два параллельных FIR+I/Q+DPLL pipeline'а (narrow + wide), сливающиеся через LLR. Goertzel не понадобился — два I/Q chain'а покрывают тот же случай (узкая полоса для clean, широкая для drift) проще и без отдельной синхронизации.

### Этап 2: Multi-phase Goertzel — **N/A** (architecture changed)

См. выше — DPLL+PI controller на обоих chain'ах закрыл потребность в multi-phase синхронизации.

### Этап 3: Character-level ML — **DONE (v13 NN)** ✅

Достигнуто через PyTorch-обученный MLP вместо 2Tone-style matched filter:

- [x] **7→128→64→32 TinyML MLP** (~44 KB float32 weights)
- [x] **B264 confidence gate** — NN запускается только когда `data_min/sig_level < 0.20`
- [x] **PyTorch trainer** с per-sample loss weighting (v13 production recipe)
- [x] **Soft output** — `nn_margin = top_logit − second_top_logit` используется как мера уверенности
- [x] **Multi-run валидация** — σ < 4 pp на ключевых SNR
- [x] **Reproducible**: код, веса, данные, бенч-эвиденс в репо

### Этап 4: Улучшения сверх 2Tone

#### 4a. Контекстный языковой приор (n-gram) — TRIED, NOT SHIPPED

Эксперимент в `tools/ngram_lm/` (см. tree history). Гейн +1.63 pp по corpus-внутреннему бенчу (B259), но на real-air не подтвердился стабильно. Отложено.

#### 4b. FIGS/LTRS Viterbi — **DONE (B262 VIT)** ✅

Реализовано как часть **Soft-Viterbi frame validation gate**:

- [x] State machine framing с energy + parity validation
- [x] Конфигурируемый через `VIT ON/OFF` serial команду
- [x] Default ON в production

#### 4c. Adaptive Noise Blanker + Spectral Subtraction — PARTIAL ✅

- [x] **LMS adaptive notch chain** (`NOTCH ON/OFF`, Build 244+) — убивает narrow carriers / heterodynes
- [ ] Impulse noise blanker (`>3σ over 100ms → mute 5ms`) — TODO
- [ ] **Wiener Spectral subtraction** — TRIED, NOT SHIPPED. Эксперимент B258 (`NR ON/OFF`) дал neutral-to-harmful результат под 3-run honest averaging. Default OFF, код оставлен на случай если найдём правильный порог.

#### 4d. Temporal Diversity — N/A

Не реализовано. Soft-Viterbi гейт частично перекрывает кейс через energy averaging.

#### 4e. Multi-band Goertzel для SEARCH — N/A

SEARCH остался FFT-based (multi-shift), достаточно быстро.

#### 4f. Tiny Neural Net — **DONE (v13 production)** ✅

Это **главный win этой версии.** v13 NN весит ~44 KB, использует PyTorch sample_weight рецепт. См. этап 3 выше и `docs/NN_TRAINING.md`.

Bonus: **B265 DUMP FRAMES** позволяет пользователю собирать real-air training data для retraining на своих условиях.

#### 4g. Soft Confidence UI — PARTIAL ✅

- [x] Красный `*` для невалидных фреймов на экране (B263)
- [x] Top bar показывает NN/NOTCH/VIT статус
- [ ] Цветная градация уверенности (зелёный/жёлтый/красный) per character — TODO
- [ ] `[ML:94%]` в top bar — TODO

### Итоговый бюджет CPU (Core 0 @ 300 МГц, B265)

| Компонент | CPU |
|---|---|
| ADC/AGC/FIR | ~2% |
| Dual-IQ (A+B) + DPLL | ~3% |
| LMS notch chain | ~0.5% |
| Soft-Viterbi frame gate | ~0.5% |
| NN inference (when gate open) | +1-2% (sparse) |
| BAUD-DET (when running) | +5-7% (transient) |
| **Steady-state total** | **~7-10%** |

Запас ~90% Core 0 для будущих режимов (CW, FT8, DRM).

## 9. Планируемые фичи

### SITOR-B / NAVTEX FEC — TODO (приоритет)

- [ ] Framer: 7 data bits + 1 stop
- [ ] CCIR 476 lookup (35 valid codewords, ratio 4:3)
- [ ] Time diversity buffer (5 символов)
- [ ] Phasing sync (DX/RX signals)
- [ ] Auto-detect: 100/170 → try SITOR-B

### NN training: real-air oracle pipeline — TODO

Тема, оставшаяся после v2.0.0. Сегодня real-air augmentation
ограничено тем, что labels берутся от hard-decision — модель не может
научиться обыгрывать hard-decision на uncertain frames, потому что
для них labels неизвестны. Решение:

- [ ] **DWD template matcher** — парсер DWD weather format (predictable
      day-of-week, PPZ/QWZ patterns, wind directions) даёт ground
      truth для известных recordings
- [ ] Replay через HW в DUMP FRAMES режиме, label uncertain frames
      против oracle
- [ ] v14 NN training с **корректными** real-air labels на uncertain
      frames — это путь сдвинуть −16 dB порог ещё на 5-10 pp

### Встроенный автотюнинг — TODO

- [ ] Кнопка AUTO в Tuning Lab
- [ ] Hill-climb: ALPHA → BW → SQ
- [ ] Score = -5×ERR + SNR - 1000×|FE| + SQ_bonus

### Прочее

- [ ] Итоговый экран "Found: ..." после SEARCH
- [ ] Цветная градация ML confidence на тексте
- [ ] Мультиплатформенность (ILI9341 320×240)
- [ ] SD карта (DWD SYNOP parser)
- [ ] CW Декодер (K-Means)
- [ ] I2S DAC Audio Output
- [ ] FT8 / FT4 mode
- [ ] WEFAX (HF weather fax)

---

*Текущий статус: **v2.0.0 released**, ветка `feat/alex-cl-dev`,
firmware build B265, NN weights v13.*

*v2.0.0 закрыла стратегическую цель раздела 8 — превзойти 2Tone на
low SNR. Дальнейшая работа над NN-частью описана в
`docs/NN_TRAINING.md` (negative-result ledger + real-air oracle
direction).*
