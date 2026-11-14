#include "gfx.h"

#include <math.h>
#include <string.h>

#define PI_CONV (3.1415926 / 180.0)

static inline uint16_t pack565(int r, int g, int b) {
	return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

void gfx_init(gfx_target *t, uint16_t *px, int w, int h) {
	t->px = px;
	t->w = w;
	t->h = h;
	t->color = pack565(0, 0, 0);
	gfx_clip_reset(t);
}

void gfx_clip_reset(gfx_target *t) {
	t->clip_x1 = 0;
	t->clip_y1 = 0;
	t->clip_x2 = t->w - 1;
	t->clip_y2 = t->h - 1;
}

void gfx_set_color(gfx_target *t, int r, int g, int b) { t->color = pack565(r, g, b); }

void gfx_set_clip(gfx_target *t, int x, int y, int w, int h) {
	int x2 = x + w - 1, y2 = y + h - 1;
	t->clip_x1 = x < 0 ? 0 : (x > t->w ? t->w : x);
	t->clip_y1 = y < 0 ? 0 : (y > t->h ? t->h : y);
	t->clip_x2 = x2 < 0 ? -1 : (x2 >= t->w ? t->w - 1 : x2);
	t->clip_y2 = y2 < 0 ? -1 : (y2 >= t->h ? t->h - 1 : y2);
}

void gfx_draw_pixel(gfx_target *t, int x, int y) {
	if (x < t->clip_x1 || x > t->clip_x2 || y < t->clip_y1 || y > t->clip_y2) {
		return;
	}
	t->px[(size_t)y * (size_t)t->w + (size_t)x] = t->color;
}

void gfx_fill_rect(gfx_target *t, int x, int y, int w, int h) {
	if (w <= 0 || h <= 0) {
		return;
	}
	int x1 = x, y1 = y, x2 = x + w - 1, y2 = y + h - 1;
	if (x1 < t->clip_x1) {
		x1 = t->clip_x1;
	}
	if (y1 < t->clip_y1) {
		y1 = t->clip_y1;
	}
	if (x2 > t->clip_x2) {
		x2 = t->clip_x2;
	}
	if (y2 > t->clip_y2) {
		y2 = t->clip_y2;
	}
	if (x1 > x2 || y1 > y2) {
		return;
	}

	uint16_t c = t->color;
	for (int row = y1; row <= y2; row++) {
		uint16_t *dst = t->px + (size_t)row * (size_t)t->w + (size_t)x1;
		int n = x2 - x1 + 1;
		// Pairs, so a run of solid colour goes in two pixels per store.
		if ((((uintptr_t)dst) & 2) && n > 1) {
			*dst++ = c;
			n--;
		}
		uint32_t pair = (uint32_t)c | ((uint32_t)c << 16);
		uint32_t *d32 = (uint32_t *)dst;
		for (int i = n / 2; i > 0; i--) {
			*d32++ = pair;
		}
		if (n & 1) {
			*(uint16_t *)d32 = c;
		}
	}
}

void gfx_draw_line(gfx_target *t, int x1, int y1, int x2, int y2) {
	int dx = x2 - x1, dy = y2 - y1;
	int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
	if (dx < 0) {
		dx = -dx;
	}
	if (dy < 0) {
		dy = -dy;
	}
	int err = dx - dy;

	for (;;) {
		gfx_draw_pixel(t, x1, y1);
		if (x1 == x2 && y1 == y2) {
			break;
		}
		int e2 = 2 * err;
		if (e2 > -dy) {
			err -= dy;
			x1 += sx;
		}
		if (e2 < dx) {
			err += dx;
			y1 += sy;
		}
	}
}

// Upstream's fillArc, kept arithmetic and all: it walks the bounding box, asks
// the angle and the squared distance of every pixel, and paints the ones inside
// the arc. The wheels are drawn with it, and a wheel is 20 pixels across, so the
// cost of the atan2 per pixel is a few hundred thousand cycles a frame -- visible
// in the profile, small enough to leave alone until the rest works.
static int to_360(int ang) {
	if (ang >= 0 && ang <= 360) {
		return ang;
	}
	if (ang < 0) {
		ang += 360;
	}
	return ang;
}

void gfx_fill_arc(gfx_target *t, int x, int y, int w, int h, int start_angle, int arc_angle) {
	int end_angle = start_angle + arc_angle;
	double a = w / 2.0, b = h / 2.0;
	if (a <= 0 || b <= 0) {
		return;
	}
	double e = sqrt(1.0 - (b * b) / (a * a));

	for (int py = (int)(y - b); py < (int)(y + b); py++) {
		for (int px = (int)(x - a); px < (int)(x + a); px++) {
			// Screen y grows downwards, the maths does not.
			double ang = atan2(-(py - y), px - x);
			double rad = b * b / (1 - e * e * cos(ang) * cos(ang));
			double dist = ((px - x) * (px - x) + (py - y) * (py - y));
			int ang2 = to_360((int)(ang / PI_CONV));

			if (ang2 >= to_360(start_angle) && ang2 <= to_360(end_angle) && dist <= rad) {
				gfx_draw_pixel(t, px, py);
			}
			if (end_angle > 360 && ang2 < end_angle % 360 && dist <= rad) {
				gfx_draw_pixel(t, px, py);
			}
		}
	}
}

void gfx_draw_arc(gfx_target *t, int x, int y, int w, int h, int start_angle, int arc_angle) {
	int xradius = w / 2, yradius = h / 2;
	x += xradius;
	y += yradius;
	if (xradius == 0 && yradius == 0) {
		return;
	}

	for (int angle = start_angle; angle < start_angle + arc_angle; angle++) {
		gfx_draw_line(t, x + (int)(xradius * cos(angle * PI_CONV)), y - (int)(yradius * sin(angle * PI_CONV)),
					  x + (int)(xradius * cos((angle + 1) * PI_CONV)), y - (int)(yradius * sin((angle + 1) * PI_CONV)));
	}
}

void gfx_blit(gfx_target *t, const uint16_t *px, const uint8_t *mask, int w, int h, int x, int y) {
	int sx1 = 0, sy1 = 0, sx2 = w - 1, sy2 = h - 1;

	if (x + sx1 < t->clip_x1) {
		sx1 = t->clip_x1 - x;
	}
	if (y + sy1 < t->clip_y1) {
		sy1 = t->clip_y1 - y;
	}
	if (x + sx2 > t->clip_x2) {
		sx2 = t->clip_x2 - x;
	}
	if (y + sy2 > t->clip_y2) {
		sy2 = t->clip_y2 - y;
	}
	if (sx1 < 0) {
		sx1 = 0;
	}
	if (sy1 < 0) {
		sy1 = 0;
	}
	if (sx2 > w - 1) {
		sx2 = w - 1;
	}
	if (sy2 > h - 1) {
		sy2 = h - 1;
	}
	if (sx1 > sx2 || sy1 > sy2) {
		return;
	}

	int dx = x + sx1;
	for (int sy = sy1; sy <= sy2; sy++) {
		const uint16_t *src = px + (size_t)sy * (size_t)w + (size_t)sx1;
		uint16_t *dst = t->px + (size_t)(y + sy) * (size_t)t->w + (size_t)dx;
		int n = sx2 - sx1 + 1;

		if (!mask) {
			memcpy(dst, src, (size_t)n * sizeof(uint16_t));
			continue;
		}

		const uint8_t *m = mask + (size_t)sy * (size_t)w + (size_t)sx1;
		// Whole transparent runs are skipped, which is what makes cutting a
		// sprite out of a 120x120 atlas cheaper than copying the atlas.
		for (int i = 0; i < n; i++) {
			if (m[i]) {
				dst[i] = src[i];
			}
		}
	}
}

void gfx_glyph(gfx_target *t, const uint8_t *coverage, int w, int h, int x, int y) {
	int r = ((t->color >> 11) & 0x1f) << 3, g = ((t->color >> 5) & 0x3f) << 2, b = (t->color & 0x1f) << 3;

	for (int gy = 0; gy < h; gy++) {
		int py = y + gy;
		if (py < t->clip_y1 || py > t->clip_y2) {
			continue;
		}
		const uint8_t *cov = coverage + (size_t)gy * (size_t)w;
		uint16_t *dst = t->px + (size_t)py * (size_t)t->w;

		for (int gx = 0; gx < w; gx++) {
			int px = x + gx;
			if (px < t->clip_x1 || px > t->clip_x2) {
				continue;
			}
			int a = cov[gx];
			if (!a) {
				continue;
			}
			if (a == 255) {
				dst[px] = t->color;
				continue;
			}
			// Blend towards the ink: the coverage is the ink's alpha.
			uint16_t under = dst[px];
			int ur = ((under >> 11) & 0x1f) << 3, ug = ((under >> 5) & 0x3f) << 2, ub = (under & 0x1f) << 3;
			int nr = (r * a + ur * (255 - a)) / 255;
			int ng = (g * a + ug * (255 - a)) / 255;
			int nb = (b * a + ub * (255 - a)) / 255;
			dst[px] = pack565(nr, ng, nb);
		}
	}
}
