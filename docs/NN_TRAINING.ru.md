# Тренировка нейронки

> 🇬🇧 [Read in English](NN_TRAINING.md)

Я возим с прошивкой маленький (7→128→64→32, около 44 КБ float32-весов)
MLP-классификатор, который голосует на Baudot-фреймах, где soft-bit
паттерн неопределённый. Production-веса закоммичены в
[`src/dsp/nn_weights.h`](../src/dsp/nn_weights.h). Если хочешь их
улучшить или натренировать свои под другую сигнальную среду — это сюда.

Резонно спросить, зачем NN на по сути 5-битовой задаче распознавания
символов. Короткий ответ: реальные Baudot-фреймы приходят с шумом, ISI,
фейдингом и AGC-артефактами, из-за которых семь soft-bit значений не
всегда чисто bipolar. А на тех SNR, что мне интересны (−14 до −20 dB),
простое решение по знаку действительно ошибается на 5–30 % фреймов. NN
часть этих ошибок отжимает обратно.

Более сложный вопрос — *когда* NN должна иметь право переопределять
sign threshold. Ранние версии давали ей стрелять на каждом фрейме,
получалась U-образная кривая: NN сильно помогала у порога, но активно
вредила на комфортном SNR, где hard decision и так была правильной.
B264 confidence gate это починил — на инференсе сеть включается только
когда модуль слабейшего data-бита ниже 30 % от оценённого уровня
сигнала. Выше — доверяем простому декодеру, точка.

---

## Архитектура, end-to-end

```
input[7] — 7 bipolar soft bits, нормированы по sig_level
    │
   w1 @ + b1
    │
ReLU 128
    │
   w2 @ + b2
    │
ReLU 64
    │
   w3 @ + b3
    │
argmax по 32 классам → Baudot-код (0–31)
```

Инференс — в [`src/dsp_pipeline.cpp`](../src/dsp_pipeline.cpp) около
строки 530. Правило решения:

```cpp
if (nn_gate_open && nn_margin > 0.5)
    current_char = nn_argmax;
```

Где `nn_margin = top_logit − second_top_logit`. Доверяем выбору NN
только если она ещё и уверена — мелкие margin'ы откатывают на hard
decision. Подтяжки на штанах и ремень.

---

## Дефолтные training data

Оба тренера (sklearn `tools/train_nn.py` и PyTorch
`tools/train_nn_torch.py`) зовут один и тот же `generate_synthetic()`,
который выдаёт 32 × 15 000 = 480 000 training-фреймов. Каждый фрейм —
это:

* Идеальный Baudot-паттерн (±1 на бит) для одного из 32 классов
* Плюс гауссов шум на сэмпл, `σ` тянется из exponential-распределения
  со средним 0.35 (потом клипается на [0.04, 1.10])
* Плюс ISI: каждый бит смешивается с предыдущим, alpha случайно из
  U(0.04, 0.32)
* Плюс per-frame signal scale из U(0.35, 2.2)

Экспоненциальное распределение шума — нестандартный выбор и тот, что
оказался важнее всего. Пришёл к нему после того, как `noise_mean=0.28`
дал веса с мелким, но устойчивым регрессом на −12 dB, а
`noise_mean=0.40` перебрал (модель стала «неуверена» во всём). 0.35
кладёт примерно 40 % training-массы в threshold-зону (σ > 0.4), и
выигрыш на −16 dB повторился.

Распределение шума переопределяется через `TRAIN_NN_NOISE_MEAN`.

---

## Production-рецепт: v13

Этот воспроизводи, если стартуешь с нуля.

```bash
TRAIN_NN_NOISE_MEAN=0.35 python tools/train_nn_torch.py \
    --epochs 60 \
    --n-synth 15000 \
    --weight-uncertain 3.0 \
    --out src/dsp/nn_weights.h
```

Главный фокус — `--weight-uncertain 3.0`. Loss каждого training-сэмпла
умножается на 3, если у сэмпла `data_min` ниже 0.30 — т.е. ровно те
фреймы, которые inference-time gate реально пустит в NN. Это фокусирует
градиент именно на тех hard-to-classify фреймах, на которых NN и должна
быть хороша.

Этого sklearn'овский `MLPClassifier` не умел — его `.fit()` не
принимает `sample_weight`. Одной этой недостающей фичи хватило, чтобы
портировать на PyTorch.

Чего ждать от чистого v13-прогона:

* Validation accuracy около 89.4 % (это число умеренно бессмысленное —
  ниже объясню почему)
* Время тренировки 5–7 минут на обычном CPU ноутбука
* Параметров всего: 11 360 float'ов = 44 КБ

Дальше — прошить и прогнать multi-seed sweep (инструкции ниже), чтобы
реально измерить, помогло или нет.

