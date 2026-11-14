#include "touch.h"

#include <stdio.h>
#include <string.h>

#include <time.h>

// How many contacts are tracked: an array size, not a request to the driver. The
// panel reports up to five.
#define MAX_CONTACTS 5

// The zones, in the game's own coordinates (480x320, landscape).
//
// The two hands hold the player's ends: the left thumb leans, the right one works
// the throttle, and the strip between them is the game's own menu button -- the
// place no thumb rests while riding. The split is deliberately not equal: a
// thumb reaching for the throttle should not also find the brake.
//
// Lean forward is the upper left zone and lean back the lower one, which is the
// way the bike pitches under that hand: pulling the nose down is forward. Edit
// these two masks to swap them; nothing else knows the difference.
typedef struct {
	int x, y, w, h;
	uint16_t keys;
} zone_t;

static const zone_t zones[] = {
	{ 210, 0, 60, 320, GD_KEY_FIRE },	 // the centre strip: select, or the menu when held
	{ 0, 0, 210, 160, GD_KEY_RIGHT },	 // lean forward
	{ 0, 160, 210, 160, GD_KEY_LEFT },	 // lean back
	{ 270, 0, 210, 160, GD_KEY_UP },	 // gas
	{ 270, 160, 210, 160, GD_KEY_DOWN }, // brake
};

#define ZONE_COUNT ((int)(sizeof(zones) / sizeof(zones[0])))

void gtouch_zone(int index, int *x, int *y, int *w, int *h, uint16_t *keys) {
	if (index < 0 || index >= ZONE_COUNT) {
		return;
	}
	if (x) {
		*x = zones[index].x;
	}
	if (y) {
		*y = zones[index].y;
	}
	if (w) {
		*w = zones[index].w;
	}
	if (h) {
		*h = zones[index].h;
	}
	if (keys) {
		*keys = zones[index].keys;
	}
}

int gtouch_zone_count(void) { return ZONE_COUNT; }

static uint16_t keys_at(int gx, int gy) {
	for (int i = 0; i < ZONE_COUNT; i++) {
		const zone_t *z = &zones[i];
		if (gx >= z->x && gx < z->x + z->w && gy >= z->y && gy < z->y + z->h) {
			return z->keys;
		}
	}
	return 0;
}

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---------------------------------------------------------------------------
// the queue of states
// ---------------------------------------------------------------------------
//
// What the caller gets is a short history rather than the latest answer, and the
// reason is the menu: the game's menu loop repaints (and therefore polls) every
// 50 ms, a tap lasts about 30, and a mask read once per poll swallowed every
// other touch -- the Play menu answered one tap in two. A state is pushed when
// the glass changes; the caller walks them in order and turns each difference into
// key presses and releases.
//
// The queue is bounded. Overflow drops the oldest states, which is right: an
// interface that far behind only cares about where the fingers are now.

static uint16_t queue[GTOUCH_MAX_STATES];
static int queue_len;

static uint16_t last_state;

static void push_state(uint16_t state) {
	if (queue_len == GTOUCH_MAX_STATES) {
		memmove(queue, queue + 1, (GTOUCH_MAX_STATES - 1) * sizeof(queue[0]));
		queue_len--;
	}
	if (queue_len > 0 && queue[queue_len - 1] == state) {
		return; // nothing changed since the last one
	}
	queue[queue_len++] = state;
	last_state = state;
}

// A select held long enough becomes the game's menu. Evaluated here rather than
// where the state is pushed, because nothing moves while a finger rests: the
// transition has to be produced by time passing.
static int64_t fire_since;
static bool menu_sent;

static void check_hold(void) {
	if (!(last_state & GD_KEY_FIRE)) {
		fire_since = 0;
		menu_sent = false;
		return;
	}
	if (fire_since == 0) {
		fire_since = now_ms();
		return;
	}
	if (!menu_sent && now_ms() - fire_since >= GTOUCH_MENU_HOLD_MS) {
		menu_sent = true;
		push_state((uint16_t)((last_state & ~GD_KEY_FIRE) | GD_KEY_MENU));
	}
}

// Filled in by whichever half is being built: the device reads the node, the
// simulator has nothing to read.
static void device_drain(void);

int gtouch_poll(uint16_t *states, int max) {
	if (!states || max <= 0) {
		return 0;
	}
	device_drain();
	if (queue_len == 0) {
		// Nothing arrived, but a finger resting on the strip still has to turn
		// into the menu when its time comes.
		check_hold();
	}
	int n = queue_len < max ? queue_len : max;
	memcpy(states, queue, (size_t)n * sizeof(states[0]));
	queue_len -= n;
	if (queue_len > 0) {
		memmove(queue, queue + n, (size_t)queue_len * sizeof(queue[0]));
	}
	return n;
}

