#pragma once

// Core 1 entry point: runs the UI loop (FFT/waterfall/touch/menus/text render).
// Owns LGFX display, UIManager, and all screen state. Reads shared_* variables
// from Core 0 (DSP) and publishes user input changes back.
void core1_main();
