#pragma once
#include <Arduino.h>
#include <SD.h>
#include <algorithm>
#include "config.h"

// Main-loop-only SD owner. Host sends <=1024-byte blocks and awaits each ACK.
// Commands: MAPINFO; MAPPUT x y size crc32hex. Zoom follows Config::map_zoom.
class MapTransfer {
public:
	bool active() const { return receiving_; }
	void poll(uint32_t now)
	{
		if ((receiving_ || used_) && now - last_ms_ > 10000) {
			abort("timeout");
			used_ = 0;
		}
		if (receiving_) {
			while (Serial.available() && used_ < block_size_) {
				buffer_[used_++] = Serial.read();
				last_ms_ = now;
			}
			if (used_ != block_size_) return;
			if (file_.write(buffer_, used_) != used_) { abort("write"); return; }
			for (size_t i = 0; i < used_; ++i) {
				crc_ ^= buffer_[i];
				for (int bit = 0; bit < 8; ++bit)
					crc_ = (crc_ >> 1) ^ ((crc_ & 1) ? 0xedb88320U : 0);
			}
			received_ += used_;
			used_ = 0;
			if (received_ < size_) {
				block_size_ = std::min(size_t(1024), size_ - received_);
				Serial.printf("MAP ACK %u\n", unsigned(received_));
				return;
			}
			file_.flush();
			file_.close();
			if ((crc_ ^ 0xffffffffU) != expected_crc_) { abort("crc"); return; }
			// Verify persisted bytes before accepting the transfer.
			File verify = SD.open(temporary_, FILE_READ);
			uint32_t check = 0xffffffffU;
			size_t total = 0;
			while (verify && verify.available()) {
				const size_t count = verify.read(buffer_, sizeof(buffer_));
				if (!count) break;
				total += count;
				for (size_t i = 0; i < count; ++i) {
					check ^= buffer_[i];
					for (int bit = 0; bit < 8; ++bit)
						check = (check >> 1) ^ ((check & 1) ? 0xedb88320U : 0);
				}
				yield();
			}
			verify.close();
			if (total != size_ || (check ^ 0xffffffffU) != expected_crc_) {
				abort("readback"); return;
			}
			// Replace a differing map tile only after CRC/readback, with rollback backup.
			char backup[56];
			snprintf(backup, sizeof(backup), "%s.previous", path_);
			if (SD.exists(backup)) SD.remove(backup);
			const bool replacing = SD.exists(path_);
			if (replacing && !SD.rename(path_, backup)) { abort("backup"); return; }
			if (!SD.rename(temporary_, path_)) {
				if (replacing) SD.rename(backup, path_);
				abort("rename"); return;
			}
			if (replacing) SD.remove(backup);
			receiving_ = false;
			Serial.printf("MAP DONE %u %08lx\n", unsigned(total), (unsigned long)expected_crc_);
			return;
		}
		// Bounded line parser; no blocking readString/readBytes calls.
		for (int budget = 128; budget && Serial.available(); --budget) {
			const char byte = Serial.read();
			last_ms_ = now;
			if (byte == '\r') continue;
			if (byte != '\n') {
				if (used_ + 1 < sizeof(buffer_)) buffer_[used_++] = byte;
				else { used_ = 0; Serial.println("MAP ERROR command"); }
				continue;
			}
			buffer_[used_] = 0;
			used_ = 0;
			if (!strcmp((char *)buffer_, "MAPFAST")) {
				Serial.println("MAP FAST"); Serial.flush();
				Serial.updateBaudRate(921600); return;
			}
			if (!strcmp((char *)buffer_, "MAPSLOW")) {
				Serial.println("MAP SLOW"); Serial.flush();
				Serial.updateBaudRate(115200); return;
			}
			if (!strcmp((char *)buffer_, "MAPINFO")) {
				Serial.printf("MAP INFO sd=%d zoom=%d free=%llu\n", SD.cardType() != CARD_NONE, Config::map_zoom,
					(unsigned long long)(SD.totalBytes() - SD.usedBytes()));
				return;
			}
			unsigned x, y, bytes, crc;
			char extra;
			if (sscanf((char *)buffer_, "MAPPUT %u %u %u %x %c", &x, &y, &bytes, &crc, &extra) != 4 ||
			    x >= (1U << Config::map_zoom) || y >= (1U << Config::map_zoom) || bytes < 33 || bytes > 49152) {
				Serial.println("MAP ERROR command"); return;
			}
			snprintf(path_, sizeof(path_), "/maps/%d/%u/%u.png", Config::map_zoom, x, y);
			snprintf(temporary_, sizeof(temporary_), "%s.upload", path_);
			char directory[48];
			if (SD.cardType() == CARD_NONE || !SD.mkdir("/maps")) { abort("sd"); return; }
			snprintf(directory, sizeof(directory), "/maps/%d", Config::map_zoom);
			if (!SD.mkdir(directory)) { abort("directory"); return; }
			snprintf(directory, sizeof(directory), "/maps/%d/%u", Config::map_zoom, x);
			if (!SD.mkdir(directory)) { abort("directory"); return; }
			char backup[56];
			snprintf(backup, sizeof(backup), "%s.previous", path_);
			if (!SD.exists(path_) && SD.exists(backup)) SD.rename(backup, path_);
			if (SD.exists(path_)) {
				File cached = SD.open(path_, FILE_READ);
				uint32_t check = 0xffffffffU;
				size_t total = 0;
				while (cached && cached.available()) {
					const size_t count = cached.read(buffer_, sizeof(buffer_));
					if (!count) break;
					total += count;
					for (size_t i = 0; i < count; ++i) {
						check ^= buffer_[i];
						for (int bit = 0; bit < 8; ++bit)
							check = (check >> 1) ^ ((check & 1) ? 0xedb88320U : 0);
					}
					yield();
				}
				cached.close();
				Serial.println(total == bytes && (check ^ 0xffffffffU) == crc ?
					"MAP EXISTS" : "MAP REPLACE");
				if (total == bytes && (check ^ 0xffffffffU) == crc) return;
			}
			if (SD.exists(temporary_)) SD.remove(temporary_);
			file_ = SD.open(temporary_, FILE_WRITE);
			if (!file_) { abort("open"); return; }
			size_ = bytes; received_ = 0; expected_crc_ = crc; crc_ = 0xffffffffU;
			block_size_ = std::min(size_t(1024), size_);
			receiving_ = true;
			Serial.println("MAP READY");
			return;
		}
	}
private:
	void abort(const char *reason)
	{
		file_.close();
		if (temporary_[0] && SD.exists(temporary_)) SD.remove(temporary_);
		receiving_ = false; used_ = 0;
		Serial.printf("MAP ERROR %s\n", reason);
	}
	File file_;
	uint8_t buffer_[1024]{};
	char path_[48]{}, temporary_[56]{};
	size_t used_ = 0, size_ = 0, received_ = 0, block_size_ = 0;
	uint32_t crc_ = 0, expected_crc_ = 0, last_ms_ = 0;
	bool receiving_ = false;
};
