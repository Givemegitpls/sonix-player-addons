// The Piped pages: what to listen to, the instance it comes from, and the
// three screens it takes. See ppage.h.
//
// The shape is qobuzpage.c's, cut down to what an addon that only needs search
// and playback has to have: one worker per request, a generation counter so a
// stale answer is dropped, and every screen built once at init rather than on
// every open -- except the instance list, which depends on settings that change
// while the page is up.
//
// The instance list is the odd thing here compared to every other service: the
// user can point the addon at a different host, and nothing else in the player
// knows or cares.

#include "src/gui/ppage.h"

#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/addon/addon.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/scale.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/utils.h"
#include "src/system/gw/pgw.h"
#include "src/system/play/pplay.h"

lv_obj_t *p_screen;

static gui_config_t *page_cfg;
static const char *page_owner;

static lv_obj_t *search_screen;
static lv_obj_t *results_screen;
static lv_obj_t *settings_screen;

static lv_obj_t *search_field;
static keyboard_t *search_kb;

static lv_obj_t *results_title;
static lv_obj_t *results_list;
static lv_obj_t *results_note;

static lv_obj_t *main_status;
static lv_obj_t *instance_value;
static lv_obj_t *settings_list;

static lv_obj_t *custom_screen;
static lv_obj_t *custom_field;

static pgw_item_t *items;
static int items_count;

static void play_cb(lv_event_t *e);
static void search_loaded_cb(lv_event_t *e);
static void display_instance(char *out, size_t size);

// --- the worker ------------------------------------------------------------
//
// One thread per request, detached from the page's point of view but counted
// here: stop() has to know that nothing of this library is still executing
// before it can say the mapping may go.

static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_cond = PTHREAD_COND_INITIALIZER;
static int workers_running;
static int generation;
static bool stopping;

enum { REQ_SEARCH, REQ_TEST, REQ_PLAY };

typedef struct {
	int gen;
	int kind;
	int index;
	char query[192];
	pgw_err_t err;
	int count;
	char instance[PGW_INSTANCE_MAX];
	char note[256];
} done_t;

static void worker_finished(void) {
	pthread_mutex_lock(&worker_lock);
	workers_running--;
	pthread_cond_broadcast(&worker_cond);
	pthread_mutex_unlock(&worker_lock);
}

