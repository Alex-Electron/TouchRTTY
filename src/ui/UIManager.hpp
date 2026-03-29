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

    // Windows 3.11 UI Colors
    static constexpr uint32_t WIN_BTN_FACE   = 0xC618U; // Light Gray
    static constexpr uint32_t WIN_BTN_TEXT   = 0x0000U; // Black
    static constexpr uint32_t WIN_BTN_HILITE = 0xFFFFU; // White
    static constexpr uint32_t WIN_BTN_SHADOW = 0x8410U; // Dark Gray

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
            
            // Yaesu Flat Style with Rounded Corners
            uint32_t bg_color = 0x2104U; // Dark Grey
            uint32_t border_color = 0x8410U; // Light Grey
            
            if (i == 4 && auto_scale) {
                bg_color = 0x03E0U; // Dark Green when AUTO is active
                border_color = 0x07E0U; // Bright Green border
            }
            
            _spr_bottom.fillRoundRect(x + 2, y + 2, btn_w - 4, btn_h - 4, 6, bg_color);
            _spr_bottom.drawRoundRect(x + 2, y + 2, btn_w - 4, btn_h - 4, 6, border_color);
            
            _spr_bottom.setTextColor(0xFFFFU); // White text
            
            char label[16];
            if (i == 4) {
                snprintf(label, sizeof(label), "AUTO");
            } else if (i == 5) {
                snprintf(label, sizeof(label), "SCL: %s", exp_scale ? "EXP" : "LIN");
            } else {
                snprintf(label, sizeof(label), "%s", labels[i]);
            }
            
            _spr_bottom.drawString(label, x + (btn_w / 2), y + (btn_h / 2));
        }

        ili9488_push_colors(0, UI_Y_BOTTOM, 480, UI_BOTTOM_BAR_H, (uint16_t*)_spr_bottom.getBuffer());
    }

    void updateTopBar(float adc_v, uint32_t fps, float signal_db, float marker_freq) {
        _spr_top.fillSprite(COLOR_BG);
        _spr_top.drawFastHLine(0, UI_TOP_BAR_H - 1, 480, COLOR_GRID); 

        _spr_top.setTextDatum(middle_left);
        _spr_top.setTextColor(COLOR_TEXT, COLOR_BG);
        _spr_top.setFont(&fonts::Font2);
        _spr_top.drawString("SIG", 5, 12);
        
        _spr_top.drawRect(40, 5, 120, 14, COLOR_GRID);
        int lvl_w = (int)((signal_db + 80.0f) * (120.0f / 70.0f));
        if (lvl_w < 0) lvl_w = 0;
        if (lvl_w > 120) lvl_w = 120;
        
        uint32_t bar_color = (signal_db > -30.0f) ? TFT_RED : COLOR_GOOD;
        _spr_top.fillRect(40, 5, lvl_w, 14, bar_color);

        char buf[32];
        snprintf(buf, sizeof(buf), "%3.0f dB", signal_db);
        _spr_top.drawString(buf, 165, 12);

        _spr_top.setTextColor(TFT_YELLOW, COLOR_BG);
        snprintf(buf, sizeof(buf), "MRK: %4.0f Hz", marker_freq);
        _spr_top.drawString(buf, 240, 12);

        _spr_top.setTextColor(COLOR_FREQ, COLOR_BG);
        _spr_top.drawString("RTTY 45", 5, 32);
        _spr_top.setTextColor(COLOR_TEXT, COLOR_BG);
        _spr_top.drawString("SH: 170", 90, 32);

        _spr_top.setTextColor(COLOR_BTN_LINE, COLOR_BG);
        _spr_top.setTextDatum(top_right);
        snprintf(buf, sizeof(buf), "ADC: %.2fV", adc_v);
        _spr_top.drawString(buf, 470, 5);
        snprintf(buf, sizeof(buf), "FPS: %lu", fps);
        _spr_top.drawString(buf, 470, 24);

        ili9488_push_colors(0, UI_Y_TOP, 480, UI_TOP_BAR_H, (uint16_t*)_spr_top.getBuffer());
    }

    void drawTextZonePlaceholder() {
        _spr_text.fillSprite(COLOR_BG);
        _spr_text.drawFastHLine(0, UI_TEXT_ZONE_H - 1, 480, COLOR_GRID); 

        _spr_text.setTextDatum(top_left);
        _spr_text.setFont(&fonts::Font2); 
        _spr_text.setTextColor(COLOR_GOOD, COLOR_BG);
        
        int start_y = 5;
        _spr_text.drawString("SYSTEM READY. WAITING FOR SYNC...", 5, start_y);
        _spr_text.setTextColor(TFT_YELLOW, COLOR_BG);
        _spr_text.drawString("TEST GEN: 1kHz (Pin 32) & 1.45kHz (Pin 34)", 5, start_y + 20);
        _spr_text.setTextColor(TFT_DARKGREY, COLOR_BG);
        _spr_text.drawString("Use Bottom Buttons to adjust Floor/Gain/Scale.", 5, start_y + 40);

        ili9488_push_colors(0, UI_Y_TEXT, 480, UI_TEXT_ZONE_H, (uint16_t*)_spr_text.getBuffer());
    }
};

#endif