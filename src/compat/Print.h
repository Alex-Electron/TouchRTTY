#ifndef ARDUINO_PRINT_H
#define ARDUINO_PRINT_H

#include <stddef.h>
#include <stdint.h>

class Print {
public:
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t *buffer, size_t size) {
        size_t n = 0;
        while (size--) {
            if (write(*buffer++)) n++;
            else break;
        }
        return n;
    }
    // Minimal implementations for compatibility
    size_t print(const char* s) { return write((const uint8_t*)s, strlen(s)); }
    size_t println(const char* s) { size_t n = print(s); n += print("\r\n"); return n; }
};

#endif
