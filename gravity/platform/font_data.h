#ifndef GRAVITY_FONT_DATA_H
#define GRAVITY_FONT_DATA_H

#include <stdint.h>

// One glyph of a baked face: 8-bit coverage, ASCII 32..126.
//
// `left` and `top` are the position of the mask's upper-left corner relative to
// the pen (top is negative: above the baseline), `advance` is what the pen moves
// after the glyph, and `off` is the glyph's offset into the face's coverage
// pool. A space has w = h = 0 and only an advance.
typedef struct {
	uint16_t advance;
	int8_t left;
	int8_t top;
	uint8_t w;
	uint8_t h;
	uint32_t off;
} gd_glyph_t;

struct gd_face {
	int size;		// what the game asked FontStorage for: 12, 16 or 32
	int bold;
	int ascent;		// pixels above the baseline
	int descent;		// pixels below it
	int line_height;	// ascent + descent: what a line of this face costs
	const gd_glyph_t *glyphs; // 95 entries, ' ' first
	const uint8_t *coverage;
};

#ifdef __cplusplus
extern "C" {
#endif

// The face for (size, bold), or NULL when the baker did not produce it. The table
// is C (platform/font_data.c), the callers are not.
const struct gd_face *gd_face_lookup(int size, int bold);

#ifdef __cplusplus
}
#endif

#endif /* GRAVITY_FONT_DATA_H */
