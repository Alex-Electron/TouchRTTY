#pragma once
#include <stdint.h>
#include "dsp/fft.hpp"

// Hardware/sample configuration
#define ADC_PIN 26
#define SAMPLE_RATE 10000
#define ENC_SW 4

// Flash offsets (last 2MB of 4MB flash)
#define CAL_FLASH_OFFSET (1024 * 1024 * 2)
#define SETTINGS_FLASH_OFFSET (1024 * 1024 * 2 + FLASH_SECTOR_SIZE)

// Shift tables (0..7 = fixed, 8 = AUTO)
#define NUM_SHIFTS 8
extern const float g_shifts[NUM_SHIFTS];
extern const int   g_shifts_int[NUM_SHIFTS];

// Eye diagram (DPLL-synchronized)
#define EYE_TRACES 16
#define EYE_MAX_SPB 256

// Persistent settings saved to flash
struct AppSettings {
    uint32_t magic;
    int   baud_idx;
    int   shift_idx;
    int   stop_idx;
    bool  rtty_inv;
    int   display_mode;
    bool  exp_scale;
    bool  auto_scale;
    float filter_k;
    float sq_snr;
    float target_freq;
    bool  serial_diag;
    int   line_width;
    bool  afc_on;
    int   font_mode;
    float dpll_alpha;
    bool  inv_auto;
    bool  stop_auto;
};

// Flash region pointers (populated at startup)
extern const uint8_t* flash_target_contents;
extern const uint8_t* flash_settings_contents;

// ---- Shared state (Core 0 <-> Core 1) ----
// Settings change tracking
extern volatile bool     settings_need_save;
extern volatile uint32_t settings_last_change;

// UI / display
extern volatile int  shared_line_width;
extern volatile bool shared_afc_on;
extern volatile bool shared_exp_scale;
extern volatile int  shared_font_mode;
extern volatile bool shared_force_cal;

// FFT / waveform buffers
extern volatile float shared_fft_mag[FFT_SIZE / 2];
extern volatile float shared_adc_waveform[480];
extern volatile float shared_fft_ts[FFT_SIZE];
extern volatile bool  new_data_ready;
// Seqlock for torn-read protection: Core 0 increments around write (odd=writing, even=stable).
// Core 1 captures seq before and after read; matching even value = consistent snapshot.
extern volatile uint32_t shared_dsp_seq;

// Signal metrics
extern volatile float shared_adc_v;
extern volatile float shared_signal_db;
extern volatile bool  shared_adc_clipping;
extern volatile float shared_snr_db;
extern volatile float shared_err_rate;
extern volatile float shared_core0_load;
extern volatile float shared_core1_load;

// Per-bin Mark/Space powers for marker display
extern volatile float shared_mag_m[480];
extern volatile float shared_mag_s[480];

// Decoded character transport
extern volatile char rtty_new_char;
extern volatile bool rtty_char_ready;
extern volatile bool shared_err_flag;
extern volatile bool shared_figs_flag;
extern volatile bool shared_ltrs_flag;

// Diagnostic values
extern volatile float shared_diag_adc_min;
extern volatile float shared_diag_adc_max;
extern volatile float shared_atc_m;
extern volatile float shared_atc_s;
extern volatile float shared_dpll_phase;
extern volatile float shared_dpll_ferr;
extern volatile bool  shared_diag_ready;

// Tuning / frequency / protocol
extern volatile float shared_target_freq;
extern volatile float shared_actual_freq;
extern volatile int   shared_baud_idx;      // 0=45, 1=50, 2=75, 3=100, 4=AUTO
extern volatile int   shared_shift_idx;     // 0..7=fixed, 8=AUTO
extern volatile int   shared_stop_idx;      // 0=1.0, 1=1.5, 2=2.0, 3=AUTO
extern volatile bool  shared_rtty_inv;
extern volatile bool  shared_squelch_open;
extern volatile bool  shared_clear_dsp;

// Eye diagram state
extern volatile int8_t shared_eye_buf[EYE_TRACES][EYE_MAX_SPB];
extern volatile int    shared_eye_spb;
extern volatile int    shared_eye_idx;
extern volatile bool   shared_eye_ready;

// DSP tuning
extern volatile float tuning_dpll_alpha;
extern volatile float tuning_lpf_k;
extern volatile float tuning_sq_snr;
extern volatile float shared_agc_gain;
extern volatile bool  shared_agc_enabled;
extern volatile bool  shared_serial_diag;
extern volatile bool  shared_save_request;

// Automation requests / state
extern volatile bool  shared_search_request;
extern volatile int   shared_search_state;     // 0=idle, 1=searching, 2=found, 3=none
extern volatile float shared_active_shift;
extern volatile bool  shared_inv_uncertain;
extern volatile bool  shared_inv_auto;
extern volatile bool  shared_stop_auto;
extern volatile float shared_active_stop;
extern volatile bool  shared_stop_detect_req;
extern volatile int   shared_stop_detect_state;
extern volatile float shared_stop_gap_last;     // last measured gap_fraction (LPF-compensated)
extern volatile int   shared_stop_gap_hist[3];  // running histogram votes during detection
extern volatile bool  shared_baud_auto;
extern volatile bool  shared_baud_detect_req;
extern volatile int   shared_baud_detect_state;
extern volatile float shared_active_baud;
extern volatile bool  shared_chain_stop_after_baud; // set by SEARCH to defer STOP-DET until BAUD-DET completes

// Helper: mark settings as dirty for auto-save debouncing
void flag_settings_change();
