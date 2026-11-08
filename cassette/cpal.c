#include "cpal.h"

#include <math.h>
#include <string.h>

#include "src/gui/nowplaying/cover.h"

// The page is drawn from one sleeve: its two loudest colour families become the
// shell and the small accents, and the recess the label sits in is black on
// purpose -- the owner asked for it, and it is what makes a cover-coloured shell
// read as a cassette instead of as a coloured bar. The rest (paper, metal,
// tape) is fixed, because a cassette is made of those whatever the record is.

// The brightest the shell is allowed to be, 0..1 in HSV value terms.
#define BODY_MAX_VALUE 0.58f
// ...and a ceiling on its measured brightness, well under the paper label's
// (the sticker is about 194): a shell that is as bright as the label stops
// reading as an object.
#define BODY_MAX_LUMA 148
// The floor, for the other end: a sleeve that is nearly black hands back a tone
// that is nearly black, and on this page the shell then disappears into the
// black around it. The body is a plastic moulding -- it catches some light even
// when the record does not.
#define BODY_MIN_VALUE 0.33f
#define BODY_MIN_LUMA 69

#define INK_R 0x24
#define INK_G 0x1d
#define INK_B 0x16

typedef struct {
	float h, s, v; // h in degrees, s and v in 0..1
} hsv_t;

static hsv_t rgb_to_hsv(int r, int g, int b) {
	float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
	float mx = fmaxf(rf, fmaxf(gf, bf));
	float mn = fminf(rf, fminf(gf, bf));
	float d = mx - mn;
	hsv_t out = {0.0f, mx > 0.0f ? d / mx : 0.0f, mx};
	if (d <= 0.0001f) {
		return out;
	}
	float h;
	if (mx == rf) {
		h = 60.0f * fmodf((gf - bf) / d, 6.0f);
	} else if (mx == gf) {
		h = 60.0f * ((bf - rf) / d + 2.0f);
	} else {
		h = 60.0f * ((rf - gf) / d + 4.0f);
	}
	if (h < 0.0f) {
		h += 360.0f;
	}
	out.h = h;
	return out;
}

static void hsv_to_rgb(hsv_t c, lv_color_t *out) {
	float h = fmodf(c.h, 360.0f);
	if (h < 0.0f) {
		h += 360.0f;
	}
	float s = c.s < 0.0f ? 0.0f : (c.s > 1.0f ? 1.0f : c.s);
	float v = c.v < 0.0f ? 0.0f : (c.v > 1.0f ? 1.0f : c.v);

	float r = v, g = v, b = v;
	float f = h / 60.0f;
	int i = (int)f;
	float p = v * (1.0f - s);
	float q = v * (1.0f - s * (f - (float)i));
	float t = v * (1.0f - s * (1.0f - (f - (float)i)));
	switch (i % 6) {
	case 0: r = v; g = t; b = p; break;
	case 1: r = q; g = v; b = p; break;
	case 2: r = p; g = v; b = t; break;
	case 3: r = p; g = q; b = v; break;
	case 4: r = t; g = p; b = v; break;
	default: r = v; g = p; b = q; break;
	}
	*out = lv_color_make((uint8_t)(r * 255.0f + 0.5f), (uint8_t)(g * 255.0f + 0.5f),
						 (uint8_t)(b * 255.0f + 0.5f));
}

static lv_color_t tone_hsv(hsv_t c) {
	lv_color_t out;
	hsv_to_rgb(c, &out);
	return out;
}


void cpal_default(cpal_t *p) {
	// The cartoon tape of the reference picture: orange shell, black recess.
	p->bg = lv_color_make(0x16, 0x54, 0x3c);
	p->body = lv_color_make(0xe0, 0x7b, 0x3c);
	p->body_dark = lv_color_make(0x9c, 0x52, 0x24);
	p->panel = lv_color_make(0x0f, 0x0d, 0x0b);
	p->label = lv_color_make(0xc5, 0xc2, 0xb1);
	p->ink = lv_color_make(INK_R, INK_G, INK_B);
	p->metal = lv_color_make(0xd6, 0xd8, 0xdc);
	p->accent = lv_color_make(0xfa, 0xce, 0x42);
	p->tape = lv_color_make(0x6d, 0x4c, 0x33);
}

