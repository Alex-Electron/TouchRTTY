#include "settings_flash.hpp"
#include "app_state.hpp"
#include "pico/multicore.h"
#include "pico/time.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <stdio.h>
#include <string.h>

void settings_load() {
    AppSettings loaded_set;
    memcpy(&loaded_set, flash_settings_contents, sizeof(AppSettings));
    if (loaded_set.magic != 0xDEADBEEF) return;

    shared_baud_idx   = (loaded_set.baud_idx  >= 0 && loaded_set.baud_idx  <= 4)          ? loaded_set.baud_idx  : 0;
    shared_baud_auto  = (shared_baud_idx == 4);
    shared_shift_idx  = (loaded_set.shift_idx >= 0 && loaded_set.shift_idx <= NUM_SHIFTS) ? loaded_set.shift_idx : 1;
    shared_stop_idx   = loaded_set.stop_idx;
    shared_stop_auto  = (shared_stop_idx == 3) || loaded_set.stop_auto;
    shared_inv_auto   = loaded_set.inv_auto;
    shared_rtty_inv   = loaded_set.rtty_inv;
    shared_exp_scale  = loaded_set.exp_scale;
    tuning_lpf_k      = loaded_set.filter_k;
    tuning_sq_snr     = loaded_set.sq_snr;
    shared_target_freq = loaded_set.target_freq;
    shared_actual_freq = shared_target_freq;
    shared_serial_diag = loaded_set.serial_diag;
    shared_line_width  = (loaded_set.line_width >= 30 && loaded_set.line_width <= 80) ? loaded_set.line_width : 60;
    shared_afc_on      = loaded_set.afc_on;
    shared_font_mode   = loaded_set.font_mode;
    if (loaded_set.dpll_alpha >= 0.005f && loaded_set.dpll_alpha <= 0.200f)
        tuning_dpll_alpha = loaded_set.dpll_alpha;
}

void settings_build_from_state(AppSettings& s, int display_mode, bool auto_scale) {
    s.magic        = 0xDEADBEEF;
    s.baud_idx     = shared_baud_idx;
    s.shift_idx    = shared_shift_idx;
    s.stop_idx     = shared_stop_idx;
    s.rtty_inv     = shared_rtty_inv;
    s.display_mode = display_mode;
    s.exp_scale    = shared_exp_scale;
    s.auto_scale   = auto_scale;
    s.filter_k     = tuning_lpf_k;
    s.sq_snr       = tuning_sq_snr;
    s.target_freq  = shared_target_freq;
    s.serial_diag  = shared_serial_diag;
    s.line_width   = shared_line_width;
    s.afc_on       = shared_afc_on;
    s.font_mode    = shared_font_mode;
    s.dpll_alpha   = tuning_dpll_alpha;
    s.inv_auto     = shared_inv_auto;
    s.stop_auto    = shared_stop_auto;
}

void settings_write_to_flash(const AppSettings& s) {
    uint8_t page_buf[FLASH_PAGE_SIZE] = {0};
    memcpy(page_buf, &s, sizeof(AppSettings));
    uint32_t t0 = time_us_32();
    printf("[SAVE] writing flash (DSP paused ~45ms)...\n");
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_FLASH_OFFSET, page_buf, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
    printf("[SAVE] done in %lu us\n", time_us_32() - t0);
}

void settings_save_now(int display_mode, bool auto_scale) {
    AppSettings s;
    settings_build_from_state(s, display_mode, auto_scale);
    settings_write_to_flash(s);
    settings_need_save = false;
}

void load_or_calibrate(lgfx::LGFX_Device& tft, bool force) {
    uint16_t calData[8];
    bool valid = true;
    for (int i = 0; i < 8; i++) {
        calData[i] = ((uint16_t*)flash_target_contents)[i];
        if (calData[i] == 0xFFFF || calData[i] == 0) valid = false;
    }

    if (valid && !force) {
        tft.setTouchCalibrate(calData);
    } else {
        tft.fillScreen(0x000000U);
        tft.setTextColor(0xFFFFFFU, 0x000000U);
        tft.setTextSize(2);
        tft.setTextDatum(lgfx::middle_center);
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
