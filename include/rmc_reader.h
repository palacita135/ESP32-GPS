#pragma once

#include <TinyGPS++.h>
#include <cstdlib>
#include <cstring>

#include "speed_state.h"

// Checksum-validated GP/GN RMC speed, with explicit handling of a lost fix.
class RmcReader {
public:
	RmcReader(TinyGPSPlus &gps, SpeedState &speed, bool require_quality = false)
		: gps_(gps), speed_(speed), require_quality_(require_quality), gp_status_(gps, "GPRMC", 2),
		  gp_speed_(gps, "GPRMC", 7), gn_status_(gps, "GNRMC", 2),
		  gn_speed_(gps, "GNRMC", 7) {}

	void feed(char c, uint32_t now)
	{
		if (c == '$')
			commas_ = 0;
		if (c == ',' && commas_ < 255)
			++commas_;
		if (gps_.encode(c)) {
			read(gp_status_, gp_speed_, now);
			read(gn_status_, gn_speed_, now);
		}
	}
	const char *state(uint32_t now) const
	{
		if (!have_rmc_ || now - last_rmc_ > 2500) return "WAIT FIX / RMC";
		return state_;
	}

private:
	void read(TinyGPSCustom &status, TinyGPSCustom &velocity, uint32_t now)
	{
		if (!status.isUpdated())
			return;
		const bool active = strcmp(status.value(), "A") == 0;
		const char *raw = velocity.value();
		char *end = nullptr;
		const double knots = strtod(raw, &end);
		const bool numeric = raw[0] != '\0' && end != raw && *end == '\0';
		have_rmc_ = true; last_rmc_ = now;
		state_ = active && numeric && commas_ >= 9 ? "GPS FIX" : "NO GPS FIX";
		if (require_quality_) {
			const bool good = active && numeric && commas_ >= 9 &&
				gps_.satellites.isValid() && gps_.satellites.age() <= 2500 && gps_.satellites.value() >= 4 &&
				gps_.hdop.isValid() && gps_.hdop.age() <= 2500 && gps_.hdop.hdop() <= 3.0;
			if (!good || (quality_started_ && now - quality_last_ > 2500)) quality_started_ = false;
			if (!good) {
				if (active && numeric && commas_ >= 9) state_ = "GPS LOW QUALITY";
				speed_.invalidate(); return;
			}
			if (!quality_started_) { quality_since_ = now; quality_started_ = true; }
			quality_last_ = now;
			if (now - quality_since_ < 3000) { state_ = "GPS STABILIZING"; speed_.invalidate(); return; }
		}
		// Require through the date field: truncated RMC must not reuse old fields.
		speed_.accept(now, active && numeric && commas_ >= 9, knots * 1.852);
	}

	TinyGPSPlus &gps_;
	SpeedState &speed_;
	bool require_quality_;
	bool quality_started_ = false;
	uint32_t quality_since_ = 0, quality_last_ = 0;
	uint32_t last_rmc_ = 0;
	bool have_rmc_ = false;
	const char *state_ = "WAIT FIX / RMC";
	TinyGPSCustom gp_status_;
	TinyGPSCustom gp_speed_;
	TinyGPSCustom gn_status_;
	TinyGPSCustom gn_speed_;
	uint8_t commas_ = 0;
};
