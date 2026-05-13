// TouchRTTY
// Copyright (C) 2026 Alexander Lavrinovich (Alex.Electron)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

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
