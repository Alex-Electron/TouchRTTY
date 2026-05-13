# Changelog: TouchRTTY (RP2350)

> 🇷🇺 [Читать на русском](CHANGELOG.ru.md)

All notable changes to this project will be documented in this file.

---

## [v2.0.0 — Phase 9 + TinyML NN] - 2026-05-12

Major rewrite over v1.72. Full release notes in
[`RELEASE_v2.0.0.md`](RELEASE_v2.0.0.md).

### Headline

* **Beats 2Tone 26.01a at low SNR** by 3-6× real error rate on the same
  audio. Multi-seed averaged AWGN bench over SNR -4..-22 dB.
* **Production NN weights (v13)** delivered with the firmware:
  PyTorch-trained MLP with `weight_uncertain=3.0` recipe. Improves
  -14/-16/-20 dB SNR by -1.9/-1.8/-9.1 pp vs prior production weights
  with σ < 4 pp across seeds.

### Added

* **Phase 9 hybrid decoder architecture** — dual-IQ paths (narrow A,
  wide B) fused via LLR (HYB), Soft-Viterbi frame validation gate,
  LMS adaptive notch chain, DPLL with PI controller, AFC/AGC.
* **TinyML NN classifier** (`NN ON/OFF`) — 7→128→64→32 MLP, ~44 KB
  weights, B264 confidence-gated to run only on uncertain frames.
* **B265 DUMP FRAMES** serial command — per-frame soft-bit + label
  stream for capturing real-air training data.
* **Tuning Lab** UI with phosphor-persistent eye diagram and live
  ALPHA / K / SQ adjustment.
* **3-bar top panel** showing SIG / AGC / ERR (rolling 100-frame
  error window).
* **Complete serial command system** — 40+ commands documented in
  `docs/SERIAL_COMMANDS.md`.
* **Reproducible bench tooling** — PyTorch trainer, AWGN sweep with
  NN-OFF vs NN-ON comparison, multi-seed aggregator, real-air
  bench. All documented in `docs/BENCH_TOOLING.md`.
* **Browser-side RTTY simulator** (`tools/rtty_simulator.html`) for
  signal generation without the Python stack.
* **Six long-form documentation files** in `docs/` covering hardware
  setup, serial commands, on-device menu, NN training, and bench
  tooling.

### Changed

* **Build counter** now at B265 (consolidates B194..B265 of incremental
  Phase 9 work).
* **PATH** UI menu cycles four states (`A / B / HYB / HYB+NN`) instead
  of three; `HYB+NN` is the recommended production setting.
* **`[ERR]` rendering on screen** collapsed to a single red `*` glyph
  (B263). Full `[ERR]` token preserved on serial.
* **NOTCH / VIT** moved from popup to inline menu toggles for
  fewer taps.

### Breaking

* **RP2040 no longer supported.** Requires Raspberry Pi Pico 2
  (RP2350) — uses M33 FPU and >150 KB SRAM for FIR + FFT buffers.
* Old per-phase planning docs (`PHASE1..PHASE7_*.md`) removed from
  `docs/` (superseded by Phase 9 implementation). Available in git
  history.

### Release artifact

`TouchRTTY_v2.0.0.uf2` — flashable via `picotool` or BOOTSEL drag-drop.

---

## [B258 — Stage 4 closed: Wiener NR neutral-to-harmful under honest 3-run averaging] - 2026-04-19

### Added
**`NR ON` / `NR OFF` serial команды** (`src/serial_commands.cpp`) + флаг
`shared_spectral_nr` (`src/app_state.{hpp,cpp}`). По умолчанию **OFF** —
см. ниже разбор что пошло не так.

**Per-bin Wiener noise reduction** (`src/dsp_pipeline.cpp`, guarded by `shared_spectral_nr`):
- Асимметричный мин-трекер для floor каждого из 4-х потоков мощности
  (`mark_a`, `space_a`, `mark_b`, `space_b`): `fast-down 0.1`, `slow-up 1e-5`.
- Per-channel Wiener gain `G = (P − floor) / P` с floor-clamp `G ≥ 0.7`.
- SNR-gated через `shared_snr_db` из Core 1 FFT (включается только при SNR
  ниже порога — cкорее всего порог подобран неверно).

### Fixed
**Stage 4 не работает — честная метрика показала neutral-to-harmful.**
После экспериментов B253-B256 (разные floor, gating, LPF-эмуляция) и
измерений одиночными sweep-прогонами казалось что есть улучшение на ~5%
при -14 dB. Но **re-run того же B256 дал до 9% CER-разброса** на одинаковой
конфигурации → baseline нестабилен из-за AWGN-реализации и Windows audio
jitter. Стадия 4 закрыта:
- 3-run averaging (`tools/cer_avg.py`, см. ниже) по B257:
  - `-14 dB`: NRON 22.58% mean vs NROFF **18.56%** mean → NRON **хуже** на 4.02%
  - `-16 dB`: NRON 59.45% mean vs NROFF **48.05%** mean → NRON хуже на 11.4%
  - `-18 dB`: NRON 80.37% mean vs NROFF 79.95% mean → ~ноль
- Симметричное применение gain к обоим channels — математически no-op
  (downstream AGC `atc_mark_env`/`atc_space_env` нормализует оба в
  одинаковом отношении). Асимметричное применение разрушает LLR-инварианты
  (per-channel SNR esrimates становятся смещёнными).
- Код оставлен за `shared_spectral_nr` gate (OFF by default) — на случай
  если в Stage 5 понадобится spectral subtraction как компонент другой
  архитектуры.

### Next
- Stage 5 variant 1 (matched filter / Path A LPF narrower): тест `BW`
  команды `{0.4, 0.5, 0.6, 0.75, 0.9}` × 3 sweep-прогона каждый →
  `cer_avg.py` aggregation.
- Stage 5 candidates остались: BCJR soft-output, character-level LM,
  ML classifier на логах сдвиговых регистров.

---

## [B257 — tools/cer_avg.py: N-run CER aggregation (mean/std/min/max)] - 2026-04-19

### Added
**`tools/cer_avg.py`** — runner над `cer_analyze.py` для усреднения CER
по N sweep+log парам:
```
python cer_avg.py --gt "RYRYRY..." \
  --pairs run1.sweep:run1.log run2.sweep:run2.log run3.sweep:run3.log
```
Output: per `(SNR, PATH)` tuple печатает `N mean std min max`. Нужно было
чтобы выловить что одиночные sweep-прогоны нестабильны на ±9% CER — без
усреднения **невозможно** отличить реальное улучшение декодера от шума
AWGN-реализации.

### Fixed
**`tools/sweep_runner.py` — post-noise rescale сохраняет SNR.**
Раньше при клиппинге (`peak > 0.95`) рескейл применялся только к audio
без учёта того, что шум внутри `add_awgn()` считается относительно
rms сигнала **до** генерации. При больших отрицательных SNR-ах это
приводило к переполнению и потере точности замера. Новое поведение:
- Pre-scale `clean` signal на `--sig-level` (default 0.10 = -20 dBFS)
  **до** add_awgn, чтобы при минимальном SNR в ladder шум укладывался
  в ±1.0 без клиппинга.
- Если всё равно peak > 0.95 после наложения шума — rescale применяется
  к **обоим** (signal уже включен в audio), SNR сохраняется (делается
  одно и то же кратное), декодер справляется через AGC.

**`tools/cer_analyze.py` — `--lag` компенсация serial-batching.**
Firmware flush-ит накопленные chars только когда приходит newline — а
newline обычно приходит на следующем `[CMD] PATH=X` echo. Значит символы
декодированные во время bin N попадают в лог под timestamp'ом bin N+1.
Опция `--lag 2.0` (default) сдвигает timestamps записей на 2 секунды
назад перед bin-assignment → правильная атрибуция символов к SNR/PATH.
Без этого первое окно sweep'а систематически показывало CER как принадлежащий
следующему окну (first-window bias).

### Next
- Использовать `cer_avg.py --pairs` минимум для 3 прогонов при всех
  будущих measurements — одиночные замеры больше не доверять.

