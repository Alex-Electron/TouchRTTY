# Бенчмарки и тестовый тулинг

> 🇬🇧 [Read in English](BENCH_TOOLING.md)

Всё, что нужно для измерения, насколько хорошо работает декодер,
лежит в [`tools/`](../tools/). Этот документ объясняет, что отдельные
скрипты делают и как они стыкуются. Ни один не требует ничего, кроме
Python 3 + пары пакетов (`numpy`, `scipy`, `sounddevice`, `pyserial`,
для тренера — `scikit-learn` или `torch`).

Честный summary заранее: большинство этих тулов существует, потому
что мне надо было уверенно говорить «изменение реально помогло».
Bench-инфраструктура — больше кода, чем сама NN в прошивке, и это
правильное соотношение.

---

## С чего начать?

| Хочу… | Беру |
|---|---|
| Сгенерить синтетический RTTY WAV с заданным SNR | `tools/rtty_gen.py` |
| Проиграть WAV через декодер и поймать его serial-вывод | `tools/bench_replay.py` |
| Полный AWGN sweep с NN OFF vs NN ON | `tools/nn_sweep_compare.py` |
| Усреднить N seed-прогонов в mean ± σ таблицу | `tools/aggregate_compare.py` |
| Превратить `DUMP FRAMES`-лог в готовую training-npz | `tools/parse_dump_frames.py` |
| Без надзора крутить train + flash + sweep циклы | `tools/overnight_runner.sh` |
| Глянуть исторический bench против 2Tone | snapshot в `datasets/logs/bench_auto_v2/` |
| Сгенерить тестовый сигнал в браузере | открой `tools/rtty_simulator.html` |

Если просто хочешь проверить, что свежая прошивка работает — сразу
к `bench_replay.py`.

---

## Генерация WAV с известной правдой

`tools/rtty_gen.py` выдаёт стерео или моно WAV с Baudot-RTTY и
опциональным AWGN. Текст, baud rate, shift, центральная частота,
целевой SNR — всё параметры. Та же Baudot-таблица используется здесь,
в прошивке и в HTML-симуляторе, так что сгенерированному можно
доверять: декодер видит ровно то, что ты задал.

```bash
python tools/rtty_gen.py \
    --text "RYRYRY THE QUICK BROWN FOX 1234567890" \
    --baud 45.45 --shift 170 --centre 1500 \
    --sample-rate 48000 --duration 30 \
    --snr-db -6 \
    --out test.wav
```

Обычно ставишь `--snr-db` в ту threshold-зону, что тебя волнует
(−14 до −20 dB), чтобы WAV был той самой сложности, которую отлаживаешь.
Лёгкие SNR декодятся нормально почти на любой версии прошивки и
ничего не говорят.

---

## Проигрываем WAV через декодер

`tools/bench_replay.py` играет один или несколько WAV-файлов на
аудиоустройство, которое слушает ADC Pico, и одновременно ловит всё,
что Pico печатает по USB-serial. Результат — по одному лог-файлу на
WAV с ISO8601-метками serial-строк плюс summary-маркдаун.

```bash
python tools/bench_replay.py \
    --wavs datasets/recs_mono/my_recording.wav \
    --outdir datasets/logs/realair \
    --tag run1 \
    --device "LEN Q27h-10" \
    --port COM27 \
    --gain 0.8 \
    --prep-cmd "BAUD 1" \
    --prep-cmd "SHIFT 5" \
    --prep-cmd "PATH HYB" \
    --prep-cmd "NN ON"
```

`--tag` префиксит имена лог-файлов, чтобы два подряд идущих прогона
(NN OFF, потом NN ON) не затёрли друг друга. Этот урок я тоже
получил на собственной шкуре — терять 4-минутный capture-лог из-за
забытого тега у первого прогона реально раздражает.

`--prep-cmd` шлёт serial-команды декодеру *до* каждого WAV. Повторяй
сколько надо. Auto-centre detection включена по умолчанию: быстрый
скан спектра находит пару mark/space в WAV и ставит `FREQ` декодера в
середину, чтобы AFC не тянуть себя через большой offset. Выключить —
`--no-auto-center`, если хочешь проверить tracking range AFC.

---

## Полный sweep — `nn_sweep_compare.py`

Это headline-бенчмарк. Гоняет SNR-sweep через железо дважды — один
раз с NN OFF, один с NN ON — и выдаёт per-SNR таблицу сравнения.

