#pragma once

#include <stdint.h>

// The software drawing the game expects from javax.microedition.lcdui.
//
// Upstream this is SDL: Graphics was a wrapper around an SDL_Renderer, Image an
// SDL_Surface, and CanvasImpl an SDL window. On the card there is no SDL and no
// GPU, so the same calls land here: one RGB565 frame owned by the addon, and
// primitives written into it pixel by pixel. The call set is upstream's own --
// clip, colour, rect, line, arc, image, and the anchor arithmetic -- because the
// game's drawing code is upstream's too and is not being rewritten.
//
// Coordinates are the game's: 480x320, the panel's own pixels with the player
// held sideways. Nothing here scales; the frame is handed to LVGL as an image
// and the shell does the rest (see gravity/addon.c).
//
// Every primitive clips to the current clip rectangle, which is what upstream's
// SDL clip did and what the game relies on: sprites are cut out of an atlas by
// setting the clip to the sprite and drawing the whole atlas offset. A blit
// therefore touches only the intersection of the image and the clip, never the
// whole atlas.

typedef struct gfx_target {
	uint16_t *px;
	int w, h;
	int clip_x1, clip_y1, clip_x2, clip_y2; // inclusive
	uint16_t color;				// RGB565
} gfx_target;

void gfx_init(gfx_target *t, uint16_t *px, int w, int h);

// setColor(255, 255, 255) and friends: 8-bit channels in, RGB565 out.
void gfx_set_color(gfx_target *t, int r, int g, int b);

void gfx_set_clip(gfx_target *t, int x, int y, int w, int h);
void gfx_clip_reset(gfx_target *t);

void gfx_fill_rect(gfx_target *t, int x, int y, int w, int h);
void gfx_draw_pixel(gfx_target *t, int x, int y);
void gfx_draw_line(gfx_target *t, int x1, int y1, int x2, int y2);
void gfx_fill_arc(gfx_target *t, int x, int y, int w, int h, int start_angle, int arc_angle);
void gfx_draw_arc(gfx_target *t, int x, int y, int w, int h, int start_angle, int arc_angle);

// An image, its upper-left corner at (x, y). `mask` is one byte per pixel and
// may be NULL for a drawing with no transparent pixel.
void gfx_blit(gfx_target *t, const uint16_t *px, const uint8_t *mask, int w, int h, int x, int y);

// 8-bit coverage text: one glyph, blended with the current colour.
void gfx_glyph(gfx_target *t, const uint8_t *coverage, int w, int h, int x, int y);
