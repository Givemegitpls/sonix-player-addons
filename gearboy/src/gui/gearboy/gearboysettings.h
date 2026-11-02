#ifndef GEARBOYSETTINGS_H
#define GEARBOYSETTINGS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Gearboy settings: Game Boy palette, shader (the real panel's pixel grid),
// GBC colour correction, boot ROM at startup. Opened from the Gearboy page's
// gear icon and from the in-game menu's settings entry; changes take effect at
// once, including mid-game, except boot ROMs which apply from the next game.
extern lv_obj_t *gearboysettings_screen;

void gearboysettings_init(gui_config_t *cfg);

// Shows the page, telling the loader about its screen first (it is not init()'s,
// and an unload while it is up must move the display off it).
void gearboysettings_open(void);

// The library is being unmapped: withdraws the theme hook and drops the
// pointers into the screen. The screen itself is deleted by the addon (it made
// it); the settings are on the card and are written as they are picked.
void gearboysettings_stop(void);

#endif /* GEARBOYSETTINGS_H */
