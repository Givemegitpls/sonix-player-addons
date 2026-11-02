#include "gearboypage.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/gearboy/gearboyplay.h"
#include "src/gui/gearboy/gearboyscreens.h"
#include "src/gui/gearboy/gearboysettings.h"
#include "src/gui/shell/icons_select.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/gearboy/gbdb.h"
#include "src/system/core/lang.h"

lv_obj_t *gearboypage_screen;

static lv_obj_t *list_box;
static lv_obj_t *status_label;
static lv_obj_t *btn_gb;
static lv_obj_t *btn_gbc;

// Which of the two pills is selected. ROMs live in two folders (Games/GB and
// Games/GBC) for two different machines, so they are shown as two lists rather
// than one list needing a subtitle on every entry.
static bool show_color;

// The list the rows are showing. Rows carry the index, not the pointer, so the
// array can be freed and rebuilt without a surviving row dangling.
static gbrom_t *roms;
static int rom_count;

// Bumped on every scan. The thread carries the generation it started with and
// compares it on return: if the page was closed and reopened meanwhile, its
// result is stale and gets dropped instead of overwriting the newer one.
static uint32_t scan_generation;
static bool scanning;

// The scan thread is detached -- nobody waits for it on the way out of the page
// -- so it counts itself here instead. gearboypage_stop() has to know when the
// last one has left its code behind, because the library those instructions
// live in is unmapped as soon as it returns.
static pthread_mutex_t scan_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t scan_done = PTHREAD_COND_INITIALIZER;
static int scan_live;
static bool scan_abandoned;

// How long stop() waits for a scan that is reading the card. Shorter than
// addon_card_gone()'s own wait for the GUI thread (4 s, addon.c): a scan parked
// in a card that has just been pulled must not make that wait end with the GUI
// thread still inside this library.
#define SCAN_STOP_WAIT_MS 2000

typedef struct {
	uint32_t generation;
	gbrom_t *list;
	int count;
} scan_result_t;

static void open_rom_cb(lv_event_t *e) {
	// The release of a back swipe lands on whichever row is under the finger:
	// without this, swiping back from here would launch a game.
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}

	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= rom_count || !roms) {
		return;
	}
	gearboyplay_open(roms[index].path, roms[index].name);
}

// settingsrow_action() is the canonical row of this interface: same height,
// radius, font and pressed state, and no chevron, because tapping it starts a
// game rather than opening a page.
//
// The name is not a UI label, it comes from the cartridge database. tr() passes
// it through unchanged, since a missing key returns the string it was given.
static void add_row(int index) {
	settingsrow_action(list_box, roms[index].name, open_rom_cb, (void *)(intptr_t)index);
}

static void refresh_view_buttons(void) {
	lv_obj_t *const buttons[] = {btn_gb, btn_gbc};
	const bool colour[] = {false, true};

	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		if (!buttons[i]) {
			continue;
		}
		bool on = colour[i] == show_color;
		lv_obj_set_style_bg_color(buttons[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(buttons[i], 0), on ? lv_color_white() : theme()->text_primary, 0);
	}
}

static void rebuild_rows(void) {
	lv_obj_clean(list_box);

	int shown = 0;
	for (int i = 0; i < rom_count; i++) {
		if (roms[i].color == show_color) {
			add_row(i);
			shown++;
		}
	}

	if (shown > 0) {
		lv_obj_set_hidden(status_label, true);
		return;
	}

	lv_label_set_text(status_label, show_color ? tr("gearboy_empty_note")
											   : tr("gearboy_empty_gb_note"));
	lv_obj_set_hidden(status_label, false);
}

static void pick_view_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	show_color = (bool)(uintptr_t)lv_event_get_user_data(e);
	refresh_view_buttons();
	rebuild_rows();
	lv_obj_scroll_to_y(list_box, 0, LV_ANIM_OFF);
}

static lv_obj_t *make_view_choice(lv_obj_t *parent, const char *text, bool color) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, SC(56));
	lv_obj_set_style_pad_hor(btn, SC(26), 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // Adwaita pill button
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, pick_view_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)color);

	// "GB" and "GBC" arrive as tags ("gearboy_gb"/"gearboy_gbc") and go through
	// tr(): each language maps them to the right capitalization, so the pills
	// never show up lowercase.
	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_center(label);

	return btn;
}

