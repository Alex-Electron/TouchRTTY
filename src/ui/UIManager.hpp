#ifndef UI_MANAGER_HPP
#define UI_MANAGER_HPP

#include <LovyanGFX.hpp>
#include "../display/ili9488_driver.h"
#include "../version.h"
#include <string>
#include <vector>

// Hardcoded Palette (Yellow on Blue - R and B channels swapped for Mode 11 quirk)        
static constexpr uint32_t PAL_BG = 0x330000U; // Dark Blue
static constexpr uint32_t PAL_GRID = 0x663300U; // Blue-Grey
static constexpr uint32_t PAL_WAVE = 0x00FFFFU; // Yellow
static constexpr uint32_t PAL_PEAK = 0x00FFFFU; // Yellow
static constexpr uint32_t PAL_TEXT = 0xFFFFFFU; // White

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

extern volatile bool shared_serial_diag;
extern volatile int  shared_line_width;

class UIManager {
private:
    lgfx::LGFX_Device* _tft;
    LGFX_Sprite _spr_top;
    LGFX_Sprite _spr_text;
    LGFX_Sprite _spr_bottom;
    static constexpr uint32_t COLOR_BG = 0x000000U;
    static constexpr uint32_t COLOR_GRID = 0x444444U;
    static constexpr uint32_t COLOR_TEXT = 0xFFFFFFU;

    std::vector<std::string> rtty_lines;
    int scroll_offset = 0;
    bool figures_mode = false;

public:
    UIManager(lgfx::LGFX_Device* tft) : _tft(tft), _spr_top(tft), _spr_text(tft), _spr_bottom(tft) {}

    void init() {
        _spr_top.setColorDepth(16); _spr_top.createSprite(480, UI_TOP_BAR_H);
        _spr_text.setColorDepth(16); _spr_text.createSprite(480, UI_TEXT_ZONE_H);
        _spr_bottom.setColorDepth(16); _spr_bottom.createSprite(480, UI_BOTTOM_BAR_H);    
        _tft->fillScreen(COLOR_BG);
        clearRTTY();
    }

    void addRTTYChar(char c, bool update_screen = true) {
        if (rtty_lines.empty()) rtty_lines.push_back("");

        static char last_c = 0;
        bool is_nl = (c == '\n');
        bool is_cr = (c == '\r');

        if (!shared_serial_diag && (c < 32 || c > 126) && !is_nl && !is_cr) {
            printf("[0x%02X]", (uint8_t)c);
        }

        if (is_cr) {
            if (last_c != '\r') {
                rtty_lines.push_back("");
                if (scroll_offset > 0) scroll_offset++;
            }
        } else if (is_nl) {
            if (last_c != '\r') {
                rtty_lines.push_back("");
                if (scroll_offset > 0) scroll_offset++;
            }
        } else {
            rtty_lines.back() += c;
            if ((int)rtty_lines.back().length() >= shared_line_width) {
                rtty_lines.push_back("");
                if (scroll_offset > 0) scroll_offset++;
            }
        }
        last_c = c;

        if (rtty_lines.size() > 200) {
            rtty_lines.erase(rtty_lines.begin());
            if (scroll_offset > 0) scroll_offset--;
        }
        if (update_screen && scroll_offset == 0) drawRTTY();
    }

    void scrollRTTY(int dir) {
        scroll_offset += dir;
        int max_lines_on_screen = 160 / 18;
        int max_scroll = (int)rtty_lines.size() - max_lines_on_screen;
        if (max_scroll < 0) max_scroll = 0;
        if (scroll_offset > max_scroll) scroll_offset = max_scroll;
        if (scroll_offset < 0) scroll_offset = 0;
        drawRTTY();
    }

    void scrollToY(int y, int track_h) {
        int max_lines = 160 / 18;
        int total_lines = (int)rtty_lines.size();
        if (total_lines <= max_lines) return;
        int max_scroll = total_lines - max_lines;
        float pct = (float)y / track_h;
        if (pct < 0.0f) pct = 0.0f;
        if (pct > 1.0f) pct = 1.0f;
        int target_pos = (int)(pct * max_scroll);
        scroll_offset = max_scroll - target_pos;
        drawRTTY();
    }

