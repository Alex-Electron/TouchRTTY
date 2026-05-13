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

#include "serial_commands.hpp"
#include "app_state.hpp"
#include "version.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

void handle_serial_commands() {
    static char cmd_buf[64];
    static int cmd_ptr = 0;
    static uint32_t last_char_time = 0;
    bool process = false;
    int c = getchar_timeout_us(0);
    if (c != PICO_ERROR_TIMEOUT) {
        last_char_time = time_us_32();
        if (c == '\n' || c == '\r') {
            process = (cmd_ptr > 0);
        } else if (cmd_ptr < 63) {
            cmd_buf[cmd_ptr++] = (char)c;
        }
    }
    // Timeout: if buffer has data and no new char for 500ms, treat as complete command
    if (!process && cmd_ptr > 0 && last_char_time > 0 && (time_us_32() - last_char_time) > 500000) {
        process = true;
    }
    if (!process) return;

    cmd_buf[cmd_ptr] = 0;
    last_char_time = 0;
    if (cmd_ptr == 0) { cmd_ptr = 0; return; }

    float val, val2; int ival;
    if (sscanf(cmd_buf, "ALPHA %f", &val) == 1) {
        val = fmaxf(0.005f, fminf(0.200f, val));
        tuning_dpll_alpha = val; printf(">> ALPHA=%.4f\n", val);
    }
    else if (sscanf(cmd_buf, "BW %f", &val) == 1 || sscanf(cmd_buf, "K %f", &val) == 1) {
        val = fmaxf(0.3f, fminf(2.0f, val));
        tuning_lpf_k = val; printf(">> BW=%.2f\n", val);
    }
    else if (sscanf(cmd_buf, "SQ %f", &val) == 1) {
        tuning_sq_snr = val; printf(">> SQ=%.1f\n", val);
    }
    else if (strcmp(cmd_buf, "BAUD AUTO") == 0) {
        shared_baud_idx = 4; shared_baud_auto = true;
        shared_baud_detect_req = true; shared_baud_detect_state = 1;
        printf(">> BAUD=AUTO (detecting...)\n");
    }
    else if (sscanf(cmd_buf, "BAUD %d", &ival) == 1) {
        if (ival >= 0 && ival <= 3) { shared_baud_idx = ival; shared_baud_auto = false; printf(">> BAUD=%d\n", ival); }
        else if (ival == 4) { shared_baud_idx = 4; shared_baud_auto = true; shared_baud_detect_req = true; shared_baud_detect_state = 1; printf(">> BAUD=AUTO\n"); }
        else printf(">> ERR: BAUD 0-4 (45/50/75/100/AUTO)\n");
    }
    else if (strcmp(cmd_buf, "SHIFT AUTO") == 0) {
        shared_shift_idx = NUM_SHIFTS; printf(">> SHIFT=AUTO\n");
    }
    else if (sscanf(cmd_buf, "SHIFT %d", &ival) == 1) {
        if (ival >= 0 && ival <= NUM_SHIFTS) { shared_shift_idx = ival; printf(">> SHIFT=%d%s\n", ival, ival==NUM_SHIFTS?" (AUTO)":""); }
        else printf(">> ERR: SHIFT 0-%d or AUTO (85/170/200/340/425/450/500/850/AUTO)\n", NUM_SHIFTS);
    }
    else if (sscanf(cmd_buf, "FREQ %f", &val) == 1) {
        shared_target_freq = val; shared_actual_freq = val;
        printf(">> FREQ=%.1f\n", val);
    }
    else if (strcmp(cmd_buf, "DIAG ON") == 0)  { shared_serial_diag = true;  printf(">> DIAG ON\n"); }
    else if (strcmp(cmd_buf, "DIAG OFF") == 0) { shared_serial_diag = false; printf(">> DIAG OFF\n"); }
    else if (strcmp(cmd_buf, "DUMP FRAMES ON") == 0)  { shared_dump_frames = true;  printf(">> DUMP FRAMES ON\n"); }
    else if (strcmp(cmd_buf, "DUMP FRAMES OFF") == 0) { shared_dump_frames = false; printf(">> DUMP FRAMES OFF\n"); }
    else if (strcmp(cmd_buf, "INV AUTO") == 0) { shared_inv_auto = true; shared_inv_uncertain = false; printf(">> INV AUTO\n"); }
    else if (strcmp(cmd_buf, "INV NOR") == 0)  { shared_inv_auto = false; shared_rtty_inv = false; shared_inv_uncertain = false; printf(">> INV NOR (manual)\n"); }
    else if (strcmp(cmd_buf, "INV INV") == 0)  { shared_inv_auto = false; shared_rtty_inv = true; shared_inv_uncertain = false; printf(">> INV INV (manual)\n"); }
    else if (strcmp(cmd_buf, "INV ON") == 0)   { shared_inv_auto = false; shared_rtty_inv = true;  printf(">> INV ON (manual)\n"); }
    else if (strcmp(cmd_buf, "INV OFF") == 0)  { shared_inv_auto = false; shared_rtty_inv = false; printf(">> INV OFF (manual)\n"); }
    else if (strcmp(cmd_buf, "AFC ON") == 0)   { shared_afc_on = true;  printf(">> AFC ON\n"); }
    else if (strcmp(cmd_buf, "AFC OFF") == 0)  { shared_afc_on = false; printf(">> AFC OFF\n"); }
    else if (strcmp(cmd_buf, "PATH A") == 0)   { shared_decoder_path = 0; shared_nn_enable = false; printf(">> PATH=A (narrow)\n"); }
    else if (strcmp(cmd_buf, "PATH B") == 0)   { shared_decoder_path = 1; shared_nn_enable = false; printf(">> PATH=B (wide)\n"); }
    else if (strcmp(cmd_buf, "PATH HYB") == 0 || strcmp(cmd_buf, "PATH LLR") == 0) {
        shared_decoder_path = 2; shared_nn_enable = false; printf(">> PATH=HYB (LLR %s)\n",
            shared_dyn_fusion ? "dynamic" : "geomean");
    }
    else if (strcmp(cmd_buf, "DYN ON") == 0)  { shared_dyn_fusion = true;  printf(">> DYN ON (SNR-weighted LLR)\n"); }
    else if (strcmp(cmd_buf, "DYN OFF") == 0) { shared_dyn_fusion = false; printf(">> DYN OFF (equal-weight geomean)\n"); }
    else if (strcmp(cmd_buf, "NR ON") == 0)   { shared_spectral_nr = true;  printf(">> NR ON (Wiener spectral noise reduction)\n"); }
    else if (strcmp(cmd_buf, "NR OFF") == 0)  { shared_spectral_nr = false; printf(">> NR OFF (raw power)\n"); }
    else if (sscanf(cmd_buf, "WEIGHTS %f %f", &val, &val2) == 2) {
        float wa = val, wb = val2;
        float s = wa + wb;
        if (s <= 1e-6f || wa < 0.0f || wb < 0.0f) {
            printf(">> ERR: WEIGHTS wa wb — both >=0 and sum>0\n");
        } else {
            shared_fuse_wa = wa / s;
            shared_fuse_wb = wb / s;
            printf(">> WEIGHTS A=%.3f B=%.3f (normalized)\n",
                   (double)shared_fuse_wa, (double)shared_fuse_wb);
        }
    }
    else if (strcmp(cmd_buf, "CLEAR") == 0)    { shared_clear_dsp = true; printf(">> CLEAR\n"); }
    else if (strcmp(cmd_buf, "SAVE") == 0)     { shared_save_request = true; printf(">> SAVE REQUESTED\n"); }
    else if (strcmp(cmd_buf, "VERSION") == 0 || strcmp(cmd_buf, "VER") == 0 || strcmp(cmd_buf, "ID") == 0) {
        printf("\n>> TouchRTTY Phase9 B%d (built %s %s)\n",
               BUILD_NUMBER, __DATE__, __TIME__);
    }
    else if (strcmp(cmd_buf, "STATUS") == 0) {
        const int bauds_t[] = {45, 50, 75, 100};
        printf("\n=== STATUS (B%d) ===\n", BUILD_NUMBER);
        printf("ALPHA=%.4f BW=%.2f SQ=%.1f\n", (double)tuning_dpll_alpha, (double)tuning_lpf_k, (double)tuning_sq_snr);
        const char* inv_str = shared_inv_auto ? (shared_rtty_inv ? "AUTO(INV)" : "AUTO(NOR)") : (shared_rtty_inv ? "INV" : "NOR");
        const char* baud_str_s = shared_baud_auto ? "AUTO" : "";
        int baud_val = shared_baud_auto ? (int)shared_active_baud : bauds_t[shared_baud_idx];
        if (shared_shift_idx < NUM_SHIFTS)
            printf("BAUD=%s%d SHIFT=%d(%d) INV=%s AFC=%s\n",
                baud_str_s, baud_val,
                shared_shift_idx, g_shifts_int[shared_shift_idx],
                inv_str, shared_afc_on ? "ON" : "OFF");
        else
            printf("BAUD=%s%d SHIFT=AUTO INV=%s AFC=%s\n",
                baud_str_s, baud_val,
                inv_str, shared_afc_on ? "ON" : "OFF");
        printf("FREQ=%.1f SNR=%.1f SIG=%.1f AGC=%.2f\n",
            (double)shared_actual_freq, (double)shared_snr_db,
            (double)shared_signal_db, (double)shared_agc_gain);
        const char* stop_str = shared_stop_auto ? "AUTO" : (shared_stop_idx==0?"1.0":shared_stop_idx==1?"1.5":"2.0");
        printf("STOP=%s(%.1f) SQ=%s ERR=%.0f%% DIAG=%s\n",
            stop_str, (double)shared_active_stop,
            shared_squelch_open ? "OPEN" : "SHUT",
            (double)shared_err_rate,
            shared_serial_diag ? "ON" : "OFF");
        printf("STOP-DET: gap_last=%.2fT hist[1.0/1.5/2.0]=%d/%d/%d\n",
            (double)shared_stop_gap_last,
            shared_stop_gap_hist[0], shared_stop_gap_hist[1], shared_stop_gap_hist[2]);
        printf("NN=%s NOTCH=%s VIT=%s\n",
               shared_nn_enable    ? "ON" : "OFF",
               shared_notch_enable ? "ON" : "OFF",
               shared_vit_enable   ? "ON" : "OFF");
        printf("====================\n");
    }
    else if (strcmp(cmd_buf, "STOP AUTO") == 0) {
        shared_stop_idx = 3; shared_stop_auto = true;
        shared_stop_detect_req = true; shared_stop_detect_state = 1;
        printf(">> STOP=AUTO (detecting...)\n");
    }
    else if (sscanf(cmd_buf, "STOP %d", &ival) == 1) {
        if (ival >= 0 && ival <= 2) { shared_stop_idx = ival; shared_stop_auto = false; printf(">> STOP=%d (%.1f bits)\n", ival, ival==0?1.0f:ival==1?1.5f:2.0f); }
        else printf(">> ERR: STOP 0-2 or AUTO (1.0/1.5/2.0/AUTO)\n");
    }
    else if (strcmp(cmd_buf, "NN ON") == 0)    { shared_nn_enable = true;  flag_settings_change(); printf(">> NN ON (TinyML classifier)\n"); }
    else if (strcmp(cmd_buf, "NN OFF") == 0)   { shared_nn_enable = false; flag_settings_change(); printf(">> NN OFF (hard threshold)\n"); }
    else if (strcmp(cmd_buf, "NOTCH ON") == 0) { shared_notch_enable = true;  flag_settings_change(); printf(">> NOTCH ON (LMS chain)\n"); }
    else if (strcmp(cmd_buf, "NOTCH OFF") == 0){ shared_notch_enable = false; flag_settings_change(); printf(">> NOTCH OFF (bypass)\n"); }
    else if (strcmp(cmd_buf, "VIT ON") == 0)   { shared_vit_enable = true;    flag_settings_change(); printf(">> VIT ON (Soft-Viterbi frame gate)\n"); }
    else if (strcmp(cmd_buf, "VIT OFF") == 0)  { shared_vit_enable = false;   flag_settings_change(); printf(">> VIT OFF (stop-bit only)\n"); }
    else if (strcmp(cmd_buf, "AGC ON") == 0)   { shared_agc_enabled = true;  printf(">> AGC ON\n"); }
    else if (strcmp(cmd_buf, "AGC OFF") == 0)  { shared_agc_enabled = false; printf(">> AGC OFF\n"); }
    else if (strcmp(cmd_buf, "SCALE EXP") == 0) { shared_exp_scale = true;  printf(">> SCALE EXP\n"); }
    else if (strcmp(cmd_buf, "SCALE LIN") == 0) { shared_exp_scale = false; printf(">> SCALE LIN\n"); }
    else if (strncmp(cmd_buf, "WIDTH ", 6) == 0) {
        int w = atoi(cmd_buf + 6);
        if (w >= 30 && w <= 120) { shared_line_width = w; printf(">> WIDTH=%d\n", w); }
        else printf(">> ERR: WIDTH 30-120\n");
    }
    else if (strcmp(cmd_buf, "DUMP SPEC") == 0) {
        printf("\n=== SPEC DUMP (B%d, bin_hz=%.2f) ===\n", BUILD_NUMBER, (double)(SAMPLE_RATE/(float)FFT_SIZE));
        printf("freq=%.1f SNR=%.1f SIG=%.1f noise_floor=TS\n",
            (double)shared_actual_freq, (double)shared_snr_db, (double)shared_signal_db);
        for (int i = 0; i < FFT_SIZE/2; i += 4) {
            printf("%d:%.1f,%.1f,%.1f,%.1f\n", i,
                (double)shared_fft_mag[i],   (double)shared_fft_mag[i+1],
                (double)shared_fft_mag[i+2], (double)shared_fft_mag[i+3]);
        }
        printf("=== SPEC END ===\n");
    }
    else if (strcmp(cmd_buf, "DUMP MS") == 0) {
        printf("\n=== MS DUMP (Mark/Space envelopes, 480 samples) ===\n");
        for (int i = 0; i < 480; i += 8) {
            printf("%d:", i);
            for (int k = 0; k < 8; k++)
                printf("%.2f,%.2f|", (double)shared_mag_m[i+k], (double)shared_mag_s[i+k]);
            printf("\n");
        }
        printf("=== MS END ===\n");
    }
    else if (strcmp(cmd_buf, "SEARCH") == 0) {
        shared_rtty_inv = false; shared_afc_on = true;
        shared_search_request = true; shared_search_state = 1;
        printf(">> SEARCHING...\n");
    }
    else if (strcmp(cmd_buf, "HELP") == 0) {
        printf("\n=== COMMANDS (B%d) ===\n", BUILD_NUMBER);
        printf("--- Tuning ---\n");
        printf("ALPHA <0.005-0.200>  DPLL loop bandwidth\n");
        printf("BW <0.3-2.0>         LPF filter K\n");
        printf("SQ <dB>              Squelch SNR threshold\n");
        printf("FREQ <Hz>            Center frequency\n");
        printf("--- Protocol ---\n");
        printf("BAUD <0-4>|AUTO      0=45 1=50 2=75 3=100 4/AUTO=auto\n");
        printf("SHIFT <0-8>          85/170/200/340/425/450/500/850/AUTO\n");
        printf("STOP <0-2>|AUTO      1.0/1.5/2.0/AUTO bits\n");
        printf("INV AUTO|NOR|INV     Inversion: auto/manual\n");
        printf("--- Control ---\n");
        printf("AFC ON|OFF           Auto frequency\n");
        printf("AGC ON|OFF           Auto gain\n");
        printf("SCALE EXP|LIN        Waterfall scale\n");
        printf("WIDTH <30-120>       Text line width\n");
        printf("DIAG ON|OFF          Diagnostic stream\n");
        printf("DUMP SPEC            Dump FFT spectrum (512 bins)\n");
        printf("DUMP MS              Dump Mark/Space envelopes (480 samples)\n");
        printf("SEARCH               Find RTTY signal\n");
        printf("STATUS               All parameters\n");
        printf("SAVE                 Save to flash\n");
        printf("CLEAR                Reset DSP state\n");
        printf("VERSION (VER, ID)    Print firmware build identifier\n");
        printf("PATH A|B|HYB         Dual-IQ decoder path (B246+)\n");
        printf("NN ON|OFF            TinyML neural net classifier (B261+)\n");
        printf("NOTCH ON|OFF         LMS notch chain bypass (B262+)\n");
        printf("VIT ON|OFF           Soft-Viterbi frame gate bypass (B262+)\n");
        printf("======================\n");
    }
    else { printf(">> UNKNOWN: %s (try HELP)\n", cmd_buf); }
    cmd_ptr = 0;
}
