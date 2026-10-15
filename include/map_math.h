#pragma once
#include <cmath>
#include "config.h"
namespace MapMath {
constexpr int zoom = Config::map_zoom;
constexpr double world = 256.0 * (1 << zoom);
struct Pixel { double x, y; };
inline double world_at(int level) { return 256.0 * (1 << level); }
inline Pixel project(double latitude, double longitude, int level = zoom)
{
	const double world = world_at(level);
	const double lat = fmax(-85.05112878, fmin(85.05112878, latitude));
	const double radians = lat * 3.14159265358979323846 / 180.0;
	return {(longitude + 180.0) / 360.0 * world,
		(1.0 - asinh(tan(radians)) / 3.14159265358979323846) / 2.0 * world};
}
inline double delta_x(double x, double center, int level = zoom)
{
	const double world = world_at(level);
	double delta = x - center;
	if (delta > world / 2) delta -= world;
	if (delta < -world / 2) delta += world;
	return delta;
}
inline int wrap_tile(int x, int level = zoom)
{
	const int count = 1 << level;
	return (x % count + count) % count;
}
}
