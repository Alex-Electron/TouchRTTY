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

#include <string>
#include <ff.h>
#include <f_util.h>
#include <hw_config.h>

class SDManager {
public:
    static SDManager& getInstance() {
        static SDManager instance;
        return instance;
    }

    bool mount() {
        if (mounted) return true;
        
        sd_card_t *pSD = sd_get_by_num(0);
        if (!pSD) return false;

        FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
        if (fr == FR_OK) {
            mounted = true;
            return true;
        }
        return false;
    }

    void unmount() {
        if (!mounted) return;
        sd_card_t *pSD = sd_get_by_num(0);
        if (pSD) {
            f_unmount(pSD->pcName);
        }
        mounted = false;
    }

    bool startLogging(const std::string& filename) {
        if (!mounted && !mount()) return false;

        FRESULT fr = f_open(&logFile, filename.c_str(), FA_OPEN_APPEND | FA_WRITE);
        if (fr == FR_OK) {
            logging = true;
            return true;
        }
        return false;
    }

    void stopLogging() {
        if (logging) {
            f_close(&logFile);
            logging = false;
        }
    }

    void writeLog(const char* text) {
        if (!logging) return;

        UINT bw;
        f_write(&logFile, text, strlen(text), &bw);
        f_sync(&logFile); // Flush to card to prevent loss on power-off
    }

    bool isLogging() const { return logging; }
    bool isMounted() const { return mounted; }

private:
    SDManager() : mounted(false), logging(false) {}
    ~SDManager() {
        stopLogging();
        unmount();
    }

    bool mounted;
    bool logging;
    FIL logFile;
};