```bash
python tools/nn_sweep_compare.py \
    --from -4 --to -22 --step 2 --dwell 30 \
    --center 2210 --sig-level 0.5 \
    --seed 42 \
    --out-dir datasets/logs/nn_compare_42
```

Вывод `compare.txt` выглядит так:

```
SNR (dB)  NN OFF  NN ON  delta
        -4      19.06%      16.64%      -2.42%
        -6      13.89%      13.89%      +0.00%
        ...
```

Плюс per-pass артефакты (полные отчёты `cer_analyze`, raw serial-логи)
для debug'а, когда CER выглядит странно.

Скрипт конфигурит декодер по serial перед каждым проходом — ставит
BAUD, SHIFT, INV, PATH, NN, FREQ, AFC. Можешь запускать с холода без
ручного выставления. В конце настройки восстанавливаются к NN OFF, но
если ткнёшь `Ctrl-C` посреди прогона, декодер может остаться в
странном состоянии. `STATUS` по serial покажет, что реально творится.

---

## Усреднение нескольких сидов

Single-run бенчи на низком SNR шумные. Стандартное отклонение на
SNR ≤ −16 dB обычно 6–15 pp между разными random seed'ами, так что
один прогон может ходить на 20 pp вверх/вниз для одной и той же NN.
**Любое production-решение должно быть усреднено минимум по трём
сидам.**

```bash
for s in 42 43 44; do
    python -c "import sounddevice as sd; sd._terminate(); sd._initialize()"
    python tools/nn_sweep_compare.py \
        --from -4 --to -22 --step 2 --dwell 30 \
        --center 2210 --sig-level 0.5 --seed $s \
        --out-dir datasets/logs/sweep_s${s}
done

python tools/aggregate_compare.py \
    datasets/logs/sweep_s42 \
    datasets/logs/sweep_s43 \
    datasets/logs/sweep_s44
```

На выходе — таблица `mean ± σ` по SNR. Я стандартизировали этот
формат для коммита экспериментов: у каждого NN-варианта в архиве
лежит рядом 3-seed таблица.

`sd._terminate(); sd._initialize()` между сидами — workaround для
причуды устройства LEN Q27h-10 на PortAudio под Windows: иногда
отказывается открыться второй раз без полного сброса PortAudio. На
другой машине может не понадобиться, но включить дёшево.

---

## Overnight-цепочка — `overnight_runner.sh`

Когда надо было пробовать шесть разных training-рецептов за ночь, я
написал это. Bash-скрипт, который чейнит `train + flash + 3-seed
sweep + aggregate` для списка вариантов. Один цикл — около 35 минут
(5 мин тренировка + 30 мин аудио + пара секунд анализа). Шесть
вариантов — около 3 часов без надзора.

Паттерн:

```bash
train_and_sweep() {
    local tag="$1"; shift
    TRAIN_NN_NOISE_MEAN=0.35 python tools/train_nn_torch.py "$@" \
        --out src/dsp/nn_weights.h
    cp src/dsp/nn_weights.h "datasets/nn_archive/nn_weights_${tag}.h"
    (cd build && ninja)
    picotool load -f build/TouchRTTY.uf2
    for s in 42 43 44; do
        python tools/nn_sweep_compare.py --seed $s \
            --out-dir "datasets/logs/nn_compare_${tag}_s${s}" \
            <остальные флаги>
    done
    python tools/aggregate_compare.py \
        datasets/logs/nn_compare_${tag}_s42 \
        datasets/logs/nn_compare_${tag}_s43 \
        datasets/logs/nn_compare_${tag}_s44
}

train_and_sweep "v12_ls005" --epochs 60 --label-smoothing 0.05
train_and_sweep "v13_wu3"   --epochs 60 --weight-uncertain 3.0
# ... и т.д.
```

Кайф в том, что утром у тебя шесть полностью оценённых NN-вариантов
с архивированными весами и agg-таблицами, готовых выбрать победителя.
Я так и сделал — v13 выбрал из получившегося evidence.

---

## DUMP FRAMES → training data

Прибавка к прошивке B265 позволяет ловить real-air training data с
любой записи, которую ты проиграешь через декодер. Когда у тебя есть
serial-лог с `FR ...` строками:

```bash
python tools/parse_dump_frames.py \
    datasets/logs/dump_real_v1/wav1_*.log \
    datasets/logs/dump_real_v1/wav2_*.log \
    --out datasets/training_real.npz
```

