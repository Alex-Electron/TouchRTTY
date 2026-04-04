#include <stdio.h>
#include <math.h>
#include <algorithm>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/spi.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/timer.h"
#include "hardware/uart.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "LGFX_Config.hpp"
#include "display/ili9488_driver.h"
#include "dsp/fft.hpp"
#include "dsp/biquad.hpp"
#include "dsp/dpll_framer.hpp"
#include "ui/UIManager.hpp"
#include "version.h"

#define ADC_PIN 26
#define SAMPLE_RATE 10000
#define ENC_SW 4

#define CAL_FLASH_OFFSET (1024 * 1024 * 2) // 2MB offset
#define SETTINGS_FLASH_OFFSET (1024 * 1024 * 2 + FLASH_SECTOR_SIZE)
const uint8_t* flash_target_contents = (const uint8_t *) (XIP_BASE + CAL_FLASH_OFFSET);
const uint8_t* flash_settings_contents = (const uint8_t *) (XIP_BASE + SETTINGS_FLASH_OFFSET);

struct AppSettings {
    uint32_t magic;
    int baud_idx;
    int shift_idx;
    int stop_idx;
    bool rtty_inv;
    int display_mode;
    bool exp_scale;
    bool auto_scale;
    float filter_k;
    float sq_snr;
    float target_freq;
    bool serial_diag;
    int line_width;
    bool afc_on;
    int font_mode;
    float dpll_alpha;
};

volatile bool settings_need_save = false;
volatile uint32_t settings_last_change = 0;
volatile int shared_line_width = 60; 
volatile bool shared_afc_on = true;
volatile bool shared_exp_scale = true;  
volatile int shared_font_mode = 0; // 0: Font2, 1: Font0 x2

void flag_settings_change() {
    settings_need_save = true;
    settings_last_change = time_us_32();
}

volatile bool shared_force_cal = false;

void load_or_calibrate(lgfx::LGFX_Device& tft, bool force = false) {
    uint16_t calData[8];
    bool valid = true;
    for(int i=0; i<8; i++) {
        calData[i] = ((uint16_t*)flash_target_contents)[i];
        if (calData[i] == 0xFFFF || calData[i] == 0) valid = false;
    }
    
    if (valid && !force) {
        tft.setTouchCalibrate(calData);
    } else {
        tft.fillScreen(0x000000U);
        tft.setTextColor(0xFFFFFFU, 0x000000U);
        tft.setTextSize(2);
        tft.setTextDatum(middle_center);
        tft.drawString("TOUCH 4 CORNERS TO CALIBRATE", 240, 160);
        tft.calibrateTouch(calData, 0xFFFFFFU, 0x000000U, 15);
        
        uint8_t page_buf[FLASH_PAGE_SIZE] = {0};
        memcpy(page_buf, calData, 16);
        
        multicore_lockout_start_blocking();
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(CAL_FLASH_OFFSET, FLASH_SECTOR_SIZE);
        flash_range_program(CAL_FLASH_OFFSET, page_buf, FLASH_PAGE_SIZE);
        restore_interrupts(ints);
        multicore_lockout_end_blocking();
        
        tft.fillScreen(0x000000U);
    }
}

volatile float shared_fft_mag[FFT_SIZE / 2];
volatile float shared_adc_waveform[480];
volatile float shared_fft_ts[FFT_SIZE];
volatile bool new_data_ready = false;
volatile float shared_adc_v = 0.0f;
volatile float shared_signal_db = -80.0f;
volatile bool shared_adc_clipping = false;
volatile float shared_snr_db = 0.0f;
volatile float shared_err_rate = 0.0f; // Error rate 0-100% over last 100 chars
volatile float shared_core0_load = 0.0f;
volatile float shared_core1_load = 0.0f;

volatile float shared_mag_m[480];
volatile float shared_mag_s[480];

volatile char rtty_new_char = 0;
volatile bool rtty_char_ready = false;
volatile bool shared_err_flag = false;
volatile bool shared_figs_flag = false;
volatile bool shared_ltrs_flag = false;

volatile float shared_diag_adc_min = 4096.0f;
volatile float shared_diag_adc_max = 0.0f;
volatile float shared_atc_m = 0.0f;
volatile float shared_atc_s = 0.0f;
volatile float shared_dpll_phase = 0.0f;
volatile float shared_dpll_ferr = 0.0f; // DPLL frequency error for diagnostics
volatile bool shared_diag_ready = false;

volatile float shared_target_freq = 1535.0f;
volatile float shared_actual_freq = 1535.0f;
volatile int shared_baud_idx = 0;
volatile int shared_shift_idx = 0;
volatile int shared_stop_idx = 1;
volatile bool shared_rtty_inv = false;
volatile bool shared_squelch_open = false;
volatile bool shared_clear_dsp = false;

// Eye diagram buffer: 16 overlaid bit periods, up to 256 samples per bit
#define EYE_TRACES 16
#define EYE_MAX_SPB 256
volatile int8_t shared_eye_buf[EYE_TRACES][EYE_MAX_SPB]; // D values scaled to -127..+127
volatile int shared_eye_spb = 220; // samples per bit (SAMPLE_RATE/baud)
volatile int shared_eye_idx = 0;   // current trace index (0..EYE_TRACES-1)
volatile bool shared_eye_ready = false;

// Tuning Globals
volatile float tuning_dpll_alpha = 0.035f;
volatile float tuning_lpf_k = 0.75f;
volatile float tuning_sq_snr = 6.0f;
volatile float shared_agc_gain = 1.0f;
volatile bool shared_agc_enabled = true;
volatile bool shared_serial_diag = false; // Toggle via Diagnostic Screen or serial command
volatile bool shared_save_request = false; // Save settings from serial command
volatile bool shared_search_request = false; // Signal search requested by SEARCH button
volatile int shared_search_state = 0; // 0=idle, 1=searching, 2=found, 3=not found

