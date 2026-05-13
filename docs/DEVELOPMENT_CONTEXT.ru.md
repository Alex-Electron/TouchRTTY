# TouchRTTY — Полный контекст разработки

> 🇬🇧 [Read in English](DEVELOPMENT_CONTEXT.md)

*Обновлено: 2026-05-12 — **Build B265 / v2.0.0** (Phase 9 + TinyML NN).*

Этот файл — единый snapshot текущего состояния проекта для разработчика,
который заходит «с холодной кофейной». Не дублирует release notes, а
связывает воедино архитектуру, фазы развития, и где смотреть детали.

---

## 0. TL;DR

- **Цель проекта:** standalone RTTY декодер на RP2350 + ILI9488, который
  принимает лучше 2Tone/fldigi/MMTTY на low SNR.
- **Цель достигнута в v2.0.0** (2026-05-12). Multi-run AWGN bench: на
  SNR −16 dB TouchRTTY даёт ~9 pp реальной CER против ~58 pp у 2Tone
  26.01a. Подробности — `RELEASE_v2.0.0.md`.
- **Текущая ветка:** `feat/alex-cl-dev` (выложена на GitHub как чистый
  squashed commit `v2.0.0`). `main` ещё отстаёт.
- **Что дальше:** real-air NN oracle pipeline, SITOR-B/NAVTEX FEC,
  Phase 10 (см. `NEIGHBOR_IDEAS.md`). Подробности — раздел 6.

**Репозиторий:** `https://github.com/Alex-Electron/TouchRTTY.git`
**Путь:** `C:\Temp\TouchRTTY`

---

## 1. Фазы разработки (хронология и статус)

Фазы шли не строго по номерам — приоритет смещался по результатам бенчей.
Канонический порядок:

| Фаза | Период | Состояние | Где смотреть |
|---|---|---|---|
| **Phase 1** — Архитектура / разделение модулей | до B189 | ✅ DONE (v1.x) | `docs/archive/phase1-8/PHASE1_ARCHITECTURE.md` |
| **Phase 2** — UI / state machines (top bar, popups) | B190-B210 | ✅ DONE (v1.x) | `docs/archive/phase1-8/PHASE2_UI_STATE.md` |
| **Phase 3** — RTTY DSP (Mark/Space IQ, DPLL, Baudot, BAUD-DET, STOP-DET, SEARCH) | B210-B240 | ✅ DONE (v1.72) | `docs/archive/phase1-8/PHASE3_RTTY_DSP_FINAL.md` |
| **Phase 4** — SD-карта (логирование, DWD SYNOP parser) | — | ⏸ PLANNED | `docs/archive/phase1-8/PHASE4_SD_CARD_PLAN.md` |
| **Phase 5** — CW advanced decoder | — | ⏸ PLANNED | `docs/archive/phase1-8/PHASE5_CW_ADVANCED_DECODER.md` |
| **Phase 6** — FT8 / FT4 | — | ⏸ PLANNED | `docs/archive/phase1-8/PHASE6_FT8_FT4_PLAN.md` |
| **Phase 7** — WEFAX (HF weather fax) | — | ⏸ PLANNED | `docs/archive/phase1-8/PHASE7_WEFAX_PLAN.md` |
| **Phase 8** — DRM | — | ⏸ PLANNED | `docs/archive/phase1-8/PHASE8_DRM_PLAN.md` |
| **Phase 9** — Hybrid RTTY decoder (dual-IQ + LLR + Soft-Viterbi + TinyML NN) | B242-B265 | ✅ DONE (v2.0.0) | `RELEASE_v2.0.0.md`, `docs/PHASE9_*.md` |
| **Phase 10** — research backlog (Symbol-MLSE, Gardner, n-gram LM, IQ-вход) | — | 🔬 RESEARCH | `docs/NEIGHBOR_IDEAS.md` |

> **Почему Phase 9 был сделан раньше Phase 4-8.** Приоритет сместился к
> стратегической цели «обогнать 2Tone на low SNR». Phase 4-8 — *модовое
> расширение* (новые виды сигналов), а Phase 9 — *улучшение качества
> приёма* RTTY, который и так центральный для устройства. Под Phase 9
> накопилось достаточно знаний (см. archived Phase 3 + neighbor ideas),
> поэтому он реализован первым.

Phase 1-8 архивы лежат в `docs/archive/phase1-8/` — это исторические
design docs, по которым велась реализация v1.x. Они актуальны как
справочник по архитектуре до Phase 9. Phase 4-8 описывают **планы** — их
ещё предстоит реализовать.

---

## 2. Текущая архитектура (v2.0.0)

### 2.1 Аппаратная часть

- **MCU:** RP2350 (dual Cortex-M33, FPU, 150 MHz)
- **Дисплей:** ILI9488 480×320 TFT, PIO SPI @ 60 MHz, тач XPT2046
- **Аудиовход:** ADC0 (GPIO26), 10 kHz sample rate, 1.65 V bias
- **Сборка:** CMake + Ninja + Pico SDK 2.x, прошивка через picotool

