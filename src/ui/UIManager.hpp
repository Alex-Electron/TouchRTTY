#ifndef UI_MANAGER_HPP
#define UI_MANAGER_HPP

#include <LovyanGFX.hpp>
#include "../display/ili9341_test.h" // For DMA push

// UI Geometry Constants for 480x320 Display
#define UI_TOP_BAR_H   48
#define UI_DSP_ZONE_H  112
#define UI_TEXT_ZONE_H 112
#define UI_BOTTOM_BAR_H 48

// Y-coordinate boundaries
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

    // Flat Design Colors (RGB565)
    static constexpr uint32_t COLOR_BG       = 0x0000U; // Black
    static constexpr uint32_t COLOR_GRID     = 0x4208U; // Dark Gray for dividers
    static constexpr uint32_t COLOR_FREQ     = 0xFD20U; // Amber/Orange
    static constexpr uint32_t COLOR_TEXT     = 0xFFFFU; // White
    static constexpr uint32_t COLOR_BTN_LINE = 0x07FFU; // Cyan
    static constexpr uint32_t COLOR_GOOD     = 0x07E0U; // Green for SNR

public:
    UIManager(lgfx::LGFX_Device* tft) : _tft(tft), _spr_top(tft), _spr_text(tft), _spr_bottom(tft) {}

    void init() {
        // Allocate 16-bit sprites in RAM
        _spr_top.setColorDepth(16);
        _spr_top.createSprite(480, UI_TOP_BAR_H);
        
        _spr_text.setColorDepth(16);
        _spr_text.createSprite(480, UI_TEXT_ZONE_H);

        _spr_bottom.setColorDepth(16);
        _spr_bottom.createSprite(480, UI_BOTTOM_BAR_H);

        // Draw initial static screens
        _tft->fillScreen(COLOR_BG);
        drawBottomBar();
        drawTextZonePlaceholder();
    }

    void updateTopBar(float adc_v, uint32_t fps) {
        _spr_top.fillSprite(COLOR_BG);
        _spr_top.drawFastHLine(0, UI_TOP_BAR_H - 1, 480, COLOR_GRID); // Grid divider

        // Status Indicators (Standard Font)
        _spr_top.setFont(&fonts::Font2); // 8x16 pixels font
        
        _spr_top.setTextColor(COLOR_TEXT, COLOR_BG);
        _spr_top.drawString("RTTY 45/170", 10, 8);
        
        _spr_top.setTextColor(COLOR_GOOD, COLOR_BG);
        _spr_top.drawString("SNR: -- dB", 10, 26);

        // System Diagnostic
        _spr_top.setTextColor(COLOR_BTN_LINE, COLOR_BG);
        _spr_top.setTextDatum(top_right);
        char buf[32];
        snprintf(buf, sizeof(buf), "ADC: %.2fV", adc_v);
        _spr_top.drawString(buf, 470, 8);
        snprintf(buf, sizeof(buf), "FPS: %lu", fps);
        _spr_top.drawString(buf, 470, 26);

        // Instantly blast to screen using DMA
        ili9488_push_colors(0, UI_Y_TOP, 480, UI_TOP_BAR_H, (uint16_t*)_spr_top.getBuffer());
    }

    void drawTextZonePlaceholder() {
        _spr_text.fillSprite(COLOR_BG);
        _spr_text.drawFastHLine(0, UI_TEXT_ZONE_H - 1, 480, COLOR_GRID); // Grid divider

        _spr_text.setTextDatum(top_left);
        _spr_text.setFont(&fonts::Font2); // 8x16 monospace terminal font
        _spr_text.setTextColor(COLOR_TEXT, COLOR_BG);
        
        int start_y = 2;
        
        _spr_text.drawString("WAITING FOR SYNC...", 5, start_y);

        ili9488_push_colors(0, UI_Y_TEXT, 480, UI_TEXT_ZONE_H, (uint16_t*)_spr_text.getBuffer());
    }

private:
    void drawBottomBar() {
        _spr_bottom.fillSprite(COLOR_BG);
        _spr_bottom.drawFastHLine(0, 0, 480, COLOR_GRID); // Grid divider
        
        const char* labels[5] = {"TUNE", "AFC", "SPEED", "CLEAR", "MENU"};
        int btn_w = 480 / 5; // Exactly 96px width per button
        int btn_h = UI_BOTTOM_BAR_H; // 48px height
        
        _spr_bottom.setFont(&fonts::Font2); 
        _spr_bottom.setTextDatum(middle_center);
        
        for (int i = 0; i < 5; i++) {
            int x = i * btn_w;
            
            // Draw 2px padded chunky button borders for stylus accuracy
            _spr_bottom.drawRect(x + 2, 2, btn_w - 4, btn_h - 4, COLOR_BTN_LINE);
            
            // Center text inside the button
            _spr_bottom.setTextColor(COLOR_FREQ);
            _spr_bottom.drawString(labels[i], x + (btn_w / 2), (btn_h / 2));
        }

        ili9488_push_colors(0, UI_Y_BOTTOM, 480, UI_BOTTOM_BAR_H, (uint16_t*)_spr_bottom.getBuffer());
    }
};

#endif