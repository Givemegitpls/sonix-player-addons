#include "touch.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// five buttons, no glass
// ---------------------------------------------------------------------------
//
// The panel on this board reports one contact at a time (four dumps with two
// fingers, 2026-11-14: never a second contact in a packet, though the GT9XX's own
// config says `Touch Points = 5`), and a bike being ridden needs gas and a lean at
// once. So the ride lives on the button device, `gpio-keys`, which has exactly the
// five keys this game wants:
//
//   KEY_PLAYPAUSE      gas          (UP)   -- GameCanvas's field_230: (+1,0)
//   KEY_NEXTSONG       brake        (DOWN)
//   KEY_VOLUMEUP       lean back    (LEFT)
//   KEY_VOLUMEDOWN     lean forward (RIGHT)
//   KEY_PREVIOUSSONG   the game's menu (Escape, the BACK command)
//
// and KEY_POWER, which is handed back to the player (power_notify_power_button),
// because a game that swallows the power button is a game you cannot switch off.
//
// The node is *grabbed* while the game runs: the same keys otherwise move the
// volume, skip the track and pause the music, and every one of those puts a popup
// over the picture. A grab is also why every state pushed below notifies the
// player of activity -- with the keys taken away, the panel's own "someone is
// using this" would come from nothing.
//
// The screen keeps LVGL's indev. It is used for two things only: the shell's back
// chevron (the way out of the addon) and a tap, which stands in for the FIRE key
// the game's menus need to choose a row. No zones, no riding on the glass.

#ifndef HOST_BUILD

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "src/gui/shell/powermenu.h"
#include "src/system/device/power.h"

#define KEYS_NODE "/dev/input/event1"

// The player's own long-press deadline (system.c, POWER_LONG_PRESS_MS): a press
// held this long is the power menu, shorter is the screen toggle. It has to be
// reproduced here because the grab means the player never sees the key.
#define POWER_LONG_MS 1200

static int fd = -1;
static bool opened;

// The power key, forwarded by hand: see the note at KEY_POWER below.
static bool power_down;
static bool power_long;
static int64_t power_since;

// The queue of states.
//
// A queue and not the latest answer: the game's menu loop repaints (and therefore
// polls) every 50 ms, and a button pressed and released inside one interval would
// otherwise be lost -- half the presses would do nothing.
static uint16_t queue[GTOUCH_MAX_STATES];
static int queue_len;
static uint16_t keys;

static void push_state(uint16_t state) {
	if (queue_len > 0 && queue[queue_len - 1] == state) {
		return;
	}
	if (queue_len == GTOUCH_MAX_STATES) {
		memmove(queue, queue + 1, (GTOUCH_MAX_STATES - 1) * sizeof(queue[0]));
		queue_len--;
	}
	queue[queue_len++] = state;
	power_notify_activity();
}

static void set_bit(uint16_t bit, bool down) {
	uint16_t was = keys;
	if (down) {
		keys |= bit;
	} else {
		keys = (uint16_t)(keys & ~bit);
	}
	if (keys != was) {
		push_state(keys);
	}
}

bool gtouch_open_done(void) { return opened; }

bool gtouch_open(const char *node) {
	if (opened) {
		return true;
	}
	if (!node || !node[0]) {
		node = KEYS_NODE;
	}
	fd = open(node, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		printf("gravity: %s: %s\n", node, strerror(errno));
		return false;
	}
	if (ioctl(fd, EVIOCGRAB, 1) != 0) {
		printf("gravity: %s: cannot take it: %s\n", node, strerror(errno));
		close(fd);
		fd = -1;
		return false;
	}
	opened = true;
	printf("gravity: keys from %s, grabbed (no volume, no track change)\n", node);
	return true;
}

void gtouch_close(void) {
	if (fd >= 0) {
		ioctl(fd, EVIOCGRAB, 0);
		close(fd);
		fd = -1;
	}
	opened = false;
	queue_len = 0;
	keys = 0;
}

void gtouch_contact(int id, int glass_x, int glass_y, bool down) {
	(void)id;
	(void)glass_x;
	(void)glass_y;
	(void)down;
}

void gtouch_fire(bool down) { set_bit(GD_KEY_FIRE, down); }

