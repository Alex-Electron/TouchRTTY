#include <stdio.h>
#include <math.h>
#include <algorithm>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/spi.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/timer.h"
#include "hardware/uart.h"
#include "LGFX_Config.hpp"

// --- Hardware Configuration ---
LGFX_RP2350 tft;

#define ADC_PIN 26
#define SAMPLE_RATE 10000

#define RING_BUFFER_SAMPLES 2048
#define RING_BUFFER_BYTES (RING_BUFFER_SAMPLES * 2)
#define RING_ALIGN_BITS 12 

// ITA2 (Baudot) Character sets
const char ita2_ltrs[32] = {
    0, 'E', '\n', 'A', ' ', 'S', 'I', 'U',
    '\r', 'D', 'R', 'J', 'N', 'F', 'C', 'K',
    'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q',
    'O', 'B', 'G', 0, 'M', 'X', 'V', 0
};

const char ita2_figs[32] = {
    0, '3', '\n', '-', ' ', '\'', '8', '7',
    '\r', '$', '4', '\'', ',', '!', ':', '(',
    '5', '\"', ')', '2', '#', '6', '0', '1',
    '9', '?', '&', 0, '.', '/', ';', 0
};

struct RTTYConfig {
    float mark_freq;
    float shift;
    float baud_rate;
    bool reverse;
};

volatile RTTYConfig current_config = { 2125.0f, 170.0f, 45.45f, false };

uint16_t adc_ring_buffer[RING_BUFFER_SAMPLES] __attribute__((aligned(RING_BUFFER_BYTES)));
int dma_adc_chan;

struct RTTYState {
    float mark_phase = 0.0f, space_phase = 0.0f;
    float mark_i_lpf = 0.0f, mark_q_lpf = 0.0f;
    float space_i_lpf = 0.0f, space_q_lpf = 0.0f;
    float bit_clock = 0.0f;
    float bit_period_samples = 10000.0f / 45.45f;
    bool last_state = false;
    uint16_t shift_reg = 0;
    int bit_count = 0;
    bool in_sync = false;
    bool figs_mode = false;
};

RTTYState rtty_state;
const float lpf_alpha = 0.05f; 

void process_rtty_sample(float sample) {
    float m_inc = 2.0f * 3.14159f * current_config.mark_freq / SAMPLE_RATE;
    float s_inc = 2.0f * 3.14159f * (current_config.mark_freq - current_config.shift) / SAMPLE_RATE;
    
    rtty_state.mark_phase = fmodf(rtty_state.mark_phase + m_inc, 2.0f * 3.14159f);
    rtty_state.space_phase = fmodf(rtty_state.space_phase + s_inc, 2.0f * 3.14159f);

    rtty_state.mark_i_lpf += lpf_alpha * (sample * cosf(rtty_state.mark_phase) - rtty_state.mark_i_lpf);
    rtty_state.mark_q_lpf += lpf_alpha * (sample * sinf(rtty_state.mark_phase) - rtty_state.mark_q_lpf);
    float m_pwr = rtty_state.mark_i_lpf * rtty_state.mark_i_lpf + rtty_state.mark_q_lpf * rtty_state.mark_q_lpf;

    rtty_state.space_i_lpf += lpf_alpha * (sample * cosf(rtty_state.space_phase) - rtty_state.space_i_lpf);
    rtty_state.space_q_lpf += lpf_alpha * (sample * sinf(rtty_state.space_phase) - rtty_state.space_q_lpf);
    float s_pwr = rtty_state.space_i_lpf * rtty_state.space_i_lpf + rtty_state.space_q_lpf * rtty_state.space_q_lpf;

    bool current_state = (m_pwr > s_pwr);
    if (current_config.reverse) current_state = !current_state;

    if (current_state != rtty_state.last_state) {
        float err = rtty_state.bit_clock - (rtty_state.bit_period_samples / 2.0f);
        rtty_state.bit_clock -= err * 0.1f; 
        if (!rtty_state.in_sync && !current_state) { 
            rtty_state.in_sync = true; rtty_state.bit_count = 0; rtty_state.shift_reg = 0; rtty_state.bit_clock = 0.0f;
        }
        rtty_state.last_state = current_state;
    }

    rtty_state.bit_clock += 1.0f;

    if (rtty_state.in_sync && rtty_state.bit_clock >= rtty_state.bit_period_samples) {
        rtty_state.bit_clock -= rtty_state.bit_period_samples;
        if (rtty_state.bit_count == 0) {
            if (current_state) rtty_state.in_sync = false; 
        } else if (rtty_state.bit_count <= 5) {
            if (current_state) rtty_state.shift_reg |= (1 << (rtty_state.bit_count - 1));
        } else {
             uint8_t code = rtty_state.shift_reg & 0x1F;
             if (code == 0x1B) rtty_state.figs_mode = true;       
             else if (code == 0x1F) rtty_state.figs_mode = false; 
             else {
                 char c = rtty_state.figs_mode ? ita2_figs[code] : ita2_ltrs[code];
                 if (c) { putchar(c); fflush(stdout); }
             }
             rtty_state.in_sync = false; 
        }
        rtty_state.bit_count++;
    }
}

