#include "listrow.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/streaming/listart.h"
#include "src/system/core/config.h"

// Rows on one page's list, each with its cover URL. The list is allocated per
// bound row and freed when the row is forgotten, so a page pays for the rows it
// actually has; nothing at all when the feature is off.
//
// How often the finished covers are collected: the same 120 ms the streaming
// pages use. Fast enough to look immediate, slow enough that a pass costs
// nothing on a list of a hundred rows.
#define LISTROW_POLL_MS 120

// How far outside the visible range a row keeps its decoded picture. Scrolling
// one screen back finds the covers already there; a list of a thousand rows
// does not hold a thousand thumbnails. Only the visible range is ever
// requested -- this is about keeping, not about downloading.
#define LISTROW_KEEP 8

// The decode size, from the row object. The fallback is for a row that has not
// been laid out yet (width and height still 0), and is about the size the
// streaming pages give their thumbnails (SC(60) on the V3's 320-pixel panel).
#define LISTROW_SIZE_DEFAULT 40
#define LISTROW_SIZE_MIN 16
#define LISTROW_SIZE_MAX 256

typedef struct listrow_entry {
	struct listrow_entry *next;
	lv_obj_t *row;		// the page's lv_image; NULL once it is being deleted
	const void *owner;	// the page, in the sense listart.h gives the word
	int index;		// the row's place in the page's list
	char url[LISTART_URL_MAX];

	// Bumped whenever the row is given another URL. The slot table carries the
	// generation the request was made with, so an answer that belongs to the
	// row's previous meaning can be recognised and thrown away instead of
	// drawn. listart has a generation of its own and already abandons a
	// superseded fetch; this one is about the row, which outlives the slot.
	uint32_t generation;

	int slot;	// LISTART slot in flight for this row, -1 when none
	bool failed;	// asked and nothing came: do not ask again before a reset
	bool has_image;
	cover_image_t image;
} listrow_entry_t;

// Which row owns each of the downloader's slots, and the generation it was
// asked for. The slot pool itself belongs to listart; this is the map from a
// slot back to the row whose picture it will be.
static struct {
	listrow_entry_t *entry;
	uint32_t generation;
} slots_used[LISTART_SLOTS];

static listrow_entry_t *entries;

// The page that currently owns the slots, and the range of its rows that is on
// screen. Only one page is on screen at a time, so there is one of each.
static const void *active_owner;
static const void *window_owner;
static int window_first = -1;
static int window_last = -1;

// Collects finished covers. Created on the first request and paused when
// nothing is in flight, so a page with no covers on its way costs no wake-ups.
static lv_timer_t *poll_timer;

// -1 until asked. The setting is read on the first call and again by every
// listrow_reset(), which is when a page opens.
static int enabled = -1;

static bool listrow_on(void) {
	if (enabled < 0) {
		enabled = config_get_bool("streaming", "list_art", false) ? 1 : 0;
	}
	return enabled == 1;
}

static listrow_entry_t *entry_for_row(lv_obj_t *row) {
	for (listrow_entry_t *e = entries; e; e = e->next) {
		if (e->row == row) {
			return e;
		}
	}
	return NULL;
}

// Call with the entry still linked: drops the picture, and only the picture.
//
// The source is cleared before the pixels are freed, and only when the row is
// still showing this very image: a page that has put its own placeholder or
// another picture there keeps it, and nothing is left pointing at freed memory.
static void entry_drop_image(listrow_entry_t *e) {
	if (!e->has_image) {
		return;
	}
	if (e->row && lv_image_get_src(e->row) == &e->image.dsc) {
		lv_image_set_src(e->row, NULL);
	}
	cover_free(&e->image);
	e->has_image = false;
}

// Gives the row's slot back to the pool. The downloader forgets anything in
// flight on it (listart_release bumps its generation), and the map entry goes
// with it, so a late answer cannot be mistaken for another row's.
static void entry_release_slot(listrow_entry_t *e) {
	int slot = e->slot;
	if (slot < 0) {
		return;
	}
	if (slot < LISTART_SLOTS && slots_used[slot].entry == e) {
		listart_release(e->owner, slot);
		slots_used[slot].entry = NULL;
		slots_used[slot].generation = 0;
	}
	e->slot = -1;
}

static bool slots_in_use(void) {
	for (int i = 0; i < LISTART_SLOTS; i++) {
		if (slots_used[i].entry) {
			return true;
		}
	}
	return false;
}