// ---------------------------------------------------------------------------
// the scan
// ---------------------------------------------------------------------------
//
// On its own thread because the first pass reads every ROM end to end to
// compute its CRC32: eight megabytes of cartridge takes well over a second on
// this card, and doing it on the UI thread freezes the page. Later passes are
// answered from the cache and read nothing, but they take the same path.

static void scan_done_cb(void *user) {
	scan_result_t *res = user;

	if (res->generation != scan_generation) {
		// Stale: the page was closed and reopened while the thread worked.
		free(res->list);
		free(res);
		return;
	}

	free(roms);
	roms = res->list;
	rom_count = res->count;
	scanning = false;

	rebuild_rows();
	free(res);
}

static void *scan_thread(void *arg) {
	uint32_t generation = (uint32_t)(uintptr_t)arg;

	gbrom_t *list = NULL;
	int count = gbdb_scan(&list, NULL, NULL);
	if (count < 0) {
		count = 0;
	}

	// The page may have been torn down while the card was being read. Then the
	// catalogue belongs to nobody, and the callback that would have taken it is
	// code in a library about to be unmapped: the list is freed here instead of
	// posted. Under the lock, so that stop() either sees this thread counted in
	// and waits, or has already set the flag that is read here.
	pthread_mutex_lock(&scan_lock);
	if (scan_abandoned) {
		free(list);
	} else {
		scan_result_t *res = malloc(sizeof(*res));
		if (res) {
			res->generation = generation;
			res->list = list;
			res->count = count;
			gui_post(scan_done_cb, res);
		} else {
			free(list);
		}
	}
	scan_live--;
	pthread_cond_broadcast(&scan_done);
	pthread_mutex_unlock(&scan_lock);
	return NULL;
}

static void start_scan(void) {
	if (scanning) {
		return;
	}
	scanning = true;
	scan_generation++;

	lv_label_set_text(status_label, tr("gearboy_looking_for_games"));
	lv_obj_set_hidden(status_label, false);

	pthread_mutex_lock(&scan_lock);
	scan_live++;
	pthread_mutex_unlock(&scan_lock);

	pthread_t t;
	if (pthread_create(&t, NULL, scan_thread, (void *)(uintptr_t)scan_generation) != 0) {
		pthread_mutex_lock(&scan_lock);
		scan_live--;
		pthread_cond_broadcast(&scan_done);
		pthread_mutex_unlock(&scan_lock);

		scanning = false;
		lv_label_set_text(status_label, tr("gearboy_search_failed"));
		return;
	}
	pthread_detach(t);
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	// The loader knows this one from init()'s return value; said from this side
	// too, so the addon's own list of its screens is complete and nothing it put
	// up is left out of the unload (gearboyscreens.h).
	gearboyscreens_own(gearboypage_screen);
	refresh_view_buttons();
	lv_obj_scroll_to_y(list_box, 0, LV_ANIM_OFF);
	// Rescan on every entry: the card may have been removed, reinserted or
	// filled elsewhere. With a warm cache it costs one directory read.
	start_scan();
}

// The top-right gear, like the one on the Music page: opens Gearboy settings.
static void open_settings_cb(lv_event_t *e) {
	(void)e;
	gearboysettings_open();
}

