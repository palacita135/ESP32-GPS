#include <cassert>
#include <cmath>
#include <cstdio>
#include "map_math.h"
int main()
{
	auto origin = MapMath::project(0, 0);
	assert(fabs(origin.x - MapMath::world / 2) < 0.001);
	assert(fabs(origin.y - MapMath::world / 2) < 0.001);
	auto jakarta = MapMath::project(-6.2, 106.8);
	assert(jakarta.x > origin.x && jakarta.y > origin.y);
	auto pole = MapMath::project(90, 0);
	assert(std::isfinite(pole.y) && fabs(pole.y) < 0.01);
	assert(MapMath::wrap_tile(-1) == (1 << MapMath::zoom) - 1);
	assert(MapMath::wrap_tile(1 << MapMath::zoom) == 0);
	assert(fabs(MapMath::delta_x(1, MapMath::world - 1) - 2) < 0.001);
	assert(fabs(MapMath::delta_x(MapMath::world - 1, 1) + 2) < 0.001);
	puts("PASS: Web Mercator, latitude clamp, dateline wrapping");
}
