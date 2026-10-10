#pragma once

#include <stdint.h>

namespace Config {
// Touch controller is unconfirmed (3248S035R: XPT2046; C: GT911).
// Use BOOT controls until the exact touch variant and wiring are identified.
constexpr bool resistive_touch_enabled = false;
// P3 UART2: directions are named from the ESP32 perspective.
constexpr int gps_rx = 35; // P3 IO35 <- GPS TX.
constexpr int gps_tx = 22; // P3 IO22 -> GPS RX; available for receiver commands.
constexpr uint32_t gps_baud = 9600;
constexpr uint32_t stale_ms = 2500;  // Appropriate for a 1 Hz receiver.
constexpr uint32_t render_ms = 200;
constexpr uint32_t splash_ms = 3000;
constexpr double speed_warning_kmh = 180.0;
constexpr uint32_t speed_warning_blink_ms = 400; // Each on/off phase.
constexpr uint32_t diagnostics_ms = 2000;
constexpr uint32_t track_interval_ms = 1000; // At most one CSV point per second.
constexpr uint8_t rotation = 1;
constexpr int backlight_channel = 7;
constexpr uint8_t day_brightness = 220;
constexpr uint8_t night_brightness = 35;
// This suppresses stationary GPS creep but also hides very slow motion.
constexpr double stop_threshold_kmh = 2.0;
// Sanity bound, not a vehicle speed limiter.
constexpr double maximum_kmh = 300.0;
}
