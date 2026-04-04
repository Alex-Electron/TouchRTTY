# TouchRTTY: Master Session Context (Save State)
**Last updated:** 2026-04-04
**Branch:** `feat/alex-cl-dev`
**Current build:** 194

## 1. PROJECT STATUS
- **DSP:** Hardware ADC FIFO (10kHz), FIR→AGC→I/Q Demod→ATC→DPLL→Baudot pipeline on Core 0 (~7% load)
- **UI:** 3 thin bars (SIG/AGC/ERR) in top panel, waterfall/spectrum/Lissajous, Tuning Lab with eye diagram
- **Tuning Lab:** Eye diagram with phosphor persistence, ALPHA/BW/SQ controls, DUMP:ON/OFF, SAVE
- **Serial:** 15 commands for full remote control (type HELP), diagnostic stream with [D] prefix
- **Fonts:** NORM (Font2, 17px) and NARW (Font0, 10px)

## 2. ENGINEERING RULES
1. **Surgical Edits Only:** Never overwrite main.cpp entirely.
2. **Strict Float Policy:** Only `float` constants (`f`) and functions (`sinf`, `cosf`).
3. **No __wfe() with ADC FIFO:** Use `tight_loop_contents()` — no ADC IRQ configured.
4. **Flash via picotool:** `picotool load build/TouchRTTY.uf2 -f && picotool reboot`
5. **-flto incompatible** with Pico SDK `__wrap_` symbols.

## 3. BUILD HISTORY (recent)
### Build 194: Tuning Lab + Serial Commands
- Tuning Lab screen with eye diagram (phosphor persistence, 240x64)
- Full serial command system (15 commands)
- Menu simplified (BW/SQ/SAVE moved to Tuning Lab)
- Boot recalibration fix (short press = recal, long = factory reset)
- Reset confirm dialog fix (RTTY chars no longer overwrite it)

### Build 191: Error Rate + Reception Fix
- Error rate indicator (100-char sliding window) in top bar
- 3 thin bars: SIG, AGC (dB), ERR (%)
- Fix: reverted FFT to Core 1 (was blocking ADC on Core 0)
- Fix: __wfe() → tight_loop_contents() for ADC wait

### Build 190: ADC FIFO + Optimizations
- Hardware ADC FIFO, ping-pong DMA, fast_log2f(), AGC precompute

### Build 189: Strict Float (committed)
### Build 188: Hardware-accurate colors (committed)

## 4. ROADMAP
### Done
- [x] Eye diagram with DPLL-synchronized phosphor persistence
- [x] Tuning Lab with parameter controls
- [x] Serial command system (full remote control)
- [x] Error rate indicator
- [x] Hardware ADC FIFO
- [x] Ping-pong DMA

### Planned
- [ ] Refactor main.cpp into logical modules (DSP, UI, Baudot, shared state)
- [ ] Hardware scroll ILI9488 (offload Core 1)
- [ ] CMSIS-DSP vectorized FIR/Biquad
- [ ] SD-Card logging (Phase 4)

## 5. NEW SESSION INSTRUCTIONS
*Load this file and say: "Analyze SESSION_CONTEXT.md, branch feat/alex-cl-dev, build 194. Ready to continue."*
