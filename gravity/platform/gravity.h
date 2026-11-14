#ifndef GRAVITY_H
#define GRAVITY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The addon's own surface, for addon.c: start the game, stop it, and take frames.
//
// The game is upstream's, and upstream's loop sleeps and polls: it has its own
// pacing (30 ms a step) and its own loading sequence (three seconds of logo). So
// it runs on a thread of its own, like Gearboy's core, and hands finished frames
// to the interface thread through gui_post(). Nothing in game/ touches LVGL.
//
// The frame callback runs on the UI thread: copy the frame with
// gravity_frame_copy() and invalidate whatever image shows it there.

// Creates the frame buffers and starts the game thread. `addon_dir` is
// <card>/Addons/gravity: the pictures are read from its assets/, the records are
// kept in a saves/ directory beside them. False when the thread could not start.
bool gravity_run(const char *addon_dir);

// Ends the game thread and waits for it. Safe to call twice, and safe when the
// game never started.
void gravity_quit(void);

bool gravity_running(void);

void gravity_frame_cb_set(void (*cb)(void *), void *user);

// True while one of the game's own menus is up (main, in-game, play). Read on the
// UI thread: the addon shows the shell's back chevron only then -- while riding it
// would sit over the picture for nothing -- and dims the frame.
bool gravity_menu_open(void);

// Called on the UI thread when the game's own loop ends -- the "Exit Game" item
// of its menu is what ends it (Micro::run() returns, the thread is done). The
// addon uses it to go back to the page the tile was tapped on: while the game has
// the panel, nothing in LVGL can be touched, so this is the only way out.
void gravity_exit_cb_set(void (*cb)(void *), void *user);

// The last finished frame, into `dst`: 480x320 RGB565, which is what
// gravity_width()/gravity_height() say. UI thread only.
void gravity_frame_copy(uint16_t *dst);

int gravity_width(void);
int gravity_height(void);

#ifdef __cplusplus
}
#endif

#endif /* GRAVITY_H */