    void drawResetConfirm() {
        _spr_text.fillSprite(COLOR_BG);
        _spr_text.drawFastHLine(0, 0, 480, COLOR_GRID);
        _spr_text.setFont(&fonts::Font2); _spr_text.setTextDatum(middle_center);
        _spr_text.setTextColor(0xFFFFFFU);
        _spr_text.drawString("FACTORY RESET?", 240, 40);
        _spr_text.drawString("All settings will be lost.", 240, 60);
        _spr_text.fillRoundRect(80, 100, 120, 40, 6, 0x333333U);
        _spr_text.drawRoundRect(80, 100, 120, 40, 6, 0x777777U);
        _spr_text.drawString("NO", 140, 120);
        _spr_text.fillRoundRect(280, 100, 120, 40, 6, 0x660000U);
        _spr_text.drawRoundRect(280, 100, 120, 40, 6, 0xFF0000U);
        _spr_text.drawString("YES", 340, 120);
        ili9488_push_colors(0, UI_Y_TEXT, 480, UI_TEXT_ZONE_H, (uint16_t*)_spr_text.getBuffer());
    }

    void clearRTTY() {
        rtty_lines.clear();
        scroll_offset = 0;
        drawRTTY();
    }

    void drawRTTY() {
        _spr_text.fillSprite(COLOR_BG);
        _spr_text.drawFastHLine(0, 0, 480, COLOR_GRID);
        _spr_text.setTextColor(0x00FF00U, COLOR_BG); 
        _spr_text.setFont(&fonts::Font2);
        _spr_text.setTextDatum(top_left);
        int max_lines_on_screen = 160 / 18; 
        int start_line = (int)rtty_lines.size() - max_lines_on_screen - scroll_offset;
        if (start_line < 0) start_line = 0;
        int y = 5;
        for (size_t i = start_line; i < rtty_lines.size() && y < 155; i++) {
            _spr_text.drawString(rtty_lines[i].c_str(), 5, y);
            y += 18;
        }
        _spr_text.fillRect(440, 0, 40, 160, 0x111111U); 
        _spr_text.drawRect(440, 0, 40, 30, COLOR_GRID);
        _spr_text.fillTriangle(460, 5, 450, 20, 470, 20, COLOR_TEXT);
        _spr_text.drawRect(440, 130, 40, 30, COLOR_GRID);
        _spr_text.fillTriangle(460, 155, 450, 140, 470, 140, COLOR_TEXT);
        if ((int)rtty_lines.size() > max_lines_on_screen) {
            int track_h = 100;
            int thumb_h = (max_lines_on_screen * track_h) / rtty_lines.size();
            if (thumb_h < 15) thumb_h = 15;
            int max_scroll = (int)rtty_lines.size() - max_lines_on_screen;
            int scroll_pos = max_scroll - scroll_offset;
            int thumb_y = 30 + (scroll_pos * (track_h - thumb_h)) / max_scroll;
            _spr_text.fillRect(445, thumb_y, 30, thumb_h, 0x777777U);
        }
        ili9488_push_colors(0, UI_Y_TEXT, 480, UI_TEXT_ZONE_H, (uint16_t*)_spr_text.getBuffer());
    }

    void drawMenu(bool auto_scale, bool exp_scale, int display_mode, float filter_k, float sq_snr, const char* diag_label, const char* save_text) {
        _spr_text.fillSprite(COLOR_BG);
        _spr_text.drawFastHLine(0, 0, 480, COLOR_GRID);
        int bw = 480 / 4; int bh = 160 / 3; 
        char buf_fl[16], buf_sq[16];
        snprintf(buf_fl, 16, "BW: %.2f", filter_k);
        snprintf(buf_sq, 16, "SQ: %.0f", sq_snr);
        const char* labels[12] = {
            display_mode == 0 ? "DISP: WF" : (display_mode == 1 ? "DISP: SPEC" : "DISP: SCOPE"),
            exp_scale ? "SCALE: EXP" : "SCALE: LIN",
            auto_scale ? "AUTO: ON" : "AUTO: OFF",
            diag_label,
            "BW -", buf_fl, "BW +", save_text,
            "SQ -", buf_sq, "SQ +", "RESET"
        };
        _spr_text.setFont(&fonts::Font2); _spr_text.setTextDatum(middle_center);
        for (int i = 0; i < 12; i++) {
            int x = (i % 4) * bw; int y = (i / 4) * bh;
            uint32_t bg = 0x333333U, brd = 0x777777U;
            if (i == 2 && auto_scale) { bg = 0x006600U; brd = 0x00FF00U; }
            if (i == 3) { bg = 0x442200U; brd = 0xAA5500U; } 
            if (i == 5 || i == 9) { bg = 0x111111U; brd = 0x333333U; } 
            if (i == 7) { bg = 0x000066U; brd = 0x0000FFU; }
            _spr_text.fillRoundRect(x + 4, y + 4, bw - 8, bh - 8, 6, bg);
            _spr_text.drawRoundRect(x + 4, y + 4, bw - 8, bh - 8, 6, brd);
            if (i == 5 || i == 9) _spr_text.setTextColor(0x00FFFFU); else _spr_text.setTextColor(0xFFFFFFU);
            _spr_text.drawString(labels[i], x + (bw / 2), y + (bh / 2));
        }
        ili9488_push_colors(0, UI_Y_TEXT, 480, UI_TEXT_ZONE_H, (uint16_t*)_spr_text.getBuffer());
    }

