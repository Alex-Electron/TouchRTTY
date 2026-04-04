#ifndef ARDUINO_COMPAT_H
#define ARDUINO_COMPAT_H

#include "pico/stdlib.h"
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <string>
#include <algorithm>
#include <stdlib.h>
#include "LittleFS.h"

#define PROGMEM
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#define pgm_read_word(addr) (*(const uint16_t*)(addr))
#define pgm_read_dword(addr) (*(const uint32_t*)(addr))

typedef bool boolean;
typedef uint8_t byte;

#ifndef HIGH
#define HIGH 1
#endif
#ifndef LOW
#define LOW 0
#endif
#ifndef OUTPUT
#define OUTPUT 1
#endif
#ifndef INPUT
#define INPUT 0
#endif

// Global min/max functions that handle mixed types
#ifdef __cplusplus
namespace arduino {
    template<class T, class U>
    constexpr auto min(T a, U b) -> decltype(a < b ? a : b) { return (a < b) ? a : b; }
    template<class T, class U>
    constexpr auto max(T a, U b) -> decltype(a > b ? a : b) { return (a > b) ? a : b; }
}
using arduino::min;
using arduino::max;
#endif

inline void pinMode(uint32_t pin, uint32_t mode) { gpio_init(pin); gpio_set_dir(pin, mode); }
inline void digitalWrite(uint32_t pin, uint32_t val) { gpio_put(pin, val); }
inline int digitalRead(uint32_t pin) { return gpio_get(pin); }

inline uint32_t millis() { return to_ms_since_boot(get_absolute_time()); }
inline uint32_t micros() { return to_us_since_boot(get_absolute_time()); }
inline void delay(uint32_t ms) { sleep_ms(ms); }
inline void delayMicroseconds(uint32_t us) { sleep_us(us); }

#define yield() tight_loop_contents()

// Minimal String class for TFT_eSPI
class String : public std::string {
public:
    String() : std::string() {}
    String(const char* s) : std::string(s ? s : "") {}
    String(const std::string& s) : std::string(s) {}
    void toCharArray(char* buf, size_t len) const {
        size_t n = (std::string::length() < len - 1) ? std::string::length() : len - 1;
        memcpy(buf, c_str(), n);
        buf[n] = 0;
    }
    int length() const { return (int)std::string::length(); }
    bool operator==(const char* s) const { return std::string::compare(s) == 0; }
    bool operator==(const String& s) const { return std::string::compare(s) == 0; }
};

class SerialFake {
public:
    void print(const String& s) { printf("%s", s.c_str()); }
    void print(const char* s) { printf("%s", s); }
    void println(const String& s) { printf("%s\n", s.c_str()); }
    void println(const char* s) { printf("%s\n", s); }
};
extern SerialFake Serial;

// Rename to avoid conflict with stdlib random()
#ifdef random
#undef random
#endif
#define random(args...) arduino_random(args)
inline long arduino_random(long max) { if (max <= 0) return 0; return rand() % max; }
inline long arduino_random(long min, long max) { if (max <= min) return min; return min + rand() % (max - min); }

#endif
