#pragma once
#include <string>
#include <cstring>
#include <cstdint>
#include <map>
#include "SPI.h"
constexpr int FILE_WRITE = 1;
inline std::map<std::string, std::string> test_files;
inline bool test_write_failure = false;
struct File {
	std::string path;
	explicit operator bool() const { return !path.empty(); }
	size_t write(const uint8_t *data, size_t n) {
		if (test_write_failure) return 0;
		test_files[path].append(reinterpret_cast<const char *>(data), n);
		return n;
	}
	size_t print(const char *s) { return write(reinterpret_cast<const uint8_t *>(s), strlen(s)); }
	void flush() {}
	void close() { path.clear(); }
};
struct TestSD {
	bool begin(int, SPIClass &, int) { return true; }
	bool mkdir(const char *) { return true; }
	bool exists(const char *path) { return test_files.count(path) != 0; }
	File open(const char *path, int) { test_files[path] = ""; return File{path}; }
};
inline TestSD SD;
