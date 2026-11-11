// t2048.h -- 2048 on the More page.
//
// Identifiers cannot start with a digit, so the sources are t2048.*; the addon
// folder and the tile title stay "2048".

#ifndef GUI_T2048_H
#define GUI_T2048_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *t2048_screen;

void t2048_init(gui_config_t *cfg);
void t2048_stop(void);

#endif /* GUI_T2048_H */
