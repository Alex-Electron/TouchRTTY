#pragma once

#include <stdint.h>
#include <math.h>

#define FFT_SIZE 1024
#define PI 3.14159265358979323846f

class SimpleFFT {
private:
    float _twiddle_re[FFT_SIZE / 2];
    float _twiddle_im[FFT_SIZE / 2];
    uint16_t _bit_rev[FFT_SIZE];
    float _window[FFT_SIZE];

public:
    SimpleFFT() {
        int log2n = log2(FFT_SIZE);
        for (int i = 0; i < FFT_SIZE; i++) {
            uint16_t rev = 0;
            for (int j = 0; j < log2n; j++) {
                if ((i >> j) & 1) rev |= (1 << (log2n - 1 - j));
            }
            _bit_rev[i] = rev;
        }

        for (int i = 0; i < FFT_SIZE / 2; i++) {
            _twiddle_re[i] = cosf(-2.0f * PI * i / FFT_SIZE);
            _twiddle_im[i] = sinf(-2.0f * PI * i / FFT_SIZE);
        }

        for (int i = 0; i < FFT_SIZE; i++) {
            _window[i] = 0.5f * (1.0f - cosf(2.0f * PI * i / (FFT_SIZE - 1)));
        }
    }

    void apply_window(float* real) {
        for (int i = 0; i < FFT_SIZE; i++) {
            real[i] *= _window[i];
        }
    }

    void compute(float* real, float* imag) {
        for (int i = 0; i < FFT_SIZE; i++) {
            uint16_t rev = _bit_rev[i];
            if (i < rev) {
                float temp_re = real[i];
                float temp_im = imag[i];
                real[i] = real[rev];
                imag[i] = imag[rev];
                real[rev] = temp_re;
                imag[rev] = temp_im;
            }
        }

        for (int len = 2; len <= FFT_SIZE; len <<= 1) {
            int half_len = len >> 1;
            int step = FFT_SIZE / len;
            for (int i = 0; i < FFT_SIZE; i += len) {
                for (int j = 0; j < half_len; j++) {
                    int idx = i + j;
                    int pair_idx = idx + half_len;
                    int twiddle_idx = j * step;
                    
                    float wr = _twiddle_re[twiddle_idx];
                    float wi = _twiddle_im[twiddle_idx];
                    
                    float tr = real[pair_idx] * wr - imag[pair_idx] * wi;
                    float ti = real[pair_idx] * wi + imag[pair_idx] * wr;
                    
                    real[pair_idx] = real[idx] - tr;
                    imag[pair_idx] = imag[idx] - ti;
                    real[idx] += tr;
                    imag[idx] += ti;
                }
            }
        }
    }

    void calc_magnitude(float* real, float* imag, float* mag) {
        for (int i = 0; i < FFT_SIZE / 2; i++) {
            float pwr = real[i] * real[i] + imag[i] * imag[i];
            mag[i] = 10.0f * log10f(pwr + 1e-10f); 
        }
    }
};