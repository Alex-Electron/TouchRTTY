#include "app_state.hpp"
#include "hardware/flash.h"
#include "pico/stdlib.h"

// ---- Constants ----
const float g_shifts[NUM_SHIFTS]     = {85.0f, 170.0f, 200.0f, 340.0f, 425.0f, 450.0f, 500.0f, 850.0f};
const int   g_shifts_int[NUM_SHIFTS] = {85,    170,    200,    340,    425,    450,    500,    850};

const uint8_t* flash_target_contents   = (const uint8_t*)(XIP_BASE + CAL_FLASH_OFFSET);
const uint8_t* flash_settings_contents = (const uint8_t*)(XIP_BASE + SETTINGS_FLASH_OFFSET);

// ---- Shared state definitions ----
volatile bool     settings_need_save   = false;
volatile uint32_t settings_last_change = 0;

volatile int  shared_line_width = 60;
volatile bool shared_afc_on     = true;
volatile bool shared_exp_scale  = true;
volatile int  shared_font_mode  = 0;
volatile bool shared_force_cal  = false;

volatile float shared_fft_mag[FFT_SIZE / 2];
volatile float shared_adc_waveform[480];
volatile float shared_fft_ts[FFT_SIZE];
volatile bool  new_data_ready = false;
volatile uint32_t shared_dsp_seq = 0;

volatile float shared_adc_v        = 0.0f;
volatile float shared_signal_db    = -80.0f;
volatile bool  shared_adc_clipping = false;
volatile float shared_snr_db       = 0.0f;
volatile float shared_err_rate     = 0.0f;
volatile float shared_core0_load   = 0.0f;
volatile float shared_core1_load   = 0.0f;

volatile float shared_mag_m[480];
volatile float shared_mag_s[480];

volatile char rtty_new_char     = 0;
volatile bool rtty_char_ready   = false;
volatile bool shared_err_flag   = false;
volatile bool shared_figs_flag  = false;
volatile bool shared_ltrs_flag  = false;

volatile float shared_diag_adc_min = 4096.0f;
volatile float shared_diag_adc_max = 0.0f;
volatile float shared_atc_m        = 0.0f;
volatile float shared_atc_s        = 0.0f;
volatile float shared_dpll_phase   = 0.0f;
volatile float shared_dpll_ferr    = 0.0f;
volatile bool  shared_diag_ready   = false;

volatile float shared_target_freq = 1535.0f;
volatile float shared_actual_freq = 1535.0f;
volatile int   shared_baud_idx    = 0;
volatile int   shared_shift_idx   = 0;
volatile int   shared_stop_idx    = 1;
volatile bool  shared_rtty_inv    = false;
volatile bool  shared_squelch_open = false;
volatile bool  shared_clear_dsp    = false;

volatile int8_t shared_eye_buf[EYE_TRACES][EYE_MAX_SPB];
volatile int    shared_eye_spb   = 220;
volatile int    shared_eye_idx   = 0;
volatile bool   shared_eye_ready = false;

volatile float tuning_dpll_alpha = 0.035f;
volatile float tuning_lpf_k      = 0.75f;
volatile float tuning_sq_snr     = 6.0f;
volatile float shared_agc_gain   = 1.0f;
volatile bool  shared_agc_enabled = true;
volatile bool  shared_serial_diag = false;
volatile bool  shared_save_request = false;

volatile bool  shared_search_request    = false;
volatile int   shared_search_state      = 0;
volatile float shared_active_shift      = 170.0f;
volatile bool  shared_inv_uncertain     = false;
volatile bool  shared_inv_auto          = true;
volatile bool  shared_stop_auto         = false;
volatile float shared_active_stop       = 1.5f;
volatile bool  shared_stop_detect_req   = false;
volatile int   shared_stop_detect_state = 0;
volatile float shared_stop_gap_last     = 0.0f;
volatile int   shared_stop_gap_hist[3]  = {0, 0, 0};

volatile bool  shared_baud_auto         = false;
volatile bool  shared_baud_detect_req   = false;
volatile int   shared_baud_detect_state = 0;
volatile float shared_active_baud       = 45.45f;
volatile bool  shared_chain_stop_after_baud = false;

void flag_settings_change() {
    settings_need_save = true;
    settings_last_change = time_us_32();
}