// Everything in flight belongs to a page that is no longer on screen: the pool
// has one owner (listart.h), so waiting for it is waiting for nothing. The
// pictures already decoded are left where they are -- they sit on the other
// page's widgets, which are still alive, and that page's reset or its rows'
// destruction frees them.
static void release_all_slots(void) {
	for (int i = 0; i < LISTART_SLOTS; i++) {
		listrow_entry_t *e = slots_used[i].entry;
		if (e) {
			listart_release(e->owner, i);
			e->slot = -1;
		}
		slots_used[i].entry = NULL;
		slots_used[i].generation = 0;
	}
}

static void begin_owner(const void *owner) {
	if (active_owner == owner) {
		return;
	}
	release_all_slots();
	active_owner = owner;
	window_owner = NULL;
	window_first = -1;
	window_last = -1;
}

static void entry_unlink(listrow_entry_t *e) {
	entry_release_slot(e);
	entry_drop_image(e);
	for (listrow_entry_t **pp = &entries; *pp; pp = &(*pp)->next) {
		if (*pp == e) {
			*pp = e->next;
			break;
		}
	}
	free(e);
	if (!slots_in_use() && poll_timer) {
		lv_timer_pause(poll_timer);
	}
}

static int thumb_size(lv_obj_t *row) {
	int size = (int)lv_obj_get_width(row);
	if (size <= 0) {
		size = (int)lv_obj_get_height(row);
	}
	if (size <= 0) {
		size = LISTROW_SIZE_DEFAULT;
	}
	if (size < LISTROW_SIZE_MIN) {
		size = LISTROW_SIZE_MIN;
	}
	if (size > LISTROW_SIZE_MAX) {
		size = LISTROW_SIZE_MAX;
	}
	return size;
}

static bool row_on_screen(const listrow_entry_t *e) {
	return e->owner == window_owner && e->index >= window_first && e->index <= window_last;
}

static bool row_near_screen(const listrow_entry_t *e) {
	return e->owner == window_owner && window_first >= 0 && e->index >= window_first - LISTROW_KEEP &&
		   e->index <= window_last + LISTROW_KEEP;
}

static int free_slot(void) {
	for (int i = 0; i < LISTART_SLOTS; i++) {
		if (!slots_used[i].entry) {
			return i;
		}
	}
	return -1;
}

static void poll_cb(lv_timer_t *timer);

static bool timer_start(void) {
	if (!poll_timer) {
		poll_timer = lv_timer_create(poll_cb, LISTROW_POLL_MS, NULL);
		if (!poll_timer) {
			return false;
		}
	}
	lv_timer_resume(poll_timer);
	return true;
}

static void serve_visible(void);

static void poll_cb(lv_timer_t *timer) {
	bool freed = false;

	for (int slot = 0; slot < LISTART_SLOTS; slot++) {
		listrow_entry_t *e = slots_used[slot].entry;
		if (!e) {
			continue;
		}

		cover_image_t image;
		bool finished = false;
		if (!listart_take(e->owner, slot, &image, &finished)) {
			if (finished) {
				// Nothing will arrive: a URL that does not answer, a picture
				// that will not decode, or the pool has moved to another page.
				// Remembered so the row does not keep a slot from the ones
				// that can be served; a reset allows it again.
				e->failed = true;
				entry_release_slot(e);
				freed = true;
			}
			continue;
		}

		// The answer belongs to the row only if the row still means the same
		// cover. The slot table's generation is the one the request was made
		// with; anything else was superseded while the fetch ran.
		bool wanted = e->generation == slots_used[slot].generation && e->row && e->url[0];
		entry_release_slot(e);
		freed = true;
		if (!wanted) {
			cover_free(&image);
			continue;
		}

		entry_drop_image(e);
		e->image = image;
		e->has_image = true;
		// A page that drew a themed placeholder in this image may have left a
		// recolour on it; a real cover keeps its own colours or it becomes a
		// solid square.
		lv_obj_set_style_image_recolor_opa(e->row, LV_OPA_TRANSP, 0);
		lv_image_set_src(e->row, &e->image.dsc);
	}

	if (freed) {
		// A slot that just came free goes to the next visible row in the same
		// pass: there are always more rows than slots, and the rows at the
		// bottom of the screen would otherwise wait for the next scroll.
		serve_visible();
	}
	if (!slots_in_use()) {
		lv_timer_pause(timer);
	}
}

// Requests covers for the visible rows that do not have one yet. Called when
// the range changes, when a row is bound into it, and after a slot frees up.
static void serve_visible(void) {
	if (!listrow_on() || window_first < 0) {
		return;
	}
	for (listrow_entry_t *e = entries; e; e = e->next) {
		if (!row_on_screen(e) || !e->url[0] || e->has_image || e->failed || e->slot >= 0) {
			continue;
		}
		int slot = free_slot();
		if (slot < 0) {
			return; // all busy: the rest on the next pass
		}
		if (!timer_start()) {
			return;
		}
		slots_used[slot].entry = e;
		slots_used[slot].generation = e->generation;
		e->slot = slot;
		listart_request(e->owner, slot, e->url, thumb_size(e->row));
	}
}

