#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "rmc_reader.h"

uint32_t test_now_ms;

static void feed(RmcReader &reader, const std::string &body, uint32_t now,
		 bool corrupt = false)
{
	unsigned char checksum = 0;
	for (char c : body)
		checksum ^= c;
	if (corrupt)
		checksum ^= 1;
	char suffix[8];
	std::snprintf(suffix, sizeof(suffix), "*%02X\r\n", checksum);
	test_now_ms = now;
	for (char c : "$" + body + suffix)
		reader.feed(c, now);
}

static std::string rmc(const char *talker, const char *status, const char *knots)
{
	return std::string(talker) + "RMC,120000," + status +
		",0610.000,S,10649.000,E," + knots + ",0,101026,,,A";
}

int main()
{
	SpeedState state(2500, 0.8, 300);
	TinyGPSPlus gps;
	RmcReader reader(gps, state);
	assert(!state.fresh(0));
	feed(reader, rmc("GP", "A", "10.0"), 1000);
	assert(state.fresh(1000));
	assert(std::abs(state.speed() - 18.52) < 0.00001);
	feed(reader, rmc("GN", "A", "20.0"), 2000);
	assert(std::abs(state.trip() - 27.78 / 3600) < 0.00001);
	assert(!state.reset_if_stopped(2000));
	feed(reader, rmc("GN", "V", "20.0"), 2100);
	assert(!state.fresh(2100));
	const double trip = state.trip();
	feed(reader, rmc("GP", "A", "10.0"), 3000);
	assert(state.trip() == trip);  // Never bridge a void fix.
	feed(reader, rmc("GP", "A", "90.0"), 4000, true);
	assert(std::abs(state.speed() - 18.52) < 0.00001);
	assert(gps.failedChecksum() == 1);
	assert(state.fresh(5500));
	assert(!state.fresh(5501));
	feed(reader, rmc("GP", "A", "10.0"), 7000);
	assert(state.trip() == trip);  // Never integrate through a stale gap.
	feed(reader, rmc("GP", "A", ""), 7100);
	assert(!state.fresh(7100));
	feed(reader, rmc("GN", "A", "bad"), 7200);
	assert(!state.fresh(7200));
	feed(reader, rmc("GP", "A", "10.0"), 7300);
	feed(reader, "GPRMC,120000,A", 7400);
	assert(!state.fresh(7400));  // Checksum-valid truncated input.
	feed(reader, rmc("GP", "A", "200.0"), 7500);
	assert(!state.fresh(7500));
	feed(reader, rmc("GP", "A", "-1.0"), 7600);
	assert(!state.fresh(7600));
	feed(reader, rmc("GP", "A", "nan"), 7700);
	assert(!state.fresh(7700));
	feed(reader, rmc("GP", "A", "0.1"), 8000);
	assert(state.speed() == 0);
	assert(state.reset_if_stopped(8000));
	assert(state.trip() == 0 && state.maximum() == 0);
	state.accept(0xfffffff0U, true, 36);
	state.accept(984, true, 36);
	assert(std::abs(state.trip() - 0.01) < 0.000001);
	assert(state.fresh(984));
	state.expire(4000);
	assert(!state.fresh(4000));
	assert(!state.reset_if_stopped(4000));
	puts("PASS: real NMEA parser, invalid/stale fixes, trip gaps, reset, rollover");
}
