#ifndef GRAVITY_TOUCH_H
#define GRAVITY_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The player's buttons as a gamepad.
//
// The game wants keys, not touches: gas is UP, the brake DOWN, and the two leans
// LEFT and RIGHT (GameCanvas's field_230: UP is (+1,0) along the bike, DOWN its
// negative, LEFT and RIGHT the other axis). This board's panel reports one contact
// at a time, measured -- see platform/touch.c -- and a bike needs gas and a lean at
// once, so the ride is on the button device instead:
//
//   play/pause      gas
//   next song       brake
//   volume up       lean back
//   volume down     lean forward
//   prev song       the game's own menu
//
// The buttons are grabbed while the game runs, so they do not move the volume, skip
// the track or pause the music. The power button is handed back to the player.
//
// The screen is not a controller any more. It keeps LVGL's indev for two things:
// the shell's back chevron (the way out) and a tap anywhere, which stands in for
// the FIRE key the game's menus need to choose a row (gtouch_fire).

#define GD_KEY_UP 0x0001	// gas
#define GD_KEY_DOWN 0x0002	// brake
#define GD_KEY_LEFT 0x0004	// lean back
#define GD_KEY_RIGHT 0x0008 // lean forward
#define GD_KEY_FIRE 0x0010	// select in a menu: a tap on the glass
#define GD_KEY_MENU 0x0020	// the game's own menu: next song

// Takes the button device (false when it cannot: the game then has no controls,
// which is useless but not a crash). Idempotent.
bool gtouch_open(const char *evdev_node);
void gtouch_close(void);
bool gtouch_open_done(void);

// The states the buttons have been through since the last call, oldest first, and
// how many of them were written.
//
// A *state*, not an edge, and a queue of them, not the latest: a press and release
// can both land between two polls of the game's loop (which runs at thirty a
// second, and the menu's own loop at twenty), and a mask read once per frame
// swallows every other press. Each entry is the whole key state at that moment; the
// caller compares consecutive ones.
//
// Called from the game's thread; it drains the node first. 0 when nothing changed.
#define GTOUCH_MAX_STATES 32
int gtouch_poll(uint16_t *states, int max);

// A tap on the glass, as the game's FIRE key. On the simulator the page binds its
// own keys to gtouch_key() instead.
void gtouch_fire(bool down);

// A lean (or any bit) from outside: the simulator binds keys to it.
void gtouch_key(uint16_t bit, bool down);

#ifdef __cplusplus
}
#endif

#endif /* GRAVITY_TOUCH_H */
