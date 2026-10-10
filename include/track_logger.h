#pragma once

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <TinyGPS++.h>
#include <esp_timer.h>
#include <cmath>
#include <cstring>

#include "config.h"

// Single owner: Arduino loop only. CSV needs no closing XML/JSON trailer.
// SD writes/flushes may block; this is not a hard real-time storage device.
class TrackLogger {
public:
	static_assert(!Config::resistive_touch_enabled,
		"Verify touch/SD SPI sharing before enabling touch");
	TrackLogger()
		: spi_(VSPI), gp_status_(gps_, "GPRMC", 2),
		  gn_status_(gps_, "GNRMC", 2), gp_date_(gps_, "GPRMC", 9),
		  gn_date_(gps_, "GNRMC", 9), gp_time_(gps_, "GPRMC", 1),
		  gn_time_(gps_, "GNRMC", 1), gp_alt_(gps_, "GPGGA", 9),
		  gn_alt_(gps_, "GNGGA", 9), gp_quality_(gps_, "GPGGA", 6),
		  gn_quality_(gps_, "GNGGA", 6), gp_units_(gps_, "GPGGA", 10),
		  gn_units_(gps_, "GNGGA", 10) {}

	void begin()
	{
		spi_.begin(18, 19, 23, 5);
		if (!SD.begin(5, spi_, 10000000) || !SD.mkdir("/tracks")) {
			fail("SD mount/directory failed; use a FAT32 card");
			return;
		}
		// Never append to a previous boot's file, even without GPS time.
		for (unsigned int n = 1; n <= 99999; ++n) {
			snprintf(path_, sizeof(path_), "/tracks/TRK%05u.csv", n);
			if (!SD.exists(path_)) {
				file_ = SD.open(path_, FILE_WRITE);
				break;
			}
		}
		if (!file_) {
			fail("Cannot create session file");
			return;
		}
		const char *header = "sequence,segment,uptime_ms,time_utc,longitude,latitude,altitude_m_msl,altitude_age_ms,speed_kmh\n";
		if (file_.print(header) != strlen(header)) {
			fail("SD header write failed");
			return;
		}
		file_.flush();
		state_ = "SD WAIT FIX";
		Serial.printf("Track file: %s\n", path_);
	}

	void feed(char c, uint32_t now)
	{
		if (c == '$') {
			length_ = 0;
			overflow_ = false;
		} else if (c != '\r' && c != '\n') {
			if (length_ + 1 < sizeof(sentence_))
				sentence_[length_++] = c;
			else
				overflow_ = true;
		}
		if (!gps_.encode(c))
			return;
		if (overflow_)
			return;
		sentence_[length_] = '\0';
		char *fields[16] = {sentence_};
		unsigned int count = 1;
		for (char *p = sentence_; *p; ++p) {
			if (*p == '*') {
				*p = '\0';
				break;
			}
			if (*p == ',' && count < 16) {
				*p = '\0';
				fields[count++] = p + 1;
			}
		}
		const bool is_rmc = strcmp(fields[0], "GPRMC") == 0 ||
			strcmp(fields[0], "GNRMC") == 0;
		if (is_rmc && (count < 10 || !coordinate(fields[3], fields[4], 2) ||
		    !coordinate(fields[5], fields[6], 3))) {
			gap_ = true;
			if (file_)
				state_ = "SD WAIT FIX";
			return;
		}
		const bool stamp = is_rmc && strlen(fields[9]) == 6 &&
			digits(fields[9], 6) && digits(fields[1], 6);
		const bool is_gga = strcmp(fields[0], "GPGGA") == 0 ||
			strcmp(fields[0], "GNGGA") == 0;
		if (is_gga && (count < 11 || !fields[9][0] ||
		    strcmp(fields[10], "M") != 0)) {
			alt_valid_ = false;
			return;
		}
		if (is_gga && strcmp(fields[0], "GPGGA") == 0 && gp_quality_.isUpdated())
			altitude(gp_quality_, gp_alt_, gp_units_, now);
		if (is_gga && strcmp(fields[0], "GNGGA") == 0 && gn_quality_.isUpdated())
			altitude(gn_quality_, gn_alt_, gn_units_, now);
		if (is_rmc && strcmp(fields[0], "GPRMC") == 0 && gp_status_.isUpdated())
			record(gp_status_, gp_date_, gp_time_, now, stamp);
		if (is_rmc && strcmp(fields[0], "GNRMC") == 0 && gn_status_.isUpdated())
			record(gn_status_, gn_date_, gn_time_, now, stamp);
	}

	void poll(uint32_t now)
	{
		if (have_point_ && now - last_point_ms_ > Config::stale_ms) {
			gap_ = true;
			if (file_)
				state_ = "SD WAIT FIX";
		}
	}