// The answer, on the UI thread. Everything a worker produced is in `d`, which
// this frees: the array it points into is the page's own `items`.
static void deliver_cb(void *user) {
	done_t *d = user;
	if (!d) {
		return;
	}
	bool current = d->gen == generation && !stopping;

	if (current && d->kind == REQ_SEARCH) {
		items_count = d->count;
		lv_obj_clean(results_list);
		if (d->err != PGW_OK || d->count == 0) {
			lv_label_set_text(results_note, pgw_error_text(d->err));
			lv_obj_set_hidden(results_note, false);
			lv_label_set_text(results_title, "Piped");
		} else {
			lv_obj_set_hidden(results_note, true);
			char heading[64];
			snprintf(heading, sizeof(heading), "%d result%s", d->count, d->count == 1 ? "" : "s");
			lv_label_set_text(results_title, heading);

			for (int i = 0; i < d->count; i++) {
				lv_obj_t *row = lv_obj_create(results_list);
				lv_obj_set_width(row, lv_pct(100));
				lv_obj_set_height(row, SC(92));
				lv_obj_add_style(row, &theme_style_card, 0);
				lv_obj_set_style_radius(row, SC(18), 0);
				lv_obj_set_style_pad_all(row, SC(14), 0);
				lv_obj_set_style_pad_row(row, SC(2), 0);
				lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
				lv_obj_set_scrollable(row, false);
				lv_obj_set_clickable(row, true);
				lv_obj_add_event_cb(row, play_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

				lv_obj_t *name = lv_label_create(row);
				lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
				lv_obj_set_width(name, lv_pct(100));
				// A height, or LVGL lays the text out over as many lines as
				// it likes and a long title runs out of the card.
				lv_obj_set_height(name, SC(28));
				lv_obj_set_style_text_font(name, &font_ui_20, 0);
				lv_label_set_text(name, items[i].title);

				lv_obj_t *sub = lv_label_create(row);
				lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
				lv_obj_set_width(sub, lv_pct(100));
				lv_obj_set_height(sub, SC(26));
				lv_obj_add_style(sub, &theme_style_text_dim, 0);
				lv_obj_set_style_text_font(sub, &font_ui_20, 0);
				int minutes = items[i].duration / 60;
				int seconds = items[i].duration % 60;
				char line[PGW_NAME_MAX + 32];
				if (minutes > 0 || seconds > 0) {
					snprintf(line, sizeof(line), "%s · %d:%02d", items[i].uploader, minutes, seconds);
				} else {
					snprintf(line, sizeof(line), "%s", items[i].uploader);
				}
				lv_label_set_text(sub, line);
			}
		}
	} else if (current && d->kind == REQ_TEST) {
		lv_label_set_text(main_status, d->err == PGW_OK ? d->instance : pgw_error_text(d->err));
		if (instance_value) {
			char shown[32];
			display_instance(shown, sizeof(shown));
			lv_label_set_text(instance_value, shown);
		}
	} else if (current && d->kind == REQ_PLAY) {
		// The user is on the results screen while this runs, so the answer
		// cannot be a line on the main page nobody is looking at.
		lv_label_set_text(main_status, d->err == PGW_OK ? d->note : pgw_error_text(d->err));
		if (d->err != PGW_OK) {
			toast_error(pgw_error_text(d->err));
		}
	}

	free(d);
}

static void *worker_main(void *arg) {
	done_t *d = arg;
	thread_signal_stack();
	thread_be_low_priority("piped request");

	if (d->kind == REQ_SEARCH) {
		d->err = pgw_health(8);
		if (d->err == PGW_OK) {
			pthread_mutex_lock(&worker_lock);
			int gen = generation;
			pthread_mutex_unlock(&worker_lock);
			(void)gen;
			int count = 0;
			d->err = pgw_search(d->query, "music_songs", items, PGW_ITEMS_MAX, &count);
			d->count = count;
		}
	} else if (d->kind == REQ_TEST) {
		d->err = pgw_health(8);
		snprintf(d->instance, sizeof(d->instance), "%s", pgw_instance());
	} else if (d->kind == REQ_PLAY) {
		pgw_track_t track;
		d->err = pgw_track(items[d->index].id, &track);
		if (d->err == PGW_OK) {
			snprintf(d->note, sizeof(d->note), "Playing %s", track.title);
			if (!pplay_queue(items, items_count, d->index, NULL, NULL)) {
				d->err = PGW_ERR_NET;
			}
		}
	}

	if (!gui_post(deliver_cb, d)) {
		free(d);
	}
	worker_finished();
	return NULL;
}

static void submit(int kind, int index, const char *query) {
	pthread_mutex_lock(&worker_lock);
	if (stopping) {
		pthread_mutex_unlock(&worker_lock);
		return;
	}
	done_t *d = calloc(1, sizeof(*d));
	if (!d) {
		pthread_mutex_unlock(&worker_lock);
		return;
	}
	d->gen = generation;
	d->kind = kind;
	d->index = index;
	if (query) {
		snprintf(d->query, sizeof(d->query), "%s", query);
	}
	workers_running++;
	pthread_mutex_unlock(&worker_lock);

	pthread_t thread;
	if (pthread_create(&thread, NULL, worker_main, d) != 0) {
		worker_finished();
		free(d);
		return;
	}
	pthread_detach(thread);
}

// --- screens -----------------------------------------------------------------

static lv_obj_t *own_screen(void) {
	lv_obj_t *screen = lv_obj_create(NULL);
	// The theme's screen style: without it the page keeps LVGL's white default
	// and the dim text the cards use disappears into it.
	lv_obj_add_style(screen, &theme_style_screen, 0);
	sonix_addon_own_screen(page_owner, screen);
	return screen;
}

// A container that scrolls, below the heading. settingsrow_page()'s own
// container is a fixed column and is meant for a page that fits.
static lv_obj_t *scrolling_body(lv_obj_t *screen, gui_config_t *cfg) {
	int top = settingsrow_content_top(cfg);
	lv_obj_t *container = lv_obj_create(screen);
	lv_obj_set_size(container, lv_pct(100), cfg->screen_height - top);
	lv_obj_align(container, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_bg_opa(container, 0, 0);
	lv_obj_set_style_border_width(container, 0, 0);
	lv_obj_set_style_radius(container, 0, 0);
	lv_obj_set_style_pad_hor(container, cfg->padding, 0);
	lv_obj_set_style_pad_ver(container, cfg->padding, 0);
	lv_obj_set_style_pad_gap(container, SC(8), 0);
	lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scroll_dir(container, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_ACTIVE);
	lv_obj_set_scroll_elastic(container, false);
	switcher_attach_back_gesture(container);
	return container;
}


// What fits in the right half of a row. A full URL does not: "Instance" and
// "https://api.piped.private.coffee" run into each other, which the row pays
// no attention to (settingsrow_add leaves the width to the name).
static void display_instance(char *out, size_t size) {
	const char *url = pgw_instance();
	if (!url || !url[0]) {
		snprintf(out, size, "auto");
		return;
	}
	const char *host = strstr(url, "//");
	host = host ? host + 2 : url;
	size_t len = strlen(host);
	if (len <= 18) {
		snprintf(out, size, "%s", host);
	} else {
		snprintf(out, size, "%.15s...", host);
	}
}

static void open_search_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(search_field, "");
	switch_screen(search_screen);
}

static void open_custom_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(custom_field, pgw_instance());
	switch_screen(custom_screen);
}

