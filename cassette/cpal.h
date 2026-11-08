#ifndef CASSETTE_CPAL_H
#define CASSETTE_CPAL_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// The colours of the page, worked out from the sleeve of what is playing.
//
// One hue from the cover is not enough for a cassette: the real thing has a
// body, an inner panel in a second colour, a paper label, metal hubs and the
// brown tape behind them. The sampler below takes the two colours a sleeve is
// mostly made of and derives the rest, so an orange-and-blue record gives an
// orange body on a blue panel and a black-and-white one still gives something
// to look at.
typedef struct {
	lv_color_t bg;		  // the page behind the cassette
	lv_color_t body;	  // the cassette's shell
	lv_color_t body_dark; // the same, in shadow (the offset under the body)
	lv_color_t panel;	  // the inset face, in the sleeve's second colour
	lv_color_t label;	  // the paper label
	lv_color_t ink;		  // every outline, and the writing on the label
	lv_color_t metal;	  // the hubs and the screws
	lv_color_t accent;	  // the small round things, in the sleeve's other hue
	lv_color_t tape;	  // the tape in the window
} cpal_t;

// For when nothing is playing, or the track has no artwork.
void cpal_default(cpal_t *p);

// Reads the sleeve the player has on screen (RGB565, as cover.c builds it) and
// fills in a palette from it. False -- with *p untouched -- when there is no
// picture or it is not in the format this understands; the caller then uses
// cpal_default().
bool cpal_from_cover(const lv_image_dsc_t *cover, cpal_t *p);

#endif // CASSETTE_CPAL_H
