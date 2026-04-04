#ifndef LITTLEFS_STUB_H
#define LITTLEFS_STUB_H

#include <stddef.h>
#include <stdint.h>

class String; // Forward declaration

namespace fs {
    enum SeekMode { SeekSet = 0, SeekCur = 1, SeekEnd = 2 };

    class File {
    public:
        File() : _valid(false) {}
        bool operator!() const { return !_valid; }
        operator bool() const { return _valid; }
        size_t read(uint8_t* buf, size_t size) { return 0; }
        int read() { return -1; }
        bool seek(uint32_t pos, SeekMode mode = SeekSet) { return false; }
        void close() { _valid = false; }
        size_t size() { return 0; }
    private:
        bool _valid;
    };

    class FS {
    public:
        bool begin(bool format = false) { return true; }
        File open(const char* path, const char* mode) { return File(); }
        bool exists(const char* path) { return false; }
        
        // Templates to handle String without needing its full definition here
        template <typename T>
        File open(const T& path, const char* mode) { return open(path.c_str(), mode); }
        template <typename T>
        bool exists(const T& path) { return exists(path.c_str()); }
    };
}

extern fs::FS SPIFFS;
extern fs::FS LittleFS;

#endif
