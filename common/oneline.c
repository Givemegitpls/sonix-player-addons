#include "common/oneline.h"

// The dots are LVGL's work (see the header); all this adds is the one number
// the caller cannot know: the line of the font that is actually drawing.
void oneline_label(lv_obj_t *label) {
	if (label == NULL) {
		return;
	}
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
	const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
	if (font != NULL) {
		lv_obj_set_height(label, lv_font_get_line_height(font));
	}
}
