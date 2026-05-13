# Phase 9 — Hybrid RTTY Decoder Plan

> 🇬🇧 [Read in English](PHASE9_HYBRID_DECODER_PLAN.md)

> **Status: SHIPPED as v2.0.0 (Build B265, 2026-05-12).**
> Этот документ — замороженный design plan, по которому шла реализация.
> Архитектура частично отклонилась от плана (см. §0 и
> `docs/ROADMAP_OPTIMIZATION.md` §8): пошёл в **dual-IQ + LLR fusion +
> TinyML NN** вместо Goertzel + Character-ML. Итог — порог декодирования
> ≈ −16 dB SNR (CER ~9 pp) против ~−13 dB у 2Tone, что закрывает цель
> «лучше 2Tone». Полные результаты — `RELEASE_v2.0.0.md`,
> `docs/NN_TRAINING.md`, `datasets/logs/bench_auto_v2/`.

**Дата создания**: 2026-04-13
**Статус**: исторический design doc (план реализован, см. шапку)
**Цель**: декодер, способный вытягивать RTTY на −15..−16 дБ SNR (лучше 2Tone, текущий threshold ≈ −6..−8 дБ).

---

## 0. Прояснение: что сейчас фактически реализовано

Путаница была обоснованная. Я выше в обсуждении неточно обозвал "Goertzel" то, что на самом деле в коде уже **IQ-demodulation**. Фактическое состояние `src/dsp_pipeline.cpp`:

```
f_out (AGC output)
  ├─► × cos(2π·f_mark·t)  → biquad LPF → mi
  ├─► × sin(2π·f_mark·t)  → biquad LPF → mq
  ├─► × cos(2π·f_space·t) → biquad LPF → si
  └─► × sin(2π·f_space·t) → biquad LPF → sq

mark_power  = mi² + mq²
space_power = si² + sq²
```

Это классический **quadrature (IQ) demod** через NCO (sin/cos таблицы 1024) + biquad LPF на каждой ветке. Формально эквивалентно sliding-window Goertzel при равной полосе LPF, но дешевле и с гибкой АЧХ (задаётся biquad-коэффициентами).

**Почему я сбил тебя с толку**: в плане писал "Dual-Goertzel" по привычке терминологии — в литературе узкополосный tone-detect часто называют Goertzel. Но у меня уже IQ-path, и это хорошо: biquad-LPF даёт лучшую форму АЧХ, чем rectangular-window Goertzel.

**Что тогда значит «гибрид»**: не Goertzel vs IQ, а **две параллельные IQ-ветки с разными LPF-характеристиками**, объединённые fusion-логикой. См. §3.

---

## 1. Этапы обработки (итоговая цепочка)

```
ADC 10 kHz
  │
  ├─► [A] DC-block + AGC                     [есть]
  │
  ├─► [B] Input BPF 300–3000 Hz              [новое, дешёвое]
  │
  ├─► [C] Adaptive LMS-notch (2-3 нулей)     [новое]
  │
  ├─► [D] Spectral noise reduction            [новое, опциональное]
  │       (spectral subtraction по FFT-пути)
  │
  ├─► [E] IQ-demod path A: narrow-LPF        [refactor из текущего]
  │       (BW ≈ baud · 1.0, minimum ISI)
  │
  ├─► [F] IQ-demod path B: wide-LPF          [новое]
  │       (BW ≈ baud · 1.5, matched raised-cosine)
  │
  ├─► [G] Fusion — weighted combine A+B       [новое]
  │       по оценке SNR/drift
  │
  ├─► [H] Soft-LLR bit decision               [новое, критичное]
  │       LLR = (M_env² − S_env²)/σ²
  │
  ├─► [I] DPLL bit-sync на LLR                [refactor: hard→soft]
  │
  ├─► [J] Soft-Viterbi framer (5N1.5/2)       [новое]
  │       start=0, stop=1 как constraint
  │
  └─► [K] ML post-classifier (eye → symbol)   [опционально, финал]
          маленькая CNN, синтетика+реал датасет
```

---

## 2. Бюджет улучшений

