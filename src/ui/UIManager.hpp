#ifndef UI_MANAGER_HPP
#define UI_MANAGER_HPP

#include <LovyanGFX.hpp>
#include "../display/ili9341_test.h"
#include "../version.h"
#include <string>
#include <vector>

// Hardcoded Palette (Yellow on Blue - R and B channels swapped for Mode 11 quirk)
static constexpr uint32_t PAL_BG = 0x330000U; 
static constexpr uint32_t PAL_GRID = 0x663300U; 
static constexpr uint32_t PAL_WAVE = 0x00FFFFU; 
static constexpr uint32_t PAL_PEAK = 0x00FFFFU; 
static constexpr uint32_t PAL_TEXT = 0xFFFFFFU; 

#define UI_TOP_BAR_H   34
#define UI_MARKER_H    14
#define UI_DSP_ZONE_H  64
#define UI_TEXT_ZONE_H 160
#define UI_BOTTOM_BAR_H 48

#define UI_Y_TOP       0
#define UI_Y_MARKER    (UI_TOP_BAR_H)
#define UI_Y_DSP       (UI_Y_MARKER + UI_MARKER_H)
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

    std::string rtty_buffer;

public:
    UIManager(lgfx::LGFX_Device* tft) : _tft(tft), _spr_top(tft), _spr_text(tft), _spr_bottom(tft) {}
    
    void init() {
        _spr_top.setColorDepth(16); _spr_top.createSprite(480, UI_TOP_BAR_H);
        _spr_text.setColorDepth(16); _spr_text.createSprite(480, UI_TEXT_ZONE_H);
        _spr_bottom.setColorDepth(16); _spr_bottom.createSprite(480, UI_BOTTOM_BAR_H);
        _tft->fillScreen(COLOR_BG);
        clearRTTY();
    }

    void addRTTYChar(char c) {
        if (rtty_buffer.length() > 500) rtty_buffer.erase(0, 50);
        rtty_buffer += c;
        drawRTTY();
    }

    void clearRTTY() {
        rtty_buffer = "RTTY TERMINAL READY... ";
        drawRTTY();
    }

    void drawRTTY() {
        _spr_text.fillSprite(COLOR_BG);
        _spr_text.drawFastHLine(0, 0, 480, COLOR_GRID);
        _spr_text.setTextColor(0x00FF00U, COLOR_BG); // Green
        _spr_text.setFont(&fonts::Font2);
        _spr_text.setTextDatum(top_left);
        
        // Simple line wrapping
        int x = 5, y = 10;
        for (char c : rtty_buffer) {
            char s[2] = {c, 0};
            _spr_text.drawString(s, x, y);
            x += _spr_text.textWidth(s);
            if (x > 460 || c == '\n') { x = 5; y += 18; }
            if (y > 140) { _spr_text.fillSprite(COLOR_BG); x = 5; y = 10; }
        }
        
        ili9488_push_colors(0, UI_Y_TEXT, 480, UI_TEXT_ZONE_H, (uint16_t*)_spr_text.getBuffer());
    }
    
    void drawBottomBar(bool auto_scale, bool exp_scale, bool menu_mode, int display_mode, int baud_idx, int shift_idx, float stop_bits, bool palette) {
        _spr_bottom.fillSprite(COLOR_BG);
        const int bauds[] = {45, 50, 75};
        const int shifts[] = {170, 200, 425, 450, 850};
        
        char labels_main[6][16];
        snprintf(labels_main[0], 16, "B %d", bauds[baud_idx]);
        snprintf(labels_main[1], 16, "S %d", shifts[shift_idx]);
        snprintf(labels_main[2], 16, "ST %.1f", stop_bits);
        snprintf(labels_main[3], 16, "AUTO");
        snprintf(labels_main[4], 16, "CLEAR");
        snprintf(labels_main[5], 16, "MENU");
        
        char labels_menu[6][16];
        if (display_mode == 0) snprintf(labels_menu[0], 16, "WF");
        else if (display_mode == 1) snprintf(labels_menu[0], 16, "SPEC");
        else snprintf(labels_menu[0], 16, "OSC");
        snprintf(labels_menu[1], 16, exp_scale ? "EXP" : "LIN");
        snprintf(labels_menu[2], 16, "FL+/-");
        snprintf(labels_menu[3], 16, "GN+/-");
        snprintf(labels_menu[4], 16, palette ? "PAL ON" : "PAL OFF");
        snprintf(labels_menu[5], 16, "BACK");

        int btn_w = 480 / 6; _spr_bottom.setFont(&fonts::Font2); _spr_bottom.setTextDatum(middle_center);
        for (int i = 0; i < 6; i++) {
            int x = i * btn_w; 
            uint32_t bg = 0x333333U, brd = 0x777777U;
            if (!menu_mode && i == 3 && auto_scale) { bg = 0x006600U; brd = 0x00FF00U; }
            if (menu_mode && i == 5) { bg = 0x660000U; brd = 0xFF0000U; } 
            
            _spr_bottom.fillRoundRect(x + 2, 2, btn_w - 4, UI_BOTTOM_BAR_H - 4, 6, bg);
            _spr_bottom.drawRoundRect(x + 2, 2, btn_w - 4, UI_BOTTOM_BAR_H - 4, 6, brd);
            _spr_bottom.drawRoundRect(x + 3, 3, btn_w - 6, UI_BOTTOM_BAR_H - 6, 5, brd);
            _spr_bottom.drawRoundRect(x + 4, 4, btn_w - 8, UI_BOTTOM_BAR_H - 8, 4, brd);
            _spr_bottom.setTextColor(0xFFFFFFU); 
            
            _spr_bottom.drawString(menu_mode ? labels_menu[i] : labels_main[i], x + (btn_w / 2), (UI_BOTTOM_BAR_H / 2));
        }
        ili9488_push_colors(0, UI_Y_BOTTOM, 480, 48, (uint16_t*)_spr_bottom.getBuffer());
    }
    
    void updateTopBar(float adc_v, uint32_t fps, float signal_db, float snr_db, float m_freq, float s_freq, bool clipping, float load_c0, float load_c1) {
        _spr_top.fillSprite(COLOR_BG); _spr_top.drawFastHLine(0, 33, 480, COLOR_GRID); 
        _spr_top.setTextDatum(middle_left); _spr_top.setTextColor(COLOR_TEXT, COLOR_BG); _spr_top.setFont(&fonts::Font2);
        
        _spr_top.drawString("SIG", 5, 8); _spr_top.drawRect(40, 1, 120, 14, COLOR_GRID);
        int lw = (int)((signal_db+80)*(120/70.0f)); if(lw<0) lw=0; if(lw>120) lw=120;
        uint32_t sig_color = 0x00FF00U; 
        if (clipping) sig_color = 0x0000FFU; // Red visual
        else if (signal_db > -30.0f) sig_color = 0xFF0000U; // Blue visual
        _spr_top.fillRect(40, 1, lw, 14, sig_color);
        
        _spr_top.setTextColor(0x00FFFFU, COLOR_BG); // Yellow visual
        char buf[64]; snprintf(buf, sizeof(buf), "M:%.0f S:%.0f", m_freq, s_freq); 
        _spr_top.drawString(buf, 170, 8); 
        
        _spr_top.setTextColor(0xFFFFFFU, COLOR_BG); _spr_top.drawString("RTTY TERMINAL", 5, 24);
        _spr_top.setTextColor(0x00FF00U, COLOR_BG); snprintf(buf, sizeof(buf), "SNR:%2.0fdB", snr_db); _spr_top.drawString(buf, 140, 24);
        
        _spr_top.setTextDatum(middle_right); _spr_top.setTextColor(0x00FFFFU, COLOR_BG); 
        snprintf(buf, sizeof(buf), "B:%d F:%lu C0:%.0f%% C1:%.0f%%", BUILD_NUMBER, fps, load_c0, load_c1); 
        _spr_top.drawString(buf, 475, 24);
        
        ili9488_push_colors(0, UI_Y_TOP, 480, UI_TOP_BAR_H, (uint16_t*)_spr_top.getBuffer());
    }

    void drawInfo(float adc_v) {
        _spr_text.fillSprite(COLOR_BG); 
        int sq_w = 40, sq_h = 30, start_x = 10, start_y = 10;
        _spr_text.fillRect(start_x, start_y, sq_w, sq_h, 0x0000FFU); // RED
        _spr_text.fillRect(start_x+45, start_y, sq_w, sq_h, 0x00FF00U); // GRN
        _spr_text.fillRect(start_x+90, start_y, sq_w, sq_h, 0xFF0000U); // BLU

        // Zero Bias Meter
        int meter_w = 200, meter_x = 240, meter_y = 10; 
        _spr_text.setTextDatum(middle_center); _spr_text.setTextColor(0xFFFFFFU, COLOR_BG); 
        _spr_text.drawString("ZERO BIAS TRIMMER", meter_x, meter_y + 30); 
        
        _spr_text.drawFastHLine(meter_x - 100, meter_y+6, meter_w, COLOR_GRID); 
        _spr_text.drawFastVLine(meter_x, meter_y, 12, COLOR_TEXT); 
        
        float err = adc_v - 1.65f; 
        int nx = meter_x + (int)(std::clamp(err/0.5f, -1.0f, 1.0f) * 100.0f);
        uint32_t nc = (abs(err) < 0.05f) ? 0x00FF00U : 0x0000FFU; 
        _spr_text.fillTriangle(nx, meter_y+6, nx-5, meter_y, nx+5, meter_y, nc); 

        ili9488_push_colors(0, UI_Y_TEXT, 480, UI_TEXT_ZONE_H, (uint16_t*)_spr_text.getBuffer());
    }
};
#endif