#include <cassert>
#include <cstdio>
#include <string>
#include "track_logger.h"

uint32_t test_now_ms;
static unsigned int map_updates;
static bool map_gap;

static void feed(TrackLogger &logger, const std::string &body, uint32_t now,
		 bool corrupt = false)
{
	unsigned char checksum = 0;
	for (char c : body) checksum ^= c;
	if (corrupt) checksum ^= 1;
	char suffix[8];
	snprintf(suffix, sizeof(suffix), "*%02X\r\n", checksum);
	test_now_ms = now;
	for (char c : "$" + body + suffix) logger.feed(c, now);
}

int main()
{
	TrackLogger logger;
	logger.begin();
	const char *path = "/tracks/TRK00001.csv";
	const std::string rmc = "GPRMC,120000,A,0610.000,S,10649.000,E,10.0,0,101026,,,A";
	feed(logger, "GPGGA,120000,0610.000,S,10649.000,E,1,08,0.9,42.5,M,0,M,,", 900);
	feed(logger, rmc, 1000);
	assert(test_files[path].find("2026-10-10T12:00:00.00Z,106.8166667,-6.1666667,42.50,100,18.520") != std::string::npos);
	const auto first = test_files[path];
	feed(logger, rmc, 1500); // Rate limit.
	feed(logger, rmc, 2000, true); // Bad checksum cannot write.
	feed(logger, "GPRMC,120001,A,,S,10649.000,E,10,0,101026", 2100);
	assert(test_files[path] == first);
	feed(logger, "GPRMC,120002,V,0610.000,S,10649.000,E,10,0,101026", 2200);
	feed(logger, rmc, 3000);
	assert(test_files[path].find("2,2,3000,") != std::string::npos);
	assert(test_files[path].find("-6.1666667,,,18.520") != std::string::npos); // Void invalidates altitude.
	feed(logger, "GNRMC,120003,A,0610.000,S,10649.000,E,10,0,", 4000);
	assert(test_files[path].find("3,2,4000,,106.8166667") != std::string::npos); // No invented UTC.
	logger.poll(7000);
	feed(logger, rmc, 8000);
	assert(test_files[path].find("4,3,8000,") != std::string::npos);
	feed(logger, "GNGGA,235959,0610.000,S,10649.000,E,1,08,0.9,50,M,0,M,,", 8900);
	feed(logger, "GNRMC,235959,A,0610.000,S,10649.000,E,10,0,101026", 9000);
	assert(test_files[path].find("2026-10-10T23:59:59.00Z,106.8166667,-6.1666667,50.00,100") != std::string::npos);
	feed(logger, "GNRMC,000000,A,0610.000,S,10649.000,E,10,0,111026", 10000);
	assert(test_files[path].find("2026-10-11T00:00:00.00Z") != std::string::npos);
	feed(logger, "GNGGA,000001,0610.000,S,10649.000,E,1,08,0.9,,M,0,M,,", 10900);
	feed(logger, rmc, 11000);
	assert(test_files[path].find("7,3,11000,2026-10-10T12:00:00.00Z,106.8166667,-6.1666667,,,18.520") != std::string::npos);
	TrackLogger next_boot;
	next_boot.begin();
	assert(test_files.count("/tracks/TRK00002.csv") == 1);
	test_write_failure = true;
	feed(next_boot, rmc, 12000);
	assert(std::string(next_boot.state()) == "SD ERROR");
	next_boot.on_position([](double latitude, double longitude, bool gap, uint32_t) {
		assert(fabs(latitude + 6.1666667) < 0.000001);
		assert(fabs(longitude - 106.8166667) < 0.000001);
		++map_updates; map_gap = gap;
	});
	feed(next_boot, rmc, 13000);
	assert(map_updates == 1); // Live map survives failed SD logging.
	feed(next_boot, "GPRMC,120002,V,0610.000,S,10649.000,E,10,0,101026", 13500);
	feed(next_boot, rmc, 14000);
	assert(map_updates == 2 && map_gap); // Do not draw across an invalid fix.
	feed(next_boot, rmc, 14500);
	assert(map_updates == 2); // Map sample rate stays bounded.
	puts("Track logger regression tests passed");
}
