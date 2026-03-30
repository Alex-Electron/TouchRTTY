#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_adc.h>
#include <gui/gui.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define FFT_SIZE 512
#define TARGET_SAMPLE_RATE 10240.0f
// 64MHz CPU Clock / 10240 Hz = Exactly 6250 CPU cycles per sample
#define CYCLES_PER_SAMPLE 6250 

float fft_real[FFT_SIZE];
float fft_imag[FFT_SIZE];
float fft_output[FFT_SIZE / 2];
float hanning_window[FFT_SIZE];

typedef struct {
    FuriMutex* mutex;
    float current_fft[FFT_SIZE / 2];
    uint16_t avg_bias;
    uint16_t p2p_vol;
    
    // Detected Frequency
    float peak_freq;
    
    bool running;
} RttyApp;

// --- Pre-calculate Hanning Window ---
static void precalculate_hanning(void) {
    for(int i = 0; i < FFT_SIZE; i++) {
        hanning_window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * (float)i / (float)(FFT_SIZE - 1)));
    }
}

// --- Custom Standalone Fast Fourier Transform ---
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
    
    canvas_draw_str(canvas, 2, 8, "STM32 ABSOLUTE CALIBRATOR");

    // Display the detected frequency prominently
    char peak_info[64];
    snprintf(peak_info, sizeof(peak_info), "TONE: %.1f Hz", (double)app->peak_freq);
    
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 22, peak_info);
    
    canvas_set_font(canvas, FontSecondary);
    char hw_info[64];
    snprintf(hw_info, sizeof(hw_info), "B:%u P2P:%u", app->avg_bias, app->p2p_vol);
    canvas_draw_str(canvas, 2, 32, hw_info);

    // Fixed math for UI markers (10240 / 512 = exactly 20Hz per bin)
    float freq_per_bin = TARGET_SAMPLE_RATE / (float)FFT_SIZE;
    int x_peak = (int)(app->peak_freq / freq_per_bin / 4.0f); // UI Scale
    
    canvas_draw_line(canvas, 15, 36, 15, 63); 
    canvas_draw_line(canvas, 15, 63, 120, 63); 

    float max_fft_mag = 1.0f;
    for(int i = 0; i < 120; i++) if(app->current_fft[i] > max_fft_mag) max_fft_mag = app->current_fft[i];

    for(int i = 0; i < 105; i++) {
        int h = (int)((app->current_fft[i] / max_fft_mag) * 26.0f);
        if(h > 26) h = 26;
        canvas_draw_line(canvas, 15 + i, 63, 15 + i, 63 - h);
    }

    // Highlight the peak
    if (x_peak > 0 && x_peak < 105 && app->peak_freq > 100) {
        canvas_draw_line(canvas, 15 + x_peak, 64, 15 + x_peak, 68);
    }

    furi_mutex_release(app->mutex);
}

static void rtty_input_callback(InputEvent* input_event, void* ctx) {
    furi_message_queue_put((FuriMessageQueue*)ctx, input_event, FuriWaitForever);
}

int32_t rtty_scope_main(void* p) {
    UNUSED(p);
    
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    RttyApp* app = malloc(sizeof(RttyApp));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->running = true;
    app->avg_bias = 0; app->p2p_vol = 0;
    app->peak_freq = 0;
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
        if(furi_message_queue_get(event_queue, &event, 0) == FuriStatusOk) {
            if(event.type == InputTypeShort && event.key == InputKeyBack) app->running = false;
        }

        uint32_t bias_acc = 0;
        uint16_t min_v = 4096, max_v = 0;
        
        // --- ABSOLUTE TIMING LOOP ---
        uint32_t next_tick = DWT->CYCCNT + CYCLES_PER_SAMPLE;
        
        for(int i = 0; i < FFT_SIZE; i++) {
            // Wait for the exact absolute clock cycle using signed difference for safety
            while((int32_t)(DWT->CYCCNT - next_tick) < 0) {}
            next_tick += CYCLES_PER_SAMPLE;

            uint16_t val = furi_hal_adc_read(adc, FuriHalAdcChannel12); 
            bias_acc += val;
            if(val < min_v) min_v = val;
            if(val > max_v) max_v = val;
            
            fft_real[i] = ((float)val - 2048.0f) * hanning_window[i];
            fft_imag[i] = 0.0f;
        }
        
        float freq_per_bin = TARGET_SAMPLE_RATE / (float)FFT_SIZE; // Exactly 20.0 Hz

        custom_fft(fft_real, fft_imag, FFT_SIZE);

        for(int i = 0; i < FFT_SIZE / 2; i++) {
            fft_output[i] = sqrtf(fft_real[i]*fft_real[i] + fft_imag[i]*fft_imag[i]);
        }

        // --- SINGLE PURE TONE DETECTION ---
        int max_i = -1;
        float max_fft_mag = 0; 
        
        // Search in the 500 Hz to 2500 Hz range
        int start_bin = (int)(500.0f / freq_per_bin);
        int end_bin = (int)(2500.0f / freq_per_bin);

        for(int i = start_bin; i < end_bin; i++) {
            if(fft_output[i] > max_fft_mag) {
                max_fft_mag = fft_output[i];
                max_i = i;
            }
        }

        float current_peak_freq = 0;
        if(max_i > 0 && max_fft_mag > 500.0f) { 
            // Parabolic Interpolation for 1 Hz accuracy
            float a = fft_output[max_i - 1];
            float b = fft_output[max_i];
            float c = fft_output[max_i + 1];
            float p = 0.5f * (a - c) / (a - 2.0f*b + c);
            current_peak_freq = ((float)max_i + p) * freq_per_bin;
        }

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        
        app->peak_freq = current_peak_freq;
        memcpy(app->current_fft, fft_output, sizeof(fft_output));
        app->avg_bias = bias_acc / FFT_SIZE;
        app->p2p_vol = max_v - min_v;
        
        furi_mutex_release(app->mutex);
        
        view_port_update(view_port);
        furi_delay_ms(1); // Keep OS Watchdog happy
    }

    furi_hal_adc_release(adc);
    gui_remove_view_port(gui, view_port); view_port_free(view_port);
    furi_record_close(RECORD_GUI); furi_message_queue_free(event_queue); furi_mutex_free(app->mutex); free(app);
    return 0;
}