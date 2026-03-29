#ifndef UI_MANAGER_HPP
#define UI_MANAGER_HPP

#include <LovyanGFX.hpp>
#include "../display/ili9341_test.h"

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

    static constexpr uint32_t COLOR_BG       = 0x0000U; 
    static constexpr uint32_t COLOR_GRID     = 0x4208U; 
    static constexpr uint32_t COLOR_FREQ     = 0xFD20U; 
    static constexpr uint32_t COLOR_TEXT     = 0xFFFFU; 
    static constexpr uint32_t COLOR_BTN_LINE = 0x07FFU; 
    static constexpr uint32_t COLOR_GOOD     = 0x07E0U; 

public:
    UIManager(lgfx::LGFX_Device* tft) : _tft(tft), _spr_top(tft), _spr_text(tft), _spr_bottom(tft) {}

    void init() {
        _spr_top.setColorDepth(16);
        _spr_top.createSprite(480, UI_TOP_BAR_H);
        
        _spr_text.setColorDepth(16);
        _spr_text.createSprite(480, UI_TEXT_ZONE_H);

        _spr_bottom.setColorDepth(16);
        _spr_bottom.createSprite(480, UI_BOTTOM_BAR_H);

        _tft->fillScreen(COLOR_BG);
        drawTextZonePlaceholder();
    }

    void drawBottomBar(bool auto_scale, bool exp_scale) {
        _spr_bottom.fillSprite(COLOR_BG);
        
        const char* labels[6] = {"FL-", "FL+", "GN-", "GN+", "AUTO", "SCALE"};
        int btn_w = 480 / 6; 
        int btn_h = UI_BOTTOM_BAR_H; 
        
        _spr_bottom.setFont(&fonts::Font2); 
        _spr_bottom.setTextDatum(middle_center);
        
        for (int i = 0; i < 6; i++) {
            int x = i * btn_w;
            int y = 0;
            
            // Yaesu Flat Style with 3px thick Rounded Corners
            uint32_t bg_color = 0x2104U; // Dark Grey
            uint32_t border_color = 0x8410U; // Light Grey
            
            if (i == 4 && auto_scale) {
                bg_color = 0x03E0U; // Dark Green
                border_color = 0x07E0U; // Bright Green
            }
            
            _spr_bottom.fillRoundRect(x + 2, 2, btn_w - 4, btn_h - 4, 6, bg_color);
            
            // Thick border (3px)
            _spr_bottom.drawRoundRect(x + 2, 2, btn_w - 4, btn_h - 4, 6, border_color);
            _spr_bottom.drawRoundRect(x + 3, 3, btn_w - 6, btn_h - 6, 5, border_color);
            _spr_bottom.drawRoundRect(x + 4, 4, btn_w - 8, btn_h - 8, 4, border_color);
            
            _spr_bottom.setTextColor(0xFFFFU); 
            
            char label[16];
            if (i == 4) {
                snprintf(label, sizeof(label), "AUTO");
            } else if (i == 5) {
                snprintf(label, sizeof(label), "SCL: %s", exp_scale ? "EXP" : "LIN");
            } else {
                snprintf(label, sizeof(label), "%s", labels[i]);
            }
            
            _spr_bottom.drawString(label, x + (btn_w / 2), (btn_h / 2));
        }

        ili9488_push_colors(0, UI_Y_BOTTOM, 480, UI_BOTTOM_BAR_H, (uint16_t*)_spr_bottom.getBuffer());
    }

    void updateTopBar(float adc_v, uint32_t fps, float signal_db, float snr_db, float marker_freq, bool is_clipping) {
        _spr_top.fillSprite(COLOR_BG);
        _spr_top.drawFastHLine(0, UI_TOP_BAR_H - 1, 480, COLOR_GRID); 

        _spr_top.setTextDatum(middle_left);
        _spr_top.setTextColor(COLOR_TEXT, COLOR_BG);
        _spr_top.setFont(&fonts::Font2);
        _spr_top.drawString("SIG", 5, 12);
        
        _spr_top.drawRect(40, 5, 120, 14, COLOR_GRID);
        int lvl_w = (int)((signal_db + 80.0f) * (120.0f / 70.0f));
        if (lvl_w < 0) lvl_w = 0; if (lvl_w > 120) lvl_w = 120;
        uint32_t bar_color = (signal_db > -30.0f) ? 0xF800U : COLOR_GOOD;
        _spr_top.fillRect(40, 5, lvl_w, 14, bar_color);
        
        char buf[32];
        snprintf(buf, sizeof(buf), "%3.0f dB", signal_db);
        _spr_top.drawString(buf, 165, 12);

        // --- CLIP WARNING ---
        if (is_clipping) {
            _spr_top.fillRoundRect(225, 2, 50, 20, 4, 0xF800U);
            _spr_top.setTextColor(0xFFFFU);
            _spr_top.setTextDatum(middle_center);
            _spr_top.drawString("CLIP", 250, 12);
            _spr_top.setTextDatum(middle_left);
        } else {
            _spr_top.setTextColor(0xFFE0U, COLOR_BG);
            snprintf(buf, sizeof(buf), "MRK:%4.0fHz", marker_freq);
            _spr_top.drawString(buf, 225, 12);
        }

        _spr_top.setTextColor(TFT_WHITE, COLOR_BG);
        _spr_top.drawString("RTTY 45", 5, 32);
        _spr_top.setTextColor(TFT_WHITE, COLOR_BG);
        _spr_top.drawString("SH: 170", 90, 32);
        
        _spr_top.setTextColor(0x07E0U, COLOR_BG);
        snprintf(buf, sizeof(buf), "SNR:%2.0fdB", snr_db);
        _spr_top.drawString(buf, 170, 32);

        // --- SYSTEM DIAGNOSTIC & ZERO TUNING ---
        _spr_top.setTextDatum(top_right);
        _spr_top.setTextColor(COLOR_BTN_LINE, COLOR_BG);
        snprintf(buf, sizeof(buf), "FPS: %lu", fps);
        _spr_top.drawString(buf, 470, 24);

        int meter_w = 60;
        int meter_x = 470 - meter_w;
        int meter_y = 6;
        _spr_top.setTextDatum(middle_right);
        _spr_top.setTextColor(COLOR_TEXT, COLOR_BG);
        _spr_top.drawString("ZERO", meter_x - 5, meter_y + 4);
        _spr_top.drawFastHLine(meter_x, meter_y + 4, meter_w, COLOR_GRID);
        _spr_top.drawFastVLine(meter_x + meter_w / 2, meter_y, 9, COLOR_TEXT);
        _spr_top.drawFastVLine(meter_x, meter_y + 2, 5, COLOR_GRID); // Left
        _spr_top.drawFastVLine(meter_x + meter_w, meter_y + 2, 5, COLOR_GRID); // Right
        
        float bias_err = adc_v - 1.65f;
        float normalized_err = bias_err / 0.5f;
        if (normalized_err < -1.0f) normalized_err = -1.0f;
        if (normalized_err > 1.0f) normalized_err = 1.0f;
        int needle_x = meter_x + (meter_w / 2) + (int)(normalized_err * (meter_w / 2));
        
        uint32_t needle_color = (abs(bias_err) < 0.05f) ? 0x07E0U : 0xF800U;
        _spr_top.fillTriangle(needle_x, meter_y + 4, needle_x - 3, meter_y - 2, needle_x + 3, meter_y - 2, needle_color);
        _spr_top.fillTriangle(needle_x, meter_y + 4, needle_x - 3, meter_y + 10, needle_x + 3, meter_y + 10, needle_color);

        ili9488_push_colors(0, UI_Y_TOP, 480, UI_TOP_BAR_H, (uint16_t*)_spr_top.getBuffer());
    }

    void drawTextZonePlaceholder() {
        _spr_text.fillSprite(COLOR_BG);
        _spr_text.drawFastHLine(0, UI_TEXT_ZONE_H - 1, 480, COLOR_GRID); 
        _spr_text.setTextDatum(top_left);
        _spr_text.setFont(&fonts::Font2); 
        _spr_text.setTextColor(TFT_CYAN, COLOR_BG);
        _spr_text.drawString("Audio Input Active. Auto-Scale Enabled.", 5, 5);
        ili9488_push_colors(0, UI_Y_TEXT, 480, UI_TEXT_ZONE_H, (uint16_t*)_spr_text.getBuffer());
    }
};

#endif