static void custom_accept_cb(lv_event_t *e) {
	(void)e;
	const char *url = lv_textarea_get_text(custom_field);
	if (!pgw_set_instance(url ? url : "")) {
		toast_error("That is not a URL this addon can use");
		return;
	}
	if (instance_value) {
		char shown[32];
		display_instance(shown, sizeof(shown));
		lv_label_set_text(instance_value, shown);
	}
	switch_screen(settings_screen);
}

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(settings_screen);
}

static void test_cb(lv_event_t *e) {
	(void)e;
	if (main_status) {
		lv_label_set_text(main_status, "Asking the instance...");
	}
	submit(REQ_TEST, 0, NULL);
}

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	const char *query = lv_textarea_get_text(search_field);
	if (!query || !query[0]) {
		return;
	}
	pthread_mutex_lock(&worker_lock);
	generation++;
	pthread_mutex_unlock(&worker_lock);

	lv_obj_clean(results_list);
	lv_label_set_text(results_note, "Searching...");
	lv_obj_set_hidden(results_note, false);
	lv_label_set_text(results_title, "Piped");
	switch_screen(results_screen);
	submit(REQ_SEARCH, 0, query);
}

static void play_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= items_count) {
		return;
	}
	lv_label_set_text(main_status, "Starting...");
	submit(REQ_PLAY, index, NULL);
}

static void instance_row_cb(lv_event_t *e) {
	const char *url = lv_event_get_user_data(e);
	pgw_set_instance(url ? url : "");
	if (instance_value) {
		char shown[32];
		display_instance(shown, sizeof(shown));
		lv_label_set_text(instance_value, shown);
	}
	switch_screen(p_screen);
}

static void settings_loaded_cb(lv_event_t *e) {
	(void)e;
	if (!settings_list) {
		return;
	}
	lv_obj_clean(settings_list);
	lv_obj_t *value = NULL;
	settingsrow_action(settings_list, "Automatic (first that answers)", instance_row_cb, NULL);
	settingsrow_action(settings_list, "Add your own...", open_custom_cb, NULL);

	const char *active = pgw_instance();
	for (int i = 0; i < pgw_instance_count(); i++) {
		const char *url = pgw_instance_at(i);
		const char *host = strstr(url, "//");
		host = host ? host + 2 : url;
		settingsrow_add(settings_list, host, &value, instance_row_cb, (void *)url);
		lv_label_set_text(value, active[0] && strcmp(active, url) == 0 ? "in use" : "");
	}
}

static void main_loaded_cb(lv_event_t *e) {
	(void)e;
	if (instance_value) {
		char shown[32];
		display_instance(shown, sizeof(shown));
		lv_label_set_text(instance_value, shown);
	}
}

