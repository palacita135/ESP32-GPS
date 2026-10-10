#pragma once
#include <TFT_eSPI.h>
#include <PNGdec.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <new>
#include <algorithm>
#include <time.h>
#include "map_math.h"
#include "tile_buffer.h"
#include "map_root_ca.h"

// Main loop exclusively owns SD/TFT/PNG/track state. Worker owns HTTPS.
class StreetMap {
	struct Request { int x, y, zoom; };
	struct Result { Request tile; uint8_t *data; size_t size; int code; int tls_code; };
	struct Point { MapMath::Pixel pixel; bool gap; };
	// HTTPClient removes transfer framing before delivering bytes to this sink.
	class TileSink : public Stream {
	public:
		TileSink(QueueHandle_t queue, Request tile) : queue_(queue), tile_(tile) {}
		int available() override { return 0; }
		int read() override { return -1; }
		int peek() override { return -1; }
		void flush() override {}
		size_t write(uint8_t byte) override { return write(&byte, 1); }
		size_t write(const uint8_t *data, size_t length) override
		{
			if (total + length > 49152) { error = -101; return 0; }
			size_t sent = 0;
			while (sent < length) {
				const size_t count = std::min(static_cast<size_t>(2048), length - sent);
				auto *buffer = static_cast<uint8_t *>(malloc(count));
				if (!buffer) { error = -100; return sent; }
				memcpy(buffer, data + sent, count);
				Result chunk{tile_, buffer, count, 206, 0};
				xQueueSend(queue_, &chunk, portMAX_DELAY);
				sent += count; total += count;
			}
			return sent;
		}
		size_t total = 0;
		int error = 0;
	private:
		QueueHandle_t queue_;
		Request tile_;
	};
public:
	explicit StreetMap(TFT_eSPI &display) : tft_(display) {}
	void begin()
	{
		requests_ = xQueueCreate(1, sizeof(Request));
		results_ = xQueueCreate(1, sizeof(Result));
		if (requests_ && results_ &&
		    xTaskCreate(worker, "osm-https", 8192, this, 1, nullptr) == pdPASS)
			worker_ready_ = true;
		cache_ready_ = SD.cardType() != CARD_NONE && SD.mkdir("/tiles");
	}
	void activate(bool active)
	{
		active_ = active;
		delete png_;
		png_ = nullptr;
		decoder_failed_ = false;
		dirty_ = true;
		if (active) {
			chrome_drawn_ = false;
			tft_.fillScreen(TFT_DARKGREY); // Clear only on page entry.
			++revision_;
		}
	}
	void add(double lat, double lon, bool gap, uint32_t now, bool moving = true)
	{
		pending_gap_ = pending_gap_ || gap;
		if (have_fix_ && !moving) {
			last_fix_ms_ = now;
			return; // Hold display position; raw SD samples remain untouched.
		}
		gap = pending_gap_;
		pending_gap_ = false;
		const MapMath::Pixel previous = position_;
		position_ = MapMath::project(lat, lon, zoom_);
		if (have_fix_ && (fabs(position_.x - previous.x) > 1 ||
		    fabs(position_.y - previous.y) > 1)) {
			// Restore map pixels under the old marker without clearing the LCD.
			for (bool &loaded : loaded_) loaded = false;
			cursor_ = 0;
		}
		if (count_ == capacity) {
			// Retain a progressively simplified whole-session route, not only
			// the last eight minutes. Propagate gaps through removed samples.
			gap = gap || points_[capacity - 1].gap;
			for (size_t i = 0; i < capacity / 2; ++i) {
				const bool removed_gap = i && points_[2 * i - 1].gap;
				points_[i] = points_[2 * i];
				points_[i].gap = points_[i].gap || removed_gap;
			}
			count_ = capacity / 2;
			next_ = count_;
			dirty_ = true;
		}
		points_[next_] = {position_, gap};
		next_ = (next_ + 1) % capacity;
		if (count_ < capacity) ++count_;
		last_fix_ms_ = now;
		have_fix_ = true;
		if (!centered_ || fabs(MapMath::delta_x(position_.x, center_.x, zoom_)) > 64 ||
		    fabs(position_.y - center_.y) > 48) {
			center_ = position_;
			centered_ = true;
			dirty_ = true;
		}
	}
	void poll(uint32_t now)
	{
		Result result;
		if (results_ && xQueueReceive(results_, &result, 0) == pdTRUE) {
			if (result.code == 206) { // Internal chunk message, not an HTTP status.
				if (!download_bytes_) {
					download_ok_ = true;
					cache_error_ = "";
					prefix_count_ = 0;
					char path[64]; tile_path(path, result.tile);
					temporary_path_ = String(path) + ".part";
					if (SD.exists(temporary_path_)) SD.remove(temporary_path_);
					download_file_ = SD.open(temporary_path_, FILE_WRITE);
					if (!download_file_) cache_error_ = "Tile SD open failed";
				}
				const size_t prefix_bytes = std::min(result.size, sizeof(prefix_) - prefix_count_);
				memcpy(prefix_ + prefix_count_, result.data, prefix_bytes);
				prefix_count_ += prefix_bytes;
				if (prefix_count_ == sizeof(prefix_) && memcmp(prefix_, "\x89PNG\r\n\x1a\n", 8)) {
					download_ok_ = false; cache_error_ = "Tile body is not PNG";
				}
				if (!download_file_ || download_file_.write(result.data, result.size) != result.size) {
					download_ok_ = false;
					if (download_file_) cache_error_ = "Tile SD write failed";
				}
				download_bytes_ += result.size;
				free(result.data);
			} else {
				busy_ = false;
				retry_ms_ = now + 30000;
				download_file_.close();
				if (result.code == 200 && download_ok_ && download_bytes_ == result.size && result.size >= 24) {
					char path[64]; tile_path(path, result.tile);
					if (SD.exists(path)) SD.remove(path);
					if (SD.rename(temporary_path_.c_str(), path)) {
						status_ = "OSM tiles"; retry_ms_ = now;
					} else status_ = "Tile cache rename failed";
				} else {
					if (result.tls_code)
						snprintf(error_status_, sizeof(error_status_), "TLS error %d / retry 30s", result.tls_code);
					else if (result.code == 200)
						snprintf(error_status_, sizeof(error_status_), "%s", cache_error_[0] ? cache_error_ : "Tile size mismatch");
					else
						snprintf(error_status_, sizeof(error_status_), "Tile error %d / retry 30s", result.code);
					status_ = error_status_;
				}
				if (temporary_path_.length()) if (SD.exists(temporary_path_)) SD.remove(temporary_path_);
				temporary_path_ = "";
				Serial.printf("MAP tile %d/%d: code=%d bytes=%u cache=%s\n",
					      result.tile.x, result.tile.y, result.code,
					      static_cast<unsigned>(result.size), status_);
				if (!download_ok_ || download_bytes_ != result.size) {
					Serial.printf("MAP cache: received=%u expected=%u error=%s prefix=",
						      static_cast<unsigned>(download_bytes_),
						      static_cast<unsigned>(result.size), cache_error_);
					for (size_t i = 0; i < prefix_count_; ++i) Serial.printf(" %02X", prefix_[i]);
					Serial.println();
				}
				download_bytes_ = 0;
				prefix_count_ = 0;
				download_ok_ = false;
				cursor_ = 0;
			}
		}
		if (!active_) return;
		if (dirty_) {
			cursor_ = 0;
			for (int i = 0; i < 9; ++i) {
				loaded_[i] = false; placeholder_[i] = false;
			}
			base_x_ = centered_ ? static_cast<int>(floor((center_.x - 240) / 256)) : 0;
			base_y_ = centered_ ? static_cast<int>(floor((center_.y - 134) / 256)) : 0;
			dirty_ = false;
		}
		if (centered_ && cursor_ < 9) {
			const int slot = cursor_;
			const int tx = base_x_ + cursor_ % 3, ty = base_y_ + cursor_ / 3;
			++cursor_;
			const int px = static_cast<int>(tx * 256.0 - center_.x + 240);
			const int py = static_cast<int>(ty * 256.0 - center_.y + 166);
			if (px < 480 && px + 256 > 0 && py < 300 && py + 256 > 32 &&
			    ty >= 0 && ty < (1 << zoom_)) {
				Request request{MapMath::wrap_tile(tx, zoom_), ty, zoom_};
				char path[64]; tile_path(path, request);
				if (cache_ready_ && SD.exists(path)) {
					if (!loaded_[slot] && !busy_ && static_cast<int32_t>(now - decode_retry_ms_) >= 0)
						loaded_[slot] = draw_tile(path, px, py);
				} else {
					if (!placeholder_[slot]) {
						tft_.setViewport(0, 32, 480, 268, false);
						tft_.fillRect(px, py, 256, 256, TFT_DARKGREY);
						tft_.resetViewport();
						placeholder_[slot] = true;
					}
				if (!busy_ && worker_ready_ && cache_ready_ &&
					 WiFi.status() == WL_CONNECTED && time(nullptr) > 1700000000 &&
					 (!requested_ || static_cast<int32_t>(now - retry_ms_) >= 0)) {
					if (xQueueSend(requests_, &request, 0) == pdTRUE) {
						busy_ = true; requested_ = true;
						status_ = "Downloading OSM tile";
						Serial.printf("MAP request %d/%d/%d heap=%u largest=%u\n",
							      request.zoom, request.x, request.y,
							      ESP.getFreeHeap(), ESP.getMaxAllocHeap());
					}
				}
				}
			}
		}
		if (cursor_ >= 9 && !busy_ && now - last_sweep_ms_ >= 100) {
			last_sweep_ms_ = now; cursor_ = 0;
		}
		if (now - last_overlay_ms_ >= 200 || cursor_ < 9) {
			last_overlay_ms_ = now;
			overlay(now);
		}
	}
	const char *status() const
	{
		if (!cache_ready_) return "Insert FAT32 SD for map tiles";
		if (!worker_ready_) return "Network worker: low RAM";
		if (WiFi.status() != WL_CONNECTED) return "WiFi not connected / cached tiles";
		if (!centered_) return "Waiting for GPS position";
		if (decoder_failed_) return "Map decoder: low RAM";
		if (time(nullptr) <= 1700000000) return "Waiting for network time (TLS)";
		return status_;
	}
	uint32_t revision() const { return revision_; }
private:
	static void worker(void *argument)
	{
		auto *self = static_cast<StreetMap *>(argument);
		Request request;
		for (;;) {
			if (xQueueReceive(self->requests_, &request, portMAX_DELAY) != pdTRUE) continue;
			Result result{request, nullptr, 0, -1, 0};
			{
				WiFiClientSecure client;
				client.setCACert(map_root_ca);
				client.setHandshakeTimeout(15);
				HTTPClient http;
				http.setConnectTimeout(15000); http.setTimeout(15000);
				char url[128];
				snprintf(url, sizeof(url), "https://tile.openstreetmap.org/%d/%d/%d.png",
					 request.zoom, request.x, request.y);
				if (http.begin(client, url)) {
					http.setUserAgent("PALACITA135/1.0 (+https://github.com/palacita135/ESP32-GPS)");
					result.code = http.GET();
					if (result.code < 0) {
						char tls_error[160] = {};
						const int tls_code = client.lastError(tls_error, sizeof(tls_error));
						result.tls_code = tls_code;
						Serial.printf("MAP HTTPS: %s; TLS=%d %s\n",
							      HTTPClient::errorToString(result.code).c_str(), tls_code, tls_error);
						Serial.printf("MAP network: UTC=%lld RSSI=%d heap=%u largest=%u\n",
							      static_cast<long long>(time(nullptr)), WiFi.RSSI(),
							      ESP.getFreeHeap(), ESP.getMaxAllocHeap());
					}
					const int length = http.getSize();
					if (result.code == 200 && (length == -1 || (length > 0 && length <= 49152))) {
						TileSink sink(self->results_, request);
						const int received = http.writeToStream(&sink);
						result.size = sink.total;
						if (sink.error) result.code = sink.error;
						else if (received < 0) result.code = received;
						else if (length > 0 && sink.total != static_cast<size_t>(length)) result.code = -2;
					} else if (result.code == 200) result.code = -101;

					http.end();
				}
			} // Release TLS memory before signalling completion and allowing PNG decode.
			xQueueSend(self->results_, &result, portMAX_DELAY);
		}
	}
	static void tile_path(char *path, const Request &request)
	{
		snprintf(path, 64, "/tiles/%d_%d_%d.png", request.zoom, request.x, request.y);
	}
	static StreetMap *&decode_owner() { static StreetMap *owner = nullptr; return owner; }
	static void *open_file(const char *, int32_t *size)
	{ *size = decode_owner()->tile_buffer_.size(); return &decode_owner()->tile_buffer_; }
	static void close_file(void *) {}
	static int32_t read_file(PNGFILE *file, uint8_t *data, int32_t length)
	{
		if (length <= 0 || file->iPos < 0) return 0;
		auto *buffer = static_cast<TileBuffer *>(file->fHandle);
		const size_t count = buffer->read(file->iPos, data, length);
		file->iPos += count; return count;
	}
	static int32_t seek_file(PNGFILE *file, int32_t position)
	{
		if (position < 0 || position > file->iSize) return -1;
		file->iPos = position; return position;
	}
	static int draw_line(PNGDRAW *line)
	{
		auto *self = static_cast<StreetMap *>(line->pUser);
		uint16_t pixels[256];
		if (line->iWidth != 256) return 0;
		if (millis() - self->decode_started_ > 3000) return 0;
		self->png_->getLineAsRGB565(line, pixels, PNG_RGB565_BIG_ENDIAN, 0xffffffff);
		self->tft_.pushImage(self->tile_x_, self->tile_y_ + line->y, 256, 1, pixels);
		if ((line->y & 15) == 15) delay(1); // Allow idle/system tasks to run.
		return 1;
	}
	bool draw_tile(const char *path, int x, int y)
	{
		// TLS is finished before this runs. Buffer bounded SD reads before inflate,
		// so a stalled/short SD callback cannot trap PNGdec in its IDAT loop.
		Serial.printf("MAP decode start: %s heap=%u largest=%u\n", path, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
		const uint32_t started = millis();
		File file = SD.open(path, FILE_READ);
		const size_t size = file ? file.size() : 0;
		png_ = new (std::nothrow) PNG;
		decoder_failed_ = !png_;
		if (!png_ || size < 33 || size > 49152 || !tile_buffer_.allocate(size)) {
			file.close(); delete png_; png_ = nullptr; tile_buffer_.clear();
			decoder_failed_ = true; decode_retry_ms_ = millis() + 5000;
			Serial.printf("MAP decode allocation failed: PNG=%u tile=%u heap=%u largest=%u\n",
				      static_cast<unsigned>(sizeof(PNG)), static_cast<unsigned>(size), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
			return false;
		}
		size_t received = 0;
		while (received < size && millis() - started < 2000) {
			const size_t count = std::min(std::min(size - received, static_cast<size_t>(512)), 2048 - received % 2048);
			const size_t bytes = file.read(tile_buffer_.block(received), count);
			if (!bytes) break;
			received += bytes;
			yield();
		}
		file.close();
		uint8_t signature[8]; tile_buffer_.read(0, signature, 8);
		bool valid = received == size && !memcmp(signature, "\x89PNG\r\n\x1a\n", 8);
		bool ended = false, idat = false;
		// Reject truncated/malformed chunk framing before entering the decoder.
		for (size_t offset = 8; valid && offset < size;) {
			if (size - offset < 12) { valid = false; break; }
			const uint32_t length = (uint32_t(tile_buffer_.at(offset)) << 24) | (uint32_t(tile_buffer_.at(offset + 1)) << 16) |
				(uint32_t(tile_buffer_.at(offset + 2)) << 8) | tile_buffer_.at(offset + 3);
			if (length > size - offset - 12) { valid = false; break; }
			uint8_t tag[4]; tile_buffer_.read(offset + 4, tag, 4);
			if (!memcmp(tag, "IDAT", 4)) idat = true;
			if (!memcmp(tag, "IEND", 4)) {
				ended = length == 0 && offset + 12 == size; break;
			}
			offset += length + 12;
		}
		valid = valid && ended && idat;
		int code = PNG_INVALID_FILE;

		if (valid && png_) {
			decode_owner() = this;
			code = png_->open(path, open_file, close_file, read_file, seek_file, draw_line);
			valid = code == PNG_SUCCESS && png_->getWidth() == 256 && png_->getHeight() == 256;
			if (valid) {
				tile_x_ = x; tile_y_ = y;
				tft_.setViewport(0, 32, 480, 268, false);
				tft_.setSwapBytes(false);
				decode_started_ = millis();
				code = png_->decode(this, PNG_CHECK_CRC);
				valid = code == PNG_SUCCESS;
				tft_.resetViewport();
			}
			png_->close();
		} else valid = false;
		delete png_; png_ = nullptr;
		tile_buffer_.clear();
		decode_owner() = nullptr;
		Serial.printf("MAP decode end: code=%d valid=%d read=%u/%u elapsed=%lu\n", code, valid,
			      static_cast<unsigned>(received), static_cast<unsigned>(size),
			      static_cast<unsigned long>(millis() - started));
		if (!valid && !decoder_failed_) {
			status_ = "Invalid cached tile / retry";
			if (SD.exists(path)) SD.remove(path);
		}
		return valid;
	}
	void overlay(uint32_t now)
	{
		tft_.setViewport(0, 32, 480, 268, false);
		if (centered_) {
			int previous_x = 0, previous_y = 0;
			for (size_t n = 0; n < count_; ++n) {
				const Point &p = points_[(next_ + capacity - count_ + n) % capacity];
				const int x = 240 + static_cast<int>(MapMath::delta_x(p.pixel.x, center_.x, zoom_));
				const int y = 166 + static_cast<int>(p.pixel.y - center_.y);
				if (n && !p.gap && abs(x) < 10000 && abs(y) < 10000 &&
				    abs(previous_x) < 10000 && abs(previous_y) < 10000) {
					tft_.drawLine(previous_x, previous_y, x, y, TFT_RED);
					tft_.drawLine(previous_x, previous_y + 1, x, y + 1, TFT_RED);
				}
				previous_x = x; previous_y = y;
			}
			const int x = 240 + static_cast<int>(MapMath::delta_x(position_.x, center_.x, zoom_));
			const int y = 166 + static_cast<int>(position_.y - center_.y);
			tft_.fillCircle(x, y, 7, TFT_WHITE);
			tft_.fillCircle(x, y, 5, now - last_fix_ms_ <= 2500 ? TFT_BLUE : TFT_ORANGE);
		}
		tft_.resetViewport();
		// Header/footer backgrounds are drawn once, not every 200 ms.
		if (!chrome_drawn_) {
			tft_.fillRect(0, 0, 480, 32, TFT_BLACK);
			tft_.fillRect(0, 300, 480, 20, TFT_BLACK);
			chrome_drawn_ = true;
		}
		tft_.setTextSize(1); tft_.setTextDatum(TL_DATUM);
		tft_.setTextColor(TFT_WHITE, TFT_BLACK);
		tft_.drawString("PALACITA 135 / MAP", 10, 7, 2);
		tft_.drawString("(c) OpenStreetMap contributors", 8, 306, 1);
		if (!have_fix_) tft_.drawString("Waiting for GPS position", 80, 150, 2);
		else if (decoder_failed_) tft_.drawString("Map decoder: insufficient RAM", 70, 150, 2);
	}
	TFT_eSPI &tft_;
	PNG *png_ = nullptr;
	uint32_t decode_started_ = 0, decode_retry_ms_ = 0;
	TileBuffer tile_buffer_;
	File download_file_;
	String temporary_path_;
	size_t download_bytes_ = 0;
	bool download_ok_ = false;
	const char *cache_error_ = "";
	uint8_t prefix_[8] = {};
	size_t prefix_count_ = 0;
	QueueHandle_t requests_ = nullptr, results_ = nullptr;
	static constexpr size_t capacity = 512;
	Point points_[capacity] = {};
	size_t next_ = 0, count_ = 0;
	MapMath::Pixel position_{}, center_{};
	uint32_t last_fix_ms_ = 0, last_sweep_ms_ = 0, last_overlay_ms_ = 0, retry_ms_ = 0;
	int cursor_ = 0, base_x_ = 0, base_y_ = 0, tile_x_ = 0, tile_y_ = 0;
	static constexpr int zoom_ = 16;
	bool active_ = false, have_fix_ = false, centered_ = false, dirty_ = true;
	bool pending_gap_ = false;
	bool decoder_failed_ = false;
	bool busy_ = false, cache_ready_ = false, worker_ready_ = false, requested_ = false;
	bool loaded_[9] = {}, placeholder_[9] = {}, chrome_drawn_ = false;
	uint32_t revision_ = 0;
	const char *status_ = "Waiting for tiles";
	char error_status_[48] = {};
};
