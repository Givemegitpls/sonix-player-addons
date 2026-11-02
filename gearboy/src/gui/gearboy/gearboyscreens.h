#ifndef GEARBOYSCREENS_H
#define GEARBOYSCREENS_H

#include "lvgl/lvgl.h"

// The screens this addon puts up, reported to the loader.
//
// Unloading needs the list (addon.h, sonix_addon_own_screen): the shell can only
// move the display off, forget from the back-stack and delete the screens it
// knows about, and on its own it knows just the one init() returned. Flappy Bird
// has that one screen and nothing else; Gearboy has three -- the list, the game
// and the settings page -- and the last two are opened by the addon itself, so
// an unload while one of them is up would otherwise leave a page whose widget
// callbacks point into a library that is about to be unmapped.
//
// They are reported when they are OPENED rather than when they are created.
// init() builds all three, but the loader writes its list from init()'s return
// value once init() has finished (addon.c, lib_open()), so a screen reported
// from inside init() -- other than that one -- is not kept.

// Learns the id the host gave this addon; every later report is addressed by it.
// Called once, at the top of init().
void gearboyscreens_init(const char *addon_id);

// Registers one of the addon's screens with the loader. Cheap and idempotent:
// the loader keeps a set, and reporting the same screen twice does nothing.
void gearboyscreens_own(lv_obj_t *screen);

#endif /* GEARBOYSCREENS_H */