#ifndef HOST_BUILD

// ---------------------------------------------------------------------------
// the device: the panel is ours while the game runs
// ---------------------------------------------------------------------------
//
// Adapted from gearboy/src/system/gearboy/gbinput.c, which this copies rather
// than links (an addon is self-contained). What changed: the zones are the
// game's own and fixed, the keys are GD_KEY_* instead of a Game Boy's, the
// coordinate turn is this addon's quarter turn rather than a 180-degree flip, and
// every packet becomes a state in the queue instead of overwriting a mask.
// The parsing is that file's, and so is the reason it is careful: the panel's
// driver speaks one of two protocols and this has to survive both.

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "src/system/core/panel.h"
#include "src/system/device/power.h"

static int fd = -1;
static bool opened;
static int glass_w = 320, glass_h = 480;
static int abs_min_x, abs_max_x, abs_min_y, abs_max_y;

bool gtouch_open_done(void) { return opened; }

static int scale(int value, int lo, int hi, int size) {
	if (hi <= lo) {
		return 0;
	}
	long long v = (long long)(value - lo) * (size - 1) / (hi - lo);
	if (v < 0) {
		v = 0;
	}
	if (v > size - 1) {
		v = size - 1;
	}
	return (int)v;
}

// The glass to this addon's coordinates: the frame is the glass, its 320 rows down
// becoming 480 across (gx = glass y) and gx's 480 columns becoming 320 up
// (gy = 319 - glass x). That is the inverse of the quarter turn gravity/addon.c
// shows the frame with (lv_image_set_rotation(view, 900)), and it is what makes a
// thumb on the left of the screen a left-hand zone.
static void to_game(int raw_x, int raw_y, int *out_x, int *out_y) {
	int x = scale(raw_x, abs_min_x, abs_max_x, glass_w);
	int y = scale(raw_y, abs_min_y, abs_max_y, glass_h);

	if (display_get_rotated()) {
		x = glass_w - 1 - x;
		y = glass_h - 1 - y;
	}

	*out_x = y;
	*out_y = 319 - x;
}

bool gtouch_open(const char *node) {
	if (opened) {
		return true;
	}
	if (!node || !node[0]) {
		node = panel_touch_device();
	}
	if (!node) {
		printf("gravity: no touch node to take\n");
		return false;
	}

	fd = open(node, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		printf("gravity: %s: %s\n", node, strerror(errno));
		return false;
	}

	abs_min_x = 0;
	abs_max_x = glass_w - 1;
	abs_min_y = 0;
	abs_max_y = glass_h - 1;

	struct input_absinfo info;
	if (ioctl(fd, EVIOCGABS(ABS_X), &info) == 0 && info.maximum > info.minimum) {
		abs_min_x = info.minimum;
		abs_max_x = info.maximum;
	}
	if (ioctl(fd, EVIOCGABS(ABS_Y), &info) == 0 && info.maximum > info.minimum) {
		abs_min_y = info.minimum;
		abs_max_y = info.maximum;
	}

	// LVGL must let go: its indev is a single pointer and would eat the touches
	// the zones need to see twice over.
	panel_touch_enable(false);
	opened = true;
	printf("gravity: touch %s, glass %dx%d, raw x %d..%d, raw y %d..%d\n", node, glass_w, glass_h, abs_min_x,
		   abs_max_x, abs_min_y, abs_max_y);
	return true;
}

void gtouch_close(void) {
	if (fd >= 0) {
		close(fd);
		fd = -1;
	}
	if (opened) {
		panel_touch_enable(true);
		opened = false;
	}
}

void gtouch_contact(int id, int glass_x, int glass_y, bool down) {
	(void)id;
	(void)glass_x;
	(void)glass_y;
	(void)down;
}

