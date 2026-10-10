#pragma once

#include <cmath>
#include <stdint.h>

// Single-owner state: call only from the Arduino loop task. Unsigned elapsed
// times handle millis() rollover. Trip is speed integration, not an odometer.
class SpeedState {
public:
	SpeedState(uint32_t stale_ms, double stop_threshold, double maximum,
		   bool filter = false)
		: stale_ms_(stale_ms), stop_threshold_(stop_threshold),
		  maximum_(maximum), filter_(filter) {}

	void accept(uint32_t now, bool valid, double kmh)
	{
		if (!valid || !std::isfinite(kmh) || kmh < 0 || kmh > maximum_) {
			invalidate();
			return;
		}
		const uint32_t elapsed = now - last_ms_;
		if (filter_) {
			if (!valid_ || elapsed > stale_ms_) {
				for (double &sample : samples_) sample = 0;
				sample_index_ = 0;
				moving_ = false;
				start_pending_ = false;
			}
			samples_[sample_index_] = kmh;
			sample_index_ = (sample_index_ + 1) % 3;
			const double a = samples_[0], b = samples_[1], c = samples_[2];
			const double median = a + b + c - fmin(a, fmin(b, c)) - fmax(a, fmax(b, c));
			// Do not delay the overspeed warning behind the median window.
			if (kmh < 180.0) kmh = median;
			const double start_threshold = fmax(5.0, stop_threshold_ + 1.0);
			if (!moving_) {
				if (kmh >= 180.0) moving_ = true;
				else if (kmh >= start_threshold) {
					if (!start_pending_) { start_since_ = now; start_pending_ = true; }
					if (now - start_since_ >= 3000) moving_ = true;
				} else start_pending_ = false;
			}
			if (moving_ && kmh < stop_threshold_) moving_ = false;
			if (!moving_) kmh = 0;
			else {
				start_pending_ = false;
				// Smooth while already moving; startup and warning stay explicit.
				if (valid_ && speed_kmh_ > 0 && elapsed <= stale_ms_ && kmh < 180.0)
					kmh = speed_kmh_ + 0.5 * (kmh - speed_kmh_);
			}
		}
		if (kmh < stop_threshold_)
			kmh = 0;
		if (valid_ && elapsed <= stale_ms_)
			trip_km_ += (kmh + speed_kmh_) * 0.5 * elapsed / 3600000.0;
		speed_kmh_ = kmh;
		if (kmh > max_kmh_)
			max_kmh_ = kmh;
		last_ms_ = now;
		valid_ = true;
	}

	void invalidate() { valid_ = false; speed_kmh_ = 0; start_pending_ = false; }
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
	bool filter_;
	bool moving_ = false;
	bool start_pending_ = false;
	uint32_t start_since_ = 0;
	double samples_[3] = {};
	uint8_t sample_index_ = 0;
	double speed_kmh_ = 0;
	double trip_km_ = 0;
	double max_kmh_ = 0;
};