Полная распиновка и схема bias-сети — `docs/HARDWARE_SETUP.md`.

### 2.2 Dual-core архитектура

- **Core 0 (DSP, ~7-10%)** — `dsp_pipeline.cpp`:
  10 kHz hard-real-time loop. ADC DMA → AGC → BPF 300-3000 Hz →
  LMS notch chain → dual-IQ path A (narrow) + path B (wide) →
  LLR fusion (HYB) → DPLL/PI → bit slicing → Soft-Viterbi frame gate →
  B264 confidence gate → optional NN inference → Baudot → ITA-2 ASCII.
- **Core 1 (UI, ~25-35%)** — `ui_loop.cpp`, `ui/UIManager.hpp`:
  FFT (1024-point) → водопад/спектр/scope → SEARCH → touch handling →
  serial command parser → display render.

Межъядерный обмен через `volatile` shared-переменные в
`app_state.hpp/.cpp` (lock-free, без mutex). Полный список — см.
исходник.

### 2.3 Signal flow

```
ADC0 @ 10 kHz, 1.65 V bias
   │
   ▼
63-tap FIR BPF, центр = FREQ
   │
   ▼
Quadrature (I/Q) demod → biquad LPF
   │
   ├── Path A (narrow, BW≈baud) ──┐
   └── Path B (wide, BW≈1.5×baud) ─┤
                                    ▼
                              LLR fusion (HYB)
                                    │
                                    ▼
                              DPLL (PI loop, ALPHA)
                                    │
                                    ▼
                              Bit slicing → 7 soft bits
                                    │
                              ┌─────┴─────┐
                              ▼           ▼
                       Hard decision   B264 gate:
                       (sign)          if data_min/sig < 0.20
                                       → NN goes
                              │           │
                              └─────┬─────┘
                                    ▼
                            32 Baudot codes
                                    ▼
                            ITA-2 → ASCII → screen + serial
```

См. также `README.md` (упрощённая блок-схема) и
`docs/PHASE9_HYBRID_DECODER_PLAN.md` (детальный дизайн).

---

## 3. Ключевые подсистемы и где их искать

| Подсистема | Файл | Документ |
|---|---|---|
| Точка входа, инициализация | `src/main.cpp` | — |
| DSP pipeline (Core 0) | `src/dsp_pipeline.cpp` | `docs/PHASE9_HYBRID_DECODER_PLAN.md`, archived Phase 3 |
| UI loop (Core 1) | `src/ui_loop.cpp` | `docs/MENU_GUIDE.md` |
| Serial CLI (40+ команд) | `src/serial_commands.cpp` | `docs/SERIAL_COMMANDS.md` |
| Shared state | `src/app_state.{hpp,cpp}` | — |
| Flash settings | `src/settings_flash.cpp` | `docs/MENU_GUIDE.md` §SAVE |
| Touch calibration | `src/touch_xpt2046.cpp` | `docs/HARDWARE_SETUP.md` |
| Display driver | `src/display/ili9488_driver.h`, PIO | — |
| UI render | `src/ui/UIManager.hpp` | `docs/MENU_GUIDE.md` |
| NN inference | `src/dsp_pipeline.cpp` (B264 gate + MLP), `src/dsp/nn_weights.h` | `docs/NN_TRAINING.md` |
| Build number | `src/version.h` (текущий: **B265**) | — |

---

## 4. Что нового в v2.0.0 относительно v1.72

Полные release notes — `RELEASE_v2.0.0.md`. Кратко:

- **Phase 9 архитектура** — dual-IQ paths + LLR fusion + Soft-Viterbi
  frame validation + LMS notch + DPLL PI + AFC ±100 Hz + SNR squelch.
- **TinyML NN** (v13 weights) — 7→128→64→32 MLP (~44 KB), запускается
  только когда **B264 confidence gate** открыт (`data_min < 0.20·sig`).
  PyTorch trainer с per-sample loss weighting (`weight_uncertain=3.0`).
- **DUMP FRAMES** — serial команда стримит per-frame soft-bits + hard
  decision для capture training data.
- **UI** — Tuning Lab с persistent eye diagram, PATH cycle (A/B/HYB/HYB+NN),
  inline NOTCH/VIT toggles, красная `*` для невалидных фреймов,
  factory-reset диалог.
- **40+ команд** serial CLI: live tuning, persistence, diagnostics, NN
  control, дамп frames/spectrum/mark-space.
- **Документация** — шесть long-form гайдов в `docs/` (Hardware, Serial,
  Menu, NN training, Bench, плюс этот файл).

### Breaking changes

- **RP2040 больше не поддерживается** — нужен RP2350 (FPU + SRAM).
- Default `PATH HYB+NN` вместо `PATH A`.
- Удалены раннефазовые planning doc'и `PHASE1..PHASE7_*.md` из корня
  `docs/` (перенесены в `docs/archive/phase1-8/`).

