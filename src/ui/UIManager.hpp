#ifndef UI_MANAGER_HPP
#define UI_MANAGER_HPP

#include <LovyanGFX.hpp>
#include "../display/ili9341_test.h"
#include "../version.h"

// Hardcoded Palette (Yellow Spectrum on Blue Background)
static constexpr uint32_t PAL_BG = 0x000033U;
static constexpr uint32_t PAL_GRID = 0x003366U;
static constexpr uint32_t PAL_WAVE = 0xFFFF00U; // Yellow wave
static constexpr uint32_t PAL_PEAK = 0xFFFF00U; // Yellow peak
static constexpr uint32_t PAL_TEXT = 0xFFFFFFU; // White text

#define UI_TOP_BAR_H   48
#define UI_DSP_ZONE_H  112
#define UI_TEXT_ZONE_H 112
#define UI_BOTTOM_BAR_H 48
#define UI_Y_TOP       0
#define UI_Y_DSP       (UI_TOP_BAR_H)
#define UI_Y_TEXT      (UI_Y_DSP + UI_DSP_ZONE_H)
#define UI_Y_BOTTOM    (UI_Y_TEXT + UI_TEXT_ZONE_H)

class UIManager {
private:
    lgfx::LGFX_Device* _tft;
    LGFX_Sprite _spr_top;
    LGFX_Sprite _spr_text;
    LGFX_Sprite _spr_bottom;
    static constexpr uint32_t COLOR_BG = 0x000000U; 
    static constexpr uint32_t COLOR_GRID = 0x444444U; 
    static constexpr uint32_t COLOR_TEXT = 0xFFFFFFU; 

public:
    UIManager(lgfx::LGFX_Device* tft) : _tft(tft), _spr_top(tft), _spr_text(tft), _spr_bottom(tft) {}
    
    void init() {
        _spr_top.setColorDepth(16); _spr_top.createSprite(480, UI_TOP_BAR_H);
        _spr_text.setColorDepth(16); _spr_text.createSprite(480, UI_TEXT_ZONE_H);
        _spr_bottom.setColorDepth(16); _spr_bottom.createSprite(480, UI_BOTTOM_BAR_H);
        _tft->fillScreen(COLOR_BG);
    }
    
    void drawBottomBar(bool auto_scale, bool exp_scale) {
        _spr_bottom.fillSprite(COLOR_BG);
        const char* labels[6] = {"FL-", "FL+", "GN-", "GN+", "AUTO", "SCL"};
        int btn_w = 480 / 6; _spr_bottom.setFont(&fonts::Font2); _spr_bottom.setTextDatum(middle_center);
        for (int i = 0; i < 6; i++) {
            int x = i * btn_w; 
            uint32_t bg = 0x333333U, brd = 0x777777U;
            if (i == 4 && auto_scale) { bg = 0x006600U; brd = 0x00FF00U; }
            _spr_bottom.fillRoundRect(x + 2, 2, btn_w - 4, UI_BOTTOM_BAR_H - 4, 6, bg);
            _spr_bottom.drawRoundRect(x + 2, 2, btn_w - 4, UI_BOTTOM_BAR_H - 4, 6, brd);
            _spr_bottom.drawRoundRect(x + 3, 3, btn_w - 6, UI_BOTTOM_BAR_H - 6, 5, brd);
            _spr_bottom.drawRoundRect(x + 4, 4, btn_w - 8, UI_BOTTOM_BAR_H - 8, 4, brd);
            _spr_bottom.setTextColor(0xFFFFFFU); 
            
            char label[16];
            if (i == 5) snprintf(label, sizeof(label), exp_scale ? "EXP" : "LIN");
            else snprintf(label, sizeof(label), "%s", labels[i]);
            
            _spr_bottom.drawString(label, x + (btn_w / 2), (UI_BOTTOM_BAR_H / 2));
        }
        ili9488_push_colors(0, UI_Y_BOTTOM, 480, 48, (uint16_t*)_spr_bottom.getBuffer());
    }
    
