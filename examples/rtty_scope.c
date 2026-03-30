#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_adc.h>
#include <gui/gui.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define FFT_SIZE 512

float fft_real[FFT_SIZE];
float fft_imag[FFT_SIZE];
float fft_output[FFT_SIZE / 2];
float hanning_window[FFT_SIZE];

typedef struct {
    FuriMutex* mutex;
    float current_fft[FFT_SIZE / 2];
    uint16_t avg_bias;
    uint16_t p2p_vol;
    float actual_sample_rate;
    bool running;
} RttyApp;

// --- Pre-calculate Hanning Window ---
static void precalculate_hanning(void) {
    for(int i = 0; i < FFT_SIZE; i++) {
        hanning_window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * (float)i / (float)(FFT_SIZE - 1)));
    }
}

// --- Custom Standalone Fast Fourier Transform (Cooley-Tukey) ---
static void custom_fft(float* fr, float* fi, int N) {
    int j = 0;
    for (int i = 0; i < N - 1; i++) {
        if (i < j) {
            float tr = fr[i], ti = fi[i];
            fr[i] = fr[j]; fi[i] = fi[j];
            fr[j] = tr; fi[j] = ti;
        }
        int m = N / 2;
        while (m <= j) { j -= m; m /= 2; }
        j += m;
    }
    for (int l = 1; l < N; l *= 2) {
        float wr = cosf(M_PI / l);
        float wi = -sinf(M_PI / l);
        for (int i = 0; i < N; i += 2 * l) {
            float wtr = 1.0f, wti = 0.0f;
            for (int k = 0; k < l; k++) {
                int idx1 = i + k;
                int idx2 = idx1 + l;
                float tr = wtr * fr[idx2] - wti * fi[idx2];
                float ti = wtr * fi[idx2] + wti * fr[idx2];
                fr[idx2] = fr[idx1] - tr;
                fi[idx2] = fi[idx1] - ti;
                fr[idx1] += tr;
                fi[idx1] += ti;
                float wtr_old = wtr;
                wtr = wtr_old * wr - wti * wi;
                wti = wtr_old * wi + wti * wr;
            }
        }
    }
}

// --- UI DRAW CALLBACK ---
static void rtty_draw_callback(Canvas* canvas, void* ctx) {
    RttyApp* app = ctx;
    if(furi_mutex_acquire(app->mutex, 20) != FuriStatusOk) return;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);
    
    canvas_draw_str(canvas, 2, 8, "STM32 FFT (SAFE BLOCK)");

    // Dynamically calculate UI markers based on REAL sample rate
    float freq_per_bin = app->actual_sample_rate / (float)FFT_SIZE;
    int x1450 = 0;
    int x1620 = 0;
    
    if (freq_per_bin > 0) {
        x1450 = (int)(1450.0f / freq_per_bin / 4.0f); // Scale / 4 for UI fitting
        x1620 = (int)(1620.0f / freq_per_bin / 4.0f);
    }
    
    canvas_draw_line(canvas, 15, 34, 15, 63); 
    canvas_draw_line(canvas, 15, 63, 120, 63); 
    canvas_draw_str(canvas, 2, 60, "F");

    float max_mag = 1.0f;
    for(int i = 0; i < 120; i++) if(app->current_fft[i] > max_mag) max_mag = app->current_fft[i];

    for(int i = 0; i < 105; i++) {
        int h = (int)((app->current_fft[i] / max_mag) * 28.0f);
        if(h > 28) h = 28;
        canvas_draw_line(canvas, 15 + i, 63, 15 + i, 63 - h);
    }

    if (x1450 > 0 && x1450 < 105) {
        canvas_draw_line(canvas, 15 + x1450, 64, 15 + x1450, 68);
        canvas_draw_str(canvas, 15 + x1450 - 10, 30, "1450");
    }
    if (x1620 > 0 && x1620 < 105) {
        canvas_draw_line(canvas, 15 + x1620, 64, 15 + x1620, 68);
        canvas_draw_str(canvas, 15 + x1620 - 10, 42, "1620");
    }

    char info[64];
    snprintf(info, sizeof(info), "B:%u P2P:%u SR:%.0f", app->avg_bias, app->p2p_vol, (double)app->actual_sample_rate);
    canvas_draw_str(canvas, 2, 18, info);

    furi_mutex_release(app->mutex);
}

