#pragma once

#include <stdint.h>
#include <cstring>

// Bold, forward-slanted, chamfered seven-segment numerals. Vector drawing
// avoids a large font bitmap and works with palette sprites or direct TFT.
namespace RacingDigits {
constexpr int width = 80;
constexpr int height = 144;
constexpr int advance = 94;
constexpr int slant = 20;

template<typename Display>
void bar(Display &canvas, int origin_x, int origin_y, int x, int y,
	 int w, int h, uint16_t color)
{
	const int bevel = 5;
	const int px[] = {x + bevel, x + w - bevel, x + w, x + w - bevel, x + bevel, x};
	const int py[] = {y, y, y + h / 2, y + h, y + h, y + h / 2};
	// Vertical bars use a bevel at the top/bottom rather than their middle.
	int vx[6], vy[6];
	for (int i = 0; i < 6; ++i) {
		int local_x = px[i], local_y = py[i];
		if (h > w) {
			const int vertical_x[] = {x + w / 2, x + w, x + w, x + w / 2, x, x};
			const int vertical_y[] = {y, y + bevel, y + h - bevel, y + h, y + h - bevel, y + bevel};
			local_x = vertical_x[i];
			local_y = vertical_y[i];
		}
		vx[i] = origin_x + local_x + (height - local_y) * slant / height;
		vy[i] = origin_y + local_y;
	}
	for (int i = 1; i < 5; ++i)
		canvas.fillTriangle(vx[0], vy[0], vx[i], vy[i], vx[i + 1], vy[i + 1], color);
}

template<typename Display>
void draw(Display &canvas, const char *text, int center_x, int center_y, uint16_t color)
{
	const int count = static_cast<int>(strlen(text));
	const int total = count ? (count - 1) * advance + width + slant : 0;
	const int left = center_x - total / 2;
	const int top = center_y - height / 2;
	const uint8_t masks[] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
	for (int i = 0; i < count; ++i) {
		if (text[i] < '0' || text[i] > '9')
			continue;
		const uint8_t mask = masks[text[i] - '0'];
		const int x = left + i * advance;
		if (mask & 0x01) bar(canvas, x, top, 8, 0, 64, 16, color);
		if (mask & 0x02) bar(canvas, x, top, 64, 10, 16, 59, color);
		if (mask & 0x04) bar(canvas, x, top, 64, 75, 16, 59, color);
		if (mask & 0x08) bar(canvas, x, top, 8, 128, 64, 16, color);
		if (mask & 0x10) bar(canvas, x, top, 0, 75, 16, 59, color);
		if (mask & 0x20) bar(canvas, x, top, 0, 10, 16, 59, color);
		if (mask & 0x40) bar(canvas, x, top, 8, 64, 64, 16, color);
	}
}
} // namespace RacingDigits
