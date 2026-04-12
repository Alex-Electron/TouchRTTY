#pragma once
#include "app_state.hpp"
#include "LGFX_Config.hpp"

// Load persisted settings from flash (no-op if invalid magic)
// Populates shared_* variables from AppSettings if found.
void settings_load();

// Build AppSettings from current shared state.
// display_mode and auto_scale are passed explicitly (not in shared state).
void settings_build_from_state(AppSettings& s, int display_mode, bool auto_scale);

// Write AppSettings to flash (erases settings sector first).
void settings_write_to_flash(const AppSettings& s);

// Save current shared state immediately (used by SAVE button / serial SAVE command)
void settings_save_now(int display_mode, bool auto_scale);

// Touchscreen calibration load/run. Called at startup.
void load_or_calibrate(lgfx::LGFX_Device& tft, bool force = false);
