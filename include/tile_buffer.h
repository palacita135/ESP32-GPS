#pragma once
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdint.h>

// Compressed tile storage does not require one contiguous PNG-sized allocation.
class TileBuffer {
public:
	~TileBuffer() { clear(); }
	bool allocate(size_t size)
	{
		clear();
		if (!size || size > 49152) return false;
		size_ = size;
		for (size_t i = 0; i < (size + 2047) / 2048; ++i) {
			blocks_[i] = static_cast<uint8_t *>(malloc(2048));
			if (!blocks_[i]) { clear(); return false; }
		}
		return true;
	}
	void clear()
	{
		for (auto &block : blocks_) { free(block); block = nullptr; }
		size_ = 0;
	}
	size_t size() const { return size_; }
	uint8_t *block(size_t offset) { return blocks_[offset / 2048] + offset % 2048; }
	uint8_t at(size_t offset) const { return blocks_[offset / 2048][offset % 2048]; }
	size_t read(size_t offset, uint8_t *data, size_t length) const
	{
		if (offset >= size_) return 0;
		length = std::min(length, size_ - offset);
		size_t done = 0;
		while (done < length) {
			const size_t pos = offset + done;
			const size_t count = std::min(length - done, 2048 - pos % 2048);
			memcpy(data + done, blocks_[pos / 2048] + pos % 2048, count);
			done += count;
		}
		return done;
	}
private:
	uint8_t *blocks_[24] = {};
	size_t size_ = 0;
};