void handle_serial_commands() {
    static char cmd_buf[64];
    static int cmd_ptr = 0;
    int c = getchar_timeout_us(0);
    if (c != PICO_ERROR_TIMEOUT) {
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_ptr] = 0;
            if (cmd_ptr > 0) {
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
                else if (sscanf(cmd_buf, "BAUD %d", &ival) == 1) {
                    if (ival >= 0 && ival <= 2) { shared_baud_idx = ival; printf(">> BAUD=%d\n", ival); }
                    else printf(">> ERR: BAUD 0-2 (45/50/75)\n");
                }
                else if (sscanf(cmd_buf, "SHIFT %d", &ival) == 1) {
                    if (ival >= 0 && ival <= 4) { shared_shift_idx = ival; printf(">> SHIFT=%d\n", ival); }
                    else printf(">> ERR: SHIFT 0-4 (170/200/425/450/850)\n");
                }
                else if (sscanf(cmd_buf, "FREQ %f", &val) == 1) {
                    shared_target_freq = val; shared_actual_freq = val;
                    printf(">> FREQ=%.1f\n", val);
                }
                else if (strcmp(cmd_buf, "DIAG ON") == 0)  { shared_serial_diag = true;  printf(">> DIAG ON\n"); }
                else if (strcmp(cmd_buf, "DIAG OFF") == 0) { shared_serial_diag = false; printf(">> DIAG OFF\n"); }
                else if (strcmp(cmd_buf, "INV ON") == 0)   { shared_rtty_inv = true;  printf(">> INV ON\n"); }
                else if (strcmp(cmd_buf, "INV OFF") == 0)  { shared_rtty_inv = false; printf(">> INV OFF\n"); }
                else if (strcmp(cmd_buf, "AFC ON") == 0)   { shared_afc_on = true;  printf(">> AFC ON\n"); }
                else if (strcmp(cmd_buf, "AFC OFF") == 0)  { shared_afc_on = false; printf(">> AFC OFF\n"); }
                else if (strcmp(cmd_buf, "CLEAR") == 0)    { shared_clear_dsp = true; printf(">> CLEAR\n"); }
                else if (strcmp(cmd_buf, "SAVE") == 0)     { shared_save_request = true; printf(">> SAVE REQUESTED\n"); }
                else if (strcmp(cmd_buf, "STATUS") == 0) {
                    const int bauds_t[] = {45, 50, 75};
                    const int shifts_t[] = {170, 200, 425, 450, 850};
                    printf("\n=== STATUS (B%d) ===\n", BUILD_NUMBER);
                    printf("ALPHA=%.4f BW=%.2f SQ=%.1f\n", (double)tuning_dpll_alpha, (double)tuning_lpf_k, (double)tuning_sq_snr);
                    printf("BAUD=%d(%d) SHIFT=%d(%d) INV=%s AFC=%s\n",
                        shared_baud_idx, bauds_t[shared_baud_idx],
                        shared_shift_idx, shifts_t[shared_shift_idx],
                        shared_rtty_inv ? "ON" : "OFF", shared_afc_on ? "ON" : "OFF");
                    printf("FREQ=%.1f SNR=%.1f SIG=%.1f AGC=%.2f\n",
                        (double)shared_actual_freq, (double)shared_snr_db,
                        (double)shared_signal_db, (double)shared_agc_gain);
                    printf("SQ=%s ERR=%.0f%% DIAG=%s\n",
                        shared_squelch_open ? "OPEN" : "SHUT",
                        (double)shared_err_rate,
                        shared_serial_diag ? "ON" : "OFF");
                    printf("====================\n");
                }
                else if (sscanf(cmd_buf, "STOP %d", &ival) == 1) {
                    if (ival >= 0 && ival <= 2) { shared_stop_idx = ival; printf(">> STOP=%d\n", ival); }
                    else printf(">> ERR: STOP 0-2 (1.0/1.5/2.0)\n");
                }
                else if (strcmp(cmd_buf, "AGC ON") == 0)   { shared_agc_enabled = true;  printf(">> AGC ON\n"); }
                else if (strcmp(cmd_buf, "AGC OFF") == 0)  { shared_agc_enabled = false; printf(">> AGC OFF\n"); }
                else if (strcmp(cmd_buf, "SCALE EXP") == 0) { shared_exp_scale = true;  printf(">> SCALE EXP\n"); }
                else if (strcmp(cmd_buf, "SCALE LIN") == 0) { shared_exp_scale = false; printf(">> SCALE LIN\n"); }
                else if (strncmp(cmd_buf, "WIDTH ", 6) == 0) {
                    int ival = atoi(cmd_buf + 6);
                    if (ival >= 30 && ival <= 120) { shared_line_width = ival; printf(">> WIDTH=%d\n", ival); }
                    else printf(">> ERR: WIDTH 30-120\n");
                }
                else if (strcmp(cmd_buf, "SEARCH") == 0) { shared_search_request = true; shared_search_state = 1; printf(">> SEARCHING...\n"); }
                else if (strcmp(cmd_buf, "HELP") == 0) {
                    printf("\n=== COMMANDS (B%d) ===\n", BUILD_NUMBER);
                    printf("--- Tuning ---\n");
                    printf("ALPHA <0.005-0.200>  DPLL loop bandwidth\n");
                    printf("BW <0.3-2.0>         LPF filter K\n");
                    printf("SQ <dB>              Squelch SNR threshold\n");
                    printf("FREQ <Hz>            Center frequency\n");
                    printf("--- Protocol ---\n");
                    printf("BAUD <0-2>           0=45 1=50 2=75\n");
                    printf("SHIFT <0-4>          170/200/425/450/850\n");
                    printf("STOP <0-2>           1.0/1.5/2.0 bits\n");
                    printf("INV ON|OFF           Mark/Space invert\n");
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
            }
            cmd_ptr = 0;
        } else if (cmd_ptr < 63) {
            cmd_buf[cmd_ptr++] = (char)c;
        }
    }
}

const char ita2_ltrs[32] = {
    '\0', 'E', '\n', 'A', ' ', 'S', 'I', 'U', 
    '\r', 'D', 'R', 'J', 'N', 'F', 'C', 'K', 
    'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q', 
    'O', 'B', 'G', '\0', 'M', 'X', 'V', '\0'
};
const char ita2_figs[32] = {
    '\0', '3', '\n', '-', ' ', '\'', '8', '7', 
    '\r', '$', '4', '\'', ',', '!', ':', '(', 
    '5', '\"', ')', '2', '#', '6', '0', '1', 
    '9', '?', '&', '\0', '.', '/', '=', '\0'
};

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

float sin_table[1024];
float cos_table[1024];

#define FIR_TAPS 63
const float fir_coeffs[FIR_TAPS] = {
    0.000167f, 0.000000f, 0.001438f, 0.000137f,
    -0.000722f, 0.001740f, -0.000000f, -0.002612f,
    0.001612f, -0.000447f, -0.006590f, 0.000000f,
    -0.001293f, -0.013294f, -0.004198f, -0.002266f,
    -0.022758f, -0.011889f, -0.002468f, -0.034228f,
    -0.023822f, -0.000000f, -0.046225f, -0.041377f,
    0.009206f, -0.056824f, -0.070578f, 0.038381f,
    -0.064124f, -0.160830f, 0.247760f, 0.600546f,
    0.247760f, -0.160830f, -0.064124f, 0.038381f,
    -0.070578f, -0.056824f, 0.009206f, -0.041377f,
    -0.046225f, -0.000000f, -0.023822f, -0.034228f,
    -0.002468f, -0.011889f, -0.022758f, -0.002266f,
    -0.004198f, -0.013294f, -0.001293f, 0.000000f,
    -0.006590f, -0.000447f, 0.001612f, -0.002612f,
    -0.000000f, 0.001740f, -0.000722f, 0.000137f,
    0.001438f, 0.000000f, 0.000167f,
};