---

## 5. Тулинг разработки

Полный decision tree — `docs/BENCH_TOOLING.md`. Основные скрипты:

| Скрипт | Назначение |
|---|---|
| `tools/rtty_simulator.html` | Браузерный генератор тестового RTTY |
| `tools/rtty_gen.py` | Синтез WAV с заданным SNR (AWGN) |
| `tools/sweep_runner.py` | SNR-лесенка через HW + serial capture |
| `tools/bench_replay.py` | Replay recorded WAV → serial log |
| `tools/nn_sweep_compare.py` | NN-OFF vs NN-ON A/B sweep |
| `tools/aggregate_compare.py` | Multi-seed mean ± σ агрегация |
| `tools/cer_analyze.py` | CER analysis (cyclic-rotation aware) |
| `tools/train_nn_torch.py` | PyTorch NN trainer (v13 recipe) |
| `tools/parse_dump_frames.py` | B265 DUMP stream → numpy npz |
| `tools/overnight_runner.sh` | Цепочка train+sweep для unattended runs |
| `tools/send_serial_cmd.py` | Одноразовая serial-команда |

Все скрипты — внутренние, в публикуемой ветке остаются только нужные для
повторяемости релиза.

### Сборка и прошивка

```bash
git clone --recurse-submodules https://github.com/Alex-Electron/TouchRTTY.git
cd TouchRTTY && mkdir build && cd build
cmake -G Ninja -DPICO_SDK_PATH=/path/to/pico-sdk ..
ninja
picotool load -f TouchRTTY.uf2
```

Прошивка под Windows (этот рабочий стенд) — всегда через `picotool`
(см. memory `feedback_picotool.md`), а не drag-and-drop в RPI-RP2.

---

## 6. Что дальше (post-v2.0.0)

Полный backlog — `docs/ROADMAP_OPTIMIZATION.md` §9. Приоритеты:

1. **Real-air NN oracle pipeline** — DWD template matcher даёт
   ground-truth для uncertain frames; ожидаемо двигает порог −16 → −20
   dB. Сейчас real-air augmentation ограничено тем, что labels берутся
   от hard-decision (NN не может научиться обыгрывать hard-decision).
2. **SITOR-B / NAVTEX FEC** — 100 baud / 170 Hz, CCIR 476, ratio 4:3,
   time diversity. Память `project_sitorb.md`.
3. **Phase 10** — `docs/NEIGHBOR_IDEAS.md`: Symbol-MLSE, Gardner clock
   recovery, Flywheel DPLL, semantic auto-INV lockout. Каждый — отдельный
   эксперимент с multi-seed bench.
4. **Phase 4-8** — расширение режимов (CW advanced, FT8/FT4, WEFAX,
   DRM, SD-карта). Низкий приоритет — отдельная стратегическая ветка.
5. **UI палитры / скины** — память `project_ui_palettes.md` («hacker
   green» и др.). Косметика.

---

## 7. Известные ограничения

1. **Real-air NN improvement plateau** — без oracle pipeline NN
   обучается только на синтетике + hard-decision labels real-air. См.
   negative-result ledger в `docs/NN_TRAINING.md`.
2. **425 vs 450 Hz shift** — FFT разрешение ~10 Hz/bin, 2.5 bin разница
   неразличима при FSK keying spectral smear. Workaround — manual
   SHIFT.
3. **Memory barriers** — нет `__dmb()` между ядрами, теоретическая race
   на shared volatile. На практике не воспроизводится. TODO.
4. **2Tone benchmark** — N1MM-эмулятор не доводит handshake до 2Tone
   26.01a, DSP отключается. Для head-to-head сравнения используем
   audio loopback (Voicemeeter) + ручной запуск. Память
   `project_2tone_unreliable.md`.

---

## 8. Куда смотреть дальше

| Хочешь… | Открывай |
|---|---|
| Прошить и начать пользоваться | `README.md` + `docs/HARDWARE_SETUP.md` |
| Понять CLI | `docs/SERIAL_COMMANDS.md` |
| Понять touchscreen UI | `docs/MENU_GUIDE.md` |
| Обучить свой NN | `docs/NN_TRAINING.md` |
| Запустить бенч | `docs/BENCH_TOOLING.md` |
| Полный roadmap / DONE-list | `docs/ROADMAP_OPTIMIZATION.md` |
| Что было в v2.0.0 | `RELEASE_v2.0.0.md` |
| История Phase 9 design | `docs/PHASE9_HYBRID_DECODER_PLAN.md` |
| Phase 9 progress срез B245 | `docs/PHASE9_PROGRESS_REPORT.md` |
| Идеи на Phase 10 | `docs/NEIGHBOR_IDEAS.md` |
| Phase 1-8 архив | `docs/archive/phase1-8/` |

---

*Это живой документ. При следующем мажорном изменении архитектуры —
обновить «фазы» и snapshot.*