// Rows that scrolled out of the range let go of their slot at once, so it goes
// to a row that is on screen. Their picture stays until they are more than
// LISTROW_KEEP rows away.
static void trim(const void *owner) {
	for (listrow_entry_t *e = entries; e; e = e->next) {
		if (e->owner != owner) {
			continue;
		}
		if (!row_on_screen(e)) {
			entry_release_slot(e);
		}
		if (!row_near_screen(e)) {
			entry_drop_image(e);
		}
	}
}

static void row_deleted_cb(lv_event_t *event) {
	lv_obj_t *row = lv_event_get_target(event);
	listrow_entry_t *e = entry_for_row(row);
	if (!e) {
		return;
	}
	// The object is on its way out: never touch it again, not even to clear a
	// source. The pixels are freed here, while nothing can draw them.
	e->row = NULL;
	entry_unlink(e);
}

void listrow_bind(void *owner, lv_obj_t *row, int index, const char *url) {
	if (!row) {
		return;
	}
	if (!owner) {
		return; // listart's "no owner" is a real value there; it is not one here
	}
	if (!url || !url[0]) {
		listrow_forget(row);
		return;
	}
	if (!listrow_on()) {
		listrow_forget(row); // in case the setting was turned off since
		return;
	}
	if (!lv_obj_check_type(row, &lv_image_class)) {
		return; // documented: the row handed over is an lv_image
	}

	begin_owner(owner);

	listrow_entry_t *e = entry_for_row(row);
	if (e && e->owner == owner && strcmp(e->url, url) == 0) {
		// The same cover, maybe another place in the list: nothing to fetch.
		e->index = index;
		serve_visible();
		return;
	}
	if (e) {
		// The row now means another cover: anything in flight is not it, and
		// what it shows is not it either.
		e->generation++;
		entry_release_slot(e);
		entry_drop_image(e);
		e->failed = false;
		e->owner = owner;
		e->index = index;
		snprintf(e->url, sizeof(e->url), "%s", url);
	} else {
		e = calloc(1, sizeof(*e));
		if (!e) {
			return;
		}
		e->row = row;
		e->owner = owner;
		e->index = index;
		e->generation = 1;
		e->slot = -1;
		snprintf(e->url, sizeof(e->url), "%s", url);
		e->next = entries;
		entries = e;
		// The page's own delete handler, if it has one, stays; LVGL calls both.
		lv_obj_add_event_cb(row, row_deleted_cb, LV_EVENT_DELETE, NULL);
	}

	serve_visible();
}

void listrow_visible(void *owner, int first, int last) {
	if (!owner || !listrow_on()) {
		return;
	}
	begin_owner(owner);

	if (first < 0) {
		first = 0;
	}
	window_owner = owner;
	window_first = first;
	window_last = last < first ? -1 : last;
	if (window_last < 0) {
		window_first = -1;
	}

	// First give back what left the range, then fill what came into it: a slot
	// freed by a row going off screen is a slot for a row coming on.
	trim(owner);
	serve_visible();
}

void listrow_reset(void *owner) {
	// A page is opening: this is the one moment at which a change made in the
	// settings can take effect.
	enabled = config_get_bool("streaming", "list_art", false) ? 1 : 0;

	if (owner && active_owner == owner) {
		active_owner = NULL;
	}
	if (owner && window_owner == owner) {
		window_owner = NULL;
		window_first = -1;
		window_last = -1;
	}

	listrow_entry_t **pp = &entries;
	while (*pp) {
		listrow_entry_t *e = *pp;
		if (owner && e->owner != owner) {
			pp = &e->next;
			continue;
		}
		entry_release_slot(e);
		entry_drop_image(e);
		*pp = e->next;
		free(e);
	}

	if (!slots_in_use() && poll_timer) {
		if (!listrow_on()) {
			// Off: the timer goes with the requests, so an addon that never
			// uses the feature again holds nothing.
			lv_timer_delete(poll_timer);
			poll_timer = NULL;
		} else {
			lv_timer_pause(poll_timer);
		}
	}
}

void listrow_forget(lv_obj_t *row) {
	if (!row) {
		return;
	}
	listrow_entry_t *e = entry_for_row(row);
	if (e) {
		entry_unlink(e);
	}
}
