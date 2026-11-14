#pragma once

#include <stdint.h>

#include "gfx.h"

// The one frame the game draws into, and the hand-over to the interface thread.
//
// The game keeps its own loop -- upstream's, with its sleeps and its pacing --
// and runs on a thread of its own, the way Gearboy's core does. It draws into
// `surface()`, and when a frame is finished, CanvasImpl::repaint() snapshots it
// and asks the interface thread to take it. So there are three buffers of 480x320
// RGB565 (300 KB each): the one being drawn, the snapshot, and the one LVGL reads.
//
//   game thread          surface()                the frame being drawn
//   game thread          surface_hand_over()     copy into the snapshot
//   interface thread     surface_copy(dst)       snapshot into the LVGL image
//
// The copies are half a megabyte each a frame, which at the game's 30 steps a
// second is 18 MB/s of memcpy -- a tenth of what the panel's own scaling costs.

void surface_init(void);
void surface_finish(void);

gfx_target *surface(void);
int surface_width(void);
int surface_height(void);

// Called on the game thread when a frame is complete. `dim` darkens it as it goes
// by -- the game's own menus are black on white, and a menu read in a dark room is
// a lamp; the multiply costs nothing extra because the copy is happening anyway.
void surface_hand_over(bool dim);

// Called on the interface thread: the last complete frame into `dst`, which must
// be surface_width() * surface_height() RGB565 pixels.
void surface_copy(uint16_t *dst);