	const char *state() const { return state_; }

private:
	static bool coordinate(const char *value, const char *hemisphere, size_t degrees)
	{
		if (!digits(value, degrees + 2) || hemisphere[1] != '\0' ||
		    (degrees == 2 && hemisphere[0] != 'N' && hemisphere[0] != 'S') ||
		    (degrees == 3 && hemisphere[0] != 'E' && hemisphere[0] != 'W'))
			return false;
		char *end = nullptr;
		const double nmea = strtod(value, &end);
		return *end == '\0' && std::isfinite(nmea) && nmea >= 0 &&
			fmod(nmea, 100.0) < 60 &&
			nmea <= (degrees == 2 ? 9000.0 : 18000.0);
	}
	void fail(const char *message)
	{
		Serial.printf("TRACK ERROR: %s\n", message);
		if (file_)
			file_.close();
		state_ = "SD ERROR";
	}

	void altitude(TinyGPSCustom &quality, TinyGPSCustom &raw,
		      TinyGPSCustom &units, uint32_t now)
	{
		const char *q = quality.value();
		const char *a = raw.value();
		char *end = nullptr;
		const double value = strtod(a, &end);
		alt_valid_ = q[0] >= '1' && q[0] <= '8' && q[1] == '\0' &&
			a[0] && end != a && *end == '\0' && std::isfinite(value) &&
			strcmp(units.value(), "M") == 0;
		if (alt_valid_) {
			alt_m_ = value;
			alt_ms_ = now;
		}
	}

	static bool digits(const char *s, size_t count)
	{
		if (strlen(s) < count)
			return false;
		for (size_t i = 0; i < count; ++i)
			if (s[i] < '0' || s[i] > '9')
				return false;
		return true;
	}

	void record(TinyGPSCustom &status, TinyGPSCustom &date,
		    TinyGPSCustom &time, uint32_t now, bool stamp)
	{
		const bool active = strcmp(status.value(), "A") == 0;
		const char *d = date.value();
		const char *t = time.value();
		if (!active || !gps_.location.isValid() ||
		    !gps_.location.isUpdated() || gps_.location.age() > 100) {
			gap_ = true;
			alt_valid_ = false;
			if (file_)
				state_ = "SD WAIT FIX";
			return;
		}
		const double lat = gps_.location.lat();
		const double lon = gps_.location.lng();
		if (!file_ || !std::isfinite(lat) || !std::isfinite(lon) ||
		    fabs(lat) > 90 || fabs(lon) > 180)
			return;
		if (have_point_ && now - last_point_ms_ < Config::track_interval_ms)
			return;
		char utc[32] = "";
		if (stamp && strlen(d) == 6 && digits(d, 6) && digits(t, 6) &&
		    gps_.date.isValid() && gps_.time.isValid() &&
		    gps_.date.month() >= 1 && gps_.date.month() <= 12 &&
		    gps_.date.day() >= 1 && gps_.date.day() <= 31 &&
		    gps_.time.hour() < 24 && gps_.time.minute() < 60 &&
		    gps_.time.second() < 60) {
			snprintf(utc, sizeof(utc), "%04u-%02u-%02uT%02u:%02u:%02u.%02uZ",
				 gps_.date.year(), gps_.date.month(), gps_.date.day(),
				 gps_.time.hour(), gps_.time.minute(), gps_.time.second(),
				 gps_.time.centisecond());
		}
		char alt[32] = "", age[16] = "", velocity[24] = "";
		if (alt_valid_ && now - alt_ms_ <= Config::stale_ms) {
			snprintf(alt, sizeof(alt), "%.2f", alt_m_);
			snprintf(age, sizeof(age), "%lu", static_cast<unsigned long>(now - alt_ms_));
		}
		if (gps_.speed.isValid() && gps_.speed.age() <= 100)
			snprintf(velocity, sizeof(velocity), "%.3f", gps_.speed.kmph());
		if (have_point_ && (gap_ || now - last_point_ms_ > Config::stale_ms))
			++segment_;
		char row[224];
		const int length = snprintf(row, sizeof(row), "%lu,%lu,%llu,%s,%.7f,%.7f,%s,%s,%s\n",
			static_cast<unsigned long>(sequence_ + 1),
			static_cast<unsigned long>(segment_),
			static_cast<unsigned long long>(esp_timer_get_time() / 1000),
			utc, lon, lat, alt, age, velocity);
		if (length <= 0 || length >= static_cast<int>(sizeof(row)) ||
		    file_.write(reinterpret_cast<const uint8_t *>(row), length) !=
		    static_cast<size_t>(length)) {
			fail("SD point write failed/full card");
			return;
		}
		// No orderly power-off callback: flush each point to limit buffered loss.
		file_.flush();
		++sequence_;
		last_point_ms_ = now;
		have_point_ = true;
		gap_ = false;
		state_ = "SD RECORDING";
	}

	SPIClass spi_;
	File file_;
	TinyGPSPlus gps_;
	TinyGPSCustom gp_status_, gn_status_, gp_date_, gn_date_, gp_time_, gn_time_;
	TinyGPSCustom gp_alt_, gn_alt_, gp_quality_, gn_quality_, gp_units_, gn_units_;
	char path_[32] = "";
	char sentence_[160] = "";
	size_t length_ = 0;
	bool overflow_ = false;
	const char *state_ = "SD NOT READY";
	uint32_t sequence_ = 0, segment_ = 1, last_point_ms_ = 0, alt_ms_ = 0;
	double alt_m_ = 0;
	bool have_point_ = false, gap_ = false, alt_valid_ = false;
};