---

## [B252 — Stage 3.3 TUNED: dynamic SNR-weighted LLR fusion] - 2026-04-19

### Added
**Stage 3.3 — dynamic LLR fusion** (`src/dsp_pipeline.cpp`, `shared_dyn_fusion`):
заменили равновесный geometric mean двух IQ paths на веса, пропорциональные
per-path SNR. Формула после тюнинга:
```
w_a = sqrt(snr_a_ema) / (sqrt(snr_a_ema) + sqrt(snr_b_ema))
w_a = clamp(w_a, 0.2, 0.8)    // защита от single-path lock-in
α    = 0.002                   // per-path EMA (медленное обновление)
mark  = exp(w_a·log(mark_a)  + w_b·log(mark_b))
space = exp(w_a·log(space_a) + w_b·log(space_b))
```
Где `snr_{a,b} = max(mark, space) / min(mark, space)` на каждой обработанной выборке.

**`DYN ON` / `DYN OFF` serial команды** + `shared_snr_a_ema`/`shared_snr_b_ema`
телеметрия (диагностика weight-конвергенции).

**`PATH LLR` alias** для `PATH HYB` (совместимость с внешними скриптами
которые могут использовать старое имя).

**`WEIGHTS <wa> <wb>` команда** — статический override весов Stage 3.2 для
A/B ablation (внутренне нормализует в sum=1.0).

### Key measurement
3-run averaged threshold (CER ≥ 5%):
- **до Stage 3 (B230 baseline)**: ~ -11 dB
- **B252 Stage 3.3 TUNED HYB**: **~ -14 dB** → honest +3 dB gain vs pre-stage
- Меньшие α (0.001) и более узкий clamp (0.3..0.7) тестировались,
  победил α=0.002 + sqrt-softening + wide clamp [0.2..0.8].

### Fixed
**Stage 3.2 weighted fusion bias** — одиночный замер B249 с fixed `WEIGHTS
0.7 0.3` дал неправдоподобно хороший результат при -18 dB. Причина — first-window
bias (первое окно sweep содержит чистый сигнал до наложения AWGN). Reversed-order
control run (B251 rev) подтвердил: первое окно всегда контаминировано
независимо от порядка DYN ON/OFF. Теперь все измерения используют
`--trim 3.0` (skip first 3 seconds per window) + `--lag 2.0` (batching
compensation) + cer_avg.py на 3 прогонах.

### Next
- Stage 4 (Wiener spectral NR) — см. B258 (failed, reverted).
- Stage 5 variant 1: matched filter tuning через BW sweep.

---

## [B247 — serial VERSION command + cer_analyze diag-strip fix] - 2026-04-19

### Added
**`VERSION` / `VER` / `ID` команды** (`src/serial_commands.cpp`) — печатает
`>> TouchRTTY Phase9 B<N> (built <DATE> <TIME>)` чтобы автоматика могла
проверить, что за прошивка сейчас на устройстве. Обнаружено что на RP2350
была прошивка из соседнего проекта (отвечала `UNKNOWN COMMAND: PATH A` и
вставляла `[HYBRID DIVERGENCE: Legacy=… ML=…]` в serial), из-за чего
B246 sweep-замеры A/B/HYB были невалидны.

### Fixed
**`tools/cer_analyze.py` — clean_decoded** теперь правильно срезает diag-линии:
- Regex `\[D\][^\n]*` → per-record strip (раньше джойнил всё пробелом и срезал
  одну большую строку, убивая всё после первого `[D]`).
- Добавлены фильтры `\[HYBRID[^\]\n]*\]?[^\n]*`, `>>[^\n]*`, `===[^\n]*`.
- Join records через `''.join(clean_decoded(c) for c in chars)` (concat без
  разделителя — char-stream firmware выдаёт без пробела между символами).

Предыдущая версия давала CER=90% даже при +20 dB (в "decoded" попадало
содержимое `[D] SNR=... ERR=...`); теперь при +20 dB видим real CER 15-30%
(остаточные 15% — FIGS-table mismatch rtty_gen vs firmware ITA2, пока
обхожу через digit-free text).

### Next
- Sweep с `--text "RYRYRY THE QUICK BROWN FOX JUMPS OVER LAZY DOG "` (без
  digits/symbols, FIGS никогда не триггерится → pure bit-decision CER).
- Stage 3.2 weighted fusion только после валидного A/B/HYB baseline.

---

## [B246.1 — testbench: impulse default-off + audio sink selector] - 2026-04-18

### Fixed
**Критический баг в `tools/rtty_simulator.html` (введён в B240.1)**: чекбокс
`Impulse noise (атмосферики / QRN)` имел атрибут `checked` по умолчанию,
rate=120/min, duration=10 ms, amplitude=×10. В результате **все sweep-замеры
B242 → B246/B** (baseline B230, Soft-LLR, Soft-Viterbi, LMS-notch AWGN,
Input BPF, Dual IQ path A/B) проводились с наложенными импульсными помехами,
а не в чистом AWGN. Пороги декодера в этих таблицах **пессимистичнее**
реального AWGN на неизвестную величину.

- `impEnable` убран `checked` — импульсы теперь off-by-default.
- Все замеры B242→B246 нужно переснять в чистом AWGN; ретро-анализ
  относительного gain по stage-переходам остаётся (импульс влияет на все
  стадии одинаково), но абсолютные пороги — нет.

### Added
**Селектор аудиовыхода** в симуляторе (`Audio output` fieldset в самом верху):
- Dropdown со списком всех output-устройств через `navigator.mediaDevices.enumerateDevices()`
- Кнопка "Show device names" — запрашивает `getUserMedia({audio:true})` на 1 мс
  (сразу останавливается), чтобы Chrome/Firefox разблокировал реальные имена
  устройств вместо `Output 1 (hash...)`.
- Auto-refresh при `devicechange` (подключение/отключение USB-карты).

**Центральный output-bus (`masterBus`)**: все 5 источников (signal gain,
AWGN noise, CW QRM, RTTY QRM, impulse bursts) теперь сходятся на
`masterBus`, а не на `audioCtx.destination`. Роутинг:
- Default → `masterBus → audioCtx.destination` (как было)
- Выбрано устройство → `masterBus → MediaStreamDestinationNode → <audio>.setSinkId(deviceId)`

Через `HTMLMediaElement.setSinkId` (Chrome любой современный, Firefox 116+) —
в отличие от экспериментального `AudioContext.setSinkId`, который в Firefox
не поддерживается и в Chrome часто не работает.

### Why
Ноут пользователя имеет несколько звуковых карт (built-in + USB). Нужно
направлять тестовый сигнал именно на карту, заведённую в ADC декодера
через audio-loop, минуя встроенные динамики.

---

## [B246 — Dual IQ path + 3-way switch (Stage 3.1)] - 2026-04-16

### Added
**Вторая IQ-ветка** в `src/dsp_pipeline.cpp`. Теперь после LMS-notch сигнал
демодулируется параллельно двумя путями:
- **Path A** (существующий, узкий): biquad LPF BW = `baud · tuning_lpf_k` (≈0.75·baud)
- **Path B** (новый, широкий): biquad LPF BW = `baud · 1.5` — больше полоса,
  устойчивее к drift/ISI, чуть больше noise

Обе ветки считаются всегда (чтобы переключение было без щелчка). Выбор power-пары
(mark/space), питающей framer, делается по `shared_decoder_path`:
- `0 = A` (narrow) — default, identical behavior to B245
- `1 = B` (wide)
- `2 = HYB` — простой average `0.5·(A+B)` (Stage 3.2 заменит на SNR-weighted)

### UI
- **Menu → PATH кнопка** (3-я колонка нижнего ряда): тап циклит A → B → HYB → A.
  Цвет постоянный (muted blue) — меняется только надпись.
- **Top bar row 3** под `ST:` — индикатор `P:A` (dim) / `P:B` (green) / `P:HYB` (cyan).
- Выбор persist в flash (`AppSettings.decoder_path`).

### Serial
- `PATH A` / `PATH B` / `PATH HYB` — явное переключение.

