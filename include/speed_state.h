#pragma once

#include <cmath>
#include <stdint.h>

// Single-owner state: call only from the Arduino loop task. Unsigned elapsed
// times handle millis() rollover. Trip is speed integration, not an odometer.
class SpeedState {
public:
	SpeedState(uint32_t stale_ms, double stop_threshold, double maximum)
		: stale_ms_(stale_ms), stop_threshold_(stop_threshold),
		  maximum_(maximum) {}

	void accept(uint32_t now, bool valid, double kmh)
	{
		if (!valid || !std::isfinite(kmh) || kmh < 0 || kmh > maximum_) {
			invalidate();
			return;
		}
		if (kmh < stop_threshold_)
			kmh = 0;
		const uint32_t elapsed = now - last_ms_;
		if (valid_ && elapsed <= stale_ms_)
			trip_km_ += (kmh + speed_kmh_) * 0.5 * elapsed / 3600000.0;
		speed_kmh_ = kmh;
		if (kmh > max_kmh_)
			max_kmh_ = kmh;
		last_ms_ = now;
		valid_ = true;
	}

	void invalidate() { valid_ = false; }
	bool fresh(uint32_t now) const
	{
		return valid_ && now - last_ms_ <= stale_ms_;
	}
	void expire(uint32_t now)
	{
		if (!fresh(now))
			invalidate();
	}
	bool reset_if_stopped(uint32_t now)
	{
		if (!fresh(now) || speed_kmh_ != 0)
			return false;
		trip_km_ = 0;
		max_kmh_ = 0;
		return true;
	}
	double speed() const { return speed_kmh_; }
	double trip() const { return trip_km_; }
	double maximum() const { return max_kmh_; }

private:
	uint32_t stale_ms_;
	double stop_threshold_;
	double maximum_;
	uint32_t last_ms_ = 0;
	bool valid_ = false;
	double speed_kmh_ = 0;
	double trip_km_ = 0;
	double max_kmh_ = 0;
};
