#pragma once
#include <TFT_eSPI.h>
#include <PNGdec.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <new>
#include <time.h>
#include "map_math.h"
#include "map_root_ca.h"

// Main loop exclusively owns SD/TFT/PNG/track state. Worker owns HTTPS.
class StreetMap {
	struct Request { int x, y; };
	struct Result { Request tile; uint8_t *data; size_t size; int code; };
	struct Point { MapMath::Pixel pixel; bool gap; };
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
		png_ = active ? new (std::nothrow) PNG : nullptr;
		dirty_ = true;
		if (active) {
			chrome_drawn_ = false;
			tft_.fillScreen(TFT_DARKGREY); // Clear only on page entry.
			++revision_;
		}
	}
	void add(double lat, double lon, bool gap, uint32_t now)
	{
		const MapMath::Pixel previous = position_;
		position_ = MapMath::project(lat, lon);
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
		if (!centered_ || fabs(MapMath::delta_x(position_.x, center_.x)) > 64 ||
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
			busy_ = false;
			retry_ms_ = now + 30000;
			if (result.code == 200 && result.data && cache_ready_ &&
			    result.size >= 24 && !memcmp(result.data, "\x89PNG\r\n\x1a\n", 8)) {
				char path[64]; tile_path(path, result.tile);
				String temporary = String(path) + ".part";
				SD.remove(temporary);
				File file = SD.open(temporary, FILE_WRITE);
				if (file && file.write(result.data, result.size) == result.size) {
					file.close();
					SD.remove(path);
					if (SD.rename(temporary.c_str(), path)) {
						status_ = "OSM tiles"; retry_ms_ = now + 2000;
					} else status_ = "Tile cache rename failed";
				} else {
					file.close(); status_ = "Tile cache write failed";
				}
			} else {
				snprintf(error_status_, sizeof(error_status_), "Tile error %d / retry 30s", result.code);
				status_ = error_status_;
			}
			Serial.printf("MAP tile %d/%d: code=%d bytes=%u cache=%s\n",
				      result.tile.x, result.tile.y, result.code,
				      static_cast<unsigned>(result.size), status_);
			free(result.data);
			cursor_ = 0;
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
		if (centered_ && png_ && cursor_ < 9) {
			const int slot = cursor_;
			const int tx = base_x_ + cursor_ % 3, ty = base_y_ + cursor_ / 3;
			++cursor_;
			const int px = static_cast<int>(tx * 256.0 - center_.x + 240);
			const int py = static_cast<int>(ty * 256.0 - center_.y + 166);
			if (px < 480 && px + 256 > 0 && py < 300 && py + 256 > 32 &&
			    ty >= 0 && ty < (1 << MapMath::zoom)) {
				Request request{MapMath::wrap_tile(tx), ty};
				char path[64]; tile_path(path, request);
				if (cache_ready_ && SD.exists(path)) {
					if (!loaded_[slot]) loaded_[slot] = draw_tile(path, px, py);
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
							      MapMath::zoom, request.x, request.y,
							      ESP.getFreeHeap(), ESP.getMaxAllocHeap());
					}
				}
				}
			}
		}
		if (cursor_ >= 9 && !busy_ && now - last_sweep_ms_ >= 2000) {
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
		if (active_ && !png_) return "Map decoder: low RAM";
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
			Result result{request, nullptr, 0, -1};
			{
				WiFiClientSecure client;
				client.setCACert(map_root_ca);
				client.setHandshakeTimeout(5);
				HTTPClient http;
				http.setConnectTimeout(5000); http.setTimeout(5000);
				char url[128];
				snprintf(url, sizeof(url), "https://tile.openstreetmap.org/%d/%d/%d.png",
					 MapMath::zoom, request.x, request.y);
				if (http.begin(client, url)) {
					http.setUserAgent("PALACITA135/1.0 (+https://github.com/palacita135/ESP32-GPS)");
					result.code = http.GET();
					if (result.code < 0) {
						char tls_error[160] = {};
						const int tls_code = client.lastError(tls_error, sizeof(tls_error));
						Serial.printf("MAP HTTPS: %s; TLS=%d %s\n",
							      HTTPClient::errorToString(result.code).c_str(), tls_code, tls_error);
					}
					const int length = http.getSize();
					if (result.code == 200 && length > 0 && length <= 49152) {
						result.data = static_cast<uint8_t *>(malloc(length));
						if (result.data) {
							http.getStreamPtr()->setTimeout(5000);
							result.size = http.getStreamPtr()->readBytes(result.data, length);
							if (result.size != static_cast<size_t>(length)) result.code = -2;
						} else result.code = -100; // Tile buffer allocation failed.
					}
					else if (result.code == 200) result.code = -101; // Invalid/oversized body.
					http.end();
				}
			} // Release TLS memory before main loop consumes the buffer.
			xQueueSend(self->results_, &result, portMAX_DELAY);
		}
	}
	static void tile_path(char *path, const Request &request)
	{
		snprintf(path, 64, "/tiles/%d_%d_%d.png", MapMath::zoom, request.x, request.y);
	}
	static void *open_file(const char *path, int32_t *size)
	{
		File *file = new (std::nothrow) File(SD.open(path, FILE_READ));
		if (!file || !*file) { delete file; return nullptr; }
		*size = file->size(); return file;
	}
	static void close_file(void *handle)
	{ auto *file = static_cast<File *>(handle); file->close(); delete file; }
	static int32_t read_file(PNGFILE *pngfile, uint8_t *buffer, int32_t length)
	{
		File *file = static_cast<File *>(pngfile->fHandle);
		const int32_t bytes = file->read(buffer, length);
		pngfile->iPos = file->position();
		return bytes;
	}
	static int32_t seek_file(PNGFILE *pngfile, int32_t position)
	{
		File *file = static_cast<File *>(pngfile->fHandle);
		if (!file->seek(position)) return -1;
		pngfile->iPos = file->position();
		return pngfile->iPos;
	}
	static int draw_line(PNGDRAW *line)
	{
		auto *self = static_cast<StreetMap *>(line->pUser);
		uint16_t pixels[256];
		if (line->iWidth != 256) return 0;
		self->png_->getLineAsRGB565(line, pixels, PNG_RGB565_BIG_ENDIAN, 0xffffffff);
		self->tft_.pushImage(self->tile_x_, self->tile_y_ + line->y, 256, 1, pixels);
		return 1;
	}
	bool draw_tile(const char *path, int x, int y)
	{
		bool valid = false;
		tile_x_ = x; tile_y_ = y;
		tft_.setViewport(0, 32, 480, 268, false);
		if (png_->open(path, open_file, close_file, read_file, seek_file, draw_line) == PNG_SUCCESS) {
			if (png_->getWidth() == 256 && png_->getHeight() == 256) {
				tft_.setSwapBytes(false);
				valid = png_->decode(this, PNG_CHECK_CRC) == PNG_SUCCESS;
			}
			png_->close();
		}
		tft_.resetViewport();
		if (!valid) {
			status_ = "Corrupt tile / retry later";
			SD.remove(path);
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
				const int x = 240 + static_cast<int>(MapMath::delta_x(p.pixel.x, center_.x));
				const int y = 166 + static_cast<int>(p.pixel.y - center_.y);
				if (n && !p.gap && abs(x) < 10000 && abs(y) < 10000 &&
				    abs(previous_x) < 10000 && abs(previous_y) < 10000) {
					tft_.drawLine(previous_x, previous_y, x, y, TFT_RED);
					tft_.drawLine(previous_x, previous_y + 1, x, y + 1, TFT_RED);
				}
				previous_x = x; previous_y = y;
			}
			const int x = 240 + static_cast<int>(MapMath::delta_x(position_.x, center_.x));
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
		else if (!png_) tft_.drawString("Map decoder: insufficient RAM", 70, 150, 2);
	}
	TFT_eSPI &tft_;
	PNG *png_ = nullptr;
	QueueHandle_t requests_ = nullptr, results_ = nullptr;
	static constexpr size_t capacity = 512;
	Point points_[capacity];
	size_t next_ = 0, count_ = 0;
	MapMath::Pixel position_{}, center_{};
	uint32_t last_fix_ms_ = 0, last_sweep_ms_ = 0, last_overlay_ms_ = 0, retry_ms_ = 0;
	int cursor_ = 0, base_x_ = 0, base_y_ = 0, tile_x_ = 0, tile_y_ = 0;
	bool active_ = false, have_fix_ = false, centered_ = false, dirty_ = true;
	bool busy_ = false, cache_ready_ = false, worker_ready_ = false, requested_ = false;
	bool loaded_[9] = {}, placeholder_[9] = {}, chrome_drawn_ = false;
	uint32_t revision_ = 0;
	const char *status_ = "Waiting for tiles";
	char error_status_[48] = {};
};