void core1_main() {
    LGFX_RP2350 tft; tft.init(); tft.setRotation(1);

    load_or_calibrate(tft, shared_force_cal);

    ili9488_init();    ili9488_fill_screen(0x0000);
    UIManager ui(&tft); ui.init();
    
    bool auto_scale = true;
    bool menu_mode = false;
    bool diag_screen_active = false;
    bool tuning_lab_active = false;
    int display_mode = 0;
    bool reset_confirm_mode = false;
    uint32_t saved_text_timer = 0;
    
    AppSettings loaded_set;
    memcpy(&loaded_set, flash_settings_contents, sizeof(AppSettings));
    if (loaded_set.magic == 0xDEADBEEF) {
        shared_baud_idx = loaded_set.baud_idx;
        shared_shift_idx = loaded_set.shift_idx;
        shared_stop_idx = loaded_set.stop_idx;
        shared_rtty_inv = loaded_set.rtty_inv;
        display_mode = loaded_set.display_mode;
        shared_exp_scale = loaded_set.exp_scale;
        auto_scale = loaded_set.auto_scale;
        tuning_lpf_k = loaded_set.filter_k;
        tuning_sq_snr = loaded_set.sq_snr;
        shared_target_freq = loaded_set.target_freq;
        shared_actual_freq = shared_target_freq;
        shared_serial_diag = loaded_set.serial_diag; 
        shared_line_width = (loaded_set.line_width >= 30 && loaded_set.line_width <= 80) ? loaded_set.line_width : 60;
        shared_afc_on = loaded_set.afc_on;
        shared_font_mode = loaded_set.font_mode;
        if (loaded_set.dpll_alpha >= 0.005f && loaded_set.dpll_alpha <= 0.200f)
            tuning_dpll_alpha = loaded_set.dpll_alpha;
    }
    
    const float bauds[] = {45.45f, 50.0f, 75.0f};
    const float shifts[] = {170.0f, 200.0f, 425.0f, 450.0f, 850.0f};
    const float stop_bits[] = {1.0f, 1.5f, 2.0f};
    
    ui.drawBottomBar(shared_baud_idx, shared_shift_idx, stop_bits[shared_stop_idx], shared_afc_on, menu_mode, shared_search_state);

    LGFX_Sprite spectrum(&tft); spectrum.setColorDepth(16); spectrum.createSprite(480, UI_DSP_ZONE_H);
    LGFX_Sprite marker_spr(&tft); marker_spr.setColorDepth(16); marker_spr.createSprite(480, UI_MARKER_H);
    
    const int bin_start = 5, bin_end = 358;
    const float bin_per_pixel = (float)(bin_end - bin_start) / 480.0f;
    uint32_t last_touch = time_us_32(), last_ui_update = time_us_32(), frame_count = 0;
    float local_mag[FFT_SIZE / 2], local_wave[480], local_mag_m[480], local_mag_s[480], local_ts[FFT_SIZE];
    int16_t tune_x = 240;
    float ui_noise_floor = -60.0f, ui_gain = 0.0f;
    static float smooth_mag[FFT_SIZE / 2] = {0};

    static SimpleFFT fft; static float real[FFT_SIZE], imag[FFT_SIZE], mag[FFT_SIZE/2];

    uint32_t c1_total_work = 0;
    uint32_t search_result_time = 0;
    uint32_t c1_last_measure = time_us_32();

    while (true) {
        uint32_t loop_start = time_us_32();
        handle_serial_commands();

        // Handle save request from serial command
        if (shared_save_request) {
            shared_save_request = false;
            AppSettings s;
            s.magic = 0xDEADBEEF;
            s.baud_idx = shared_baud_idx; s.shift_idx = shared_shift_idx;
            s.stop_idx = shared_stop_idx; s.rtty_inv = shared_rtty_inv;
            s.display_mode = display_mode; s.exp_scale = shared_exp_scale;
            s.auto_scale = auto_scale; s.filter_k = tuning_lpf_k;
            s.sq_snr = tuning_sq_snr; s.target_freq = shared_target_freq;
            s.serial_diag = shared_serial_diag; s.line_width = shared_line_width;
            s.afc_on = shared_afc_on; s.font_mode = shared_font_mode; s.dpll_alpha = tuning_dpll_alpha;             uint8_t page_buf[FLASH_PAGE_SIZE] = {0};
            memcpy(page_buf, &s, sizeof(AppSettings));
            multicore_lockout_start_blocking();
            uint32_t ints = save_and_disable_interrupts();
            flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
            flash_range_program(SETTINGS_FLASH_OFFSET, page_buf, FLASH_PAGE_SIZE);
            restore_interrupts(ints);
            multicore_lockout_end_blocking();
            settings_need_save = false;
            printf(">> SAVED OK\n");
        }

        if (rtty_char_ready) {
            char c = rtty_new_char;
            ui.addRTTYChar(c, !diag_screen_active && !menu_mode && !tuning_lab_active && !reset_confirm_mode);
            rtty_char_ready = false;
            // Only output to Serial if diagnostics are OFF
            if (!shared_serial_diag) printf("%c", c);
        }
        
        if (shared_err_flag) { 
            shared_err_flag = false; 
            if (!shared_serial_diag) printf("[ERR]"); 
        }
        if (shared_figs_flag) { 
            shared_figs_flag = false; 
            if (!shared_serial_diag) printf("[FIGS]"); 
        }
        if (shared_ltrs_flag) { 
            shared_ltrs_flag = false; 
            if (!shared_serial_diag) printf("[LTRS]"); 
        }

        if (shared_diag_ready) {
            shared_diag_ready = false;
            if (shared_serial_diag) {
                // Compact diagnostic stream — one line per update, easy to parse
                float agc_db = 20.0f * log10f(shared_agc_gain + 1e-10f);
                const int bauds_d[] = {45, 50, 75};
                printf("[D] SNR=%.1f SIG=%.1f ERR=%.0f%% SQ=%s AGC=%+.0fdB PH=%.2f FE=%.5f M=%.3f S=%.3f A=%.4f K=%.2f SQT=%.1f F=%.0f B=%d C0=%d%% C1=%d%%\n",
                    (double)shared_snr_db, (double)shared_signal_db,
                    (double)shared_err_rate,
                    shared_squelch_open ? "OPEN" : "SHUT",
                    (double)agc_db, (double)shared_dpll_phase,
                    (double)shared_dpll_ferr,
                    (double)shared_atc_m, (double)shared_atc_s,
                    (double)tuning_dpll_alpha, (double)tuning_lpf_k,
                    (double)tuning_sq_snr, (double)shared_actual_freq,
                    bauds_d[shared_baud_idx],
                    (int)shared_core0_load, (int)shared_core1_load);
            }
        }

        float bin_idx = shared_actual_freq / (SAMPLE_RATE / (float)FFT_SIZE);
        tune_x = (int)((bin_idx - bin_start) / bin_per_pixel);
        tune_x = std::clamp((int)tune_x, 10, 470);

        if (loop_start - last_ui_update > 200000) {
            uint32_t fps = frame_count * 5; frame_count = 0; last_ui_update = loop_start;
            float m_freq = shared_actual_freq - shifts[shared_shift_idx]/2.0f;
            float s_freq = shared_actual_freq + shifts[shared_shift_idx]/2.0f;
            bool is_clipping = shared_adc_clipping; shared_adc_clipping = false; 
            
            if (tuning_lab_active) {
                ui.drawTuningControls(tuning_dpll_alpha, tuning_lpf_k, tuning_sq_snr, shared_err_rate);
            } else if (diag_screen_active) {
                ui.drawDiagScreen(shared_adc_v, shared_font_mode);
            }
            
            ui.updateTopBar(shared_adc_v, fps, shared_signal_db, shared_snr_db, m_freq, s_freq, is_clipping, shared_core0_load, shared_core1_load, shared_squelch_open, shared_agc_gain, shared_agc_enabled, shared_err_rate, shared_rtty_inv);
            // Auto-clear search result after 2 seconds + redraw bottom bar on state change
            static int prev_search_state = 0;
            if (shared_search_state >= 2 && search_result_time > 0 && (loop_start - search_result_time > 2000000)) {
                shared_search_state = 0;
            }
            if (shared_search_state != prev_search_state) {
                prev_search_state = shared_search_state;
                if (!tuning_lab_active && !diag_screen_active)
                    ui.drawBottomBar(shared_baud_idx, shared_shift_idx, stop_bits[shared_stop_idx], shared_afc_on, menu_mode, shared_search_state);
            }
        }
        
        if (new_data_ready) {
            memcpy(local_ts, (void*)shared_fft_ts, sizeof(local_ts));
            memcpy(local_wave, (void*)shared_adc_waveform, sizeof(local_wave));
            memcpy(local_mag_m, (void*)shared_mag_m, sizeof(local_mag_m));
            memcpy(local_mag_s, (void*)shared_mag_s, sizeof(local_mag_s));
            new_data_ready = false;

            float sq=0.0f; for(int i=0; i<FFT_SIZE; i++) sq+=local_ts[i]*local_ts[i];
            shared_signal_db = 10.0f*log10f((sq/FFT_SIZE)+1e-10f) - 20.0f*log10f(shared_agc_gain);

            memcpy(real, local_ts, sizeof(local_ts)); memset(imag, 0, sizeof(imag));
            fft.apply_window(real); fft.compute(real, imag); fft.calc_magnitude(real, imag, mag);

            float pk=-100.0f, sm=0.0f; for(int i=0; i<FFT_SIZE/2; i++) { if(mag[i]>pk) pk=mag[i]; sm+=mag[i]; }
            float avg_noise = sm/(FFT_SIZE/2);
            shared_snr_db = pk - avg_noise;

            float shift = shifts[shared_shift_idx];
            int m_bin = (int)((shared_actual_freq - shift/2.0f) * FFT_SIZE / SAMPLE_RATE);
            int s_bin = (int)((shared_actual_freq + shift/2.0f) * FFT_SIZE / SAMPLE_RATE);
            int search_r = 12;

            float best_m_mag = -100.0f, best_s_mag = -100.0f;
            int best_m_bin = m_bin, best_s_bin = s_bin;

            for(int i = m_bin - search_r; i <= m_bin + search_r; i++) {
                if (i>0 && i<FFT_SIZE/2 && mag[i] > best_m_mag) { best_m_mag = mag[i]; best_m_bin = i; }
            }
            for(int i = s_bin - search_r; i <= s_bin + search_r; i++) {
                if (i>0 && i<FFT_SIZE/2 && mag[i] > best_s_mag) { best_s_mag = mag[i]; best_s_bin = i; }
            }

            // Relaxed squelch with hysteresis for 75 Baud (using dB differences)
            bool sq_strong = ((best_m_mag - avg_noise) > 4.0f || (best_s_mag - avg_noise) > 4.0f) && (shared_snr_db > tuning_sq_snr);
            bool sq_weak = ((best_m_mag - avg_noise) < 1.5f && (best_s_mag - avg_noise) < 1.5f) || (shared_snr_db < tuning_sq_snr - 2.0f);

            if (shared_signal_db < -65.0f) shared_squelch_open = false;
            else if (sq_strong) shared_squelch_open = true;
            else if (sq_weak) shared_squelch_open = false;

            if (shared_squelch_open && shared_afc_on) {
                if ((best_m_mag - best_s_mag) > 2.0f || (best_s_mag - best_m_mag) > 2.0f) {
                    float found_m_f = best_m_bin * SAMPLE_RATE / (float)FFT_SIZE;
                    float found_s_f = best_s_bin * SAMPLE_RATE / (float)FFT_SIZE;
                    float implied_center = (best_m_mag > best_s_mag) ? (found_m_f + shift/2.0f) : (found_s_f - shift/2.0f);
                    implied_center = std::clamp(implied_center, shared_target_freq - 100.0f, shared_target_freq + 100.0f);
                    shared_actual_freq = shared_actual_freq * 0.8f + implied_center * 0.2f;
                }
            } else if (!shared_afc_on && shared_squelch_open) {
                // Keep current frequency if AFC is OFF but squelch is open
            } else {
                shared_actual_freq = shared_actual_freq * 0.98f + shared_target_freq * 0.02f;
            }

            frame_count++;

            for (int i = 0; i < FFT_SIZE / 2; i++) smooth_mag[i] = smooth_mag[i] * 0.7f + mag[i] * 0.3f;

            // --- Signal Search: find two peaks separated by current shift ---
            if (shared_search_request) {
                shared_search_request = false;
                float target_shift = shifts[shared_shift_idx];
                int shift_bins = (int)(target_shift * FFT_SIZE / SAMPLE_RATE);
                int tolerance = 3; // +/- 3 bins tolerance
                float best_score = -200.0f;
                int best_lo_bin = -1, best_hi_bin = -1;
                // Average noise floor
                float noise_sum = 0.0f;
                for (int i = 1; i < FFT_SIZE/2; i++) noise_sum += smooth_mag[i];
                float noise_avg = noise_sum / (FFT_SIZE/2 - 1);
                float min_snr = 4.0f; // minimum dB above noise for each peak
                float max_imbalance = 10.0f; // max dB difference between Mark and Space
                // Scan all possible low-frequency peak positions
                for (int lo = 5; lo < FFT_SIZE/2 - shift_bins - tolerance; lo++) {
                    float lo_mag = smooth_mag[lo];
                    float lo_snr = lo_mag - noise_avg;
                    if (lo_snr < min_snr) continue;
                    // Must be a local maximum (peak, not slope)
                    if (lo > 0 && smooth_mag[lo-1] > lo_mag) continue;
                    if (lo < FFT_SIZE/2-1 && smooth_mag[lo+1] > lo_mag) continue;
                    // Check for matching high peak at lo + shift_bins +/- tolerance
                    for (int d = -tolerance; d <= tolerance; d++) {
                        int hi = lo + shift_bins + d;
                        if (hi < 1 || hi >= FFT_SIZE/2) continue;
                        float hi_mag = smooth_mag[hi];
                        float hi_snr = hi_mag - noise_avg;
                        if (hi_snr < min_snr) continue;
                        // Balance check: both peaks must be similar amplitude
                        float imbalance = fabsf(lo_mag - hi_mag);
                        if (imbalance > max_imbalance) continue;
                        // Score: sum of SNRs, penalize imbalance
                        float score = lo_snr + hi_snr - imbalance * 0.5f;
                        if (score > best_score) {
                            best_score = score;
                            best_lo_bin = lo;
                            best_hi_bin = hi;
                        }
                    }
                }
                if (best_lo_bin >= 0 && best_score > 10.0f) {
                    // Found! Set center frequency
                    float lo_freq = best_lo_bin * SAMPLE_RATE / (float)FFT_SIZE;
                    float hi_freq = best_hi_bin * SAMPLE_RATE / (float)FFT_SIZE;
                    float new_center = (lo_freq + hi_freq) / 2.0f;
                    shared_target_freq = new_center;
                    shared_actual_freq = new_center;
                    shared_search_state = 2; // found
                    flag_settings_change();
                    printf(">> SEARCH: found pair at %.0f/%.0f Hz, center=%.0f\n", lo_freq, hi_freq, new_center);
                } else {
                    shared_search_state = 3; // not found
                    printf(">> SEARCH: no RTTY signal found (best_score=%.1f)\n", best_score);
                }
                search_result_time = time_us_32();
            }

            if (auto_scale) {
                float peak_db = -100.0f;
                for (int x = 0; x < 480; x++) {
                    int b = (int)(bin_start + x * bin_per_pixel);
                    if (b >= 0 && b < FFT_SIZE/2 && smooth_mag[b] > peak_db) peak_db = smooth_mag[b];
                }
                ui_noise_floor = ui_noise_floor * 0.90f + (std::max(peak_db,-40.0f) - 50.0f) * 0.10f;
                ui_gain = 0.0f;
            }
            
            float hz_px = ((bin_end-bin_start)*(SAMPLE_RATE/(float)FFT_SIZE))/480.0f;
            int shift_px = (int)(shifts[shared_shift_idx]/hz_px);
            int half_shift = shift_px / 2;
            int m_x = tune_x - half_shift;
            int s_x = tune_x + half_shift;
            
            marker_spr.fillSprite(PAL_BG);
            if (display_mode != 2) {
                marker_spr.drawFastHLine(0, 13, 480, PAL_GRID); 
                marker_spr.fillTriangle(m_x, 13, m_x - 5, 5, m_x + 5, 5, 0x00FFFFU);
                marker_spr.fillTriangle(s_x, 13, s_x - 5, 5, s_x + 5, 5, 0xFFFF00U);
            }
            ili9488_push_colors(0, UI_Y_MARKER, 480, UI_MARKER_H, (uint16_t*)marker_spr.getBuffer());

            if (tuning_lab_active) {
                // Eye diagram in left half of DSP zone
                spectrum.fillSprite(PAL_BG);
                ui.drawEyeDiagram(spectrum, 240, UI_DSP_ZONE_H);
                ili9488_push_colors(0, UI_Y_DSP, 480, UI_DSP_ZONE_H, (uint16_t*)spectrum.getBuffer());
            } else if (display_mode == 0) {
                spectrum.scroll(0, 1);
                uint16_t* line_ptr = (uint16_t*)spectrum.getBuffer();
                for (int x = 0; x < 480; x++) {
                    float eb = bin_start + x * bin_per_pixel; int b0 = (int)eb; float db = smooth_mag[b0] * (1.0f-(eb-b0)) + smooth_mag[std::min(b0+1,FFT_SIZE/2-1)] * (eb-b0);
                    float norm = (db + ui_gain - ui_noise_floor) / 50.0f;
                    norm = std::clamp(norm, 0.0f, 1.0f); if (shared_exp_scale) norm *= norm;
                    
                    uint8_t r=0, g=0, b=0; // r=visual_B, g=visual_G, b=visual_R
                    if (norm < 0.25f) { r = (uint8_t)(norm * 4.0f * 255.0f); }
                    else if (norm < 0.5f) { r = 255; g = (uint8_t)((norm - 0.25f) * 4.0f * 255.0f); }
                    else if (norm < 0.75f) { g = 255; b = (uint8_t)((norm - 0.5f) * 4.0f * 255.0f); r = 255 - b; }
                    else { b = 255; g = 255 - (uint8_t)((norm - 0.75f) * 4.0f * 255.0f); }
                    
                    uint16_t c = lgfx::color565(r, g, b); // b goes to blue physically, r goes to red physically
                    line_ptr[x] = (c >> 8) | (c << 8); // Swap for SPI DMA
                }
                ili9488_push_waterfall(0, UI_Y_DSP, 480, UI_DSP_ZONE_H, (uint16_t*)spectrum.getBuffer(), tune_x, half_shift);
            } else if (display_mode == 1) { 
                spectrum.fillSprite(PAL_BG);
                for (int x = 0; x < 480; x++) {
                    float eb = bin_start + x * bin_per_pixel; int b0 = (int)eb; float db = smooth_mag[b0] * (1.0f-(eb-b0)) + smooth_mag[std::min(b0+1,FFT_SIZE/2-1)] * (eb-b0);
                    float norm = (db + ui_gain - ui_noise_floor) / 50.0f;
                    norm = std::clamp(norm, 0.0f, 1.0f); if (shared_exp_scale) norm *= norm;
                    int h = (int)(norm * UI_DSP_ZONE_H); if (h > 0) spectrum.drawFastVLine(x, UI_DSP_ZONE_H - h, h, PAL_PEAK);
                }
                spectrum.drawFastVLine(m_x, 0, UI_DSP_ZONE_H, 0x00FFFFU);
                spectrum.drawFastVLine(s_x, 0, UI_DSP_ZONE_H, 0xFFFF00U);
                ili9488_push_colors(0, UI_Y_DSP, 480, UI_DSP_ZONE_H, (uint16_t*)spectrum.getBuffer());
            } else {
                // Lissajous SCOPE with Phosphor Decay effect
                // Bitmask fade: clear LSBs of each channel, shift right, ~0.84x per frame
                // RGB565: RRRRR GGGGGG BBBBB — mask preserves top bits after shift
                uint32_t* buf32 = (uint32_t*)spectrum.getBuffer();
                const int total_words = (480 * 64) / 2; // 2 pixels per uint32
                const uint32_t mask = 0xE7BCE7BCU; // keeps top 3R, top 5G, top 3B per pixel
                for (int i = 0; i < total_words; i++) {
                    uint32_t v = buf32[i];
                    if (v != 0) buf32[i] = (v >> 1) & mask;
                }

                int cx = 240, cy = 32;
                spectrum.drawFastHLine(0, cy, 480, PAL_GRID);
                spectrum.drawFastVLine(cx, 0, 64, PAL_GRID);

                // Lissajous drawing with table lookup instead of sinf/cosf
                for (int x = 0; x < 480; x++) {
                    float m = sqrtf(local_mag_m[x]) * 60.0f;
                    float s = sqrtf(local_mag_s[x]) * 60.0f;
                    float phase = x * 0.4f;
                    int tidx = (int)(phase * (1024.0f / (2.0f * (float)M_PI))) & 1023;
                    float sin_p = sin_table[tidx];
                    float cos_p = cos_table[tidx];
                    int px = cx + (int)(m * sin_p + s * 0.05f * cos_p);
                    int py = cy + (int)(s * cos_p + m * 0.05f * sin_p);
                    spectrum.drawPixel(std::clamp(px, 0, 479), std::clamp(py, 0, 63), PAL_WAVE);
                }
                ili9488_push_colors(0, UI_Y_DSP, 480, UI_DSP_ZONE_H, (uint16_t*)spectrum.getBuffer());
            }
        }
        
        static uint32_t touch_ignore_until = 0;
        if (loop_start - last_touch > 50000) {
            uint16_t tx, ty; static bool was_touched = false;
            bool is_touched = tft.getTouch(&tx, &ty);
            if (is_touched && time_us_32() > touch_ignore_until) {
                if (ty >= UI_Y_MARKER && ty <= (UI_Y_DSP + UI_DSP_ZONE_H)) {
                    flag_settings_change(); shared_target_freq = (bin_start + (tx / 480.0f) * (bin_end - bin_start)) * (SAMPLE_RATE / (float)FFT_SIZE);
                    shared_actual_freq = shared_target_freq; // SNAP INSTANTLY
                }
                else if (ty > UI_Y_BOTTOM && !was_touched) {
                    int btn_idx = tx / 68; // 480 / 7 approx 68
                    if (btn_idx == 0) { flag_settings_change(); shared_baud_idx = (shared_baud_idx + 1) % 3; }
                    else if (btn_idx == 1) { flag_settings_change(); shared_shift_idx = (shared_shift_idx + 1) % 5; }
                    else if (btn_idx == 2) { shared_search_request = true; shared_search_state = 1; }
                    else if (btn_idx == 3) { flag_settings_change(); shared_afc_on = !shared_afc_on; }
                    else if (btn_idx == 4) { flag_settings_change(); shared_stop_idx = (shared_stop_idx + 1) % 3; }
                    else if (btn_idx == 5) { ui.clearRTTY(); shared_clear_dsp = true; }
                    else if (btn_idx >= 6) {
                        if (tuning_lab_active) {
                            // Tuning Lab -> back to main menu
                            tuning_lab_active = false;
                            menu_mode = true;
                        } else if (diag_screen_active && !menu_mode) {
                            diag_screen_active = false;
                            ui.drawRTTY();
                        } else {
                            menu_mode = !menu_mode;
                            if(menu_mode) diag_screen_active = false; else ui.drawRTTY();
                        }
                    }
                    
                    if (!menu_mode) reset_confirm_mode = false;
                    ui.drawBottomBar(shared_baud_idx, shared_shift_idx, stop_bits[shared_stop_idx], shared_afc_on, menu_mode, shared_search_state);
                    if (menu_mode) ui.drawMenu(auto_scale, display_mode, "DIAG");
                    touch_ignore_until = time_us_32() + 300000;
                }
                else if (ty >= UI_Y_TEXT && ty < UI_Y_BOTTOM) {
                    if (reset_confirm_mode && !was_touched) {
                        int local_y = ty - UI_Y_TEXT;
                        if (local_y >= 100 && local_y <= 140) {
                            if (tx >= 280 && tx <= 400) { // YES
                                multicore_lockout_start_blocking();
                                uint32_t ints = save_and_disable_interrupts();
                                flash_range_erase(CAL_FLASH_OFFSET, FLASH_SECTOR_SIZE);
                                flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
                                restore_interrupts(ints);
                                multicore_lockout_end_blocking();
                                watchdog_enable(1, 1);
                                while(1);
                            } else if (tx >= 80 && tx <= 200) { // NO
                                reset_confirm_mode = false;
                                diag_screen_active = true;
                                ui.drawDiagScreen(shared_adc_v, shared_font_mode);
                            }
                        }
                        // Ignore taps outside YES/NO buttons — keep confirm dialog open
                        touch_ignore_until = time_us_32() + 300000;
                    } else if (menu_mode && !was_touched) {
                        int col = tx / (480/4);
                        int row = (ty - UI_Y_TEXT) / (160/3);
                        int btn = row * 4 + col;
                        if (btn == 0) { flag_settings_change(); display_mode = (display_mode + 1) % 3; spectrum.fillSprite(PAL_BG); }
                        else if (btn == 1) { flag_settings_change(); auto_scale = !auto_scale; }
                        else if (btn == 2) { diag_screen_active = true; menu_mode = false; }
                        else if (btn == 3) { tuning_lab_active = true; menu_mode = false; }

                        if (menu_mode && !reset_confirm_mode) ui.drawMenu(auto_scale, display_mode, "DIAG");
                        touch_ignore_until = time_us_32() + 300000;
                        } else if (diag_screen_active && !was_touched) {
                            // Diagnostics screen touch handling (6 buttons, 80px each)
                            int local_y = ty - UI_Y_TEXT;
                            if (local_y > 111) {
                                if (tx < 240) { // FONT toggle: BIG→MED→SMALL→TINY
                                    shared_font_mode = (shared_font_mode + 1) % 4;
                                    const int widths[] = {55, 62, 73, 90}; // BIG:8px, MED:7px, SMALL:6px, TINY:5px
                                    shared_line_width = widths[shared_font_mode];
                                    flag_settings_change();
                                } else { // RST
                                    reset_confirm_mode = true;
                                    diag_screen_active = false;
                                    ui.drawResetConfirm();
                                }
                                if (!reset_confirm_mode) ui.drawDiagScreen(shared_adc_v, shared_font_mode);
                                touch_ignore_until = time_us_32() + 300000;
                            }
                        } else if (tuning_lab_active && !was_touched) {
                            // Tuning Lab touch: 6 cols x 2 rows, 80px each
                            // Row 0 (y=42..83): A- A_val A+ K- K_val K+
                            // Row 1 (y=86..127): SQ- SQ_val SQ+ DUMP --- SAVE
                            int local_y = ty - UI_Y_TEXT;
                            int col = tx / 80;
                            int row = -1;
                            if (local_y >= 42 && local_y < 84) row = 0;
                            else if (local_y >= 86 && local_y < 128) row = 1;

                            if (row == 0) {
                                if (col == 0) { flag_settings_change(); tuning_dpll_alpha -= 0.005f; if (tuning_dpll_alpha < 0.005f) tuning_dpll_alpha = 0.005f; }
                                else if (col == 2) { flag_settings_change(); tuning_dpll_alpha += 0.005f; if (tuning_dpll_alpha > 0.200f) tuning_dpll_alpha = 0.200f; }
                                else if (col == 3) { flag_settings_change(); tuning_lpf_k -= 0.05f; if (tuning_lpf_k < 0.3f) tuning_lpf_k = 0.3f; }
                                else if (col == 5) { flag_settings_change(); tuning_lpf_k += 0.05f; if (tuning_lpf_k > 2.0f) tuning_lpf_k = 2.0f; }
                            } else if (row == 1) {
                                if (col == 0) { flag_settings_change(); tuning_sq_snr -= 1.0f; }
                                else if (col == 2) { flag_settings_change(); tuning_sq_snr += 1.0f; }
                                else if (col == 3) {
                                    // DUMP toggle: enable/disable diagnostic stream
                                    shared_serial_diag = !shared_serial_diag;
                                }
                                else if (col == 5) {
                                    // SAVE settings to flash
                                    AppSettings s;
                                    s.magic = 0xDEADBEEF;
                                    s.baud_idx = shared_baud_idx;
                                    s.shift_idx = shared_shift_idx;
                                    s.stop_idx = shared_stop_idx;
                                    s.rtty_inv = shared_rtty_inv;
                                    s.display_mode = display_mode;
                                    s.exp_scale = shared_exp_scale;
                                    s.auto_scale = auto_scale;
                                    s.filter_k = tuning_lpf_k;
                                    s.sq_snr = tuning_sq_snr;
                                    s.target_freq = shared_target_freq;
                                    s.serial_diag = shared_serial_diag;
                                    s.line_width = shared_line_width;
                                    s.afc_on = shared_afc_on;
                                    s.font_mode = shared_font_mode;
                                    s.dpll_alpha = tuning_dpll_alpha;
                                    
                                    uint8_t page_buf[FLASH_PAGE_SIZE] = {0};
                                    memcpy(page_buf, &s, sizeof(AppSettings));
                                    multicore_lockout_start_blocking();
                                    uint32_t ints = save_and_disable_interrupts();
                                    flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
                                    flash_range_program(SETTINGS_FLASH_OFFSET, page_buf, FLASH_PAGE_SIZE);
                                    restore_interrupts(ints);
                                    multicore_lockout_end_blocking();
                                    settings_need_save = false;
                                    // Show SAVED! feedback
                                    ui.drawTuningControls(tuning_dpll_alpha, tuning_lpf_k, tuning_sq_snr, shared_err_rate, true);
                                    touch_ignore_until = time_us_32() + 500000;
                                    continue; // skip normal redraw below
                                }
                            }
                            ui.drawTuningControls(tuning_dpll_alpha, tuning_lpf_k, tuning_sq_snr, shared_err_rate);
                            touch_ignore_until = time_us_32() + 200000;

 } else if (!menu_mode && !diag_screen_active && !tuning_lab_active) {

                        if (tx > 440) {
                            int local_y = ty - UI_Y_TEXT;
                            if (local_y < 30) ui.scrollRTTY(1);
                            else if (local_y > 130) ui.scrollRTTY(-1);
                            else ui.scrollToY(local_y - 30, 100);
                        } else if (!was_touched && diag_screen_active) {
                            diag_screen_active = false;
                            ui.drawRTTY();
                            touch_ignore_until = time_us_32() + 300000;
                        }
                    }
                }
            }
            was_touched = is_touched; last_touch = loop_start;
        }
        
        uint32_t loop_end = time_us_32();
        c1_total_work += (loop_end - loop_start);
        if (loop_start - c1_last_measure >= 500000) {
            shared_core1_load = (c1_total_work * 100.0f) / (float)(loop_start - c1_last_measure);
            c1_total_work = 0; c1_last_measure = loop_start;
        }

        // Short yield when idle — reduces AHB bus pressure from Core 1
        if (!new_data_ready) sleep_us(20);

        if (settings_need_save && (loop_start - settings_last_change > 15000000)) {
            AppSettings s;
            s.magic = 0xDEADBEEF;
            s.baud_idx = shared_baud_idx;
            s.shift_idx = shared_shift_idx;
            s.stop_idx = shared_stop_idx;
            s.rtty_inv = shared_rtty_inv;
            s.display_mode = display_mode;
            s.exp_scale = shared_exp_scale;
            s.auto_scale = auto_scale;
            s.filter_k = tuning_lpf_k;
            s.sq_snr = tuning_sq_snr;
            s.target_freq = shared_target_freq;
            s.serial_diag = shared_serial_diag;
            s.line_width = shared_line_width;
            s.afc_on = shared_afc_on;
            s.font_mode = shared_font_mode;
            s.dpll_alpha = tuning_dpll_alpha;
            
            // Skip write if flash already has identical data
            if (memcmp(flash_settings_contents, &s, sizeof(AppSettings)) != 0) {
                uint8_t page_buf[FLASH_PAGE_SIZE] = {0};
                memcpy(page_buf, &s, sizeof(AppSettings));

                multicore_lockout_start_blocking();
                uint32_t ints = save_and_disable_interrupts();
                flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
                flash_range_program(SETTINGS_FLASH_OFFSET, page_buf, FLASH_PAGE_SIZE);
                restore_interrupts(ints);
                multicore_lockout_end_blocking();
            }
            settings_need_save = false;
        }
        tight_loop_contents();
    }
}

