#pragma once
#include <cstdint>
extern uint32_t test_now_ms;
inline int64_t esp_timer_get_time() { return static_cast<int64_t>(test_now_ms) * 1000; }
