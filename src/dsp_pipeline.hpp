// TouchRTTY
// Copyright (C) 2026 Alexander Lavrinovich (Alex.Electron)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

// Core 0 DSP pipeline entry point.
// Runs forever: ADC → AGC → biquad LPF → I/Q demod → ATC envelope →
// DPLL framer → baud/stop/inversion auto-detection → Baudot decode.
// Uses shared state declared in app_state.hpp.
void core0_dsp_loop();

// FIR coefficients and sin/cos tables (defined in dsp_pipeline.cpp).
#define FIR_TAPS 63
extern const float fir_coeffs[FIR_TAPS];
extern float sin_table[1024];
extern float cos_table[1024];
