#pragma once
constexpr int VSPI = 3;
struct SPIClass {
	explicit SPIClass(int) {}
	void begin(int, int, int, int) {}
};