На выходе — numpy `.npz` с:

* `X`: `(N, 7)` float32, bipolar soft-bits уже нормализованы по sig_level
* `y`: `(N,)` int32, hard-decision labels (0–31)
* `sig`: `(N,)` raw sig_level на фрейм
* `data_min`: `(N,)` `min(|X[:, 1:6]|) / sig` — прокси для гейта

Скрипт по ходу парсинга печатает статистику — гистограмма labels плюс
бакеты data_min. Сразу видно, сколько твоих захваченных фреймов
реально в неопределённом бакете — это единственная часть данных, что
интересует NN (всё с `data_min > 0.30` на инференсе будет загейчено и
для тренировки — мёртвый груз).

---

## CER-анализатор

`tools/cer_analyze.py` делает тяжёлую работу: коррелирует sweep-лог
(«между t1 и t2 SNR был −14 dB») с serial-логом («в момент t декодер
выдал такие символы»). Выход — per-SNR-bin character error rate.

Внутри его вызывает `nn_sweep_compare.py`. Запустить можно и
напрямую, если у тебя своя пара sweep/serial:

```bash
python tools/cer_analyze.py \
    --sweep datasets/logs/my_sweep.txt \
    --serial datasets/logs/my_serial.txt \
    --gt "RYRYRY THE QUICK BROWN FOX JUMPS OVER 1234567890\r\n"
```

Особенность: анализатор использует cyclic-rotation comparison, чтобы
найти правильное выравнивание между декодированным текстом и
ground-truth. Для стандартной "RYRYRY..." GT-строки длиной 50
символов идеальный декод, начавшийся со смещения 14 вместо 0, читается
как 14 % CER — это чистый артефакт cyclic-rotation выравнивания, а не
реальные ошибки. Поэтому когда видишь CER около 14 % на чистом декоде —
это artifact floor, не реальный error rate.

Когда новые веса показывают, скажем, 22 % CER на конкретном SNR,
вычти 14 % baseline артефакта — получишь оценку реальной ошибки
(~8 pp). Для headline-сравнения с 2Tone в моих memory notes я это
делаем по умолчанию.

---

## Headline bench против 2Tone

Сравнивал TouchRTTY лицом к лицу с 2Tone 26.01a (G3YYD) на одинаковом
аудио. Bench evidence закоммичен в
[`datasets/logs/bench_auto_v2/`](../datasets/logs/bench_auto_v2/) —
per-SNR `compare.txt` плюс raw декодированный текст обоих декодеров.
На SNR ≤ −12 dB TouchRTTY выдаёт читаемый телетайп там, где у 2Tone
случайные буквы.

Инфраструктура, что прогоняла этот бенч (Win32 hwnd handshake с
N1MM Logger+, 2Tone'овский File→Save Text protocol, Voicemeeter
routing и пр.), была реверс-инженирлена для одноразового бенчмарка и
сюда не входит. Цель public-tree тулинга — воспроизводимость для
TouchRTTY-only экспериментов, а не полная воспроизводимость 2Tone
сетапа (он капризный и Windows-only). Закоммиченное evidence стоит
само по себе.

TouchRTTY-only бенчи (`nn_sweep_compare.py` и т.п.) работают
кросс-платформенно.

---

## Браузерный RTTY-генератор

[`tools/rtty_simulator.html`](../tools/rtty_simulator.html) — один
HTML-файл с полноценным FSK-генератором в браузере. Без сборки, без
Python — открываешь в любом современном браузере и получаешь:

* Живую RTTY-генерацию через WebAudio API
* Конфигурируемые baud / shift / centre / SNR / ISI mixing
* Опциональный CR/LF каждые N символов (соответствует ground-truth
  конвенции прошивки)
* Кнопку download для WAV

Я им пользуюсь как быстрым черновиком: визуально подбираю сложную
комбинацию параметров, слушаю её, потом либо играю через колонки в
микрофон Pico, либо скачиваю WAV и прогоняю через `bench_replay.py`
для измеримого A/B-теста.

Baudot ITA-2 таблица здесь та же, что в прошивке, — что видишь в
браузере, то увидит и декодер. Стоит открыть хотя бы раз — поиграй
со слайдером SNR, послушай, как звучит Baudot на −15 dB. Полезная
интуиция.