void gearboypage_init(gui_config_t *cfg) {
	// A page built after one was taken down (the library reloaded, or init()
	// called again in the same copy) may scan again: the flag that tells a
	// finishing scan nobody wants its result belongs to the teardown that set
	// it, not to the page.
	pthread_mutex_lock(&scan_lock);
	scan_abandoned = false;
	scanning = false;
	pthread_mutex_unlock(&scan_lock);

	lv_obj_add_style(gearboypage_screen, &theme_style_screen, 0);
	lv_obj_t *page_title = settingsrow_title(gearboypage_screen, cfg, "gearboy");
	settingsrow_title_corner_slots(page_title, cfg, 1);

	{
		lv_obj_t *button = lv_btn_create(gearboypage_screen);
		lv_obj_set_size(button, SC(56), SC(56));
		lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(button, 0, 0);
		lv_obj_set_style_shadow_width(button, 0, 0);
		lv_obj_set_style_pad_all(button, 0, 0);
		lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
		lv_obj_add_event_cb(button, open_settings_cb, LV_EVENT_CLICKED, NULL);

		lv_obj_t *icon = lv_image_create(button);
		lv_image_set_src(icon, &icon_music_settings);
		lv_obj_add_style(icon, &theme_style_icon, 0);
		lv_obj_center(icon);
	}

	int content_top = settingsrow_content_top(cfg);

	// The pills stay put while only the list below them scrolls. Same
	// construction as the Audiobooks page: a fixed column holding the chooser
	// row and, under it, the list.
	lv_obj_t *container = lv_obj_create(gearboypage_screen);
	lv_obj_set_size(container, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(container, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(container, 0, 0);
	lv_obj_set_style_border_width(container, 0, 0);
	lv_obj_set_style_radius(container, 0, 0);
	lv_obj_set_style_pad_hor(container, cfg->padding, 0);
	lv_obj_set_style_pad_ver(container, 0, 0);
	lv_obj_set_style_pad_gap(container, SC(12), 0);
	lv_obj_set_scrollable(container, false);
	lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	switcher_attach_back_gesture(container);

	lv_obj_t *chooser = lv_obj_create(container);
	lv_obj_set_size(chooser, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(chooser, 0, 0);
	lv_obj_set_style_border_width(chooser, 0, 0);
	lv_obj_set_style_pad_all(chooser, 0, 0);
	lv_obj_set_style_pad_gap(chooser, SC(10), 0);
	lv_obj_set_scrollable(chooser, false);
	lv_obj_set_flex_flow(chooser, LV_FLEX_FLOW_ROW);

	btn_gb = make_view_choice(chooser, "gearboy_gb", false);
	btn_gbc = make_view_choice(chooser, "gearboy_gbc", true);

	list_box = lv_obj_create(container);
	lv_obj_set_width(list_box, lv_pct(100));
	lv_obj_set_flex_grow(list_box, 1);
	lv_obj_set_style_bg_opa(list_box, 0, 0);
	lv_obj_set_style_border_width(list_box, 0, 0);
	lv_obj_set_style_radius(list_box, 0, 0);
	lv_obj_set_style_pad_all(list_box, 0, 0);
	lv_obj_set_style_pad_bottom(list_box, cfg->padding, 0);
	lv_obj_set_style_pad_gap(list_box, SC(10), 0);
	lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(list_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
	switcher_attach_back_gesture(list_box);

	status_label = lv_label_create(gearboypage_screen);
	lv_label_set_text(status_label, "");
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(status_label, lv_pct(80));
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_20, 0);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_center(status_label);

	refresh_view_buttons();
	theme_register_refresh(refresh_view_buttons);

	lv_obj_add_event_cb(gearboypage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(gearboypage_screen);
}

// ---------------------------------------------------------------------------
// unloading
// ---------------------------------------------------------------------------

// The addon's library is about to be unmapped. Two things of this page are
// still alive at that moment and would be code that no longer exists:
//
//   * the scan thread, which is detached because nothing used to wait for it.
//     It reads the card, so it is waited for with a bound -- a read parked in a
//     card that has just been pulled can outlast the player's own patience with
//     the GUI thread -- and the catalogue it returns is freed by the thread
//     itself when the flag below says nobody wants it;
//   * the theme hook, which the player calls on every theme change.
//
// The rest is memory: the catalogue and the pointers into the screen. The
// screen itself is the shell's -- init() returned it, and it deletes it after
// this returns.
//
// True when the scan thread is out. False when the wait above ran out with it
// still inside a read() on the card: the library must then not be unmapped, and
// the loader keeps it (addon.h).
bool gearboypage_stop(void) {
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_nsec += (long)SCAN_STOP_WAIT_MS * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec += deadline.tv_nsec / 1000000000L;
		deadline.tv_nsec %= 1000000000L;
	}

	pthread_mutex_lock(&scan_lock);
	scan_abandoned = true;
	scan_generation++; // in flight now is stale even if it does get posted
	while (scan_live > 0) {
		if (pthread_cond_timedwait(&scan_done, &scan_lock, &deadline) == ETIMEDOUT) {
			break;
		}
	}
	int left = scan_live;
	pthread_mutex_unlock(&scan_lock);

	if (left > 0) {
		// Nothing here can make a thread in a read() come back sooner. Said
		// plainly, and returned: the loader keeps the mapping rather than letting
		// the thread's next instruction land in nothing.
		fprintf(stderr, "gearboy: %d scan thread(s) still reading the card; not safe to unload\n", left);
	}

	theme_unregister_refresh(refresh_view_buttons);

	free(roms);
	roms = NULL;
	rom_count = 0;
	scanning = false;
	show_color = false;

	list_box = NULL;
	status_label = NULL;
	btn_gb = NULL;
	btn_gbc = NULL;
	gearboypage_screen = NULL;

	return left == 0;
}
