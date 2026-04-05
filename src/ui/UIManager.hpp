#ifndef UI_MANAGER_HPP
#define UI_MANAGER_HPP

#include <LovyanGFX.hpp>
#include "../display/ili9488_driver.h"
#include "../version.h"
#include "../fonts/Spleen8x16.h"
#include "../fonts/Spleen5x8.h"
#include "../fonts/Bitocra7x13.h"
#include <string>
#include <vector>

// Eye diagram shared data (defined in main.cpp)
#define EYE_TRACES 16
#define EYE_MAX_SPB 256
extern volatile int8_t shared_eye_buf[EYE_TRACES][EYE_MAX_SPB];
extern volatile int shared_eye_spb;
extern volatile int shared_eye_idx;
extern volatile bool shared_eye_ready;
extern volatile float tuning_dpll_alpha;
extern volatile float tuning_lpf_k;
extern volatile float tuning_sq_snr;
extern volatile float shared_err_rate;

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
extern volatile int  shared_font_mode;

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
        if (update_screen && scroll_offset == 0) {
            // Throttle redraws for small font — full redraw is expensive with 16 lines
            static uint32_t last_redraw_us = 0;
            uint32_t now_us = time_us_32();
            bool force = is_nl || is_cr; // Always redraw on newline
            if (force || (now_us - last_redraw_us >= 80000)) {
                drawRTTY();
                last_redraw_us = now_us;
            }
        }
    }

    int getLineHeight() {
        switch (shared_font_mode) {
            case 0: return 17; // BIG
            case 1: return 14; // MED
            case 2: return 10; // SMALL
            default: return 9; // TINY: Spleen 5x8
        }
    }

    void scrollRTTY(int dir) {
        scroll_offset += dir;
        int line_h = getLineHeight();
        int max_lines_on_screen = 160 / line_h;
        int max_scroll = (int)rtty_lines.size() - max_lines_on_screen;
        if (max_scroll < 0) max_scroll = 0;
        if (scroll_offset > max_scroll) scroll_offset = max_scroll;
        if (scroll_offset < 0) scroll_offset = 0;
        drawRTTY();
    }

    void scrollToY(int y, int track_h) {
        int line_h = getLineHeight();
        int max_lines = 160 / line_h;
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
        _spr_text.setTextColor(0xFFFFFFU, COLOR_BG);
        _spr_text.drawString("FACTORY RESET?", 240, 40);
        _spr_text.drawString("All settings will be lost.", 240, 60);
        _spr_text.fillRoundRect(80, 100, 120, 40, 6, 0x333333U);
        _spr_text.drawRoundRect(80, 100, 120, 40, 6, 0x777777U);
        _spr_text.setTextColor(0xFFFFFFU, 0x333333U); _spr_text.drawString("NO", 140, 120);
        _spr_text.fillRoundRect(280, 100, 120, 40, 6, 0x660000U);
        _spr_text.drawRoundRect(280, 100, 120, 40, 6, 0xFF0000U);
        _spr_text.setTextColor(0xFFFFFFU, 0x660000U); _spr_text.drawString("YES", 340, 120);
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
        
        int line_h = 17;
        _spr_text.setTextSize(1.0, 1.0);
        switch (shared_font_mode) {
            case 0: // BIG: Spleen 8x16
                _spr_text.setFont((const lgfx::GFXfont*)&Spleen8x16);
                line_h = 17;
                break;
            case 1: // MED: Bitocra 7x13
                _spr_text.setFont((const lgfx::GFXfont*)&Bitocra7x13);
                line_h = 14;
                break;
            case 2: // SMALL: Font0 6x8
                _spr_text.setFont(&fonts::Font0);
                line_h = 10;
                break;
            default: // TINY: Spleen 5x8
                _spr_text.setFont((const lgfx::GFXfont*)&Spleen5x8);
                line_h = 9;
                break;
        }
        
        _spr_text.setTextDatum(top_left);
        int y_start = 3;
        int y_limit = 158; // keep 2px margin at bottom
        int max_lines_on_screen = (y_limit - y_start) / line_h;
        int start_line = (int)rtty_lines.size() - max_lines_on_screen - scroll_offset;
        if (start_line < 0) start_line = 0;

        int y = y_start;
        for (size_t i = start_line; i < rtty_lines.size() && y + line_h <= 160; i++) {
            _spr_text.drawString(rtty_lines[i].c_str(), 5, y);
            y += line_h;
        }
        _spr_text.setTextSize(1.0, 1.0); // Reset
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

    void drawMenu(bool auto_scale, int display_mode, const char* diag_label) {
        _spr_text.fillSprite(COLOR_BG);
        _spr_text.drawFastHLine(0, 0, 480, COLOR_GRID);
        int bw = 480 / 4; int bh = 160 / 3;
        const char* labels[12] = {
            display_mode == 0 ? "DISP: WF" : (display_mode == 1 ? "DISP: SPEC" : "DISP: SCOPE"),
            auto_scale ? "AUTO: ON" : "AUTO: OFF",
            diag_label,
            "TUNE",
            "", "", "", "",
            "", "", "", ""
        };
        _spr_text.setFont(&fonts::Font2); _spr_text.setTextDatum(middle_center);
        for (int i = 0; i < 12; i++) {
            if (strlen(labels[i]) == 0) continue;
            int x = (i % 4) * bw; int y = (i / 4) * bh;
            uint32_t bg = 0x333333U, brd = 0x777777U;
            if (i == 2 && auto_scale) { bg = 0x006600U; brd = 0x00FF00U; }
            if (i == 3) { bg = 0x442200U; brd = 0xAA5500U; }
            if (i == 11) { bg = 0x004400U; brd = 0x00FF00U; } // TUNE
            _spr_text.fillRoundRect(x + 4, y + 4, bw - 8, bh - 8, 6, bg);
            _spr_text.drawRoundRect(x + 4, y + 4, bw - 8, bh - 8, 6, brd);
            _spr_text.setTextColor(0xFFFFFFU, bg);
            _spr_text.drawString(labels[i], x + (bw / 2), y + (bh / 2));
        }
        ili9488_push_colors(0, UI_Y_TEXT, 480, UI_TEXT_ZONE_H, (uint16_t*)_spr_text.getBuffer());
    }

    void drawBottomBar(int baud_idx, int shift_idx, float stop_bits, bool afc_on, bool menu_mode, int search_state, bool stop_auto = false) {
        _spr_bottom.fillSprite(COLOR_BG);
        const int bauds[] = {45, 50, 75};
        const int shifts[] = {85, 170, 200, 340, 425, 450, 500, 850};
        char labels_main[7][16];
        snprintf(labels_main[0], 16, "B %d", bauds[baud_idx]);
        if (shift_idx < 8) snprintf(labels_main[1], 16, "S %d", shifts[shift_idx]);
        else snprintf(labels_main[1], 16, "S:AUTO");
        if (search_state == 1) snprintf(labels_main[2], 16, "SRCH..");
        else if (search_state == 2) snprintf(labels_main[2], 16, "FOUND!");
        else if (search_state == 3) snprintf(labels_main[2], 16, "NONE");
        else snprintf(labels_main[2], 16, "SEARCH");
        snprintf(labels_main[3], 16, afc_on ? "AFC:ON" : "AFC:OFF");
        if (stop_auto) snprintf(labels_main[4], 16, "ST:AUTO");
        else snprintf(labels_main[4], 16, "ST %.1f", stop_bits);
        snprintf(labels_main[5], 16, "CLEAR");
        snprintf(labels_main[6], 16, "MENU");
        int btn_w = 480 / 7; _spr_bottom.setFont(&fonts::Font2); _spr_bottom.setTextDatum(middle_center);
        for (int i = 0; i < 7; i++) {
            int x = i * btn_w;
            uint32_t bg = 0x333333U, brd = 0x777777U;
            if (i == 2 && search_state == 1) { bg = 0x444400U; brd = 0xFFFF00U; }
            if (i == 2 && search_state == 2) { bg = 0x004400U; brd = 0x00FF00U; }
            if (i == 2 && search_state == 3) { bg = 0x660000U; brd = 0xFF0000U; }
            if (i == 3 && afc_on) { bg = 0x004400U; brd = 0x00FF00U; }
            if (i == 6 && menu_mode) { bg = 0x006600U; brd = 0x00FF00U; }
            _spr_bottom.fillRoundRect(x + 2, 2, btn_w - 4, UI_BOTTOM_BAR_H - 4, 6, bg);
            _spr_bottom.drawRoundRect(x + 2, 2, btn_w - 4, UI_BOTTOM_BAR_H - 4, 6, brd);
            _spr_bottom.setTextColor(0xFFFFFFU, bg);
            _spr_bottom.drawString(labels_main[i], x + (btn_w / 2), (UI_BOTTOM_BAR_H / 2));
        }
        ili9488_push_colors(0, UI_Y_BOTTOM, 480, 48, (uint16_t*)_spr_bottom.getBuffer());
    }

    void updateTopBar(float adc_v, uint32_t fps, float signal_db, float snr_db, float m_freq, float s_freq, bool clipping, float load0, float load1, bool squelch_open, float agc_gain, bool agc_enabled, float err_rate, bool rtty_inv, int shift_idx, float active_shift, bool inv_uncertain, bool stop_auto = false, float active_stop = 1.5f, int stop_detect_state = 0) {
        _spr_top.fillSprite(COLOR_BG); _spr_top.drawFastHLine(0, 33, 480, COLOR_GRID);
        _spr_top.setFont(&fonts::Font0); // Compact 6x8 font for 3-row layout
        char buf[64];

        // --- Row 1 (y=0..9): SIG bar + signal info ---
        const int bar_x = 28, bar_w = 82, bar_h = 9;
        _spr_top.setTextDatum(middle_left); _spr_top.setTextColor(0xAAAAAAU, COLOR_BG);
        _spr_top.drawString("SIG", 2, 5);
        _spr_top.drawRect(bar_x, 0, bar_w, bar_h, COLOR_GRID);
        int lw = (int)((signal_db+80)*(bar_w/70.0f)); if(lw<0) lw=0; if(lw>bar_w) lw=bar_w;
        uint32_t sig_color = 0x00FF00U; if (clipping) sig_color = 0x0000FFU; else if (signal_db > -30.0f) sig_color = 0xFF0000U;
        _spr_top.fillRect(bar_x, 0, lw, bar_h, sig_color);
        _spr_top.setTextColor(0x00FFFFU, COLOR_BG);
        snprintf(buf, sizeof(buf), "%3.0fdB", signal_db); _spr_top.drawString(buf, bar_x+bar_w+4, 5);
        snprintf(buf, sizeof(buf), "M:%.0f S:%.0f", m_freq, s_freq); _spr_top.drawString(buf, 170, 5);
        if (squelch_open) { _spr_top.setTextColor(0x00FF00U, COLOR_BG); _spr_top.drawString("SYNC", 280, 5); }
        else { _spr_top.setTextColor(0x777777U, COLOR_BG); _spr_top.drawString("WAIT", 280, 5); }
        // INV/NOR indicator next to SYNC (with ? for uncertain)
        if (inv_uncertain) {
            _spr_top.setTextColor(0x00FFFFU, COLOR_BG);
            _spr_top.drawString(rtty_inv ? "INV?" : "NOR?", 308, 5);
        } else if (rtty_inv) {
            _spr_top.setTextColor(0x0000FFU, COLOR_BG); _spr_top.drawString("INV", 308, 5);
        } else {
            _spr_top.setTextColor(0x888888U, COLOR_BG); _spr_top.drawString("NOR", 308, 5);
        }
        _spr_top.setTextColor(0x00FF00U, COLOR_BG);
        snprintf(buf, sizeof(buf), "SNR:%2.0f", snr_db); _spr_top.drawString(buf, 340, 5);

        // --- Row 2 (y=11..20): AGC bar + dB value + info ---
        _spr_top.setTextColor(0xAAAAAAU, COLOR_BG);
        _spr_top.drawString("AGC", 2, 16);
        _spr_top.drawRect(bar_x, 11, bar_w, bar_h, COLOR_GRID);
        float gain_db = 20.0f * log10f(agc_gain + 1e-5f);
        if (agc_enabled) {
            int gw = (int)((gain_db)*(bar_w/46.0f)); if(gw<0) gw=0; if(gw>bar_w) gw=bar_w;
            _spr_top.fillRect(bar_x, 11, gw, bar_h, 0x00FFFFU);
        }
        _spr_top.setTextColor(0x00FFFFU, COLOR_BG);
        snprintf(buf, sizeof(buf), "%+.0fdB", gain_db); _spr_top.drawString(buf, bar_x+bar_w+4, 16);
        // Shift indicator on Row 2
        if (shift_idx >= 8) { // AUTO mode
            _spr_top.setTextColor(0x00FF00U, COLOR_BG);
            snprintf(buf, sizeof(buf), "SH:%.0f(A)", active_shift);
        } else {
            _spr_top.setTextColor(0x00FFFFU, COLOR_BG);
            snprintf(buf, sizeof(buf), "SH:%.0f", active_shift);
        }
        _spr_top.drawString(buf, 170, 16);
        // Stop-bit indicator on Row 2 after SH
        if (stop_auto) {
            if (stop_detect_state == 1) {
                _spr_top.setTextColor(0x00FFFFU, COLOR_BG); // yellow = detecting
                _spr_top.drawString("ST:..", 230, 16);
            } else {
                _spr_top.setTextColor(0x00FF00U, COLOR_BG); // green = auto detected
                snprintf(buf, sizeof(buf), "ST:%.1f(A)", active_stop);
                _spr_top.drawString(buf, 230, 16);
            }
        } else {
            _spr_top.setTextColor(0x00FFFFU, COLOR_BG); // cyan = manual
            snprintf(buf, sizeof(buf), "ST:%.1f", active_stop);
            _spr_top.drawString(buf, 230, 16);
        }

        // --- Row 3 (y=22..31): ERR bar + % value + info ---
        _spr_top.setTextColor(0xAAAAAAU, COLOR_BG);
        _spr_top.drawString("ERR", 2, 27);
        _spr_top.drawRect(bar_x, 22, bar_w, bar_h, COLOR_GRID);
        int ew = (int)(err_rate * bar_w / 100.0f); if(ew<0) ew=0; if(ew>bar_w) ew=bar_w;
        uint32_t err_color = (err_rate < 5.0f) ? 0x00FF00U : (err_rate < 20.0f) ? 0x00FFFFU : 0x0000FFU;
        if (ew > 0) _spr_top.fillRect(bar_x, 22, ew, bar_h, err_color);
        _spr_top.setTextColor(err_color, COLOR_BG);
        snprintf(buf, sizeof(buf), "%.0f%%", err_rate); _spr_top.drawString(buf, bar_x+bar_w+4, 27);

        // Right-aligned: build/fps/load on row 2, free row 3
        _spr_top.setTextDatum(middle_right); _spr_top.setTextColor(0x00FFFFU, COLOR_BG);
        snprintf(buf, sizeof(buf), "B:%d F:%lu C0:%.0f%% C1:%.0f%%", BUILD_NUMBER, fps, load0, load1);
        _spr_top.drawString(buf, 475, 16);

        ili9488_push_colors(0, UI_Y_TOP, 480, UI_TOP_BAR_H, (uint16_t*)_spr_top.getBuffer());
    }

    void drawShiftPopup(int current_shift_idx) {
        _spr_text.fillSprite(COLOR_BG);
        _spr_text.setFont(&fonts::Font2);
        _spr_text.setTextDatum(middle_center);
        const char* labels[] = {"85", "170", "200", "340", "425", "450", "500", "850", "AUTO"};
        // Grid 3x3, each cell 160x50px, centered in 480x160
        int cols = 3, rows = 3;
        int cell_w = 160, cell_h = 50;
        int y_off = 5; // top padding
        for (int i = 0; i < 9; i++) {
            int col = i % cols, row = i / cols;
            int x = col * cell_w, y = y_off + row * cell_h;
            uint32_t bg = 0x333333U, brd = 0x777777U;
            if (i == current_shift_idx) { bg = 0x004400U; brd = 0x00FF00U; }
            if (i == 8) { bg = (current_shift_idx == 8) ? 0x004400U : 0x222244U; brd = (current_shift_idx == 8) ? 0x00FF00U : 0x6666FFU; }
            _spr_text.fillRoundRect(x + 4, y + 2, cell_w - 8, cell_h - 4, 8, bg);
            _spr_text.drawRoundRect(x + 4, y + 2, cell_w - 8, cell_h - 4, 8, brd);
            _spr_text.setTextColor(0xFFFFFFU, bg);
            _spr_text.drawString(labels[i], x + cell_w / 2, y + cell_h / 2);
        }
        ili9488_push_colors(0, UI_Y_TEXT, 480, UI_TEXT_ZONE_H, (uint16_t*)_spr_text.getBuffer());
    }

    void drawStopPopup(int current_stop_idx, bool stop_auto) {
        _spr_text.fillSprite(COLOR_BG);
        _spr_text.setFont(&fonts::Font2);
        _spr_text.setTextDatum(middle_center);
        const char* labels[] = {"1.0", "1.5", "2.0", "AUTO"};
        // Grid 2x2, each cell 240x60px, centered in 480x160 with padding
        int cols = 2, cell_w = 240, cell_h = 60;
        int y_off = 20;
        for (int i = 0; i < 4; i++) {
            int col = i % cols, row = i / cols;
            int x = col * cell_w, y = y_off + row * cell_h;
            uint32_t bg = 0x333333U, brd = 0x777777U;
            bool selected = stop_auto ? (i == 3) : (i == current_stop_idx);
            if (selected) { bg = 0x004400U; brd = 0x00FF00U; }
            if (i == 3) { bg = stop_auto ? 0x004400U : 0x222244U; brd = stop_auto ? 0x00FF00U : 0x6666FFU; }
            _spr_text.fillRoundRect(x + 6, y + 4, cell_w - 12, cell_h - 8, 8, bg);
            _spr_text.drawRoundRect(x + 6, y + 4, cell_w - 12, cell_h - 8, 8, brd);
            _spr_text.setTextColor(0xFFFFFFU, bg);
            _spr_text.drawString(labels[i], x + cell_w / 2, y + cell_h / 2);
        }
        ili9488_push_colors(0, UI_Y_TEXT, 480, UI_TEXT_ZONE_H, (uint16_t*)_spr_text.getBuffer());
    }

    void drawDiagScreen(float adc_v, int font_mode) {
        _spr_text.fillSprite(COLOR_BG);
        _spr_text.drawFastHLine(0, 111, 480, COLOR_GRID);
        _spr_text.setTextDatum(top_left); _spr_text.setFont(&fonts::Font2);
        _spr_text.setTextColor(0x00FF00U, COLOR_BG);
        _spr_text.drawString("DIAGNOSTICS & SETUP", 5, 5);
        
        // Rainbow Gradient (Anchors in BGR for LovyanGFX sprite compatibility)
        uint32_t anchors[] = {
            0x0000FFU, // Red
            0x00A5FFU, // Orange
            0x00FFFFU, // Yellow
            0x00FF00U, // Green
            0xFFFF00U, // Cyan
            0xFF0000U, // Blue
            0xFF008BU  // Purple
        };
        
        for (int x = 0; x < 440; x++) {
            float pos = (float)x / 439.0f * 6.0f;
            int idx = (int)pos;
            float t = pos - idx;
            if (idx >= 6) { idx = 5; t = 1.0f; }
            
            uint32_t c1 = anchors[idx];
            uint32_t c2 = anchors[idx + 1];
            
            // Linear interpolation for each channel (BGR order)
            int r1 = c1 & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = (c1 >> 16) & 0xFF;
            int r2 = c2 & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = (c2 >> 16) & 0xFF;
            
            uint8_t r = (uint8_t)(r1 + (r2 - r1) * t);
            uint8_t g = (uint8_t)(g1 + (g2 - g1) * t);
            uint8_t b = (uint8_t)(b1 + (b2 - b1) * t);
            
            uint32_t color = r | (g << 8) | (b << 16);
            _spr_text.drawFastVLine(5 + x, 25, 15, color);
        }

        int meter_w = 100, meter_x = 110, meter_y = 70;
        _spr_text.setTextDatum(middle_right); _spr_text.setTextColor(0xFFFFFFU, COLOR_BG);
        _spr_text.drawString("ZERO BIAS", meter_x - 5, meter_y + 6);
        _spr_text.drawFastHLine(meter_x, meter_y+6, meter_w, COLOR_GRID);
        _spr_text.drawFastVLine(meter_x+(meter_w/2), meter_y, 12, COLOR_TEXT);
        float err = adc_v - 1.65f; float norm_err = std::clamp(err / 0.5f, -1.0f, 1.0f);
        int nx = meter_x + (meter_w/2) + (int)(norm_err * (meter_w/2));
        uint32_t nc = (fabsf(err) < 0.05f) ? 0x00FF00U : 0x0000FFU;
        _spr_text.fillTriangle(nx, meter_y+6, nx-5, meter_y-2, nx+5, meter_y-2, nc);      
        _spr_text.fillTriangle(nx, meter_y+6, nx-5, meter_y+14, nx+5, meter_y+14, nc);    

        _spr_text.setTextDatum(middle_center);
        const char* font_names[] = {"BIG", "MED", "SML", "TINY"};
        const char* l_font = (font_mode >= 0 && font_mode < 4) ? font_names[font_mode] : "BIG";

        // FONT button (left)
        _spr_text.fillRoundRect(2, 118, 236, 36, 6, 0x333333U);
        _spr_text.drawRoundRect(2, 118, 236, 36, 6, 0x777777U);
        _spr_text.setTextColor(0xFFFFFFU, 0x333333U);
        char font_label[32]; snprintf(font_label, 32, "FONT: %s", l_font);
        _spr_text.drawString(font_label, 120, 136);

        // RST button (right)
        _spr_text.fillRoundRect(242, 118, 236, 36, 6, 0x000044U);
        _spr_text.drawRoundRect(242, 118, 236, 36, 6, 0x0000FFU);
        _spr_text.setTextColor(0xFFFFFFU, 0x000044U);
        _spr_text.drawString("RST", 360, 136);
        ili9488_push_colors(0, UI_Y_TEXT, 480, UI_TEXT_ZONE_H, (uint16_t*)_spr_text.getBuffer());
    }

    // ========== TUNING LAB ==========

    // Phosphor accumulation buffer for eye diagram (240 x 64)
    static constexpr int EYE_W = 240;
    static constexpr int EYE_H = 64;
    uint8_t _eye_acc[EYE_W][EYE_H] = {};
    int _eye_last_idx = -1; // track which traces we already drew

    void drawEyeDiagram(LGFX_Sprite& spr, int w, int h) {
        int spb = shared_eye_spb;
        if (spb < 2) return;
        if (w > EYE_W) w = EYE_W;
        if (h > EYE_H) h = EYE_H;

        // Fade accumulation: ~96% retention per frame (245/256)
        for (int x = 0; x < w; x++)
            for (int y = 0; y < h; y++)
                _eye_acc[x][y] = (uint8_t)((_eye_acc[x][y] * 245) >> 8);

        // Add only NEW traces since last call
        int cur_idx = shared_eye_idx;
        int traces_to_add = (cur_idx - _eye_last_idx + EYE_TRACES) % EYE_TRACES;
        if (traces_to_add == 0) traces_to_add = 1; // at least redraw newest
        if (traces_to_add > EYE_TRACES) traces_to_add = EYE_TRACES;

        for (int t = traces_to_add; t > 0; t--) {
            int tidx = (cur_idx - t + EYE_TRACES) % EYE_TRACES;
            int prev_x = -1, prev_y = -1;
            for (int s = 0; s < spb; s++) {
                int x = s * w / spb;
                int8_t dv = shared_eye_buf[tidx][s];
                int y = h/2 - (dv * (h/2 - 2)) / 127;
                if (y < 0) y = 0; if (y >= h) y = h - 1;

                if (x >= 0 && x < w) {
                    int v = _eye_acc[x][y] + 80;
                    _eye_acc[x][y] = (v > 255) ? 255 : (uint8_t)v;
                    // Fill gaps between consecutive samples for line continuity
                    if (prev_x >= 0 && prev_y != y) {
                        int dy = y - prev_y;
                        int steps = (dy > 0) ? dy : -dy;
                        for (int i = 1; i < steps; i++) {
                            int iy = prev_y + (dy * i) / steps;
                            if (iy >= 0 && iy < h) {
                                int v2 = _eye_acc[x][iy] + 40;
                                _eye_acc[x][iy] = (v2 > 255) ? 255 : (uint8_t)v2;
                            }
                        }
                    }
                }
                prev_x = x; prev_y = y;
            }
        }
        _eye_last_idx = cur_idx;

        // Render accumulation buffer to sprite as green phosphor
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint8_t v = _eye_acc[x][y];
                if (v > 2) {
                    spr.drawPixel(x, y, ((uint32_t)v << 8));
                }
            }
        }

        // Grid overlay
        spr.drawRect(0, 0, w, h, 0x333300U);
        spr.drawFastHLine(1, h/2, w - 2, 0x222200U);
        spr.drawFastVLine(w/2, 0, h, 0x222200U);
        spr.drawFastVLine(w/4, 0, h, 0x111100U);
        spr.drawFastVLine(w*3/4, 0, h, 0x111100U);

        // Labels
        spr.setFont(&fonts::Font0); spr.setTextDatum(top_left);
        spr.setTextColor(0x00FF00U, PAL_BG);
        spr.drawString("EYE", 2, 2);
    }

    void drawTuningControls(float alpha, float lpf_k, float sq_snr, float err_rate, bool saved = false) {
        _spr_text.fillSprite(COLOR_BG);
        _spr_text.drawFastHLine(0, 0, 480, COLOR_GRID);
        _spr_text.setFont(&fonts::Font2); _spr_text.setTextDatum(middle_center);

        // Title
        char buf[32];
        _spr_text.setTextColor(0x00FF00U, COLOR_BG);
        _spr_text.drawString("TUNING LAB", 240, 20);

        // Parameter rows: 3 params, each with [-] [value] [+]
        // Layout: 6 columns x 2 rows of buttons
        const int bw = 480 / 6; // 80px per button
        const int bh = 42;
        const int row0_y = 42;  // First param row
        const int row1_y = row0_y + bh + 2; // Second row

        struct Param {
            const char* name; float val; const char* fmt;
            uint32_t bg; uint32_t brd;
        };
        Param params[] = {
            {"ALPHA", alpha, "%.4f", 0x333333U, 0x777777U},
            {"BW",    lpf_k, "%.2f",  0x333333U, 0x777777U},
            {"SQ",    sq_snr, "%.0f", 0x333333U, 0x777777U},
        };

        // Row 0: [A-] [ALPHA:val] [A+]  [K-] [BW:val] [K+]
        for (int p = 0; p < 2; p++) {
            int bx = p * 3 * bw;
            // Minus button
            _spr_text.fillRoundRect(bx + 2, row0_y, bw - 4, bh, 6, 0x333333U);
            _spr_text.drawRoundRect(bx + 2, row0_y, bw - 4, bh, 6, 0x777777U);
            _spr_text.setTextColor(0xFFFFFFU, 0x333333U);
            snprintf(buf, sizeof(buf), "%s-", params[p].name);
            _spr_text.drawString(buf, bx + bw/2, row0_y + bh/2);
            // Value display
            _spr_text.fillRoundRect(bx + bw + 2, row0_y, bw - 4, bh, 6, 0x111111U);
            _spr_text.drawRoundRect(bx + bw + 2, row0_y, bw - 4, bh, 6, 0x333333U);
            snprintf(buf, sizeof(buf), params[p].fmt, params[p].val);
            _spr_text.setTextColor(0x00FFFFU, 0x111111U);
            _spr_text.drawString(buf, bx + bw + bw/2, row0_y + bh/2);
            // Plus button
            _spr_text.fillRoundRect(bx + 2*bw + 2, row0_y, bw - 4, bh, 6, 0x333333U);
            _spr_text.drawRoundRect(bx + 2*bw + 2, row0_y, bw - 4, bh, 6, 0x777777U);
            _spr_text.setTextColor(0xFFFFFFU, 0x333333U);
            snprintf(buf, sizeof(buf), "%s+", params[p].name);
            _spr_text.drawString(buf, bx + 2*bw + bw/2, row0_y + bh/2);
        }

        // Row 1: [SQ-] [SQ:val] [SQ+]  [SERIAL] [---] [BACK]
        int bx = 0;
        // SQ minus
        _spr_text.fillRoundRect(bx + 2, row1_y, bw - 4, bh, 6, 0x333333U);
        _spr_text.drawRoundRect(bx + 2, row1_y, bw - 4, bh, 6, 0x777777U);
        _spr_text.setTextColor(0xFFFFFFU, 0x333333U);
        _spr_text.drawString("SQ-", bx + bw/2, row1_y + bh/2);
        // SQ value
        _spr_text.fillRoundRect(bx + bw + 2, row1_y, bw - 4, bh, 6, 0x111111U);
        _spr_text.drawRoundRect(bx + bw + 2, row1_y, bw - 4, bh, 6, 0x333333U);
        snprintf(buf, sizeof(buf), "%.0f", sq_snr);
        _spr_text.setTextColor(0x00FFFFU, 0x111111U);
        _spr_text.drawString(buf, bx + bw + bw/2, row1_y + bh/2);
        // SQ plus
        _spr_text.fillRoundRect(bx + 2*bw + 2, row1_y, bw - 4, bh, 6, 0x333333U);
        _spr_text.drawRoundRect(bx + 2*bw + 2, row1_y, bw - 4, bh, 6, 0x777777U);
        _spr_text.setTextColor(0xFFFFFFU, 0x333333U);
        _spr_text.drawString("SQ+", bx + 2*bw + bw/2, row1_y + bh/2);

        // DUMP toggle button
        {
            bool dump_on = shared_serial_diag;
            uint32_t dbg = dump_on ? 0x004400U : 0x222200U;
            uint32_t dbr = dump_on ? 0x00FF00U : 0x777700U;
            _spr_text.fillRoundRect(3*bw + 2, row1_y, bw - 4, bh, 6, dbg);
            _spr_text.drawRoundRect(3*bw + 2, row1_y, bw - 4, bh, 6, dbr);
            _spr_text.setTextColor(dump_on ? 0x00FF00U : 0x777700U, dbg);
            _spr_text.drawString(dump_on ? "DUMP:ON" : "DUMP:OFF", 3*bw + bw/2, row1_y + bh/2);
        }

        // Empty slot
        _spr_text.fillRoundRect(4*bw + 2, row1_y, bw - 4, bh, 6, 0x111111U);
        _spr_text.drawRoundRect(4*bw + 2, row1_y, bw - 4, bh, 6, 0x333333U);

        // SAVE button
        if (saved) {
            _spr_text.fillRoundRect(5*bw + 2, row1_y, bw - 4, bh, 6, 0x006600U);
            _spr_text.drawRoundRect(5*bw + 2, row1_y, bw - 4, bh, 6, 0x00FF00U);
            _spr_text.setTextColor(0x00FF00U, 0x006600U);
            _spr_text.drawString("SAVED!", 5*bw + bw/2, row1_y + bh/2);
        } else {
            _spr_text.fillRoundRect(5*bw + 2, row1_y, bw - 4, bh, 6, 0x000066U);
            _spr_text.drawRoundRect(5*bw + 2, row1_y, bw - 4, bh, 6, 0x0000FFU);
            _spr_text.setTextColor(0xFFFFFFU, 0x000066U);
            _spr_text.drawString("SAVE", 5*bw + bw/2, row1_y + bh/2);
        }

        ili9488_push_colors(0, UI_Y_TEXT, 480, UI_TEXT_ZONE_H, (uint16_t*)_spr_text.getBuffer());
    }
};
#endif
