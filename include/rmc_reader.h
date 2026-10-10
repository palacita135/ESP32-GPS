#pragma once

#include <TinyGPS++.h>
#include <cstdlib>
#include <cstring>

#include "speed_state.h"

// Checksum-validated GP/GN RMC speed, with explicit handling of a lost fix.
class RmcReader {
public:
	RmcReader(TinyGPSPlus &gps, SpeedState &speed)
		: gps_(gps), speed_(speed), gp_status_(gps, "GPRMC", 2),
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
		// Require through the date field: truncated RMC must not reuse old fields.
		speed_.accept(now, active && numeric && commas_ >= 9, knots * 1.852);
	}

	TinyGPSPlus &gps_;
	SpeedState &speed_;
	TinyGPSCustom gp_status_;
	TinyGPSCustom gp_speed_;
	TinyGPSCustom gn_status_;
	TinyGPSCustom gn_speed_;
	uint8_t commas_ = 0;
};