| # | Этап | Ожидаемый gain | Сложность |
|---|------|----------------|-----------|
| C | LMS-notch | +1-2 дБ (в эфире с QRM) | низкая |
| D | Spectral NR | +1-2 дБ (на слабых) | средняя |
| E+F+G | Fusion двух IQ-путей | +0.5-1.5 дБ | средняя |
| H+J | Soft-LLR + Viterbi framer | +2-3 дБ | высокая |
| K | ML post-classifier | +1-2 дБ | высокая |
| | **Суммарно (потенциал)** | **+6-10 дБ** | |

Текущий threshold ≈ −6..−8 дБ → цель −15..−16 дБ. При честной реализации достижимо.

---

## 3. Что такое «гибрид» — уточнение

Две параллельные ветки IQ-demod:

- **Path A (narrow)**: biquad LPF с BW ≈ baud. Оптимально по SNR при стабильном сигнале без дрейфа. Чувствителен к частотному offset.
- **Path B (wide/matched)**: raised-cosine FIR с BW ≈ 1.5·baud. Устойчив к drift и timing-jitter, чуть хуже по thermal noise.

**Fusion** (этап G):
- Vариант 1 (простой): weighted-sum огибающих, веса = f(SNR_estimate).
- Vариант 2 (продвинутый): выбор по текущему drift/jitter metric.
- Vариант 3 (ML-based): маленький classifier на 4-х метриках → веса.

Начнём с варианта 1, проверим, надо ли сложнее.

---

## 4. Приоритет реализации (утверждённый план)

**Этап 1** (быстрый выигрыш, простая реализация):
- [1.1] Soft-LLR bit decision (H) — заменить hard-slice
- [1.2] Soft-Viterbi framer (J) — использовать stop-bit как constraint
- **Ожидаемый gain**: +2-3 дБ за счёт двух правок

**Этап 2** (шумовая обстановка):
- [2.1] LMS-notch (C) — 2 адаптивных нуля
- [2.2] Input BPF (B) — фиксированный
- **Ожидаемый gain**: +1-2 дБ в реальном эфире

**Этап 3** (fusion):
- [3.1] Вторая IQ-ветка с raised-cosine FIR (F)
- [3.2] Fusion-логика (G) — weighted combine
- **Ожидаемый gain**: +0.5-1.5 дБ

**Этап 4** (NR):
- [4.1] Spectral subtraction на FFT (D)
- **Ожидаемый gain**: +1-2 дБ

**Этап 5** (ML):
- [5.1] Сбор датасета: синтетика + записи веб-SDR + реального RX
- [5.2] Обучение CNN на eye-diagram (16×220 → symbol)
- [5.3] Инференс на RP2350 (рукописный, без TFLite)
- **Ожидаемый gain**: +1-2 дБ

После каждого этапа — замер threshold, обновление CHANGELOG, согласование перед следующим.

---

## 5. Методология измерений

Нужно прежде чем начнём: **референсный testbench** для объективной оценки gain.

- [5a] Скрипт генерации синтетического RTTY + AWGN на заданный SNR (Python offline).
- [5b] Запись reference-сигналов через веб-SDR (разные баунды/shift/погодные/любительские).
- [5c] Процедура «проиграть в линию» (audio cable в ADC или через USB-DAC) — воспроизводимый тест.
- [5d] Метрика: character error rate (CER) как функция SNR. Threshold = SNR при CER=5%.

Без этого будем двигаться вслепую. Первое, что делаем после согласования плана — §5.

---

## 6. Архитектурные решения, которые нужно подтвердить

1. **Двойной IQ-path против одного улучшенного**: согласен делать fusion (этап 3) или хватит одной ветки с хорошим raised-cosine?
2. **ML runtime**: рукописный inference на float32, без внешних библиотек. CNN ≤8K параметров. Согласен?
3. **Датасет**: берём записи с веб-SDR (у тебя есть доступ) + синтетика. Объём цель: 10k символов реальных + неограниченно синтетики.
4. **Core распределение**: Core 0 — DSP (A..I), Core 1 — framer/ML (J,K) + UI. Согласен?
5. **Feature flag**: новую цепочку делаем за флагом (DIAG HYBRID ON/OFF) для A/B-сравнения со старой. Согласен?

---

## 7. Open questions / риски

- **Timing budget**: Core 0 сейчас 5%. Этап 2-3 добавит ~3-5%. Этап 4-5 ещё ~10-20%. Должно помещаться, но нужен замер.
- **Flash**: ML-модель 8K·4B = 32 KB, plus code. У меня есть запас.
- **Калибровка sq_snr**: при soft-decoding старая squelch-логика может мешать. Надо переделать на soft-confidence.
- **Обратная совместимость**: AUTO-поиск и framer-сейчас работают на hard-decision. Нужно аккуратно мигрировать.

