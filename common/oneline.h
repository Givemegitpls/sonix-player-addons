#ifndef ADDON_ONELINE_H
#define ADDON_ONELINE_H

#include "lvgl/lvgl.h"

// A label on one line, cut with dots.
//
// LVGL has this itself: LV_LABEL_LONG_MODE_DOTS measures the text with the font
// that is going to draw it, cuts it where the width runs out and writes "..."
// in place of the rest. So there is no character count to guess -- a fixed one
// is wrong in both directions, because this interface's faces are proportional
// (measured on the cassette's Caveat at 22 px: 38 lower-case letters fit the
// 300 px label, 26 capitals, and "The Weeknd, Playboi Carti & Madonna" is 35
// characters at 297 px).
//
// Two things make the mode work, and they are why this is a function rather
// than one lv_label_set_long_mode() at every call site:
//
// - the width has to be fixed -- an lv_pct(100) inside a sized parent is
//   enough, LV_SIZE_CONTENT is not, there is nothing to cut against;
// - the height has to be exactly one line, or the mode wraps to a second line
//   instead of cutting. That height belongs to the font, and a page that
//   guessed it would either cut descenders or leave half a line of paper.
//
// Call it after the label's font and width are set. A label whose font changes
// afterwards (a size setting, a fallback face) has to be passed here again.
void oneline_label(lv_obj_t *label);

#endif // ADDON_ONELINE_H