void core0_dsp_loop() {
    multicore_lockout_victim_init();
    for(int i=0; i<1024; i++) {
        sin_table[i] = sinf(2.0f * (float)M_PI * (float)i / 1024.0f);
        cos_table[i] = cosf(2.0f * (float)M_PI * (float)i / 1024.0f);
    }
    
    static float ts[FFT_SIZE], tw[480], tw_m[480], tw_s[480], fb[63]={0.0f};
    int sc=0, wi=0, fi=0;

    // Hardware-paced ADC via FIFO (replaces blocking adc_read)
    // ADC runs free at exact 10kHz from hardware timer — zero jitter
    adc_init(); adc_gpio_init(ADC_PIN); adc_select_input(0);
    adc_fifo_setup(
        true,   // Write to FIFO
        false,  // Don't use DMA DREQ (yet — enable for Phase 8 DRM dual-channel)
        1,      // DREQ threshold (IRQ when 1+ samples ready)
        false,  // No ERR bit in FIFO
        false   // Don't shift to 8-bit
    );
    // Clock divider: 48MHz / (96 + 4704) = 10,000 Hz exactly
    adc_set_clkdiv(4704.0f);
    adc_run(true);  // Start free-running mode

    float dc=0.0f;
    
    float phase_m = 0.0f, phase_s = 0.0f;
    Biquad lp_mi, lp_mq, lp_si, lp_sq;
    float current_baud = -1.0f;
    float atc_mark_env = 0.01f, atc_space_env = 0.01f;
    float cached_atc_fast = 0.0f, cached_atc_slow = 0.0f; // Cached expf() results
    
    agc_t agc;
    agc_init(&agc, (float)SAMPLE_RATE);
    
    // Diagnostics
    float diag_adc_min = 4096.0f, diag_adc_max = 0.0f;
    float diag_m_pow = 0.0f, diag_s_pow = 0.0f;
    int diag_samples = 0;
    
    int baudot_state = 0;
    float symbol_phase = 0.0f;
    float integrate_acc = 0.0f;
    uint8_t current_char = 0;
    bool is_figs = false;
    bool last_d_sign = true;
    uint8_t err_hist[100] = {0};
    int err_idx = 0, err_sum = 0;
    float freq_error = 0.0f;

    // Auto-inversion detection
    uint32_t auto_inv_check_time = 0;  // when to check next
    uint32_t auto_inv_high_err_since = 0; // when high ERR started
    bool auto_inv_trying = false;      // currently testing inverted
    bool auto_inv_prev_inv = false;    // previous inversion state

    const float shifts_hz[] = {170.0f, 200.0f, 425.0f, 450.0f, 850.0f};
    const float bauds[] = {45.45f, 50.0f, 75.0f};

    while(true) {
        // Wait for hardware-timed ADC sample (FIFO provides exact 10kHz timing)
        while (adc_fifo_is_empty()) {
            tight_loop_contents();
        }

        if (shared_clear_dsp) {
            shared_clear_dsp = false;
            shared_actual_freq = shared_target_freq;
            baudot_state = 0;
            symbol_phase = 0.0f;
            integrate_acc = 0.0f;
            freq_error = 0.0f;
            atc_mark_env = 0.01f;
            atc_space_env = 0.01f;
            shared_squelch_open = false;
            last_d_sign = true;
            // Reset error counter
            memset(err_hist, 0, sizeof(err_hist));
            err_idx = 0; err_sum = 0;
            shared_err_rate = 0.0f;
        }

        uint32_t st = time_us_32();
        uint16_t rv = adc_fifo_get_blocking(); // Hardware-timed, no software jitter
        
        if ((float)rv < diag_adc_min) diag_adc_min = (float)rv;
        if ((float)rv > diag_adc_max) diag_adc_max = (float)rv;
        
        if(rv<50 || rv>4045) shared_adc_clipping=true;
        float v = ((float)rv / 4095.0f) * 3.3f; shared_adc_v=v; 
        float s = ((float)rv - 2048.0f) / 2048.0f;
        dc = dc * 0.99f + s * 0.01f; s -= dc; fb[fi]=s; float f_out=0.0f; int bi=fi;
        for(int i=0; i<63; i++) { f_out += fir_coeffs[i] * fb[bi]; bi--; if(bi<0) bi=62; }
        fi = (fi + 1) % 63; 
        
        float agc_out = agc_process(&agc, f_out);
        shared_agc_gain = agc.gain;
        
        if(wi<480) { tw[wi] = agc_out * 1.65f + 1.65f; } 
        ts[sc++] = agc_out * 2.0f;
        
        f_out = agc_out;
        
        float baud = bauds[shared_baud_idx];
        static float current_k = -1.0f;
        float stop_bits_expected = (shared_stop_idx == 0) ? 1.0f : ((shared_stop_idx == 1) ? 1.5f : 2.0f);
        
        if (baud != current_baud || tuning_lpf_k != current_k) {
            current_baud = baud;
            current_k = tuning_lpf_k;
            float fc = baud * tuning_lpf_k;
            design_lpf(&lp_mi, fc, (float)SAMPLE_RATE); design_lpf(&lp_mq, fc, (float)SAMPLE_RATE);
            design_lpf(&lp_si, fc, (float)SAMPLE_RATE); design_lpf(&lp_sq, fc, (float)SAMPLE_RATE);
            // Cache expf() — was computed 10,000x/sec, now only on baud change
            cached_atc_fast = expf(-1.0f / (2.0f * ((float)SAMPLE_RATE / baud)));
            cached_atc_slow = expf(-1.0f / (16.0f * ((float)SAMPLE_RATE / baud)));
        }
        
        float shift = shifts_hz[shared_shift_idx];
        float fm = shared_actual_freq - shift / 2.0f;
        float fs = shared_actual_freq + shift / 2.0f;
        
        phase_m += fm * 0.0001f; if(phase_m >= 1.0f) phase_m -= 1.0f;
        phase_s += fs * 0.0001f; if(phase_s >= 1.0f) phase_s -= 1.0f;
        int idx_m = (int)(phase_m * 1024.0f) & 1023;
        int idx_s = (int)(phase_s * 1024.0f) & 1023;
        
        float mi = process_biquad(&lp_mi, f_out * cos_table[idx_m]);
        float mq = process_biquad(&lp_mq, f_out * sin_table[idx_m]);
        float si = process_biquad(&lp_si, f_out * cos_table[idx_s]);
        float sq = process_biquad(&lp_sq, f_out * sin_table[idx_s]);
        
        float mark_power = mi*mi + mq*mq;
        float space_power = si*si + sq*sq;
        
        if (wi < 480) { tw_m[wi] = mark_power; tw_s[wi] = space_power; wi++; }
        
        diag_m_pow += mark_power;
        diag_s_pow += space_power;
        diag_samples++;
        
        float new_m = sqrtf(mark_power + 1e-10f);
        float new_s = sqrtf(space_power + 1e-10f);
        // Use cached expf() values (computed only on baud change)
        float tc_m = (new_m > atc_mark_env) ? cached_atc_fast : cached_atc_slow;
        float tc_s = (new_s > atc_space_env) ? cached_atc_fast : cached_atc_slow;
        atc_mark_env  = atc_mark_env  * tc_m + new_m * (1.0f - tc_m);
        atc_space_env = atc_space_env * tc_s + new_s * (1.0f - tc_s);
        
        float m_norm = new_m / (atc_mark_env + 1e-6f);
        float s_norm = new_s / (atc_space_env + 1e-6f);
        
        float D = m_norm - s_norm;
        D = fmaxf(-1.5f, fminf(1.5f, D));
        if (shared_rtty_inv) D = -D;
        bool d_sign = (D > 0.0f);
        
        float phase_inc = baud / (float)SAMPLE_RATE;
        // freq_error declared above, before main loop
        float dpll_beta = tuning_dpll_alpha * tuning_dpll_alpha / 2.0f;
        
        if (!shared_squelch_open) {
            baudot_state = 0;
            last_d_sign = true; 
            freq_error = 0.0f;
        } else {
            // Full PI loop DPLL
            if (d_sign != last_d_sign) {
                if (baudot_state > 0) {
                    float phase_error;
                    if (symbol_phase < 0.5f) phase_error = symbol_phase;
                    else phase_error = symbol_phase - 1.0f;
                    
                    phase_error = fmaxf(-0.1f, fminf(0.1f, phase_error));
                    symbol_phase -= tuning_dpll_alpha * phase_error; 
                    freq_error -= dpll_beta * phase_error;
                    float max_fe = 0.05f * phase_inc;
                    freq_error = fmaxf(-max_fe, fminf(max_fe, freq_error));
                } else if (!d_sign) { 
                    // Transition to space = start bit
                    baudot_state = 1;
                    symbol_phase = 0.0f; 
                    integrate_acc = 0.0f;
                    current_char = 0;
                }
            }
            last_d_sign = d_sign;
            
            if (baudot_state > 0) {
                symbol_phase += phase_inc + freq_error;
                integrate_acc += D;

                // Eye diagram: DPLL-locked accumulation (zero jitter)
                // symbol_phase 0..1 maps to x position within one bit period
                {
                    int spb = (int)((float)SAMPLE_RATE / baud);
                    if (spb > EYE_MAX_SPB) spb = EYE_MAX_SPB;
                    shared_eye_spb = spb;
                    int ex = (int)(symbol_phase * (float)(spb - 1));
                    if (ex >= 0 && ex < spb) {
                        int8_t dv = (int8_t)fmaxf(-127.0f, fminf(127.0f, D * 85.0f));
                        shared_eye_buf[shared_eye_idx][ex] = dv;
                    }
                }

                if (symbol_phase >= 1.0f) {
                    symbol_phase -= 1.0f;
                    // Advance eye trace on bit boundary
                    shared_eye_idx = (shared_eye_idx + 1) % EYE_TRACES;
                    shared_eye_ready = true;
                    bool bit = (integrate_acc > 0.0f);
                    integrate_acc = 0.0f;
                    
                    if (baudot_state == 1) { 
                        if (bit) baudot_state = 0; // False start
                        else baudot_state = 2;
                    } else if (baudot_state >= 2 && baudot_state <= 6) { 
                        if (bit) current_char |= (1 << (baudot_state - 2));
                        baudot_state++;
                    } else if (baudot_state == 7) {
                        // Error rate tracking (100-char sliding window)

                        if (bit) { // Valid stop
                            char decoded = '\0';
                            if (current_char == 27) { is_figs = true; shared_figs_flag = true; }
                            else if (current_char == 31) { is_figs = false; shared_ltrs_flag = true; }
                            else {
                                decoded = is_figs ? ita2_figs[current_char] : ita2_ltrs[current_char];
                                if (decoded == ' ') is_figs = false;
                                if (decoded != '\0') {
                                    rtty_new_char = decoded;
                                    rtty_char_ready = true;
                                }
                            }
                            err_sum -= err_hist[err_idx]; err_hist[err_idx] = 0;
                            err_idx = (err_idx + 1) % 100;
                        } else {
                            shared_err_flag = true;
                            err_sum -= err_hist[err_idx]; err_hist[err_idx] = 1; err_sum++;
                            err_idx = (err_idx + 1) % 100;
                        }
                        shared_err_rate = (float)err_sum;

                        // Auto-inversion: if ERR > 40% for 3 sec with open squelch, try flipping
                        {
                            uint32_t now = time_us_32();
                            if (shared_squelch_open && shared_err_rate > 40.0f) {
                                if (auto_inv_high_err_since == 0) auto_inv_high_err_since = now;
                                else if (!auto_inv_trying && (now - auto_inv_high_err_since > 3000000)) {
                                    // High ERR for 3 sec — try inversion
                                    auto_inv_prev_inv = shared_rtty_inv;
                                    shared_rtty_inv = !shared_rtty_inv;
                                    auto_inv_trying = true;
                                    auto_inv_check_time = now + 3000000; // measure for 3 sec
                                    memset(err_hist, 0, sizeof(err_hist));
                                    err_idx = 0; err_sum = 0;
                                    shared_err_rate = 0.0f;
                                }
                            } else if (!auto_inv_trying) {
                                auto_inv_high_err_since = 0; // reset timer
                            }

                            if (auto_inv_trying && now > auto_inv_check_time) {
                                if (shared_err_rate > 35.0f) {
                                    // Still bad — revert
                                    shared_rtty_inv = auto_inv_prev_inv;
                                }
                                // Either helped or reverted — done
                                auto_inv_trying = false;
                                auto_inv_high_err_since = 0;
                                memset(err_hist, 0, sizeof(err_hist));
                                err_idx = 0; err_sum = 0;
                                shared_err_rate = 0.0f;
                            }
                        }

                        if (stop_bits_expected <= 1.0f && !d_sign) {
                            baudot_state = 1;
                            symbol_phase = 0.0f; 
                            integrate_acc = D;
                            current_char = 0;
                        } else {
                            baudot_state = 0; 
                        }
                    }
                }
            }
        }
        
        if(sc==FFT_SIZE) {
            memcpy((void*)shared_fft_ts, ts, sizeof(ts));
            memcpy((void*)shared_adc_waveform, tw, sizeof(tw));
            memcpy((void*)shared_mag_m, tw_m, sizeof(tw_m));
            memcpy((void*)shared_mag_s, tw_s, sizeof(tw_s));
            new_data_ready=true;

            wi=0; memmove(ts, &ts[480], (FFT_SIZE-480)*sizeof(float)); sc=FFT_SIZE-480;
            
            static int diag_timer = 0;
            if (++diag_timer >= 10) { 
                diag_timer = 0;
                shared_diag_adc_min = diag_adc_min;
                shared_diag_adc_max = diag_adc_max;
                shared_atc_m = atc_mark_env;
                shared_atc_s = atc_space_env;
                shared_dpll_phase = symbol_phase;
                shared_dpll_ferr = freq_error;
                shared_diag_ready = true;
                
                diag_adc_min = 4096.0f; diag_adc_max = 0.0f;
                diag_m_pow = 0.0f; diag_s_pow = 0.0f;
                diag_samples = 0;
            }
        }
        
        static uint32_t total_work = 0, total_time = 0;
        uint32_t work_end = time_us_32();
        total_work += (work_end - st);
        total_time += 100; // 100μs per sample at 10kHz
        if (total_time >= 500000) {
            shared_core0_load = (total_work * 100.0f) / (float)total_time;
            total_work = 0; total_time = 0;
        }
        // Hardware FIFO provides exact timing — no drain needed
        // (tight_loop_contents above ensures no overflow)
    }
}

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_30); sleep_ms(10); set_sys_clock_khz(300000, true);
    
    gpio_init(ENC_SW);
    gpio_set_dir(ENC_SW, GPIO_IN);
    gpio_pull_up(ENC_SW);
    sleep_ms(10);
    
    // 100ms Debounce for calibration check
    bool pressed = true;
    for(int i=0; i<10; i++) {
        if (gpio_get(ENC_SW) != 0) { pressed = false; break; }
        sleep_ms(10);
    }
    shared_force_cal = pressed; // Any press = recalibrate touch

    // Hardware Hard Reset (Hold for 3 seconds) — wipes all settings too
    if (pressed) {
        bool held_long = true;
        gpio_init(PICO_DEFAULT_LED_PIN);
        gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
        for(int i=0; i<15; i++) {
            gpio_put(PICO_DEFAULT_LED_PIN, i%2);
            sleep_ms(200);
            if (gpio_get(ENC_SW) != 0) { held_long = false; break; }
        }
        if (held_long) {
            // WIPE IT ALL (cal + settings)
            multicore_lockout_start_blocking();
            uint32_t ints = save_and_disable_interrupts();
            flash_range_erase(CAL_FLASH_OFFSET, FLASH_SECTOR_SIZE);
            flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
            restore_interrupts(ints);
            multicore_lockout_end_blocking();

            // Flash LED rapidly to confirm
            for(int i=0; i<20; i++) { gpio_put(PICO_DEFAULT_LED_PIN, i%2); sleep_ms(50); }
        }
        // shared_force_cal stays true — Core 1 will run touch calibration
    }
    
    stdio_init_all(); sleep_ms(2000);
    multicore_launch_core1(core1_main); core0_dsp_loop();
    return 0;
}