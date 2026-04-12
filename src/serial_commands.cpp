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

    float val; int ival;
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
    else if (strcmp(cmd_buf, "INV AUTO") == 0) { shared_inv_auto = true; shared_inv_uncertain = false; printf(">> INV AUTO\n"); }
    else if (strcmp(cmd_buf, "INV NOR") == 0)  { shared_inv_auto = false; shared_rtty_inv = false; shared_inv_uncertain = false; printf(">> INV NOR (manual)\n"); }
    else if (strcmp(cmd_buf, "INV INV") == 0)  { shared_inv_auto = false; shared_rtty_inv = true; shared_inv_uncertain = false; printf(">> INV INV (manual)\n"); }
    else if (strcmp(cmd_buf, "INV ON") == 0)   { shared_inv_auto = false; shared_rtty_inv = true;  printf(">> INV ON (manual)\n"); }
    else if (strcmp(cmd_buf, "INV OFF") == 0)  { shared_inv_auto = false; shared_rtty_inv = false; printf(">> INV OFF (manual)\n"); }
    else if (strcmp(cmd_buf, "AFC ON") == 0)   { shared_afc_on = true;  printf(">> AFC ON\n"); }
    else if (strcmp(cmd_buf, "AFC OFF") == 0)  { shared_afc_on = false; printf(">> AFC OFF\n"); }
    else if (strcmp(cmd_buf, "CLEAR") == 0)    { shared_clear_dsp = true; printf(">> CLEAR\n"); }
    else if (strcmp(cmd_buf, "SAVE") == 0)     { shared_save_request = true; printf(">> SAVE REQUESTED\n"); }
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
    else if (strcmp(cmd_buf, "AGC ON") == 0)   { shared_agc_enabled = true;  printf(">> AGC ON\n"); }
    else if (strcmp(cmd_buf, "AGC OFF") == 0)  { shared_agc_enabled = false; printf(">> AGC OFF\n"); }
    else if (strcmp(cmd_buf, "SCALE EXP") == 0) { shared_exp_scale = true;  printf(">> SCALE EXP\n"); }
    else if (strcmp(cmd_buf, "SCALE LIN") == 0) { shared_exp_scale = false; printf(">> SCALE LIN\n"); }
    else if (strncmp(cmd_buf, "WIDTH ", 6) == 0) {
        int w = atoi(cmd_buf + 6);
        if (w >= 30 && w <= 120) { shared_line_width = w; printf(">> WIDTH=%d\n", w); }
        else printf(">> ERR: WIDTH 30-120\n");
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
        printf("SEARCH               Find RTTY signal\n");
        printf("STATUS               All parameters\n");
        printf("SAVE                 Save to flash\n");
        printf("CLEAR                Reset DSP state\n");
        printf("======================\n");
    }
    else { printf(">> UNKNOWN: %s (try HELP)\n", cmd_buf); }
    cmd_ptr = 0;
}
