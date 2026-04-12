#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "app_state.hpp"
#include "dsp_pipeline.hpp"
#include "ui_loop.hpp"

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
