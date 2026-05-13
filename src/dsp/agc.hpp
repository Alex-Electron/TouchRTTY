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
#include <math.h>

// Automatic Gain Control — RMS-envelope based, attack/release smoothing
typedef struct {
    float gain;
    float target;
    float attack;
    float release_inv; // Precomputed 1/release for multiply instead of divide
    float rms;
    float rms_tc;
    float rms_tc_inv;  // Precomputed (1 - rms_tc)
} agc_t;

inline void agc_init(agc_t *a, float fs) {
    a->gain        = 1.0f;
    a->target      = 0.30f;
    a->attack      = expf(-1.0f / (0.010f * fs));
    float release   = expf(-1.0f / (0.500f * fs));
    a->release_inv = 1.0f / release; // Precompute reciprocal
    a->rms_tc      = expf(-1.0f / (0.050f * fs));
    a->rms_tc_inv  = 1.0f - a->rms_tc;
    a->rms         = 0.01f;
}

inline float agc_process(agc_t *a, float x) {
    float out = x * a->gain;
    a->rms = a->rms * a->rms_tc + out * out * a->rms_tc_inv;
    float rms_now = sqrtf(a->rms + 1e-10f);
    if (rms_now > a->target) {
        a->gain *= a->attack;
    } else {
        a->gain *= a->release_inv; // Multiply instead of divide
    }
    a->gain = fmaxf(0.01f, fminf(a->gain, 200.0f));
    return out;
}