#define HUE_BINS 24

// The player already answers "what colour is this sleeve" -- and it answers the
// same way for its own now-playing page, so the cassette and the player never
// disagree about a record. cover_page_tone() sorts the sleeve into families of
// hue and takes one of them, dark page the darkest and light page the lightest;
// cover_tone_stretched() is that colour at full strength. Both are exported.
bool cpal_from_cover(const lv_image_dsc_t *cover, cpal_t *p) {
	if (cover == NULL || cover->data == NULL || cover->header.cf != LV_COLOR_FORMAT_RGB565) {
		return false;
	}

	cover_image_t img;
	memset(&img, 0, sizeof(img));
	img.dsc = *cover;
	img.pixels = (uint8_t *)(uintptr_t)cover->data;

	uint32_t tone = 0;
	int luma = 0;
	bool light = false;
	if (!cover_page_tone(&img, COVER_PAGE_LIGHT_FROM, &tone, &luma, &light)) {
		return false;
	}
	uint32_t vivid = cover_tone_stretched(tone);

	// The player's colour is the colour. Nothing is mixed into it: what its own
	// page paints is what the cassette is made of. Only a shade of it is worked
	// out, for the chamfers cut out of the label and the edge of the plate.
	hsv_t body_hsv = rgb_to_hsv((int)((tone >> 16) & 0xFF), (int)((tone >> 8) & 0xFF), (int)(tone & 0xFF));

	// A ceiling on how bright the shell may be. A pale sleeve can hand back a
	// tone that is nearly white, and on a backlit panel a white body next to the
	// black recess reads as glowing rather than as plastic. Only the brightness
	// is capped -- the hue and the colour are the player's.
	if (body_hsv.v > BODY_MAX_VALUE) {
		body_hsv.v = BODY_MAX_VALUE;
	}
	if (body_hsv.v < BODY_MIN_VALUE) {
		body_hsv.v = BODY_MIN_VALUE;
	}

	lv_color_t body = tone_hsv(body_hsv);
	int32_t body_luma = (body.red * 299 + body.green * 587 + body.blue * 114) / 1000;
	if (body_luma > BODY_MAX_LUMA) {
		body.red = (uint8_t)(body.red * BODY_MAX_LUMA / body_luma);
		body.green = (uint8_t)(body.green * BODY_MAX_LUMA / body_luma);
		body.blue = (uint8_t)(body.blue * BODY_MAX_LUMA / body_luma);
	} else if (body_luma < BODY_MIN_LUMA) {
		// Lifted, not greyed: the hue is kept and only the brightness comes up,
		// so a black record gives a very dark shell of its own colour.
		int32_t from = body_luma < 6 ? 6 : body_luma;
		body.red = (uint8_t)(body.red * BODY_MIN_LUMA / from);
		body.green = (uint8_t)(body.green * BODY_MIN_LUMA / from);
		body.blue = (uint8_t)(body.blue * BODY_MIN_LUMA / from);
	}
	p->body = body;
	p->body_dark = tone_hsv((hsv_t){body_hsv.h, body_hsv.s, body_hsv.v * 0.70f});
	p->accent = lv_color_make((uint8_t)((vivid >> 16) & 0xFF), (uint8_t)((vivid >> 8) & 0xFF),
							  (uint8_t)(vivid & 0xFF));
	p->bg = tone_hsv((hsv_t){body_hsv.h, body_hsv.s * 0.85f, 0.16f});
	p->panel = lv_color_make(0x0f, 0x0d, 0x0b);
	p->label = lv_color_make(0xc5, 0xc2, 0xb1);
	p->ink = lv_color_make(INK_R, INK_G, INK_B);
	p->metal = lv_color_make(0xd6, 0xd8, 0xdc);
	p->tape = lv_color_make(0x6d, 0x4c, 0x33);
	return true;
}
