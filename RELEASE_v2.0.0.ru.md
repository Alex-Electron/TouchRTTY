# TouchRTTY v2.0.0 — Phase 9 + TinyML NN

> 🇬🇧 [Read in English](RELEASE_v2.0.0.md)

**Дата релиза:** 2026-05-12
**Build прошивки:** B265
**Веса NN:** v13 (`weight_uncertain=3.0` PyTorch-рецепт)
**Артефакт:** [`TouchRTTY_v2.0.0.uf2`](TouchRTTY_v2.0.0.uf2)
**Предыдущий релиз:** [v1.72](https://github.com/Alex-Electron/TouchRTTY/releases/tag/v1.72) (март 2026)

---

## Главное

TouchRTTY v2.0.0 — это серьёзный переписанный код относительно v1.72:
принципиально новая архитектура декодера плюс learned post-classifier
поверх. На том же аудио, где [2Tone 26.01a](https://www.rttycontesting.com/downloads/2tone/)
разваливается в случайные буквы на низком SNR, эта прошивка выдаёт
читаемый телетайп. Я сравнили с 2Tone по нескольким сидам, и
multi-run-усреднённые цифры такие:

| SNR | TouchRTTY NN OFF | TouchRTTY NN ON | 2Tone (реальные ошибки) |
|---:|---:|---:|---:|
| −12 | 16 % | **15 %** | ~22 pp |
| −14 | 32 % | **23 %** (σ = 1.5) | — |
| −16 | 78 % | **55 %** (σ = 3.2) | ~58 pp |
| −20 | 88 % | **80 %** (σ = 2.3) | — |

Это в 3–6 раз ниже реальный error rate на тех SNR, что имеют значение.
Низкое стандартное отклонение важнее самих цифр — оно значит, что
улучшение воспроизводится между сидами, а не один счастливый прогон.
Эталонный 2Tone-бенч (закоммиченный evidence, то самое аудио, что
показывает «гиббериш vs телеграф») — в
[`datasets/logs/bench_auto_v2/`](datasets/logs/bench_auto_v2/).

---

## Что нового vs v1.72

### Полностью новая архитектура декодера (Phase 9)

* **Dual-IQ paths** — узкая A и широкая B цепочки фильтров параллельно
* **LLR fusion** — log-likelihood-ratio объединение A и B, с
  опциональным SNR-взвешенным dynamic-режимом (`DYN ON`)
* **Soft-Viterbi frame validation** — полный гейт по энергии +
  parity для Baudot-фреймов; тюнится через `VIT ON/OFF`
* **LMS adaptive notch chain** — тогл `NOTCH ON/OFF` убивает узкие
  несущие внутри audio-passband
* **DPLL с PI-контроллером** — `ALPHA` подстраивается живьём с
  экрана или по serial
* **AFC** — трекинг дрейфа ±100 Гц от настроенной `FREQ`
* **AGC** — быстрая атака, медленный release
* **SNR-based squelch** с гистерезисом

### Нейросетевой post-classifier

Маленький (7→128→64→32, около 44 КБ float32) MLP голосует на
Baudot-фреймах, где soft-bit паттерн неопределённый.

* **B264 confidence gate** — NN стреляет только когда слабейший
  data-бит ниже 20 % от оценённого уровня сигнала. Выше — hard
  decision доверяем без вмешательства. Это и убирает pre-gate
  U-образную кривую, где NN помогала у порога и вредила на
  комфортном SNR.
* **Production-веса — v13** — PyTorch trainer, ключевой трюк —
  per-sample loss weighting (3× буст для `data_min < 0.30` фреймов).
  Sklearn'овский `MLPClassifier` не поддерживает `sample_weight`, это
  и есть фактическая причина порта на torch.
* **DUMP FRAMES serial-команда** — стримит per-frame soft-bits +
  hard decision labels, годится для capture real-air training data и
  расширения синтетического сета.

### Настоящий UI

* **3-bar top panel** — SIG / AGC / ERR (скользящее окно 100 фреймов)
* **Tuning Lab** с phosphor-persistent eye diagram и 6×2 сеткой
  кнопок для ALPHA / K (LPF bandwidth) / squelch
* **Menu-оверлей** с PATH cycle (A / B / HYB / HYB+NN), тоглами
  NOTCH / VIT, циклом DISP (waterfall / spectrum / scope)
* **DIAG-экран** с гистограммой символов и zero-bias meter
* **Touch calibration** с 4-corner оверлеем
* **Factory-reset диалог** с явным подтверждением YES/NO
* **Красная `*` для [ERR]** на экране (полный токен в serial)
* **PIO-driven ILI9488** на 60 МГц с DMA-водопадом — ~20 FPS

### Настоящая serial-CLI

То, что в v1.72 было рудиментарным, теперь полноценный CLI:

* Живая настройка: `ALPHA`, `BW`, `SQ`, `FREQ`
* Протокол: `BAUD`, `SHIFT`, `STOP`, `INV` (каждый принимает AUTO)
* Тоглы: `AFC`, `AGC`, `SCALE`, `NN`, `NOTCH`, `VIT`, `NR`
* Путь декодера: `PATH A / B / HYB`, `DYN ON/OFF`, `WEIGHTS`
* Сохранение: `SAVE`, `CLEAR`, `STATUS`, `VERSION`
* Диагностика: `DIAG ON/OFF`, `DUMP SPEC`, `DUMP MS`, `DUMP FRAMES`
* Help: `HELP`, `SEARCH`

Полный reference: [`docs/SERIAL_COMMANDS.ru.md`](docs/SERIAL_COMMANDS.ru.md).

### Воспроизводимый bench и training-тулинг

`tools/` везёт полный Python-комплект для:

* Генерации синтетических RTTY-WAV с заданным SNR (`rtty_gen.py`,
  `sweep_runner.py`, browser-side `rtty_simulator.html`)
* Проигрывания записанного аудио через железо с capture serial-decode
  (`bench_replay.py`)
* AWGN-sweep бенчмарков с NN-OFF vs NN-ON сравнением
  (`nn_sweep_compare.py`)
* Multi-seed усреднения (`aggregate_compare.py`) — single-run бенчи
  на низком SNR достаточно шумные, чтобы production-решение требовало
  ≥ 3 сидов
* CER-анализа (`cer_analyze.py`)
* PyTorch NN-тренировки (`train_nn_torch.py`) с v13 production-рецептом
  одним флагом
* Парсинга B265 `DUMP FRAMES` лога → numpy npz (`parse_dump_frames.py`)
* Overnight-цепочки (`overnight_runner.sh`) для unattended-evaluation
  нескольких рецептов

Полное decision tree: [`docs/BENCH_TOOLING.ru.md`](docs/BENCH_TOOLING.ru.md).

### Документация

Шесть длинных гайдов в `docs/`:

* [`HARDWARE_SETUP.md`](docs/HARDWARE_SETUP.md) — GPIO-распиновка,
  bias-сеть, сборка/прошивка, troubleshooting
* [`SERIAL_COMMANDS.md`](docs/SERIAL_COMMANDS.md) — полный CLI-reference
  с примерами
* [`MENU_GUIDE.md`](docs/MENU_GUIDE.md) — walkthrough по тачскрин-UI
* [`NN_TRAINING.md`](docs/NN_TRAINING.md) — production-рецепт v13 +
  ledger негативных результатов с объяснением, что пробовал и что
  не зашло
* [`BENCH_TOOLING.md`](docs/BENCH_TOOLING.md) — decision tree
  bench-скриптов и workflow
* Плюс исторические Phase 9 design docs и lessons-learned заметки

---

## Breaking changes

* **RP2040 больше не поддерживается.** Этот релиз требует
  **Raspberry Pi Pico 2 (RP2350)**. Прошивка не влезает в SRAM
  RP2040, а DSP нужен FPU от M33. Если на RP2040 у тебя крутится
  v1.72, не шей этот релиз.
* Serial-протокол в основном additive (старые команды работают), но
  часть дефолтов поменялась (`PATH HYB+NN` — рекомендация, было
  просто `A`).
* Часть ранне-фазовых planning документов
  (`docs/PHASE1..PHASE7_*.md`) удалены из корня `docs/`. Они живут
  в git history и заменены реализацией Phase 9.

---

## Как прошить

```bash
picotool load -f TouchRTTY_v2.0.0.uf2
```

Или зажми BOOTSEL при подключении Pico, drag-and-drop `.uf2` на
mass-storage диск `RPI-RP2`.

После прошивки шёл `VERSION` по serial для подтверждения:

```
>> TouchRTTY Phase9 B265 (built May 12 2026 ...)
```

---

## Сборка из исходников

```bash
git clone --recurse-submodules https://github.com/Alex-Electron/TouchRTTY.git
cd TouchRTTY
mkdir build && cd build
cmake -G Ninja -DPICO_SDK_PATH=/path/to/pico-sdk ..
ninja
picotool load -f TouchRTTY.uf2
```

Требует Pico SDK 2.x и ARM-тулчейн.

---

## Roadmap

Запас по NN ещё есть — ledger негативных результатов в
[`docs/NN_TRAINING.ru.md`](docs/NN_TRAINING.ru.md) документирует
10+ рецептов, что не зашли, а самое перспективное оставшееся
направление — построение trusted-oracle pipeline для размеchki
uncertain real-air фреймов (DWD template matching). Real-air
augmentation сегодня ограничена, потому что hard-decision labels не
могут научить NN обыгрывать hard decision.

Помимо NN — исторический roadmap (SD-карта, CW, FT8/FT4, WEFAX)
эскизирован в
[`docs/ROADMAP_OPTIMIZATION.md`](docs/ROADMAP_OPTIMIZATION.md).

---

## Спасибо

* Pico SDK © Raspberry Pi (BSD-3-Clause)
* [LovyanGFX](https://github.com/lovyan03/LovyanGFX) © lovyan03 (FreeBSD)
* 2Tone от G3YYD — упоминается только как бенчмарк, не редистрибьютим
* Метеослужба DWD — за круглосуточный надёжный источник тестового
  сигнала
