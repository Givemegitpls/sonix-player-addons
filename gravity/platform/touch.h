#ifndef GRAVITY_TOUCH_H
#define GRAVITY_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The glass as a gamepad: four controls and a menu, in the game's own space.
//
// The game wants keys, not touches: gas is UP, the brake DOWN, and the two leans
// LEFT and RIGHT (GameCanvas's field_230: UP is (+1,0) along the bike, DOWN its
// negative, LEFT and RIGHT the other axis). A bike being ridden needs gas and a
// lean at the same time, which one LVGL pointer cannot express -- so while the
// game runs the panel changes hands the way it does for Gearboy: LVGL's indev is
// switched off and this module reads the real multitouch protocol from the same
// evdev node (src/system/core/panel.h, and gearboy/src/system/gearboy/gbinput.c,
// which this is adapted from).
//
// On the simulator there is no evdev node and no second finger: the page feeds
// contacts in with gtouch_contact(), one pointer, enough to exercise the zones.
//
// Coordinates here are the game's: 480x320, landscape. The glass is 320x480 and
// the frame is shown turned a quarter turn (gravity/addon.c), so a touch is
// mapped back through that turn in one place, gtouch_poll()'s caller having
// nothing to know about it.

#define GD_KEY_UP 0x0001   // gas
#define GD_KEY_DOWN 0x0002 // brake
#define GD_KEY_LEFT 0x0004 // lean back
#define GD_KEY_RIGHT 0x0008 // lean forward
#define GD_KEY_FIRE 0x0010 // the centre strip: select in a menu
#define GD_KEY_MENU 0x0020 // the centre strip, held: the game's own menu

// Takes the panel (false when it cannot: the game then has no controls, which is
// useless but not a crash). Idempotent.
bool gtouch_open(const char *evdev_node);
void gtouch_close(void);
bool gtouch_open_done(void);

// On the simulator: one contact, in the glass's own coordinates, as the player's
// LVGL indev reports them. `id` is the contact's identity; one is enough.
void gtouch_contact(int id, int glass_x, int glass_y, bool down);

// The states the glass has been through since the last call, oldest first, and
// how many of them were written.
//
// A *state*, not an edge, and a queue of them, not the latest: a tap can be
// pressed and released between two polls of the game's loop (which runs at thirty
// a second, and the menu's own loop at twenty), and a mask that is only read once
// per frame loses that tap -- the menu then answers every other touch. Each entry
// is the whole key state at that moment; the caller compares consecutive ones.
//
// `gtouch_poll()` is called from the game's thread. On the device it drains the
// node first, one state per SYN_REPORT; on the simulator the states were queued
// by gtouch_contact(). Returns 0 when nothing changed.
#define GTOUCH_MAX_STATES 32
int gtouch_poll(uint16_t *states, int max);

// The zones, for a caller that wants to draw them (the centre strip is the game's
// menu; see README.txt). In game coordinates.
void gtouch_zone(int index, int *x, int *y, int *w, int *h, uint16_t *keys);
int gtouch_zone_count(void);

// How long the centre strip has to be held before it counts as the game's menu
// rather than a select. A tap selects; a hold opens the menu. The hold is
// evaluated by gtouch_poll(), so a state with GD_KEY_MENU instead of GD_KEY_FIRE
// appears in the queue once the time is up, without the finger moving.
#define GTOUCH_MENU_HOLD_MS 500

#ifdef __cplusplus
}
#endif

#endif /* GRAVITY_TOUCH_H */