static void device_drain(void) {
	static bool protocol_a, protocol_b, saw_btn_touch = true;
	static bool btn_touch_down = true;
	static int pending_x = -1, pending_y = -1, pending_id = -1;
	static int pending_count;
	static int cur_slot;
	static struct {
		int x, y;
		int id;
		bool used;
	} pending[MAX_CONTACTS], slots[MAX_CONTACTS];

	struct input_event ev[64];
	for (;;) {
		ssize_t got = read(fd, ev, sizeof(ev));
		if (got < (ssize_t)sizeof(ev[0])) {
			break;
		}
		int count = (int)(got / (ssize_t)sizeof(ev[0]));
		for (int i = 0; i < count; i++) {
			const struct input_event *e = &ev[i];

			if (e->type == EV_ABS) {
				switch (e->code) {
				case ABS_MT_SLOT:
					protocol_b = true;
					cur_slot = e->value;
					if (cur_slot < 0 || cur_slot >= MAX_CONTACTS) {
						cur_slot = 0;
					}
					break;
				case ABS_MT_TRACKING_ID:
					// Type B, and the driver here does not send ABS_MT_SLOT at
					// all: the input core drops a slot event whose value has not
					// changed, so a panel that is always slot 0 sends none, and
					// waiting for one meant reading events and decoding nothing
					// (found on the device, 2026-11-14). A tracking id with no
					// SYN_MT_REPORT in the stream is the same statement.
					if (!protocol_a) {
						protocol_b = true;
					}
					if (protocol_b) {
						slots[cur_slot].used = e->value >= 0;
					} else {
						pending_id = e->value;
					}
					break;
				case ABS_MT_POSITION_X:
					if (!protocol_a) {
						protocol_b = true;
					}
					if (protocol_b) {
						slots[cur_slot].x = e->value;
						slots[cur_slot].used = true;
					} else {
						pending_x = e->value;
					}
					break;
				case ABS_MT_POSITION_Y:
					if (!protocol_a) {
						protocol_b = true;
					}
					if (protocol_b) {
						slots[cur_slot].y = e->value;
						slots[cur_slot].used = true;
					} else {
						pending_y = e->value;
					}
					break;
				default:
					break;
				}
				continue;
			}

			if (e->type == EV_KEY && e->code == BTN_TOUCH) {
				saw_btn_touch = true;
				btn_touch_down = e->value != 0;
				continue;
			}

			if (e->type != EV_SYN) {
				continue;
			}

			if (e->code == SYN_MT_REPORT) {
				protocol_a = true;
				if (pending_x >= 0 && pending_y >= 0 && pending_count < MAX_CONTACTS) {
					pending[pending_count].x = pending_x;
					pending[pending_count].y = pending_y;
					pending[pending_count].id = pending_id;
					pending[pending_count].used = true;
					pending_count++;
				}
				pending_x = pending_y = pending_id = -1;
				continue;
			}

			if (e->code != SYN_REPORT) {
				continue;
			}

			uint16_t packet_mask = 0;
			if (protocol_b) {
				for (int s = 0; s < MAX_CONTACTS; s++) {
					if (!slots[s].used) {
						continue;
					}
					int gx, gy;
					to_game(slots[s].x, slots[s].y, &gx, &gy);
					packet_mask |= keys_at(gx, gy);
				}
			} else if (protocol_a) {
				for (int s = 0; s < pending_count; s++) {
					int gx, gy;
					to_game(pending[s].x, pending[s].y, &gx, &gy);
					packet_mask |= keys_at(gx, gy);
				}
			} else {
				continue; // protocol still unknown: the previous state stands
			}

			if (saw_btn_touch && !btn_touch_down) {
				packet_mask = 0;
			}

			push_state(packet_mask);

			pending_count = 0;
			pending_x = pending_y = pending_id = -1;
		}
	}
}

#else // HOST_BUILD

// ---------------------------------------------------------------------------
// the simulator: one pointer, fed in by the page
// ---------------------------------------------------------------------------
//
// No evdev node, and LVGL's indev is a mouse. The page hands the pointer in as a
// single contact (gravity/addon.c, the view's press events), which is enough to
// exercise the zones one at a time.

static struct {
	int x, y;
	bool down;
} contacts[MAX_CONTACTS];
static bool opened;

bool gtouch_open_done(void) { return opened; }

bool gtouch_open(const char *node) {
	(void)node;
	opened = true;
	printf("gravity: simulator input, zones from the pointer\n");
	return true;
}

void gtouch_close(void) {
	memset(contacts, 0, sizeof(contacts));
	queue_len = 0;
	last_state = 0;
	opened = false;
}

static uint16_t contacts_mask(void) {
	uint16_t mask = 0;
	for (int i = 0; i < MAX_CONTACTS; i++) {
		if (!contacts[i].down) {
			continue;
		}
		// The pointer is in the glass's coordinates; the zones are the game's.
		mask |= keys_at(contacts[i].y, 319 - contacts[i].x);
	}
	return mask;
}

void gtouch_contact(int id, int glass_x, int glass_y, bool down) {
	if (id < 0 || id >= MAX_CONTACTS) {
		return;
	}
	contacts[id].x = glass_x;
	contacts[id].y = glass_y;
	contacts[id].down = down;
	push_state(contacts_mask());
}

static void device_drain(void) {}

#endif // HOST_BUILD