    void drawBottomBar(int baud_idx, int shift_idx, float stop_bits, bool inv, bool afc_on, bool menu_mode) {
        _spr_bottom.fillSprite(COLOR_BG);
        const int bauds[] = {45, 50, 75};
        const int shifts[] = {170, 200, 425, 450, 850};
        char labels_main[7][16];
        snprintf(labels_main[0], 16, "B %d", bauds[baud_idx]);
        snprintf(labels_main[1], 16, "S %d", shifts[shift_idx]);
        snprintf(labels_main[2], 16, inv ? "INV" : "NORM");
        snprintf(labels_main[3], 16, afc_on ? "AFC:ON" : "AFC:OFF");
        snprintf(labels_main[4], 16, "ST %.1f", stop_bits);
        snprintf(labels_main[5], 16, "CLEAR");
        snprintf(labels_main[6], 16, "MENU");
        int btn_w = 480 / 7; _spr_bottom.setFont(&fonts::Font2); _spr_bottom.setTextDatum(middle_center);
        for (int i = 0; i < 7; i++) {
            int x = i * btn_w;
            uint32_t bg = 0x333333U, brd = 0x777777U;
            if (i == 2 && inv) { bg = 0x660000U; brd = 0xFF0000U; }
            if (i == 3 && afc_on) { bg = 0x004400U; brd = 0x00FF00U; }
            if (i == 6 && menu_mode) { bg = 0x006600U; brd = 0x00FF00U; }
            _spr_bottom.fillRoundRect(x + 2, 2, btn_w - 4, UI_BOTTOM_BAR_H - 4, 6, bg);   
            _spr_bottom.drawRoundRect(x + 2, 2, btn_w - 4, UI_BOTTOM_BAR_H - 4, 6, brd);  
            _spr_bottom.setTextColor(0xFFFFFFU);
            _spr_bottom.drawString(labels_main[i], x + (btn_w / 2), (UI_BOTTOM_BAR_H / 2));
        }
        ili9488_push_colors(0, UI_Y_BOTTOM, 480, 48, (uint16_t*)_spr_bottom.getBuffer()); 
    }

    void updateTopBar(float adc_v, uint32_t fps, float signal_db, float snr_db, float m_freq, float s_freq, bool clipping, float load0, float load1, bool squelch_open, float agc_gain, bool agc_enabled) {
        _spr_top.fillSprite(COLOR_BG); _spr_top.drawFastHLine(0, 33, 480, COLOR_GRID);    
        _spr_top.setTextDatum(middle_left); _spr_top.setTextColor(COLOR_TEXT, COLOR_BG); _spr_top.setFont(&fonts::Font2);
        _spr_top.drawString("SIG", 5, 8); _spr_top.drawRect(35, 1, 95, 14, COLOR_GRID);   
        int lw = (int)((signal_db+80)*(95/70.0f)); if(lw<0) lw=0; if(lw>95) lw=95;        
        uint32_t sig_color = 0x00FF00U; if (clipping) sig_color = 0x0000FFU; else if (signal_db > -30.0f) sig_color = 0xFF0000U;
        _spr_top.fillRect(35, 1, lw, 14, sig_color);
        _spr_top.drawString("AGC", 5, 24); _spr_top.drawRect(35, 17, 95, 14, COLOR_GRID);   
        if (agc_enabled) {
            float gain_db = 20.0f * log10f(agc_gain + 1e-5f);
            int gw = (int)((gain_db)*(95/46.0f)); if(gw<0) gw=0; if(gw>95) gw=95;
            _spr_top.fillRect(35, 17, gw, 14, 0x00FFFFU);
        } else {
            _spr_top.setTextColor(0x777777U, COLOR_BG); _spr_top.drawString("OFF", 40, 24); _spr_top.setTextColor(COLOR_TEXT, COLOR_BG);
        }
        char buf[64];
        _spr_top.setTextColor(0x00FFFFU, COLOR_BG); 
        snprintf(buf, sizeof(buf), "%3.0fdB", signal_db); _spr_top.drawString(buf, 135, 8);
        snprintf(buf, sizeof(buf), "M:%.0f S:%.0f", m_freq, s_freq); _spr_top.drawString(buf, 185, 8);
        if (squelch_open) { _spr_top.setTextColor(0x00FF00U, COLOR_BG); _spr_top.drawString("RTTY: SYNC", 200, 24); }
        else { _spr_top.setTextColor(0x777777U, COLOR_BG); _spr_top.drawString("RTTY: WAIT", 200, 24); }
        _spr_top.setTextColor(0x00FF00U, COLOR_BG); snprintf(buf, sizeof(buf), "SNR:%2.0fdB", snr_db); _spr_top.drawString(buf, 135, 24);
        _spr_top.setTextDatum(middle_right); _spr_top.setTextColor(0x00FFFFU, COLOR_BG);  
        snprintf(buf, sizeof(buf), "B:%d F:%lu C0:%.0f%% C1:%.0f%%", BUILD_NUMBER, fps, load0, load1);
        _spr_top.drawString(buf, 475, 24);
        ili9488_push_colors(0, UI_Y_TOP, 480, UI_TOP_BAR_H, (uint16_t*)_spr_top.getBuffer());
    }

