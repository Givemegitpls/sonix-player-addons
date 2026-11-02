#ifndef GUI_FLAPPYBIRD_H
#define GUI_FLAPPYBIRD_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Flappy Bird, an addon (addons/flappybird).
//
// The artwork is read from usr/resource/sonix/gui/flappybird when the game is
// opened and freed when it is left: nothing of it stays in memory in between.
// The best score is kept in the config, [flappybird] best, and whether the
// game plays its own sounds -- stopping the music -- in [flappybird] sound.
//
// The screen is created by the addon's init(), which is why this file no
// longer makes it: an addon owns what it builds.

extern lv_obj_t *flappybird_screen;

void flappybird_init(gui_config_t *cfg);

// Where the artwork (and the sounds, in sounds/) is. The addon calls this in
// init() with its own folder on the card, so that the pictures travel with the
// game; without it they are read from the resource tree in the image.
void flappybird_set_assets(const char *dir);
const char *flappybird_assets_dir(void);

// The best score and the sound switch, in the same folder (fstate.h).
void flappybird_set_home(const char *dir);

// Loads the artwork and puts the game on its title screen. It does NOT switch to
// the screen: the addon's init() returns it and the shell does the switching.
// That is not a style choice -- the status bar is hidden from the screen's own
// SCREEN_LOADED event, and a second switch_screen() to the screen already up is
// a no-op in LVGL (lv_display.c returns before loading), so the bar would be
// shown by the switcher and never hidden again. False, with a pop-up, when a
// file is missing.
bool flappybird_load(void);

// The library is being unmapped: the mixer thread joined and the PCM closed, the
// artwork and the frame freed, the tick timer forgotten by the power manager and
// deleted, every pointer into the screen dropped. Flappy Bird registers no theme
// hook. The screen itself is the shell's to delete.
//
// True when the threads are out -- the mixer's join is the only wait, and it
// always finishes. False is reserved for a worker the player cannot interrupt,
// which would keep the library mapped (addon.h).
bool flappybird_stop(void);

#endif /* GUI_FLAPPYBIRD_H */