void core1_main() {
    printf("Core 1: LovyanGFX Starting...\n");
    tft.init();
    
    // Set rotation to Landscape (1 = 90 deg, 3 = 270 deg)
    // Based on user feedback, rotation 1 usually puts the ribbon cable on the left/right.
    tft.setRotation(1); 
    
    // Test Pattern: Color Bars
    tft.startWrite();
    int w = tft.width() / 8;
    tft.fillRect(0 * w, 0, w, tft.height(), TFT_WHITE);
    tft.fillRect(1 * w, 0, w, tft.height(), TFT_YELLOW);
    tft.fillRect(2 * w, 0, w, tft.height(), TFT_CYAN);
    tft.fillRect(3 * w, 0, w, tft.height(), TFT_GREEN);
    tft.fillRect(4 * w, 0, w, tft.height(), TFT_MAGENTA);
    tft.fillRect(5 * w, 0, w, tft.height(), TFT_RED);
    tft.fillRect(6 * w, 0, w, tft.height(), TFT_BLUE);
    tft.fillRect(7 * w, 0, w, tft.height(), TFT_BLACK);
    tft.endWrite();

    printf("Core 1: Color bars drawn. Waiting 3s...\n");
    sleep_ms(3000);

    // Native LovyanGFX Touch Calibration
    uint16_t calData[8];
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(10, 10);
    tft.println("Touch arrows to calibrate");
    tft.calibrateTouch(calData, TFT_WHITE, TFT_BLACK, 15);
    
    printf("Calibration Data: ");
    for(int i=0; i<8; i++) printf("%d ", calData[i]);
    printf("\n");

    // Clear for drawing test
    tft.fillScreen(TFT_BLUE);
    tft.setCursor(10, 10);
    tft.println("Draw! (Filtered Mode - 480x320)");

    uint16_t last_x = 0, last_y = 0;
    
    // Filtering parameters
    const int SAMPLES = 5;
    uint16_t x_buf[SAMPLES], y_buf[SAMPLES];

    while (true) {
        if (multicore_fifo_rvalid()) (void)multicore_fifo_pop_blocking();

        const int SAMPLE_COUNT = 5;
        uint16_t tx[SAMPLE_COUNT], ty[SAMPLE_COUNT];
        int found = 0;

        // 1. Collect a burst of samples
        for (int i = 0; i < SAMPLE_COUNT; i++) {
            if (tft.getTouch(&tx[found], &ty[found])) {
                if (tx[found] > 2 && ty[found] > 2 && tx[found] < 478 && ty[found] < 318) {
                    found++;
                }
            }
            sleep_ms(1); 
        }

        // 2. Consensus Filter: Only accept if we have enough samples and they are close
        if (found >= 3) {
            bool consensus = false;
            uint16_t best_x = 0, best_y = 0;
            int max_cluster = 0;

            for (int i = 0; i < found; i++) {
                int cluster_count = 0;
                for (int j = 0; j < found; j++) {
                    if (abs((int)tx[i] - (int)tx[j]) < 15 && abs((int)ty[i] - (int)ty[j]) < 15) {
                        cluster_count++;
                    }
                }
                if (cluster_count > max_cluster) {
                    max_cluster = cluster_count;
                    best_x = tx[i];
                    best_y = ty[i];
                }
            }

            // Require at least 3 samples to agree on the location
            if (max_cluster >= 3) {
                if (last_x == 0 && last_y == 0) {
                    last_x = best_x; last_y = best_y;
                } else {
                    int dx = abs((int)best_x - (int)last_x);
                    int dy = abs((int)best_y - (int)last_y);
                    if (dx < 50 && dy < 50) {
                        tft.drawLine(last_x, last_y, best_x, best_y, TFT_WHITE);
                        tft.fillCircle(best_x, best_y, 1, TFT_WHITE);
                        last_x = best_x; last_y = best_y;
                    } else {
                        last_x = best_x; last_y = best_y;
                    }
                }
            }
        } else {
            last_x = 0; last_y = 0;
        }
        
        sleep_ms(5);
    }
}

void core0_dsp_loop() {
    uint32_t last_read_idx = 0;
    while (true) {
        uint32_t current_write_idx = (dma_hw->ch[dma_adc_chan].write_addr - (uint32_t)adc_ring_buffer) / 2;
        while (last_read_idx != current_write_idx) {
            uint16_t raw_val = adc_ring_buffer[last_read_idx];
            float sample = ((float)raw_val - 2048.0f) / 2048.0f;
            process_rtty_sample(sample);
            last_read_idx = (last_read_idx + 1) % RING_BUFFER_SAMPLES;
        }
        tight_loop_contents();
    }
}

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_30); sleep_ms(10); set_sys_clock_khz(300000, true);
    stdio_init_all();
    sleep_ms(1000);
    printf("\n\nRTTY DECODER STARTING (LovyanGFX Mode)...\n\n");

    multicore_launch_core1(core1_main);

    adc_init(); adc_gpio_init(ADC_PIN); adc_select_input(0);
    adc_fifo_setup(true, true, 1, false, false);
    adc_set_clkdiv(48000000.0f / SAMPLE_RATE);
    dma_adc_chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(dma_adc_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, RING_ALIGN_BITS);
    channel_config_set_dreq(&c, DREQ_ADC);
    dma_channel_configure(dma_adc_chan, &c, adc_ring_buffer, &adc_hw->result, 0xFFFFFFFF, true);
    adc_run(true);

    core0_dsp_loop();
    return 0;
}