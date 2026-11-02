#ifndef GUI_GEARBOYPAGE_H
#define GUI_GEARBOYPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The game list: whatever is in Games/GB and Games/GBC on the card, titled
// from the database (see src/system/gbdb.h) rather than from the filename.

extern lv_obj_t *gearboypage_screen;

void gearboypage_init(gui_config_t *cfg);

// The library is being unmapped: waits (bounded, loudly) for a scan still
// reading the card, frees the catalogue, withdraws the theme hook and drops the
// pointers into the screen. gearboypage_screen is the shell's to delete.
//
// False when the scan thread is still inside the card: the library must stay
// mapped then (addon.h).
bool gearboypage_stop(void);

#endif /* GUI_GEARBOYPAGE_H */