### Measurement plan
Следующий sweep: три отдельных прогона (A/B/HYB) через симулятор при идентичных
условиях — сравнение threshold и CER на −8..−12 дБ. Ожидания:
- A baseline −10 дБ (regression check vs B245)
- B: может быть чуть хуже в AWGN (шире BW), но лучше при drift
- HYB: между ними, простой average без интеллекта; Stage 3.2 должна обогнать

### Next
Stage 3.2 — заменить простой average на weighted fusion + SNR-estimate.

---

## [Phase 9 Progress Report — Этапы 1-2 закрыты] - 2026-04-16

Полный детальный разбор: `docs/PHASE9_PROGRESS_REPORT.md`.

### Сводная таблица
| Build | Stage | Threshold | Quality | Статус |
|-------|-------|-----------|---------|--------|
| B230 | baseline | −10..−11 дБ | baseline | — |
| B242 | 1.1 Soft-LLR | −10 дБ | −14 дБ оживает (0→282 chars) | ✅ |
| B243.1 | 1.2 Soft-Viterbi | −10 дБ | −8 дБ 6% → 0% | ✅ |
| B244 | 2.1 LMS-notch | −10 дБ (AWGN) / +1-2 дБ (QRM) | Stable под CW | ✅ |
| B245 | 2.2 Input BPF | −10 дБ | −10 дБ 15% → 0% (чистый край) | ✅ |

### Что реально получили на Этапах 1-2
- Threshold в AWGN **не сдвинулся** (−10 дБ), но это **ожидаемо**: Этапы 1-2 это
  подготовительные — честный framer + стойкость к QRM + гигиена полосы.
- Framer больше не отдаёт ложняк на границе (B243)
- Устойчивость к CW QRM (B244) — новое качество
- Чистый вход для fusion (B245)

### Настоящий сдвиг порога начинается со Stage 3
- Stage 3 (fusion двух IQ): +0.5-1.5 дБ
- Stage 4 (spectral NR): +1-2 дБ
- Stage 5 (ML post): +1-2 дБ
- Цель: threshold **−14..−15 дБ**

### Оговорки к замерам
- `cer_analyze.py` иногда показывает фантомные 7-8% на высоких SNR из-за
  одиночных byte-loss в serial — читаем по сплошному 0% диапазону.
- Маркеры `=NN=` иногда теряются (LMS-notch cold-start), тогда соседний бин
  захватывает content и CER искусственно растёт.