После v13 должен увидеть что-то такое:

| SNR | NN OFF mean | NN ON v13 mean | σ NN ON | Δ vs OFF |
|---:|---:|---:|---:|---:|
| −14 | 32.2 % | **23.4 %** | 1.5 | **−8.8** |
| −16 | 77.7 % | **55.3 %** | 3.2 | **−22.4** |
| −20 | 88.2 % | **80.4 %** | 2.3 | **−7.8** |

Критично — что σ на NN ON маленькая, 1.5–3.2 pp на тех SNR, что
важны. Широкая σ значила бы «повезло на одном сиде»; маленькая —
выигрыш устойчив.

---

## Почему validation accuracy врёт

Я это вытерпели на собственном опыте. Validation accuracy на
синтетическом hold-out set'е сидит около 89–91 % почти для каждого
варианта, что я пробовал. v4 — 89.46 %, v9 — 91.02 %, v13 — 89.44 %,
v17 — 89.42 %. Цифры на одно лицо и про реальное качество в эфире не
говорят почти ничего.

Причина: у validation set'а тот же SNR-distribution, что у training —
там доминируют лёгкие фреймы. Модель, которая 100 % безупречна на
лёгких и 50 % случайна на threshold'е, покажет ~92 % validation
accuracy, потому что лёгких больше. А в эфире её на пороге разнесёт.

Поэтому: **всегда гоняй multi-seed AWGN sweep на железе перед
решением, лучше ли новые веса**. Validation accuracy — это health
check тренировочной петли, не оракул.

---

## Опции тренировки, полностью

`tools/train_nn_torch.py` принимает:

| Флаг | Что делает |
|---|---|
| `--n-synth N` | Сэмплов на класс. Default 15 000 → 480k всего. |
| `--epochs N` | Макс. эпох с cosine LR schedule. Default 80. |
| `--early-patience N` | Стоп после N эпох без улучшения val_acc. Default 12. |
| `--lr <float>` | Стартовый LR Adam. Default 1e-3. |
| `--batch-size N` | Размер мини-батча. Default 1024. |
| `--weight-decay <float>` | L2 decay AdamW. Default 3e-4. |
| `--label-smoothing <float>` | Label smoothing в cross-entropy. Пробовал 0.05; навредило на −14 и −16. |
| `--weight-uncertain <float>` | Множитель per-sample weight для фреймов с `data_min < 0.30`. **3.0 победил; 5.0 перебрал.** |
| `--dropout <float>` | Dropout. Пробовал 0.1; маргинально, увеличил variance. |
| `--seed N` | Master seed для torch + numpy. Default 42. |
| `--real-npz <path>` | Подмешать real-air фреймы из `parse_dump_frames.py`. Повторяемый. |
| `--real-replicate N` | Реплицировать real-сэмплы N× перед смешиванием. Default 1. |
| `--out <path>` | Путь к выходному C-хедеру. Default `src/dsp/nn_weights.h`. |

Environment overrides:

* `TRAIN_NN_H1`, `TRAIN_NN_H2` — поменять ширину скрытых слоёв
  (default 128, 64). v16 пробовал 160 / 80 с тяжёлой регуляризацией.
  Шире давало больший абсолютный выигрыш на части SNR, но сильно
  большую seed-variance, так что не приняли. Если у тебя специфичные
  паттерны шума, которые baseline не вытягивает, — стоит
  поэкспериментировать.
* `TRAIN_NN_NOISE_MEAN` — exponential mean синтетического шума.
  Default 0.28 (legacy); v4/v13 production = 0.35.
* `TRAIN_NN_GATE_FILTER` — legacy с v7/v8; дропает синтетические
  сэмплы, у которых нормализованный `data_min` превышает порог.
  Оставлено для воспроизводимости, на практике не помогает.

---

## Capture real-air training data

Прошивка B265 умеет режим `DUMP FRAMES` — стримит по serial все семь
soft-bit значений каждого валидированного фрейма плюс label hard
decision. В сочетании с известно-хорошей записью получаются labeled
training data.

Полная петля:

```bash
# 1. Настрой декодер под сигнал в записи (пример DWD)
python tools/send_serial_cmd.py --port COM27 << 'EOF'
BAUD 1
SHIFT 5
INV NOR
PATH HYB
NN OFF
DUMP FRAMES ON
EOF

# 2. Проиграй WAV через audio loopback, логируй serial
python tools/bench_replay.py \
    --wavs datasets/recs_mono/your_recording.wav \
    --outdir datasets/logs/capture \
    --tag capture --device "LEN Q27h-10" --port COM27 \
    --gain 0.8

# 3. Распарсь FR-записи из лога в numpy training-файл
python tools/parse_dump_frames.py \
    datasets/logs/capture/capture_your_recording.log \
    --out datasets/training_real.npz

# 4. Тренируй с real-air augmentation
TRAIN_NN_NOISE_MEAN=0.35 python tools/train_nn_torch.py \
    --epochs 60 --n-synth 15000 --weight-uncertain 3.0 \
    --real-npz datasets/training_real.npz \
    --real-replicate 3 \
    --out src/dsp/nn_weights.h
```

