#pragma once
#include <cmath>
namespace MapMath {
constexpr int zoom = 16;
constexpr double world = 256.0 * (1 << zoom);
struct Pixel { double x, y; };
inline Pixel project(double latitude, double longitude)
{
	const double lat = fmax(-85.05112878, fmin(85.05112878, latitude));
	const double radians = lat * 3.14159265358979323846 / 180.0;
	return {(longitude + 180.0) / 360.0 * world,
		(1.0 - asinh(tan(radians)) / 3.14159265358979323846) / 2.0 * world};
}
inline double delta_x(double x, double center)
{
	double delta = x - center;
	if (delta > world / 2) delta -= world;
	if (delta < -world / 2) delta += world;
	return delta;
}
inline int wrap_tile(int x)
{
	const int count = 1 << zoom;
	return (x % count + count) % count;
}
}
