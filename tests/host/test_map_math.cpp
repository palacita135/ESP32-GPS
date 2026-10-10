#include <cassert>
#include <cmath>
#include <cstdio>
#include "map_math.h"
#include "tile_buffer.h"
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
	for (int level = 12; level <= 18; ++level) {
		const auto point = MapMath::project(-6.2, 106.8, level);
		const double scale = std::ldexp(1.0, level - MapMath::zoom);
		assert(fabs(point.x - jakarta.x * scale) < 0.001);
		assert(fabs(point.y - jakarta.y * scale) < 0.001);
		assert(MapMath::wrap_tile(-1, level) == (1 << level) - 1);
		assert(fabs(MapMath::delta_x(1, MapMath::world_at(level) - 1, level) - 2) < 0.001);
	}
	TileBuffer buffer;
	assert(buffer.allocate(35241));
	for (size_t i = 0; i < buffer.size(); ++i) *buffer.block(i) = static_cast<uint8_t>(i % 251);
	uint8_t bytes[4096];
	assert(buffer.read(2040, bytes, sizeof(bytes)) == sizeof(bytes));
	for (size_t i = 0; i < sizeof(bytes); ++i) assert(bytes[i] == (2040 + i) % 251);
	assert(buffer.read(35240, bytes, sizeof(bytes)) == 1);
	assert(buffer.read(35241, bytes, 1) == 0);
	assert(!buffer.allocate(49153) && buffer.size() == 0);
	assert(buffer.allocate(49152));
	buffer.clear(); assert(buffer.size() == 0);
	puts("PASS: Web Mercator, wrapping and segmented tile reads across block boundaries");
}