⚠️ **Подводный камень, который я прошёл больно.** Первая попытка с
real-air augmentation (v10) показала неожиданно высокую seed-variance.
Глянул на собранные данные — большинство real-air фреймов *чистые*
(data_min > 0.40), и их hard-decision labels тривиально правильные.
Получается, модель в основном учится копировать hard decision на
лёгких фреймах — а это бесполезно: я хотел, чтобы NN *обыгрывала*
hard decision на сложных.

Чтобы реально двинуть стрелку через real data, надо размечать
*неопределённые* фреймы против **доверенного oracle** — например,
DWD template matcher, который знает ожидаемый формат wind-direction /
дня недели, и может уверенно сказать «правильный символ здесь был X,
даже если декодер выдал Y». Эта работа в roadmap'е. До тех пор
real-air augmentation полезна больше как разнообразие шумовых
паттернов, чем разнообразие labels.

---

## После тренировки: прошить и проверить

```bash
cd build && ninja              # пересобираем прошивку с новыми весами
picotool load -f TouchRTTY.uf2 # шьём через BOOTSEL
```

Если picotool не в PATH:

```bash
~/.pico-sdk/picotool/2.2.0-a4/picotool/picotool.exe load -f TouchRTTY.uf2
```

Дальше multi-seed bench:

```bash
for s in 42 43 44; do
    python -c "import sounddevice as sd; sd._terminate(); sd._initialize()"
    python tools/nn_sweep_compare.py \
        --from -4 --to -22 --step 2 --dwell 30 \
        --center 2210 --sig-level 0.5 --seed $s \
        --out-dir datasets/logs/nn_compare_myrun_s${s}
done

python tools/aggregate_compare.py \
    datasets/logs/nn_compare_myrun_s42 \
    datasets/logs/nn_compare_myrun_s43 \
    datasets/logs/nn_compare_myrun_s44
```

Вывод — таблица NN OFF mean / NN ON mean / σ / Δ по SNR. Сравни свою
Δ-против-NN-OFF с baseline v13 выше. Если у нового варианта и mean
ниже, *и* σ ниже на нужных SNR — обогнал v13. Если mean ниже, а σ
выше — seed-зависимо и в production не пойдёт.

`tools/overnight_runner.sh` автоматизирует весь цикл
train + flash + 3-seed sweep + aggregate как одну bash-цепочку. Я им
гонял шесть вариантов за ночь без надзора.

---

## Что пробовал и что не зашло

Каждая неудачная попытка переобучения закоммичена и заархивирована,
чтобы будущий я могл перепроверить эксперименты, не повторяя:

| Вариант | Что пробовал | Почему не зашло |
|---|---|---|
| v5  | noise_mean=0.40 | Перебрали — NN ON хуже OFF |
| v6  | 30K сэмплов/символ (2× больше) | Без выигрыша; та же val_acc |
| v7  | gate_filter=0.30 only | Эффективный set ужался до 7.5K/класс |
| v8  | gate_filter=0.30 + 60K (компенсация) | Threshold обменяли на средние SNR |
| v9  | noise_mean=0.32 | Внутри шума от v4 (нет ясного выигрыша/проигрыша) |
| v10 | synth + real-air, sklearn | Hard-decision labels = неправильный оракул |
| v12 | label_smoothing=0.05 | Навредил на −14 и −16 |
| v14 | dropout + тяжелее L2 | Высокая σ, маргинальный mean |
| v15 | label_smoothing + weight_uncertain | Два рецепта дрались друг с другом |
| v17 | weight_uncertain=5.0 | Перебрал (тот же шейп, что v5) |
| v18 | шире 160/80 + weight_uncertain | Шире не дала ценности поверх baseline |
| 256/128 | Wide arch, без регуляризации | Катастрофическая 84–90 % CER |

У каждого — веса в [`datasets/nn_archive/`](../datasets/nn_archive/) и
bench evidence под
[`datasets/logs/nn_compare_v*_s{42,43,44}/`](../datasets/logs/). Если
будущее изменение кажется перспективным, сначала загляни в архив —
скорее всего кто-то уже пробовал похожее, и я записал, что получилось.

Sweet spot остался: **v13** — PyTorch, baseline 128/64, noise=0.35,
`weight_uncertain=3.0`.
