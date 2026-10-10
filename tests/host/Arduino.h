#pragma once

// Test-only shim for the actual TinyGPS++ parser; no embedded HAL is mocked.
#include <cmath>
#include <cstdint>
#include <cstdlib>
extern uint32_t test_now_ms;
using byte = unsigned char;
inline unsigned long millis() { return test_now_ms; }
constexpr double TWO_PI = 6.28318530717958647692;
inline double radians(double x) { return x * TWO_PI / 360; }
inline double degrees(double x) { return x * 360 / TWO_PI; }
inline double sq(double x) { return x * x; }