void ppage_init(gui_config_t *cfg, const char *addon_id) {
	page_cfg = cfg;
	page_owner = addon_id;
	if (!items) {
		items = calloc(PGW_ITEMS_MAX, sizeof(*items));
	}

	// --- main
	p_screen = own_screen();
	lv_obj_t *main_page = settingsrow_page(p_screen, cfg, "Piped");
	settingsrow_add(main_page, "Search", NULL, open_search_cb, NULL);
	instance_value = NULL;
	settingsrow_add(main_page, "Instance", &instance_value, open_settings_cb, NULL);
	{
		char shown[32];
		display_instance(shown, sizeof(shown));
		lv_label_set_text(instance_value, shown);
	}
	settingsrow_action(main_page, "Test connection", test_cb, NULL);

	main_status = lv_label_create(main_page);
	lv_label_set_long_mode(main_status, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(main_status, lv_pct(100));
	lv_obj_add_style(main_status, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(main_status, &font_ui_20, 0);
	lv_label_set_text(main_status, "");
	lv_obj_add_event_cb(p_screen, main_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	// --- search
	search_screen = own_screen();
	settingsrow_title(search_screen, cfg, "Search");
	int top = settingsrow_content_top(cfg);
	search_field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, "Track or artist");
	lv_obj_set_size(search_field, cfg->screen_width - 2 * cfg->padding, SC(64));
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_set_scrollbar_mode(search_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_style_radius(search_field, SC(12), 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_shadow_width(search_field, 0, 0);
	lv_obj_set_style_pad_all(search_field, SC(14), 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	keyboard_style_caret(search_field);
	search_kb = keyboard_create(search_screen, cfg->screen_width, SC(316), search_field, NULL, "ok", search_accept_cb, NULL);
	lv_obj_add_event_cb(search_screen, search_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(search_screen);

	// --- results
	results_screen = own_screen();
	results_title = settingsrow_title(results_screen, cfg, "Piped");
	results_list = scrolling_body(results_screen, cfg);
	results_note = lv_label_create(results_screen);
	lv_label_set_long_mode(results_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(results_note, lv_pct(100));
	lv_obj_add_style(results_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(results_note, &font_ui_24, 0);
	lv_obj_set_style_text_align(results_note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(results_note, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + SC(80));
	lv_obj_set_hidden(results_note, true);
	switcher_attach_back_gesture(results_screen);

	// --- a custom instance
	custom_screen = own_screen();
	settingsrow_title(custom_screen, cfg, "Instance URL");
	custom_field = lv_textarea_create(custom_screen);
	lv_textarea_set_one_line(custom_field, true);
	lv_textarea_set_placeholder_text(custom_field, "https://piped.example");
	lv_obj_set_size(custom_field, cfg->screen_width - 2 * cfg->padding, SC(64));
	lv_obj_align(custom_field, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_set_scrollbar_mode(custom_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_style(custom_field, &theme_style_card, 0);
	lv_obj_set_style_radius(custom_field, SC(12), 0);
	lv_obj_set_style_border_width(custom_field, 0, 0);
	lv_obj_set_style_shadow_width(custom_field, 0, 0);
	lv_obj_set_style_pad_all(custom_field, SC(14), 0);
	lv_obj_set_style_text_font(custom_field, &font_ui_20, 0);
	keyboard_style_caret(custom_field);
	keyboard_create(custom_screen, cfg->screen_width, SC(316), custom_field, NULL, "ok", custom_accept_cb, NULL);
	switcher_attach_back_gesture(custom_screen);

	// --- settings
	settings_screen = own_screen();
	settingsrow_title(settings_screen, cfg, "Instance");
	settings_list = scrolling_body(settings_screen, cfg);
	lv_obj_add_event_cb(settings_screen, settings_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(settings_screen);
}

bool ppage_stop(void) {
	pthread_mutex_lock(&worker_lock);
	stopping = true;
	generation++;
	pthread_mutex_unlock(&worker_lock);

	// A worker parked in an HTTP request cannot be interrupted (docs/ADDONS.md,
	// "Unloading"); the answer is a bounded wait and a "no" that keeps the
	// mapping instead of unmapping code a thread is still in.
	bool quiet = false;
	pthread_mutex_lock(&worker_lock);
	for (int waited = 0; waited < 20 && !quiet; waited++) {
		if (workers_running == 0) {
			quiet = true;
			break;
		}
		struct timespec until;
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_nsec += 100 * 1000 * 1000;
		if (until.tv_nsec >= 1000000000L) {
			until.tv_nsec -= 1000000000L;
			until.tv_sec += 1;
		}
		pthread_cond_timedwait(&worker_cond, &worker_lock, &until);
	}
	if (workers_running == 0) {
		quiet = true;
	}
	pthread_mutex_unlock(&worker_lock);

	page_cfg = NULL;
	page_owner = NULL;
	search_screen = NULL;
	results_screen = NULL;
	settings_screen = NULL;
	search_field = NULL;
	search_kb = NULL;
	results_title = NULL;
	results_list = NULL;
	results_note = NULL;
	main_status = NULL;
	instance_value = NULL;
	settings_list = NULL;
	custom_screen = NULL;
	custom_field = NULL;
	items_count = 0;
	p_screen = NULL;

	stopping = false;
	return quiet;
}

// The search page is reused for every query, and LVGL keeps the caret blinking
// from the moment a textarea exists: bringing it to the front again has to
// re-open the keyboard, which the user closed with "ok".
static void search_loaded_cb(lv_event_t *e) {
	(void)e;
	keyboard_reset(search_kb);
	keyboard_set_visible(search_kb, true);
}
