#pragma once
#include "../host/Arduino.h"
#include <cstdio>
struct TestSerial {
	template<typename... Args> void printf(const char *, Args...) {}
};
inline TestSerial Serial;
