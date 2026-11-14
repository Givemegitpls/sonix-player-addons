// Difficulty picker for Tic Tac Toe — own screen, opened from the gear in the header.

#ifndef GUI_TTTSETTINGS_H
#define GUI_TTTSETTINGS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *tttsettings_screen;

void tttsettings_init(gui_config_t *cfg, const char *addon_id);
void tttsettings_open(void);
void tttsettings_stop(void);

#endif /* GUI_TTTSETTINGS_H */
