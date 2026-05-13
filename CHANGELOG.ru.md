# Changelog: TouchRTTY (RP2350) — русская версия

> 🇬🇧 [Read in English](CHANGELOG.md)

Все заметные изменения проекта.

> **Примечание.** Полная история сборок ведётся в английской версии
> [`CHANGELOG.md`](CHANGELOG.md). Ранние записи (от B194 и старше)
> сохранены на смеси языков как исторический artefact. Здесь дублируем
> только последние релизы.

---

## [v2.0.0 — Phase 9 + TinyML NN] — 2026-05-12

Большой переписанный код относительно v1.72. Полные release notes —
[`RELEASE_v2.0.0.ru.md`](RELEASE_v2.0.0.ru.md).

### Главное

* **Обгоняет 2Tone 26.01a на низком SNR** в 3–6 раз по реальному error
  rate на одинаковом аудио. Multi-seed усреднённый AWGN-бенч на
  SNR −4..−22 dB.
* **Production-веса NN (v13)** едут с прошивкой: PyTorch-натренированный
  MLP с рецептом `weight_uncertain=3.0`. Улучшает SNR −14/−16/−20 dB на
  −1.9/−1.8/−9.1 pp относительно прошлых production-весов, σ < 4 pp
  между сидами.

### Добавлено

* **Архитектура Phase 9 hybrid decoder** — dual-IQ paths (узкая A,
  широкая B), сливаются через LLR (HYB), Soft-Viterbi frame validation
  gate, LMS adaptive notch chain, DPLL с PI-контроллером, AFC/AGC.
* **TinyML NN classifier** (`NN ON/OFF`) — 7→128→64→32 MLP, ~44 КБ
  весов, через B264 confidence-гейт работает только на неопределённых
  фреймах.
* **B265 DUMP FRAMES** serial-команда — поток per-frame soft-bit +
  label для capture real-air training data.
* **Tuning Lab** UI с phosphor-persistent eye diagram и живой
  подстройкой ALPHA / K / SQ.
* **3-bar top panel** с SIG / AGC / ERR (скользящее окно 100 фреймов).
* **Полноценная serial CLI** — 40+ команд, описаны в
  [`docs/SERIAL_COMMANDS.ru.md`](docs/SERIAL_COMMANDS.ru.md).
* **Воспроизводимый bench-тулинг** — PyTorch trainer, AWGN sweep с
  NN-OFF vs NN-ON, multi-seed aggregator, real-air bench. Всё в
  [`docs/BENCH_TOOLING.ru.md`](docs/BENCH_TOOLING.ru.md).
* **Браузерный RTTY-симулятор** (`tools/rtty_simulator.html`) для
  генерации сигнала без Python-стека.
* **Шесть длинных документов** в `docs/` про подключение железа,
  serial-команды, экранное меню, тренировку NN и bench-тулинг.

### Изменено

* **Build counter** теперь B265 (консолидирует B194..B265
  инкрементальной работы по Phase 9).
* **PATH** в UI меню теперь цикл по четырём состояниям
  (`A / B / HYB / HYB+NN`) вместо трёх; `HYB+NN` — рекомендованный
  production-сетап.
* **Рендеринг `[ERR]` на экране** свёрнут в одну красную `*` (B263).
  Полный токен `[ERR]` сохранён в serial.
* **NOTCH / VIT** перенесены из попапа в inline-тоглы меню — меньше
  тапов.

### Breaking

* **RP2040 больше не поддерживается.** Требуется Raspberry Pi Pico 2
  (RP2350) — используется FPU от M33 и > 150 КБ SRAM под FIR + FFT
  буферы.
* Старые per-phase planning docs (`PHASE1..PHASE7_*.md`) убраны из
  `docs/` (заменены реализацией Phase 9). Доступны в git history.

### Release artifact

`TouchRTTY_v2.0.0.uf2` — шить через `picotool` или BOOTSEL
drag-and-drop.

---

## История до v2.0.0

Все записи между v1.72 и v2.0.0 (билды B194..B265) идут в
английской версии [`CHANGELOG.md`](CHANGELOG.md). Это
day-by-day история работы по Phase 9 — много технических деталей,
которые относятся к разработке, а не к интерфейсу для конечного
пользователя. Если ищешь когда именно появилась какая-то конкретная
фича, лучшее место — английский CHANGELOG плюс
[`docs/PHASE9_HYBRID_DECODER_PLAN.ru.md`](docs/PHASE9_HYBRID_DECODER_PLAN.ru.md)
(§7a — execution log по билдам).
