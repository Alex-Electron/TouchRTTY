
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