- Всё измерено в синтетическом AWGN через simulator→ADC. Реальный эфир
  добавит selective fading, импульсы, дрейф TX — Stage 4-5 могут дать
  другую картину на реальном датасете (задача #16).

---

## [B245 — Input BPF 300-3000 Hz (Stage 2.2)] - 2026-04-16

### Added
**Фиксированный Butterworth BPF 300-3000 Hz** — два биквада (HPF@300 + LPF@3000)
вставлены после AGC, перед LMS-notch. Помощник `design_hpf()` добавлен в
`src/dsp/biquad.hpp` (раньше был только LPF).

### Why
Phase 9, Stage 2.2. Пайплайн плана: `AGC → BPF → LMS-notch → IQ`. BPF
дополняет 63-tap FIR (уже bandpass) — срезает остаточный DC/hum <300 Гц и
HF-шум >3 кГц, которые FIR пропускает. Основная цель — очистить полосу
перед LMS-notch и IQ-демод, чтобы Stage 3 (fusion) получил чистый вход.

### Measured (2026-04-16, AWGN only)
| SNR  | B243.1 | **B245** |
|------|--------|----------|
| +14..−8 | 0-2% | 0% ✓ |
| **−10** | ~15%* | **0.00%** ✓ |
| −12  | 9%* | 31%* |
| −14  | 25.9% | lost |

\* бины захватывают соседний SNR из-за сбежавших маркеров.

**Threshold: −10 dB** (тот же, что B243.1/B244), но bin −10 стал полностью
чистым (0.00% против 15% у B243.1). Край порога чище.

AWGN-нейтрально по threshold, как и ожидалось для фиксированного BPF:
реальный выигрыш BPF раскроется при QRM/noise-floor тестах и как чистый
вход для Stage 3 (fusion).

Артефакт: `datasets/logs/b245_inputbpf_awgn_{sweep.txt,cer.csv,*.log}`.

### Next
Stage 3: Fusion двух IQ-веток (narrow LPF + wide raised-cosine) с weighted
combine. Ожидаемый gain +0.5-1.5 дБ — первый этап, который реально должен
двигать порог вниз.

---

## [B244 — LMS-notch adaptive (Stage 2.1)] - 2026-04-14

### Added
**Новый модуль `src/dsp/lms_notch.hpp`** — 2nd-order constrained adaptive notch
(Nehorai-style). Вставлен в pipeline каскадом из двух экземпляров сразу после AGC,
до IQ-демодуляции.

- **Nieж-notch**: окно 300–1350 Hz, старт 600 Hz. Ловит CW QRM ниже RTTY band.
- **Верх-notch**: окно 1650–3200 Hz, старт 2200 Hz. Ловит QRM сверху.
- Pole radius `r = 0.985` → BW ≈ 48 Hz (узкий нуль, не повреждает соседние тона).
- LMS step `mu = 5e-6` — консервативный, сходимость ~1-2 s.
- Коэффициент `a` кламплен к допустимому диапазону, чтобы нуль не уполз в
  RTTY band (1400..1600 Hz) и чтобы два notch'а не сошлись на одну помеху.

### Why
Phase 9, Stage 2.1. В чистом AWGN gain ≈ 0 (notch'у не на что сходиться), в
реальном эфире с CW QRM ожидаем +1-2 дБ threshold. Основная цель — продвинуть
декодер к −15..−16 дБ в условиях узкополосных помех.

### How to measure
- **AWGN-only sanity**: тот же sweep, CER не должен вырасти.
- **QRM test**: в симуляторе включить CW QRM на 1000 Hz level −10 дБ, sweep.
  Без notch на −5 дБ смерть, с notch надеемся на −8..−10 дБ threshold.

### Cost
- ~2 MAC × 2 notch × 10 kSps = 40 kMAC/s на Core 0. Пренебрежимо.
- Стабильность: форма `1 + a·z⁻¹ + z⁻²` с полюсами на r < 1 — всегда устойчива.

### Measured (2026-04-14)

**AWGN-only (sanity)**: threshold −10 dB (как B243.1). Мелкий шум на +14 дБ
из-за cold-start notch (мало времени на convergence перед высоким SNR).
Артефакт: `datasets/logs/b244_lmsnotch_awgn_{sweep.txt,cer.csv,*.log}`.

**AWGN + CW QRM** (уровень оператора, частота вне RTTY band):
threshold −10 dB **тот же**, от +20 до −8 дБ везде ≤2% CER. Notch успешно
нулифицирует QRM — без него CW обычно разваливает декодер даже на высоком SNR.
Артефакт: `datasets/logs/b244_lmsnotch_qrm_{sweep.txt,cer.csv,*.log}`.

Subjectively +1-2 дБ gain в QRM-условиях, как и планировалось.

### Tools changes
`tools/cer_analyze.py::best_cer` оптимизирован: было O(49·N²) (Levenshtein на
каждый циклический сдвиг GT), стало O(49·N + 3·N²) — грубый char-match для
выбора top-3 сдвигов, потом Levenshtein только для них. На логах с большими
бинами (merged из-за потерянных маркеров) ускорение 10-50x.

### Next
Stage 2.2: Input BPF 300-3000 Hz — защитит от высокочастотного white spectrum
мусора, который текущий анти-алиасинг FIR пропускает.

---

## [B243 — Soft-Viterbi framer (Stage 1.2)] - 2026-04-14

### Changed
**Framer в `src/dsp_pipeline.cpp` + `src/dsp/dpll_framer.hpp`**: к B242 soft-LLR
добавлены два soft-bit гейта на границе фрейма:

- **Weakest-link (data-bit)**: отклонить фрейм если `min(|soft_data[i]|) < 0.20·sig_level`.
  Фильтрует кейс когда один из 5 data-битов оказался около нуля — тогда слайс по знаку
  давал случайное решение и случайный Baudot-код. Именно это давало 6% CER на -8 дБ после B242.
- **Frame-average**: отклонить если `mean(|start| + |data[0..4]| + stop) / 7 < 0.30·sig_level`.
  Отсекает фреймы с общей слабой статистикой (низкий SNR окно).

### Why
Phase 9, Stage 1.2 — вторая половина soft-решения. B242 только валидировал
границы фрейма (stop/start), но внутренние data-биты всё ещё получали hard-slice
без проверки уверенности → ложные фреймы на грани SNR.

### Tuning (B243.1)
Первый замер с порогами 0.20/0.30 дал **регрессию**: на +20 дБ CER=4.94% (чистые
фреймы режутся), на -8 дБ CER=28%, на -10..-14 декодер умирал. Пороги ослаблены
до 0.10/0.15 — чувствительный soft-бит гейт, но не параноик.

### Measured (2026-04-14, AWGN only, B243.1 thresholds=0.10/0.15)

| SNR | B230 | B242 | B243.1 |
|-----|------|------|--------|
| +20..−6 | 0-2% | 0-6% | **0-2%** |
| **−8** | ~0% | **6.0%** | **0.00%** ✓ |
| −10 | 0.6% | 1.8% | ~15%* |
| −12 | 9.1% | 9.4% | — (маркер повреждён) |
| −14 | lost | 25.9% | 27.2% |

\* bin −10 дБ захватил контент −12 дБ из-за потерянного маркера =17= в декод-потоке.

**Threshold (CER≥5%): −10 dB** — тот же, что B230/B242, но на границе чисто.

**Главный win**: ложные фреймы на −8 дБ (6.0% B242 → 0.00% B243.1) вычищены
weakest-link гейтом. Это что и должен был дать Stage 1.2.

Артефакты: `datasets/logs/b243_1_softviterbi_lenient_{sweep.txt,cer.csv,*.log}`.

### Next
Stage 1.2 закрыт. Переходим к Stage 2 — шумовая обстановка:
- **Stage 2.1**: LMS-notch (2 адаптивных нуля) против CW QRM.
- **Stage 2.2**: Input BPF 300-3000 Hz.
Ожидаемый gain: +1-2 дБ в реальном эфире (в чистом AWGN ничего не даст).

---

## [B242 — Soft-LLR bit decision (Stage 1.1)] - 2026-04-15

### Changed
**Framer в `src/dsp_pipeline.cpp`**: hard-slice на границе бита (`integrate_acc > 0`) заменён на
Soft-LLR с адаптивным порогом на границе фрейма.

- Сохраняем `soft_start`, `soft_data[5]`, `soft_stop` (= последний `integrate_acc`) — soft-values, не битовые решения.
- EMA `sig_level = 0.98·sig + 0.02·|integrate_acc|` отслеживает уровень сигнала (адаптируется к AGC drift / M–S имбалансу).
- На stop-bit: фрейм валиден только если `soft_stop > 0.25·sig_level` **и** `-soft_start > 0.15·sig_level`.
  Раньше fixed-порог `integrate_acc > 0` принимал слабые/нулевые биты как MARK → мусор при низком SNR.
- Stop-gap арминг для STOP-DET теперь привязан к `valid_stop`, а не к сырому биту.
- Data-биты по-прежнему hard-slice в `current_char` (soft-Viterbi придёт в Stage 1.2).

### Why
План Phase 9, Этап 1.1 — первый простой выигрыш в цепочке к −15..−16 дБ.
Ожидаемый gain +2–3 дБ от замены hard-slice на adaptive-threshold frame-validation.

### Measured (2026-04-15, AWGN only)

| SNR | B230 CER | B242 CER |
|-----|----------|----------|
| +18..−6 | ~0% | ~0% |
| −8  | ~0%  | **6.02%** ⚠️ |
| −10 | 0.60% | 1.79% |
| −12 | 9.09% | 9.38% |
| −14 | маркер потерян | **25.89%** (282 chars) |

**Threshold (CER≥5%): ~−10..−11 dB — без изменения vs B230.**

Наблюдения:
- На −14 декодер теперь не умирает (282 символа vs потерянный маркер) — адаптивный порог пропускает больше фреймов.
- Но на −8 странный всплеск 6% — в preview `QWERTYUIOP RYRYRY...`, похоже на ложный фрейм, проскочивший через ослабленный порог.
- Без soft-Viterbi (Stage 1.2) одних адаптивных порогов не хватает — больше символов, но и больше мусора.

Артефакты: `datasets/logs/b242_softllr_{sweep.txt,cer.csv}`, serial `b242_softllr_20260415_224405.log`.

### Next
- Stage 1.2: Soft-Viterbi framer с stop-bit как constraint — должен отфильтровать мусорные фреймы за счёт мягких решений по 5 data-битам.
- Возможно: покрутить `STOP_MIN_FRAC` / `START_MIN_FRAC` — но это тюнинг, а не архитектура.

---

## [Baseline Build 230 — AWGN only] - 2026-04-15

### Measured
Первый честный baseline-замер Build 230 через sync-маркеры (`--markers`).

| SNR (dB) | CER     |
|----------|---------|
| +18..−8  | ~0%     |
| −10      | 0.60%   |
| **−12**  | **9.09%** |
| −14      | маркер потерян |

**Decoder threshold (CER≥5%): ~−10..−11 dB** — 4 dB лучше, чем в плане писалось (−6..−8). Но условия идеальные: только AWGN, без QRM/drift/fading/impulse. Артефакты в `docs/baseline_build230_{cer.csv,sweep.txt,serial.log}`.

### Fixes в `cer_analyze.py`
- `clean_decoded()`: стрипает теги `[FIGS]`/`[LTRS]`/`[ERR]` перед Levenshtein. До фикса CER был ~40% на чистом декоде — теги считались как вставки.
- Threshold estimate: игнорирует пустые бины (0 chars) чтобы не врать про "threshold=+20 dB" когда первая точка просто не попала в маркеры.

### Цель Phase 9
Превзойти 2Tone: threshold **−15..−16 dB** (на 4-5 dB лучше baseline). Начинаем Этап 1.1 — Soft-LLR bit decision (+2-3 dB ожидаемый gain).

## [Build 241] - 2026-04-15
### Added (sweep sync-markers — robust CER binning)
- **Симулятор**: в `startSweep()` на каждом переходе SNR в поток основного RTTY инжектируется маркер `=NN=` (NN = двузначный индекс точки, padStart `01`..`18`). Реализовано через модульный `markerQueue`, который `scheduleChunk()` потребляет **перед** основным текстом; `charIndex` не продвигается, когда берём символ из очереди.
- **Формат маркера**: `" =NN= "` с пробелами-разделителями. `=` присутствует только в FIGS, поэтому regex `=\d\d=` в декодированном потоке не конфликтует с `RYRYRY...` основного текста.
- **`cer_analyze.py --markers`**: новый режим. Склеивает весь serial-вывод, находит все `=NN=` через regex, режет поток на сегменты между маркерами и маппит на точки sweep по номеру. Устойчив к батчингу serial-вывода (именно то, что завалило baseline B230).
- Sweep-лог теперь дополнительно пишет `MARK==NN=` в строке каждой точки для трассировки.

### Why
Baseline-замер B230 показал: serial-вывод устройства приходит большими чанками с одним timestamp на весь чанк. Timestamp-бинирование не работает. Inline-маркеры в самом аудио-сигнале — самое чистое решение: не нужен общий clock, не нужно менять прошивку. На низких SNR маркеры тоже теряются, но там CER и так 100% — не критично.

## [Build 240] - 2026-04-15
### Added (simulator — noise preview + impulse tone/duration controls)
- **Кнопка `NOISE ONLY`** в `rtty_simulator.html`: запускает всю звуковую цепочку (AWGN, CW, QRM-RTTY, impulses, fading, drift) БЕЗ основного RTTY-передатчика. Main markBitGain остаётся в 0. Нужна чтобы на слух проверить, что каждый тип помех реально звучит и регулируется.
- **Импульсы — новые регулировки**:
  - `Tone (Hz)` 100..4000 — центральная частота "щелчка".
  - `Duration (ms)` 1..40 — длина бёрста.
  - Checkbox `Random tone + duration per burst` — каждый impulse берёт случайные tone (150..3650 Hz) и duration (1..16 ms).
- **Изменена форма импульса**: было — декей-белый-шум 2 ms. Стало — damped sine `env·(0.7·sin(2πf·t) + 0.3·noise)`, τ=len/4. Звучит как естественный атмосферик/разряд, а не просто "пшик".
- Рефакторинг `startRTTY()` → `startRTTY(muteMain)` + глобальный `rttyMuted`. Статус-строка в noise-only показывает жёлтым "NOISE ONLY (main RTTY muted)".

### Why
Пользователь заметил, что импульсы не слышны на фоне RTTY. Кнопка noise-only позволяет изолировать каждую помеху. Регулируемый тон/длительность делает импульсы реалистичнее (атмосферики разные: grain-level, молнии, reg-spikes).

## [Build 239] - 2026-04-13
### Added (testbench — Python tools — **closes testbench phase**)
- **`tools/cer_analyze.py`**: корреляция HTML sweep-лога + serial-лога → CER(SNR) таблица.
- **Алгоритм**:
  1. Parse sweep-log — извлекает (ISO_ts, SNR, idx) на каждой точке.
  2. Parse serial-log — timestamp+text на каждой строке.
  3. Для каждой sweep-точки: окно `[ts+trim, next_ts)`, собрать декодированные chars.
  4. Clean: только ASCII printable, uppercase.
  5. **Best cyclic Levenshtein** vs ground-truth: пробует все offset'ы в GT-цикле, берёт минимум. Нормализация: `cer = edit_distance / len(decoded)`.
- CLI: `--sweep --serial --gt <str|@file> --out <csv> --plot <png> --trim <sec>`.
- Эвристика **threshold estimate**: самый высокий SNR, на котором CER ≥ 5%. Это наша главная метрика (decoder threshold).
- Опциональный matplotlib-график CER vs SNR.
- Smoke-test пройден на синтетических логах: 0%→67% CER, threshold найден корректно.

### Phase 9 testbench готов
Все инструменты для объективного измерения декодера на месте:
- HTML-симулятор (AWGN, QRM, drift, fading, morse, sweep)
- Python offline (rtty_gen, serial_logger, cer_analyze)

Следующий шаг: **baseline-замер текущего декодера Build 230**, затем Этап 1.1 (soft-LLR).

## [Build 238] - 2026-04-13
### Added (testbench — Python tools)
- **`tools/serial_logger.py`**: timestamped логгер serial-вывода устройства. Каждая пришедшая строка пишется как `<ISO8601>\t<line>`, совместимо с таймстемпами HTML sweep-лога. Используется в паре с sweep из `rtty_simulator.html` → потом `cer_analyze.py` (следующий билд) сопоставит по времени.
- CLI: `--port COM27 --baud 115200 --out datasets/logs/session.log [--raw] [--echo]`.
- Папка `datasets/logs/` создаётся автоматически. Маркеры `=== LOG START/END ===` на границах сессии.
- Зависимость: `pyserial` (уже установлен).

## [Build 237] - 2026-04-13
### Added (testbench — Python tools)
- **`tools/rtty_gen.py`**: offline-генератор WAV с известным текстом + контроль SNR. CLI-args: `--text --baud --shift --stop --center --snr --sr --duration --out`.
- ITA2 Baudot (LSB first, start=Space, stop=Mark), FIGS/LTRS auto-shifts, совпадает с конвенциями `rtty_simulator.html`.
- Continuous-phase синтез (фаза переносится через frequency-edges — нет разрывов, как у `OscillatorNode`-перестроек).
- AWGN в audio-полосе: `noise_rms = signal_rms · 10^(−SNR/20)`. Clip-protect автоматический.
- Ground-truth текст выводится в stdout (для сверки с decoded serial output).
- Smoke-test пройден: 3s @ +5 dB SNR → 16 символов "RYRYRY RYRYRY RY".
- Зависимости: `numpy`, `scipy` (установлены).

## [Build 236] - 2026-04-13
### Added (testbench — Phase 9, подпункт 5/5 — closes testbench phase)
- **Batch SNR sweep в `rtty_simulator.html`**. Автоматический sweep SNR по заданным точкам с dwell-временем. UI:
  - Sliders: `SNR from` +30..−25, `SNR to` +30..−25, `Step` 1..5 dB, `Dwell` 5..120 s.
  - Кнопки `SWEEP` / `CANCEL`.
  - Log area: ISO-таймстемп + SNR + индекс на каждой точке. Первая/последняя строка = маркеры границ sweep'а.
- Sweep автоматически включает AWGN и двигает slider SNR. Направление (вверх/вниз) выводится из знака разницы.
- **Методика замера CER(SNR)**: (1) запустить TX в симуляторе, (2) запустить serial-логгер устройства с timestamp'ами, (3) нажать SWEEP, (4) скопировать лог-текст + serial-лог, (5) offline-Python (следующая задача) сопоставит по времени и посчитает CER на каждой точке.
- **Закрывает testbench-фазу Phase 9**. Дальше: Python `rtty_gen.py` + `cer_measure.py` для offline-воспроизводимости, потом baseline замер текущего декодера.

## [Build 235] - 2026-04-13
### Added (testbench)
- **CW QRM теперь с реальной морзянкой**. Добавлен dropdown `CW mode: Continuous carrier | Keyed morse`. В режиме keyed — полноценная передача текста `"CQ CQ DE UA3TEST K  "` по азбуке Морзе с character-dict для A-Z/0-9/пунктуации.
- **Ручной ключ** (реализм): jitter-слайдер `0..50 %` добавляет случайное масштабирование на длину каждого элемента (точка/тире/пауза). Реалистично имитирует оператора, не идеального автомата. При 20% джиттере соотношение dot/dash плавает, межбуквенные паузы тоже. Слайдер WPM `10..40` (PARIS-based: dot = 1.2/WPM).
- **Envelope**: 5 ms linear ramp на включении/выключении элемента — мягкий key-click (не идеально острый, как у реального ключа с side-tone фильтром).
- Scheduler отдельный (`scheduleCWChunk`), look-ahead 1.5с, восстанавливает `cwTime` если отстаём.

## [Build 234.1] - 2026-04-13
### Fixed
- **QRM RTTY scheduler не запускался**: в `startRTTY` вызов `scheduleQRMChunk()` стоял **до** `isPlaying = true`, а функция на входе `if (!isPlaying) return;` без рескеда. Второй RTTY-сигнал в итоге застревал на начальной Mark-частоте (звучал как непрерывный тон). Вызов перенесён после `isPlaying = true`.

## [Build 234] - 2026-04-13
### Refactored
- **Simulator signal path → dual-tone (Mark/Space независимые ветки)**. Вместо одного `OscillatorNode` с `frequency.setValueAtTime`-переключением — две постоянные ветки: `markOsc → markBitGain → markFadeGain → gain` и `spaceOsc → spaceBitGain → spaceFadeGain → gain`. Scheduler теперь toggleит BitGain'ы (с микро-ramp 0.5ms для anti-click), а не частоту. Drift-ветки подключены к **обеим** `osc.frequency` (ConstantSource + SinOsc → markOsc.frequency + spaceOsc.frequency), т.е. оба тона дрейфуют синхронно.
- **Зачем refactor**: нельзя честно смоделировать selective fading (Mark и Space замирают независимо — КВ-multipath) без раздельных gain на каждую несущую.

### Added (testbench — Phase 9, подпункт 4/5)
- **QSB — flat amplitude fading**: синусоидальный envelope поверх всего сигнала. Слайдеры `depth 0..40 dB` и `period 1..60 s`. Формула: `fade = 10^(−(depth/2)·(1−cos(2π·t/T))/20)` ∈ [10^(−depth/20), 1] — периодически проваливается до минимума и возвращается к 1.
- **Selective fading** (КВ-multipath): Mark и Space замирают **независимо**, с фазовым сдвигом 0.7π между ними. Имитирует ситуацию, когда один тон глубоко в нуле, другой виден — типичное на КВ при ионосферном multipath. Слайдеры `depth 0..40 dB`, `period 1..30 s`.
- Envelope-апдейт в JS через `setInterval(50ms)` + `setTargetAtTime` на `markFadeGain`/`spaceFadeGain` (дёшево, QSB медленный).
- **Impulse noise** (QRN / атмосферики): случайные короткие (~2 ms) экспоненциально-затухающие шумовые всплески. Poisson-распределение: интервалы `−ln(1−rand) · 60/rate`. Слайдеры `rate 0..300 clicks/min`, `amplitude ×0..×20` от SIGNAL_PEAK.
- **Use case**: максимально приближенные к реальному КВ-эфиру условия для стресс-теста декодера (текущего и будущего гибридного).

## [Build 233] - 2026-04-13
### Added (testbench — Phase 9, подпункт 3/5)
- **tools/rtty_simulator.html: частотный drift** основного сигнала. Две независимые составляющие, суммируются на `osc.frequency` AudioParam (поверх `setValueAtTime`-scheduling, т.к. AudioParam складывает intrinsic + входы):
  - **Linear drift**: `ConstantSourceNode` с длинным `linearRampToValueAtTime` (rate·3600 за час). Slider `−10..+10 Hz/s`, step 0.1. Имитирует тепловой уход TRX после включения.
  - **Sinusoidal drift**: low-freq `OscillatorNode` × `GainNode` → amplitude Hz. Sliders `amp 0..50 Hz`, `period 1..60 s`. Имитирует Doppler/ионосферный wobble/QSB-частоту.
- Обе ветки с live-update через `setTargetAtTime`. Checkbox-ы включают/выключают независимо.
- **Use case**: проверка устойчивости AFC и SEARCH к медленному уходу частоты; подготовка к оценке, насколько widely-matched filter (path B из §3 плана) лучше narrow (path A) в drift-условиях.

## [Build 232] - 2026-04-13
### Added (testbench — Phase 9, подпункт 2/5)
- **tools/rtty_simulator.html: QRM-инжекция**. Две параллельные ветки помех поверх основного RTTY-сигнала:
  - **CW carrier**: непрерывный sine-тон. Slider частоты `300..3000 Hz` + уровень `−30..+20 дБ` относительно основного сигнала. Live-обновление без перезапуска.
  - **Second RTTY**: второй RTTY-сигнал 45.45/170/1.5 с фиксированным текстом `"CQCQCQ DE TEST RYRYRY 73 "`. Slider center-freq `400..2800 Hz` + уровень `−30..+20 дБ`. Отдельный scheduler со своим look-ahead (1.0с).
- **Уровни**: оба QRM-источника нормированы к `SIGNAL_PEAK=0.5`, то есть `cw_gain = 0.5·10^(lv/20)`. `lv=0 dB` = той же мощности что основной сигнал.
- **Use case**: проверка устойчивости SEARCH и декодера к соседним сигналам, имитация реального эфира с несколькими станциями на одной полосе.

## [Build 231] - 2026-04-13
### Added (testbench — Phase 9, подпункт 1/N)
- **tools/rtty_simulator.html: AWGN + SNR slider**. Первый шаг к testbench для гибридного декодера (Phase 9). В симулятор добавлена параллельная ветка белого гауссовского шума (сумма 3-х uniform, RMS≈1.0, 10-секундный looped buffer). Gain шума вычисляется как `SIGNAL_RMS · 10^(−SNR/20)` — slider `−25..+30 дБ` меняет уровень шума live, без перезапуска. Checkbox "AWGN" включает/выключает ветку.
- **Конвенция**: sine peak = 0.5 (RMS ≈ 0.354), SNR в полной audio-полосе (не в bit-bandwidth). Упрощение для интерактивного теста; точный SNR-в-полосе считаем позже в Python.
- **Цель use**: крутим slider, смотрим, на каком SNR декодер (Build 230) начинает сыпаться — получаем первичную оценку threshold baseline перед внедрением soft-LLR.
- План: отдельные билды добавят QRM-инжекцию, частотный drift, импульсные помехи, batch-mode для CER-замера.

## [Build 230] - 2026-04-13
### Verified (no code changes)
- **STOP-DET алгоритм подтверждён на реальном эфире (50/450/1.5)**: 60-секундный захват через `C:\Temp\stopdet_capture.ps1` с корректным SEARCH-локом (FREQ=984.1, ERR=3%) показал однозначное голосование `Result: 1.5 bits (votes: 1.0=0 1.5=19 2.0=1)`. Все измеренные gap_fraction в диапазоне 0.34–0.60T — плотно внутри bin 1.5 (границы 0.25/0.85 корректны).
- **Ранее наблюдавшийся баг** ("определил 1.0 вместо 1.5") был downstream-симптомом: SEARCH не залочился корректно, state-7-end timing измерялся на несинхронизированном framer. С правильным локом STOP-DET работает как задумано.
- TODO: ловить edge-cases на других сигналах (100 бод, 2.0 стоп, слабые SNR).

## [Build 229] - 2026-04-13
### Fixed (measurement)
- **Core 1 load metric теперь честная**: DMA-waits в `ili9488_push_*` функциях (`dma_channel_wait_for_finish_blocking`) были учтены как работа, хотя на деле это блокирующее ожидание (Cortex-M33 спит). Добавлен `shared_c1_dma_wait_time` — аккумулирует DMA-ожидания, вычитается из Core 1 total_work перед расчётом процента.
- **Результат**: замер Core 1 упал с 41-47% до **8-10%** (соответствует reference-проекту). Compute-нагрузка всегда была низкой — только метрика врала. Реальный запас для гибридного декодера огромный.

### Changed
- UI update interval 200ms → **500ms** (как у соседей). Снижает частоту top-bar перерисовок в 2.5×. Косметически; реальный выигрыш маскирован исправлением метрики.

## [Build 228] - 2026-04-13
### Optimized
- **Incremental text rendering (Core 1)**: regular char-append теперь рендерит только нижнюю строку (`drawRTTYLastLineOnly`) — fillRect 440×line_h + один drawString + push_colors только затронутой полосы, вместо fillSprite 480×160 + 16×drawString + полный push. На hot path (60 символов из 61 до переноса) экономится ~10× SPI-трафика и ~10× рендер-работы. Полный `drawRTTY` вызывается только при добавлении новой строки (newline/CR/line-wrap), смене scroll_offset, или ре-рендере экрана. Throttle 8ms (~120 fps cap) для incremental.

## [Build 227] - 2026-04-13
### Optimized
- **Ring buffer FFT collection (Core 0)**: убран 2 KB `memmove` каждые 480 сэмплов (~102 ms) — теперь `ts[]` циркулярный с bitmask-индексом `& (FFT_SIZE-1)`. Snapshot в `shared_fft_ts` делается одним unwrap-проходом от oldest→newest вместо memmove + memcpy.
- **ADC-pacing через `adc_fifo_get_blocking()` без busy-wait**: удалён избыточный `while(adc_fifo_is_empty()) tight_loop_contents()` перед реальным blocking-вызовом. Cortex-M33 теперь реально спит между сэмплами. Timestamp `st` перенесён *после* wake-up — Core 0 load metric исключает idle/sleep time.
- **Результат**: Core 0 7% → **4-5%**. Приближение к reference-проекту (3%). Ring FFT также снижает I-cache pressure.

## [Build 226] - 2026-04-13
### Fixed
- **SEARCH больше не пропускает широкие FSK-пары**: в B222 добавленный valley-test (rejection пар с глубокой впадиной между пиками) был слишком агрессивен — отсекал легитимный 450-Hz сигнал (Mark b180=+41 dB, Space b224=+22 dB, впадина на шумовом полу ~−2 dB, diff 33 dB). Порог повышен 25 → 40 dB: real wide-FSK (valley ~30-35 dB ниже пиков) проходит, cross-signal false combos (diff 40+ dB) по-прежнему отсекаются. Проверено на живом эфире: погодный 50/450/1.5 выбирается с score=109.8, обгоняя шумовые 200-Hz кандидаты.
- **SEARCH tolerance per-shift (внутренний цикл)**: B222 расширил `local_tolerance` только во внешнем цикле (границы lo). Внутренний `for (d = -tolerance; ...)` остался на константе 2 — поэтому для 425/450/500/850 часть легитимных кандидатов с drift ±3-4 bins всё равно отсекалась. Теперь оба цикла используют `local_tolerance`.

### Added
- **DUMP SPEC**: serial-команда, дампит текущие FFT-магнитуды (512 бинов, bin=9.77 Hz). Нужна для offline-анализа спектра: можно получить срез с устройства и в диалоге определить, какие сигналы присутствуют, почему AUTO залочилось на неверную пару, etc.
- **DUMP MS**: дампит огибающие Mark/Space (480 сэмплов истории).
- **shared_fft_mag[]**: Core 1 копирует `smooth_mag` в shared-массив после каждого FFT-расчёта. Нужен для DUMP SPEC (serial-handler не делает FFT сам).

## [Build 222] - 2026-04-13
### Added
- **Valley test в SEARCH**: отвергает фейковые FSK-пары из двух независимых сигналов, чья случайная разница частот совпала со стандартным shift. Например: узкий CW на 890 Hz + сильный Mark широкого RTTY на 1758 Hz → разница 868 Hz ≈ 850 shift → SEARCH выбирает ложный 850. Проверяется минимум магнитуды между пиками для shift > 20 bins; при очень глубокой впадине (>40 dB ниже пиков, скорректировано в B226) пара отвергается как "два разных сигнала".

### Changed
- **Wider shifts have wider tolerance**: при shift_bins ≥ 15 tolerance=3, при ≥ 40 tolerance=4 (было константа 2). Компенсирует FSK spectral smearing и TX drift на широких разносах — 450-Hz сигнал может сидеть на 44 bins вместо идеальных 46, не теряется.

## [Build 221] - 2026-04-12
### Added
- **Seqlock для shared DSP data**: Core 0 оборачивает запись `shared_fft_ts/adc_waveform/mag_m/mag_s` в инкремент `shared_dsp_seq` с `__dmb()` барьерами. Core 1 читает с retry-циклом (до 3 попыток) — если seq изменилась между началом и концом memcpy, данные считаются рваными и перечитываются. Задел под будущий перенос FFT на Core 0 (частота shared-обновлений вырастет).
- **SAVE flash serial indicator**: `[SAVE] writing flash (DSP paused ~45ms)...` + `[SAVE] done in X me`. Кнопка SAVE в UI уже меняет цвет визуально.

### Changed
- Memory barriers `__dmb()` добавлены в Core 0 writer и Core 1 reader для корректной работы seqlock на двухъядерном ARM.

## [Build 220] - 2026-04-12
### Optimized
- **FIR 63-tap симметричный**: буфер power-of-2 (64) для bitmask-индексации вместо `% 63`. Использована симметрия коэффициентов (`fir_coeffs[i] == fir_coeffs[62-i]`) — 32 умножения + 31 сложение пар вместо 63 умножений. Forward iteration убирает reverse branch.
- FIR ~50% быстрее, освобождает ~0.5% Core 0.

## [Build 219] - 2026-04-12
### Added
- **PIO Waterfall LUT**: предвычисленная `waterfall_pio_lut[256]` таблица rainbow-gradient (uint8 → 32-bit PIO-ready RGB666). Rainbow-расчёт теперь O(1) lookup вместо 6 float-операций + color565 + byte swap на каждый из 480×64 = 30720 пикселей в кадре.
- **Circular history buffer**: `wf_history[64][480]` uint8 (30 KB) вместо RGB565 sprite (61 KB). Скролл = декремент `wf_offset` без memcpy.
- Новая функция `ili9488_push_waterfall_lut()` — рендер через history + LUT + ping-pong DMA.

### Changed
- Core 1 нижняя граница загрузки: 60% → **39%**. FPS водопада: стабильно 22 → 20-25.
- Reference идея из `c:\YandexDisk\DIY\RP2350_RTTY\TouchRTTY\` портирована (там та же схема PIO LUT + history buffer).

### Documented
- `docs/ROADMAP_OPTIMIZATION.md` раздел 8: гибридный декодер RTTY (цель — **лучше 2Tone**, порог ~−15..−16 дБ SNR). 4 этапа: Goertzel matched filter → Multi-phase Goertzel → Character-level ML → Bayesian prior + Viterbi + noise blanker + spectral sub + temporal diversity + tiny NN fallback + soft confidence UI.
- `docs/20260412/` — детальный анализ алгоритмов (RTTY_DECODER_ALGORITHMS_COMPARISON, IQ_VS_GOERTZEL_ML_ANALYSIS, OPTIMIZATION_AND_INTERFERENCE_MITIGATION).

## [Build 218] - 2026-04-12
### Added
- **Chain BAUD→STOP detection** (Build 217): STOP-DET now waits for BAUD-DET to complete before starting. New flag `shared_chain_stop_after_baud` ensures STOP gap classification uses the correct baud rate instead of a stale default.
- **STOP-DET warmup** (Build 218): first 1.5s of gap measurements are discarded — DPLL phase noise is too high immediately after framer switches to permissive mode.
- **STOP-DET idle filter** (Build 218): gaps > 1.25T are rejected as inter-frame pauses (previously counted as bin=2 votes, corrupting results).
- **Parabolic peak interpolation** (Build 216): sub-bin FFT precision for SEARCH frequency measurement. Center frequency accuracy improved from ±10 Hz to ±2-5 Hz.
- **Shift-proportional dedup tolerance** (Build 216): `max(3, shift_bins/8)` — prevents FSK spectral smearing from generating multiple false candidates for wide shifts (850 Hz: 6→1 candidate).
- **Clipping indicator** (Build 216): SIG bar blinks red/white with "CLIP!" text when ADC clips. 1.5s latch.
- **Auto-recovery chain** (Build 217): ERR > 15% for 3s triggers BAUD-DET → STOP-DET re-measurement.
- **Simulator Mark frequency mode** (Build 216): `rtty_simulator.html` now accepts both Center and Mark frequency as input.
- **serial_cmd.ps1 improvements** (Build 217): try/finally/Dispose for proper COM port cleanup; DTR/RTS enabled for USB CDC reads.

### Changed
- **STOP-DET bin boundaries** (Build 218): adjusted from 0.25/0.75 to 0.25/0.85 based on empirical gap measurements across all baud rates. 2.0 stop bits now correctly detected (gap ≈ 1.0T → bin 2).
- **SEARCH dist_penalty** increased from 1.5 to 2.5 for better shift discrimination (425 vs 450 Hz).
- **SEARCH pipeline**: when both BAUD and STOP are AUTO, only BAUD-DET fires; STOP-DET chains after completion (was: both fired in parallel, causing stale-baud misclassification).

### Fixed
- **STOP-DET wrong on 100 baud**: gap_fraction was computed with default baud (45.45) instead of detected baud. Fixed by chain logic.
- **STOP-DET always voting 2.0 for inter-frame pauses**: 54ms idle gaps (5.5T) were not filtered, all landed in bin=2. Fixed by 1.25T upper filter.
- **SEARCH cycle-leak** (Build 215): `found_current` from previous test caused entry into cycle path instead of full rescan. Removed cycle-by-frequency path after full rescan.
- **COM port phantom locks**: serial_cmd.ps1 had no try/finally, killed processes left phantom port locks.

### Documentation
- Full rewrite of `DEVELOPMENT_CONTEXT.md` — all algorithms, architecture, test results
- Full rewrite of `PHASE3_RTTY_DSP_FINAL.md` — detailed DSP/DPLL/SEARCH/BAUD-DET/STOP-DET
- Updated `ROADMAP_OPTIMIZATION.md` — refactoring history, performance optimizations, current status

### Tested
- **Simulator matrix (8/8 pass)**: 45/170, 50/450, 75/425, 100/850 × stop 1.0, 1.5, 2.0
- **Real signals via WebSDR (3/3 pass)**:
  - 4583 kHz DWD: 50/450/1.5 — clean decode
  - 10100 kHz DWD: 50/425/1.5 — correct with noise
  - 7646 kHz DWD: 50/450/1.5 — noisy but correct
  - 12579 kHz SITOR-B: 100/170 detected correctly (Baudot decoder N/A for FEC)

## [Build 206] - 2026-04-05
### Added
- **Baud rate auto-detection**: symbol duration histogram approach (like PhosphorRTTY)
  - Accumulates D-sign transitions for 3 seconds, builds interval histogram
  - Scores each candidate baud (45.45/50/75/100) by matching peaks at multiples of bit_period
  - Weighted scoring: distance decay + harmonic multiplier
  - Clear winner (>1.5× second best): apply immediately
  - Ambiguous: sequential ERR verification (2s per baud)
- **100 Baud support**: new baud rate for NAVTEX/SITOR
  - Baud popup: 3×2 grid (45/50/75/100/AUTO)
  - Serial command: `BAUD 0-3` (manual) or `BAUD 4`/`BAUD AUTO`
  - `shared_baud_idx`: 0=45, 1=50, 2=75, 3=100, 4=AUTO
- **BD indicator in top bar** (Row 3, under shift): BD:45 (cyan), BD:50(A) (green auto), BD:.. (yellow detecting)
- **100 Baud in test generator** (`tools/rtty_simulator.html`)

### Fixed
- **SEARCH not finding 450Hz meteo signal**: was only scanning manual shift; now always scans ALL 8 shifts
- **SEARCH breaking manual settings**: was forcing all params to AUTO; now only triggers auto-detect for params already in AUTO mode
- **SEARCH always applies detected shift**: switches shift_idx to AUTO after applying found shift

## [Build 205] - 2026-04-05
### Added
- **Stop-bit popup**: 2×2 touch grid (1.0 / 1.5 / 2.0 / AUTO) with blue AUTO highlight
- **Auto stop-bit detection**: sequential test 1.0→1.5→2.0 (3s each), picks lowest ERR rate
- **Multi-signal SEARCH**: finds ALL RTTY signals on waterfall, cycles between them on repeat press
  - First press: selects strongest signal by score
  - Subsequent presses (< 10s): cycles through saved list without re-scanning
  - After 10s timeout: performs fresh search
- **SEARCH → AUTODETECT pipeline**: SEARCH triggers stop-bit detection + auto-inversion
- **Serial commands**: `STOP AUTO`, `STOP 0/1/2` for stop-bit control
- **Top bar indicators**: ST:1.5 (cyan), ST:1.5(A) (green auto), ST:.. (yellow detecting)
- **Bottom bar**: ST button shows current stop-bit or "ST:AUTO"

### Fixed
- **SEARCH not finding real signals**: candidates array overflow (32→128 with eviction), imbalance threshold too strict (10→20 dB), first press selected by frequency instead of score
- **1.0 stop-bit decoding ("123" → "0)")**: two root causes fixed:
  - Simulator ITA2 FIGURES table had `\03` octal escape bug (single ETX char instead of `\0`+`3`), fixed with `\x003`
  - Framer Continuous DPLL checked D polarity which failed due to biquad LPF delay; removed check for 1.0 stop bits
- **Serial console not responding to HELP**: VS Code Serial Monitor sends without CR/LF; added 500ms timeout-based command parsing
- **Auto stop-bit always picking 1.5**: test time too short (1s→3s), removed priority tie-breaker

### Changed
- **RTTY Simulator** (`tools/rtty_simulator.html`): shift dropdown (8 values + Custom), single center frequency input, auto-computed Mark/Space display, `setValueAtTime` for instantaneous frequency switching
- **Adaptive SEARCH threshold**: candidates scoring < 40% of best are discarded

## [Build 194] - 2026-04-04
### Added
- **Tuning Lab** (MENU → TUNE): dedicated screen for DSP parameter tuning
  - Eye diagram with phosphor persistence (240×64, DPLL-synchronized X axis)
  - Touch controls: ALPHA±, BW±, SQ± buttons
  - DUMP:ON/OFF toggle — enables continuous diagnostic stream to serial
  - SAVE button — writes all settings to flash
- **Serial Command System** (15 commands, type `HELP` for full list):
  - Tuning: `ALPHA`, `BW`, `SQ`, `FREQ`
  - Protocol: `BAUD`, `SHIFT`, `STOP`, `INV`
  - Control: `AFC`, `AGC`, `DIAG`, `STATUS`, `SAVE`, `CLEAR`
- **Diagnostic Stream** (`[D]` prefix, ~500ms interval):
  - SNR, SIG, ERR%, SQ state, AGC dB, DPLL phase/freq error, Mark/Space envelopes, core loads

### Changed
- **Menu restructure**: removed BW±, SQ±, SAVE from main menu (moved to Tuning Lab)
- **DIAG screen**: renamed DIAG:ON/OFF button to DUMP:ON/OFF
- **Boot encoder**: short press = touch recalibration only, long press (3s) = factory reset + recal

### Fixed
- Reset confirm dialog disappearing instantly (incoming RTTY chars overwrote text zone)
- Text zone flicker when Tuning Lab active
- Touch recalibration on boot: `shared_force_cal` was reset on early encoder release

## [Build 191] - 2026-04-04
### Added
- **Error rate indicator**: 100-character sliding window, displayed as percentage and bar in top panel
- **3 thin bars** in top panel: SIG (signal level), AGC (auto gain in dB), ERR (error rate %)
- **AGC display in dB** (right of AGC bar, replaces old multiplier display)

### Fixed
- **Reception broken** (Build 190): FFT on Core 0 blocked ADC for ~1ms → FIFO overflow → DPLL lost phase. Reverted FFT back to Core 1.
- **FPS drop 22→14** (Build 190): `__wfe()` in ADC wait loop didn't wake on ADC FIFO events. Fixed with `tight_loop_contents()`.
- **Core 1 at 90% load**: `tight_loop_contents()` idle loop counted as work. Fixed with `sleep_us(20)`.

## [Build 190] - 2026-04-04
### Added
- **Hardware ADC FIFO**: `adc_fifo_setup()` + `adc_run(true)` for jitter-free 10kHz sampling
- **Ping-pong double buffering** in `ili9488_push_colors()` for DMA transfers
- **fast_log2f()**: IEEE 754 bit-trick approximation (~4x faster than `log10f`)
- **AGC optimization**: precomputed `1/release` (multiply instead of divide)
- **Lissajous scope**: bitmask phosphor fade + sin/cos lookup table

## [Build 189] - 2026-04-02
### Optimized
- **Hardware FPU Acceleration:** Enforced strict `float` policy across all DSP code (Core 0).
- **Fast Math Migration:** Replaced all double-precision functions with single-precision `float` variants.
- **Performance Milestone:** Core 0 load reduced to ~7% at 10kHz sample rate.
- **Compilation Flags:** `-O3`, `-ffast-math`, `-funroll-loops` verified in CMake.

## [Build 188] - 2026-04-02
### Added
- Professional font system: NORM (Font2, 17px) and NARW (Font0, 10px).
- Pixel-perfect rendering (removed all fractional scaling).
- Hardware-accurate color rendering for ILI9488 (RC1.2).

## [Build 185] - 2026-04-01
### Added
- DIAG sub-menu with Zero Bias Meter, Rainbow Palette, line width control.
- Smart Newline (CR/LF collapsing for radio-teletype streams).
- AFC button in bottom bar.

## [Build 172] - 2026-03-25
### Added
- Continuous DPLL with PI controller for 1.0 stop-bit streams.
- Strict SNR-based squelch with hysteresis.
- Quadrature I/Q demodulator with Biquad LPF.
- 63-tap FIR bandpass filter.
- Baudot/ITA2 decoder with FIGS/LTRS support.