---

## 7a. Execution log (один подпункт = один билд)

| Build | Дата | Подпункт | Статус |
|-------|------|----------|--------|
| 231 | 2026-04-13 | Testbench #1: AWGN + SNR slider в `rtty_simulator.html` | ✅ done |
| 232 | 2026-04-13 | Testbench #2: QRM-инжекция (CW + второй RTTY) | ✅ done |
| 233 | 2026-04-13 | Testbench #3: Частотный drift (linear + sine) | ✅ done |
| 234 | 2026-04-13 | Testbench #4: QSB + selective fading + impulse + dual-osc refactor | ✅ done |
| 235 | 2026-04-13 | Testbench extra: CW keyed morse mode | ✅ done |
| 236 | 2026-04-13 | Testbench #5: Batch-mode SNR sweep (closes testbench phase) | ✅ done |
| —   | 2026-04-15 | **Baseline Build 230 (AWGN only): threshold ~−10..−11 dB** | ✅ done |
| 237 | 2026-04-13 | Python: `rtty_gen.py` (offline WAV generator + AWGN) | ✅ done |
| 238 | 2026-04-13 | Python: `serial_logger.py` (timestamped serial capture) | ✅ done |
| 239 | 2026-04-13 | Python: `cer_analyze.py` + **testbench phase closed** | ✅ done |
| 240 | 2026-04-15 | Simulator: NOISE ONLY btn + impulse tone/duration/random | ✅ done |
| 241 | 2026-04-15 | Sweep sync-markers (=NN=) + cer_analyze --markers mode | ✅ done |
| 242 | 2026-04-15 | Этап 1.1: soft-LLR bit decision (adaptive stop/start thresholds) | ✅ done — threshold не сдвинулся (~−10..−11), но на −14 декодер жив (282 chars vs lost). Ждём Stage 1.2 для отсечки мусора. |
| 243 | 2026-04-14 | Этап 1.2: Soft-Viterbi framer (weakest-link data + frame-average) | ✅ done — на −8 дБ 6% B242 → 0% B243.1 (ложные фреймы вычищены). Threshold −10 дБ. Пороги 0.10/0.15 после тюнинга. |
| 244 | 2026-04-14 | Этап 2.1: LMS-notch adaptive (2 каскада: 300-1350 / 1650-3200 Hz) | ✅ done — AWGN threshold −10 dB сохранён; с CW QRM threshold −10 dB и 0% CER от +8 до −8 дБ. |
| 245 | 2026-04-16 | Этап 2.2: Input BPF 300-3000 Hz (HPF + LPF Butterworth) | ✅ done — AWGN threshold −10 dB, bin −10 стал 0.00% (B243.1 был 15%). Нейтрально, готов к Stage 3. |
| —   | —    | Этап 3: Fusion двух IQ-веток | pending |
| —   | —    | Этап 4: Spectral NR | pending |
| —   | —    | Этап 5: ML post-classifier | pending |

Каждый подпункт — отдельный коммит с номером билда, запись в CHANGELOG, обновление этой таблицы.

## 8. Что делаем прямо сейчас

После согласования:
1. Создать testbench (§5) — 1-2 сессии.
2. Снять baseline CER(SNR) текущего декодера.
3. Начать Этап 1 (soft-LLR).

Все изменения — под feature flag, с A/B-замером gain после каждого подэтапа.

---

## 9. Roadmap после Stage 5 var.1 (BW sweep) — утверждён 2026-04-20

**Глобальная цель**: TouchRTTY должен быть **лучше всех публичных декодеров** на AWGN и реальном канале: лучше 2Tone (−12..−14 дБ), fldigi (−9..−11 дБ), MMTTY (−8..−10 дБ). Цель: honest threshold **−15..−16 дБ**.

### 9.1. Методологическая находка (2026-04-20)