    void updateTopBar(float adc_v, uint32_t fps, float signal_db, float snr_db, float marker_freq, bool clipping) {
        _spr_top.fillSprite(COLOR_BG); _spr_top.drawFastHLine(0, 47, 480, COLOR_GRID); 
        _spr_top.setTextDatum(middle_left); _spr_top.setTextColor(COLOR_TEXT, COLOR_BG); _spr_top.setFont(&fonts::Font2);
        _spr_top.drawString("SIG", 5, 12); _spr_top.drawRect(40, 5, 120, 14, COLOR_GRID);
        int lw = (int)((signal_db+80)*(120/70.0f)); if(lw<0) lw=0; if(lw>120) lw=120;
        _spr_top.fillRect(40, 5, lw, 14, (signal_db > -30.0f) ? 0xFF0000U : 0x00FF00U);
        char buf[32]; snprintf(buf, sizeof(buf), "%3.0f dB", signal_db); _spr_top.drawString(buf, 165, 12);
        
        if (clipping) { 
            _spr_top.fillRoundRect(225, 2, 50, 20, 4, 0xFF0000U); 
            _spr_top.setTextColor(0xFFFFFFU); _spr_top.setTextDatum(middle_center); 
            _spr_top.drawString("CLIP", 250, 12); _spr_top.setTextDatum(middle_left); 
        } else { 
            _spr_top.setTextColor(0xFFFF00U, COLOR_BG); snprintf(buf, sizeof(buf), "MRK:%4.0fHz", marker_freq); 
            _spr_top.drawString(buf, 225, 12); 
        }
        
        _spr_top.setTextColor(0xFFFFFFU, COLOR_BG); _spr_top.drawString("RTTY 45  SH: 170", 5, 32);
        _spr_top.setTextColor(0x00FF00U, COLOR_BG); snprintf(buf, sizeof(buf), "SNR:%2.0fdB", snr_db); _spr_top.drawString(buf, 170, 32);
        
        _spr_top.setTextDatum(top_right); _spr_top.setTextColor(0x00FFFFU, COLOR_BG); 
        snprintf(buf, sizeof(buf), "B:%d FPS:%lu", BUILD_NUMBER, fps); _spr_top.drawString(buf, 470, 24);
        
        // Zero Bias Meter
        int meter_w = 60, meter_x = 400, meter_y = 6; 
        _spr_top.setTextDatum(middle_right); _spr_top.setTextColor(0xFFFFFFU, COLOR_BG); 
        _spr_top.drawString("ZERO", meter_x - 5, meter_y + 4); 
        
        _spr_top.drawFastHLine(meter_x, meter_y+4, meter_w, COLOR_GRID); 
        _spr_top.drawFastVLine(meter_x+30, meter_y, 9, COLOR_TEXT); // Center
        
        float err = adc_v - 1.65f; 
        float norm_err = err / 0.5f;
        if (norm_err < -1.0f) norm_err = -1.0f;
        if (norm_err > 1.0f) norm_err = 1.0f;
        int nx = meter_x + 30 + (int)(norm_err * 30);

        uint32_t nc = (abs(err) < 0.05f) ? 0x00FF00U : 0xFF0000U; // Green if good, Red if bad
        _spr_top.fillTriangle(nx, meter_y+4, nx-3, meter_y-2, nx+3, meter_y-2, nc); 
        _spr_top.fillTriangle(nx, meter_y+4, nx-3, meter_y+10, nx+3, meter_y+10, nc);

        ili9488_push_colors(0, UI_Y_TOP, 480, UI_TOP_BAR_H, (uint16_t*)_spr_top.getBuffer());
    }

    void drawInfo() {
        _spr_text.fillSprite(COLOR_BG); 
        _spr_text.drawFastHLine(0, 111, 480, COLOR_GRID); 
        _spr_text.setTextDatum(top_left); _spr_text.setFont(&fonts::Font2); 
        
        _spr_text.setTextColor(0x00FF00U, COLOR_BG); 
        _spr_text.drawString("ACTIVE MODE: 11 (Hardcoded Build 107 Engine)", 5, 5);
        
        _spr_text.setTextColor(0xFFFFFFU, COLOR_BG); 
        _spr_text.drawString("Hardware: 16-bit Swapped, BGR out", 5, 25);
        
        _spr_text.setTextColor(0x00FFFFU, COLOR_BG); 
        _spr_text.drawString("Audio Input Active. AGC Running.", 5, 45);

        ili9488_push_colors(0, UI_Y_TEXT, 480, 112, (uint16_t*)_spr_text.getBuffer());
    }
};
#endif