    void drawDiagScreen(float adc_v, bool serial_diag_on, int line_width) {
        _spr_text.fillSprite(COLOR_BG);
        _spr_text.drawFastHLine(0, 111, 480, COLOR_GRID);
        _spr_text.setTextDatum(top_left); _spr_text.setFont(&fonts::Font2);
        _spr_text.setTextColor(0x00FF00U, COLOR_BG);
        _spr_text.drawString("DIAGNOSTICS & SETUP", 5, 5);
        _spr_text.setTextColor(0xFFFFFFU, COLOR_BG);
        _spr_text.drawString("Hardware: RP2350, 16-bit DMA", 5, 25);
        
        int meter_w = 100, meter_x = 110, meter_y = 70;
        _spr_text.setTextDatum(middle_right); _spr_text.setTextColor(0xFFFFFFU, COLOR_BG);
        _spr_text.drawString("ZERO BIAS", meter_x - 5, meter_y + 6);
        _spr_text.drawFastHLine(meter_x, meter_y+6, meter_w, COLOR_GRID);
        _spr_text.drawFastVLine(meter_x+(meter_w/2), meter_y, 12, COLOR_TEXT);
        float err = adc_v - 1.65f; float norm_err = std::clamp(err / 0.5f, -1.0f, 1.0f);
        int nx = meter_x + (meter_w/2) + (int)(norm_err * (meter_w/2));
        uint32_t nc = (abs(err) < 0.05f) ? 0x00FF00U : 0x0000FFU;
        _spr_text.fillTriangle(nx, meter_y+6, nx-5, meter_y-2, nx+5, meter_y-2, nc);      
        _spr_text.fillTriangle(nx, meter_y+6, nx-5, meter_y+14, nx+5, meter_y+14, nc);    

        _spr_text.setTextDatum(middle_center);
        
        uint32_t s_bg = serial_diag_on ? 0x004400U : 0x440000U;
        uint32_t s_brd = serial_diag_on ? 0x00FF00U : 0xFF0000U;
        _spr_text.fillRoundRect(5, 118, 150, 36, 6, s_bg);
        _spr_text.drawRoundRect(5, 118, 150, 36, 6, s_brd);
        _spr_text.drawString(serial_diag_on ? "DIAG: ON" : "DIAG: OFF", 80, 136);

        _spr_text.fillRoundRect(165, 118, 100, 36, 6, 0x333333U);
        _spr_text.drawRoundRect(165, 118, 100, 36, 6, 0x777777U);
        _spr_text.drawString("WIDTH -", 215, 136);

        _spr_text.fillRoundRect(275, 118, 100, 36, 6, 0x333333U);
        _spr_text.drawRoundRect(275, 118, 100, 36, 6, 0x777777U);
        _spr_text.drawString("WIDTH +", 325, 136);

        char wbuf[16]; snprintf(wbuf, 16, "W:%d", line_width);
        _spr_text.setTextColor(0x00FFFFU);
        _spr_text.drawString(wbuf, 425, 136);

        ili9488_push_colors(0, UI_Y_TEXT, 480, UI_TEXT_ZONE_H, (uint16_t*)_spr_text.getBuffer());
    }
};
#endif