static void rtty_input_callback(InputEvent* input_event, void* ctx) {
    furi_message_queue_put((FuriMessageQueue*)ctx, input_event, FuriWaitForever);
}

int32_t rtty_scope_main(void* p) {
    UNUSED(p);
    
    // Enable DWT Cycle Counter hardware (just in case OS disabled it)
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    RttyApp* app = malloc(sizeof(RttyApp));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->running = true;
    app->avg_bias = 0;
    app->p2p_vol = 0;
    app->actual_sample_rate = 10000.0f; // Initial guess
    memset(app->current_fft, 0, sizeof(app->current_fft));
    
    precalculate_hanning();

    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, rtty_draw_callback, app);
    view_port_input_callback_set(view_port, rtty_input_callback, event_queue);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    
    FuriHalAdcHandle* adc = furi_hal_adc_acquire();
    furi_hal_adc_configure_ex(adc, FuriHalAdcScale2048, FuriHalAdcClockSync64, FuriHalAdcOversampleNone, FuriHalAdcSamplingtime12_5);

    while(app->running) {
        InputEvent event;
        // Check for exit button
        if(furi_message_queue_get(event_queue, &event, 0) == FuriStatusOk) {
            if(event.type == InputTypeShort && event.key == InputKeyBack) app->running = false;
        }

        uint32_t bias_acc = 0;
        uint16_t min_v = 4096, max_v = 0;
        
        // --- 1. BLOCK SAMPLING WITH SELF-TIMING ---
        uint32_t t_start = DWT->CYCCNT;
        
        for(int i = 0; i < FFT_SIZE; i++) {
            uint16_t val = furi_hal_adc_read(adc, FuriHalAdcChannel12); // Pin A7
            
            bias_acc += val;
            if(val < min_v) min_v = val;
            if(val > max_v) max_v = val;
            
            // Apply Hanning Window immediately
            fft_real[i] = ((float)val - 2048.0f) * hanning_window[i];
            fft_imag[i] = 0.0f;
            
            // Small delay to achieve roughly 10-12kHz sample rate
            furi_delay_us(65); 
        }
        
        uint32_t t_end = DWT->CYCCNT;
        
        // --- 2. CALCULATE ACTUAL SAMPLE RATE ---
        // 64000000 is the CPU clock speed of STM32WB55
        float time_taken_sec = (float)(t_end - t_start) / 64000000.0f;
        float real_sr = (float)FFT_SIZE / time_taken_sec;

        // --- 3. RUN FFT ---
        custom_fft(fft_real, fft_imag, FFT_SIZE);

        for(int i = 0; i < FFT_SIZE / 2; i++) {
            fft_output[i] = sqrtf(fft_real[i]*fft_real[i] + fft_imag[i]*fft_imag[i]);
        }

        // --- 4. UPDATE UI ---
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        memcpy(app->current_fft, fft_output, sizeof(fft_output));
        app->avg_bias = bias_acc / FFT_SIZE;
        app->p2p_vol = max_v - min_v;
        app->actual_sample_rate = real_sr;
        furi_mutex_release(app->mutex);
        
        view_port_update(view_port);
        
        // Feed the OS Watchdog! Crucial to prevent reboot.
        furi_delay_ms(1); 
    }

    furi_hal_adc_release(adc);
    gui_remove_view_port(gui, view_port); view_port_free(view_port);
    furi_record_close(RECORD_GUI); furi_message_queue_free(event_queue); furi_mutex_free(app->mutex); free(app);
    return 0;
}