При проработке BW-свипа (#37) обнаружено: **в ground-truth тексте не было CR/LF**, поэтому декодер копил выдачу минутами и флашил одним куском → бин-attribution ломалось → threshold по 5% CER смещался к max SNR в свипе (артефакт, а не реальный порог).

**Fix**: `GT_TEXT = "RYRYRY THE QUICK BROWN FOX JUMPS OVER 1234567890 \r\n"` в оркестраторе, dwell 60s.

**Последствия для прошлых тестов**:
- Тесты *с* PATH/DYN/CMD cycling (b256 NR, b257 avg3) — OK, serial-команды давали естественный flush каждые 10-20с.
- Тесты *без* cycling (часть baseline-замеров, возможно часть gain-замеров Stage 1-2) — под подозрением.

### 9.2. Приоритеты (по порядку)

**P0. Stage 5 var.1 — matched filter BW sweep (task #37, in_progress)**
- k ∈ {0.40, 0.50, 0.60, 0.75, 0.90}, SNR −10..−18, dwell 60s, 3 run.
- Оркестратор: `tools/bw_sweep_orchestrator.py`.
- Ожидается: победитель в k=0.50..0.60 по Path A (по ранним данным lead у k=0.50 на верхних SNR, k=0.60 на −16).
- Artefacts: `datasets/logs/b258_bw/`.

**P1. Plan B — revalidate baseline + Stage 3.3 (task #38)**
- DYN ON/OFF A/B через `serial_logger --cmd-cycle 10 --cmd-seq "ON=DYN ON|OFF=DYN OFF"`, SNR −10..−18, 3 run.
- Цель: подтвердить что ~-14 дБ threshold и +3 дБ Stage 3.3 KEY WIN воспроизводятся под честной методикой.
- Если просело: откатываемся к Stage 2 и думаем заново.

**P2. Side-by-side benchmark vs 2Tone / fldigi / MMTTY (task #39)**
- Сгенерить AWGN-лесенку WAV файлов (синтетика rtty_gen), прогнать через:
  1. Мой firmware (sweep_runner + COM27 logger)
  2. 2Tone.exe через Wine или нативно Windows, audio loopback (VAC/Voicemeeter)
  3. fldigi через sounddevice loopback
  4. MMTTY через sounddevice loopback
- CER per decoder per SNR.
- **Без этого все мои цифры — «по слухам»**. Объективное сравнение — единственный способ доказать лидерство.

**P3. Stage 5 var.2 — Character N-gram LM (task #40)**
- Bigram/trigram likelihood таблица 32×32 (Baudot codes) или 32×32×32.
- Умножается с Viterbi path LLR на каждом шаге.
- Корпус: ham QSO logs + English/Russian news.
- Ожидаемый gain: **+1..3 дБ** — самый дешёвый путь к −15..−16 дБ.
- Реализация: Python генератор таблицы → const array в firmware → модификация soft-Viterbi (src/dsp_pipeline.cpp).

**P4. Real-air dataset (task #16)**
- Записи с веб-SDR и реального RX: AWGN, QSB, QRM, drift сценарии.
- Формат: 48 kHz / 16-bit mono (см. `datasets/RECORDING_GUIDE.md`).
- Нужно для: (a) валидация в реальных условиях, (b) обучение ML-классификатора (task #23).

**P5. Stage 5 var.3 — ML post-classifier (task #23)**
- Маленькая CNN ≤8K параметров, eye-diagram 16×220 → символ.
- Тренировка: синтетика + P4 датасет.
- Инференс ~1 мс/символ на RP2350 @ 300 MHz.
- Ожидаемый gain: +1..2 дБ.

**P6. Будущее — IQ-вход (task #24)**
- Прямо с SDR, обходя аудио-тракт и AGC/клиппинг.
- Требует нового аппаратного I/O формата.
- Gain: +2..4 дБ в marginal условиях.

### 9.3. Дополнительные техники (backlog)

- **BCJR** вместо Viterbi (+0.5..1 дБ): full forward-backward MAP. Если помещается в Core 0 budget.
- **Адаптивный stop-bit soft-detector** (+0.5 дБ): soft-decision marginal likelihood для stop=1.0/1.5/2.0.
- **Joint optimization** fusion-weights × BW × DPLL alpha (+0.5..1 дБ): кропотливо, но даёт последние децибелы.

### 9.4. Execution model

- Каждый подпункт → отдельный build + commit + CHANGELOG + update §7a.
- Каждый gain-замер → `cer_avg.py` (3-run avg, std), **с CR/LF в GT**.
- Каждое принятое изменение → A/B vs previous build под feature flag.
- Финальная валидация — P2 benchmark vs 2Tone/fldigi/MMTTY.
