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