void gtouch_key(uint16_t bit, bool down) { set_bit(bit, down); }

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// The long press, which has no event of its own: it is time passing.
static void power_watch(void) {
	if (power_down && !power_long && now_ms() - power_since >= POWER_LONG_MS) {
		power_long = true;
		powermenu_request_show();
	}
}

static void drain(void) {
	struct input_event ev[32];
	for (;;) {
		ssize_t got = read(fd, ev, sizeof(ev));
		if (got < (ssize_t)sizeof(ev[0])) {
			break;
		}
		int count = (int)(got / (ssize_t)sizeof(ev[0]));
		for (int i = 0; i < count; i++) {
			const struct input_event *e = &ev[i];
			if (e->type != EV_KEY) {
				continue;
			}
			bool down = e->value != 0;
			switch (e->code) {
			case KEY_PLAYPAUSE:
				set_bit(GD_KEY_UP, down);
				break;
			case KEY_NEXTSONG:
				set_bit(GD_KEY_DOWN, down);
				break;
			case KEY_VOLUMEUP:
				set_bit(GD_KEY_LEFT, down);
				break;
			case KEY_VOLUMEDOWN:
				set_bit(GD_KEY_RIGHT, down);
				break;
			case KEY_PREVIOUSSONG:
				set_bit(GD_KEY_MENU, down);
				break;
			case KEY_POWER:
				// Not the game's, and not ours to swallow either: the player's
				// power button is a short press (screen on or off) and a long one
				// (its power menu), and the grab means neither reaches it. Both
				// are given back by hand, with the same 1.2 s deadline the
				// player's own system.c uses -- a long press opens the menu, and
				// its release must not also toggle the screen.
				if (down) {
					if (!power_down) {
						power_down = true;
						power_long = false;
						power_since = now_ms();
					}
				} else if (power_down) {
					if (!power_long) {
						power_notify_power_button();
					}
					power_down = false;
				}
				break;
			default:
				break;
			}
		}
	}
}

int gtouch_poll(uint16_t *states, int max) {
	if (!states || max <= 0) {
		return 0;
	}
	drain();
	power_watch();
	int n = queue_len < max ? queue_len : max;
	memcpy(states, queue, (size_t)n * sizeof(states[0]));
	queue_len -= n;
	if (queue_len > 0) {
		memmove(queue, queue + n, (size_t)queue_len * sizeof(queue[0]));
	}
	return n;
}

#else // HOST_BUILD

// ---------------------------------------------------------------------------
// the simulator: the same five keys, on the keyboard
// ---------------------------------------------------------------------------
//
// No gpio-keys node: the page binds keys to the same mask (gravity/addon.c), so the
// scheme can be tried without the player in a hand.

static uint16_t queue[GTOUCH_MAX_STATES];
static int queue_len;
static uint16_t keys;
static bool opened;

bool gtouch_open_done(void) { return opened; }

bool gtouch_open(const char *node) {
	(void)node;
	opened = true;
	printf("gravity: simulator input, keys from the page\n");
	return true;
}

void gtouch_close(void) {
	opened = false;
	queue_len = 0;
	keys = 0;
}

static void push_state(uint16_t state) {
	if (queue_len > 0 && queue[queue_len - 1] == state) {
		return;
	}
	if (queue_len == GTOUCH_MAX_STATES) {
		memmove(queue, queue + 1, (GTOUCH_MAX_STATES - 1) * sizeof(queue[0]));
		queue_len--;
	}
	queue[queue_len++] = state;
}

void gtouch_contact(int id, int glass_x, int glass_y, bool down) {
	(void)id;
	(void)glass_x;
	(void)glass_y;
	(void)down;
}

void gtouch_key(uint16_t bit, bool down) {
	uint16_t was = keys;
	if (down) {
		keys |= bit;
	} else {
		keys = (uint16_t)(keys & ~bit);
	}
	if (keys != was) {
		push_state(keys);
	}
}

void gtouch_fire(bool down) { gtouch_key(GD_KEY_FIRE, down); }

int gtouch_poll(uint16_t *states, int max) {
	if (!states || max <= 0) {
		return 0;
	}
	int n = queue_len < max ? queue_len : max;
	memcpy(states, queue, (size_t)n * sizeof(states[0]));
	queue_len -= n;
	if (queue_len > 0) {
		memmove(queue, queue + n, (size_t)queue_len * sizeof(queue[0]));
	}
	return n;
}

#endif // HOST_BUILD
